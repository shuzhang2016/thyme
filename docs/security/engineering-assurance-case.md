# Restricted engineering assurance case evidence package

> Document nature: restricted engineering argument and evidence index; it does not contain the results of any execution.
>
> This document is **not an ISO 26262 safety case** and does not address ASIL, ASIL
> decomposition or HARA; it does not perform a MISRA C++/AUTOSAR compliance
> assessment, does not claim MISRA/AUTOSAR compliance, does not represent external
> audit, certification or regulatory acceptance, and must not be used to claim
> "ready for submission".

## Goal

On a single Linux host, under the explicit same-uid trust assumption, establish
reviewable, traceable and clearly bounded engineering evidence for the following
scope:

- the thyme data plane: POSIX shared memory, ring/slot publication, and selected-core hot paths;
- the trigger/recorder control plane: local UNIX domain sockets, credential authentication,
  protocol integrity, replay resistance, rejection and audit behavior;
- recorded evidence packages: file inventory, hash chain, optional HMAC and offline verification
  within the declared scope;
- the coding rules, error handling, review, requirement traceability and CI engineering gates
  that support the above.

Excluded are multi-host/fleet trust, network service security, target-board or HIL
acceptance, privacy compliance, arguments about functional-safety integrity levels,
and any external standard certification.

## Claim

All of the following are **restricted claims**; CTest/CI constitute behavioral
evidence only when supported by an actual execution record on the same source,
configuration and toolchain. Test registration or citation in this document is not
execution evidence.

- **C1 — Control-plane boundary.** Under a configuration that denies legacy
  downgrades by default, trigger V3 and recorder-control V2 provide testable kernel
  credential authentication, allowlisting, CRC32, sequence-based replay resistance,
  response deadlines and auditing.
- **C2 — Robustness against specified attack inputs.** The real `record_control`
  service has negative and liveness checks for the registered malformed, oversized,
  storm and path-traversal inputs; this claim does not extend to a complete
  penetration-test conclusion.
- **C3 — Recorded evidence is verifiable.** For sealed packages, or packages
  explicitly marked partial, the file set, sizes, declared hashes and chain can be
  checked within the declared full/metadata/off/mixed scope; when a key is
  configured and protected, the optional HMAC provides symmetric signature
  verification.
- **C4 — Selected engineering constraints are automatically reviewable.** The C++
  heap-allocation sentinels on selected core hot paths, clang-tidy over production
  sources, and the first-party `-Werror` build provide local engineering
  guarantees; this is not extrapolated to a claim of no allocation across the whole
  data plane, absence of defects, or standards compliance.
- **C5 — The evidence chain resists drift.** The mapping from security work items to
  tests/CI, report consistency, and checker negative cases can be verified
  automatically; this claim covers only mapping completeness and does not replace
  requirement correctness or execution results.

## Evidence

"Can support" describes what the evidence is for and does not record the status of
this run. A document proves that a design, rule or boundary has been recorded; only
tests and CI accompanied by actual logs or pipeline records can support the
corresponding execution claim.

| ID | Evidence | Related | Can support | Cannot prove |
|---|---|---|---|---|
| D1 | [Threat model](threat-model.md) | C1–C3 | The argument baseline for assets, trust boundaries, attack surface, existing mitigations and residual risk | That mitigations are effective in the target environment, that the threat enumeration is complete, or that risks have been accepted externally |
| D2 | Requirement–test traceability | C1–C5 | How security requirements are mapped, how evidence is registered, and the open hardware/legal/external-audit dimensions | Execution results of the registered tests, target-hardware validation, or external audit conclusions |
| D3 | [Recorded-evidence package integrity](evidence-integrity.md) | C3 | The manifest format, verification scope, partial semantics, HMAC and offline re-verification contract | Encryption, intrinsic source authenticity, whole-package deletion/rollback protection, or symmetric-key security |
| D4 | [Programming rules](../reference/coding-rules.md) | C4 | Engineering constraints: core libraries avoid exceptions/RTTI, hot paths do not allocate, and publication is in place | MISRA/AUTOSAR compliance, that all code satisfies the constraints, or runtime correctness |
| D5 | [Error-handling audit](../reference/error-handling-audit.md) | C1, C4 | Classification, fixes and review rules for control-plane/startup/sealing error paths in the specified audit snapshot | That current and future code has no silent failures, that scans are exhaustive, or that fault paths have executed |
| D6 | [Review checklist](../reference/review-checklist.md) | C1, C3–C5 | The review entry point for concurrency, ABI, error paths, tests and the language subset | That review actually happened, was sufficient, or that third-party standards review is complete |
| D7 | [Coding standard and static analysis assessment](coding-standard-assessment.md) | C4, C5 | The mapping from local rules to compiler, static analysis, CTest and review evidence, plus gaps against external standards | Clause-by-clause MISRA/AUTOSAR assessment, tool qualification, independent audit, or that all code satisfies the local rules |
| E1 | CTest [`test_trigger_security`](../../tests/test_trigger_security.cpp) | C1 | When actually run: uid allowlisting, rejection of missing credentials, priority clamping, replay/CRC/legacy rejection, and trigger audit behavior | Same-uid isolation, all kernel/deployment combinations, target-board timing, or audit tamper resistance |
| E2 | CTest [`test_trigger_protocol`](../../tests/test_trigger_protocol.cpp) | C1 | When actually run: V3 encoding/decoding, authentication, malformed input, and the client response deadline | Real target latency guarantees, all protocol inputs, or end-to-end recording correctness |
| E3 | CTest [`test_record_control`](../../tests/test_record_control.cpp) | C1 | When actually run: recorder V2 command/state handling, allowlisting, CRC/replay, independent audit, and legacy default-deny | The full recorder business path, same-uid isolation, target-board/HIL, or replay resistance once legacy is enabled |
| E4 | CTest [`test_package_integrity`](../../tests/test_package_integrity.cpp) | C3 | When actually run: real sealing, rejection of tampering/missing files/extra files/bad manifests, HMAC key negative cases, and partial/recovery behavior | Confidentiality of plaintext payloads, detection of whole-package replacement/rollback, key-distribution security, or cross-package rollback protection |
| E5 | CTest [`test_pentest_record_control`](../../tools/ci/pentest_record_control.py) | C2 | When actually run: a valid V2 acceptance control, `POLL ok=0` for each attack input, legacy rejection, no path writes, and report/log/audit evidence | Full-system penetration testing, continuous fuzzing, coverage of other sockets/parsers, or unknown attacks |
| E6 | CTest [`test_core_no_alloc`](../../tests/test_core_no_alloc.cpp) | C4 | When actually run: C++ `new/delete` allocation counts on the selected cyclic/tap/event paths after Start plus warm-up | Recorder drain, other threads/libraries, `malloc`, zero allocation across the whole data plane, memory safety, or real-time behavior |
| E7 | CTest [`test_check_coding_rule_assessment`](../../tools/ci/test_check_coding_rule_assessment.py) | C4, C5 | When actually run: negative cases for missing and duplicate local-rule mappings, fabricated external conclusions, and evidence drift | That the local rules themselves are correct, that mapped tests execute, or external standards compliance |
| E8 | CTest [`test_coding_rule_assessment`](../../tools/ci/check_coding_rule_assessment.py) | C4, C5 | When actually run: rule mapping, CMake/CI/clang-tidy configuration, CTest registration, and report consistency | Re-running static analysis or compilation, complete rule coverage, or external audit |
| E9 | CTest [`test_coding_rule_language_subset`](../../tools/ci/test_coding_rule_language_subset.py) | C4 | When actually run: a positive control confirming that `throw`, `try/catch`, `dynamic_cast` and `typeid` are respectively rejected by `-fno-exceptions`/`-fno-rtti` | That every core-library TU carries the flags, other language rules, runtime behavior, or external standards compliance |
| CI1 | CI job `clang-tidy` in [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) | C4 | A hard gate on diagnostics for all production `src/` main TUs and for first-party `include/` exposed through those TUs, under a fixed CI configuration on GitHub-hosted Ubuntu runners | `tests/`/`tools/` main files, dynamic behavior, MISRA/AUTOSAR compliance, or equivalent results on other toolchains |
| CI2 | CI job `-Werror` in [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) | C4 | A first-party whole-tree build gate in Release with strict warnings and `THYME_WERROR=ON` | Test execution, runtime correctness, zero warnings on other compilers, or MISRA/AUTOSAR compliance |

## Limit

The residual risks and open items below are part of the boundary of this case and
cannot be resolved by the evidence index above.

| Residual risk / open item | Constraint and evidence still required | Threat-model backlink |
|---|---|---|
| Same-uid trust | The same uid/root can still read and write SHM, configuration, packages and audit logs; SHM has no producer authentication. Process/uid isolation, least privilege and deployment review are required | [§1 Trust boundaries, §4 Explicitly unprotected items](threat-model.md#1-assets-and-trust-boundaries) |
| No encryption | Packages and payloads remain plaintext, and directory permissions provide no at-rest confidentiality; a separate encryption and key-management decision is required | [§2 row 10, §4 Package confidentiality](threat-model.md#2-stride-matrix) |
| Package authenticity and rollback | Without an HMAC, a self-consistent manifest can be rewritten; a verifier that holds the HMAC key can also forge. Whole-package deletion/replacement and cross-package rollback still require an out-of-band anchor, key protection and a forensic process | [§2 row 10, §5 Residual risk](threat-model.md#2-stride-matrix) |
| Audit tamper resistance and durability | The audit log has no hash chain and the same uid/root can rewrite it; an un-fsynced tail may be lost on power failure, and a slow disk can block the control thread. Tamper resistance, durability and alerting design are required | [§2 rows 4/16, §4 Audit durability](threat-model.md#2-stride-matrix) |
| Downgrade and bounded replay resistance | Explicitly enabling V1/V2 legacy gives up part of the CRC/sequence protection; PID reuse and eviction from the 64-entry window remain limitations. Deployments must disable unnecessary downgrades and retain configuration evidence | [§2 rows 3/7, §4 Legacy limitations](threat-model.md#2-stride-matrix) |
| Target board / HIL | Host CTest, static analysis and QEMU cannot replace target-board end-to-end timing, resource, fault-injection and long-run evidence; HIL records must be added for each deployment configuration | [§2 Attack-surface residual risk, §5 Risk register](threat-model.md#2-stride-matrix) |
| External standards and safety audit | ISO 26262, ASIL/HARA and MISRA/AUTOSAR are all outside this case; threat enumeration, residual-risk acceptance, key/forensic processes and standards gaps still require independent review | [§2 STRIDE matrix, §5 Risk register](threat-model.md#2-stride-matrix) |

## Verification commands

Run these from the repository root. Configure and build first, then confirm that the
selector lists the nine CTests above; an empty match is not evidence. A
verification record should include the commit, configuration, toolchain/runner
image, time, and raw logs or the workflow run URL; this document does not pre-fill
these dynamic results.

```bash
cmake -S . -B build -DCMAKE_CXX_FLAGS="-Wall -Wextra"
cmake --build build -j"$(nproc)"

ctest --test-dir build -N \
  -R '^(test_trigger_security|test_trigger_protocol|test_record_control|test_package_integrity|test_pentest_record_control|test_core_no_alloc|test_check_coding_rule_assessment|test_coding_rule_assessment|test_coding_rule_language_subset)$'
ctest --test-dir build --output-on-failure \
  -R '^(test_trigger_security|test_trigger_protocol|test_record_control|test_package_integrity|test_pentest_record_control|test_core_no_alloc|test_check_coding_rule_assessment|test_coding_rule_assessment|test_coding_rule_language_subset)$'

python3 tools/ci/check_coding_rule_assessment.py --build-dir build
python3 tools/ci/check_doc_links.py
python3 tools/ci/check_doc_commands.py --no-counts
```

The commands below correspond to the two CI jobs; a local toolchain or version
difference does not constitute the same environment. The clang-tidy CI job uses
clang-tidy-14 as pinned by CI, and the `-Werror` job only builds without running
tests.

```bash
# clang-tidy CI job
cmake -S . -B build-tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python3 tools/ci/run_clang_tidy.py \
  --build-dir build-tidy --clang-tidy clang-tidy-14 -j"$(nproc)"

# -Werror CI job
cmake -S . -B build-werror \
  -DCMAKE_BUILD_TYPE=Release \
  -DTHYME_STRICT_WARNINGS=ON \
  -DTHYME_WERROR=ON
cmake --build build-werror -j"$(nproc)"
```
