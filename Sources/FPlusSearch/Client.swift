import Foundation

public final class FPlusSearchClient: Sendable {
    public let executableURL: URL
    public let root: URL
    public let requestTimeout: TimeInterval
    private let bridge: StdioBridge

    public init(executableURL: URL? = nil, root: URL, requestTimeout: TimeInterval = 15) throws {
        try validateTimeout(requestTimeout)
        guard root.isFileURL, root.path.hasPrefix("/"), !root.path.contains("\0") else {
            throw FPlusSearchError.invalidConfiguration("root must be an absolute file URL")
        }
        let executable = try executableURL ?? Self.discoverExecutable()
        guard executable.isFileURL, executable.path.hasPrefix("/"),
              !executable.path.contains("\0"),
              FileManager.default.isExecutableFile(atPath: executable.path) else {
            throw FPlusSearchError.invalidConfiguration("executableURL must point to an executable file")
        }
        self.executableURL = executable
        self.root = root
        self.requestTimeout = requestTimeout
        bridge = StdioBridge(executable: executable, root: root)
    }

    public static func discoverExecutable() throws -> URL {
        let paths = [
            "/opt/homebrew/bin/fplussearch",
            FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".local/bin/fplussearch").path,
            "/usr/local/bin/fplussearch"
        ]
        guard let path = paths.first(where: { FileManager.default.isExecutableFile(atPath: $0) }) else {
            throw FPlusSearchError.executableNotFound(searchedPaths: paths)
        }
        return URL(fileURLWithPath: path)
    }

    public func status(timeout: TimeInterval? = nil) async throws -> StatusResponse {
        try await request(Request(op: "status"), timeout: timeout)
    }

    public func search(_ query: String, limit: Int = 50, timeout: TimeInterval? = nil) async throws -> SearchResponse {
        guard limit >= 0 else { throw FPlusSearchError.invalidConfiguration("limit must be nonnegative") }
        return try await request(Request(q: query, limit: limit), timeout: timeout)
    }

    public func grep(
        _ pattern: String,
        query: String = "",
        mode: GrepMode = .literal,
        limit: Int = 50,
        perFile: Int = 5,
        budgetMilliseconds: Int = 250,
        timeout: TimeInterval? = nil
    ) async throws -> GrepResponse {
        guard !pattern.isEmpty, limit >= 0, perFile > 0,
              budgetMilliseconds > 0, budgetMilliseconds <= Int(Int32.max) else {
            throw FPlusSearchError.invalidConfiguration("grep requires a pattern, nonnegative limit, and positive perFile and budget")
        }
        return try await request(
            Request(op: "grep", q: query, limit: limit, pattern: pattern, mode: mode,
                    perFile: perFile, budgetMilliseconds: budgetMilliseconds),
            timeout: timeout
        )
    }

    public func awaitReady(
        timeout: TimeInterval = 60,
        requireContent: Bool = false,
        pollInterval: TimeInterval = 0.2
    ) async throws -> StatusResponse {
        try validateTimeout(timeout)
        try validateTimeout(pollInterval)
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(timeout))
        while true {
            try Task.checkCancellation()
            let remaining = clock.now.duration(to: deadline).seconds
            guard remaining > 0 else { throw FPlusSearchError.timedOut }
            let result = try await status(timeout: min(requestTimeout, remaining))
            if result.ready && (!requireContent || !result.contentPending) { return result }
            let sleep = min(pollInterval, clock.now.duration(to: deadline).seconds)
            guard sleep > 0 else { throw FPlusSearchError.timedOut }
            try await Task.sleep(for: .seconds(sleep))
        }
    }

    public func close() async {
        await bridge.close()
    }

    private func request<Response: Decodable & Sendable>(
        _ request: Request, timeout: TimeInterval?
    ) async throws -> Response {
        try Task.checkCancellation()
        let duration = timeout ?? requestTimeout
        try validateTimeout(duration)
        let data = try JSONEncoder().encode(request)
        guard data.count <= 1_048_576 else {
            throw FPlusSearchError.invalidConfiguration("request exceeds 1 MiB")
        }
        let response = try await bridge.request(data, id: request.id, timeout: duration)
        try Task.checkCancellation()
        do {
            return try JSONDecoder().decode(Response.self, from: response)
        } catch {
            throw FPlusSearchError.invalidResponse(String(describing: error))
        }
    }
}

private func validateTimeout(_ timeout: TimeInterval) throws {
    guard timeout.isFinite, timeout > 0, timeout <= 86_400 else {
        throw FPlusSearchError.invalidConfiguration("timeouts must be greater than zero and at most 86400 seconds")
    }
}

private extension Duration {
    var seconds: Double {
        Double(components.seconds) + Double(components.attoseconds) / 1e18
    }
}

private struct Request: Encodable, Sendable {
    var id = UUID().uuidString
    var op: String?
    var q: String?
    var limit: Int?
    var pattern: String?
    var mode: GrepMode?
    var perFile: Int?
    var budgetMilliseconds: Int?

    enum CodingKeys: String, CodingKey {
        case id, op, q, limit, pattern, mode
        case perFile = "per_file"
        case budgetMilliseconds = "budget_ms"
    }
}
