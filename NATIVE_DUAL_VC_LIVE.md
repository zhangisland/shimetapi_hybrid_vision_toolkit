# X5 双 VC / APS ISP 实时展示

本次接入的是现有 `hv_sample_live_record_display` 可执行程序的原生构建路径，启动入口为 `hvs.py live`。旧 `hvs.py record` 和 VIN bypass 仍保留，旧录制器的平均接收 FPS、逐帧接收时间和回调耗时修复未改动。

详细控制链路审计及此次修补边界见 [APX003CC_EXPOSURE_AUDIT.md](APX003CC_EXPOSURE_AUDIT.md)。

## 源码依据与边界

依据本地 `multimedia_samples/sample_apx003cc/sample_hvs/dual_vc_vin_src/dual_vc_vin.c`、其 `hvs_config.{h,c}`、Makefile 和 `vp_sensors/apx003cc/hvs_aps_binning_evs_240fps_4lane*.c` 实施。读取了参考树中 `utils/common_utils.h` 以及 ISP 图像结构使用示例。指定目录及工程祖先未发现适用 AGENTS.md；修改开始时 git 状态干净。

官方路径：VC0 RAW8 VIN；VC1 RAW10 VIN → 离线 ISP（input_mode=2、flyby=0、端口 0 绑定）→ NV12。默认 profile 1 的配置为 EVS 4096×256 字节包、APS 1632×1224；程序使用运行时图像尺寸与 stride，并拒绝非 NV12 APS。共享 MIPI 仅调用一次探测，然后共享 RX 与 CSI 配置，按官方顺序为两个 VC 各建一个 camera 对象；不会再调用预编译 Camera.Init。两路各自启动 vflow，APS 启动后配置手动 AE/AWB。退出先结束采集线程，再逆序清理，包括初始化中途失败的句柄。

2026-09-29 已读取用户提供的 `RDK_X5_usr_include/usr/include/hbn_isp_api.h` 等头文件；尚未完成原生 C 适配器的 X5 编译/链接验证。WSL Ubuntu 虚拟磁盘 `G:\WSL\ext4.vhdx` 缺失，交叉编译不可用。未确认可用开发板连接，未启动或停止板端进程、未覆盖板端安装。

## 数据与线程

- VC0 VIN → 缓存失效 → 按 stride 拷贝到自有内存 → 释放 SDK 帧 → 有界 EVS 队列。
- VC1 VIN → ISP → 校验 NV12 双平面 → 按 stride 打包自有 NV12 → 释放 SDK 帧 → 有界 APS 队列。
- 以 APS 接收时间为基准，选最近 EVS 包；显示线程用原有 MipiRaw8Decoder 解码该包，并将包内事件累积成 EVS 图，与校正后的 APS 并排。
- 采集线程只做缓存失效、拷贝、计数与短临界区入队；不做颜色处理、UI 或写盘。显示缓冲 EVS 16 包、APS 3 帧，超量淘汰最旧帧；录制队列 64 MiB，超量丢新帧并计数，停止后后台排空。

这里是**主机接收时间近似配对，不是硬件同步**。官方 VIN 配置 `time_stamp_en=0`，没有证据证明 ISP 时间与 EVS sensor 时间同源，所以不混用。接收时间来自同一个 `steady_clock`，采样点在 getframe 和缓存失效返回后、拷贝前，含线程调度和 ISP/SDK 延迟。EVS 一个包含多个逻辑帧，展示是该包的事件累积，未人为给子帧插值主机时间。匹配差值不能当成曝光时刻误差。

默认容差 25 ms、最长等待 40 ms，均可配置（应用允许 0–1000 ms）；超差/缺 EVS 显示黑色 EVS 面板及 UNMATCHED，APS 超过 500 ms 标 STALE。一路断流不会阻塞另一采集线程或按键处理。日志每秒输出两路实收速率（EVS 分包速率和逻辑帧速率）、匹配差值、未匹配和队列淘汰数、帧龄、录制丢帧、采集/回调/校正/解码/显示/录制耗时。设备实际丢帧无法仅凭这些队列计数推断。

## 曝光和颜色职责

`--aps-exposure-us` 单位微秒，转换为 SDK `manual_attr.exp_time` 的秒；`--aps-gain`、`--aps-dgain` 原样转换为 SDK 的模拟/数字增益 float 字段。所查头文件及官方 API 文档没有明确其倍率/dB/编码语义，之前写成“倍数”缺少依据；不能据此认定 1 或 0 为传感器最小增益。默认请求 5100 us / 1 / 1，用于对照用户日志；不是已测量值。实际调用 get → 修改 manual 字段 → set，检查返回码，随后独立 get 读回并标为属性读回；回读失败或非有限数据会中止初始化。没有逐帧曝光反馈，录像实际曝光字段为 unknown。参数仅做有限正数和浮点可表示性检查，传感器范围及量化交给匹配 SDK，未臆造硬件范围。API 接受不等于曝光已经在传感器生效。

当前入口只实现初始化设置，改变曝光需正常退出后重启。`--aps-ae auto` 明确拒绝：官方示例说明该 HVS sensor_mode=2 的 HDR AE 路径不收敛；未把存在 AUTO 枚举作为可用证据。ISP AWB 锁定四通道中性增益 1，检查失败并退出。

工程及参考构建树中没有找到名为 `hvs_color` 的源码；已复用现有 `samples/cpp/player/aps_color.h` 和 `samples/cpp/common/aps_core.h` 的 RGB 增益、保守白平衡估计器和状态机，新增的是 NV12 输出接入分支。ISP 输出先转 BGR8，再作残余白平衡；**不再次黑电平扣除、去马赛克、CCM 或 gamma**。黑电平/去马赛克/CCM/gamma 由硬件 ISP 及其 tuning 配置负责，应用的 RAW gamma/black 参数不应用于此分支。残余 WB 在编码后的 BGR 上工作，是预览色偏修正，不宣称是传感器线性色度标定。

`--aps-correction off` 或按 k 开关校正；`--aps-wb off|manual|once|continuous` 沿用已有参数，w/c/m/o 按键沿用已有 WB 控制。`--aps-config JSON` 复用已有配置加载器（非单位 CCM、亮度/饱和度艺术参数仍按原行为拒绝）。

## 录制与时间轴

r 开始/停止，或 `--record` 启动即录。`--output` 是新会话目录前缀，自动追加单调时间编号，不覆盖现有目录。文件为 `events.raw`（原始 EVS 包）、`aps.avi`（未施加软件 WB 的 ISP NV12）、`aps.frames.csv`（逐帧主机接收时间、写入耗时，实际曝光 unknown）、`summary.txt`。本路径不把 ISP NV12 冒充 APS RAW10，也不替换旧的派生 Bayer8 保存能力。校正只影响预览，录像保存未校正 ISP 输出。

AVI 是恒定帧率容器：收尾以 `(写入帧数-1)/(最后接收时间-首次接收时间)` 修正帧率，沿用既有 RIFF 时间字段修复器。总播放时长等于接收跨度加一个平均帧周期；不规则间隔和丢帧被均匀时间轴摊开，原间隔保存在 CSV，不宣称 VFR。不足两帧无法估率，明确标为未验证。单会话有效载荷上限 1 GiB，避免 RIFF 超限，停止入队后排空收尾。`summary.txt` 提供旧播放器需要的状态/尺寸字段；不写虚构的 EVS sensor 配对时间。

## 板端构建与验证命令

在已有工程目录构建，不安装到系统、不替换驱动。先只读确认相机没有被其他程序占用；发现未知相机进程时不要终止它，也不要同时启动本程序。

```bash
ps -eo pid,comm,args | grep -E 'dual_vc|hv_sample|hv_hvs|sunrise|camera'
cd /app/shimetapi_hybrid_vision_toolkit
python3 hvs.py build --with-native-live --with-player --platform-samples /app/multimedia_samples --sdk-root /usr/hobot --sdk-include-dir /usr/include
python3 hvs.py live -- --help

# 桌面/X 转发显示；按 k 对比校正、r 开停录制、q 退出。
python3 hvs.py live -- --profile 1 --aps-exposure-us 5100 --aps-gain 1 --aps-dgain 1 --sync-tolerance-ms 25 --sync-wait-ms 40

# 无显示，自动结束，记录实际吞吐和耗时。选择存在且可写的父目录。
# 主要修改的参数是 --seconds 和 --output 
python3 hvs.py live -- --profile 1 --no-display --seconds 5 --record --output /app/recordings/hvs_native_1000 --aps-exposure-us 1000 --aps-wb off

# 固定场景和照明，在前次正常退出后改变曝光；比较属性读回和实际图像亮度。
python3 hvs.py live -- --profile 1 --no-display --seconds 10 --record --output /tmp/hvs_native_2500 --aps-exposure-us 2500 --aps-wb off

# 使用日志给出的实际会话目录替换 SESSION，SESSION是在以上--output指定目录后会添加时间戳后缀
python3 tools/check_native_recording.py SESSION
ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames,r_frame_rate,duration -of json SESSION/aps.avi
python3 hvs.py play --aps-bayer none --output SESSION 

```

原生 SDK 安装布局不同可传 `--sdk-root`。CMake 在缺少匹配 SDK/官方配置源码时明确失败，不能拿模拟头文件替代硬件编译。性能与官方示例比较时使用相同 profile、显示开关、曝光及分辨率；分别测无录制、录制、校正开/关。不承诺用户日志中的 APS 29.8 / EVS 208 FPS。

## 主机验证记录

Windows MSVC + 本地 OpenCV 的 C++ 构建成功，13 项 C++ 测试全部通过；32 项 Python 测试全部通过。C++ 模拟测试验证：近邻/容差/等待/缺帧/有界队列、padded NV12 拷贝与释放后所有权、独立 EVS 断流、RAW 与 NV12 校正隔离、录像错误传播，以及合成 55 帧 / 13.75 FPS 的 AVI 头时间轴为 4 秒。合成 RIFF 夹具仅验证时间字段，不是实拍录像或完整解码验证。Python 回归覆盖原有录制/导出/回放和新 CLI。

未验证：真实 SDK 编译链接、板端节点生命周期、曝光对图像的实际作用、硬件 ISP tuning 颜色效果、实测吞吐、真实 AVI 播放。需要上述板端命令完成验收。


## SDK 头文件位置排查

构建不再要求 `hbn_isp_api.h` 必须位于 `/usr/hobot/include` 顶层，会逐个搜索 `include/HAL`、官方样例 include、CMake 系统/sysroot 路径，并输出实际选中目录。`GLES3` 与 `vulkan` 是图形 API，不能替代 ISP 头文件。

```bash
find /usr/hobot /usr/include /app/multimedia_samples -type f -name hbn_isp_api.h 2>/dev/null
```

更新 `hvs.py`、`samples/cpp/live_record_display/CMakeLists.txt` 和新增的 `FindX5Sdk.cmake` 后重跑原构建命令即可。非标准布局可加 `--sdk-include-dir /实际/头文件目录`，多目录重复该选项。若搜索无结果，只能确认这些位置没有该头文件；已有官方二进制能运行并不保证开发头文件已经安装。需取得与板端运行库匹配的 SDK 开发头文件，不能通过删掉检查或复制不匹配版本来假装构建成功。
