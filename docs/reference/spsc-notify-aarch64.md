# SpscRing notify ordering: AArch64 evidence (RT-06 / FUP-33)

This page records the instructions emitted for the existing RT-06 protocol and
its emulated functional regression. It does not change the protocol or compare
its performance with an alternative.

## Build identity and method

The inspected source was `12747b0d51f7e04f0a619965b82ee2e3cc21b2d4`.
The existing Release build used GCC 11.4.0 with `-march=armv8-a+crc -O3
-DNDEBUG`; QEMU was 6.2.0. Evidence came from the real relocatable object and
the final linked executable, not from source-level assumptions:

| Artifact | SHA-256 |
|---|---|
| `/tmp/thyme-aarch64-ci09/build/CMakeFiles/test_spsc_notify_order.dir/tests/test_spsc_notify_order.cpp.o` | `c38386e4705e119106d181cb9fca3359afdfa5e18d2368d88085dd9fe9d3ecea` |
| `/tmp/thyme-aarch64-ci09/build/test_spsc_notify_order` | `9a95da718d64078d72eb92c97d26e6d2ae23040e634ec3e2d12cfd85c0a3c6cd` |

The binutils in this toolchain need its host-side library directory:

```bash
ROOT=/tmp/thyme-aarch64-ci09
export LD_LIBRARY_PATH="$ROOT/toolchain/usr/lib/x86_64-linux-gnu"
OBJDUMP="$ROOT/toolchain/usr/bin/aarch64-linux-gnu-objdump"
OBJ="$ROOT/build/CMakeFiles/test_spsc_notify_order.dir/tests/test_spsc_notify_order.cpp.o"
BIN="$ROOT/build/test_spsc_notify_order"

"$OBJDUMP" -drC -j .text._ZN5thyme8SpscRingILm8EE6NotifyEv "$OBJ"
"$OBJDUMP" -drC -j .text._ZN5thyme8SpscRingILm8EE9WaitUntilEm "$OBJ"
"$OBJDUMP" -dC --disassemble='thyme::SpscRing<8ul>::Notify()' "$BIN"
"$OBJDUMP" -dC --disassemble='thyme::SpscRing<8ul>::WaitUntil(unsigned long)' "$BIN"
"$OBJDUMP" -dC --disassemble='(anonymous namespace)::PublishWithoutWaiter()' "$BIN"
"$OBJDUMP" -dC --disassemble=__aarch64_ldadd4_acq_rel "$BIN"
```

`EmptySeqCst` is inlined into `WaitUntil`; it has no separate symbol. The
`SpscRing<64>` instances in the same executable have the same instruction
shapes. Object and linked-binary excerpts below use section-relative and linked
addresses respectively.

## Observed instructions

### Producer publish and `Notify`

The inlined `push_latest` in `PublishWithoutWaiter` shows both release stores,
followed by the inlined notify fence and relaxed waiter probe:

```text
2e04: c89ffc01  stlr x1, [x0]       # buf_.store(..., release)
2e0c: 889ffed7  stlr w23, [x22]     # head_.store(..., release)
...
2e78: d5033bbf  dmb ish             # Notify: seq_cst fence
2e7c: b9400300  ldr  w0, [x24]      # waiters_.load(relaxed)
2e80: 34000100  cbz  w0, 2ea0       # zero-waiter skip
```

The out-of-line `SpscRing<8>::Notify()` confirms that the fence and probe are
not an artifact of inlining:

```text
3d30: d5033bbf  dmb ish
3d34: 91033000  add  x0, x0, #0xcc
3d38: b9400000  ldr  w0, [x0]
3d3c: 35000080  cbnz w0, 3d4c
```

Thus the current publish path is **`stlr` + `dmb ish` + plain `ldr`**, not a
plain `str` release store. The `stlr` at `2e0c` is the existing release head
store; it is not the alternative "fold the edge into a seq_cst head store"
implementation. The current implementation still has the additional `dmb ish`.
No RT-06 publish/wait path inspected here uses plain `str + dmb ish` as its
release-store encoding. Plain `str` instructions elsewhere in the functions
are stack or non-atomic accesses and are not part of this protocol.

The separate observability API `waiter_count()` uses a seq_cst load and appears
as `ldar` (for example at `2d98`); that is distinct from `Notify`'s intentional
relaxed `ldr` probe.

### Consumer registration and `EmptySeqCst`

`WaitUntil` contains the registration call, the second seq_cst fence, and the
inlined readiness re-check:

```text
3f1c: 940012fd  bl   8b10 <__aarch64_ldadd4_acq_rel>  # waiters_ += 1
3f20: d5033bbf  dmb  ish                              # seq_cst fence
...
3f34: b9400002  ldr  w2, [x0]                         # tail_, relaxed
3f3c: 88dffc00  ldar w0, [x0]                         # head_, seq_cst
3f40: 6b00005f  cmp  w2, w0
```

The registration is an outlined GCC atomic helper. The linked helper dispatches
on the ARM HWCAP LSE bit and contains both implementations:

```text
8b20: b8e00020  ldaddal w0, w0, [x1]  # LSE path
...
8b2c: 885ffc20  ldaxr   w0, [x1]      # non-LSE retry loop
8b30: 0b100011  add     w17, w0, w16
8b34: 880ffc31  stlxr   w15, w17, [x1]
8b38: 35ffffaf  cbnz    w15, 8b2c
```

The seq_cst read-modify-write uses the acquire-release helper; the explicit
`dmb ish` remains present. The seq_cst head re-check is an acquire load (`ldar`),
as expected for an AArch64 seq_cst load. These instructions match the RT-06
ordering argument, so no production-code or memory-order change was made.

## QEMU functional regression

The already-built executables were run with the matching AArch64 sysroot. The
repeat function below was used with the listed test/count/CPU tuples; logs are
in `/tmp/thyme-aarch64-ci09/logs/fup33-*.log` and are not repository artifacts.

```bash
QEMU=/usr/bin/qemu-aarch64-static
SYSROOT=/tmp/thyme-aarch64-ci09/toolchain/usr/aarch64-linux-gnu
BUILD=/tmp/thyme-aarch64-ci09/build
run_repeat() {
  name=$1; count=$2; cpu=$3
  i=1
  while [ "$i" -le "$count" ]; do
    "$QEMU" -cpu "$cpu" -L "$SYSROOT" "$BUILD/$name" || return 1
    i=$((i + 1))
  done
}
```

| Test | QEMU CPU | Result |
|---|---|---|
| `test_spsc_notify_order` | `max` | 100/100 passed |
| `test_spsc_notify_order` | `cortex-a53` | 20/20 passed |
| `test_spsc_ring` | `max` | 20/20 passed |
| `test_notif_pipe` | `max` | 20/20 passed |
| `test_event_wake_word` | `max` | 20/20 passed |
| `test_wake_coalesce` | `max` | 10/10 passed |

Each `test_spsc_notify_order` run covers 400 registration-race iterations,
20,000 ordered payload messages, and the no-waiter skip path. Across 120 runs
this is 48,000 race iterations and 2,400,000 payload messages with zero lost
wakeups, torn payloads, or reordering reported. The two CPU models exercise the
LSE and non-LSE registration helpers under emulation.

## Boundaries

- This is instruction-encoding evidence for this GCC/flags/build and functional
  regression evidence under QEMU, not a result for every compiler or target.
- QEMU does not model real ARM memory timing. These runs do not establish weak
  memory-model behavior on target hardware.
- No target-board performance claim is made. In particular, this does not compare the
  cost of `stlr + dmb ish` with a folded seq_cst-store alternative.
- Under QEMU, `test_wake_coalesce` reports `ptrace unavailable, count skipped`;
  its functional checks pass, but its FUTEX_WAKE syscall-count assertions were
  not validated in this environment.
