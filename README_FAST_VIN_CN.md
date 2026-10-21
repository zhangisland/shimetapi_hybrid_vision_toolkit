# X5：快速保存、按需回放和纯预览

## 板端执行 pipeline

源码已部署到本次测试板。板端执行（输出目录必须是新目录）：

```bash
cd /root/zhangsr/shimetapi_hybrid_vision_toolkit
python3 hvs.py build --with-vin-record --with-player \
  --platform-samples /app/multimedia_samples --sdk-root /usr/hobot \
  --sdk-include-dir /usr/include --jobs 2

# 只预览，不录制、不创建输出文件；q / Esc 退出
python3 hvs.py live --x5-vin-bypass -- \
  --aps-gain-db 0 --i2c-bus 6 --preview-width 960

# 5 秒实际采集；预热及停止后保存另计
python3 hvs.py record --x5-vin-bypass --storage memory --save-format fast \
  --output /app/recordings/fast_new --seconds 5 --max-mib 1024 \
  --aps-gain-db 0 --i2c-bus 6

# 直接打开原始数据，不自动导出 AVI，不预缓存整段 APS
python3 hvs.py play --output /app/recordings/fast_new \
  --window-width 960 --window-height 600

# 需要兼容文件时，恢复原有四文件到另一个新目录；保留源会话
python3 hvs.py export-recording --input /app/recordings/fast_new \
  --output /app/recordings/fast_new_export --max-mib 1024
python3 hvs.py play --output /app/recordings/fast_new_export \
  --window-width 960 --window-height 600
```

GUI 命令需在 X5 桌面终端或已正确配置 X11 转发的终端执行。本次 SSH 本地桌面验证使用 `DISPLAY=:0 XAUTHORITY=/var/run/lightdm/root/:0`，该授权路径是本板配置，不应套用所有镜像。

`raw` 不需要 zstd；`fast` 和 `zstd` 需要 zstd 开发头文件和库，缺少时在打开相机前报错。Windows 到板端的源码部署必须包括新的 `samples/cpp/common/{Zstd.cmake,vin_archive.h}` 和 `samples/cpp/hvs_record/vin_archive_save.h`，以及修改过的 Python、CMake、播放器和录制源码。只更新 hvs.py 不够。启动日志打印实际二进制路径和 SHA256。

原有四文件模式仍为默认，也可显式选择：

```bash
python3 hvs.py record --x5-vin-bypass --storage memory --save-format full \
  --output /app/recordings/full_new --seconds 5 --max-mib 1024 \
  --aps-gain-db 0 --i2c-bus 6
# 另一终端可提前停止；也可在录制终端 Ctrl+C
python3 hvs.py stop --output /app/recordings/full_new
```

## 数据保存与恢复

采集阶段：两路 VIN 独立取帧 → 每个 DMA 缓冲复制一次到有界进程内 arena → 立即归还上游缓冲。停止接收、线程结束之后，才开始保存；没有边采集边写盘线程。

保存模式：

| 模式 | 策略 | 输出 |
|---|---|---|
| `fast`（record.sh 默认） | Zstandard level -1，逐帧快速压缩，保留帧校验和；不计算 FNV、不整段回读解压；关闭文件并 fsync 后发布 | `.vin.zst` + 索引 + summary |
| `raw` | 原生示例的 DDR 缓存 + 4 MiB 顺序直写，不编码、不压缩、不逐字节哈希或回读；每路只写一份；fsync 后发布 | `.vin.bin` + 索引 + summary |
| `zstd` | 原来的 fast：level 1 + FNV + 完整回读、解压、逐字节比对 | `.vin.zst` + 索引 + summary |
| `full`（hvs.py 默认） | 原始数据 + 兼容 AVI/events + 完整校验 | 两份 `.vin.bin` + `.avi` + `.raw` + 索引 + summary |

参考代码：`/app/multimedia_samples/sample_apx003cc/sample_evs_release/evs_mode/01_evs_live_player/evs_live_player.cpp`。原生示例只保存原始 EVS 数据，4 MiB 顺序复制，关闭文件但不等待 fsync。本项目保存双路 APS+EVS，保留 RAW10、stride/padding、帧号和时间戳；不会将只进入页缓存的数据当作已持久化。

本板 15 秒双路原始数据约 2.10 GiB，实测 `raw` 保存并同步需要 **211.13 秒**。因此不将无压缩直写作为默认最快模式；SD 卡带宽较低时，低开销压缩减少写盘量更有效。实际速度取决于场景压缩率和存储设备，详细结果见 `HVS_QUICK_SAVE_VALIDATION.md`。

`fast` / `zstd` 为 `session_format=vin_zstd_v1`，旧压缩录制仍可回放和导出；两个 `.zst` 是 **HVZ1 容器**，内含逐帧 Zstandard 数据，不是普通 zstd 文件。`raw` 为 `vin_raw_v1`，输出 `aps.vin.bin`、`evs.vin.bin`。三种模式都必须保留 `vin.frames.jsonl` 和 `summary.txt`，都支持同一套 play / export-recording 命令。快速保存不在保存时完整验证介质内容：`fast` 在读取时验证 Zstandard 校验和，`raw` 不能检测同长度静默损坏；需保存后立即全面校验时使用 `zstd`。

```bash
# 原生无压缩直写：适合高速存储，不生成 AVI 或 events.raw
bash scripts/record.sh --save-format raw --seconds 15 --max-mib 2816 \
  --output /app/recordings/raw_new
# 保留原来的强校验压缩保存
bash scripts/record.sh --save-format zstd --output /app/recordings/verified_new
```

| 文件 | 保存内容 |
|---|---|
| aps.vin.bin / aps.vin.zst | 完整 APS VIN 缓冲；RAW10 放在 16-bit little-endian 容器内，含原始 stride/padding。1632×1224、stride3264 时每帧3995136字节 |
| evs.vin.bin / evs.vin.zst | 完整 EVS VIN 接收缓冲，包括布局与 padding；包数不是事件数 |
| events.raw | 去除 VIN 行 padding、添加 SDK 文件头的兼容事件数据 |
| aps.avi | RAW10 右移2位得到的8-bit Bayer样本放入 NV12 Y，UV为128；不是已标定的彩色 ISP 输出，也不能代替10-bit原始数据 |

三种原始保存模式均保留原始字节、host/VIN时间戳、帧序号、尺寸、stride和格式。只有 `zstd` 模式在保存后全部解压，与内存原始数据逐字节比较。导出也检查每块校验和，恢复完整 `.vin.bin` 和兼容文件。播放器 APS 按索引读取原始帧或解压所需帧，不先导出 AVI。EVS 仍需扫描解码建立回放序列，启动不是零等待。

保存用 `saving.partial` 暂存，summary 最后发布；只有 `status=complete` 才是成功会话。保存失败时进程保留唯一内存副本，修复空间或权限后，在输出目录创建 `retry.request` 重试。导出失败则源会话仍可重试到新目录。纯内存录制无法恢复断电、kill -9或进程崩溃前尚未保存的数据。

`--max-mib` 包含预留32MiB工作空间，其余给原始数据与元数据；压缩不会增加采集时可装的帧数。启动检查可用内存并留出系统余量；本板实测 mlock成功、swap关闭。达容量上限停止，不覆盖、不偷偷落盘。当前预算范围64～2048MiB。导出同样需要足够容纳解压后原始数据的预算。空间检查采用保守上界，压缩率不能事先保证。

## 显示性能与图像控制

默认 `--aps-preview-scale 4` 保留完整2×2 Bayer单元进行预览降采样，缩小处理结果再由界面缩放。减少显示细节并可能影响自动白平衡的采样统计；原始录制数据不变。没有修改黑电平、白平衡、gamma或颜色校正参数。全分辨率 gamma LUT 与旧浮点管线已有逐像素一致性测试。

需要完整空间分辨率处理可用：

```bash
python3 hvs.py play --output /app/recordings/fast_new \
  --aps-preview-scale 1 --precache-aps --window-width 960 --window-height 600
```

此选项会重新引入全段准备等待。旧未压缩 AVI 在本板 SD 卡冷读取时仍可能明显掉速；快速会话的近30FPS实测不能套用到旧AVI。需要完整旧AVI展示时可显式使用全段预缓存，代价是等待和缓存开销。默认按需缓存有界8个显示帧、额外1个完整 RAW 解压预取缓冲；显示跟不上时可以跳过预览帧，文件中的真实帧全部保留。窗口或远程X11带宽也影响显示速度，不把显示丢帧当作录制丢帧。

纯预览使用相同双 VIN 原始采集和增益控制，只留最新帧，允许覆盖未展示的预览；没有录制 arena 和文件写入。`evs_events_per_s` 只统计实际解码展示的包，是预览子集，不能当完整传感器事件率。

## 曝光边界

已阅读并遵守 [APX003CC_PRIOR_KNOWLEDGE.md](APX003CC_PRIOR_KNOWLEDGE.md)。正常命令不再写已经排除的0x01xx页；曝光行数/微秒参数在打开硬件前明确拒绝。没有已验证的曝光控制实现，不代表已经证明硬件不能运行时控制曝光。

保留已验收的模拟增益0～24dB、最近0.375dB量化与开流后 repeated-start 读回。0dB 是最低表项，不能解决所有室外过曝。当前场景不是室外强光，因此没有虚构室外饱和改善结果。后续曝光候选实验须先做亮场/高增益响应锚定，正常预览保持看门狗；受控外部I2C探测才使用既有 `--no-verify`，不能用它掩盖正常录制的读回失败。

实测与测试结果见 [2026-10-03 验证记录](BOARD_X5_VALIDATION_20261003.md)。
