# X5 APX003CC：双 VIN 内存录制与手动曝光

## 2026-10-02 X5 实测更正（优先于下方历史调查）

已通过 SSH 在 10.129.113.92 编译和测试。详细证据见 [板端实测记录](BOARD_X5_VALIDATION_20261002.md)。

- 旧录像 EVS 包首两字节被 VIN 的 frame-id 注入覆盖。播放器仅在同会话元数据证明该值等于 frame_id 时，在临时副本恢复头；新录制关闭该注入，原始文件不改写。
- 这批 AVI 的 SDK 时间戳 `valid=true` 但数值全零。零值不能作为同步依据，改用同会话 `vin.frames.jsonl`。
- 板上 EVS 解码时间单位与主机微秒之比约 0.800004；桥接同时拟合速率和偏移，不能仅对齐起点。此方式仍是接收时间近似同步，不是曝光时刻的硬件同步。
- 现有 ISP 每帧约 0.6 秒。颜色算法保持原样，Bayer 回放先按顺序准备全部帧，再启动播放时钟；148 帧实测 RAM 准备约 88 秒。缓存优先使用受限 RAM，不足时用自动删除的临时文件，界面显示准备进度。
- 显示画布缩放改为线性插值，EVS 使用掩码复制累积；最终板端回放展示全部 148 个 APS 帧，约 29.57 帧/秒，跳过预览 0。准备耗时 85.88 秒，最大合成显示耗时 29.16 ms。保存数据和 ISP 算法未改。
- 手动 **0、6、12 dB 增益**录制已成功；**曝光行数仍未通过硬件验收**：015B 写 100 后读回 0。不得把下面接口说明理解成曝光已经生效。
- 官方芯片探测发生在初始化前（0808）；开流后 3428 读到 0202，不可继续用开流后的值判定芯片不匹配。

板端已存在配置可使用：

```bash
cd /app/shimetapi_hybrid_vision_toolkit
cmake --build out/x5/hvs-build --parallel 2 --target hv_sample_player hv_hvs_record_vin
python3 hvs.py play --output /app/recordings/vin_01 --window-width 960 --window-height 600
```

窗口尺寸 960×600 适合本次板端 1024 宽桌面。远程 X11 的带宽和桌面尺寸另行影响显示速度。
颜色偏绿尚未解决；本次未修改 ISP 颜色算法，可使用工程现有 `tools/aps_isp_tuner.py` 离线调参，需已确认的 CFA 和标定数据，不能保证通用白平衡恢复正确颜色。

## 偏绿图像：使用已有离线工具，不继续改播放器颜色代码

这次没有修改 `aps_color.h`、`common/aps_core.h` 或默认 Bayer/WB 参数。现有软件预览
没有完成实际 CFA、白平衡和 CCM 标定，不能保证可靠色彩。可使用工程已有
`tools/aps_isp_tuner.py` 离线处理，支持黑电平、灰卡白平衡、去马赛克、CCM 和 gamma。
先确认 summary 中 `gray8_frames == aps_frames > 0`，再提取保留 Bayer 的 AVI Y 平面：

```bash
python3 tools/aps_isp_tuner.py extract --avi /app/recordings/vin_01/aps.avi --frame 30 --width 1632 --height 1224 --preserved-bayer --out frame30.raw
python3 tools/aps_isp_tuner.py dump-config --out isp_offline.json
# 编辑 JSON：对白纸/灰卡用 wb_mode="patch"，wb_patch=[x,y,w,h] 为实际灰卡区域；
# 黑电平来自遮光帧，CCM 来自色卡标定，不直接套用他人的数值。
python3 tools/aps_isp_tuner.py process --input frame30.raw --width 1632 --height 1224 --bayer gbrg --config isp_offline.json --out frame30_isp.png --with-baseline
```

`gbrg` 仍是现有预设，并非本次确认的 CFA；需用已知颜色场景核实。
该提取路径只有 RAW10>>2 后的 8-bit 精度，不能把完整 `aps.vin.bin` 直接当作这个工具的
8-bit 输入。保留原始 10-bit 文件用于后续标定；本次不增加或更改 ISP 算法。

本次改动直接接入 `hvs.py record --x5-vin-bypass`。该参数现在启动
`hv_hvs_record_vin`，不再启动旧的 patched Camera SDK。需要重新构建，不能只更新
`hvs.py`。旧路径可以用 `--legacy-sdk-bypass` 显式选择作对照。

## 已定位的事实与尚未完成的硬件验收

本节保留早期代码调查；最新 X5 实测见文首链接，不以早期未测说明覆盖实测结果。
板端现已通过 SSH 认证并原生编译；Windows 模拟测试与 X5 实测分开报告。

旧链路：`hvs.py` → `hv_hvs_record` → 版本锁定的 patched
`libshimetapi_hv.so.2` → `MipiHvsDeviceImpl::readImageFrame` →
`StreamSession::imgLoop/dispLoop` → `Recorder::consume` →
`DualStreamWriter` → tmpfs → 停止后的磁盘复制。

1. `recording_storage.h` 的确使用 tmpfs。不能把此前 14.28 FPS 归咎于持续写硬盘。
2. 旧回调持有一把应用互斥锁，执行 Gray8→NV12、AVI 写入和 JSONL 格式化；APS/EVS
   在这个回调中串行。旧 SDK 本身另有图像、事件与派发线程，不能说它们共用一个取帧线程。
3. 对 bundled SDK SHA256
   `6ae0ae6ba5335f8a2f09b546ab49ccade1e22474ddd5e161a9bdb5f78b147f4f`
   的只读反汇编显示：
   - `readImageFrame` 在 `0xff90` 调用 getframe，`0x101ec` 使缓存失效，
     RAW10 分支 `0x103b0/0x1042c` 对 16 位字右移 2 位后缩为 8 位，
     `0x102b0` 才归还帧。
   - `imgLoop` 在 `0xd15c` 再做池内存复制；`0xd16c` 传入 20 ms，
     `0xd170` 调用 `bridgeMatch`；保存最新 APS 供事件触发的派发路径使用。
     此路径不保证每个 VIN 帧都交付到录制回调。
   - `imgLoop` 的错误/池空分支可见 nanosleep；没有证据证明成功路径固定睡眠半个帧周期。
4. 官方 `hvs_aps_binning_evs_240fps_4lane*.c` 指定 profile=1、config_index=240、
   sensor_mode=2、名义 APS 30、VC1 RAW10 1632×1224、stride=3264；VC0 为
   RAW8 4096×256 传输包，事件坐标几何为 768×608。`240fps` 不是 APS 帧率。
   配置里的 framelenth=2582 与文档 VTS=3174 不一致，均不能直接当作已测行时序。
5. 当前可定位到的边界是上述转换、额外复制、配对等待、最新帧派发及回调封装。
   **旧 SDK 成功路径的单一根因仍未证明；原生双 VIN 已实测约 29.79 APS 帧/秒，上游传感器漏帧仍不可观测。**
   新原生路径绕开这些环节；若 receive 对照仍约 15，则调查 VIN/驱动/双 VC/传感器，
   不再改 AVI 帧率或凭初始化日志声称解决。

可复现静态审计（只读，不修改二进制）：

```bash
python3 -m pip install pyelftools capstone
python3 tools/apx003cc_diagnostics/disassemble_sdk.py lib/x5/libshimetapi_hv.so.2.0.0 > sdk_capture_disassembly.txt
```

## 数据流和容量

```text
             VC0 VIN getframe → cache invalidate → copy full plane → release
APX003CC →                                                        ↓
             VC1 VIN getframe → cache invalidate → copy full plane → release
                               bounded preallocated process arena
                                      ↓ stop + join + close camera
               RAW files + stride/frame-ID/host timestamps + compatibility AVI/events
                                      ↓ byte verify + fsync + rename
                                summary.txt completion marker
```

- 两个独立接收线程，各自最多持有一个 VIN lease。原生 C 适配器的 frame descriptor
  固定分配，每次取帧不再 calloc/free。数据复制完成后立即 release，不持有可复用裸指针。
- arena 一次预分配并触页，包含所有原始平面、帧元数据及 64 字节对齐开销。两路仅在
  atomic CAS 预留空间时共享计数，不持有跨流大锁，不设编码/磁盘后台线程。
- `--max-mib` 范围 64～2048，包含 32 MiB 封装、校验和临时工作空间；例如 1024 MiB
  分配 992 MiB arena。SDK/DMA、系统另外保留至少 256 MiB MemAvailable，不计为可录负载。
  这是启动余量，不是整个操作系统内存的硬配额，别的进程仍可能抢内存/OOM。
- 满容量停止，不覆盖，不退回边采边落盘。容量边界上已经收到但放不下的帧计入
  `*_capacity_rejected`；接收数与保存数因此可能不同，检查工具会明确指出。
- 启动读 `/proc/swaps`，尝试 mlock 整个 arena。存在 swap 且锁失败直接拒绝启动；
  没有 swap 时锁失败允许运行并记录 `mlocked=0`。没有修改 swap 或全局 sysctl。
  有 swap 时需由管理员给当前进程足够的 RLIMIT_MEMLOCK/CAP_IPC_LOCK，重启命令。
  mlock 不防 SIGKILL/OOM/断电，也不固定所有 SDK 内存；未锁时运行期间不要启用 swap。
- 小型会话标记、实际库映射及控制台日志可以在采集期写入；**录制负载、逐帧元数据、AVI
  全部只在停止后写入**。`--ram-dir` 在原生路径为旧 CLI 兼容参数，不再决定数据存储。
- `--warmup 1` 为丢弃预热数据的窗口，两路同时运行；`--seconds 5` 从该窗口结束计时。
  独立记录预热帧数、完整采集窗口、接收间隔速率、停止后的保存时间。最后的 100 ms VIN
  等待退出/归还过程不混入 5 秒统计；超出窗口的数据不被当作窗口内帧。

## 构建、部署与实际加载检查

在 X5 上完整更新本项目源码。平台样例应使用与板端 3.4.1 匹配的那一份；
`--platform-samples` 指向同时含 `vp_sensors` 和 `sample_apx003cc` 的目录。
不需要替换系统传感器驱动。原生 recorder 不要求 OpenCV；播放器需要 OpenCV 开发库。

```bash
cd /app/shimetapi_hybrid_vision_toolkit
python3 hvs.py build --with-vin-record --with-player --platform-samples /app/multimedia_samples --sdk-root /usr/hobot

# 若头文件分散，按构建错误提示重复追加 --sdk-include-dir DIR。
# 没有 OpenCV 时去掉 --with-player，仍能构建录制目标。
out/x5/hvs-build/samples/cpp/hvs_record/hv_hvs_record_vin --help
python3 tools/apx003cc_diagnostics/runtime_inventory.py \
  --binary out/x5/hvs-build/samples/cpp/hvs_record/hv_hvs_record_vin > runtime_before.json
```

CLI 打印实际二进制绝对路径、SHA256 和参数；原生会话还保存 `runtime.maps`。
采集期间用另一个终端检查实际 PID（hvs.py exec 后 PID 不变）：

```bash
pgrep -af hv_hvs_record_vin
# 将 12345 替换为上述 PID
python3 tools/apx003cc_diagnostics/runtime_inventory.py --pid 12345 \
  --binary out/x5/hvs-build/samples/cpp/hvs_record/hv_hvs_record_vin > runtime_during.json
```

确认存在正确的 libcam/vpf/hbmem/传感器库路径，原生 recorder 不应加载 patched
libshimetapi_hv。部署时保留新二进制、项目 `lib/x5`、匹配平台样例和 SDK。若跨编译，
用可用的 Linux aarch64 sysroot/SDK 加 `--cross`；本板已原生编译验证，交叉编译产物仍须独立核对。

## 录制、停止、失败重试、回放

先保证没有旧 Camera、ISP live、厂商样例等进程占用相机。原生进程使用 advisory flock
避免本程序的重复实例；无法强制协调不使用该锁的其他厂商程序。

```bash
python3 hvs.py record --x5-vin-bypass --storage memory --output /app/recordings/vin_01 --seconds 5 --warmup 1 --max-mib 1024

# 另一个终端，或原终端 Ctrl+C：
python3 hvs.py stop --output /app/recordings/vin_01 --wait 120

# 仅当终端报告 SAVE FAILED 后：释放输出所在磁盘空间/修复权限，然后
touch /app/recordings/vin_01/retry.request

python3 hvs.py play --output /app/recordings/vin_01
python3 hvs.py play --output /app/recordings/vin_01 --dump-timestamps
python3 hvs.py export-csv --input /app/recordings/vin_01/events.raw --output ./events_01.csv
python3 tools/apx003cc_diagnostics/check_vin_recording.py /app/recordings/vin_01
```

文件：`aps.vin.bin`/`evs.vin.bin` 保存各自每个完整 DMA 平面（包括 stride/padding），
`vin.frames.jsonl` 的 offset 分别相对于对应 `.vin.bin` 起点。保存字节数、几何、stride、
实际 buffer format、VIN frame_id、单调主机到达时间、FNV-1a64 校验值。原始字节在发布前
逐字节校验。EVS `events.raw` 去除行 padding，继续使用已有 event writer/header；
`aps.avi` 为 RAW10 16LE 右移 2 位的 NV12 预览，与已审计 SDK 算法一致，**原始 10 位不丢失**。
如果原始字超过 1023，记录 `raw10_words_outside_10bits` 并标记预览解释未验证。
检查工具不使用 AVI 帧率计算采集速率；如 RAW 对齐异常，不用预览做曝光定量分析。

AVI 仍根据真实主机间隔设置播放时长，不填充/重复/插帧。逐帧时间在 JSONL 中；原生路径
没有 SDK 的 APS↔EVS sensor timestamp 配对，旧播放器只能近似同步，不能声称精确同步。
实际 CFA 仍须核实；auto 播放沿用项目 gbrg 预览预设，也可传 `--aps-bayer none` 看灰度。

所有输出先写 `saving.partial`；flush/尺寸校验/原始字节校验/fsync 后发布，最后发布
`summary.txt`。失败时进程保留完整 arena，等待 `retry.request` 并重做保存。
此时第二次 Ctrl+C/SIGTERM 不释放唯一副本；恢复空间再重试。强制终止只能丢失未保存数据，
半成品绝不能当作完整会话。关闭硬件/曝光验证失败的会话也保存可用数据，但 summary 为 failed，
正常播放器拒绝将其当作成功录制。

## 外部曝光与增益

仅 profile=1/config_index=240/sensor_mode=2/地址=当前配置支持此手动控制。纯 VIN 不创建
ISP，因此没有本进程 ISP AE。设置在两个 vflow 初始化/开流之后，预热之前；之后每秒及
停止前读回检查。任何显式设置/读取/保持检查失败均报错，不假装生效。

本地历史驱动反汇编曾发现增益回调写 `0x0157`，不符合本次新文档，因此没有调用该错误
回调来冒充 dB 控制。采用非强占 Linux i2c-dev reg16/data8：I2C_SLAVE 做所有权检查，
I2C_RDWR repeated-start 读取，大端寄存器地址字节。需要用户提供经板端配置确认的总线；
不扫描，不用 `I2C_SLAVE_FORCE`/`i2ctransfer -f`。若设备被内核独占，当前实现明确失败，
需要厂商支持的手动接口或修正驱动，不能绕过占用。

启动先通过官方初始化前探测核对 0x3428 的 0808，再核对实际总线及 7 位地址。本板为 bus 6、0x3c。
以下与实测成功命令的参数相同，输出目录必须未存在：

```bash
python3 hvs.py record --x5-vin-bypass --storage memory --output /app/recordings/gain0_new --seconds 5 --max-mib 1024 --i2c-bus 6 --i2c-address 0x3c --aps-gain-db 0
python3 hvs.py play --output /app/recordings/gain0_new --window-width 960 --window-height 600
```

当前模式 `--aps-exposure-lines 100` 明确失败（015B 请求 64、读回 00），已移除原先错误地标为可用的明亮场景曝光命令。必须先由厂商确认本板模式的曝光寄存器及写入条件；不要绕过读回检查。

曝光写 `0x015A` 低 4 位（保留高位）及 `0x015B`，范围 1～1162 行；增益写
`0x3603=7`、`0x3660=1`、查表 `0x3602`、`0x3661=1`、`0x3662=0`；最后 latch
`0x342C=0→1→0`。只改曝光时不改变增益，只改增益时不改变曝光。

`--aps-gain-db` 是模拟增益 dB，0～24，按最近 0.375 dB 量化，恰好半步向上。
使用提供指南的 65 行模拟表，数字增益固定 1×；不补造“161 项”标题与实际 146 行之间
缺失的条目，也不使用有歧义的 109/80 数字增益索引。0、6、12 dB 已做板端读回与图像统计，其他档位尚未逐档实测。

`--aps-exposure-us` 与 `--aps-exposure-lines` 互斥。微秒请求必须同时提供
`--aps-line-time-us`，表示用户已用实际模式时钟/驱动或硬件测量验证的行时间；
round(us/line_us)，越界报错，不暗中截断。程序读回 VTS 记录，但不由 VTS×名义 30 FPS
自动推导。例：只有独立验证行时间确为 10.5 µs 时，才可将行数参数替换成
`--aps-exposure-us 1050 --aps-line-time-us 10.5`。当前板上行数写入也未验证通过，不能通过提供行时间绕过此限制。

summary 分别记录请求、量化后提交、寄存器快照/保持检查、VTS；直接寄存器路径的 SDK
配置读回写为 unavailable。均不关联具体帧，不能把寄存器配置读回叫作逐帧实际曝光。
检查工具比较 RAW10 均值、饱和比例与 EVS 接收情况；不自动将“ACK/配置变化”判为成像成功。

## 性能对照与归因

同一板、同一模式、无其他相机进程，依次执行，避免同时抢传感器。A/B/D 固定相同双 VC、
同样的缓存失效操作、预热及 5 秒窗口；A 的上限包含 cache invalidate，不能代表传感器裸输出。

```bash
# A: 取帧、缓存同步、统计后立即释放，不保存负载
python3 hvs.py record --x5-vin-bypass --storage memory --output /app/recordings/diag_A \
  --seconds 5 --warmup 1 --max-mib 1024 --diagnostic receive
# B: 复制进有界 arena，结束后丢弃，仅保存诊断统计
python3 hvs.py record --x5-vin-bypass --storage memory --output /app/recordings/diag_B \
  --seconds 5 --warmup 1 --max-mib 1024 --diagnostic copy
# C: 旧版 SDK + tmpfs（诊断对照，不满足新内存热路径要求）
python3 hvs.py record --legacy-sdk-bypass --storage memory --output /app/recordings/diag_C \
  --seconds 5 --max-mib 1024
# D: 新的完整两路录制
python3 hvs.py record --x5-vin-bypass --storage memory --output /app/recordings/diag_D \
  --seconds 5 --warmup 1 --max-mib 1024
python3 tools/apx003cc_diagnostics/check_vin_recording.py \
  /app/recordings/diag_A /app/recordings/diag_B /app/recordings/diag_D
```

C 的原实现没有独立预热窗口，不能直接把总帧数与 A/B/D 作同窗口验收；先核实其日志
模式相同，再用 JSONL 中稳定段的接收间隔作辅助比较。正式 5 秒验收使用 A/B/D。

汇总包含 VIN get 等待、cache invalidate、复制+atomic 预留、release、最大持有/接收间隔、
字节率、ID gaps/duplicates/resets、容量拒收、arena 峰值、采集/保存耗时和吞吐。新路径没有
SDK 帧回调/配对队列/热路径编码；`queue_depth=0` 是指应用队列，不是 VIN 内部队列。
库内、驱动内、MIPI 之前的不可观测丢失为 unknown；EVS packets 不是事件数量。

- A/B/D 稳定约 30，而 C 约 15：瓶颈位于被绕开的 SDK/应用链路；进一步按阶段耗时归因。
- A 约 30、B 明显降低：检查复制带宽、cache、CPU、内存压力、VIN buffer 等待/ID gaps。
- A 也约 15：检查同模式厂商双 VIN 样例、VIN 计数、驱动 frame-length、MIPI 时间戳/硬件。
  本补丁不擅自修改不明含义的 VTS/PLL 寄存器。名义 fps=30 不是验收依据。
- 检查工具默认窗口 APS≥27 FPS 且没有已知 ID/容量丢失作为筛查；仍需稳定多次 5 秒接近
  150 帧、VIN ID 连续、EVS 正常，必要时用变化场景证实真实独立帧。

## 本地测试及待验证项

Windows VS2022 编译原生录制 C++ 逻辑 + 假 VIN/IO；加入播放器修复后，20 项 CTest 和 51 项 Python 测试通过。
覆盖参数透传、互斥/范围、曝光换算、65 项表端点/量化、寄存器/latch/写失败/覆盖检测、
并发 arena 边界、DMA 归还后字节不变、padding 保留、满容量保存、stop.request、SIGINT handler、
首次封装失败/不可写保存路径保留内存并重试、诊断不假装完整会话、错误布局归还缓冲。模拟速率不是硬件速率。

```powershell
cmake --build out/aps-tests --config Release --parallel 2
ctest --test-dir out/aps-tests -C Release --output-on-failure
$env:HVS_VIN_FAKE=(Resolve-Path out/aps-tests/Release/vin_record_fake.exe).Path
python -m unittest discover -s tests -p 'test_*.py'
```

待板端完成：匹配 SDK 编译链接；实际库/传感器模式确认；A/B/D 帧率与 EVS 流有效性；
RAW 对齐/CFA、相机 frame_id 语义；I2C 非强占访问是否可用、chip ID/行时序、曝光亮度响应及
无外部写入者覆盖；真实磁盘 ENOSPC/拔盘/权限故障和 retry；mlock/swap 权限与系统压力。
本地已测试内存余量判定和容量边界，但未在 X5 制造系统 OOM 或真实磁盘满。
