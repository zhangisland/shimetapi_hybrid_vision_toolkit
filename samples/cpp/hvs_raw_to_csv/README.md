# HVS RAW8 转 CSV

接入 toolkit 的 CMake 样例树；`hvs.py build` 默认同时构建录制器和 CSV 转换器。
不需要 OpenCV、Metavision 或连接相机。运行平台为 Linux / X5。

在 toolkit 根目录执行：

```bash
python3 hvs.py build
# 如需同时构建播放器：python3 hvs.py build --with-player
# x86 Linux 交叉编译：python3 hvs.py build --cross
python3 hvs.py export-csv --input /app/recordings/session/events.raw --output /app/recordings/session/events.csv
```

自定义构建目录时，build 和 export-csv 均传 `--build-dir <目录>`。
输出文件必须不存在，父目录必须已存在。请在录制结束后转换。
可执行文件位于 `<build-dir>/samples/cpp/hvs_raw_to_csv/hv_hvs_raw_to_csv`。

输入限定为 hvs_record 保存的“文本头 + MIPI RAW8 子帧流”，不是通用 EVT2/EVT3 文件。
复用播放器的 HybridReader 和 MipiRaw8Decoder；逐个 32768 字节子帧转换，内存不随录像长度增长。
输出列为 `x,y,polarity,timestamp`，极性为 0=OFF、1=ON，时间为 SDK 解码的传感器微秒值。
不归零、不排序、不另行补偿时间戳回绕。CSV 通常比 RAW 文件大。

该工具没有改变录制格式。真实 SDK 解码与 ARM 链接仍需在板端验证。

## 进度和压缩 NPZ

重新运行 `python3 hvs.py build`（需要播放器时加 `--with-player`）。

```bash
python3 hvs.py export-csv --input /app/recordings/session/events.raw --output /app/recordings/session/events.csv
python3 hvs.py export-npz --input /app/recordings/session/events.raw --output /app/recordings/session/events.npz
```

CSV 显示单行刷新的百分比进度和 ETA（预计剩余秒数）。NPZ 分解码、压缩两个阶段显示进度与各阶段 ETA；解码阶段无法预知压缩耗时，不提供虚假的全程 ETA。
NPZ 采用 ZIP DEFLATE 压缩，含 x(uint16)、y(uint16)、polarity(uint8)、timestamp(int64) 四个一维数组，长度均为事件数，时间戳单位微秒，与 CSV 保持同样顺序和值。
导出使用 Python 标准库，不要求板端安装 NumPy。读取示例：

```python
import numpy as np
with np.load("events.npz", allow_pickle=False) as events:
    print(events["x"], events["y"], events["polarity"], events["timestamp"])
```

NPZ 复用统一构建的转换器，将中间列文件写入输出目录下的临时子目录，再分块压缩，避免整段录像驻留内存。
临时磁盘空间约为每事件 13 字节，另外还需 NPZ 输出空间；正常完成或 Python 捕获异常后清理临时文件。
同样拒绝覆盖已有文件。压缩率依赖实际数据，不保证比稀疏 RAW8 输入更小。
