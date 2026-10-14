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

The fork currently has no mutation operators or mutation reports to migrate.
If adding such tests, consult the current shared guidance titled "Corruption
mutation vocabulary uses sniper bolter shotgun and nuke" before selecting a
profile. Its October 3 and 5 refinements supersede the September 29 notice:
shotgun defaults to exactly 16 distinct bit flips in a full 32-byte window,
and nuke denotes the dense pseudorandom overwrite, normally 4096 bytes.
Record the algorithm version and replay parameters, preserve historical
definitions, and count crashes, timeouts, and resource failures separately
from detected corruption. This guidance does not schedule a stress campaign.

## Maintenance status

The September fork documentation, release gate, LERC 4.2.0 merge, and dependent
notifications are complete. The October 14 inbox review found no outstanding
implementation milestone or local mutation/verdict migration. Completed plan
items are preserved in [the plan log](../docs/PLAN_LOG.md). Future development
remains governed by [INTENT.md](../INTENT.md).
