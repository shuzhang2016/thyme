#!/usr/bin/env python3
"""OPS-11: validate the deploy units and the orchestration contract.

Two layers, both cheap and deterministic:

1. **Text/contract checks** (always): the trigger-center unit carries every
   directive the orchestration contract promises -- `Type=notify`,
   `NotifyAccess=main`, the two `ExecStartPre` gates
   (`--check-config`/`--check-resources`), `Restart=on-failure`, the watchdog,
   the OPS-07-mirroring limits, the hardening block and the `[Install]`
   target; the acquisition target orders the producer before the recorder;
   `docs/guides/orchestration.md` points at the rollback procedure.

2. **`systemd-analyze verify`** (when the tool is installed): unit syntax and
   directive names. Executable paths are rewritten to a temporary dummy in a
   staged copy, because the real binaries are not installed on the build host;
   the real paths are checked textually. The check says "skipped" when
   systemd-analyze is absent -- it never pretends to have validated syntax.

`--self-test` runs the validator against inline fixtures (positive and
negative) so the checker itself is covered by ctest.

Exit code 0 when every enabled check passed.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

SERVICE = "deploy/systemd/thyme-trigger-center.service"
TARGET = "deploy/systemd/thyme-acquisition.target"
ENV_EXAMPLE = "deploy/thyme-trigger-center.env.example"
ORCH_DOC = "docs/guides/orchestration.md"
PRODUCER_UNIT = "thyme-app.service"
RECORDER_UNIT = "thyme-trigger-center.service"
ACQUISITION_TARGET = "thyme-acquisition.target"

SECTION = re.compile(r"^\[([A-Za-z]+)\]\s*$")
ASSIGNMENT = re.compile(r"^([A-Za-z0-9_.-]+)\s*=\s*(.*)$")


class Unit:
    """Minimal systemd unit reader: sections -> list of (key, value) pairs."""

    def __init__(self, name: str, text: str):
        self.name = name
        self.entries: dict[str, list[tuple[str, str]]] = {}
        section: str | None = None
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith(("#", ";")):
                continue
            match = SECTION.match(line)
            if match:
                section = match.group(1)
                self.entries.setdefault(section, [])
                continue
            match = ASSIGNMENT.match(line)
            if match and section is not None:
                self.entries[section].append((match.group(1), match.group(2)))

    def values(self, section: str, key: str) -> list[str]:
        return [v for k, v in self.entries.get(section, []) if k == key]

    def value(self, section: str, key: str) -> str | None:
        values = self.values(section, key)
        return values[0] if values else None

    def tokens(self, section: str, key: str) -> list[str]:
        out: list[str] = []
        for value in self.values(section, key):
            out.extend(value.split())
        return out

    def has_section(self, section: str) -> bool:
        return section in self.entries


def parse_unit(path: pathlib.Path) -> Unit:
    return Unit(path.name, path.read_text(encoding="utf-8"))


def require(unit: Unit, section: str, key: str | None, expected: str | None,
            errors: list[str], *, contains: bool = False) -> None:
    if not unit.has_section(section):
        errors.append(f"{unit.name}: missing [{section}] section")
        return
    if key is None:
        return
    values = unit.values(section, key)
    if not values:
        errors.append(f"{unit.name}: missing {section}.{key}")
        return
    if expected is None:
        return
    if contains:
        if not any(expected in value for value in values):
            errors.append(
                f"{unit.name}: {section}.{key} must contain '{expected}' "
                f"(got {values!r})")
    elif expected not in values:
        errors.append(
            f"{unit.name}: {section}.{key} must be '{expected}' "
            f"(got {values!r})")


def check_service(unit: Unit) -> list[str]:
    errors: list[str] = []
    # Start-up contract.
    require(unit, "Service", "Type", "notify", errors)
    require(unit, "Service", "NotifyAccess", "main", errors)
    require(unit, "Service", "Environment", "THYME_SD_NOTIFY=1", errors)
    require(unit, "Service", "EnvironmentFile", None, errors)
    require(unit, "Service", "WatchdogSec", "30s", errors)
    require(unit, "Service", "WatchdogSignal", "SIGABRT", errors)
    require(unit, "Service", "TimeoutStartSec", "60s", errors)
    require(unit, "Service", "TimeoutStopSec", "30s", errors)

    # OPS-12/OPS-07 gates: both must run before ExecStart and name a config.
    pres = unit.values("Service", "ExecStartPre")
    config_gate = [v for v in pres if "--check-config" in v]
    resource_gate = [v for v in pres if "--check-resources" in v]
    if not config_gate:
        errors.append(
            "thyme-trigger-center.service: missing ExecStartPre --check-config")
    if not resource_gate:
        errors.append(
            "thyme-trigger-center.service: missing ExecStartPre "
            "--check-resources")
    for gate in config_gate + resource_gate:
        if "/trigger_center" not in gate:
            errors.append(
                f"thyme-trigger-center.service: pre-start gate does not invoke "
                f"trigger_center: {gate}")
        if ".ini" not in gate:
            errors.append(
                f"thyme-trigger-center.service: pre-start gate has no config "
                f"path: {gate}")

    exec_start = unit.value("Service", "ExecStart")
    if exec_start is None:
        errors.append("thyme-trigger-center.service: missing Service.ExecStart")
    else:
        if "/trigger_center" not in exec_start:
            errors.append(
                "thyme-trigger-center.service: ExecStart must run trigger_center")
        if ".ini" not in exec_start:
            errors.append(
                "thyme-trigger-center.service: ExecStart has no config path")
        if exec_start.lstrip().startswith("/bin/sh"):
            errors.append(
                "thyme-trigger-center.service: ExecStart must exec the binary "
                "directly so NotifyAccess=main sees READY=1")

    # Restart semantics and crash-loop guard.
    require(unit, "Service", "Restart", "on-failure", errors)
    require(unit, "Service", "RestartSec", "5s", errors)
    require(unit, "Service", "KillMode", "mixed", errors)
    require(unit, "Unit", "StartLimitIntervalSec", "300", errors)
    require(unit, "Unit", "StartLimitBurst", "5", errors)

    # Dependency order: producers first, recorder follows.
    for key in ("After", "Wants", "PartOf"):
        require(unit, "Unit", key, PRODUCER_UNIT, errors, contains=True)

    # OPS-07-mirroring limits.
    for key, value in (("LimitNOFILE", "65536"), ("TasksMax", "512"),
                       ("IOWeight", "100")):
        require(unit, "Service", key, value, errors)

    # Hardening that the companion thyme-app unit also promises.
    for key, value in (
        ("NoNewPrivileges", "yes"),
        ("ProtectSystem", "strict"),
        ("ProtectHome", "yes"),
        ("PrivateTmp", "yes"),
        ("RestrictAddressFamilies", "AF_UNIX"),
        ("LockPersonality", "yes"),
        ("MemoryDenyWriteExecute", "yes"),
    ):
        require(unit, "Service", key, value, errors)
    rw = unit.value("Service", "ReadWritePaths") or ""
    for path in ("/dev/shm", "/run/thyme"):
        if path not in rw.split():
            errors.append(
                f"thyme-trigger-center.service: ReadWritePaths must include "
                f"{path} (got {rw!r})")

    require(unit, "Service", "User", None, errors)
    require(unit, "Service", "Group", None, errors)
    require(unit, "Service", "RuntimeDirectory", "thyme", errors)
    require(unit, "Service", "StateDirectory", "thyme", errors)

    wanted = unit.values("Install", "WantedBy")
    if ACQUISITION_TARGET not in wanted:
        errors.append(
            f"thyme-trigger-center.service: [Install].WantedBy must include "
            f"{ACQUISITION_TARGET} (got {wanted!r})")
    return errors


def check_target(unit: Unit) -> list[str]:
    errors: list[str] = []
    wants = unit.tokens("Unit", "Wants")
    after = unit.tokens("Unit", "After")
    for dep in (PRODUCER_UNIT, RECORDER_UNIT):
        if dep not in wants:
            errors.append(
                f"{unit.name}: [Unit].Wants must include {dep} (got {wants!r})")
        if dep not in after:
            errors.append(
                f"{unit.name}: [Unit].After must include {dep} (got {after!r})")
    wanted = unit.values("Install", "WantedBy")
    if "multi-user.target" not in wanted:
        errors.append(
            f"{unit.name}: [Install].WantedBy must include multi-user.target "
            f"(got {wanted!r})")
    return errors


def check_contract(root: pathlib.Path) -> tuple[list[str], list[str]]:
    """Return (errors, notes) for the live checkout."""
    errors: list[str] = []
    notes: list[str] = []

    service_path = root / SERVICE
    target_path = root / TARGET
    for path in (service_path, target_path, root / ENV_EXAMPLE,
                 root / ORCH_DOC):
        if not path.is_file():
            errors.append(f"missing {path.relative_to(root)}")
    if errors:
        return errors, notes

    service = parse_unit(service_path)
    target = parse_unit(target_path)
    errors.extend(check_service(service))
    errors.extend(check_target(target))

    env_text = (root / ENV_EXAMPLE).read_text(encoding="utf-8")
    for needle in ("THYME_SD_NOTIFY=1", "THYME_RESOURCE_CHECK"):
        if needle not in env_text:
            errors.append(f"{ENV_EXAMPLE}: must mention {needle}")

    doc_text = (root / ORCH_DOC).read_text(encoding="utf-8")
    for needle, why in (
        ("upgrade.md", "rollback pointer"),
        ("WatchdogSec", "watchdog semantics"),
        ("READY=1", "readiness contract"),
        ("thyme-acquisition.target", "ordered launch"),
        ("ExecStartPre", "start-up gates"),
        ("thyme_probe", "health check"),
        ("test_trigger_center_ready", "readiness test"),
    ):
        if needle not in doc_text:
            errors.append(f"{ORCH_DOC}: must document {why} ('{needle}')")

    # The document must reference the real unit paths, not a stale name.
    for path in (SERVICE, TARGET):
        if path not in doc_text:
            errors.append(f"{ORCH_DOC}: does not reference {path}")

    return errors, notes


# systemd-analyze verify wants ExecStart to exist. Rewrite the deployment
# paths to one dummy in a staged copy; the real paths are checked textually
# above.
def staged_copy(unit: pathlib.Path, dest_dir: pathlib.Path,
                dummy: pathlib.Path) -> pathlib.Path:
    text = unit.read_text(encoding="utf-8")
    text = text.replace("/usr/libexec/thyme/", f"{dummy.parent}/")
    text = text.replace("/opt/usr/thyme/bin/", f"{dummy.parent}/")
    dest = dest_dir / unit.name
    dest.write_text(text, encoding="utf-8")
    return dest


def run_systemd_analyze(root: pathlib.Path,
                        errors: list[str]) -> str:
    tool = shutil.which("systemd-analyze")
    if tool is None:
        return "systemd-analyze not installed: syntax check skipped"
    with tempfile.TemporaryDirectory(prefix="thyme-unit-verify-") as tmp:
        tmp_path = pathlib.Path(tmp)
        # Every rewritten command must exist; one dummy covers both names.
        for name in ("trigger_center", "thyme-app-run"):
            path = tmp_path / name
            path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            path.chmod(0o755)
        staged = [
            staged_copy(root / SERVICE, tmp_path, tmp_path / "trigger_center"),
            staged_copy(root / "deploy/systemd/thyme-app.service", tmp_path,
                        tmp_path / "thyme-app-run"),
        ]
        proc = subprocess.run(
            [tool, "verify", *[str(path) for path in staged]],
            capture_output=True, text=True, check=False)
        detail = (proc.stdout + proc.stderr).strip()
        if proc.returncode != 0:
            errors.append(
                "systemd-analyze verify failed:\n" + detail)
            return "systemd-analyze verify FAILED"
        return "systemd-analyze verify ok" + (
            f" ({detail})" if detail else "")


# ---- self-test fixtures --------------------------------------------------

VALID_SERVICE = """
[Unit]
After=local-fs.target thyme-app.service
Wants=thyme-app.service
PartOf=thyme-app.service
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=notify
NotifyAccess=main
Environment=THYME_SD_NOTIFY=1
EnvironmentFile=-/etc/thyme/thyme-trigger-center.env
ExecStartPre=/opt/usr/thyme/bin/trigger_center --check-config --record-manifest /etc/thyme/record_manifest.h /etc/thyme/x.ini
ExecStartPre=/opt/usr/thyme/bin/trigger_center --check-resources --record-manifest /etc/thyme/record_manifest.h /etc/thyme/x.ini
ExecStart=/opt/usr/thyme/bin/trigger_center --record-manifest /etc/thyme/record_manifest.h /etc/thyme/x.ini
Restart=on-failure
RestartSec=5s
KillMode=mixed
TimeoutStartSec=60s
TimeoutStopSec=30s
WatchdogSec=30s
WatchdogSignal=SIGABRT
LimitNOFILE=65536
TasksMax=512
IOWeight=100
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
RestrictAddressFamilies=AF_UNIX
LockPersonality=yes
MemoryDenyWriteExecute=yes
ReadWritePaths=/dev/shm /run/thyme
User=thyme
Group=thyme
RuntimeDirectory=thyme
StateDirectory=thyme

[Install]
WantedBy=thyme-acquisition.target
"""

VALID_TARGET = """
[Unit]
Wants=thyme-app.service thyme-trigger-center.service
After=thyme-app.service thyme-trigger-center.service

[Install]
WantedBy=multi-user.target
"""


def self_test() -> int:
    failures: list[str] = []
    service = Unit("thyme-trigger-center.service", VALID_SERVICE)
    target = Unit("thyme-acquisition.target", VALID_TARGET)
    if check_service(service):
        failures.append(f"valid service rejected: {check_service(service)}")
    if check_target(target):
        failures.append(f"valid target rejected: {check_target(target)}")

    missing_gate = VALID_SERVICE.replace(
        "ExecStartPre=/opt/usr/thyme/bin/trigger_center --check-resources "
        "--record-manifest /etc/thyme/record_manifest.h /etc/thyme/x.ini\n", "")
    errors = check_service(Unit("thyme-trigger-center.service", missing_gate))
    if not any("--check-resources" in e for e in errors):
        failures.append("missing --check-resources gate was not reported")

    always = VALID_SERVICE.replace("Restart=on-failure", "Restart=always")
    errors = check_service(Unit("thyme-trigger-center.service", always))
    if not any("Restart" in e for e in errors):
        failures.append("Restart=always was not reported")

    unordered = VALID_TARGET.replace(
        "After=thyme-app.service thyme-trigger-center.service\n", "")
    errors = check_target(Unit("thyme-acquisition.target", unordered))
    if not any("After" in e for e in errors):
        failures.append("missing target ordering was not reported")

    no_wanted = VALID_SERVICE.replace(
        "WantedBy=thyme-acquisition.target\n", "")
    errors = check_service(Unit("thyme-trigger-center.service", no_wanted))
    if not any("WantedBy" in e for e in errors):
        failures.append("missing [Install] WantedBy was not reported")

    if failures:
        for failure in failures:
            print(f"self-test FAIL: {failure}", file=sys.stderr)
        return 1
    print("self-test: 5/5 fixtures behaved as expected")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(ROOT),
                        help="repository root (default: this checkout)")
    parser.add_argument("--self-test", action="store_true",
                        help="validate the checker against inline fixtures")
    parser.add_argument("--no-systemd-analyze", action="store_true",
                        help="skip the optional systemd-analyze verify pass")
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root).resolve()

    if args.self_test:
        rc = self_test()
        if rc != 0:
            return rc

    errors, _notes = check_contract(root)
    if not args.no_systemd_analyze:
        status = run_systemd_analyze(root, errors)
    else:
        status = "systemd-analyze verify skipped by request"

    for error in errors:
        print(f"check_deploy_units: {error}", file=sys.stderr)
    if errors:
        print(f"check_deploy_units: FAIL ({len(errors)} error(s))",
              file=sys.stderr)
        return 1
    print(f"check_deploy_units: ok ({SERVICE}, {TARGET}); {status}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
