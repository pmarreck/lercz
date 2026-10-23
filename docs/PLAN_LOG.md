# PLAN log

Completed PLAN.md items retired by plan-retire, oldest retirement first.

## Retired 2026-10-14

- [x] [Current work] Explain the Zig/Nix fork and its tests in README.md and preserve purpose in INTENT.md. (done 2026-09-27 16:11 EDT; commit: docs and upstream gate change)
- [x] [Current work] Add an upstream-release build gate with tests for stale, current, malformed, and unavailable release data. (done 2026-09-27 16:11 EDT; commit: docs and upstream gate change)
- [x] [Current work] Verify native/Nix tests and demonstrate the live gate rejecting the currently outdated source. (done 2026-09-27 16:11 EDT; commit: docs and upstream gate change)
- [x] [Upstream upgrade] Merge upstream v4.2.0, synchronize package versions, and expose its new DimensionsTooLarge error in Zig. (done 2026-09-27 19:29 EDT; commit: upstream v4.2.0 merge)
- [x] [Upstream upgrade] Verify oversized-dimension regression coverage, the full suite, both checked build backends, and sandboxed Nix checks. (done 2026-09-27 19:29 EDT; commit: upstream v4.2.0 merge)

## Retired 2026-10-23

- [x] [C CLI and corruption detection] Finish the version-bit corruption regression: ReleaseSafe rejects the version-field mutation instead of trapping in ReadTile. (done 2026-10-21 15:14 EDT)
