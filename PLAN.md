# PLAN

Completed work: [plan log](docs/PLAN_LOG.md).
Maintenance guidance: [fork maintenance](doc/FORK_MAINTENANCE.md).

## Decoder streaming
- [x] Add a separate forward-only Lerc2 validation API for headers, checksums, constant bands, and raw one-sweep, reporting offsets, codes, and categories and checked against the blob decoder (context: docs/plan_context/stream-parser.md). (done 2026-10-21 22:05 EDT)
- [x] Parse stored RLE masks in the forward-only Lerc2 validation API, including a later band that reuses the previous mask, and check them against the blob decoder (context: docs/plan_context/stream-parser.md). (done 2026-10-23 11:47 EDT)
- [ ] Parse tiled bit-stuffing and Huffman in that validation API without calling the blob decoder (context: docs/plan_context/stream-parser.md).

## C CLI and corruption detection
- [x] Finish the version-bit corruption regression: ReleaseSafe rejects the version-field mutation instead of trapping in ReadTile. (done 2026-10-21 15:14 EDT)
- [x] Build a C CLI over the public LERC C ABI with encode, decode, check, info, standard stream/options, and regression tests. (done 2026-10-21 15:23 EDT)
- [x] Add corruption-coverage using corruption-reader sniper/bolter/shotgun/nuke, clean controls, replayable cases, and 95% confidence intervals. (done 2026-10-21 15:23 EDT)
- [x] Render human coverage tables through tsv_to_ansi_table while retaining machine-readable results. (done 2026-10-21 15:23 EDT)
- [x] Align JSON stderr diagnostics with validate's published error contract and preserve unknown locations explicitly. (done 2026-10-21 15:23 EDT)
- [ ] Decide whether the CLI should gain localization groundwork now or after the interface settles.
- [x] Verify native tests, five-target builds, Nix packaging, and a fixed corruption replay before shipping. (done 2026-10-21 15:23 EDT)
