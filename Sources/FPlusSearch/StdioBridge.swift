import Darwin
import Dispatch
import Foundation

private final class CancellationFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false

    var isCancelled: Bool { lock.withLock { value } }
    func cancel() { lock.withLock { value = true } }
}

final class StdioBridge: Sendable {
    private let state: BridgeState

    init(executable: URL, root: URL) {
        state = BridgeState(executable: executable, root: root)
    }

    deinit {
        let state = state
        state.queue.async { state.stop(error: .closed) }
    }

    func request(_ data: Data, id: String, timeout: TimeInterval) async throws -> Data {
        let flag = CancellationFlag()
        let deadline = DispatchTime.now() + timeout
        let state = state
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                state.queue.async {
                    state.enqueue(data, id: id, deadline: deadline, flag: flag, continuation: continuation)
                }
            }
        } onCancel: {
            flag.cancel()
            state.queue.async { state.cancel(id: id) }
        }
    }

    func close() async {
        let state = state
        await withCheckedContinuation { continuation in
            state.queue.async {
                state.stop(error: .closed)
                continuation.resume()
            }
        }
    }
}

// Mutable transport state is confined to queue; cancellation flags use their own lock.
private final class BridgeState: @unchecked Sendable {
    let queue = DispatchQueue(label: "FPlusSearch.stdio")
    private let executable: URL
    private let root: URL
    private var process: Process?
    private var input: FileHandle?
    private var readers: [DispatchSourceRead] = []
    private var writer: DispatchSourceWrite?
    private var writerActive = false
    private var pending: [Pending] = []
    private var active: Pending?
    private var output = Data()
    private var diagnostics = Data()
    private var writeOffset = 0
    private var closed = false
    private var receivedResponse = false

    private struct Pending {
        let id: String
        let data: Data
        let flag: CancellationFlag
        let continuation: CheckedContinuation<Data, Error>
        let timer: DispatchSourceTimer
    }

    init(executable: URL, root: URL) {
        self.executable = executable
        self.root = root
    }

    func enqueue(
        _ data: Data, id: String, deadline: DispatchTime, flag: CancellationFlag,
        continuation: CheckedContinuation<Data, Error>
    ) {
        guard !flag.isCancelled else { continuation.resume(throwing: CancellationError()); return }
        guard !closed else { continuation.resume(throwing: FPlusSearchError.closed); return }
        guard deadline > .now() else { continuation.resume(throwing: FPlusSearchError.timedOut); return }
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: deadline)
        timer.setEventHandler { [weak self] in self?.expire(id: id) }
        var line = data
        line.append(10)
        pending.append(Pending(id: id, data: line, flag: flag, continuation: continuation, timer: timer))
        timer.resume()
        startNext()
    }

    private func launch() throws {
        let process = Process()
        let stdin = Pipe()
        let stdout = Pipe()
        let stderr = Pipe()
        process.executableURL = executable
        process.arguments = ["stdio", "--root", root.path]
        process.standardInput = stdin
        process.standardOutput = stdout
        process.standardError = stderr
        let handles = [stdin.fileHandleForWriting, stdout.fileHandleForReading, stderr.fileHandleForReading]
        for handle in handles {
            let fd = handle.fileDescriptor
            guard fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != -1 else {
                throw FPlusSearchError.startup("cannot configure bridge pipes: \(String(cString: strerror(errno)))")
            }
        }
        guard fcntl(stdin.fileHandleForWriting.fileDescriptor, F_SETNOSIGPIPE, 1) != -1 else {
            throw FPlusSearchError.startup("cannot suppress SIGPIPE for bridge input")
        }
        process.terminationHandler = { [weak self] child in
            guard let self else { return }
            self.queue.async { [self] in
                guard !self.closed else { return }
                // Let pipe events consume the child's final response and diagnostics first.
                self.queue.asyncAfter(deadline: .now() + 0.05) { [weak self] in
                    guard let self, !self.closed else { return }
                    self.stop(error: self.exitError("bridge exited with status \(child.terminationStatus)"))
                }
            }
        }
        do {
            try process.run()
        } catch {
            throw FPlusSearchError.startup("cannot launch \(executable.path): \(error)")
        }
        self.process = process
        input = stdin.fileHandleForWriting
        let writer = DispatchSource.makeWriteSource(fileDescriptor: stdin.fileHandleForWriting.fileDescriptor, queue: queue)
        writer.setEventHandler { [weak self] in self?.flushInput() }
        self.writer = writer
        try? stdin.fileHandleForReading.close()
        try? stdout.fileHandleForWriting.close()
        try? stderr.fileHandleForWriting.close()
        addReader(stdout.fileHandleForReading, isError: false)
        addReader(stderr.fileHandleForReading, isError: true)
    }

    private func addReader(_ handle: FileHandle, isError: Bool) {
        let source = DispatchSource.makeReadSource(fileDescriptor: handle.fileDescriptor, queue: queue)
        source.setEventHandler { [weak self] in self?.read(handle, isError: isError) }
        source.setCancelHandler { try? handle.close() }
        readers.append(source)
        source.resume()
    }

    private func startNext() {
        guard !closed, active == nil, !pending.isEmpty else { return }
        active = pending.removeFirst()
        if active?.flag.isCancelled == true {
            finishActive(.failure(CancellationError()))
            startNext()
            return
        }
        do {
            if process == nil { try launch() }
            writeOffset = 0
            flushInput()
        } catch let error as FPlusSearchError {
            stop(error: error)
        } catch {
            stop(error: .startup(String(describing: error)))
        }
    }

    private func flushInput() {
        guard !closed, let active, let input else { return }
        let fd = input.fileDescriptor
        while writeOffset < active.data.count {
            let count = active.data.withUnsafeBytes { bytes in
                Darwin.write(fd, bytes.baseAddress!.advanced(by: writeOffset), bytes.count - writeOffset)
            }
            if count > 0 { writeOffset += count; continue }
            if count < 0 && errno == EINTR { continue }
            if count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) {
                if !writerActive {
                    writerActive = true
                    writer?.resume()
                }
                return
            }
            stop(error: exitError("bridge input failed: \(String(cString: strerror(errno)))"))
            return
        }
        if writerActive {
            writer?.suspend()
            writerActive = false
        }
    }

    private func read(_ handle: FileHandle, isError: Bool) {
        guard !closed else { return }
        var bytes = [UInt8](repeating: 0, count: 65_536)
        let count = Darwin.read(handle.fileDescriptor, &bytes, bytes.count)
        if count < 0 {
            if errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK { return }
            stop(error: exitError("bridge output failed: \(String(cString: strerror(errno)))"))
            return
        }
        if count == 0 {
            readers[isError ? 1 : 0].cancel()
            if !isError {
                queue.asyncAfter(deadline: .now() + 0.05) { [weak self] in
                    guard let self, !self.closed else { return }
                    self.stop(error: self.exitError("bridge closed its output"))
                }
            }
            return
        }
        if isError {
            diagnostics.append(contentsOf: bytes.prefix(count))
            if diagnostics.count > 8192 { diagnostics.removeFirst(diagnostics.count - 8192) }
            return
        }
        output.append(contentsOf: bytes.prefix(count))
        guard output.count <= 16 * 1_048_576 else {
            stop(error: .invalidResponse("response exceeds 16 MiB"))
            return
        }
        while let newline = output.firstIndex(of: 10) {
            let line = Data(output[..<newline])
            output.removeSubrange(...newline)
            receive(line)
            if closed { return }
        }
    }

    private struct Envelope: Decodable {
        let ok: Bool
        let id: String
        let error: String?
    }

    private func receive(_ line: Data) {
        guard let active else { stop(error: .invalidResponse("unsolicited response")); return }
        do {
            let envelope = try JSONDecoder().decode(Envelope.self, from: line)
            guard envelope.id == active.id else {
                stop(error: .invalidResponse("response ID does not match the active request"))
                return
            }
            receivedResponse = true
            if envelope.ok {
                finishActive(.success(line))
            } else {
                guard let message = envelope.error else {
                    stop(error: .invalidResponse("server error response has no error message"))
                    return
                }
                finishActive(.failure(FPlusSearchError.server(id: envelope.id, message: message)))
            }
            startNext()
        } catch {
            stop(error: .invalidResponse(String(describing: error)))
        }
    }

    private func finishActive(_ result: Result<Data, Error>) {
        guard let request = active else { return }
        active = nil
        request.timer.cancel()
        request.continuation.resume(with: result)
    }

    func cancel(id: String) {
        if active?.id == id {
            finishActive(.failure(CancellationError()))
            stop(error: .transport("bridge closed after an active request was cancelled"))
        } else if let index = pending.firstIndex(where: { $0.id == id }) {
            let request = pending.remove(at: index)
            request.timer.cancel()
            request.continuation.resume(throwing: CancellationError())
        }
    }

    private func expire(id: String) {
        if active?.id == id {
            finishActive(.failure(FPlusSearchError.timedOut))
            stop(error: .transport("bridge closed after an active request timed out"))
        } else if let index = pending.firstIndex(where: { $0.id == id }) {
            let request = pending.remove(at: index)
            request.timer.cancel()
            request.continuation.resume(throwing: FPlusSearchError.timedOut)
        }
    }

    private func exitError(_ message: String) -> FPlusSearchError {
        let stderr = String(decoding: diagnostics, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        let detail = stderr.isEmpty ? message : "\(message): \(stderr)"
        return receivedResponse ? .transport(detail) : .startup(detail)
    }

    func stop(error: FPlusSearchError) {
        guard !closed else { return }
        closed = true
        finishActive(.failure(error))
        for request in pending {
            request.timer.cancel()
            request.continuation.resume(throwing: error)
        }
        pending.removeAll()
        for reader in readers { reader.cancel() }
        readers.removeAll()
        if let input {
            if let writer {
                writer.setCancelHandler { try? input.close() }
                writer.cancel()
                if !writerActive { writer.resume() }
            } else {
                try? input.close()
            }
        }
        writer = nil
        input = nil
        output.removeAll()
        if let process, process.isRunning {
            process.terminate()
            queue.asyncAfter(deadline: .now() + 0.2) {
                if process.isRunning { Darwin.kill(process.processIdentifier, SIGKILL) }
            }
        }
        process = nil
    }
}
