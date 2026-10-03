**以下内容仅针对 HVS APX 003CC + RDK X5 3.4.1**

# 以下是基于RDK X5: rdk-x5-ubuntu22-preinstalled-desktop-3.4.1-arm64 镜像的命令 (64GB TF卡, 当前板上版本)

## 1. 库构建
```bash
git clone https://github.com/zhangisland/shimetapi_hybrid_vision_toolkit # 我修改过的

# git config core.fileMode false  # git 不追踪文件权限的修改

# aarch64 交叉；工具链与 SDK 已就绪即可, 会构建一些自带的sample
./run.sh build x5 

# 部署代码后，修改代码重新build
python3 hvs.py build --with-vin-record --with-player --platform-samples /app/multimedia_samples --sdk-root /usr/hobot --sdk-include-dir /usr/include --jobs 2

```
> 如果后续运行代码提示缺libxxx, 可能需要的:
> `export LD_LIBRARY_PATH=/app/shimetapi_hybrid_vision_toolkit/lib/x5${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}`


## 2. 数据采集流程

### 2.0 显示实时画面 preview

```bash
cd /app/shimetapi_hybrid_vision_toolkit
# 实时预览，不保存录像，先预览一下当前aps-gain-db设置下的画面效果；q / Esc 退出
python3 hvs.py live --x5-vin-bypass --  --aps-gain-db 0 --i2c-bus 6 --preview-width 960

# 或者通过scripts执行
bash scripts/live.sh --preview-width 960 --aps-gain-db 0 

```


### 2.1 录制 record
```bash
cd /app/shimetapi_hybrid_vision_toolkit

# 现在是通过 --storage 先写到memory 再转到disk, 一段5s 150帧的avi需要大约110s才能转化完存储到硬盘, 有点耽误时间
# 通过 --aps-gain-db 调整曝光增益, 范围 0-12 dB
# 如果要快速保存就需要将 --save-format 设置为 fast
python3 hvs.py record --x5-vin-bypass --storage memory --seconds 5 --max-mib 1024 --i2c-bus 6 --i2c-address 0x3c --aps-gain-db 0  --output /app/recordings/gain0_new --save-format fast

## 或者是通过 scripts/record.sh
bash scripts/record.sh --output /app/recordings/gain0_new --seconds 5
```

### 2.2 回放 play
```bash
cd /app/shimetapi_hybrid_vision_toolkit

# 直接回放压缩会话，无需先导出 AVI
# 兼容原先的不带 --save-format fast 录制的数据
python3 hvs.py play --window-width 1280 --window-height 720 --output /app/recordings/gain0_new 

## 或者是通过 scripts/play.sh
bash scripts/play.sh --output /app/recordings/gain0_new
```


### 2.3 采集后的数据处理 aps/evs.vin.zst -> aps.avi + event.raw + evs/aps.vin.bin 以及 events.raw -> events.npz
```bash
# 1. 随时导出原有四文件，源压缩会话保留
## 如果 record 的时候采用了 **--save-format fast 参数**，则需要进行以下转换，将 2个文件 `aps.vin.zst`和`evs.vin.zst`导出为 4个文件 `aps.vin.bin, evs.vin.bin, aps.avi, events.raw`
python3 hvs.py export-recording --max-mib 1024 --input /app/recordings/fast_new --output /app/recordings/fast_new_export
bash scripts/export_raw_in_fast_mode.sh --input /app/recordings/fast_new --output /app/recordings/fast_new_export 


# 2. event raw 转 npz (为了减少文件大小)
## 一个原本 149M 的events.raw，转为csv文件大小是 199M, 而转为npz后文件大小 18M，文件大小减少 90%
## 会显示进度条
python3 hvs.py export-npz --input events.raw --output events.npz

# 或者是用script，直接指定包含events.raw的目录, 对应转换的npz将保存到events.raw所在目录
bash scripts/events_to_npz.sh /app/recordings/fast_new_export/
```


# 以下是基于RDK X5: shimetapi_rdk_x5_v3.4.1_v2.1 镜像的命令 (128GB TF卡)

**原生内存录制更新**：`hvs.py record --x5-vin-bypass` 现在需要先 `hvs.py build --with-vin-record`，使用独立双 VIN、停止后落盘和外部曝光控制。
参见 [构建、曝光、内存边界及性能验收说明](README_VIN_MEMORY_CN.md)。
旧 patched SDK 对照改用 `--legacy-sdk-bypass`。

## 必要的构建库
以下开发库二选一
1. `git clone https://github.com/ShiMetaPi/shimetapi_hybrid_vision_toolkit`  # 原始官方库
2. `git clone https://github.com/zhangisland/shimetapi_hybrid_vision_toolkit` # 我修改过的


```bash
./run.sh build x5      # aarch64 交叉；工具链与 SDK 已就绪即可

# 如果后续运行代码提示缺libxxx, 可能需要的:
export LD_LIBRARY_PATH=/app/shimetapi_hybrid_vision_toolkit/lib/x5${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
```


## HVS相关的源代码编译
```bash
cd /app/shimetapi_hybrid_vision_toolkit
# python3 hvs.py build --with-player
python3 hvs.py build --with-native-live --with-player --platform-samples /app/multimedia_samples --sdk-root /usr/hobot --sdk-include-dir /usr/include
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


## 📄 版权声明

版权所有 © ShiMetaPi。本仓库以预编译二进制形式分发 HV Toolkit 运行库；头文件与示例代码供集成开发使用。未经书面许可，不得反向工程、反汇编库文件或再分发其中的二进制组件。EVT2/EVT3 编解码为基于公开规范的独立实现（clean-room）。

---

## 🙋 联系我们

如果你在使用 HV Toolkit 过程中遇到任何问题或有任何建议，欢迎通过以下方式与我们联系：

开源硬件网站：https://www.shimetapi.cn （国内） / https://www.shimetapi.com （海外）
在线技术文档：https://forum.shimetapi.cn/wiki/zh/
在线技术社区：https://forum.shimetapi.cn

**HV Toolkit** - 让事件相机开发更简单 🚀


