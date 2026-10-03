# 双模态APS+EVS相机采集原型+系统+脚本


**语言**: **中文** | [English](README_EN.md)

## 我们用的精简版的README: [多命令版README](README_HVS_CN.md)

# 01 HV Toolkit


HV Toolkit（ShiMetaPi Hybrid Vision Toolkit）**v2.0** 是面向事件相机（DVS/EVS）的高性能 C++17 SDK，统一采集事件数据流（EVS）与图像数据流（APS），自带零第三方依赖的 EVT2/EVT3 编解码。v2.0 完成核心重写：**USB / MIPI / Ethernet 三后端统一为同一套 `Camera` API**，公有 API 表面不再依赖任何第三方事件 SDK。

> 本仓库为**预编译二进制发布版**（核心以 `.so` 闭源交付，仅公开头文件与示例源码）。
> `lib/x86_64/` 为 x86_64 Linux 库（USB + Ethernet 后端），`lib/s100/` 与 `lib/x5/` 为 aarch64 库（MIPI + Ethernet 后端）。


## 📄 版权声明

版权所有 © ShiMetaPi。本仓库以预编译二进制形式分发 HV Toolkit 运行库；头文件与示例代码供集成开发使用。未经书面许可，不得反向工程、反汇编库文件或再分发其中的二进制组件。EVT2/EVT3 编解码为基于公开规范的独立实现（clean-room）。


## 📋 技术规格

### 事件相机参数

- **EVS 分辨率**：768×608
- **APS 分辨率**：1632x1224
- **数据传输**：USB 3.0（USB 后端）/ MIPI-CSI（MIPI 后端）/ TCP（Ethernet 后端）
- **事件格式**：EVT2 / EVT3（兼容 Prophesee EventCD 语义）


## 🚀 快速开始

### 系统要求

- **C++ 标准**：C++17 或更高
- **CMake**：3.16+
- **操作系统**：Linux —— aarch64/S100 / X5（MIPI + Ethernet 后端）

### 构建

预编译库随仓库分发，无需编译 SDK 本体；构建只编译示例（链接 `lib/<arch>` 的库），CMake 按目标架构自动选择。

#### X5（ARM MIPI，交叉编译）

X5 平台预编译库在 `lib/x5/`（aarch64）。前置：

```bash
# 1) aarch64 交叉工具链（同 S100 步骤，apt 装 g++-aarch64-linux-gnu 或 Arm GNU 11.3）
sudo apt-get install g++-aarch64-linux-gnu            # Ubuntu/Debian 系统编译器

# 2) X5 SDK 源码树（hobot-spdev/hobot-multimedia/hobot-multimedia-samples/hobot-camera）
#    默认期望 ../x5_sdk/RDK_X5（相对 toolkit 根目录），也可设 X5_SDK_ROOT 指向其他位置
git clone <X5_SDK_REPO> /path/to/RDK_X5
export X5_SDK_ROOT=/path/to/RDK_X5
```

构建：

```bash
./run.sh build x5      # aarch64 交叉；工具链与 SDK 已就绪即可
```

> `./run.sh` 自动探测 `../x5_sdk/RDK_X5`；缺失 X5_SDK_ROOT 时显式导出 `X5_SDK_ROOT=<path>`。
> 也可用 `-DCMAKE_TOOLCHAIN_FILE=<你的-toolchain.cmake>` 覆盖工具链。

产物布局：

```bash
ls out/x5/build/libshimetapi_*.so                                                # 预编译 4 个库已捆绑
file out/x5/build/samples/cpp/get_started/hv_sample_get_started                  # 应为 ELF aarch64
# OpenCV 类样例（player / live_record_display）用 third_party/ 自带 aarch64 OpenCV，7/7 全编
```

#### 查看支持的架构

```bash
./run.sh --list
# ARCH    STATUS        PREBUILT LIBS
# x86_64   ready         .../lib/x86_64
# s100     ready         .../lib/s100
# x5       ready         .../lib/x5
```

### 运行示例程序

构建产物在 `out/<arch>/build/samples/cpp/<name>/hv_sample_<name>`（7 个）。
采集类样例（get_started / callback / record / viewer）默认 USB 后端，
支持 `--mipi`（MIPI EVS-only）/ `--mipi-hvs`（MIPI 双 VC，S100 板上用，get_started暂不支持x5上的双模态录制）切换；


#### x5 HVS）

部署到板卡（`out/x5/build` 是**自包含**的——构建时已把 `lib/x5` 的
`libshimetapi_*.so` 捆绑进去，样例 rpath 用 `$ORIGIN` 相对路径，整个目录拷上板即可）：

如果后续运行代码提示缺libxxx, 可能需要的:
```bash
export LD_LIBRARY_PATH=/app/shimetapi_hybrid_vision_toolkit/lib/x5${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
```

```bash
# 宿主机
rsync -avzuP out/x5/build root@<板卡IP>:/app/
```

> 板卡上**不需要**交叉工具链等构建环境 —— 那些只在编译主机上用。
> OpenCV 样例（player / live_record_display）还需把 `third_party/aarch64_opencv/lib/aarch64-linux-gnu`
> 拷到板上某目录并 `export LD_LIBRARY_PATH` 指向它。

> 我们自己实测后发现以下示例在当前镜像(`rdk-x5-ubuntu22-preinstalled-desktop-3.4.1-arm64.img`)下无法正常实现 (x5: --mipi-hvs)

```bash
# get_started — MIPI-HVS 最小采集
/app/build/samples/cpp/get_started/hv_sample_get_started --mipi-hvs

# record — MIPI-HVS 混合录制
/app/build/samples/cpp/record/hv_sample_record --mipi-hvs

# live_record_display — 实时预览 + 按键录制
/app/build/samples/cpp/live_record_display/hv_sample_live_record_display
# 按 r 开始/停止录制，按 q 退出；--no-display 无屏模式，--evs-prefix/--aps-prefix 指定录制前缀

# player — 离线回放录制文件
/app/build/samples/cpp/player/hv_sample_player events.raw aps.avi
```


## 📁 项目结构

```
shimetapi_Hybrid_vision_toolkit/
├── CMakeLists.txt              # 预编译库接入配置（IMPORTED 目标 + 示例 + 安装规则）
├── run.sh                      # 一站式入口（build/install/samples/pydeploy/--list）
├── README.md / README_EN.md    # 项目文档（中/英）
├── API.md / API_EN.md          # 公有 API 参考（中/英）
├── include/shimetapi/          # 公有头文件
│   ├── core/                   # EventCD / Status / BufferPool / Frame / PixelFormat / Timestamp
│   ├── hv/                     # Camera / DeviceConfig / EventFormat / EventPacket / ImageData
│   ├── codec/                  # EVT2 / EVT3 / MIPI RAW8 编解码
│   └── io/                     # EventReader / EventWriter / HybridWriter / HybridReader
├── lib/                        # 预编译库（闭源二进制）
│   ├── x86_64/                 # x86_64：USB + Ethernet 后端（含 python/ 绑定模块）
│   └── s100/ | x5/             # S100/X5 aarch64：MIPI + Ethernet 后端
├── toolchains/                 # 交叉工具链文件（aarch64-linux-gnu）
├── third_party/                # aarch64 OpenCV（交叉编 OpenCV 类示例用）
├── samples/                    # 示例
│   ├── cpp/                    # C++ 示例（7 个）
│   └── python/                 # Python 示例
└── docs/                       # 板端验证步骤与冒烟记录
```

## 🔍 示例程序说明

| 样例 | 用途 | 后端 | 命令速查 |
|---|---|---|---|
| `get_started` | 最小采集（同步 GetFrame） | USB / `--mipi` | `hv_sample_get_started [vid pid] [--mipi]` |
| `callback` | 事件 + APS 异步回调 | USB / `--mipi` / `--mipi-hvs` | `hv_sample_callback [--mipi-hvs]` |
| `record` | EVS+APS 混合录制到 /tmp | USB / `--mipi` / `--mipi-hvs` | `hv_sample_record [--mipi-hvs]` |
| `viewer` | 实时采集 + 解码计数 | USB / `--mipi` / `--mipi-hvs` | `hv_sample_viewer [--mipi]` |
| `bench_hw` | USB 实机吞吐基准 | USB | `hv_sample_bench_hw [vid pid duration_s]` |
| `live_record_display` | MIPI-HVS 实时预览 + 录制（OpenCV） | MipiHvs | `hv_sample_live_record_display [--no-display] [--evs-prefix s] [--aps-prefix s]` |
| `player` | 离线回放 .raw + .avi（OpenCV） | 离线 | `hv_sample_player <events.raw> <video.avi> [fps] [speed]` |

详细说明：

- **get_started**：基础入门，Init → StartStream → GetFrame 10 帧，打印每帧 evs 字节数。学习 HV Toolkit 的最佳起点。
- **callback**：`SetEventCallback` / `SetImageCallback` 双异步回调演示，采集 2 秒后打印计数。
- **record**：`HybridWriter` 把 10 帧写入 `/tmp/hv_record.raw`（EVS）+ `/tmp/hv_record.avi`（APS）。
- **viewer**：拉流并按后端自动选解码器（USB=EVT2，MIPI=MipiRaw8），打印累计解码事件数。
- **bench_hw**：USB 实机计时基准（默认 `0x1d6b:0x0105`，5 秒），输出 Mev/s 与 APS fps。
- **live_record_display**：MIPI-HVS 双 VC 实时预览（左 EVS 可视化 / 右 APS）+ `r` 键录制，`HybridWriter` 落盘。
- **player**：`HybridReader` + `MipiRaw8Decoder` 回放录制文件，带 GUI 按钮（播放/暂停/步进/变速/同步）。

---

## 🙋 联系我们

如果你在使用 HV Toolkit 过程中遇到任何问题或有任何建议，欢迎通过以下方式与我们联系：

开源硬件网站：https://www.shimetapi.cn （国内） / https://www.shimetapi.com （海外）
在线技术文档：https://forum.shimetapi.cn/wiki/zh/
在线技术社区：https://forum.shimetapi.cn

**HV Toolkit** - 让事件相机开发更简单 🚀
