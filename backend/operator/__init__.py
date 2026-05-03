"""
Operator-facing components: manual approval queue (CoT gate), mission
lifecycle (start/end/export), and operator overrides (friendly list,
freq blacklist, manual location overrides).

These exist as a separate package because they're the "human in the
loop" surface — distinct from automated fusion / coordination / sensing.
The operator UI talks to these via /api/v1/{approvals,missions,overrides}.
"""
