# Priority-inversion / blocking-point audit (SCH-06)

Scope: every place a real-time worker (`thyme::CyclicThread::Loop`,
`thyme::EventThread::Dispatch`) can block, spin, or contend with a lower
priority thread, plus the I/O paths that are often wrongly assumed to be on
the RT path. The rule is simple:

> **A recorder, visualizer, retention sweep, log rotation, or `/proc` read must
> never run in the RT dispatch thread.** RT threads may hand data to a
> lock-free/SHM structure and return; everything slow lives on a host thread.

Audit date: 2026-10-24 (SCH-03/04/06 runtime batch). Code references are to
the `aplus/schedrt` tree.

## 1. Blocking points on the RT path

| # | Blocking point | Primitive | Runs in | Holding dependency / wait-for | Risk | Mitigation / status |
|---|---|---|---|---|---|---|
| 1 | `EventThread::WakeHub::WaitUntil` (`src/runtime/event_thread.cpp`) | Linux futex (`FUTEX_WAIT`); Apple `pthread_cond_timedwait` | RT worker, while idle | Waits for a producer/`Notify`; no lock held. Futex is **non-PI** | Low: unbounded only until idle-poll deadline. A low-priority producer that is preempted can delay the wake | Register-then-check protocol, `idle_poll_ns` bound, single wake word per node. Keep producers at least as high priority as consumers when coupling RT chains |
| 2 | `EventThread::WakeHub::Notify` | futex wake (`FUTEX_WAKE`), condvar broadcast on Apple | Producer thread (often RT) | No lock on Linux; Apple condvar mutex is held briefly | Low; wake syscall cost, no inversion | Waiter-count fast path skips the syscall when nobody sleeps (PERF-14) |
| 3 | `CyclicThread::SleepUntil` | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` | RT worker, idle | None | Low; signal EINTR loop is bounded | Absolute deadline, stop-aware |
| 4 | `CyclicThread::WaitAllUntil` | 100 µs spin + `WaitUntil` on the first empty input | RT worker | Producer readiness; no lock | Low/medium: a slow producer bounds the tick; a fused input set turns the period into a deadline | Period bounds the wait; `AllReady()` re-checked after every wake. Avoid `wait_inputs: period` for soft inputs |
| 5 | `SpscRing::WaitUntil` / `Notify` (`include/thyme/runtime/spsc_ring.h`) | futex / condvar | Consumer RT thread; producer on push | No lock; register-then-check | Low | Same protocol as the wake hub; waiter count gates the syscall |
| 6 | `NotifPipe::BeginWrite` (`include/thyme/runtime/notif_pipe.h:98`) | Bounded retry loop over `free_` (`WaitUntil` slices) | Producer (may be RT) | Consumer draining the pipe | Medium under sustained consumer stall: producer spends bounded slices, then returns nullptr (drop), never spins forever | RT-16 in-place publish, bounded retries, caller credits drops and does not retry |
| 7 | `LatestSlot` seqlock read / write | `seq_cst` loads/stores, bounded retry | Reader or writer (RT) | Writer can be preempted while a reader retries | Medium: a mid-write RT writer makes the reader spin (bounded retries, then a torn-read outcome); priority inversion if writer priority < reader | Writes are short and wait-free; retries bounded (RT-09 counters). Do not put long work between `Begin`/`Commit` |
| 8 | `ObjectPool` free-list acquire/release (`include/thyme/runtime/object_pool.h`) | CAS/`for(;;)` loops | RT producer/consumer | Concurrent pool users | Low/medium: lock-free, ABA-protected; a preempted holder of a slot does not hold a lock, only the slot | Bounded retry paths, generation counter (RT-04). Pool sizing/admission is SCH-05/SCH-12 |
| 9 | Big-frame lease pin/wait (`include/thyme/runtime/pinned_slot_store.h`) | Reader pin state checks + bounded retry | Reader (RT) | Writer replacing a slot | Medium: a slow reader forces the writer to skip (counted), never blocks the writer indefinitely | BF-06 lease/pin, `kOverrun`/`kPinnedSkip` outcomes counted; no unbounded wait |
| 10 | `RecordMirror` tap (`include/thyme/runtime/record_mirror.h`) | seqlock write into SHM page | RT producer | Recorder is a separate reader; no lock | Low: a slow recorder never blocks the tap; a full slot drops and counts (`AddMirrorDrop`) | Rate-limited warning only (`kOnceThenEvery1000`, `record_mirror.h:285`); no per-message log. Arena sized at startup |
| 11 | Log sink `log::WriteRecord` (`src/runtime/log.cpp`) | Process-wide `pthread_mutex` + one `write(2)`; rotation does `unlink`/`rename`/`open` | **Any caller**, including an RT callback that logs | Holds the sink mutex across the write/rotation; another thread may hold it | **High**: a non-PI mutex shared with a non-RT logger can invert; a full disk stalls `write`; rotation renames in the caller | Contract CR-03: no unconditional per-message logging on hot paths; all hot sites use `_RL` rate limiting. **Fix below**: rotation is now deferred when the caller is in an RT section. Remaining risk is the `write` itself and the mutex; the long-term fix is an async/lock-free log ring (follow-up) |
| 12 | `TriggeredRecorder::Tick` / `DrainSettle` / `CloseSlice` | `write`/`fsync`/`rename`, token bucket, `fdatasync` | Host recorder thread only (thread-free library driven by `trigger_center_main`) | Filesystem latency; no RT lock | None on RT path **if the host wiring is kept** | Verified: `tools/apps/trigger_center/trigger_center_main.cpp` calls `rec.Tick()` in the main loop; no generated node or `EventThread` calls into the recorder. I/O priority is IDLE/best-effort switchable (6.3) |
| 13 | `retention::Manager::Enforce` / `Scan` / `MarkIncomplete` (`src/record/retention.cpp`) | Recursive `lstat`, `unlink`, `rmdir`, manifest hashing | `trigger_center` main thread (startup + after capture + 5 min tick) | Filesystem; can take seconds on a full disk | None on RT path; **would be high** if called from a callback | Verified: only `trigger_center_main.cpp` calls it. No runtime/thread header references retention. Keep it that way; the API takes an explicit active-package exclusion set |
| 14 | SHM lifecycle scan / unlink (`src/runtime/shm_lifecycle.cpp`, probe) | `shm_open`/`shm_unlink`/`stat`/directory scan | Supervisor/probe/startup | Filesystem / `/dev/shm` | None on RT path | Verified: not referenced from `CyclicThread`/`EventThread` or generated Dispatch |
| 14a | Visualization / probe readers (`thyme_probe`, Foxglove bridge, status exporter) | read-only `mmap` + seqlock reads | Separate process/thread | None held by the producer | Low: a slow reader only falls behind (drops/gaps counted); it cannot block the writer | Read-only probe API (ECO-11); producers write lock-free slots. Do not link visualization I/O into a node callback |
| 15 | `/proc` sampling (`ProcSampler`) | `read()` of `/proc/self/stat` / `status` | Dedicated `ProcSampleThread` (default 1 Hz) or host tick | `/proc` | None on RT path | OBS-04 H1 ledger; `proc_reads()` regression test proves loop iterations never read `/proc` |
| 16 | Perf dump / histogram flush (`PerfLogDumper`) | file `write`, optional `fsync` | Host/dump thread | Filesystem | None on RT path | Dumps are periodic from a normal thread; `PerfSampler::Note` is a fixed-ring store |
| 17 | `StatusServer` | `poll`/`accept`/`send` on a UNIX socket | Dedicated pthread | Client sockets | None on RT path | Independent thread, bounded response |
| 18 | `MpSupervisor` `fork`/`waitpid`/backoff | process syscalls | Supervisor process/thread | Kernel | None in node RT threads | Roles are separate processes; parent only supervises |
| 19 | Trigger center control/FSM (`src/record/trigger_center.cpp`) | socket `poll`, audit `write`, rate limiting | Host tick thread | Filesystem/socket | None on RT path | Center is thread-free and host-driven; audit writes are bounded and rotated on the host |

### What is *not* a shared lock

The mirror tap, SPSC/SPMC rings, latest slots, `HealthSlot`, and the event
wake word are **lock-free** or single-writer structures. They can invert only
through scheduler priority (a low-priority thread preempted while a high
priority thread retries), not through a mutex held across a slow operation.
The only cross-thread **mutex** reachable from a callback is the log sink.

## 2. Fix implemented in this batch

### RT-context hook

`include/thyme/runtime/realtime_guard.h` provides a thread-local depth and
`rt::ScopedRealtimeSection`. It is entered around `CyclicThread::Loop()` and
`EventThread::Dispatch()` — one increment/decrement per callback batch, never
per message, no allocation, no syscall.

### Log rotation is deferred out of the RT context

`RotatingFile::RotateIfNeeded()` now checks `rt::InRealtimeSection()` after
the size comparison. In an RT section it appends the record to the live file
and returns without `unlink`/`rename`/`open`, counting the deferral in
`LogBudget::deferred_rotations()`. The next non-RT append (or an explicit
`RotateIfNeeded()` from a host thread) performs the rotation. The size cap was
already specified as soft (at most one record/block overshoot), so RT logging
— which is rate-limited by contract — can only delay rotation, not break the
file format.

Verified by `tests/test_realtime_guard.cpp` (`mr_gate`):

- ordinary threads see `InRealtimeSection() == false`;
- `EventThread::Dispatch` and `CyclicThread::Loop` see `true`;
- an `Append` that crosses the cap inside an RT section performs **zero**
  rotations and increments `deferred_rotations`;
- the next non-RT call rotates, and an explicit `RotateIfNeeded` rotates
  again.

### Retention and recorder I/O — verified, no code change needed

`grep -R "Enforce("` finds exactly the host calls in
`tools/apps/trigger_center/trigger_center_main.cpp`; `RetentionManager` is not
included by `event_thread.*`, `cyclic_thread.*`, or generated node bases.
`TriggeredRecorder` is a thread-free library: its `Tick`/drain/seal run in the
host loop. Shifting either into a callback would be a design regression, so
the audit keeps this as an invariant rather than adding a runtime guard.

## 3. Residual risks and follow-ups

1. **Log sink mutex + write** — deferred rotation removes the rename/file
   churn from RT, but a callback that *does* log still takes the process-wide
   mutex and issues a `write(2)`. CR-03 forbids unconditional per-message
   logging; the remaining hardening is an async bounded ring in front of the
   sink (future OBS item). Until then, treat any new `THYME_LOG_*` call in a
   `Loop`/`Dispatch`/`On*` as a review blocker.
2. **Non-PI futex** — Linux futexes do not inherit priority. A preempted
   low-priority producer can delay an RT consumer wake by the producer's
   remaining runtime. Mitigations are placement/priority design (RM/DM is
   SCH-02) and the idle-poll bound, not a framework change. `SCHED_DEADLINE`
   is deliberately out of scope (SCH-07).
3. **`LatestSlot`/ring retry under a preempted writer** — bounded retries
   turn a long inversion into a drop/gap, counted by RT-09 counters. Keep
   critical sections short; do not call user code between begin/commit.
4. **Recorder package marker for overruns** — `overrun_session::count()` is a
   process-local marker updated once per deadline miss (SCH-04). Writing it
   into a capture package would require the recorder process to read the
   producer's counter through shared memory (an ABI change) and to snapshot it
   per capture. Not done in this batch; the package format's
   `CaptureStatsRecord::flags` remains the extension point
   (`kCaptureFlagTruncated` is bit 0; a future `kCaptureFlagOverrun` would be
   bit 1).

## 4. Review checklist

- [ ] New `Loop`/`Dispatch` path: no file I/O, no `/proc`, no unconditional
      log, no sleep/`fsync`/`rename`.
- [ ] New shared structure between RT and non-RT: lock-free or
      single-writer; if a mutex is unavoidable, document the holder and the
      priority relationship.
- [ ] Recorder/retention/visualization calls stay in the host loop.
- [ ] New hot-path logging uses `_RL` with a bounded `RatePolicy`.
