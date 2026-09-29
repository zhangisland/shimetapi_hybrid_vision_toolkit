# APX003CC 板端跟踪（2026-09-29 后续）

此跟踪已在 10.129.113.92 的真实 X5 上执行。原生曝光仍未修复，以下是驱动定位证据，不是成像验收通过。

## 已确认

板端 `/usr/hobot/lib/sensor/libapx003cc.so.1.0.0` SHA256：

`8677acc4d62d791d52ff8f69a10779e2ce826b4f348ef3b9ca17767ae857c861`

与本地 `shimetapi_rdk_x5_v3.4.1_v2.1.img` 中的驱动逐字节哈希一致。库未剥离静态符号，实际文件反汇编可见：

- `sensor_aexp_line_control` 地址 0x1adc：接收 mode 1/2，将 line[0] 限制为 1..1162，把高 4 位及低 8 位写到 0x015A/0x015B；两次写返回值都被忽略，最终直接返回 0。
- `sensor_aexp_gain_control` 地址 0x1a04：接收 mode 1/2，用最大 109 的索引查询 LUT，向 0x0157 写一个字节；不使用传入的 dgain，忽略写返回值并返回 0。
- `sensor_userspace_control` 地址 0x1d74 返回 enable=3；ELF relocation 证明两个回调分别注册到模块对象地址 0x1c560、0x1c558。
- 上述曝光地址、增益地址及 1162 行上限，与 [D-Robotics IMX219 实现](https://github.com/D-Robotics/x5-libcam-sensor/blob/8c4159238e5a0db768d273853e4dce0039522cba/imx219/imx219_utility.c) 相同。这是“APX 驱动疑似沿用错误映射”的强证据，但没有 APX003CC 寄存器手册，不能单凭地址相同断言正确地址是什么。

被动 interposer 只转发原有 camera_reg_i2c_write8 调用，记录参数和原始返回值，不新增写入、不改变参数、不替换传感器库。接口签名来源于 [官方 camera_reg.h](https://github.com/D-Robotics/x5-libcam-inc/blob/main/develop/camera_reg.h)。

| 请求 us | SDK 回读 us | 驱动实际发出的高/低字节 | 行数编码 | I2C 返回 | 运行中中心 Y 均值 |
|---:|---:|---|---:|---|---:|
| 50 | 51.085568 | 00 / 04 | 4 | 0 / 0 | 91.27 |
| 500 | 498.084293 | 00 / 27 | 39 | 0 / 0 | 92.78–93.04 |
| 5000 | 4993.61428 | 01 / 87 | 391 | 0 / 0 | 91.80 |

三档都是 bus=6、16 位寄存器地址、sensor_addr=0x3c。增益请求 1 时 0x0157 写 00，请求 2 时写 80，底层返回 0，后者图像均值约 88.38。这些短时测试未独立确认现场光照恒定，故用于控制路径定位，不作为定量曝光测量。

可以排除这几次运行中的 CLI 丢参、请求全部被夹成同一个行数、曝光回调未调用、I2C API 报错四种解释；不能将 I2C ACK 当成目标寄存器正确或传感器实际执行。跟踪期间没有观察到这些地址的后续写入，不能推广为所有驱动路径均无覆盖。

用户原始三档录像重新解码：500/1000/5000 us 对应 Y=148.635/140.614/148.989。新验证工具报告 `failed: SDK readback changed but no clear brightness increase`，返回码 2；新报告保存在板端 `/app/recordings/exposure_sweep_audit/analysis_driver_followup_20260929`，原录像/报告未覆盖。

## 本次修改与验证

- `tools/exposure_compare.py`：明确成像响应筛查结论；SDK 接受/回读变化不足以通过。只有至少三档、固定 SDK 增益、足够曝光跨度、非严重饱和/黑场且有明显一致亮度响应才返回 0。未通过或证据不足返回 2；执行错误返回 1。阈值为筛查启发式（跨度 4、Y 提升 10、比例 1.2、噪声阈值），报告完整保留；不是线性光学定律或硬件范围。
- `tests/test_exposure_compare.py`：用用户提供的真实三档数据回归，确认不能被回读变化误判通过，另测响应、饱和及增益变化。
- `tools/apx003cc_diagnostics/passive_i2c_trace.c`：被动跟踪现有驱动三处写入，编译为单进程 LD_PRELOAD，不安装或替换系统库。
- `tests/test_passive_i2c_trace.c`：在板端 gcc 构建并运行假 SDK 测试，确认参数不变、错误 -37 原样传递、只产生原本两次调用，无额外写入。
- 本地主机 39 项 Python 测试通过；本轮没有改动 native 采集代码，沿用前轮 17 项 C++ 回归结果。

## 被动跟踪复现

先确保没有其它采集程序运行，使用已构建的 native live。日志仅说明原驱动发出的写操作。

```bash
cd /app/shimetapi_hybrid_vision_toolkit
gcc -Wall -Wextra -shared -fPIC tools/apx003cc_diagnostics/passive_i2c_trace.c -o /tmp/apx_passive_i2c_trace.so -ldl -pthread
LD_PRELOAD=/tmp/apx_passive_i2c_trace.so python3 hvs.py live -- --profile 1 --aps-exposure-us 500 --aps-gain 1 --aps-dgain 1 --aps-correction off --aps-wb off --no-display --seconds 2
```

## 下一步所需的准确资料

板端定向搜索没有找到 apx003cc_utility.c 或 apx003cc_setting.h。公开 ShiMetaPi X5 mini SDK 也只包含配置与链接库，没有该驱动源码。需要与该板端版本匹配的 APX003CC 寄存器资料或厂商修正驱动/源码，才能确定正确曝光字段、位宽、增益 LUT、单位、更新时序和帧周期约束。

尚未替换系统驱动、未修改初始化寄存器表、未凭猜测将写操作重定向到其它地址。已确认的忽略 I2C 错误问题也应在厂商驱动源码中修正，但本次跟踪的 I2C 返回均为 0，因此仅补错误传播不会解决当前无响应。
