# MDC1200 独立研究与 BK4829 移植结论

## 结论

老师的提醒是对的，但需要把“packet value”分成三层：

1. MDC1200 逻辑包：操作码、参数、ID、CRC 和纠错码。
2. 实际空口比特：交织后再做差分/XOR 编码，以 1200/1800 Hz 发送。
3. 射频芯片接口值：写入 `REG_5F` 的 16 位字，以及芯片私有的字节、位序。

前两层必须符合 MDC1200，第三层不属于协议。BK4819 和 BK4829 的第三层
即使数值不同，只要最终空口比特完全相同，仍然兼容；如果只听起来相同、
空口比特不同，就不是可互通的 MDC1200。

本移植不再依赖第三层。发射端直接使用 BK4829 的音频发生器，按 1200 波特
逐符号发送已完成编码的空口比特，因此不会受到 FSK FIFO 装载值差异影响。

## 标准单包结构

单个 PTT-ID 包采用：

- 9M2RTX 原始的 16 位 `00 FF` 短前导；
- 40 位同步字 `07 09 2A 44 6F`；
- 112 位交织有效载荷；
- 有效载荷结束后立即停振，不附加连续 mark 尾段。

菜单 `MDCPre` 增加的是编码器外的原始音调前导。其首轮为一个 `00`
（连续 1800 Hz）和三个 `FF`（连续 1200 Hz），不经过差分编码，因而
第一个音中没有单 bit 翻转。

七字节信息块为：

`op arg idH idL crcL crcH status`

其中 PTT-ID 的 `status` 为 `00`。CRC 是反射形式的 CRC-16/CCITT：多项式
`0x8408`、初值 `0x0000`、末异或 `0xFFFF`，只覆盖前四字节。随后使用
K=7、抽头 `{0,2,5,6}` 的纠错编码，形成 14 字节，再按 16×7 交织。

最后对整个帧从最高位到最低位连续做差分/XOR 编码并反相。编码后的 1 对应
1200 Hz mark，0 对应 1800 Hz space；符号周期是 833.333 微秒。

## 可复现黄金向量

输入：PTT-ID PRE，`op=01`、`arg=80`、ID=`1234`。

- CRC：`3E2E`，包内低字节在前，即 `2E 3E`
- 信息＋FEC：
  `01 80 12 34 2E 3E 00 65 80 A8 62 DD 88 08`
- 交织载荷：
  `8E 94 2A 2E 06 99 22 04 00 1C 18 A2 2C 88`
- 不含 `MDCPre` 附加前导的完整 21 字节空口线码：
  `FF 7F 7B 72 40 99 A7 B6 21 C0 C6 FA 2A 4C F9 FF ED EB 0C C5 33`

可运行 `python tools/verify_mdc1200.py` 独立重算，不依赖固件 CRC 函数。

## BK4829 寄存器资料核查

流传的《BK4829 Registers Table》确实标注：

- `REG_58<15:13>=001`：FFSK 1200/1800 发射；
- `REG_58<12:10>=111`：FFSK 1200/1800 接收；
- `REG_59`：FIFO 清除、收发使能、数据反相、前导及同步长度；
- `REG_5F`：FSK Word Input/Output；
- `REG_71/72`：音频/FSK 频率控制字。

但该表没有说明 `REG_5F` 的 16 位字如何串行化为空口字节和比特，而且 FSK
章节与较早的 BK4819 v3 表几乎逐字相同。因此，不能凭寄存器名字相同就断定
FIFO 的字节序、位序和内部模式配置相同。

F4HWN 旧的 `FSK_RogerTable` 也不能作为 MDC 包参考：它配置的是四字节
`55 55 55 55` 同步，而 MDC1200 必须匹配 40 位 `07 09 2A 44 6F` 同步；
固定七个 FIFO 字只能证明能发出相似声音，不能证明解码器能得到正确 ID。

## 本次实现选择

发射流程现在是：

1. 软件生成逻辑信息、CRC、FEC、交织和差分线码；
2. 使用 48 MHz SysTick，每 40,000 个 CPU 周期切换一个符号；
3. 通过 BK4829 `REG_71` 在 1200 Hz 与 1800 Hz 之间切换；
4. 整个包发送期间保持音调发生器开启，避免用 FIFO 解释数据。

这条路线复用了 F4HWN 已有 APRS 发送器经过验证的计时和音调发生方法。

## 软件接收方案

Dondji 已在同为 PY32F071/BK4829 的 UV-K1/K5-V3 上实现 MDC1200 接收，
而且没有套用 BK4819 的 FSK 寄存器值。它从 PA4 取得鉴频音频，以 TIM3
触发 ADC1 按 14.4 kHz 采样，再经 1469.7 Hz 带通、24 样本滑动相关器和
DPLL 恢复 1200/1800 Hz 比特。当前移植采用了这一方案，所以收发两侧都
绕开了 BK4829 私有的包 FIFO 格式。

接收由静噪门控，仅在 FM 信号打开静噪时采样；发射前会立即停止采样并
恢复电池 ADC 配置。若编译 Voice 功能，PA4 被占用，因此软件 MDC 接收会
自动禁用。

## 尚需实机确认

- 用 SDR 或鉴频音频确认符号周期、mark/space 极性和完整黄金向量；
- 确认切换 `REG_71` 时相位连续性；
- 校准窄带与宽带下的频偏，必要时调整 `MDC_TONE_GAIN`；
- 用两台机器验证弱信号、窄带、宽带和中继尾部情况下的解码率；
- 检查双守候、扫描及长时间接收时的 CPU 负载与电池采样。

## 公开资料

- TrueMDC：<https://github.com/IvanMaestroGTR/TrueMDC-9M2RTX-Losehu-UVK5-Firmware>
- OneOfEleven MDC1200 编解码实现：
  <https://github.com/OneOfEleven/uv-k5-firmware-custom/blob/main/mdc1200.c>
- Matthew Kaufman MDC 编解码库：
  <https://github.com/atmatthewat/mdc-encode-decode>
- EAS Station MDC1200 协议说明：
  <https://kr8mer.github.io/eas-station/reference/protocols/MDC1200/>
- F4HWN BK4829 MDC 接收讨论：
  <https://github.com/armel/uv-k1-k5v3-firmware-custom/discussions/79>
- Dondji 的 K1/K5-V3 软件 MDC 接收：
  <https://github.com/EthanYan6/Dondji>
