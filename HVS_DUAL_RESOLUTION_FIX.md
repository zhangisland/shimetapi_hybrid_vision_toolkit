# 修复：EVS 768×608，APS 1632×1224

用户板端日志已经确认 VIN 旁路取得首帧：
`APS VIN frame: 1632x1224 stride=3264 dtype=0x2b size=3995136 (Gray8 bypass)`。
当前失败来自应用错误地把 APS 默认尺寸写成 768×608，而不是相机仍无输出。
`stride=3264` 是底层 RAW10 的行字节数；SDK 返回应用前已转换并打包成 Gray8。

## 此次修改

- APS 默认尺寸：1632×1224，参数 `--aps-width/--aps-height`。
- EVS 默认尺寸：768×608，参数 `--evs-width/--evs-height`。
- 旧 `--width/--height` 保留为 APS 参数别名。
- EVS 用 `EventWriter` 独立写入 RAW，保持 768×608 文件头。
- APS 使用原有 `HybridWriter` 的 AVI 和 tsmp 写入功能，按 1632×1224 建 AVI。
  它附带的未使用 RAW 文件头写到 `/dev/null`，真正的 EVS 包只交给独立 EventWriter 一次。
- summary.txt 增加两路宽高，便于检查记录设置。

不能只把旧版 HybridWriter 的唯一一组 width/height 改为 1632×1224：
它没有两路独立尺寸参数，可能同时把 EVS 文件头也改成 APS 尺寸。
本次因此修改了 C++ 写入器，必须重新构建，不能只更新 Python 文件。

## 板端操作

将 `hvs_x5_dual_resolution_fix.tar.gz` 上传到 X5 的 `/tmp/`。
解包覆盖包内同名源码与配置，若板端自行修改过这些文件，请先保留修改。

```bash
tar -xzf /tmp/hvs_x5_dual_resolution_fix.tar.gz -C /app/shimetapi_hybrid_vision_toolkit
cd /app/shimetapi_hybrid_vision_toolkit
python3 hvs.py build --with-player
python3 hvs.py record --x5-vin-bypass \
  --evs-width 768 --evs-height 608 \
  --aps-width 1632 --aps-height 1224 \
  --output /app/recordings/test_vin02 --seconds 10 --timeout 15 --max-mib 2048
```

这些宽高参数已经是新默认值，显式写出便于确认。test_vin02 必须不存在。
全分辨率 APS 存为无压缩 NV12，每帧约 3 MB；例子提高大小上限至 2 GiB，
避免 10 秒录制因旧 1 GiB 限制提前停止。仍需足够空闲空间和写盘吞吐。

成功结束后检查与回放：

```bash
cat /app/recordings/test_vin02/summary.txt
python3 hvs.py play --output /app/recordings/test_vin02
```

应有 status=complete、两路非零计数、evs_width=768、evs_height=608、
aps_width=1632、aps_height=1224。回放需要图形桌面。
停止仍支持 Ctrl+C 和 `python3 hvs.py stop --output /app/recordings/test_vin02`。

## 色彩与验证范围

这是 RGB 传感器的 APS 尺寸，但**当前 SDK 旁路交给应用的是 Gray8，不是 RGB 图像**。
存成 NV12 仅保留当前单通道数据，并填中性 UV，不进行 Bayer 去马赛克、白平衡或色彩恢复。
若需要真正彩色 RGB，还需要验证 ISP，或依据厂商确认的 RAW 格式、Bayer 排列和有效区域
另外实现彩色处理；尺寸修正本身不能做到这些。

20 项 Python 测试通过；C++17 语法检查通过；使用 SDK 写入器替身的 C++ 回归测试通过，
覆盖两路独立尺寸、EVS 字节不重复写入、APS 时间戳传递与错误返回。
这些测试不等于真实 SDK 文件写入或 ARM 链接验证。
用户日志证明旁路已出首帧，但新版本完整录制、同步及回放仍待板端确认。

后续已新增条件性的 Bayer 彩色回放：读取保存的原始 Y 平面，见 [HVS_COLOR_PLAYBACK.md](HVS_COLOR_PLAYBACK.md)。Gray8 是当前接口格式；如果像素仍为 Bayer 采样，并不意味着全部颜色信息已经丢失。
