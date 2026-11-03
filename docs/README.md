# thyme documentation index

The documentation is layered by purpose. The top-level `README.md` and
`ARCHITECTURE.md` are the project entry points; this directory holds detailed
material.

## Getting started

| Document | Content |
|---|---|
| [onboarding.md](guides/onboarding.md) | Build in one day → run the demo → test → add a channel |
| [tutorial.md](guides/tutorial.md) | 15-minute end-to-end tutorial |
| [usage/index.md](usage/index.md) | User manual (API / tools / topics, plain Markdown) |

## Guides (task-oriented)

| Document | Content |
|---|---|
| [trigger-config.md](guides/trigger-config.md) | Complete trigger/recording configuration key reference and SIGHUP matrix |
| [recorder-cli.md](guides/recorder-cli.md) | `triggerctl` and status JSON |
| [replay.md](guides/replay.md) | Replay pipeline, gating, export, topology mapping |
| [runbook.md](guides/runbook.md) | Operations: disk full, SHM leaks, trigger storms, crash recovery, package verification |
| [upgrade.md](guides/upgrade.md) | Upgrade/rollback and ABI preflight |
| [orchestration.md](guides/orchestration.md) | systemd startup, dependency order, readiness/health |
| [test-matrix.md](guides/test-matrix.md) | Test layers, CI jobs, quarantine, repeat |
| [bench-baselines.md](guides/bench-baselines.md) | Bench baselines and tolerances |
| [eval-chains.md](guides/eval-chains.md) | eval_chains CPU/I/O/IRQ scenarios |
| [observability-dashboards.md](guides/observability-dashboards.md) | Metric sources, Grafana/Foxglove, alerts |
| [channel-audit.md](guides/channel-audit.md) | Channel observability audit: allowlists and detection matrix for latest/reject/single-slot pinned |
| [python-api.md](guides/python-api.md) | `thyme_py` package reader |

## Reference (contracts, rules, audits)

| Document | Content |
|---|---|
| [abi-compatibility.md](reference/abi-compatibility.md) | ABI rules, version matrix, byte order |
| [glossary.md](reference/glossary.md) | Glossary and primitive contracts |
| [coding-rules.md](reference/coding-rules.md) | Coding rules (CR-01..) |
| [scheduling-model.md](reference/scheduling-model.md) | Scheduling model, RM, admission, overrun |
| [review-checklist.md](reference/review-checklist.md) | Merge-request review checklist |
| [regression-corpus.md](reference/regression-corpus.md) | Fix → regression test mapping |
| [as-built-deviations.md](reference/as-built-deviations.md) | Design claims vs implementation |
| [default-policy-matrix.md](reference/default-policy-matrix.md) | Default failure actions, switching cost, re-evaluation conditions |
| [cacheline-audit.md](reference/cacheline-audit.md) | Layout and false-sharing audit |
| [copy-budget.md](reference/copy-budget.md) | CRC, cross-process SHM, and tap copy budgets |
| [performance-placement.md](reference/performance-placement.md) | H1 budgets, placement constraints, and evidence index |
| [object-pool-aba.md](reference/object-pool-aba.md) | ObjectPool ABA/wraparound analysis |
| [mechanism-policy-audit.md](reference/mechanism-policy-audit.md) | Mechanism/policy boundary |
| [error-handling-audit.md](reference/error-handling-audit.md) | Silent-failure cleanup |
| [priority-inversion-audit.md](reference/priority-inversion-audit.md) | Blocking points and priority inversion |
| [third-party-licenses.md](reference/third-party-licenses.md) | Third-party licenses |

## Design

| Document | Content |
|---|---|
| [triggered-acquisition-design.md](design/triggered-acquisition-design.md) | Triggered acquisition design |
| [adr/README.md](design/adr/README.md) | Architecture decision record index (0001–0016) |
| [api-docs.md](design/api-docs.md) | API comment conventions (brief/ownership/lifetime) |

## Security

| Document | Content |
|---|---|
| [security/threat-model.md](security/threat-model.md) | Threat model and residual risk |
| [security/evidence-integrity.md](security/evidence-integrity.md) | Evidence package integrity and signing |
| [security/coding-standard-assessment.md](security/coding-standard-assessment.md) | Local coding-rule evidence and MISRA/AUTOSAR assessment boundary |
| [security/engineering-assurance-case.md](security/engineering-assurance-case.md) | Evidence index for the constrained engineering assurance case |

> This repository maintains Markdown only and does not generate an HTML site:
> there is no MkDocs or Doxygen `doc` target. `usage/`, `guides/`, and
> `reference/` cross-link with source-relative paths, validated in CI by
> `check_doc_links.py` and `check_doc_commands.py`.
