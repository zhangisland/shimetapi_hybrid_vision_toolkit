# APS Bayer 彩色回放

## 为什么 Gray8 仍可能重建彩色

本项目 X5 SDK 的 VIN RAW10 路径逐像素执行 `sample >> 2`，并按原空间位置输出 Gray8；
没有进行 RGB 加权转灰度或去马赛克。录制器将这些字节原样复制到 AVI 的 Y 平面，并填 UV=128。
因此，如果传感器该模式输出正常 Bayer CFA，Y 平面仍保留各位置的 R/G/B 采样，
可以在回放时去马赛克生成 BGR（OpenCV 的彩色通道顺序）。

这不是给灰度图自动上色，也不能恢复已丢弃的 RAW10 低两位。
如果传感器/其他软件已混色为真正灰度、改变了 CFA 排列，或者录像被缩放/有损转码，
本功能不能恢复原始颜色。这里需要原始的、由本项目 Gray8 录制器生成的 aps.avi。

参考：OpenCV demosaicing 支持 8/16 位 Bayer 单通道输入：
https://docs.opencv.org/4.x/d8/d01/group__imgproc__color__conversions.html

## 部署与使用

将 `hvs_x5_color_playback.tar.gz` 上传到板端 `/tmp/`。更新包覆盖同名源码与说明。

```bash
tar -xzf /tmp/hvs_x5_color_playback.tar.gz -C /app/shimetapi_hybrid_vision_toolkit
cd /app/shimetapi_hybrid_vision_toolkit
python3 hvs.py build --with-player
python3 hvs.py play --output /app/recordings/test_vin02 --aps-bayer compare
```

已有成功完成的原始 Gray8 录像可以直接尝试，不需要因为这个播放器功能重新录制。
把示例目录替换为你实际录制成功的目录；失败/未收尾的录像仍会被拒绝。
compare 在 APS 区域显示四幅标注为 rggb、bggr、grbg、gbrg 的候选彩色画面。
输入全分辨率先去马赛克，再缩小用于四宫格显示；不会先缩放 Bayer 再解码。
EVS 回放、暂停、步进和时间戳逻辑保持原有行为。

选择确认的排列后，改用对应参数。例如，仅在确认 RGGB 后：

```bash
python3 hvs.py play --output /app/recordings/test_vin02 --aps-bayer rggb
```

不带 `--aps-bayer` 或设置 `--aps-bayer none` 为原来的 NV12 播放。
程序使用 BGR 交给 OpenCV 显示；这是彩色图像，不是红蓝颠倒。
如自行把图像交给要求 RGB 的其他库，需另做 BGR→RGB 通道转换。
此选项仅改变显示，不改原录像，也不会保存为新 RGB 视频。

## 不知道 Bayer 排列时如何验证

用户目前没有厂商确认的 CFA 排列，因此程序不设默认彩色排列。
可用包含红/蓝/绿物体和白纸的画面比较：检查红蓝是否互换、是否存在明显棋盘纹。
实际排列还受传感器镜像/翻转、裁剪起点以及 binning 模式影响。
自然场景的“颜色看起来舒服”并不足以最终确认，最好向 Shimeta 核对当前模式。
四种结果都明显不合理时，不要任选一种当作正确彩色，需核实模式是否为普通 Bayer。

程序只做基础去马赛克，不包含厂商 ISP 的黑电平、白平衡、色彩校准和 gamma 曲线。
即使排列正确，颜色/亮度也可能与厂商 ISP 输出不同。

## 文件与兼容性

- `samples/cpp/player/aps_color.h`：NV12/Bayer 解码、四宫格比较。
- `player_widgets.*`：在读取 APS 原始 Y 平面时调用解码，避免先做 YUV 范围变换。
- `player/main.cpp`：新增 `--aps-bayer` 参数及错误报告。
- `hvs.py`：转发参数，要求录像 summary 表明全部 APS 来自 Gray8。
- 每帧会检查 UV=128，避免误把普通彩色 NV12 当作 Bayer；UV 中性本身不能证明 CFA 正确。

随包 OpenCV 头文件仅提供旧的双字母转换枚举。
按照 OpenCV 定义，RGGB→BGR 使用 COLOR_BayerBG2BGR，BGGR→BGR 使用 COLOR_BayerRG2BGR，
GRBG→BGR 使用 COLOR_BayerGB2BGR，GBRG→BGR 使用 COLOR_BayerGR2BGR。
不要根据旧枚举名称的两个字母反向猜传感器排列。

## 验证范围

23 项 Python 测试通过，其中合成 Bayer 阵列测试验证了四种实际代码映射的红绿蓝通道。
播放器解码模块和 C++ 颜色测试通过 MSVC C++17 语法检查；这不是 ARM 链接验证。
C++ 测试程序可在装有 OpenCV 开发包的 Linux/X5 执行：

```bash
c++ -std=c++17 tests/test_aps_color.cpp -I include \
  $(pkg-config --cflags --libs opencv4) -o /tmp/hvs_test_aps_color
/tmp/hvs_test_aps_color
```

尚未拿到真实录像或在 X5 上运行本次彩色播放器，不能保证哪一种排列适合当前传感器模式。
