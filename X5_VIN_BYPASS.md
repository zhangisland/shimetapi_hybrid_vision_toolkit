# X5 APS=0：定位结果与可选 VIN 旁路实验

更新：用户已验证旁路取得 1632×1224 首帧。当前版本需应用 [双尺寸修复](HVS_DUAL_RESOLUTION_FIX.md) 并重新构建。以下实验包部署命令仅记录上一版步骤。

## 本次日志已确认的范围

- `hv_hvs_record` 和 `hv_sample_player` 已在用户板端编译、链接成功。
- EVS 包计数从 31 增至 217，APS 一直为 0；原来的“等待 APS 把 EVS 全丢掉”已消除。
- 2A 调用失败后，SDK 打印继续自动 2A，并持续报 APS getframe 失败。
- 这不是 AVI 播放器或录制输出路径导致的失败；应用没有收到 APS 图像。
- EVS packets 是传输包数，不是传感器帧数或事件数，不能据约 30 包/秒判定传感器仅 30 fps。

## 预编译库的具体控制流

对本地 X5 ELF 进行只读分析，并与官方 GitHub 提交比较：

官方提交：
https://github.com/ShiMetaPi/shimetapi_hybrid_vision_toolkit/commit/02c3732de83df9f0cb7e020c13ea7efc4d67bede

原库 SHA-256：
`6ae0ae6ba5335f8a2f09b546ab49ccade1e22474ddd5e161a9bdb5f78b147f4f`

本地与该提交的 `lib/x5/libshimetapi_hv.so.2.0.0` 完全一致。
这证明本地不是随意混入的 S100 库，但不代替核对板端实际库文件和系统多媒体版本。

`startPipe(X5Pipe&, bool)` 在 APS 配置具有 ISP 属性时创建 ISP，随后尝试设置 2A。
2A 失败后仅打印消息，没有清空 ISP handle，也没有重建为 VIN-only 管线。
`readImageFrame` 优先选择非零 ISP handle；只有 ISP handle 为零时才选择 VIN handle 并执行
RAW10→Gray8 路径。getframe 返回失败后也没有自动切换到 VIN。
因此，“库包含 Gray8 旁路”不等于“目前初始化方式实际启用了旁路”。

需注意 `APS ISP getframe failed` 字符串是这个版本共用的失败消息，
单靠这条字符串不能识别节点类型；本次结合初始化代码和 2A 调用日志判断 ISP 路径。
不把 `ret=-35` 擅自解释成某个确定的 Linux errno 或唯一硬件根因。

## 推荐的正式修复

请 Shimeta 为匹配的 X5 SDK 提供显式 VIN-only 模式或失败回退实现：
APS 从 VIN DDR 读取，避免创建/绑定不可用的 ISP，再复用已有 Gray8 转换。
厂商应验证底层多媒体版本、双 VC 配置、缓存刷新、时间戳与数据完整性。
本地缺少该实现的 C++ 源码，无法给出厂商源码级补丁。

## 本次提供的可选实验（不是厂商正式更新）

入口：`python3 hvs.py record --x5-vin-bypass ...`

该选项做的是**独立库副本中的二进制分支修改**，不是公开的 Camera API 参数。
只对上面的精确 SHA-256 生效，其他版本立即拒绝，不能通过删除校验强行套用。
它在 `out/x5/vin-bypass/<原库哈希前16位>/libshimetapi_hv.so.2` 中生成一个副本，
仅对这次录制进程优先加载该副本。`lib/x5` 原文件不改变。

变更位于文件偏移/虚拟地址 `0xf798`：

- 原指令：`d6 04 00 34`，`cbz w22, 0xf830`，只有 EVS 管线跳过 ISP 创建。
- 实验指令：`26 00 00 14`，`b 0xf830`，APS 管线也跳过 ISP 创建。
- 在这个分支前，VIN 节点和输出缓冲已创建；之后保留 vflow 启动与现有取帧代码。
- 预期结果是 ISP handle 保持零，已有取帧逻辑选择 VIN，并输出 Gray8。

副本 SHA-256：
`3fba9bf11934a3b0fea2abd4aca8b1f2f0ff5647101468d4a0e4c1a753b3580d`

这只是基于精确版本控制流的诊断性实验，没有真实 X5/HVS 测试结果。
它不保证 VIN 有图像，也不保证双路同步或无丢包；需要单独验收。

## 板端操作

将 `hvs_x5_vin_experiment.tar.gz` 上传到 X5 的 `/tmp/`，在已经成功编译的工程上执行：

```bash
tar -xzf /tmp/hvs_x5_vin_experiment.tar.gz -C /app/shimetapi_hybrid_vision_toolkit
cd /app/shimetapi_hybrid_vision_toolkit
python3 hvs.py record --x5-vin-bypass \
  --output /app/recordings/test_vin01 --seconds 10 --timeout 15
```

此次更新是 Python 入口与辅助脚本，沿用已编译的 C++ 录制程序，无需再次构建。
输出目录必须是新目录；不要覆盖刚才失败但仍保留 EVS 的 test01。

关注以下结果：

1. 开头出现 `EXPERIMENTAL X5 VIN bypass:`，指向独立副本目录。
2. 不再在这个 APS 初始化流程里调用 ISP 2A；出现 `APS VIN frame ... (Gray8 bypass)`。
3. APS 与 EVS 计数都增长，最终 `summary.txt` 为 complete。
4. 若报告 APS 尺寸与新默认 1632×1224 不同，按错误中的实际宽高使用 `--width W --height H`，
   并换一个新输出目录再录制，不猜测传感器日志中的 resolution 就是应用输出高度。
5. 在图形桌面执行 `python3 hvs.py play --output /app/recordings/test_vin01`，检查两路内容。

APS 是灰度图，转换 NV12 只是让现有 AVI/player 保存和显示它，不会恢复彩色。

停止仍可 Ctrl+C 或 `python3 hvs.py stop --output /app/recordings/test_vin01`。
恢复原行为只需下次不带 `--x5-vin-bypass`，无需还原任何原库。

若实验仍 APS=0，请保留全部日志，并执行：

```bash
python3 hvs.py diagnose > /app/recordings/x5-diagnose.txt 2>&1
sha256sum lib/x5/libshimetapi_hv.so.2.0.0
cat /etc/os-release
```

这些信息与完整录制日志用于进一步核对 BSP/多媒体 ABI 和 VIN 配置。

## 已验证 / 未验证

20 项 Python 测试通过，包括原控制脚本和实验的版本拒绝、单指令修改范围、整库输出哈希、
原库不变、缓存副本校验、默认关闭和加载路径优先级。
通过 ELF SONAME 和 AArch64 反汇编核对分支目标。
用户板端日志现已验证修改后的库取得 APS VIN 首帧（1632×1224）。完整录制、同步与回放质量仍未验证。
