# PLAN

Completed work: [plan log](docs/PLAN_LOG.md).
Maintenance guidance: [fork maintenance](doc/FORK_MAINTENANCE.md).

## Decoder streaming
- [x] Add a separate forward-only Lerc2 validation API for headers, checksums, constant bands, and raw one-sweep, reporting offsets, codes, and categories and checked against the blob decoder (context: docs/plan_context/stream-parser.md). (done 2026-10-21 22:05 EDT)
- [x] Parse stored RLE masks in the forward-only Lerc2 validation API, including a later band that reuses the previous mask, and check them against the blob decoder (context: docs/plan_context/stream-parser.md). (done 2026-10-23 11:47 EDT)
- [x] Parse tiled bit-stuffing in the forward-only Lerc2 validation API and check it against the blob decoder (context: docs/plan_context/stream-parser.md). (done 2026-10-23 12:45 EDT)
- [x] Parse byte and char Huffman in the forward-only Lerc2 validation API and check it against the blob decoder (context: docs/plan_context/stream-parser.md). (done 2026-10-23 12:59 EDT)
- [x] Parse lossless float and double Huffman in that validation API without calling the blob decoder (context: docs/plan_context/stream-parser.md). (done 2026-10-23 13:19 EDT)

## C CLI and corruption detection
- [x] Render human coverage tables through tsv_to_ansi_table while retaining machine-readable results. (done 2026-10-21 15:23 EDT)
- [x] Align JSON stderr diagnostics with validate's published error contract and preserve unknown locations explicitly. (done 2026-10-21 15:23 EDT)
- [x] Keep the library and C CLI unlocalized. Message catalogs and --lang are out of scope. (done 2026-10-23 14:02 EDT)
- [x] Add a --stream option on the C CLI that calls the forward-only Lerc2 parser for check and decode (context: docs/plan_context/stream-parser.md). (done 2026-10-23 14:22 EDT)
- [x] Verify native tests, five-target builds, Nix packaging, and a fixed corruption replay before shipping. (done 2026-10-21 15:23 EDT)
