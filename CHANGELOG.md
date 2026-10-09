# Changelog

## Unreleased

- Reduce retained memory after indexing and searches by directly mapping large temporary buffers and releasing grep read buffers after each search.
- Reduce duplicate grep candidate storage and release oversized daemon request and response buffers after processing.

- Add the `FPlusSearch` Swift package for asynchronous access to the shared macOS daemon.
- Add a Homebrew formula in `Formula/fplussearch.rb`, making this repository an explicit-URL custom tap.
- Document Swift integration, the JSON-lines daemon protocol, and Homebrew installation and services.
- Declare Apple Silicon as the engine's supported architecture. Homebrew builds avoid host-specific CPU flags.
- Prevent client disconnects during large responses from killing the shared daemon with `SIGPIPE`.
