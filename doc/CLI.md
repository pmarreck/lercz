# C command-line interface

`lercz` is a small C consumer of `Lerc_c_api.h`, linked to the same library
used by the Zig module. It does not import a Zig module or implement a second
codec. The public C ABI is unchanged. Two local decoder checks reject a
version-field mutation that the Lerc2 checksum does not cover, and they copy
uncompressed samples with `memcpy` so a misaligned blob returns an error
instead of trapping.

Build with `./build-cli` (Nix) or `nix develop -c ./build-cli --backend=direct`.
The command runs the live upstream-release gate, builds the library and CLI,
checks the executable header, then publishes `bin/<os>/<arch>/lercz`.
`./build-cli --debug` selects Debug; `--test` runs the full native suite first.
`./build-all --backend=direct` builds the five declared targets. Upstream owns
this fork's `build/` directory, so the build command remains `build-cli` rather
than replacing that directory. `./run` builds the host CLI and runs it.

Direct installation trees are `zig-out/<target>/<profile>/`; Nix roots are
`.nix-out/<target>/<profile>`. Cross products coexist. Windows products have an
`.exe` suffix and are copied into their platform entrypoint directory. Other
entrypoints are direct symlinks. Runtime tests on a host do not establish runtime
behavior on another operating system. Raw `zig build` and Nix derivations remain
offline and do not check upstream freshness.

## Commands and data

```sh
lercz encode --type f32 --width 256 --height 256 --error 0 pixels.raw -o tile.lerc
lercz info --json tile.lerc
cat tile.lerc | lercz decode @stdin -o pixels.raw
cat tile.lerc | lercz check --json-errors -
printf '[1,2,3,4]' | lercz encode --type u8 --width 2 --height 2 --json-input - > tile.lerc
lercz decode --json tile.lerc
```

`encode` reads raw little-endian samples. Types are `i8`, `u8`, `i16`, `u16`,
`i32`, `u32`, `f32`, and `f64`. Width and height are required; depth and bands
default to one. Values are row-major, depth interleaved within a pixel, bands
sequential. `--error` is the maximum absolute sample error and defaults to zero.
`--json-input` accepts a flat array of finite JSON numbers with exactly the
required number of samples. Integer types reject fractional/out-of-range values;
float types use their normal representable precision. JSON cannot express NaN or
infinity; nonfinite decoded values are represented as `null`.

`--mask PATH` supplies byte validity values, each zero or one, when encoding.
Its size is width*height or width*height*bands. On decode it writes one mask per
band, including all-valid masks. Invalid sample values are unspecified; consumers
must use the masks. `--nodata NUMBER` supplies the same no-data sentinel for all
bands when encoding; decode JSON includes the per-band sentinels. The 4D C API
handles mixed valid/invalid values within a pixel.

`decode` fully decompresses before writing samples. Its JSON representation
contains `info`, `values`, `mask`, and `nodata`. `check` performs the same full
decode and discards samples; `--json` emits a success object. `info` inspects
metadata without full decoding and must not be used as the corruption oracle.
The CLI rejects trailing bytes after complete LERC blobs; this is an additional
framing policy beyond upstream's permissive metadata API.

Inputs default to stdin; `-` and `@stdin` also select it. Outputs default to
stdout; `-` and `@stdout` select it. Paths with spaces and UTF-8 are supported;
Windows arguments and paths use native wide-character adapters. Binary streams
use Windows binary mode. `--` ends option parsing, and named options can precede
or follow the verb. Repeated value options use the last value.

Diagnostics and progress go to stderr. `--json-errors` selects structured errors;
`--json-metadata` selects structured completion metadata. `--progress` and
`--no-progress` override interactive-only completion counts. Codec calls operate
on whole tiles and expose no internal progress callback, so no invented percentage
or ETA is shown. `--ascii`/`--simple`, `--no-color`/`--no-ansi`, and `NO_COLOR`
suppress decoration. Debug builds announce themselves unless `MUTE_DEBUG_STATUS`
is set. `--about` reports the linked codec version and compile target.

The default `--max-bytes 268435456` caps each input and decoded buffer. This is
not an aggregate RSS limit: raw data, validity masks, compressed bytes and
upstream working allocations can coexist. The coverage runner adds a process
deadline. The CLI reads one complete bounded tile, even from a non-seekable pipe.

## Errors

Exit codes are 0 for success, 1 for blob rejection, 2 for usage or I/O (including
invalid encoding requests), and 3 for resource/codec-size limits. Success means
the operation completed, not that arbitrary file content is authentic.

JSON stderr uses `schema: "lercz.error.v1"`. `code` and `detail` are stable static
tokens. `category` separates `codec`, `format`, `limit`, `io`, and `usage`.
`class` is `failure` for a rejected blob and null for operational errors. This
interface has no downstream validator verdict taxonomy.

The record includes `operation`, `stage`, `input`, `input_bytes`, and nullable
`lerc_status`. Status codes preserve the upstream meanings:

| Status | Code | Meaning |
| --- | --- | --- |
| 1 | LercFailed | Generic codec rejection |
| 2 | LercWrongParam | Upstream rejected parameters |
| 3 | LercBufferTooSmall | Buffer limit, separate from generic rejection |
| 4 | LercNaN | Unhandled NaN values |
| 5 | LercHasNoData | No-data handling required |
| 6 | LercDimensionsTooLarge | Codec dimension limit |

Status zero is success and does not produce an error record. Unknown future
statuses retain their numeric value with `LercUnknownStatus`.

Locations are zero-based byte positions in the CLI input. `offset: null` means
unknown, with `offset_is_exact: false`. `block_offset` is null because the public
LERC API does not return an enclosing failing block. `blob_offset: 0` identifies
the input blob, not the location of a defect. `TrailingData` reports the exact
first excess byte in `offset` and the excess byte count in `length`. Other
lengths are unknown. No offset is inferred from a generic codec failure.

This shape was reconciled with validate's library finding conventions. The
CLI's JSON schema is documented here; upstream's C ABI is unchanged, and TIFF
consumers still own their own error mapping.
