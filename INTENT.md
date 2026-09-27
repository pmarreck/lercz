# Project intent

## Purpose and users

`lercz` makes Esri's LERC raster-compression library available as a Zig package
for consumers such as `tiffz`. It follows the `zstdz` wrapper pattern: compile
the upstream implementation through Zig and expose its C ABI with a thin Zig
module. Nix supplies the toolchain and reproducible package/test builds.

LERC serves numerical raster data whose precision matters. Callers choose a
maximum error per pixel, including zero for lossless compression.

## Scope and constraints

- Preserve the upstream codec and public C ABI; keep fork-specific changes
  concentrated in packaging, the Zig interface, tests, and documentation.
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
- A pure-Zig codec rewrite, standalone raster application, and changes to the
  LERC format are outside this fork's purpose.

## Evidence of success

- The library builds and links through Zig and Nix.
- Zig callers can reach the C ABI, and lossless encode/decode preserves input
  bytes. See [src/lercz.zig](src/lercz.zig).
- Gate tests must admit current sources as well as reject outdated sources;
  a checker that rejects every build is incorrect.
- Documentation distinguishes upstream LERC features from this fork's additions
  and names any build paths that cannot perform a live freshness check.

## Decision sources

The initial wrapper commit (`f1e7dea`, July 19, 2026) records the thin-wrapper
design and `zstdz` precedent. Peter requested the fork README, this intent
document, and a gate against falling behind upstream on September 27, 2026.

Read this file before substantive changes. [README.md](README.md) contains
usage instructions; [PLAN.md](PLAN.md) tracks current work. Intent describes
the required outcomes, not a claim that the current checkout already meets them.
