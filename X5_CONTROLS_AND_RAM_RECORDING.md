# X5 原生控制与 HVS 内存录制

## 本次已实现

`hvs.py record` / `hv_hvs_record` 默认使用 `--storage memory`，双流文件写到 `/dev/shm/hvs-XXXXXX`，停止采集、关闭 writer 后复制到目标目录的 `.partial` 文件，检查长度、fsync，再发布最终文件。两份目标文件落盘后才清理 RAM 源文件。原 RAW/AVI 格式、双尺寸和时间戳保持不变。

采用本地厂商 `sample_evs_release/evs_mode/01_evs_live_player/evs_live_player.cpp` 的 tmpfs 思路；网页读取超时，实际核对的是该样例源码。没有修改厂商样例。

启动时检查目标空间、tmpfs 类型/空间和 MemAvailable，要求 payload 上限外预留 64 MiB。运行中监测剩余空间/系统内存，达到保护阈值正常停录。tmpfs 可能被操作系统换出；建议板端避免 swap 压力。当前仍由回调写 tmpfs，未增加异步队列，不能保证消除所有卡顿。

```sh
python3 hvs.py build
python3 hvs.py record --output /app/recordings/ram01 --seconds 10 --max-mib 512
python3 hvs.py stop --output /app/recordings/ram01 --wait 120
```

原先确需实验旁路的板端继续添加 `--x5-vin-bypass`，本次不改变其行为。
`--storage disk` 恢复直接写盘；`--ram-dir` 可选择另一个已挂载的 tmpfs。需要重新构建 C++ 录制程序。

停止命令识别 `recording.storage` 标记；落盘耗时可能超过默认 30 秒，超时不代表录制进程失败。请等待 summary.txt，避免重复录制覆盖相同目录。

复制失败时 summary 为 failed，RAM 目录保留在 `recording.storage` 和错误信息里；磁盘上的 `.partial` 不作为成功文件。释放空间后可人工复制 RAM 中的文件到新的目录，检查播放器后再清理。tmpfs 在重启/断电后丢失，无法提供断电恢复。不要根据不完整文件伪造 complete summary。

## 原生控制接口与接入限制

新增 `include/shimetapi/hv/x5_controls.h`，用于持有同进程有效 APS VIN/ISP 句柄的原生 X5 管线。调用者先包含匹配板端 SDK 的 `common_utils.h`、`hbn_isp_api.h`，并链接其 HBN 库。控制操作与节点销毁须由调用者串行化。

```cpp
#include "common_utils.h"
#include "hbn_isp_api.h"
#include <shimetapi/hv/x5_controls.h>
// 在原生管线创建并启动之后、销毁之前：
Shimeta::hv::x5::setOutputFrameRate(aps_vin_handle, 30, 15);
auto readback = Shimeta::hv::x5::setManualExposureUs(aps_isp_handle, 5000);
// 恢复自动曝光：
readback = Shimeta::hv::x5::setAutoExposure(aps_isp_handle);
// 输出 FPS=0 恢复 sensor 原生输出：
Shimeta::hv::x5::setOutputFrameRate(aps_vin_handle, 30, 0);
```

输入 FPS 必须来自实际 sensor 配置；VIN API 是按比例跳帧，不改变 sensor 帧长或曝光周期。曝光以微秒输入、转换为秒，读改写保留原增益等字段，SDK 返回错误时抛出异常，设置后另行读回属性。此接口保守限制曝光在 (0, 1 秒]，具体范围及量化仍由驱动检查。官方说明 getter 的 mode 不可用，不能拿它证明 AE 模式；属性读回也不是逐帧曝光生效证明。

**尚未接入当前 hvs.py 的 APS 控制参数。** 当前分发包只有预编译 Camera 实现且不公开 VIN/ISP 句柄，不能从 Camera 安全取得上述句柄。原有 --aps-fps/--aps-exposure-us 的拒绝仍保留；没有假装它们已经可以控制设备。完成这一步需要 Camera 后端源码或厂商正式公开 native-handle/control 接口；另一个方案是完整替换为原生双 VC 采集后端。VIN bypass 没有 ISP，不能调用 ISP 曝光接口代替 sensor 控制。

本次未新增原始 RAW10 获取后端；现有 Gray8 旁路的精度限制仍存在。

官方依据：
- https://developer.d-robotics.cc/x5_sdk_doc_v2.0.0/samples/sample_vin.html
- https://developer.d-robotics.cc/x5_sdk_doc_v2.0.0/multimedia_development/4-VIN_API_zh_CN.html （VIN_DYNAMIC_FPS_CTRL）
- https://developer.d-robotics.cc/x5_sdk_doc_v2.0.0/multimedia_development/5-ISP_Tune_API_zh_CN.html （曝光 get/set）
- https://forum.shimetapi.cn/wiki/zh/evs-camera/mipi-modules/carrier-boards/rdk-x5.html

## 验证边界

Windows 主机：录制源码编译检查、控制 API 契约测试（替身类型，不证明厂商 ABI）、文件发布成功/失败保留测试、既有 APS C++ 回归。Linux 的 tmpfs/statfs/fsync 分支及真实板端 SDK 编译、FPS/曝光效果、录制流畅度仍需 X5 验证。板端需核对本机 SDK 是否具有新版 VIN_DYNAMIC_FPS_CTRL；旧头文件不能直接使用该接口。

建议同一场景分别录制 memory/disk，比较实际 APS/EVS 接收计数、总时长和 CPU/内存使用；控制测试以图像亮度、时间戳间隔及 native frame_id 为证据。

主机验证结果：C++ 6/6、Python 22/22 通过；录制主程序编译检查通过。Python 使用已有 pyqt_env（默认 Python 缺少 cv2）。未进行板端测试。
