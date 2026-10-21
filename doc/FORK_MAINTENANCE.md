# Fork maintenance

## Downstream findings

`lercz` exposes upstream LERC status codes. It does not implement a validator
verdict taxonomy, a known-defect registry, or diagnostic byte offsets. Preserve
the C ABI and let consumers map statuses under their published contracts.
Do not invent exact offsets when upstream supplies none.

The October 14, 2026 downstream classification guidance separates evidence
about a file from limits of the reader. Reader tolerance does not downgrade a
proven violation to a warning. An admitted producer defect requires the
consumer's registry and evidence; this wrapper cannot infer one from a generic
decode error. Size or implementation limits belong to the consumer's reach
reporting, not an automatic corruption verdict. In particular, preserve
`err_dimensions_too_large` distinctly when adapting LERC errors.

Before changing downstream routing, consult the current consumer terminology
and published status contract. The shared guidance titled "Validate verdicts -
FAIL is unexplained damage, KNOWN_DEFECT is an admitted producer defect, WARN
is no proven violation, limits are reach" records the October 14 decision and
warns that an older checkout can contain superseded wording.

## Mutation testing

The fork delegates mutation generation to the pinned `corruption-reader` CLI.
Its resolved profiles and replay records govern the actual mutations, including
undersize behavior; older fleet notices may describe superseded defaults.
[Coverage methodology](CORRUPTION_COVERAGE.md) documents the experiment,
confidence intervals, and separate crash/timeout/resource accounting.

## Maintenance status

The September fork documentation, release gate, LERC 4.2.0 merge, and dependent
notifications are complete. The October 14 inbox review found no outstanding
implementation milestone or local mutation/verdict migration. Completed plan
items are preserved in [the plan log](../docs/PLAN_LOG.md). The subsequent C CLI
and corruption-coverage work is tracked in [PLAN.md](../PLAN.md). Future development
remains governed by [INTENT.md](../INTENT.md).
