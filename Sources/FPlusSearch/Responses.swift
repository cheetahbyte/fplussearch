import Foundation

public enum FPlusSearchError: Error, Sendable, Equatable {
    case executableNotFound(searchedPaths: [String])
    case invalidConfiguration(String)
    case startup(String)
    case server(id: String, message: String)
    case transport(String)
    case invalidResponse(String)
    case timedOut
    case closed
}

extension FPlusSearchError: LocalizedError {
    public var errorDescription: String? {
        switch self {
        case .executableNotFound(let paths): "fplussearch not found: \(paths.joined(separator: ", "))"
        case .invalidConfiguration(let message), .startup(let message), .transport(let message),
             .invalidResponse(let message): message
        case .server(_, let message): message
        case .timedOut: "fplussearch request timed out"
        case .closed: "fplussearch client is closed"
        }
    }
}

public struct StatusResponse: Decodable, Sendable {
    public let ok: Bool
    public let id: String
    public let ready: Bool
    public let entries: UInt64
    public let directories: UInt64
    public let overlay: UInt64
    public let removed: UInt64
    public let symbols: UInt64
    public let eventID: UInt64
    public let owner: Bool
    public let busy: Bool
    public let fullDiskAccess: Bool
    public let contentDocuments: UInt64
    public let contentBytes: UInt64
    public let contentPending: Bool

    enum CodingKeys: String, CodingKey {
        case ok, id, ready, entries, overlay, removed, symbols, owner, busy
        case directories = "dirs"
        case eventID = "event_id"
        case fullDiskAccess = "full_disk_access"
        case contentDocuments = "content_docs"
        case contentBytes = "content_bytes"
        case contentPending = "content_pending"
    }
}

public struct FileHit: Decodable, Sendable {
    public let path: String
    public let isDirectory: Bool
    public let size: UInt64
    public let modificationTime: Int64
    public let score: Int

    enum CodingKeys: String, CodingKey {
        case path, size, score
        case isDirectory = "dir"
        case modificationTime = "mtime"
    }
}

public struct SymbolHit: Decodable, Sendable {
    public let path: String
    public let line: UInt64
    public let symbol: String
    public let kind: String
}

public enum SearchHit: Decodable, Sendable {
    case file(FileHit)
    case symbol(SymbolHit)

    private enum CodingKeys: String, CodingKey { case symbol }

    public init(from decoder: Decoder) throws {
        let fields = try decoder.container(keyedBy: CodingKeys.self)
        if fields.contains(.symbol) {
            self = .symbol(try SymbolHit(from: decoder))
        } else {
            self = .file(try FileHit(from: decoder))
        }
    }
}

public struct SearchResponse: Decodable, Sendable {
    public let ok: Bool
    public let id: String
    public let total: UInt64
    public let milliseconds: Double
    public let warning: String?
    public let hits: [SearchHit]

    enum CodingKeys: String, CodingKey {
        case ok, id, total, warning, hits
        case milliseconds = "ms"
    }
}

public struct GrepLine: Decodable, Sendable {
    public let line: UInt64
    public let text: String
}

public struct GrepFile: Decodable, Sendable {
    public let path: String
    public let lines: [GrepLine]
}

public struct GrepResponse: Decodable, Sendable {
    public let ok: Bool
    public let id: String
    public let milliseconds: Double
    public let candidates: UInt64
    public let read: UInt64
    public let complete: Bool
    public let files: [GrepFile]

    enum CodingKeys: String, CodingKey {
        case ok, id, candidates, read, complete, files
        case milliseconds = "ms"
    }
}

public enum GrepMode: String, Encodable, Sendable {
    case literal
    case regex
}
