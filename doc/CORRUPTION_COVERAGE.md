# Corrupt-input detection coverage

`./corruption-coverage` drives `corruption-reader` consumer CLI v1 and pipes
each generated case to the C CLI's full `check` decoder. It uses the named
`sniper`, `bolter`, `shotgun`, and `nuke` profiles from the pinned tool; it does
not implement a local substitute. The initial pin is corruption_reader 0.7.0,
commit `ac62a738a17ca61a8c74f57952138972ce66b7b6`.

```sh
nix develop
./build-cli --backend=direct
./corruption-coverage --trials 1000 --seed 42 good-tile.lerc another-tile.lerc
./corruption-coverage --format json --output evidence-run good-tile.lerc > summary.json
./corruption-coverage --profile shotgun --param shotgun:bits=8 good-tile.lerc
```

The known-good corpus is explicit. Use files independently known to be valid;
a successful pristine decode alone does not establish intended pixel content.
The test suite includes an independent C caller for bidirectional codec/CLI
interoperability, clean sample round trips, malformed inputs, and bounded real
corruption-reader cases. Extended campaigns remain in this script.

Every pristine source is snapshotted and accepted through stdin before any
mutation starts. Each file/profile then gets a fixed, predetermined number of
trials (default 1000), using the requested seed and corruption-reader's round
numbers. There is no early stopping or retry-until-success. `--profile` can
select a subset of the four names; repeated selection is deduplicated.
`--param PROFILE:KEY=JSON` forwards configuration to the external tool.

Current defaults come from the pinned executable. In particular shotgun flips
16 distinct bits in a 32-byte window, while nuke overwrites 4096 bytes and fits
shorter input according to its profile. The full case record, including the
resolved profile and any undersize behavior, is the replay authority. Older
fleet descriptions of random shotgun counts or non-fitting nukes are not used.

## Counts and confidence

Each file/profile has its own row. Detection is a normal, typed codec rejection
or the CLI's strict framing rejection, with those two counts also reported
separately. Accepted mutations remain misses even if their output might differ
from the original. Actual byte equality is checked independently of the tool's
`applied` status. Reported no-ops and applied-but-unchanged cases are excluded
from the mutation denominator and counted separately. A zero denominator has
no rate or confidence interval.

Crashes, timeouts, resource limits and operational errors are never credited as
detection. Changed cases with those outcomes stay in the denominator; the run
is marked incomplete and exits nonzero. No-op inputs must remain unchanged and
be accepted. A reject-everything decoder cannot pass the pristine controls.

The JSON includes a two-sided 95% Clopper-Pearson interval for each row and a
simultaneous interval using Bonferroni: with M file/profile comparisons, each
row uses alpha=0.05/M. The union bound gives at least 95% family coverage under
the binomial sampling assumptions, without requiring independence across rows.
The table displays these simultaneous intervals, rounded outward to two decimal
percentage points; JSON retains the computed precision. Calculations invert binomial
tails in log space, with closed-form boundary cases and 50 bisection steps.
Tests include the independently published numerical example from
[NIST's binomial confidence interval reference](https://www.itl.nist.gov/div898/handbook/prc/section2/prc241.htm).

The sampling model treats pseudorandom rounds as independent draws from each
profile's distribution on each fixed source. Replaying the same seed is not
additional independent evidence. Repeated draws of the same mutation are valid
sampling with replacement, not coverage of unique bit positions. The intervals
do not establish a detection rate for unseen file types, arbitrary adversarial
changes, or all possible LERC files. All trials detecting corruption still give
a lower bound below 100%. There is no invented minimum acceptable detection
rate or hypothesis threshold; choose those before using the report as a release
decision. Increasing the predetermined trial count narrows uncertainty.

## Evidence and output

The evidence directory must be new. By default one is created under ignored
`.corruption-coverage/`. It contains:

- `summary.json`, `summary.tsv`, source snapshots and SHA-256 hashes;
- decoder/corruptor identity strings and executable hashes;
- each file/profile's complete `records.jsonl` from corruption-reader;
- per-case outcomes and JSON stderr diagnostics, including accepted cases;
- clean-control diagnostics and corruptor stderr.

The script deletes its temporary mutation files; records plus the preserved
source snapshot reproduce them exactly:

```sh
jq -c 'select(.item==17)|.record' evidence/f0-sniper/records.jsonl > case.json
corruption-reader apply --case case.json evidence/source-0.lerc |
  lercz check --json-errors -
# In scripts inspect PIPESTATUS for both programs; never hide a corruptor failure.
```

Human tables are TSV rendered by `tsv-to-ansi-table`; `--format json` and
`--format tsv` retain machine-readable values. `--ascii` selects plain glyphs
and no color. File numbers correspond to `sources` in the JSON. Use
`TSV_TO_ANSI_TABLE`, `CORRUPTION_READER`, or `--cli` to select explicit tools;
`./test` passes its freshly built CLI rather than trusting PATH.

The Nix development shell pins both sibling tools. corruption_reader is private
and its input uses authenticated Git over SSH. Direct CLI compilation itself
has no dependency on either measurement tool. A missing required tool fails
visibly; no test or campaign silently skips corruption coverage.

## Recorded miss

A ReleaseFast campaign of 1000 trials per file and profile, seed 42, on three
fixed tiles accepted one sniper mutation and rejected the other 11,999. The
accepted change flipped the Lerc2 version byte from 6 to 4. That field is
outside the Fletcher checksum, so the blob still checked as valid and the
shorter version-4 header made the decoder return success with the wrong
values. ReleaseSafe trapped on the resulting misaligned sample load before it
could return.

The library now requires each decoded band to consume exactly its declared
blob, and it copies uncompressed samples with `memcpy`. The fixed decoder
rejects this blob with `LercFailed` and still round-trips versions 2 through 6.
`tests/ffi/reference.c` holds that regression. Replaying the same seed is a
check of this fix, not a new sample, and the original campaign is not a result
for the fixed decoder.
