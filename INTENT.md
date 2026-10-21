# Project intent

## Purpose and users

`lercz` makes Esri's LERC raster-compression library available as a Zig package
for consumers such as `tiffz`. It follows the `zstdz` wrapper pattern: compile
the upstream implementation through Zig and expose its C ABI with a thin Zig
module. Nix supplies the toolchain and reproducible package/test builds.

A small C CLI exercises that same public C ABI for encoding, decoding, blob
inspection, and full-decode checks. Its stdin interface supports reproducible
corruption experiments using the external `corruption-reader` tool. See
[the CLI contract](doc/CLI.md) and [coverage methodology](doc/CORRUPTION_COVERAGE.md).

LERC serves numerical raster data whose precision matters. Callers choose a
maximum error per pixel, including zero for lossless compression.

## Scope and constraints

- Preserve the upstream codec and public C ABI, except two local decoder
  checks: a decoded band must consume exactly its declared blob, and
  uncompressed samples are copied by byte count. The Lerc2 version field sits
  outside the checksum, so those checks close a wrong-output decode. Keep
  other fork-specific changes concentrated in packaging, the Zig interface,
  tests, and documentation.
- Expose the C functions through `lercz.c` and provide Zig constants for the
  public data types and error codes. Allocation policy, error mapping, and TIFF
  container handling belong to consumers.
- Retain C++ exception support, which the upstream implementation requires.
- Keep Nix derivations sandbox-compatible and their inputs pinned. Linux Nix
  packages use musl targets; other targets use Zig's target selection.
- Refuse supported builds when the bundled LERC is behind the latest stable
  upstream LERC release. Updating a wrapper version string alone is not an
  upstream upgrade; import and verify the actual upstream source changes.
- The supported `./build-checked` entrypoint queries upstream before invoking
  Zig or Nix and fails if freshness cannot be verified. Raw build graphs and
  deterministic tests stay offline for sandboxed packaging, downstream
  dependencies, and maintenance. See README for the enforcement boundary.
- A pure-Zig codec rewrite, a raster editing application, and changes to the
  LERC format are outside this fork's purpose.

## Evidence of success

- The library builds and links through Zig and Nix.
- Zig callers can reach the C ABI, and lossless encode/decode preserves input
  bytes. See [src/lercz.zig](src/lercz.zig).
- Gate tests must admit current sources as well as reject outdated sources;
  a checker that rejects every build is incorrect.
- Documentation distinguishes upstream LERC features from this fork's additions
  and names any build paths that cannot perform a live freshness check.
- The C CLI uses the public header exclusively and interoperates with an
  independent C caller. Clean controls pass before corrupt-input experiments.
- Corruption coverage reports replayable trials and 95% confidence intervals,
  including simultaneous bounds across its file/profile comparisons. Crashes,
  limits, no-ops and accepted mutations cannot be passed off as detection.

## Decision sources

The initial wrapper commit (`f1e7dea`, July 19, 2026) records the thin-wrapper
design and `zstdz` precedent. Peter requested the fork README, this intent
document, and a gate against falling behind upstream on September 27, 2026.

Read this file before substantive changes. [README.md](README.md) contains
usage instructions; [PLAN.md](PLAN.md) tracks current work. Intent describes
the required outcomes, not a claim that the current checkout already meets them.
