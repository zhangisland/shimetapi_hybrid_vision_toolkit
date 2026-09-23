# 以下内容仅针对 HVS APX 003CE + RDK X5 3.4.1

## HVS相关的源代码编译
```bash
cd /app/shimetapi_hybrid_vision_toolkit
python3 hvs.py build --with-player
```

## 数据采集流程
```bash
# 1. 录制 record
cd /app/shimetapi_hybrid_vision_toolkit
python3 hvs.py record --x5-vin-bypass --evs-width 768 --evs-height 608 --aps-width 1632 --aps-height 1224 --output /app/recordings/test --seconds 2 --timeout 15 --max-mib 2048
## 或者是用script
bash scripts/record.sh --output /app/recordings/test --seconds 2

# 2. 回放
## --aps-bayer compare 可以换成任一排列，包括 ['rggb', 'bggr', 'grbg', 'gbrg']，其中只有`gbrg`是正确排列
python3 hvs.py play --output /app/recordings/test --aps-bayer gbrg  # 
## 或者是用script
bash scripts/play.sh --output /app/recordings/test 

```


## 采集后的数据处理
```bash
# 1. event raw 转 csv
## 会显示进度条
python3 hvs.py export-csv  --input /app/recordings/test_vin03/events.raw   --output /app/recordings/test_vin03/events.csv

# 2. event raw 转 npz (为了减少文件大小)
python3 hvs.py export-npz --input /app/recordings/test_vin03/events.raw --output /app/recordings/test_vin03/events_vin03.npz
## 一个原本 149M 的events.raw，转为csv文件大小是 199M, 而转为npz后文件大小 18M，文件大小减少 90%

```



## ISP流程Debug
尝试对原始采集的RAW（NV12, rawvideo, 1632x1224, 420采样）进行离线ISP调参，找到适合当前HVS芯片的ISP处理流程

正常流程是:
1. 采集到RAW (目前是NV12, rawvideo, 1632x1224, 420采样) avi 视频
2. [extract] 从录像提取一帧 Bayer（Y 平面）：优先 ffmpeg，无 ffmpeg 自动用纯 Python AVI 解析
   1. `python aps_isp_tuner.py extract --width 1632 --height 1224 --avi aps.avi --frame 100 --out aps.raw`  # 从aps.avi中抽取第100帧raw
3. [dump-config] 导出默认参数 JSON，用于后续ISP调参
   1. `python aps_isp_tuner.py dump-config --out isp_params.json`
4. [process] 用完整ISP链路处理第2步提前的raw帧, 得到可视的RGB彩色图像(.png), 常规bayer像素阵列包括['rggb', 'bggr', 'grbg', 'gbrg'], 其中海康工业相机采用的是rggb，而当前HVS的输出是gbrg
   1. `python aps_isp_tuner.py process --config isp_params.json --input aps.raw --width 1632 --height 1224 --bayer gbrg --out out.png`  
5. 其他
   1. [stats] 统计raw帧中不同颜色通道的饱和像素和极暗像素（用于评估black_level设置是否合理）占比: `python aps_isp_tuner.py stats --width 1632 --height 1224 --input frame30.raw`


## 其他预先流程

### 烧录 RDK X5 系统镜像
1. 下载 [RDK Studio](https://developer.d-robotics.cc/rdkstudio)
2. 下载 镜像 .img 文件：[shimetapi_rdk_x5_v3.4.1_v2.1.img](https://pan.baidu.com/s/1uscQDdj84pvjso4FKFYgfQ?pwd=iiwn)
3. 安装RDK Studio后，选择`设备与开发`->`系统烧录`

# 以下是原始仓库README的部分内容

---


# 01 HV Toolkit
> 本仓库为**预编译二进制发布版**（核心以 `.so` 闭源交付，仅公开头文件与示例源码）。
> `lib/x5/` 为 aarch64 库（MIPI + Ethernet 后端）。


## ✨ v2.0 特性

- **三后端统一**：USB（libusb）/ MIPI（RDK，仅 ARM）/ Ethernet（POSIX TCP）—— 同一套 `Camera` 接口，后端由 `DeviceConfig.backend` 选择。
- **双回调 + 同步拉取**：`SetFrameCallback` / `SetEventCallback` / `SetImageCallback` 异步回调，`GetFrame` 同步拉取。
- **自洽编解码**：EVT2 / EVT3 / MIPI RAW8 的 `Encoder`/`Decoder`，零外部 SDK。
- **IO 读写**：`EventReader` / `EventWriter` / `HybridWriter` / `HybridReader`（RAW 文件读写 + EVS/APS 混合录制/回放）。
- **Python 绑定**（可选）：单一 `hv_toolkit` 模块（pybind11），x86_64 预编译（Python 3.10）。
- **MIPI 帧率档运行时选择**：`DeviceConfig.evs_fps`（120/240/300/500/750/1000，Init 时生效，无需重编库）。
- **零第三方事件 SDK 依赖**：公有表面已与旧版第三方事件 SDK 完全解耦。

## 📋 技术规格

### 事件相机参数

- **EVS 分辨率**：768×608
- **APS 分辨率**：1632x1224
- **数据传输**：MIPI-CSI（MIPI 后端）
- **事件格式**：新写的hvs_record输出的事件格式是 EVT3 文件头 + MIPI RAW8 数据
  - EVT2 / EVT3（兼容 Prophesee EventCD 语义）


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
## 例如
## export X5_SDK_ROOT=/app/shimetapi_hybrid_vision_toolkit/lib/x5
```

构建：

```bash
./run.sh build x5      # aarch64 交叉；工具链与 SDK 已就绪即可
```

产物布局同 s100：

```bash
ls out/x5/build/libshimetapi_*.so                                                # 预编译 4 个库已捆绑
file out/x5/build/samples/cpp/get_started/hv_sample_get_started                  # 应为 ELF aarch64
```


#### 查看支持的架构

```bash
./run.sh --list
# ARCH    STATUS        PREBUILT LIBS
# x86_64   ready         .../lib/x86_64
# s100     ready         .../lib/s100
# x5       ready         .../lib/x5
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
│   └── s100/                   # S100 aarch64：MIPI + Ethernet 后端
├── toolchains/                 # 交叉工具链文件（aarch64-linux-gnu）
├── third_party/                # aarch64 OpenCV（交叉编 OpenCV 类示例用）
├── samples/                    # 示例
│   ├── cpp/                    # C++ 示例（7 个 + 2 个我新增的`hvs_record`和`hvs_raw_to_csv`）
│   └── python/                 # Python 示例
└── docs/                       # 板端验证步骤与冒烟记录
```

## 🔍 示例程序说明

| 样例 | 用途 | 后端 | 命令速查 |
|---|---|---|---|
| `get_started` | 最小采集（同步 GetFrame） | USB / `--mipi` | `hv_sample_get_started [vid pid] [--mipi]` |
| `callback` | 事件 + APS 异步回调 | USB / `--mipi` / `--mipi-hvs` | `hv_sample_callback [--mipi-hvs]` |
| `record` | (不兼容X5 3.4.1+我的视觉模组) EVS+APS 混合录制到 /tmp | USB / `--mipi` / `--mipi-hvs` | `hv_sample_record [--mipi-hvs]` |
| `viewer` | 实时采集 + 解码计数 | USB / `--mipi` / `--mipi-hvs` | `hv_sample_viewer [--mipi]` |
| `bench_hw` | USB 实机吞吐基准 | USB | `hv_sample_bench_hw [vid pid duration_s]` |
| `live_record_display` | MIPI-HVS 实时预览 + 录制（OpenCV） | MipiHvs | `hv_sample_live_record_display [--no-display] [--evs-prefix s] [--aps-prefix s]` |
| `player` | 离线回放 .raw + .avi（OpenCV） | 离线 | `hv_sample_player <events.raw> <video.avi> [fps] [speed]` |
| `hvs_record` | MIPI-HVS 实时录制 .raw + .avi | MipiHvs | `bash scripts/record.sh --output [OUTPUT_DIR (存储 events.raw 和 video.avi)] --seconds [SECONDS]` |
| `hvs_raw_to_csv` | 离线转换 .raw 转为 csv (文件会比raw还大) 或者npz (文件压缩90%) | 离线 | `python3 hvs.py export-npz --input <events.raw> --output <events.npz>` |


## 📄 版权声明

版权所有 © ShiMetaPi。本仓库以预编译二进制形式分发 HV Toolkit 运行库；头文件与示例代码供集成开发使用。未经书面许可，不得反向工程、反汇编库文件或再分发其中的二进制组件。EVT2/EVT3 编解码为基于公开规范的独立实现（clean-room）。

---

## 🙋 联系我们

如果你在使用 HV Toolkit 过程中遇到任何问题或有任何建议，欢迎通过以下方式与我们联系：

开源硬件网站：https://www.shimetapi.cn （国内） / https://www.shimetapi.com （海外）
在线技术文档：https://forum.shimetapi.cn/wiki/zh/
在线技术社区：https://forum.shimetapi.cn

**HV Toolkit** - 让事件相机开发更简单 🚀


