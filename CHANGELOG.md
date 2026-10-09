# Changelog

## Unreleased

- Add the `FPlusSearch` Swift package for asynchronous access to the shared macOS daemon.
- Add a Homebrew formula in `Formula/fplussearch.rb`, making this repository an explicit-URL custom tap.
- Document Swift integration, the JSON-lines daemon protocol, and Homebrew installation and services.
- Declare Apple Silicon as the engine's supported architecture. Homebrew builds avoid host-specific CPU flags.
- Prevent client disconnects during large responses from killing the shared daemon with `SIGPIPE`.
