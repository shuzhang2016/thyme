#!/usr/bin/env python3
"""Unit tests for thyme_metrics_exporter.py (OBS-07).

The exporter is the only Prometheus surface thyme has, so its contract is
tested end to end through the CLI: fixture inputs, a real AF_UNIX HTTP
server for the status path, golden text output, watch mode, and the atomic
textfile-collector write.
"""

from __future__ import annotations

import json
import os
import re
import socketserver
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
EXPORTER = TOOLS_DIR / "thyme_metrics_exporter.py"
TESTDATA = TOOLS_DIR / "testdata"
REPO_ROOT = TOOLS_DIR.parents[2]
ALERTS = REPO_ROOT / "dashboards" / "alerts" / "thyme-alerts.yml"
DASHBOARD = REPO_ROOT / "dashboards" / "grafana" / "thyme-overview.json"
METRIC_REF = re.compile(r"\b(thyme_[a-z0-9_]+)\b")
SAMPLE = re.compile(
    r'^(?P<name>[a-zA-Z_:][a-zA-Z0-9_:]*)'
    r'(?P<labels>\{[^}]*\})? (?P<value>[^ ]+)$')

try:  # optional: the rule/dashboard contract test is skipped without it
    import yaml  # type: ignore
except ImportError:  # pragma: no cover
    yaml = None


def run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(EXPORTER), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def sample_lines(text: str) -> list[tuple[str, str, str]]:
    out = []
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        m = SAMPLE.match(line)
        if m is None:
            raise AssertionError(f"not a valid exposition line: {line!r}")
        out.append((m.group("name"), m.group("labels") or "",
                    m.group("value")))
    return out


class _StatusHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        self.rfile.readline()  # request line: "GET /status HTTP/1.1"
        while self.rfile.readline() not in (b"\r\n", b"\n", b""):
            continue
        body = (TESTDATA / "status.json").read_bytes()
        self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         b"Content-Length: " + str(len(body)).encode() +
                         b"\r\nConnection: close\r\n\r\n" + body)


class _RecorderStatusHandler(socketserver.DatagramRequestHandler):
    def handle(self) -> None:
        assert self.request[0] == b"status-json"
        self.wfile.write((TESTDATA / "recorder_status.json").read_bytes())


class ExporterTests(unittest.TestCase):
    def test_golden_snapshot(self) -> None:
        proc = run_cli(
            "--status", f"file:{TESTDATA / 'status.json'}",
            "--probe", str(TESTDATA / "probe_list.json"),
            "--hist", str(TESTDATA / "hist.jsonl"),
            "--recorder-status", f"file:{TESTDATA / 'recorder_status.json'}",
            "--audit", str(TESTDATA / "audit.log"),
            "--once",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        expected = (TESTDATA / "expected_metrics.txt").read_text()
        self.assertEqual(proc.stdout, expected)

    def test_every_line_is_valid_exposition(self) -> None:
        proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                       "--probe", str(TESTDATA / "probe_list.json"),
                       "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        samples = sample_lines(proc.stdout)
        self.assertTrue(samples)
        keys = [(name, labels) for name, labels, _ in samples]
        self.assertEqual(len(keys), len(set(keys)), "duplicate series")
        for name in {k[0] for k in keys}:
            self.assertIn(f"# TYPE {name} ", proc.stdout)
            self.assertIn(f"# HELP {name} ", proc.stdout)

    def test_status_unreachable_still_writes_snapshot(self) -> None:
        proc = run_cli("--status", "file:/nonexistent/status.json", "--once")
        self.assertEqual(proc.returncode, 0)
        self.assertIn("thyme_status_up 0\n", proc.stdout)
        self.assertNotIn("thyme_role_state", proc.stdout)
        self.assertIn("status file:/nonexistent/status.json", proc.stderr)

    def test_status_over_unix_socket(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sock_path = os.path.join(tmp, "status.sock")
            server = socketserver.UnixStreamServer(sock_path, _StatusHandler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                proc = run_cli("--status", sock_path, "--once")
            finally:
                server.shutdown()
                server.server_close()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("thyme_status_up 1\n", proc.stdout)
        self.assertIn('thyme_role_up{role="perception",'
                      'role_id="perception_0"} 1\n', proc.stdout)
        self.assertIn('thyme_role_state{role="recorder",'
                      'role_id="recorder_0"} 4\n', proc.stdout)

    def test_recorder_status_over_unix_datagram(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sock_path = os.path.join(tmp, "recorder.sock")
            server = socketserver.UnixDatagramServer(
                sock_path, _RecorderStatusHandler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                               "--recorder-status", sock_path, "--once")
            finally:
                server.shutdown()
                server.server_close()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("thyme_recorder_status_up 1\n", proc.stdout)
        self.assertIn("thyme_recorder_disk_errors_total 11\n", proc.stdout)
        self.assertIn('thyme_recorder_integrity_mode{mode="metadata"} 1\n',
                      proc.stdout)
        self.assertIn("thyme_trigger_audit_drops_total 13\n", proc.stdout)
        self.assertIn("thyme_trigger_fsm_event_drops_total 14\n", proc.stdout)
        self.assertIn("thyme_service_log_sink_open 1\n", proc.stdout)
        self.assertIn("thyme_service_log_records_written_total 21\n", proc.stdout)
        self.assertIn("thyme_service_log_records_suppressed_total 22\n", proc.stdout)
        self.assertIn("thyme_service_log_records_dropped_total 23\n", proc.stdout)
        self.assertIn("thyme_service_log_records_truncated_total 24\n", proc.stdout)
        self.assertIn("thyme_service_log_rotations_total 25\n", proc.stdout)

    def test_watch_emits_one_snapshot_per_iteration(self) -> None:
        proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                       "--watch", "0.05", "--iterations", "3")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.count("# TYPE thyme_status_up gauge"), 3)
        self.assertEqual(
            [l for l in proc.stdout.splitlines() if l == "thyme_status_up 1"],
            ["thyme_status_up 1"] * 3)

    def test_out_file_is_written_atomically(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "thyme.prom")
            proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                           "--out", out, "--once")
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(proc.stdout, "")
            self.assertFalse(os.path.exists(out + ".tmp"))
            text = Path(out).read_text()
        self.assertTrue(text.endswith("\n"))
        self.assertIn("thyme_status_up 1\n", text)

    def test_label_values_are_escaped(self) -> None:
        doc = json.loads((TESTDATA / "status.json").read_text())
        doc["roles"][0]["id"] = 'we"ird\\id'
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "status.json"
            path.write_text(json.dumps(doc))
            proc = run_cli("--status", f"file:{path}", "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn('role_id="we\\"ird\\\\id"', proc.stdout)

    def test_probe_bin_is_invoked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "fake_probe"
            fake.write_text("#!/bin/sh\ncat "
                            f"{TESTDATA / 'probe_list.json'}\n")
            fake.chmod(0o755)
            proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                           "--probe-bin", str(fake), "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("thyme_probe_up 1\n", proc.stdout)
        self.assertIn("thyme_shm_page_pinned_skip_total"
                      '{kind="pinned_slot",page="/h264_front"} 17\n',
                      proc.stdout)

    def test_hist_unknown_series_is_labelled_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "hist.jsonl"
            path.write_text(json.dumps({
                "node": 1, "series": 7, "n": 10, "p50_ns": 1, "p90_ns": 2,
                "p99_ns": 3, "p999_ns": 4, "max_ns": 5}) + "\n")
            proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                           "--hist", str(path), "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn('series_kind="unknown"', proc.stdout)

    def test_sys_series_latest_value_and_probe_labels(self) -> None:
        proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                       "--sys-json", str(TESTDATA / "sys.jsonl"), "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        # Two samples: the newest row wins, deltas are not summed.
        self.assertIn('thyme_process_cpu_user_ns_per_interval{node="3"} '
                      '7000000\n', proc.stdout)
        self.assertIn('thyme_process_rss_kb{node="3"} 4224\n', proc.stdout)
        self.assertIn('thyme_process_open_fds{node="3"} 41\n', proc.stdout)
        # Named probes become a labelled sample, not a dedicated metric.
        self.assertIn('thyme_process_probe{node="3",'
                      'series="overrun_long_callback"} 2\n', proc.stdout)
        self.assertIn("thyme_process_sys_up 1\n", proc.stdout)
        # OBS-13 optional sources: caps bitmask expands to per-source gauges.
        self.assertIn('thyme_process_ext_caps{node="3"} 19\n', proc.stdout)
        self.assertIn('thyme_process_ext_supported{node="3",'
                      'source="cgroup"} 1\n', proc.stdout)
        self.assertIn('thyme_process_ext_supported{node="3",'
                      'source="disk"} 0\n', proc.stdout)
        self.assertIn('thyme_process_cgroup_mem_current_kb{node="3"} 4224\n',
                      proc.stdout)
        self.assertIn('thyme_process_cgroup_io_read_kb_per_interval'
                      '{node="3"} 12\n', proc.stdout)
        self.assertIn('thyme_process_psi_cpu_some_us_per_interval'
                      '{node="3"} 12000\n', proc.stdout)
        self.assertIn('thyme_process_thread_cpu_ns_per_interval'
                      '{node="3",thread="main"} 7000000\n', proc.stdout)
        # OBS-14 machine-level rows reuse --sys-json (host file, node 0).
        self.assertIn('thyme_host_cpu_user_ns_per_interval{node="0"} '
                      '1000000\n', proc.stdout)
        self.assertIn('thyme_host_load1{node="0"} 1.5\n', proc.stdout)
        self.assertIn('thyme_host_mem_total_kb{node="0"} 16384000\n',
                      proc.stdout)
        self.assertIn('thyme_host_file_nr_allocated{node="0"} 1000\n',
                      proc.stdout)
        self.assertIn('thyme_host_core_busy_ns_per_interval'
                      '{core="0",node="0"} 2000000\n', proc.stdout)

    def test_bad_sys_json_sets_up_zero(self) -> None:
        proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                       "--sys-json", "file:/nonexistent/sys.jsonl", "--once")
        self.assertEqual(proc.returncode, 0)
        self.assertIn("thyme_process_sys_up 0\n", proc.stdout)

    def test_probe_info_object_is_accepted(self) -> None:
        row = json.loads((TESTDATA / "probe_list.json").read_text())[0]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "probe_info.json"
            path.write_text(json.dumps(row))
            proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                           "--probe", str(path), "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn('thyme_shm_page_head{kind="pinned_slot",'
                      'page="/h264_front"} 90210\n', proc.stdout)
        self.assertNotIn("thyme_shm_page_tail", proc.stdout)

    def test_probe_spmc_reader_drops_and_legacy_json(self) -> None:
        proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                       "--probe", str(TESTDATA / "probe_list.json"), "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(
            'thyme_shm_spmc_reader_drops_total{kind="spmc_ring",'
            'page="/thyme_control",reader_slot="0"} 1\n', proc.stdout)
        self.assertIn(
            'thyme_shm_spmc_reader_drops_total{kind="spmc_ring",'
            'page="/thyme_control",reader_slot="1"} 2\n', proc.stdout)
        self.assertIn(
            'thyme_shm_page_drops_total{kind="spmc_ring",'
            'page="/thyme_control"} 3\n', proc.stdout)
        reader_samples = [sample for sample in sample_lines(proc.stdout)
                          if sample[0] == "thyme_shm_spmc_reader_drops_total"]
        self.assertEqual(len(reader_samples), 8)
        self.assertNotIn("overwritten", proc.stdout)

        rows = json.loads((TESTDATA / "probe_list.json").read_text())
        spmc = next(row for row in rows if row["kind"] == "spmc_ring")
        for value in (None, "missing"):
            with self.subTest(reader_drop_slots=value):
                legacy = dict(spmc)
                if value == "missing":
                    del legacy["reader_drop_slots"]
                else:
                    legacy["reader_drop_slots"] = None
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "probe_legacy.json"
                    path.write_text(json.dumps([legacy]))
                    proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                                   "--probe", str(path), "--once")
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertIn("thyme_shm_page_drops_total", proc.stdout)
                self.assertNotIn("thyme_shm_spmc_reader_drops_total", proc.stdout)

    def test_channel_manifest_capabilities_and_edge_counters(self) -> None:
        proc = run_cli(
            "--status", f"file:{TESTDATA / 'status.json'}",
            "--channel-manifest", str(TESTDATA / "channel_capabilities.json"),
            "--probe", str(TESTDATA / "probe_channels.json"), "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("thyme_channel_manifest_up 1\n", proc.stdout)
        self.assertIn(
            'thyme_channel_manifest_info{hash="0x0123456789abcdef",version="1"} 1\n',
            proc.stdout)
        info = next(line for line in proc.stdout.splitlines()
                    if line.startswith("thyme_channel_capability_info{")
                    and 'channel="h264"' in line)
        for fragment in (
                'from="camera"', 'to="recorder"', 'shm="h264_front"',
                'transport_kind="pinned_slot"', 'retention="queue"',
                'readers="many"', 'overflow="drop"', 'recordable="true"',
                'replay_gate="true"', 'lease_required="true"',
                'hub_required="false"', 'max_wire_bytes="1966080"',
                'slot_count="8"'):
            self.assertIn(fragment, info)

        # The probe kind is deliberately future_page_kind: mapping is based on
        # manifest capabilities, never the C++ page/type name.
        self.assertIn(
            'thyme_channel_push_fail_total{channel="can",from="can",'
            'overflow="reject",readers="one",retention="queue",'
            'shm="thyme_recr_can",to="recorder",transport_kind="frame"} 5\n',
            proc.stdout)
        self.assertIn(
            'thyme_channel_pinned_skip_total{channel="h264",from="camera",'
            'overflow="drop",readers="many",retention="queue",'
            'shm="h264_front",to="recorder",transport_kind="pinned_slot"} 17\n',
            proc.stdout)
        self.assertIn(
            'thyme_channel_lease_steal_total{channel="h264",from="camera",'
            'overflow="drop",readers="many",retention="queue",'
            'shm="h264_front",to="recorder",transport_kind="pinned_slot"} 1\n',
            proc.stdout)
        self.assertIn(
            'thyme_channel_pinned_skip_total{channel="h264_zero",from="camera",'
            'overflow="drop",readers="one",retention="queue",'
            'shm="h264_zero",to="replay",transport_kind="pinned_slot"} 0\n',
            proc.stdout)
        self.assertIn(
            'thyme_channel_lease_steal_total{channel="h264_zero",from="camera",'
            'overflow="drop",readers="one",retention="queue",'
            'shm="h264_zero",to="replay",transport_kind="pinned_slot"} 0\n',
            proc.stdout)
        self.assertIn(
            'thyme_channel_metric_observed{channel="h264_zero",from="camera",'
            'metric="pinned_skip",overflow="drop",readers="one",'
            'retention="queue",shm="h264_zero",to="replay",'
            'transport_kind="pinned_slot"} 1\n', proc.stdout)
        self.assertIn(
            'thyme_channel_metric_observed{channel="h264",from="camera",'
            'metric="overwritten",overflow="drop",readers="many",'
            'retention="queue",shm="h264_front",to="recorder",'
            'transport_kind="pinned_slot"} 0\n', proc.stdout)
        self.assertIn(
            'thyme_channel_metric_observed{channel="local_latest",from="planner",'
            'metric="pin_skip",overflow="overwrite",readers="one",'
            'retention="latest",shm="",to="estimator",transport_kind="frame"} 0\n',
            proc.stdout)

        runtime_names = {
            "thyme_channel_push_fail_total", "thyme_channel_overwritten_total",
            "thyme_channel_pin_skip_total", "thyme_channel_lease_steal_total",
            "thyme_channel_pinned_skip_total",
        }
        samples = sample_lines(proc.stdout)
        self.assertEqual(len([s for s in samples if s[0] in runtime_names]), 5)
        self.assertEqual(len([s for s in samples
                              if s[0] == "thyme_channel_metric_observed"]), 25)
        self.assertNotIn("thyme_channel_push_fail_total{channel=\"control\"",
                         proc.stdout)
        self.assertNotIn("thyme_channel_push_fail_total{channel=\"h264\"",
                         proc.stdout)
        self.assertNotIn("thyme_channel_overwritten_total{", proc.stdout)
        self.assertNotIn("thyme_channel_pin_skip_total{", proc.stdout)

    def test_channel_manifest_without_probe_marks_all_counters_unobserved(self) -> None:
        proc = run_cli(
            "--status", f"file:{TESTDATA / 'status.json'}",
            "--channel-manifest", str(TESTDATA / "channel_capabilities.json"),
            "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("thyme_channel_manifest_up 1\n", proc.stdout)
        samples = sample_lines(proc.stdout)
        capabilities = [s for s in samples
                        if s[0] == "thyme_channel_capability_info"]
        observed = [s for s in samples
                    if s[0] == "thyme_channel_metric_observed"]
        self.assertEqual(len(capabilities), 5)
        self.assertEqual(len(observed), 25)
        self.assertEqual({value for _, _, value in observed}, {"0"})
        self.assertNotIn("thyme_channel_push_fail_total{", proc.stdout)
        self.assertNotIn("thyme_channel_pinned_skip_total{", proc.stdout)
        self.assertNotIn("thyme_channel_lease_steal_total{", proc.stdout)

    def test_failed_probe_collection_cannot_supply_channel_counters(self) -> None:
        rows = json.loads((TESTDATA / "probe_channels.json").read_text())
        rows[-1]["reader_drop_slots"] = "bad"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "probe_bad.json"
            path.write_text(json.dumps(rows))
            proc = run_cli(
                "--status", f"file:{TESTDATA / 'status.json'}",
                "--channel-manifest", str(TESTDATA / "channel_capabilities.json"),
                "--probe", str(path), "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("thyme_probe_up 0\n", proc.stdout)
        self.assertIn("thyme_channel_manifest_up 1\n", proc.stdout)
        observed = [s for s in sample_lines(proc.stdout)
                    if s[0] == "thyme_channel_metric_observed"]
        self.assertEqual(len(observed), 25)
        self.assertEqual({value for _, _, value in observed}, {"0"})
        self.assertNotIn("thyme_channel_push_fail_total{", proc.stdout)
        self.assertNotIn("thyme_channel_pinned_skip_total{", proc.stdout)

    def test_bad_channel_manifest_sets_up_zero_without_fake_counters(self) -> None:
        doc = json.loads((TESTDATA / "channel_capabilities.json").read_text())
        doc["version"] = 2
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "channel_capabilities.json"
            path.write_text(json.dumps(doc))
            proc = run_cli("--status", f"file:{TESTDATA / 'status.json'}",
                           "--channel-manifest", str(path), "--once")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("thyme_channel_manifest_up 0\n", proc.stdout)
        self.assertIn("unsupported channel manifest version 2", proc.stderr)
        self.assertNotIn("thyme_channel_capability_info", proc.stdout)
        self.assertNotIn("thyme_channel_metric_observed", proc.stdout)

    def test_bad_argument_exits_2(self) -> None:
        proc = run_cli("--watch", "0", "--once")
        self.assertEqual(proc.returncode, 2)


class RuleContractTests(unittest.TestCase):
    """The shipped rules may only name metrics the exporter really emits."""

    @classmethod
    def setUpClass(cls) -> None:
        proc = run_cli(
            "--status", f"file:{TESTDATA / 'status.json'}",
            "--probe", str(TESTDATA / "probe_list.json"),
            "--hist", str(TESTDATA / "hist.jsonl"),
            "--recorder-status", f"file:{TESTDATA / 'recorder_status.json'}",
            "--audit", str(TESTDATA / "audit.log"),
            "--disk", str(TESTDATA), "--shm-dir", str(TESTDATA),
            "--once",
        )
        assert proc.returncode == 0, proc.stderr
        cls.exported = {name for name, _, _ in sample_lines(proc.stdout)}

    @unittest.skipUnless(yaml is not None, "PyYAML not installed")
    def test_alert_metrics_exist(self) -> None:
        doc = yaml.safe_load(ALERTS.read_text())
        pending = 0
        for group in doc["groups"]:
            for rule in group["rules"]:
                names = set(METRIC_REF.findall(rule["expr"]))
                if group["name"] == "thyme.pending-export":
                    pending += 1
                    self.assertIn("thyme_export_pending", rule["labels"], rule)
                    self.assertFalse(names & self.exported,
                                     f"{rule['alert']} names an exported metric")
                    continue
                self.assertNotIn("thyme_export_pending", rule["labels"], rule)
                missing = names - self.exported
                self.assertFalse(
                    missing, f"{rule['alert']}: not exported: {sorted(missing)}")
        self.assertEqual(pending, 0)

    @unittest.skipUnless(yaml is not None, "PyYAML not installed")
    def test_every_rule_has_duration_severity_and_runbook(self) -> None:
        doc = yaml.safe_load(ALERTS.read_text())
        for group in doc["groups"]:
            for rule in group["rules"]:
                self.assertIn(rule["labels"]["severity"],
                              ("critical", "warning"), rule["alert"])
                self.assertTrue(rule["annotations"]["runbook"].startswith(
                    "docs/"), rule["alert"])
                if group["name"] == "thyme.runtime":
                    self.assertIn("for", rule, rule["alert"])

    def test_dashboard_queries_stay_off_unexported_metrics(self) -> None:
        dash = json.loads(DASHBOARD.read_text())
        for panel in dash["panels"]:
            for target in panel["targets"]:
                names = set(METRIC_REF.findall(target["expr"]))
                missing = names - self.exported
                self.assertFalse(
                    missing, f"{panel['title']}: not exported: {sorted(missing)}")


if __name__ == "__main__":
    unittest.main()
