# PLAN

## Current work
- [x] Explain the Zig/Nix fork and its tests in README.md and preserve purpose in INTENT.md. (done 2026-09-27 16:11 EDT; commit: docs and upstream gate change)
- [x] Add an upstream-release build gate with tests for stale, current, malformed, and unavailable release data. (done 2026-09-27 16:11 EDT; commit: docs and upstream gate change)
- [x] Verify native/Nix tests and demonstrate the live gate rejecting the currently outdated source. (done 2026-09-27 16:11 EDT; commit: docs and upstream gate change)

## Follow-up
- [ ] Import upstream LERC 4.2.0 or later and synchronize package versions; the live gate currently rejects bundled 4.1.1.
