# APS 控制与 ISP 审计 / 实机验收

2026-09-27 补充：X5 底层 SDK 有 VIN 输出帧率和 ISP 曝光接口，新增适配见 [X5 原生控制与内存录制](X5_CONTROLS_AND_RAM_RECORDING.md)。下文的限制针对预编译 Camera 公开接口，不能推广为芯片或底层 SDK 不支持。

本次修改仅在源码仓库；没有修改预编译 SDK、构建部署目录、样本目录，也没有提交 Git。
审计时工作区干净；仓库及检查的父目录没有 AGENTS.md。

## 关键结论与能力表

**现有公开 SDK 无法完成可靠的 APS 曝光控制。不能据 SetExposure 返回 true 声称设置成功。**
这不是已证明传感器不支持曝光，而是当前分发包缺少有效、可读回的控制接口。
可运行 `python hvs.py aps-capabilities`，机器可读结果见 `tools/aps_capabilities.json`。
该表来自仓库资料和指定哈希的预编译库静态审计，不是设备运行时能力查询。

| 参数 | 原有接口/实现 | 本次行为 | 仍缺少的信息 |
|---|---|---|---|
| APS 曝光 | Camera::SetExposure(int) 有声明，但三平台二进制只检查指针 | 明确拒绝 --aps-exposure-us；不调用空实现 | 有效 setter/getter、单位、范围、步进、时序 |
| APS 自动曝光 | X5 ISP 内部有 2A 初始化；无公开控制 | 硬件/软件 AE 均不开放 | 2A 模式和曝光帧反馈；VIN bypass 不经过 ISP |
| APS 采集帧率 | Set/GetFrameRate 文档针对 USB/Ethernet EVS；evs_fps 是 EVS 档位 | 拒绝 --aps-fps；独立报告回调接收、消费和预览速率 | APS setter/getter、曝光/帧长与链路约束 |
| APS 输出格式 | DeviceConfig 无像素格式 selector | 拒绝 --aps-format；严格校验收到的 Gray8/NV12 大小 | 原生 RAW8 选择、RAW10 原始输出、stride |
| 模拟/数字增益 | 无公开 API | 拒绝 --aps-gain | 范围、单位、读回、采集中设置规则 |
| 硬件 WB | X5 内部 hbn_isp_*awb* 符号，不属于 Camera API | 拒绝 --aps-hardware-wb，不叠加到正常 NV12 | 硬件增益/模式/实际值接口 |
| 软件 WB | 离线全图均值；C++ 头未接入播放器 | Bayer 链路支持 off/manual/once/continuous，实际增益与状态输出 | 无灰卡估计不能保证所有场景准确 |
| 原始保存 | hvs_record 可将 Gray8 不改值包入 NV12；旧 live 不录 Gray8 | live 同样保留 Bayer8 字节，双流独立尺寸，诊断可导出原字节 | 不能恢复 SDK 已丢弃的 RAW10 低两位 |

`SetExposure` 审计：X5 地址 0xbfb0、S100 0x93e0，均为 20 字节：
`ldr x0,[x0]; ldr x0,[x0]; cmp x0,#0; cset w0,ne; ret`。
x86_64 0x69b0 为 15 字节，等价检查对象内设备指针非空。三者都没有读取 value 参数或调用设备。
SHA256 列在能力 JSON 中；不要将本结论自动推广到厂商未来版本。

依据：`include/shimetapi/hv/camera.h`、`device_config.h`、`core/frame.h`、`API.md`、
`X5_VIN_BYPASS.md`、实时与录制源代码及上述 ELF。没有添加未公开的 SDK 函数、寄存器或二进制补丁。

## 路径与数值约定

- 设备采集/HAL/采集线程在预编译库内；本仓库没有该源码。公开 Frame 的池 owner 维持缓冲区生命周期。
- 正常 APS：RAW10 传输 → 硬件 ISP/PYM → packed NV12 → 标准 NV12 到 BGR 显示，不重复软件 WB/gamma。
- 现有实验 VIN bypass：RAW10 → SDK 降位 Gray8 → 应用显式声明 Bayer 排列 → 软件 ISP。
  RAW10 是传输格式描述，不足以确认 ADC 有效位深。Gray8 自身也没有 Bayer 元数据。
- ISP：原始副本 → float32 黑电平扣除/归一化 → Bayer WB → 按最大增益缩放到 uint16 去马赛克 →
  还原 float32 余量 → 单位 CCM（未标定）→ 显示处裁剪并作 1/gamma 编码 → BGR8。
  默认黑电平 0 是“未标定”，不是测量结果；没有任意压绿或增强饱和度。
- 只接受确切 packed 大小；没有 stride 字段时不能把多余字节猜作行 padding。
- Bayer-in-NV12 提取必须有录制来源证明。UV=128 是必要校验，不能独自证明图像是 Bayer。
  离线工具改为直接取原始 Y 字节，不通过可能改变 Y 数值范围的 ffmpeg gray 转码。
- C++ 软件 WB 使用 2×2 RGB 单元的对数比值中位数，排除暗/饱和单元，检查样本量、比例、亮度跨度和色度离散。
  强单色色度、样本少或估计不可靠时保持历史增益（初始为配置值/1）。有色表面和同色照明无法完全区分，因此保守放弃。
  增益在配置范围内，按 0.2 权重平滑；once 连续 5 个估计变化小于 1.5% 时收敛，最多 90 个新帧后转 manual。
  超时不等于成功；状态注明 timeout 并保留最后增益。m 从自动转手动保留当前增益；off 停用但保留历史值。
- 离线工具一次只处理一帧，gray_world/once/continuous 是稳健单帧估计；时间平滑与 once 状态机属于 C++ 连续处理链路。

## 使用

板端沿用原 CMake 配置构建：

```sh
cmake --build out/x5/hvs-build --target hv_sample_live_record_display hv_sample_player -j2
python3 hvs.py aps-capabilities
```

需已有 OpenCV 开发库和正确的 X5 SDK 运行环境。`hvs.py build --with-player` 不会自动构建 live 目标。
本次没有自动启用或更改实验 VIN bypass；若采用原有旁路流程，继续按 X5_VIN_BYPASS.md 核验库版本。

实时入口（仅在实际收到 Bayer8 的旁路环境下指定排列）：

```sh
out/x5/hvs-build/samples/cpp/live_record_display/hv_sample_live_record_display \
  --aps-bayer gbrg --aps-config tools/isp_params.json --aps-wb continuous
```

不要把 gbrg 当作已标定结论。先用 `--aps-bayer compare` 观察四色排列（比较时关闭自动 WB，避免掩盖相位错误），
对照具有已知颜色的物体确定相位，再指定一种排列。正常硬件 NV12 使用默认 `--aps-bayer none`。

- `w`：一次软件 WB；`c`：连续；`m`：固定当前增益；`o`：关闭；`s`：新目录导出诊断；`r`：录制；`q`：退出。
- `--no-display` 使用同样的小写命令并按回车。
- 手动 R/G/B：JSON 设置 `wb_mode: "manual"`、`wb_gains: [R,G,B]`；重新启动加载。它们是软件增益，不是传感器增益。
- 配置保留旧 black_level/gamma/wb_gains 等键；gray_world 迁移为 continuous。未知键/非法范围明确报错。
  C++ 不默默忽略非单位 CCM、brightness/saturation 的旧自定义值，而是要求使用离线工具。
- 日志区分 callback receive/s（SDK 回调）、consumed unique snapshots/s（应用处理）和 preview/s；设备采集 FPS 仍为 unknown。
  SDK 未公开可靠 APS 序号，丢帧计数为 unknown，不能拿两个速率相减冒充传感器丢帧。
- `s` 产生 `derived_bayer8.raw`、`display.png`、float32 中间阶段 `stages.yml`、含直方图/饱和比例/模式/增益的 `metadata.txt`。
  RAW 文件是 ISP 前的派生 Bayer8，不是原始 RAW10；所有未知硬件值明确标注，未冒充逐帧曝光信息。
- 离线回放入口已接通：

```sh
python3 hvs.py play --output SESSION --aps-bayer gbrg --aps-config tools/isp_params.json --aps-wb once
python3 tools/aps_isp_tuner.py extract --avi SESSION/aps.avi --frame 10 \
  --width 1632 --height 1224 --preserved-bayer --out frame10.raw
python3 tools/aps_isp_tuner.py process --input frame10.raw --width 1632 --height 1224 \
  --bayer gbrg --config tools/isp_params.json --out display.png --with-baseline
```

`--preserved-bayer` 前先核对 SESSION/summary.txt 的 gray8_frames == aps_frames。
离线提取检查 AVI 的 NV12 格式、尺寸、完整帧长度和中性 UV，拒绝覆盖已有 RAW 输出。

无灰卡时：先用多种颜色、亮暗适中的场景执行 w，避免对着纯色墙/屏幕；成功后 m 固定。
光源变化才用 c。不要用大软件增益弥补严重欠曝；增加照明、减少运动模糊需求更有帮助。
可遮光拍黑帧，用 stats --dark 估计当前 8 位尺度黑电平；不要用正常图像最暗像素当作黑电平标定。
当前无法通过此 SDK 真正增加曝光，必须等厂商提供有效控制。

## 稳定性与限制

没有新增运行中硬件设置，也没有改变 APS/EVS 共享设备的停流/重启时序。
软件参数只由消费线程修改；回调只短暂锁定计数和上一帧 owner，不等待 UI。
采集失败会报错；成功 Init 后任何异常出口均 StopStream/Destroy。
APS 10 秒无新数据退出；**不声称自动重连已实现**。重新连接后重启程序加载原配置，清空旧 owner/ISP 状态。
尺寸或收到的格式变化会重建 ISP 中间 Mat 并重置 WB；正在录制时停止并报错，避免把不同格式写入同一 AVI。
live 录制从首个 APS 开始，之前 EVS 不写入，并明确打印开始时点。使用独立几何写入 EVS 和 APS。
同步只透传 SDK aps_evs_ts，不再将当前 EVS 包时间戳伪造为 APS 精确配对。

## 验证结果与复现

2026-09-23，Windows/MSVC + 已有 OpenCV DLL：

```sh
cmake -S tests -B out/aps-tests -DAPS_OPENCV_ROOT=D:/Prophesee/third_party
cmake --build out/aps-tests --config Release
# Windows PATH 加入已有 OpenCV bin；Linux 不指定 APS_OPENCV_ROOT，使用 find_package
ctest --test-dir out/aps-tests -C Release --output-on-failure
python -m unittest discover -s tests -v
```

- C++：实际 ISP、完整 live 主程序链接 fake SDK、录制 fake writer、播放器 widgets 编译。
- CTest 4/4：数值/配置/导出，fake 采集尺寸变化，硬件请求拒绝，RAW 保存/双尺寸/时间戳/格式切换/写入失败。
- Python 20/20 通过，使用带 numpy/opencv 的环境；包含原 CLI/NPZ 回归和新增 ISP/控制拒绝测试。
- 所有编译产物在本仓库 out/aps-tests；没有链接或运行真实 X5 设备。
- 本机 WSL 的 VHDX 路径缺失，未完成 Linux/X5 生产目标链接或板端验证。fake 编译不替代实机验收。

## 实机验收清单

1. 运行能力查询；传入 --aps-exposure-us / --aps-ae / --aps-fps / --aps-format RAW8 应明确拒绝且不启流。
   当前不能验收真实曝光变化、AE 响应或 APS FPS 限制；待厂商有效 API 到位后，以读回和帧反馈重测，
   核对 exposure_us 与帧周期（1e6/FPS）和 readout/带宽约束，不能只观察 UI 数字。
2. 正常 NV12 图像不应用软件 WB；旁路 Gray8 尺寸/字节数应正确。native RAW8 切换仍未支持，不以旁路替代。
3. 多色场景测试 w/c/m/o，检查实际 RGB 增益和平滑；单色/遮光/过曝时保持增益，once 90 新帧超时明确显示。
4. 同时录制 APS/EVS，检查尺寸独立、同步 tsmp 来源正确、计数增长，回调接收与预览速率分开显示。
5. 拔线后 10 秒停止并关闭录像；重新接线后重启，确认配置加载、无旧帧。自动重连未提供。
6. 保存同一帧 RAW 和显示图，确认调整 WB 只改变显示/浮点阶段；Bayer8 原字节不变，Gray8-in-NV12 的 Y 完全一致。
7. 导出直方图、中间结果；用已知颜色物体验证 Bayer 和 R/B 顺序，再评估黑电平及 WB。无色卡不宣称颜色标定完成。

## 修改文件

- `samples/cpp/common/aps_core.h`：校验、稳健 AWB、状态机、能力限制。
- `samples/cpp/player/aps_color.h`：ISP、JSON 配置、格式校验、导出、共用 CLI。
- `samples/cpp/player/main.cpp`、`player_widgets.h/.cpp`、`player/CMakeLists.txt`：接通真实 Bayer/ISP 回放路径，补齐链接依赖。
- `samples/cpp/live_record_display/main.cpp`、`live_widgets.h/.cpp`：软件控制、接收/预览统计、owner 去重、保存与生命周期修正。
- `hvs.py`、`tools/aps_capabilities.json`：能力查询、拒绝无效硬件请求、转发回放设置。
- `tools/aps_isp_tuner.py`：稳健离线估计、精度与提取修正。
- `tests/CMakeLists.txt`、`test_aps.cpp`、`fake_sdk.cpp`、`test_record.cpp`、`test_aps_isp.py`：主机编译、fake 和数值回归。
- 本文及 API.md：记录真实限制、操作和验收。
