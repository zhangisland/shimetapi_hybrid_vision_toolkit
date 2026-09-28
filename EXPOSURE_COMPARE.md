# APS 曝光对比

工具：`tools/exposure_compare.py`；板端入口：`scripts/exposure_compare.sh`。
需要 Python 3 和 ffmpeg（仅 ffprobe 不够）。这次工具修改无需重编译 C++。
同时同步 `tools/check_native_recording.py`，它负责核查 AVI 和接收时间戳。

## 分析已有三份录像

在板端工程目录执行，输出目录必须尚不存在：

```bash
bash scripts/exposure_compare.sh analyze \
  --session 500=/app/recordings/hvs_native_500_347516460090104 \
  --session 1000=/app/recordings/hvs_native_1000_347097070243452 \
  --session 5000=/app/recordings/hvs_native_5000_347289799634910 \
  --output /app/recordings/exposure_analysis_01
```

只读源录像。要求 native 录制的 `summary.txt`、`aps.avi`、`aps.frames.csv`，
且标记为完整的 ISP_NV12_uncorrected；不把 Bayer 数据当 NV12。
曝光标签来自命令输入，没有启动日志时读回值为 null，不推断曝光实际生效。

## 自动采集三组并比较

先正常退出正在占用相机的程序，固定相机、光照和场景，在画面中央放置不过曝的静态物体。
避免对着亮窗或显示屏。三组数据需预留数 GB 空间。

```bash
ffmpeg -version
bash scripts/exposure_compare.sh capture \
  --output /app/recordings/exposure_compare_01 \
  --exposures-us 500 1000 5000 --gain 1 --dgain 1 \
  --profile 1 --seconds 6
```

使用现有 `out/x5/hvs-build` 的 native live，可用 `--build-dir` 指定其他构建目录。
依次启动采集，固定模拟/数字增益，关闭软件校正与软件白平衡，无显示录制。
检测到已知相机进程会退出，不终止已有进程；检测不能覆盖所有自定义相机程序。
失败时保留日志及已采集文件。中断只向本脚本启动的进程组发送 SIGINT。

输出包含各次启动日志、录像、`manifest.json` 和 `analysis/comparison.csv`、`comparison.json`。
重新分析（使用新报告目录）可保留日志中的读回信息：

```bash
bash scripts/exposure_compare.sh analyze \
  --manifest /app/recordings/exposure_compare_01/manifest.json \
  --output /app/recordings/exposure_analysis_02
```

## 如何解读

默认跳过首 1 秒，仅用于统计，不删除录像帧。再取 3 秒、最多 120 帧。
`--warmup` 和 `--sample-seconds` 可调整，自动采集总时长须大于两者之和。
从 ISP NV12 提取中心半宽、半高区域 Y 平面，面积缩小至 64×48 后统计：

- requested_us：请求曝光，单位微秒。
- readback_us：ISP 属性读回，缺失时为 null，不是逐帧曝光测量。
- median_Y：每帧中心区域平均 Y 的中位数；JSON 另含波动范围和标准差。
- Y_ratio：相对最短请求曝光的亮度比，不等同曝光倍数。
- near_white%：缩小区域中 Y≥235 的比例，用于识别高亮区域；不是传感器饱和率。
- FPS、frames、duration_s：录像时序检查结果，不用于判断曝光是否生效。

相同场景和增益下，亮度随请求曝光变化，是成像响应的证据，但仍不能测得实际积分时间。
亮度接近时，应结合读回值、过曝比例和帧间波动进一步排查。
ISP gamma、色调处理、Y 黑电平和灯光闪烁都会影响比较，不能要求亮度严格按曝光倍数变化。
本工具不会将 API 接受请求或属性读回标记为传感器曝光已验证。

## 本地验证

`python -m unittest discover -s tests -p test_exposure_compare.py`
包含合成 NV12 AVI 的真实 ffmpeg 解码、已知 Y 值对比、时间戳核查、读回解析、
空帧/非法参数拒绝及禁止覆盖报告目录。板端真实曝光效果需执行上述命令验证。
