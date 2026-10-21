# HVS foreground turnaround and vendor-equivalent buffered writes

Board: `root@10.129.113.92`. Repository: `/root/zhangsr/shimetapi_hybrid_vision_toolkit`, branch `hvs_quick_save`. Tested 2026-10-22.

## Corrected performance comparison

The user's vendor screenshot is valid: 418 MiB copied from `/dev/shm` in 1.76 s, displayed as 236.96 MB/s (the vendor calculation actually uses MiB). Its copy function closes the destination without fsync. That measures application/kernel-cache completion, not confirmed physical storage completion.

Our previous 211 s raw result waited for fsync and wrote 2,254,539,264 bytes, about five times that payload. Our previous compressed result additionally included compression. Those figures were not a comparable basis for rejecting the vendor's 200+ MB/s observation. Moreover, only about 31 MB/s of HVS payload is EVS; APS adds about 119 MB/s of uncompressed RAW10 at 30 fps.

Same completion semantics, similar size, on this board:

| Case | Payload | Foreground save/close time | Throughput |
|---|---:|---:|---:|
| Vendor screenshot | 418 MiB | 1.76 s | 236.96 MiB/s |
| New HVS `native --wait-save` | 438,802,944 bytes = 418.476 MiB | **1.50473 s** | **278.106 MiB/s** |
| Explicit fsync after the HVS close-only return | Same files | **47.7955 s additional wait** | A different completion metric |

The HVS run actually captured both APS and EVS for 2.92 s, 87 buffers per stream, and saved original data, index and summary. It did not compress, hash, read back, or fsync. The explicit fsync measurement was performed afterward on the four files, separately from the save time; the summary was deliberately not relabelled as if the recorder itself had waited. This is a screenshot comparison and one new HVS run, not a claim that HVS is always faster than the vendor implementation.

Large writes can still be throttled by Linux dirty-page writeback and memory pressure. The board had dirty_ratio=20 and dirty_background_ratio=10; their actual byte thresholds are kernel-derived. No global VM or storage settings were changed. A high buffered copy rate for 418 MiB does not guarantee the same sustained rate for 2.1 GiB or an unlimited sequence of sessions.

## Implemented workflow

`scripts/record.sh --save-format fast` now returns after capture and camera shutdown. The original recorder process continues the existing low-CPU compressed save with fsync in the background. No fork is performed from an active camera/SDK process: Python spawns the recorder into its own session before initialization, forwards Ctrl+C/SIGTERM while foreground capture is active, and tails its file log until camera handoff.

The C++ process joins both producers, closes the hardware, releases the capture lock, and discards unused whole arena pages. Every captured byte remains in its arena for saving/retry. It publishes `capture.done` before bulk saving. Therefore the next capture may start while the previous writer still runs. New capture budgets are checked against current MemAvailable; there is no unbounded hidden queue.

- `capture.done`: capture is over and the camera lock has been released. Its capture status is not a storage-completion status.
- `writer.json`: writer PID, process start identity, log path and final marker path.
- `<session>.record.log`: child capture/save diagnostics, beside the session directory.
- `save.failed`: saving failed; a live writer retains RAM and waits for `retry.request`.
- `summary.txt`: final files have been published. For fast this includes fsync; for native the durability field explicitly says `kernel_writeback_pending` (not fsync-confirmed).

`python3 hvs.py status --output PATH` distinguishes capturing, saving, final completion, live save failure, and interrupted processes. PID start identity avoids treating a reused PID as the writer. Playback/export still require a finalized summary, not the capture handoff marker.

## Actual consecutive 15-second captures

Both used `--max-mib 2304`, one second warmup, native dual VIN, gain 0 dB and I2C bus 6. The second command started immediately after the first command returned, without waiting for save completion.

| Session | Capture duration | APS / EVS buffers | Capture end to foreground return | Entire command wall time, including allocation/init/warmup/capture/shutdown |
|---|---:|---:|---:|---:|
| fast15_a | 15 s | 447 / 447 | **0.905 s** | 21.303 s |
| fast15_b | 15 s | 447 / 447 | **0.911 s** | 21.441 s |

The first writer was still `saving` when the second full capture finished. Both eventually completed with fsync. Each had 2,254,539,264 original payload bytes, no reported capture get errors/timeouts or capacity rejection. VIN frame IDs were unavailable (447 per stream); zero gap counters therefore must not be interpreted as proof of no upstream loss.

Concurrent background saves took about 91.84 / 91.90 s each before final summary publication. Their compressed payloads were 437,297,324 / 434,489,740 bytes. This work was not made instantaneous or falsely counted as completed: it was removed from the camera turnaround wait. Persistent acquisition throughput is still limited by storage/processing bandwidth and available RAM; with long bursts, wait for earlier writers or reduce the capture budget/duration.

## Commands and semantics

```bash
cd /root/zhangsr/shimetapi_hybrid_vision_toolkit
# Recommended for rapid next-shot availability; background compression + fsync.
bash scripts/record.sh --save-format fast --seconds 15 --max-mib 2304 \
  --output /app/recordings/new_session
python3 hvs.py status --output /app/recordings/new_session

# Vendor-equivalent, no compression, no fsync wait; measure close-only speed.
bash scripts/record.sh --save-format native --wait-save --seconds 3 --max-mib 640 \
  --output /app/recordings/native_new

# Keep the previous synchronous fast behavior when final durable files are needed.
bash scripts/record.sh --save-format fast --wait-save --seconds 15 --max-mib 2304 \
  --output /app/recordings/wait_new
```

Without `--wait-save`, both fast and native detach after capture. The native worker writes `.vin.bin`; the fast worker retains `.vin.zst`. `raw` remains raw+fsync and synchronous; `zstd` remains the original full-readback compressed path; `full` remains unchanged. Direct C++ recorder invocation still waits in its own process; the Python/script launcher provides the foreground handoff. Direct `hvs.py record` retains its existing default format `full`.

## Validation and artifacts

- Board Release recorder build succeeded.
- Recorder integration covers raw/native round-trip byte equality, padded RAW10, native zero-sync/pending-durability metadata, compressed checksum corruption, export, stop/capacity behavior and retry.
- New background tests cover default/wait-save routing, status transitions and PID identity, failure/retry retention, and a second fake capture while the first writer is blocked on saving.
- Existing archive routing and VIN/storage CLI tests passed; actual native timestamp inspection passed.
- Source and benchmark evidence: `/app/recordings/hvs_turnaround_validation_20261022/`, `/tmp/hvs_turnaround_benchmark.py`, `/tmp/hvs_turnaround_*tests.log`.
- Saved all three measured sessions and their foreground/background logs; neither the vendor sample nor system drivers were modified.
