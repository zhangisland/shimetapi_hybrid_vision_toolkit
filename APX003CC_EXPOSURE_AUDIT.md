# APX003CC 曝光控制审计与应用层修补（2026-09-29）

**后续已有板端实测：**已通过用户提供的 SSH 密钥确认真实驱动及 I2C 控制调用，见 [板端驱动证据](tools/apx003cc_diagnostics/BOARD_FINDINGS.md)。下文“缺少板端连接”等内容是首轮历史限制，已被后续证据更新；曝光生效问题仍未修复。

**尚未证明硬件曝光控制已修复。** 本次实际修改了应用层失败传播、控制日志、元数据和二进制识别，并执行主机测试。缺少匹配的 APX003CC sensor 驱动实现和板端连接，无法验证曝光/增益最终写入。不能将本次改动作为室外或夜间验收通过。

## 证据与边界

- 初始 Git 工作区干净；主仓库、已检查父目录及参考树未找到适用 AGENTS.md。未提交、推送、修改厂商参考文件或部署到板端。
- `hvs.py live` 原样传递参数，默认启动 `out/x5/hvs-build/samples/cpp/live_record_display/hv_sample_live_record_display`，不是旁边的 `build` 或 `out/x5/build`。本机确实存在多个旧构建目录，但不能据此断言板端用了旧文件。现在打印绝对路径、SHA256、参数和 native 修订标识。
- `native_live.cpp` 默认请求 5100 us / 1 / 1，显式 CLI 覆盖默认值，重复项最后一个生效。profile 仅选择配置，不覆盖曝光。参考 hvs_config.c 的 profile 0..5 对应 EVS 120/240/300/500/750/1000；profile 1 是 240，不是 500。scripts/live.sh 的默认请求本来是 1000 us，其帮助误写 5100，现已修正。
- `x5_open` 选择两个 vp_sensors 配置，共享一次 MIPI 探测，然后各自 create/attach/start。APS 是 VC1 VIN → offline ISP → NV12。vp_sensors/apx003cc 文件是配置对象，不是 sensor 曝光/增益回调实现。
- 本地 hbn_isp_api.h 的 manual_attr.exp_time 明确为秒。原有微秒 / 1e6 换算正确，无固定曝光行数或应用层夹紧。现打印最终 float 值，避免低精度日志混淆短曝光。
- 增益字段仅定义为 float，所查官方 API 文档未注明倍率/dB/编码。保持既有正数约束和原样传递，撤回无充分依据的“倍数”说明；没有开放 0、臆造换算、声称 1 为硬件最小值。实际合法范围仍需匹配驱动。
- 原来已使用 get → 修改 mode/manual_attr → set；设置点在 VC1 start 后，VC0 此时已经启动。**没有证据证明第二个 VC 后续初始化覆盖了这次设置**。本次把控制调用显式放在两路启动循环之后，实质顺序不变；未删除 camera 对象或操作未知复位寄存器。
- 提交 HBN_ISP_MODE_MANUAL，保留自动模式拒绝策略；getter 的 mode 暂不可用，不能拿它证明 AE/AGC 已停。保留并记录原 ISP gain，不通过软件压暗、gamma 或 WB 改动伪装曝光生效。

## 已确认并修补的问题

1. 设置后 getter 失败原来只打印 unavailable 并返回成功。现在传播原始错误码、终止初始化并清理管线；非有限回读也失败。
2. 原录像没有请求/提交/回读信息。新增独立快照存入 summary.txt；逐帧实际曝光仍为 unknown。hvs.py play（含 scripts/play.sh）显示四层信息，旧数据缺失字段仍为 unknown。
3. 原启动入口没有真实二进制标识。现在打印绝对路径及 SHA256；native 打印 `capture_control_revision=aps-control-audit-v1` 和编译时间。标识只证明文件身份，不证明曝光生效。

新 x5_exposure_control.h 被实际 C 适配器调用，也接受假 SDK 合同测试。保留原 SDK version、ISP gain、其它属性，只修改原有曝光、增益及 mode 字段。记录初始值、AE 策略范围、提交值、回读值。AE 策略范围不是已确认的手动 sensor 范围，不用于夹紧。回读与提交不同会明确提示，原因保留未知，不自动归因或把请求填成实际值。

## 仍阻塞的最后一段链路

`-65545` 的绝对值是 `0x10009`；本地 hbn_error.h 的 ISP 错误通过 HB_ISP 组合，注释为 `0x0C....`，无法据此确认该错误的具体含义。日志保留原始十进制、十六进制及调用阶段，不猜测错误码。

已检索给定样例、头文件及融合相机资料相关文件，未找到 APX003CC sensor_module_t、aexp_line_control、aexp_gain_control、userspace_control 的实现/注册。厂商官方 GitHub SDK 树查询失败：HTTP 403 rate limit exceeded。没有可据以修改的驱动源码，无法提供真实驱动补丁或其构建命令。

需要匹配板端安装版本的 APX003CC 源码与 tuning，检查 mode=2 控制分支、每秒行数、最小/最大曝光行数、帧周期、gain LUT/单位、回调注册和 I2C 返回值。通用框架源码位置为 hbre/camsys/libcam/src/sensor/，但不能假定具体厂商发行版也提供同样目录。双 camera 是否产生共享状态冲突/重复 reset，须记录驱动 bus/address/mode/init/start 和控制回调顺序确认。当前及参考样例均使用双对象，不能直接认定为根因。

真正传感器实际值需要厂商支持的回读或帧元数据。SDK getter 成功不足以证明积分时间，不能直接写未知寄存器。

官方参考：
- [X5 ISP API](https://developer.d-robotics.cc/x5_sdk_doc/multimedia_development/5-ISP_Tune_API_zh_CN.html)：秒单位、getter mode 限制、tuning 模块状态前提。
- [X5 Camera 调试指南](https://developer.d-robotics.cc/x5_sdk_doc/multimedia_development/isp_tuning_guide/Camera_sensor_bringup.html)：驱动目录及曝光/增益回调职责；不证明 APX003CC 已实现这些回调。

## 部署与复测

源文件更新包需同步到板端同版本的 `/app/shimetapi_hybrid_vision_toolkit`，先保留板端已有改动。包括新增 x5_exposure_control.h，不能只复制 hvs.py；更新包不含已验证的 X5 二进制。使用匹配 SDK、样例和 OpenCV 开发库重新构建：

```bash
cd /app/shimetapi_hybrid_vision_toolkit
python3 hvs.py build --with-native-live --with-player --platform-samples /app/multimedia_samples --sdk-root /usr/hobot --sdk-include-dir /usr/include
sha256sum out/x5/hvs-build/samples/cpp/live_record_display/hv_sample_live_record_display
python3 hvs.py live -- --profile 1 --aps-ae manual --aps-exposure-us 500 --aps-gain 1 --aps-dgain 1 --aps-correction off --aps-wb off --no-display --seconds 6 --record --output /app/recordings/exposure_audit_500
```

500/1/1 是请求示例，**尚不是已确认的传感器合法参数**。先核实匹配驱动的增益编码和曝光范围；API 拒绝时保留日志，不能改用 0、忽略失败或退回默认值。输出应有新 revision 标识，SHA256 与前一条一致。output 是前缀，实际目录自动追加时间编号。

厂商确认 500/1000/5000 us 及增益 1/1 合法后，用现有验证工具（需 ffmpeg 和存在的父目录）：

```bash
python3 tools/exposure_compare.py capture --profile 1 --exposures-us 500 1000 5000 --gain 1 --dgain 1 --seconds 6 --output /app/recordings/exposure_sweep_audit
```

否则用确认过的三档值替换：跨度明显，不超过该模式帧周期/积分上限。固定室内光照、固定光圈、固定合法增益、手动模式及固定未饱和场景，避免频闪和运动。工具关闭软件校正/WB，记录未校正 ISP NV12，跳过开头 1 秒后比较中心 ROI 的 Y 统计。

判断：三档请求及提交应不同；检查 SDK 回读是否随之变化、差异是否符合驱动量化；亮度应有明显一致响应，不要求 ISP 后亮度严格线性。setter 失败先查 API/驱动，回读不变或成像无响应继续查回调；全白不能单独判断曝光是否变化。实际回读未知时只能报告“成像响应验证”，不可声称积分时间已测量。

室内通过后再用最短合法曝光测试室外及饱和比例。50 us CLI 不代表真实 50 us。夜间尚未验证；暗光仍受实际曝光范围和帧周期限制，不保证任意极暗场景可用。

## 本机验证记录

Windows MSVC + 已有 OpenCV 构建成功，17 项 C++ 测试全部通过（包括单位/数值边界、get/set/get 错误传播、原生 CLI profile 前后顺序、录像快照、EVS 断流、缓冲区释放及原有回归）；模拟测试不代表 X5 ABI、SDK 链接或硬件响应验证。Python 默认环境缺 cv2 导致首轮完整测试失败，改用已有 `D:/miniforge3/envs/pyqt_env/python.exe` 后 38 项通过。

WSL 启动失败：`Wsl/Service/CreateInstance/MountVhd/HCS/ERROR_PATH_NOT_FOUND`，缺失 `G:/WSL/ext4.vhdx`。尚未完成 X5 编译链接、板端运行或室内/室外/夜间实拍验证。

## 修改文件

| 文件 | 作用 |
|---|---|
| samples/cpp/live_record_display/x5_exposure_control.h（新增） | 可测试的曝光 get/set/get 事务、转换校验、分阶段日志与错误传播 |
| samples/cpp/live_record_display/x5_capture.c / x5_capture.h | 实际采集调用事务，按实例保存快照，在两路启动后设置 |
| samples/cpp/live_record_display/native_live.cpp | 启动修订标识，取得并传递快照，澄清增益参数说明 |
| samples/cpp/live_record_display/native_record.h | summary.txt 保存请求、提交、SDK 回读及 unknown 实际值 |
| hvs.py | 真正启动路径/SHA256、回放显示记录信息，兼容旧数据 |
| scripts/live.sh | 修正曝光默认值帮助信息 |
| tools/aps_capabilities.json | 撤回未经确认的增益倍率声明，明确当前验证边界 |
| tests/test_native_exposure.cpp（新增） | 执行实际控制 helper，验证转换、边界、属性保留和三阶段错误 |
| tests/fake_x5_capture.cpp / tests/CMakeLists.txt | 真实 native 参数解析及 profile 顺序、错误退出的集成测试 |
| tests/test_native_record.cpp / tests/test_hvs_live_cli.py | 快照元数据、旧数据 unknown 与参数转发测试 |
| NATIVE_DUAL_VC_LIVE.md / 本文（新增） | 更新证据、部署命令、验收要求和未解决问题 |
