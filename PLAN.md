# PLAN

## Current work
- [x] Explain the Zig/Nix fork and its tests in README.md and preserve purpose in INTENT.md. (done 2026-09-27 16:11 EDT; commit: docs and upstream gate change)
- [x] Add an upstream-release build gate with tests for stale, current, malformed, and unavailable release data. (done 2026-09-27 16:11 EDT; commit: docs and upstream gate change)
- [x] Verify native/Nix tests and demonstrate the live gate rejecting the currently outdated source. (done 2026-09-27 16:11 EDT; commit: docs and upstream gate change)

## Upstream upgrade
- [x] Merge upstream v4.2.0, synchronize package versions, and expose its new DimensionsTooLarge error in Zig. (done 2026-09-27 19:29 EDT; commit: upstream v4.2.0 merge)
- [x] Verify oversized-dimension regression coverage, the full suite, both checked build backends, and sandboxed Nix checks. (done 2026-09-27 19:29 EDT; commit: upstream v4.2.0 merge)
