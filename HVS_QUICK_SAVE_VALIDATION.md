# HVS quick save: board validation, 2026-10-21

> Historical fsync-waiting measurements. These are not directly comparable with the vendor close-only speed display or the new foreground handoff time. See HVS_TURNAROUND_VALIDATION_20261022.md for the corrected comparison and background workflow.

Board: `root@10.129.113.92`, repository `/root/zhangsr/shimetapi_hybrid_vision_toolkit`, branch `hvs_quick_save`.

## Implementation and choice of default

The vendor `01_evs_live_player/evs_live_player.cpp` caches EVS raw MIPI in DDR/tmpfs, then copies sequentially with a 4 MiB buffer. It does not encode EVT2, hash, read back, or fsync the destination. Our native dual-VIN capture already caches both raw streams in a process arena before saving.

Added `--save-format raw` to write each original VIN stream once with a reusable 4 MiB buffer, preserving RAW10, stride/padding and frame metadata. Unlike the vendor sample, completion waits for fsync. The SD card makes uncompressed output slower than compressed output, so raw is an explicit option rather than the default.

`--save-format fast` now uses Zstandard level -1 with frame checksums, without the extra FNV pass or complete read/decompress/byte-compare pass. File close and fsync still precede publication of the completion marker. Integrity is checked when compressed frames are read. The old level-1, hash and full-readback path remains as `--save-format zstd`; `full` remains unchanged. Capture itself and sensor controls are unchanged.

## Actual dual-camera runs

Each run used 15 seconds, 1 second warmup, and a 2816 MiB memory budget. All three completed by duration with 447 APS frames and 447 EVS packets, zero reported VIN frame-ID gaps in either stream. VIN counters do not prove absence of losses upstream of VIN.

| Mode | Original payload | Saved payload | Save wall time, including final publication |
|---|---:|---:|---:|
| `raw` | 2,254,539,264 bytes | 2,254,539,264 bytes | 211.132 s |
| `fast` | 2,254,539,264 bytes | 544,499,670 bytes | 57.8019 s |
| `zstd` (original fast algorithm) | 2,254,539,264 bytes | 487,035,632 bytes | 81.2333 s |

New fast took approximately 29% less save time than the original algorithm in these runs. Compression ratio is scene dependent; raw retains more bytes on storage, and level -1 compressed output was about 12% larger than level 1. These were sequential captures of the same setup, not identical replayed input, and system load/cache state can affect results. The raw run also overlapped compilation. This is not a guarantee for every scene or storage device.

`summary.txt` records stage times before writing/syncing the summary and final renames; the terminal completion message above includes that final work. Fast summary: compression 39.782 s, write/metadata 6.120 s, stream sync 11.868 s, full readback 0 s. Original summary: compression 36.823 s, write/hash/metadata 25.808 s, stream sync 1.581 s, readback/compare 16.996 s. These stage timings overlap kernel writeback and should not be interpreted as independent device benchmarks.

Recordings retained:

- `/app/recordings/hvs_quick_save_20261021_15s_v2` (`raw`)
- `/app/recordings/hvs_quick_save_20261021_fast15` (`fast`)
- `/app/recordings/hvs_quick_save_20261021_zstd15` (original algorithm)

Example:

```bash
cd /root/zhangsr/shimetapi_hybrid_vision_toolkit
bash scripts/record.sh --save-format fast --seconds 15 --max-mib 2816 \
  --output /app/recordings/new_session
# Native sequential raw save, especially for faster destination storage:
bash scripts/record.sh --save-format raw --seconds 15 --max-mib 2816 \
  --output /app/recordings/new_raw_session
```

Use a new output directory and a memory budget supported by current MemAvailable. `scripts/record.sh` defaults to fast; direct `hvs.py record` retains its existing full default. Existing `.vin.zst` sessions remain readable/exportable; raw sessions use `vin_raw_v1` with `.vin.bin` and the same per-frame index.

## Verification

- Release board recorder/player build succeeded.
- 12 recorder integration tests passed with zstd enabled. They cover both fast/verified compressed archive export and checksum corruption, raw byte preservation including padded RAW10, export equivalence, truncated raw rejection, stop/capacity handling, and save failure/retry.
- No-zstd build: 10 integration tests passed, 2 compression tests skipped; raw save/export remains available.
- 3 archive CLI routing tests passed, including raw playback and existing compressed sessions.
- Player contract covers compressed/raw display equivalence, seeking/rewind, raw metadata timestamp dump and existing nonblocking playback behavior. Test fake now supplies the width/height getters already required by the board player's original RAW10 path.
- `git diff --check` passed.

Earlier SD-backed synthetic tests timed out while a pre-existing large export saturated the SD card. The successful integration runs used `TMPDIR=/dev/shm` and ran serially because the fake recorder uses the same capture lock as the real recorder. Actual measurements above always wrote to `/app/recordings` on the SD card.
