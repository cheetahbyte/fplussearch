# Use fplussearch from Swift

`FPlusSearch` is a dependency-free Swift Package Manager library for macOS 13 or later and Swift 6.
It connects to the existing daemon through a persistent `fplussearch stdio --root PATH` subprocess.
The package doesn't build or bundle the C++ executable.

## Add the package

Install the executable with Homebrew, or build it and run `build/fplussearch install`.
See the [installation instructions](../README.md) for details.

Add the repository in Xcode's package dependencies, or add these entries to your `Package.swift`:

```swift
// In dependencies:
.package(url: "https://github.com/cheetahbyte/fplussearch.git", branch: "main")

// In your target's dependencies:
.product(name: "FPlusSearch", package: "fplussearch")
```

There are no release tags yet. Use `main` until a versioned release is available.
For local development, use `.package(path: "/absolute/path/to/fplussearch")` instead.

## Create a client

Pass an explicit executable URL when you know its location.
The root URL is required: choose the directory you intend to index.
Using `/` opts into whole-disk indexing.

```swift
import Foundation
import FPlusSearch

let client = try FPlusSearchClient(
    executableURL: URL(fileURLWithPath: "/opt/homebrew/bin/fplussearch"),
    root: FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Projects"),
    requestTimeout: 15
)
```

If you omit `executableURL`, discovery checks these locations in order:

1. `/opt/homebrew/bin/fplussearch`
2. `~/.local/bin/fplussearch`
3. `/usr/local/bin/fplussearch`

`FPlusSearchClient.discoverExecutable()` also exposes this lookup.
Discovery doesn't search `PATH`. An executable stored elsewhere requires an explicit URL.
Arguments and JSON are passed directly, without shell interpolation.
Initialization validates configuration; the first request launches the bridge and can start the daemon.

## Check readiness and search

Run this code inside an asynchronous function or task:

```swift
let status = try await client.status()
print(status.ready, status.entries, status.contentPending)

let ready = try await client.awaitReady(timeout: 60)
print(ready.symbols)

let files = try await client.search("report ext:pdf", limit: 20)
for hit in files.hits {
    if case .file(let file) = hit {
        print(file.path, file.isDirectory, file.size, file.modificationTime, file.score)
    }
}

let symbols = try await client.search("sym:parse ext:swift", limit: 20)
for hit in symbols.hits {
    if case .symbol(let symbol) = hit {
        print(symbol.path, symbol.line, symbol.symbol, symbol.kind)
    }
}

_ = try await client.awaitReady(timeout: 120, requireContent: true)
let contents = try await client.grep(
    "TODO",
    query: "ext:swift",
    mode: .literal,
    limit: 20,
    perFile: 5,
    budgetMilliseconds: 250
)
for file in contents.files {
    for line in file.lines {
        print(file.path, line.line, line.text)
    }
}
print(contents.complete)

await client.close()
```

Use `do`/`catch` and call `await client.close()` on the failure path, too.
Dropping the final client reference also schedules bridge cleanup.
`close()` is idempotent. It closes pipes and requests bridge termination without waiting indefinitely.
A bridge that ignores termination is killed after a short grace period.
The shared daemon keeps running after `close()`, cancellation, timeout, or client destruction.
Closing the client doesn't stop indexing or delete its cache.

`search` accepts the [daemon query syntax](../README.md#query-syntax).
Use `sym:` to request symbols; ordinary queries return files and directories.
Use `grep`, not `search`, for content queries.
`grep` accepts `.literal` or `.regex`; `query` supplies file filters and scope, such as `ext:swift` or `in:~/Projects`.
Limits are passed directly to the JSON protocol. Unlike the CLI's `-n 0`, zero isn't an unlimited-results shortcut.

## Response fields

All response models conform to `Decodable` and `Sendable`.
Successful responses expose `ok` and their correlated string `id`.

- `StatusResponse`: `ready`, `entries`, `directories`, `overlay`, `removed`, `symbols`, `eventID`, `owner`,
  `busy`, `fullDiskAccess`, `contentDocuments`, `contentBytes`, and `contentPending`.
- `SearchResponse`: `total`, `milliseconds`, optional `warning`, and `hits`.
  Each `SearchHit` is `.file(FileHit)` or `.symbol(SymbolHit)`.
- `FileHit`: `path`, `isDirectory`, `size` in bytes, `modificationTime` in Unix seconds, and `score`.
- `SymbolHit`: `path`, `line`, `symbol`, and the daemon's `kind` string.
  Symbol locations are heuristic; `kind` can be `"?"` when the definition can't be located.
- `GrepResponse`: `milliseconds`, `candidates`, `read`, `complete`, and `files`.
  Each `GrepFile` has `path` and `lines`; each `GrepLine` has `line` and `text`.

A grep response with `complete == false` is partial, not a transport failure.
The daemon's `budgetMilliseconds` bounds content work; it isn't the client's request timeout.

## First-index and content readiness

`status()` works while the initial index is being built.
Search and grep can throw `.server(id:message:)` with the daemon's indexing error until `ready` becomes true.
`awaitReady` polls status with an overall deadline; it doesn't retry startup or transport failures.

File readiness and content readiness are separate.
`requireContent: true` waits for `ready == true` and `contentPending == false`.
This doesn't prove that content search is enabled or that any documents were indexed.
A disabled content index produces the server error `content search is off`.
An empty symbol result can mean that no definitions matched or no symbol index is available.

## Timeouts and cancellation

Timeouts are seconds. Every timeout must be finite, greater than zero, and at most 86,400 seconds.
The default request timeout is 15 seconds; individual requests accept a `timeout` override.
It includes time queued behind other requests, bridge startup, writing, and waiting for a response.
`awaitReady` defaults to a 60-second overall timeout and a 0.2-second polling interval.

You can share one client between tasks. Requests are serialized and responses are checked against unique IDs.
Transport I/O uses a private dispatch queue and nonblocking pipes, not the main actor.

Cancelling a queued request throws `CancellationError` without closing the bridge.
Cancelling an active request closes the client to avoid accepting a stale or partial response.
The cancelled task gets `CancellationError`; other pending tasks get a transport error.
An active request timeout similarly closes the client and throws `.timedOut` for that request.
A queued timeout removes only that request.
Cancellation during the readiness polling sleep leaves the bridge open.
Create a new client after a terminal transport failure, active cancellation, active timeout, or explicit close.

Requests are limited to 1 MiB and responses to 16 MiB.
Larger payloads fail rather than growing buffers without a bound.
Use smaller result limits when necessary.

## Handle errors

`FPlusSearchError` separates these failures:

- `.executableNotFound(searchedPaths:)`: convenience discovery found no executable.
- `.invalidConfiguration`: invalid URLs, timeouts, request size, or grep options.
- `.startup`: launch failure or a bridge exit before its first valid response.
  Bridge stderr is included when available.
- `.server(id:message:)`: the daemon returned `ok: false`; the client remains usable.
- `.transport`: pipe failure, bridge exit, or closure caused by another active request.
- `.invalidResponse`: malformed JSON, mismatched IDs, missing or mistyped fields, or an oversized response.
- `.timedOut`: the request or readiness deadline expired.
- `.closed`: a request used a closed client.

Task cancellation uses Swift's `CancellationError`.
The bridge captures a bounded stderr tail for diagnostics.
Daemon startup logs remain in `~/Library/Caches/fplussearch/daemon.log`.

## macOS permissions and sandboxing

This integration isn't promised to work inside the macOS App Sandbox.
Launching an external executable, connecting to its Unix socket, accessing the cache, and indexing files can be restricted.
A security-scoped URL doesn't automatically grant the separately launched daemon access.
Assess signing, entitlements, distribution rules, and process permissions for your app before adopting this integration.

Full Disk Access and ordinary filesystem permissions still apply to the daemon.
The wrapper doesn't bypass them or grant permissions.
Choose a narrow root when you don't need whole-disk search.
