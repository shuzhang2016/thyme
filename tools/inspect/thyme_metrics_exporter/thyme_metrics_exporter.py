#!/usr/bin/env python3
"""thyme_metrics_exporter — OBS-07 local Prometheus textfile bridge.

thyme ships no Prometheus HTTP server. This sidecar collects the observability
surfaces that *do* exist and renders them in the Prometheus text exposition
format, so a stock node_exporter `--collector.textfile.directory` (or any
scraper that can read a textfile) can serve them:

  * OPS-10 status endpoint      `GET /status` over the AF_UNIX socket
                                (or `--status file:PATH` for a saved snapshot)
  * ECO-02 recorder status      `--recorder-status PATH` (status-json datagram)
  * ECO-09 `thyme_probe list`   `--probe PATH` (JSON written by the probe, e.g.
                                a systemd timer) or `--probe-bin PATH` (run the
                                binary each cycle)
  * ARCH-19 channel capabilities `--channel-manifest PATH` (generated JSON;
                                joined to probe counters by SHM name)
  * OBS-06/OBS-09 histograms    `--hist PATH`, the JSONL written by
                                `thyme_log_merge <perf dumps> --hist --json`
  * SEC-02 audit log            `--audit PATH`, key=value lines
  * filesystem / SHM pressure   `--disk PATH`, `--shm-dir PATH` (statvfs)

Every number below is read from an existing interface; see
docs/guides/observability-dashboards.md for the metric -> source mapping and the
remaining fields that are deliberately not exported.

Exit codes: 0 snapshot written (a failed source only sets *_up 0), 2 usage,
3 the output file could not be written.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import stat
import subprocess
import sys
import time
from typing import Iterable

ROLE_STATES = {
    "not_started": 0,
    "running": 1,
    "terminating": 2,
    "waiting_backoff": 3,
    "gave_up": 4,
    "stopped": 5,
    "unknown": 6,
}

# perf_log.h: PerfSampler::kHistSeries* labels a type-6 histogram block.
HIST_SERIES = {
    0: "latency",
    1: "edge_latency",
    2: "callback_work",
}

QUANTILES = (("0.5", "p50"), ("0.9", "p90"), ("0.99", "p99"),
             ("0.999", "p999"))


class Registry:
    """Ordered, duplicate-checked Prometheus text exposition buffer."""

    def __init__(self) -> None:
        self._metrics: dict[str, dict] = {}

    def add(self, name: str, mtype: str, help_text: str, value: float,
            labels: dict[str, str] | None = None) -> None:
        entry = self._metrics.get(name)
        if entry is None:
            entry = {"type": mtype, "help": help_text, "samples": {}}
            self._metrics[name] = entry
        elif entry["type"] != mtype:
            raise ValueError(f"{name}: type {entry['type']} vs {mtype}")
        key = tuple(sorted((labels or {}).items()))
        if key in entry["samples"]:
            # A duplicate (name, labels) pair would make the textfile
            # unparseable; it is always a collector bug, never input data.
            raise ValueError(f"{name}{dict(key)}: duplicate sample")
        entry["samples"][key] = value

    def render(self) -> str:
        out: list[str] = []
        for name in sorted(self._metrics):
            m = self._metrics[name]
            out.append(f"# HELP {name} {m['help']}")
            out.append(f"# TYPE {name} {m['type']}")
            for labels, value in sorted(m["samples"].items()):
                out.append(f"{name}{_render_labels(labels)} {_render_value(value)}")
        return "\n".join(out) + "\n"


def _escape_label(value: str) -> str:
    return value.replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')


def _render_labels(labels: tuple[tuple[str, str], ...]) -> str:
    if not labels:
        return ""
    inner = ",".join(f'{k}="{_escape_label(v)}"' for k, v in labels)
    return "{" + inner + "}"


def _render_value(value: float) -> str:
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    if value != value:  # NaN
        return "NaN"
    text = f"{value:.3f}".rstrip("0").rstrip(".")
    return text if text not in ("", "-0") else "0"


# ---------------------------------------------------------------------------
# OPS-10 status endpoint
# ---------------------------------------------------------------------------

def read_status_source(src: str, timeout_s: float) -> str:
    """Return the /status body from a unix socket path or `file:PATH`."""
    if src.startswith("file:"):
        with open(src[len("file:"):], "r", encoding="utf-8") as fh:
            return fh.read()
    if os.path.exists(src) and not stat.S_ISSOCK(os.stat(src).st_mode):
        with open(src, "r", encoding="utf-8") as fh:
            return fh.read()
    return _http_get_unix(src, "/status", timeout_s)


def _http_get_unix(path: str, target: str, timeout_s: float) -> str:
    # StatusServer speaks line-oriented HTTP/1.1 and closes after one
    # response; urllib has no AF_UNIX support, so speak it directly.
    req = (f"GET {target} HTTP/1.1\r\nHost: localhost\r\n"
           "Connection: close\r\n\r\n").encode("ascii")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout_s)
        sock.connect(path)
        sock.sendall(req)
        chunks: list[bytes] = []
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            chunks.append(chunk)
    raw = b"".join(chunks)
    head, sep, body = raw.partition(b"\r\n\r\n")
    if not sep:
        raise ValueError("status: malformed HTTP response")
    status_line = head.split(b"\r\n", 1)[0].decode("ascii", "replace")
    parts = status_line.split(" ")
    if len(parts) < 2 or parts[1] != "200":
        raise ValueError(f"status: HTTP {status_line!r}")
    return body.decode("utf-8")


def collect_status(reg: Registry, body: str) -> None:
    doc = json.loads(body)
    reg.add("thyme_status_up", "gauge",
            "1 when the OPS-10 status snapshot was read and parsed.", 1)
    reg.add("thyme_status_healthy", "gauge",
            "StatusSnapshot::healthy (`ok` in the /status JSON).",
            1 if doc.get("ok") else 0)
    reg.add("thyme_app_uptime_seconds", "gauge",
            "StatusSnapshot::uptime_ns rendered as seconds.",
            doc.get("uptime_ms", 0) / 1000.0)
    roles = doc.get("roles", [])
    if "role" in doc:  # GET /status?role=<id> returns one role
        roles = [doc["role"]]
    reg.add("thyme_roles", "gauge",
            "StatusSnapshot::n_roles (roles in this snapshot).", len(roles))
    for role in roles:
        labels = {
            "role_id": str(role.get("id") or "-"),
            "role": str(role.get("role") or "-"),
        }
        state = ROLE_STATES.get(str(role.get("state")), 6)
        health = role.get("health", {})
        reg.add("thyme_role_state", "gauge",
                "MpRoleState: 0 not_started, 1 running, 2 terminating, "
                "3 waiting_backoff, 4 gave_up, 5 stopped, 6 unknown.",
                state, labels)
        reg.add("thyme_role_up", "gauge",
                "1 when the role state is `running`.", 1 if state == 1 else 0,
                labels)
        reg.add("thyme_role_restarts_attempted_total", "counter",
                "OPS-01 supervisor restart attempts for this role.",
                int(role.get("restarts_attempted", 0)), labels)
        reg.add("thyme_role_restarts_succeeded_total", "counter",
                "OPS-01 supervisor restarts that brought the role back.",
                int(role.get("restarts_succeeded", 0)), labels)
        reg.add("thyme_role_last_exit_code", "gauge",
                "Last exited child's exit code (-1: signaled/never reaped).",
                int(role.get("last_exit_code", -1)), labels)
        reg.add("thyme_role_last_term_signal", "gauge",
                "Last terminating signal (0: normal exit/never reaped).",
                int(role.get("last_term_signal", 0)), labels)
        reg.add("thyme_role_stall_age_seconds", "gauge",
                "Age of the role's last progress stamp (OPS-01 stall watch).",
                int(role.get("stall_age_ms", 0)) / 1000.0, labels)
        reg.add("thyme_role_loops_total", "counter",
                "MpControl::proc_loops for this role's process.",
                int(role.get("loops", 0)), labels)
        reg.add("thyme_role_alive_total", "counter",
                "HealthSlot::alive_counter summed over the role's nodes.",
                int(health.get("alive", 0)), labels)
        reg.add("thyme_role_deadline_misses_total", "counter",
                "HealthSlot::deadline_miss_cnt summed over the role's nodes.",
                int(health.get("deadline_misses", 0)), labels)
        reg.add("thyme_role_drops_total", "counter",
                "HealthSlot::drop_cnt summed over the role's nodes.",
                int(health.get("drops", 0)), labels)
        reg.add("thyme_role_error_slots", "gauge",
                "Health slots whose error_code is non-zero (instantaneous "
                "count of nodes in error, not a counter).",
                int(health.get("errors", 0)), labels)
        reg.add("thyme_role_sched_mismatches_total", "counter",
                "SCH-08: threads whose actual policy/priority/affinity differs "
                "from the requested one (latch at Start).",
                int(health.get("sched_mismatches", 0)), labels)
        for node, error_code in health.get("error_codes", {}).items():
            reg.add("thyme_health_slot_error_code", "gauge",
                    "HealthSlot::error_code value for one graph node.",
                    int(error_code), dict(labels, node=str(node)))
        for edge, drops in health.get("drop_counts", {}).items():
            reg.add("thyme_edge_drops_total", "counter",
                    "HealthSlot::drop_cnt for one consumer graph edge.",
                    int(drops), dict(labels, edge=str(edge)))
    rec = doc.get("recorder", {})
    if rec:
        reg.add("thyme_recorder_present", "gauge",
                "1 when the /status snapshot carries a recorder block.",
                1 if rec.get("present") else 0)
        sent = int(rec.get("trigger_relay_sent", 0))
        debounced = int(rec.get("trigger_relay_debounced", 0))
        req = int(rec.get("trigger_req_seq", 0))
        ack = int(rec.get("trigger_ack_seq", 0))
        reg.add("thyme_trigger_relay_sent_total", "counter",
                "MpControl trigger relay messages sent.", sent)
        reg.add("thyme_trigger_relay_debounced_total", "counter",
                "MpControl trigger relay messages coalesced (storm signal).",
                debounced)
        reg.add("thyme_trigger_relay_seq", "gauge",
                "MpControl relay sequence number (wrap at 2^32, OPS-10).",
                req, {"kind": "request"})
        reg.add("thyme_trigger_relay_seq", "gauge",
                "MpControl relay sequence number (wrap at 2^32, OPS-10).",
                ack, {"kind": "ack"})
        reg.add("thyme_trigger_relay_inflight", "gauge",
                "Requests relayed but not yet acknowledged, "
                "(req - ack) mod 2^32.", (req - ack) & 0xFFFFFFFF)
    # OPS-05: per-process append-only log rotation/budget counters. The budget
    # is per process (see docs/guides/trigger-config.md [logging]); roles enforce
    # their own, so this reports the status-serving process.
    log = doc.get("logging", {})
    if log:
        reg.add("thyme_log_budget_limit_bytes", "gauge",
                "OPS-05 per-process log budget in bytes "
                "(0 = per-file caps only).",
                int(log.get("budget_limit_bytes", 0)))
        reg.add("thyme_log_budget_used_bytes", "gauge",
                "Bytes accounted by this process's rotating log writers "
                "(live + retained generations).",
                int(log.get("budget_used_bytes", 0)))
        reg.add("thyme_log_budget_drops_total", "counter",
                "Records dropped because the process log budget was reached "
                "(audit records are never dropped).",
                int(log.get("budget_drops", 0)))
        reg.add("thyme_log_rotations_total", "counter",
                "Rotations across this process's rotating log writers "
                "(audit, .fsm, perf dumps).",
                int(log.get("rotations", 0)))


# ---------------------------------------------------------------------------
# ECO-02 recorder / trigger-center status JSON
# ---------------------------------------------------------------------------

def read_recorder_status_source(src: str, timeout_s: float) -> str:
    if src.startswith("file:"):
        with open(src[len("file:"):], "r", encoding="utf-8") as fh:
            return fh.read()
    if os.path.exists(src) and not stat.S_ISSOCK(os.stat(src).st_mode):
        with open(src, "r", encoding="utf-8") as fh:
            return fh.read()
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout_s)
        sock.bind(f"\0thyme-metrics-{os.getpid()}-{time.time_ns()}")
        sock.connect(src)
        sock.send(b"status-json")
        return sock.recv(65536).decode("utf-8")


def collect_recorder_status(reg: Registry, body: str) -> None:
    doc = json.loads(body)
    if doc.get("kind") != "thyme-recorder-status" or not doc.get("ok"):
        raise ValueError("not a successful thyme-recorder-status document")
    reg.add("thyme_recorder_status_up", "gauge",
            "1 when recorder status-json was read and parsed.", 1)
    reg.add("thyme_recorder_disk_full", "gauge",
            "TriggeredRecorder ENOSPC latch.", 1 if doc.get("disk_full") else 0)
    integrity_mode = doc.get("integrity_mode", "full")
    if integrity_mode not in ("full", "metadata", "off", "mixed"):
        raise ValueError(f"unknown recorder integrity_mode: {integrity_mode}")
    reg.add("thyme_recorder_integrity_mode", "gauge",
            "Evidence manifest integrity mode (constant 1, mode label).", 1,
            {"mode": integrity_mode})
    stats = doc.get("stats", {})
    for field in (
            "captures_done", "triggers_rejected", "descriptors_queued",
            "pending_evicted", "overrun_before_queue", "samples_written",
            "frames_written", "holes_written", "torn_reads",
            "harvest_retries", "session_reattaches", "source_errors",
            "disk_errors", "sync_errors"):
        reg.add(f"thyme_recorder_{field}_total", "counter",
                f"TriggeredRecorder::Stats::{field}.",
                int(stats.get(field, doc.get(field, 0))))
    trigger = doc.get("trigger", {})
    for field in ("audit_drops", "fsm_event_drops"):
        reg.add(f"thyme_trigger_{field}_total", "counter",
                f"CenterStats::{field} from the trigger-center status snapshot.",
                int(trigger.get(field, 0)))
    service_log = doc.get("logging", {})
    reg.add("thyme_service_log_sink_open", "gauge",
            "1 when the service logger writes to its configured file sink.",
            1 if service_log.get("service_sink_open") else 0)
    for field in ("service_records_written", "service_records_suppressed",
                  "service_records_dropped", "service_records_truncated",
                  "service_rotations"):
        suffix = field[len("service_"):] if field.startswith("service_") else field
        reg.add(f"thyme_service_log_{suffix}_total",
                "counter", f"Service logger {field} from the trigger-center snapshot.",
                int(service_log.get(field, 0)))


# ---------------------------------------------------------------------------
# ECO-09 thyme_probe JSON
# ---------------------------------------------------------------------------

def collect_probe(reg: Registry, doc) -> None:
    rows = doc if isinstance(doc, list) else [doc]
    for row in rows:
        if not isinstance(row, dict) or "name" not in row:
            continue
        labels = {
            "page": str(row["name"]),
            "kind": str(row.get("kind", "unknown")),
        }
        reg.add("thyme_shm_page_compatible", "gauge",
                "1 when the page ABI/geometry matched the probe "
                "(0 = incompatible, see runbook §2).",
                1 if row.get("compatible") else 0, labels)
        for metric, field, help_text in (
            ("thyme_shm_page_head", "head",
             "Publish counter (ring/PinnedSlot head, LatestSlot seq/2)."),
            ("thyme_shm_page_tail", "tail",
             "Shared consumer cursor (only ShmSpscRing publishes one)."),
            ("thyme_shm_page_epoch", "epoch",
             "Lease/ring epoch (BF-04 generation tag)."),
            ("thyme_shm_page_drops_total", "drop_count",
             "Container drops (push_fail / dropped_oversize / mirror drop)."),
            ("thyme_shm_page_pinned_skip_total", "skip_count",
             "BF-03: writer skipped a pinned PinnedSlot slot."),
            ("thyme_shm_page_lease_steal_total", "steal_count",
             "BF-04: expired lease reclaimed while its reader was gone."),
        ):
            value = row.get(field)
            if value is None:  # null = this page kind does not carry it
                continue
            reg.add(metric, "counter" if metric.endswith("_total") else "gauge",
                    help_text, int(value), labels)
        reader_drop_slots = row.get("reader_drop_slots")
        if reader_drop_slots is None:
            continue
        for reader_slot, drops in enumerate(reader_drop_slots):
            reg.add("thyme_shm_spmc_reader_drops_total", "counter",
                    "ShmSpmcRing drops observed by one lifetime reader slot.",
                    int(drops), dict(labels, reader_slot=str(reader_slot)))


# ---------------------------------------------------------------------------
# ARCH-19 generated channel capabilities
# ---------------------------------------------------------------------------

CHANNEL_LABEL_FIELDS = (
    "channel", "from", "to", "shm", "transport_kind", "retention",
    "readers", "overflow",
)
CHANNEL_INFO_FIELDS = CHANNEL_LABEL_FIELDS + (
    "recordable", "replay_gate", "lease_required", "hub_required",
    "max_wire_bytes", "slot_count",
)
CHANNEL_COUNTERS = (
    ("push_fail", "Producer push failures for a frame queue."),
    ("overwritten", "Producer-side overwritten values (not reader drops)."),
    ("pin_skip", "Writes skipped by a process-local pinned channel."),
    ("lease_steal", "Expired leases reclaimed by the writer."),
    ("pinned_skip", "Writes skipped by a leased SHM channel."),
)


def _channel_page_name(shm: str) -> str:
    return "/" + shm.lstrip("/") if shm else ""


def _validate_channel_manifest(doc) -> list[dict]:
    if not isinstance(doc, dict) or type(doc.get("version")) is not int:
        raise ValueError("channel manifest must be an object with integer version")
    if doc["version"] != 1:
        raise ValueError(f"unsupported channel manifest version {doc['version']}")
    if not isinstance(doc.get("hash"), str) or not doc["hash"]:
        raise ValueError("channel manifest hash must be a non-empty string")
    rows = doc.get("channels")
    if not isinstance(rows, list):
        raise ValueError("channel manifest channels must be an array")
    allowed = {
        "transport_kind": {"frame", "pinned_slot"},
        "retention": {"latest", "queue"},
        "readers": {"one", "many"},
        # "block" is the pre-rename name for the same producer-visible
        # rejection; accept both so older manifests still parse.
        "overflow": {"overwrite", "drop", "reject", "block"},
    }
    validated = []
    identities = set()
    for index, row in enumerate(rows):
        where = f"channel manifest channels[{index}]"
        if not isinstance(row, dict):
            raise ValueError(f"{where} must be an object")
        for field in CHANNEL_LABEL_FIELDS:
            if not isinstance(row.get(field), str):
                raise ValueError(f"{where}.{field} must be a string")
            if field in allowed and row[field] not in allowed[field]:
                raise ValueError(f"{where}.{field} has unknown value {row[field]!r}")
        for field in ("recordable", "replay_gate", "lease_required",
                      "hub_required"):
            if type(row.get(field)) is not bool:
                raise ValueError(f"{where}.{field} must be a boolean")
        for field in ("max_wire_bytes", "slot_count"):
            if type(row.get(field)) is not int or row[field] < 0:
                raise ValueError(f"{where}.{field} must be a non-negative integer")
        identity = tuple(row[field] for field in
                         ("channel", "from", "to", "shm"))
        if identity in identities:
            raise ValueError(f"{where} duplicates edge identity {identity}")
        identities.add(identity)
        validated.append(row)
    return validated


def _channel_labels(row: dict) -> dict[str, str]:
    return {field: row[field] for field in CHANNEL_LABEL_FIELDS}


def _channel_info_labels(row: dict) -> dict[str, str]:
    labels = _channel_labels(row)
    for field in CHANNEL_INFO_FIELDS[len(CHANNEL_LABEL_FIELDS):]:
        value = row[field]
        labels[field] = str(value).lower() if isinstance(value, bool) else str(value)
    return labels


def _probe_pages_by_channel(doc) -> dict[str, dict]:
    rows = doc if isinstance(doc, list) else [doc]
    pages = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("name"), str):
            continue
        name = _channel_page_name(row["name"])
        if name in pages:
            raise ValueError(f"duplicate probe page {name}")
        pages[name] = row
    return pages


def _channel_counter_values(row: dict, page: dict | None) -> dict[str, int | None]:
    values: dict[str, int | None] = {name: None for name, _ in CHANNEL_COUNTERS}
    if page is None or page.get("compatible") is not True:
        return values
    # Probe's legacy drop_count is push_fail only for the frame queue/SPSC
    # capability combination. In particular, SPMC reader drops and PinnedSlot
    # oversize drops must not be relabelled as push_fail.
    if (row["transport_kind"] == "frame" and row["retention"] == "queue"
            and row["readers"] == "one"
            and row["overflow"] in ("drop", "reject", "block")):
        values["push_fail"] = page.get("drop_count")
    if row["lease_required"]:
        values["pinned_skip"] = page.get("skip_count")
        values["lease_steal"] = page.get("steal_count")
    for name, value in values.items():
        if value is not None and (type(value) is not int or value < 0):
            raise ValueError(f"probe counter for {name} must be a non-negative integer")
    return values


def collect_channel_manifest(reg: Registry, doc, probe_doc=None) -> None:
    rows = _validate_channel_manifest(doc)
    pages = _probe_pages_by_channel(probe_doc) if probe_doc is not None else {}
    reg.add("thyme_channel_manifest_info", "gauge",
            "Generated ARCH-19 channel manifest identity (constant 1).", 1,
            {"version": str(doc["version"]), "hash": doc["hash"]})
    for row in rows:
        labels = _channel_labels(row)
        reg.add("thyme_channel_capability_info", "gauge",
                "Generated capabilities for one graph edge (constant 1).", 1,
                _channel_info_labels(row))
        values = _channel_counter_values(
            row, pages.get(_channel_page_name(row["shm"])))
        for name, help_text in CHANNEL_COUNTERS:
            value = values[name]
            reg.add("thyme_channel_metric_observed", "gauge",
                    "1 when the edge counter has a real runtime source; 0 means "
                    "unobserved, not a measured zero (the counter sample is then "
                    "omitted).", 1 if value is not None else 0,
                    dict(labels, metric=name))
            if value is not None:
                reg.add(f"thyme_channel_{name}_total", "counter", help_text,
                        value, labels)


# ---------------------------------------------------------------------------
# OBS-09 histogram snapshot from `thyme_log_merge --hist --json`
# ---------------------------------------------------------------------------

def collect_hist(reg: Registry, lines: Iterable[str]) -> None:
    budgets: dict[str, tuple[int, int]] = {}
    for line in lines:
        line = line.strip()
        if not line:
            continue
        row = json.loads(line)
        node = str(row.get("node", "?"))
        series = int(row.get("series", 0))
        labels = {
            "node": node,
            "series": str(series),
            "series_kind": HIST_SERIES.get(series, "unknown"),
        }
        reg.add("thyme_perf_samples", "gauge",
                "Samples in the merged OBS-09 histogram (per node/series; "
                "snapshot of the analysed perf dumps, not a monotonic "
                "counter).", int(row.get("n", 0)), labels)
        for label, key in QUANTILES:
            q_labels = dict(labels, quantile=label)
            reg.add("thyme_perf_latency_quantile_ns", "gauge",
                    "OBS-09 bucket-resolution latency percentile in ns "
                    "(+6.25%/-5.56% bucket error; see histogram.h).",
                    int(row.get(key + "_ns", 0)), q_labels)
        reg.add("thyme_perf_latency_max_ns", "gauge",
                "OBS-09 exact observed maximum latency in ns.",
                int(row.get("max_ns", 0)), labels)
        if "period_ns" in row or "deadline_ns" in row:
            budget = (int(row.get("period_ns", 0)),
                      int(row.get("deadline_ns", 0)))
            if node in budgets and budgets[node] != budget:
                raise ValueError(f"conflicting perf budgets for node {node}")
            budgets[node] = budget
    for node, (period, deadline) in budgets.items():
        labels = {"node": node}
        reg.add("thyme_perf_node_period_ns", "gauge",
                "Declared graph-node period from perf type-9 metadata.",
                period, labels)
        reg.add("thyme_perf_node_deadline_ns", "gauge",
                "Declared dispatch deadline from perf type-9 metadata "
                "(0 = no event-node deadline).", deadline, labels)


# ---------------------------------------------------------------------------
# OBS-04 process / resource series (thyme_log_merge --json --sys)
# ---------------------------------------------------------------------------

# series_name -> (metric name, help). Gauge rows keep the last sampled value;
# delta rows keep the last sampling-interval delta and say so in the name.
_SYS_SERIES = {
    "utime_ns": ("thyme_process_cpu_user_ns_per_interval",
                 "User CPU time accumulated during the last OBS-04 sampling "
                 "interval, ns (delta, not a cumulative counter)."),
    "stime_ns": ("thyme_process_cpu_kernel_ns_per_interval",
                 "Kernel CPU time during the last sampling interval, ns "
                 "(delta, not a cumulative counter)."),
    "minflt": ("thyme_process_minor_faults_per_interval",
               "Minor page faults during the last sampling interval."),
    "majflt": ("thyme_process_major_faults_per_interval",
               "Major page faults during the last sampling interval."),
    "vol_ctx": ("thyme_process_voluntary_ctx_per_interval",
                "Voluntary context switches during the last interval."),
    "nonvol_ctx": ("thyme_process_involuntary_ctx_per_interval",
                   "Involuntary context switches during the last interval."),
    "rss_kb": ("thyme_process_rss_kb", "Resident set size, KiB (VmRSS)."),
    "vsize_kb": ("thyme_process_vsize_kb",
                 "Virtual address space, KiB (VmSize)."),
    "threads": ("thyme_process_threads", "Threads in the sampled process."),
    "fd_count": ("thyme_process_open_fds",
                 "Open file descriptors (/proc/self/fd entries)."),
    "fd_limit": ("thyme_process_fd_limit",
                 "RLIMIT_NOFILE soft limit; 4294967295 means unlimited."),
    "io_read_kb": ("thyme_process_io_read_kb_per_interval",
                   "Bytes read from block devices during the last interval, "
                   "KiB (delta)."),
    "io_write_kb": ("thyme_process_io_write_kb_per_interval",
                    "Bytes written to block devices during the last interval, "
                    "KiB (delta)."),
    "io_read_syscalls": ("thyme_process_io_read_syscalls_per_interval",
                         "Read syscalls during the last interval (delta)."),
    "io_write_syscalls": ("thyme_process_io_write_syscalls_per_interval",
                          "Write syscalls during the last interval (delta)."),
    "cpu_delay_ms": ("thyme_process_cpu_delay_ms_per_interval",
                     "Time spent waiting on block I/O (delayacct) during the "
                     "last interval, ms (delta)."),
    # OBS-13 optional sources, off unless THYME_PROC_EXT requests them.
    "cgroup_cpu_usage_us": (
        "thyme_process_cgroup_cpu_usage_us_per_interval",
        "cgroup CPU time consumed during the last interval, us (delta; "
        "cgroup v2 cpu.stat usage_usec or v1 cpuacct.usage)."),
    "cgroup_cpu_throttle_us": (
        "thyme_process_cgroup_cpu_throttled_us_per_interval",
        "cgroup CPU time lost to bandwidth throttling during the last "
        "interval, us (delta)."),
    "cgroup_cpu_nr_throttled": (
        "thyme_process_cgroup_cpu_nr_throttled_per_interval",
        "Throttling periods entered during the last interval (delta)."),
    "cgroup_mem_current_kb": ("thyme_process_cgroup_mem_current_kb",
                             "cgroup memory usage, KiB (memory.current / "
                             "memory.usage_in_bytes)."),
    "cgroup_mem_max_kb": ("thyme_process_cgroup_mem_max_kb",
                          "cgroup memory limit, KiB (4294967295 = "
                          "unlimited)."),
    "cgroup_oom_events": ("thyme_process_cgroup_oom_events_per_interval",
                          "cgroup OOM events during the last interval "
                          "(delta)."),
    "cgroup_io_read_kb": (
        "thyme_process_cgroup_io_read_kb_per_interval",
        "Block-device bytes read by the cgroup during the last interval, KiB "
        "(delta; summed over devices)."),
    "cgroup_io_write_kb": (
        "thyme_process_cgroup_io_write_kb_per_interval",
        "Block-device bytes written by the cgroup during the last interval, "
        "KiB (delta; summed over devices)."),
    "host_psi_cpu_some_us": (
        "thyme_process_psi_cpu_some_us_per_interval",
        "Host-wide CPU stall time (PSI some) during the last interval, us "
        "(delta of the monotonic total)."),
    "host_psi_mem_some_us": (
        "thyme_process_psi_mem_some_us_per_interval",
        "Host-wide memory stall time (PSI some) during the last interval, "
        "us (delta)."),
    "host_psi_mem_full_us": (
        "thyme_process_psi_mem_full_us_per_interval",
        "Host-wide memory stall time with all tasks stalled (PSI full), us "
        "(delta)."),
    "host_psi_io_some_us": (
        "thyme_process_psi_io_some_us_per_interval",
        "Host-wide block-IO stall time (PSI some) during the last interval, "
        "us (delta)."),
    "host_psi_io_full_us": (
        "thyme_process_psi_io_full_us_per_interval",
        "Host-wide block-IO stall time with all tasks stalled (PSI full), "
        "us (delta)."),
    "host_disk_read_kb": (
        "thyme_process_host_disk_read_kb_per_interval",
        "Bytes read from whole block devices host-wide during the last "
        "interval, KiB (delta)."),
    "host_disk_write_kb": (
        "thyme_process_host_disk_write_kb_per_interval",
        "Bytes written to whole block devices host-wide during the last "
        "interval, KiB (delta)."),
    "host_net_rx_kb": (
        "thyme_process_host_net_rx_kb_per_interval",
        "Bytes received on all interfaces except lo during the last "
        "interval, KiB (delta)."),
    "host_net_tx_kb": (
        "thyme_process_host_net_tx_kb_per_interval",
        "Bytes sent on all interfaces except lo during the last interval, "
        "KiB (delta)."),
    # OBS-14 machine-level sampler (supervisor `<prefix>_host_<pid>.bin`).
    "host_cpu_user_ns": ("thyme_host_cpu_user_ns_per_interval",
                         "Aggregate user+nice CPU time during the interval, "
                         "ns (delta, host scope)."),
    "host_cpu_system_ns": ("thyme_host_cpu_system_ns_per_interval",
                           "Aggregate kernel CPU time during the interval, ns "
                           "(delta)."),
    "host_cpu_idle_ns": ("thyme_host_cpu_idle_ns_per_interval",
                         "Aggregate idle CPU time during the interval, ns."),
    "host_cpu_iowait_ns": ("thyme_host_cpu_iowait_ns_per_interval",
                           "Aggregate iowait time during the interval, ns."),
    "host_cpu_irq_ns": ("thyme_host_cpu_irq_ns_per_interval",
                        "Aggregate irq+softirq time during the interval, ns."),
    "host_cpu_steal_ns": ("thyme_host_cpu_steal_ns_per_interval",
                          "Aggregate steal time during the interval, ns."),
    "host_load1_milli": ("thyme_host_load1", "1-minute load average."),
    "host_load5_milli": ("thyme_host_load5", "5-minute load average."),
    "host_load15_milli": ("thyme_host_load15", "15-minute load average."),
    "host_mem_total_kb": ("thyme_host_mem_total_kb", "MemTotal, KiB."),
    "host_mem_available_kb": ("thyme_host_mem_available_kb",
                              "MemAvailable (MemFree before 3.14), KiB."),
    "host_file_nr_allocated": ("thyme_host_file_nr_allocated",
                               "System-wide allocated file handles "
                               "(/proc/sys/fs/file-nr)."),
    "host_file_nr_max": ("thyme_host_file_nr_max",
                         "System-wide file-handle limit; 4294967295 = "
                         "unlimited."),
}

# Bit values of the `ext_caps` series (proc_ext.h ProcExtSource).
_EXT_SOURCES = (("cgroup", 0x01), ("psi", 0x02), ("disk", 0x04),
                ("net", 0x08), ("threads", 0x10))
_THREAD_PREFIX = "thr_"
_THREAD_SUFFIX = "_cpu_ns"


def collect_sys(reg: Registry, lines: Iterable[str]) -> None:
    """Latest value per (node, series_name) from `thyme_log_merge --sys --json`.

    These are snapshots of an already-finished perf dump, one row per sample
    period per series, so the exporter keeps the newest row instead of
    summing: summing would depend on how many samples the dump happens to
    hold. Gauge series carry the level at that sample; delta series carry the
    delta over the sampling period that ended there.
    """
    latest: dict[tuple[str, str], tuple[int, int]] = {}
    for line in lines:
        line = line.strip()
        if not line:
            continue
        row = json.loads(line)
        if row.get("kind") != "sys":
            continue
        name = row.get("series_name")
        if not name:
            # Pre-type-8 dumps have no names; the numeric id is all we have.
            continue
        node = str(row.get("node", "?"))
        ts = int(row.get("ts", 0))
        value = int(row.get("value", 0))
        prev = latest.get((node, name))
        if prev is None or ts >= prev[0]:
            latest[(node, name)] = (ts, value)
    for (node, name), (_, value) in latest.items():
        labels = {"node": node}
        if name == "ext_caps":
            reg.add("thyme_process_ext_caps", "gauge",
                    "OBS-13 extended-source capability bitmask recorded by "
                    "ProcSampler (1 cgroup, 2 psi, 4 disk, 8 net, 16 "
                    "threads); sources are opt-in via THYME_PROC_EXT.",
                    value, labels)
            for source, bit in _EXT_SOURCES:
                reg.add("thyme_process_ext_supported", "gauge",
                        "1 when the OBS-13 extended source was requested and "
                        "the kernel/cgroupfs provided it.",
                        1 if value & bit else 0,
                        {"node": node, "source": source})
            continue
        if name.startswith(_THREAD_PREFIX) and name.endswith(_THREAD_SUFFIX):
            thread = name[len(_THREAD_PREFIX):-len(_THREAD_SUFFIX)]
            reg.add("thyme_process_thread_cpu_ns_per_interval", "gauge",
                    "CPU time (utime+stime) of a registered thread during "
                    "the last interval, ns (delta).", value,
                    {"node": node, "thread": thread})
            continue
        if name.startswith("core") and name.endswith("_busy_ns"):
            core = name[4:-len("_busy_ns")]
            reg.add("thyme_host_core_busy_ns_per_interval", "gauge",
                    "Per-core busy CPU time during the last interval, ns "
                    "(user+nice+system+irq+softirq+steal; idle/iowait "
                    "excluded).", value, {"node": node, "core": core})
            continue
        if name in _SYS_SERIES:
            metric, help_text = _SYS_SERIES[name]
            if name.startswith("host_load"):
                value = value / 1000.0
        else:
            metric = "thyme_process_probe"
            help_text = ("Registered OBS-04 probe (queue depth, overrun "
                         "counter, ...) as recorded; unit depends on the "
                         "probe.")
            labels = {"node": node, "series": name}
        reg.add(metric, "gauge", help_text, value, labels)
    if latest:
        reg.add("thyme_process_sys_up", "gauge",
                "1 when OBS-04 process/resource series were parsed from the "
                "--sys-json snapshot.", 1)


# ---------------------------------------------------------------------------
# SEC-02 audit log (key=value lines)
# ---------------------------------------------------------------------------

def collect_audit(reg: Registry, path: str) -> None:
    records = 0
    unparsed = 0
    decisions: dict[tuple[str, str], int] = {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            fields: dict[str, str] = {}
            for token in line.split(" "):
                key, sep, value = token.partition("=")
                if sep and key:
                    fields[key] = value
            if "req" not in fields:
                unparsed += 1
                continue
            records += 1
            key = (fields.get("decision", "-"), fields.get("code", "-"))
            decisions[key] = decisions.get(key, 0) + 1
    reg.add("thyme_trigger_audit_records", "gauge",
            "Parseable SEC-02 audit records in the current file "
            "(rotates at audit_max_bytes, so not monotonic).", records)
    reg.add("thyme_trigger_audit_unparsed_lines", "gauge",
            "Audit lines without a `req=` field: a partial/torn write "
            "(crash) or a format drift, not the audit_drops counter.",
            unparsed)
    for (decision, code), count in sorted(decisions.items()):
        reg.add("thyme_trigger_audit_decisions", "gauge",
                "Audit records by decision/code in the current file "
                "(snapshot; rotates).", count,
                {"decision": decision, "code": code})


# ---------------------------------------------------------------------------
# Local filesystem pressure (df / df -h /dev/shm equivalents)
# ---------------------------------------------------------------------------

def collect_filesystem(reg: Registry, path: str, metric_prefix: str,
                       label_name: str, what: str) -> None:
    st = os.statvfs(path)
    total = st.f_blocks * st.f_frsize
    avail = st.f_bavail * st.f_frsize
    used = total - st.f_bfree * st.f_frsize
    labels = {label_name: os.path.abspath(path)}
    reg.add(f"{metric_prefix}_size_bytes", "gauge",
            f"Capacity of the {what} filesystem (statvfs f_blocks).",
            total, labels)
    reg.add(f"{metric_prefix}_avail_bytes", "gauge",
            f"Bytes available to an unprivileged writer on the {what} "
            "filesystem (statvfs f_bavail).", avail, labels)
    reg.add(f"{metric_prefix}_used_percent", "gauge",
            f"Used bytes / capacity on the {what} filesystem, 0..100.",
            (used / total * 100.0) if total else 0.0, labels)


def collect_shm_objects(reg: Registry, shm_dir: str) -> None:
    count = 0
    for entry in os.scandir(shm_dir):
        count += 1
    reg.add("thyme_shm_objects", "gauge",
            "Entries in the SHM directory (df -h /dev/shm equivalent for "
            "the OBS-07 SHM-growth alert; see runbook §2).", count,
            {"dir": os.path.abspath(shm_dir)})


# ---------------------------------------------------------------------------
# Snapshot assembly and CLI
# ---------------------------------------------------------------------------

def build_registry(args) -> tuple[Registry, list[str]]:
    reg = Registry()
    errors: list[str] = []

    try:
        body = read_status_source(args.status, args.status_timeout)
        collect_status(reg, body)
    except Exception as exc:  # noqa: BLE001 - any failure means up == 0
        errors.append(f"status {args.status}: {exc}")
        reg.add("thyme_status_up", "gauge",
                "1 when the OPS-10 status snapshot was read and parsed.", 0)

    if args.recorder_status:
        try:
            body = read_recorder_status_source(args.recorder_status,
                                               args.status_timeout)
            collect_recorder_status(reg, body)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"recorder status {args.recorder_status}: {exc}")
            reg.add("thyme_recorder_status_up", "gauge",
                    "1 when recorder status-json was read and parsed.", 0)

    probe_doc = None
    if args.probe or args.probe_bin:
        try:
            if args.probe_bin:
                proc = subprocess.run([args.probe_bin, "list", "--json"],
                                      capture_output=True, text=True,
                                      timeout=args.status_timeout)
                if proc.returncode != 0:
                    raise RuntimeError(
                        f"{args.probe_bin} exited {proc.returncode}")
                text = proc.stdout
            else:
                with open(args.probe, "r", encoding="utf-8") as fh:
                    text = fh.read()
            parsed_probe = json.loads(text)
            collect_probe(reg, parsed_probe)
            probe_doc = parsed_probe
            reg.add("thyme_probe_up", "gauge",
                    "1 when the thyme_probe JSON was read and parsed.", 1)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"probe: {exc}")
            reg.add("thyme_probe_up", "gauge",
                    "1 when the thyme_probe JSON was read and parsed.", 0)

    if args.channel_manifest:
        try:
            with open(args.channel_manifest, "r", encoding="utf-8") as fh:
                manifest_doc = json.load(fh)
            collect_channel_manifest(reg, manifest_doc, probe_doc)
            reg.add("thyme_channel_manifest_up", "gauge",
                    "1 when the ARCH-19 channel manifest was read and parsed.", 1)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"channel manifest {args.channel_manifest}: {exc}")
            reg.add("thyme_channel_manifest_up", "gauge",
                    "1 when the ARCH-19 channel manifest was read and parsed.", 0)

    if args.hist:
        try:
            with open(args.hist, "r", encoding="utf-8") as fh:
                collect_hist(reg, fh)
            reg.add("thyme_hist_up", "gauge",
                    "1 when the thyme_log_merge --hist JSONL was read.", 1)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"hist {args.hist}: {exc}")
            reg.add("thyme_hist_up", "gauge",
                    "1 when the thyme_log_merge --hist JSONL was read.", 0)

    if args.sys_json:
        try:
            with open(args.sys_json, "r", encoding="utf-8") as fh:
                collect_sys(reg, fh)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"sys json {args.sys_json}: {exc}")
            reg.add("thyme_process_sys_up", "gauge",
                    "1 when OBS-04 process/resource series were parsed from "
                    "the --sys-json snapshot.", 0)

    if args.audit:
        try:
            collect_audit(reg, args.audit)
            reg.add("thyme_audit_up", "gauge",
                    "1 when the SEC-02 audit log was read.", 1)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"audit {args.audit}: {exc}")
            reg.add("thyme_audit_up", "gauge",
                    "1 when the SEC-02 audit log was read.", 0)

    if args.disk:
        try:
            collect_filesystem(reg, args.disk, "thyme_filesystem", "mount",
                               "recorder output / log")
        except Exception as exc:  # noqa: BLE001
            errors.append(f"disk {args.disk}: {exc}")

    if args.shm_dir:
        try:
            collect_filesystem(reg, args.shm_dir, "thyme_shm", "mount", "SHM")
            collect_shm_objects(reg, args.shm_dir)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"shm {args.shm_dir}: {exc}")

    return reg, errors


def write_snapshot(text: str, out_path: str | None) -> None:
    if out_path is None:
        sys.stdout.write(text)
        sys.stdout.flush()
        return
    # node_exporter's textfile collector must never read a half-written file:
    # write a sibling temp file, then rename it into place.
    tmp = f"{out_path}.tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, out_path)


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Render thyme observability surfaces as Prometheus text.")
    p.add_argument("--status", default="/run/thyme/status.sock",
                   help="OPS-10 unix socket path, or file:PATH for a saved "
                        "/status body (default /run/thyme/status.sock)")
    p.add_argument("--status-timeout", type=float, default=2.0,
                   help="per-source timeout in seconds (default 2.0)")
    p.add_argument("--recorder-status",
                   help="recorder control unix socket, or file:PATH for a saved "
                        "status-json body")
    p.add_argument("--probe", help="thyme_probe list/info --json output file")
    p.add_argument("--probe-bin", help="thyme_probe binary to run each cycle")
    p.add_argument("--channel-manifest",
                   help="generated channel_capabilities.json; emits per-edge "
                        "capabilities and joins available probe counters")
    p.add_argument("--hist",
                   help="JSONL from `thyme_log_merge <dumps> --hist --json`")
    p.add_argument("--sys-json",
                   help="JSONL from `thyme_log_merge <dumps> --sys --json`: "
                        "OBS-04 process/resource series (RSS, fds, IO, CPU "
                        "delay, probes)")
    p.add_argument("--audit", help="SEC-02 audit log path")
    p.add_argument("--disk", help="filesystem to watch (e.g. the recorder "
                                  "output_dir)")
    p.add_argument("--shm-dir", help="SHM directory to watch (e.g. /dev/shm)")
    p.add_argument("--out", help="write atomically to this path instead of "
                                 "stdout (node_exporter textfile collector)")
    p.add_argument("--once", action="store_true",
                   help="collect one snapshot and exit (default)")
    p.add_argument("--watch", type=float, metavar="SEC",
                   help="collect every SEC seconds until interrupted")
    p.add_argument("--iterations", type=int, default=0,
                   help="with --watch: stop after N snapshots (tests)")
    args = p.parse_args(argv)
    if args.watch is not None and args.watch <= 0:
        p.error("--watch must be > 0")
    if args.iterations < 0:
        p.error("--iterations must be >= 0")
    if args.once and args.watch is not None:
        p.error("--once and --watch are mutually exclusive")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    iteration = 0
    while True:
        reg, errors = build_registry(args)
        for err in errors:
            print(f"thyme_metrics_exporter: {err}", file=sys.stderr)
        try:
            write_snapshot(reg.render(), args.out)
        except OSError as exc:
            print(f"thyme_metrics_exporter: write {args.out}: {exc}",
                  file=sys.stderr)
            return 3
        iteration += 1
        if args.watch is None:
            return 0
        if args.iterations and iteration >= args.iterations:
            return 0
        time.sleep(args.watch)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
