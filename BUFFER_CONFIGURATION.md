# Native VIN recording RAM budget

`bash scripts/record.sh --max-mib N` configures the total process recording RAM budget in MiB. Default: 1024. Native VIN accepts budgets >=64 MiB that fit current Linux `MemAvailable` minus a 256 MiB system/SDK reserve; there is no fixed 2048 MiB ceiling. 32 MiB of the budget is reserved as workspace; the remainder stores original VIN payloads plus frame headers/alignment. This check runs before buffer allocation and camera startup. Allocation can still fail if other processes consume memory concurrently.

Example (20 seconds, about 150 MB/s combined APS/EVS on this board):

```bash
cd /root/HVS_Imaging/shimetapi_hybrid_vision_toolkit
bash scripts/record.sh --seconds 20 --max-mib 3328 --save-format fast --output /app/recordings/record20_3328_new
```

Use a new output directory for every recording. 3328 MiB requires at least 3584 MiB `MemAvailable` at startup. Check `free -m`; total RAM is not the same as currently available RAM. The original legacy SDK recorder retains its 2048 MiB limit for AVI compatibility.

The recorder stops at the first of requested duration, full buffer, stop request, or stream error. Compression and disk saving happen after capture. Inspect `summary.txt`: `reason=duration` and `capture_seconds=20` confirm the requested capture window; `reason=capacity` means the buffer filled early, even if `status=complete` indicates the captured data was saved successfully. Increasing RAM does not increase the requested duration automatically.

## Board validation (2026-10-19)

Rebuilt hv_hvs_record_vin on the board. C++ VIN contract test, Python syntax, shell syntax, git diff whitespace checks, and 7 existing Python CLI tests passed.
Actual `scripts/record.sh --seconds 20 --max-mib 3328` completed with `reason=duration`, `capture_seconds=20`, 596 APS frames and 596 EVS packets. Buffer peak: 3006204928 bytes. Archive original payload: 3006052352 bytes. Lossless compression, disk synchronization and full decompression/byte comparison completed in 108.355 seconds after capture.
Board recording: `tmp_recordings/audit_20261019/record20_3328` under the project directory.
Original source backup: `/tmp/hvs_buffer_backup_20261019` (temporary; not guaranteed across reboot). Changes are uncommitted. The user's existing `.gitignore` change was preserved.
