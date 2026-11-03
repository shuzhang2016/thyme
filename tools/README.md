# tools index

The tools are grouped by responsibility. Runtime libraries live in `include/`
and `src/`; everything here is an executable tool, generator, or CI helper.

| Directory | Contents | Typical entry points |
|---|---|---|
| `gen/` | Code generation | `gen/gen_graph` (graph → wiring/scaffold), `gen/gen_contracts` (datatypes.json → contract headers) |
| `apps/` | Runtime applications | `apps/trigger_center`: `trigger_center` (recording/trigger center), `triggerctl` (control CLI), `demo_producer` (demo producer) |
| `replay/` | Replay and export | `replay/thyme-replay` (list/info/play/verify/integrity/schema/export/status), `replay/mcap_export` (single `.thyme.bin` → MCAP) |
| `inspect/` | Read-only inspection and observability | `inspect/thyme_probe` (SHM probe/ABI preflight), `inspect/thyme_py` (Python package reader), `inspect/thyme_log_merge` (multi-process log merging), `inspect/thyme_metrics_exporter` (Prometheus textfile bridge) |
| `ops/` | Operations and test helpers | `ops/upgrade` (upgrade/rollback scripts plus ABI goldens), `ops/fault_matrix` (fault-matrix driver), `ops/coverage` (coverage export) |
| `ci/` | CI check scripts | Documentation links/commands, ABI matrix, bench/eval baselines, regression corpus, quarantine, and more |

Conventions:

- After building, binaries are under `build/tools/<bucket>/<name>/` (for
  example `build/tools/inspect/thyme_probe/thyme_probe`).
- Generators write only `generated/` and scaffolds. Never hand-edit generated
  artifacts; `--check` is the CI gate.
- Read-only tools (`thyme_probe`, `thyme_py`) must not write shared memory or
  unlink resources.
- Operational scripts default to dry-run; destructive actions require an
  explicit flag (see `ops/upgrade/thyme_upgrade.sh`).

Related documentation: `docs/README.md` and `docs/usage/`.
