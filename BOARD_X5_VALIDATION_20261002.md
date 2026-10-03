# X5 板端验证记录 — 2026-10-02

实测设备：`root@10.129.113.92`；工程 `/app/shimetapi_hybrid_vision_toolkit`，平台样例 `/app/multimedia_samples`。使用板上原生 Release 构建和实际摄像头，窗口测试使用本机 Xorg `DISPLAY=:0`，不是 SSH 转发窗口。

## 已确认的故障和修复

1. 官方 VIN 配置的 `enable_frame_id` 将 EVS 包首两字节覆盖为帧号。旧 `vin_01` 的首包为 `1d 00`，同会话元数据为 frame_id=29。新录制关闭注入；播放器仅在元数据精确匹配时恢复内存副本，149 个旧包恢复后解码出 6,779,961 个事件、1192 幅空间拼图。源文件未修改。
2. SDK 给旧 AVI 的所有时间戳填零，却标为 valid。现要求 valid 且数值非零，否则使用保存的主机时间桥接。EVS 时间/主机时间斜率实测 0.800004，桥接同时拟合速率、偏移；旧会话拟合残差跨度约 649 微秒。仍有接收延迟，不代表曝光时刻硬件同步。
3. APS 现有 ISP 约 0.6 秒/帧，实时逐帧处理无法达到 30 FPS。保留颜色算法，先顺序准备全部帧后再播放；148 帧 RAM 准备实测 85.7374 秒，缓存 886,920,192 字节。RAM 不足时使用有容量上限、退出自动删除的临时缓存。UI 显示准备进度。
4. 画布 INTER_AREA 缩放基准约 32.14 ms；改为显示用 INTER_LINEAR。EVS 累积使用同语义的 OpenCV 非零掩码复制，播放活动期间移除固定 5 ms sleep。颜色算法、录制数据均未改变。
5. 芯片身份应在官方初始化前检查（0x3428 两字节为 0808）。开流后同地址读到 0202，原来的开流后身份判断会误报。

## 最终回放实测

修复后的 `vin_01` 在板上 960×600 视口完整展示底部按钮和右侧栏。准备时间 85.8844 秒；随后首个约 5 秒统计窗口展示全部 148 个 APS 帧，29.5732 帧/秒，跳过预览 0，最大 compose+imshow 29.1571 ms。末帧 APS index=147、主机相对时间 4,934,528 us，对应 EVS index=1184、主机拟合时间 4,933,052 us。两路速度一致；这些数值不构成硬件曝光同步的证明。统计窗口包括末帧停留，不能拿 29.57 代替采集帧率。

## 采集吞吐与内存

同一 profile=1、sensor_mode=2、config_index=240，APS 1632×1224、RAW10 16-bit 容器 stride=3264，完整 DMA 平面 3,995,136 字节；EVS 1 MiB/包。双路独立取帧，必要 cache invalidate 后各复制一次至预分配 arena，随后归还 VIN 缓冲。停止、join 后才进行封装、转换、写文件和验证。

| 实机会话 | 有效时间 | APS | EVS 包 | APS 间隔 FPS | 说明 |
|---|---:|---:|---:|---:|---|
| codex_final_receive | 5 s | 148 | 149 | 29.792183 | 立即归还，不保留负载 |
| codex_final_copy | 5 s | 148 | 149 | 29.792275 | 单次复制，峰值 747,555,968 B |
| codex_hw_gain0 | 5 s | 149 | 149 | 29.792252 | 完整录制；保存 132.438 s，部分与编译重叠 |
| codex_night_gain0/6/12 | 各 1 s | 各 30 | 各 30 | 约 29.79 | 完整录制、增益读回 |

只接收与复制对照无 VIN timeout/error；复制 APS 最大持有缓冲约 1.21 ms，EVS 约 0.32 ms。EVS 约 31.25 MB/s 是**包字节率**，不是事件率或传感器帧率。

这证明原生双 VIN 支持接近 30 APS 接收帧/秒，不能反推旧闭源 SDK 内部的唯一半帧率原因。旧链路边界是 SDK 回调/应用处理，原生接口对照排除了当前模式仅能输出 15 FPS 的假设。

SDK 帧 ID 全零，已标为 unavailable；VIN `tv` 也全零。另一个 VIN `timestamps` 字段非零，三组短录制各有 30 个不同值，相邻约 33.57 ms（数值尺度符合纳秒，尚不称曝光时间）。三组各 30 个去掉首两字节后的完整 APS 负载 SHA256 均不同，避免用嵌入计数器制造“不同图像”结论。上游传感器漏帧仍不可直接观测。

APS 首个 16-bit 字呈连续计数，可能是嵌入元数据；本次未重新解释或删改原始文件。其余测试像素均在 0..1023。整平面图像布局/首字含义仍须厂商确认，预览保留 layout unverified 标志。

实际 mlock=1，/proc/swaps 无设备。没有修改全局 swap 配置。进程强杀或断电无法恢复纯 RAM 副本。保存过程比采集慢，不能混合统计；12 dB 短会话保存约 24.17 s，负载保存吞吐约 5.97 MiB/s（包含转换、验证、其他输出）。

## 曝光与增益：明确未完成部分

0、6、12 dB 命令均成功，模拟增益寄存器读回分别匹配量化值，数字增益固定 1×，设置后、周期检查、停流前均匹配。0、6、12 dB 三组去掉未确认首字后的 RAW 均值分别 63.816、64.345、65.262，饱和比例分别 0、0、0.084%。现场夜间大部分接近黑电平，变化很小，不能据此宣称强光曝光问题解决。

**曝光行数没有成功**：015B 请求 0x64 后读回 0x00；重复起始与分离读事务结果一致，latch 前后均不保持。VTS 读回也为 0。增益寄存器正常读回，因此不能简单归因于 I2C 总线不通。程序继续对用户显式曝光请求报错，未跳过检查，也未猜测其他寄存器。需要厂商核实当前模式曝光寄存器、使能/写入时机和行时序；不提供假称有效的曝光行数或微秒命令。

没有改变 ISP 算法。绿色偏色尚未校准，已有 `tools/aps_isp_tuner.py` 可用于离线黑电平、灰卡 WB、CCM 和 gamma 调整，需真实 CFA/色卡数据。

## 构建、命令与追溯

```bash
cd /app/shimetapi_hybrid_vision_toolkit
cmake --build out/x5/hvs-build --parallel 2 --target hv_sample_player hv_hvs_record_vin
python3 hvs.py play --output /app/recordings/vin_01 --window-width 960 --window-height 600
# 新录制须选尚未存在的输出目录；以下参数组合已在本板验证
python3 hvs.py record --x5-vin-bypass --storage memory --output /app/recordings/gain0_new --seconds 5 --max-mib 1024 --i2c-bus 6 --i2c-address 0x3c --aps-gain-db 0
# 另一个终端请求正常停止，也可在录制终端 Ctrl+C
python3 hvs.py stop --output /app/recordings/gain0_new
```

运行 hvs.py 会打印实际二进制路径与 hash。需要重新配置时使用 README 的 build 命令，本次部署是同步源文件后在板上重新构建。

实测二进制 SHA256：
- player: `1aae076c019b561cabcd1ad81c983854b71d0dab1fe08b93c870dacc50d4128b`
- recorder: `8824956efd4f8dffaa267df89324e35faa1d4f58e6a940e3b2e2e583cddf6c43`
- 板上传感器库 `/usr/hobot/lib/sensor/libapx003cc.so.1.0.0`: `46d0de5774b90ab8e1e2de010eb1adb87a6da03437e061f9d7829343dd4988e6`

最终另修复缓存满后回跳帧被立即淘汰的问题。板上实际键盘操作验证了播完重播、暂停、单步和 q 退出（exit=0）；单步截图显示 APS=14 / 469927 us，EVS=120 / 469936 us。最终 player 的 `/proc/PID/maps` 确认加载工程 `lib/x5` 下的 codec、core、io 2.0.0；本地保存了 `out/codex_player_inventory.json` 完整指纹。上面的 148 帧性能统计来自加入该缓存修复之前的同一渲染实现，缓存修复后完成短录像控件回归。

## 测试范围

Windows Release CTest 20/20，Python unittest 51/51；覆盖内存预算、缓冲复制所有权、停止/保存/故障恢复、参数透传、曝光换算/增益表/读回错误、EVS 旧头恢复、时间轴速率桥接和 valid-but-zero 时间戳。模拟失败测试不能冒充板上磁盘写满实验。本次没有填满板上真实根分区、没有断电测试、没有获得强光场景多曝光成功对照。

实测日志位于板上 `/tmp/codex_final_receive.log`、`/tmp/codex_final_copy.log`、`/tmp/codex_night_gain{0,6,12}.log`、`/tmp/codex_replay_verified.log`；会话元数据在对应 `/app/recordings/codex_*` 目录。临时日志不是持久交付物，本报告记录关键结果。
