# 实体交通灯接线与集成文档

StickS3 经 PbHub 驱动 3 个交通灯模块,作为屏幕"会话条"的外置版,并被 REACT
反应竞速游戏复用为发车灯。本文件记录**经硬件自检验证过**的接线方式、寄存器、
固件映射与安全事项,供复现与排障。

相关代码:`src/trafficlight.h`、`src/game.h`(REACT)、`src/main.cpp`。

---

## 1. 拓扑

```
StickS3 (M5StickC Plus S3)
   │  Grove A 口 / HY2.0-4P,I2C 主机(防呆,直插)
   ▼
PbHub v1.1 (I2C 从机, 地址 0x61)
   │  6 个 Port.B 通道 CH0~CH5,每通道 2 路数字 IO(IO0 / IO1)
   ▼
交通灯 ×3(每个 4 针:GND / RE / YE / GR,共阴,拉高点亮)
```

- 9 路信号(3 灯 × 红黄绿)分布在 **5 个通道**上,**避开 CH3**(不好插线)。
- 每通道用满 2 路数字输出(IO0 + IO1),5 通道 = 10 路,占 9 路,**CH5·IO1 空、CH3 空**。

---

## 2. 物料(BOM)

| 件 | 说明 |
|---|---|
| StickS3 | M5StickC Plus S3(ESP32-S3,8MB/8MB,135×240,BMI270) |
| PbHub | Unit PbHub v1.1(STM32F030,I2C 0x61,固件支持数字读写/PWM/WS2812) |
| 交通灯 ×3 | 普通 4 针共阴模块(GND/RE/YE/GR),每路一颗 LED |
| Grove 线 ×1 | StickS3↔PbHub,HY2.0-4P,防呆直插 |
| 杜邦线 | PbHub 通道口 ↔ 交通灯排针 |

---

## 3. 连接一:StickS3 ↔ PbHub(I2C)

一根标准 Grove 线直插,防呆,不会接错。

| 线色 | 信号 |
|---|---|
| 黑 | GND |
| 红 | 5V |
| 黄 | SDA |
| 白 | SCL |

- 固件用 M5Unified 的外部 I2C `M5.Ex_I2C`(自动取 Grove 口 GPIO,**勿硬编 32/33**)。
- PbHub 地址 **0x61**(可经 A0~A2 焊点改 0x61~0x68)。

---

## 4. 连接二:PbHub 通道 ↔ 3 个交通灯(**权威表**)

下表是**最终验证过**的映射(以 PbHub 通道 + IO 索引为准 —— 这是固件 `TL_LAMP`
的真实对应,不受"黄/白线哪根是 IO0"的标注歧义影响)。照此连线即可直接工作。

| 交通灯 | RE(红) | YE(黄) | GR(绿) |
|---|---|---|---|
| **灯1**(模块0) | CH0 · IO1 | CH0 · IO0 | CH1 · IO1 |
| **灯2**(模块1) | CH1 · IO0 | CH2 · IO1 | CH2 · IO0 |
| **灯3**(模块2) | CH4 · IO1 | CH4 · IO0 | CH5 · IO1 |

- **GND**:3 个模块的 GND 全部并接,接任一已用通道的 GND(黑线)即可,共地。
- **5V(红线)全部不接**:模块只用 GND + 3 路信号,信号由 IO 脚 3.3V 经限流点亮。
- **CH3 整条空闲;CH5·IO0 空闲。**

> 备注:本机布线时,每个通道的两根信号线相对"IO0=黄线/IO1=白线"的惯例是
> **对调**接的(自检发现每通道一致地反),所以上表看起来"红走 IO1、黄走 IO0"。
> 重新布线时**以本表的"通道·IO 索引"为准**,无论实际用哪根线插。

固件侧映射(`src/trafficlight.h`,逻辑灯 0..8 = 灯1R/Y/G、灯2R/Y/G、灯3R/Y/G):

```c
static const TLPin TL_LAMP[9] = {
  {0, 1}, {0, 0}, {1, 1},   // 灯1 R Y G
  {1, 0}, {2, 1}, {2, 0},   // 灯2 R Y G
  {4, 1}, {4, 0}, {5, 1},   // 灯3 R Y G   (CH3 避开)
};
```

---

## 5. PbHub I2C 寄存器(自行驱动时参考)

驱动单路 = 一次寄存器写:`writeRegister8(0x61, reg, 0|1)`(0 灭 / 1 亮)。

```
reg = (0x40 + 0x10 * chTable[ch]) + io     // chTable = {0,1,2,3,4,6}
```

| 通道 | 基址 | IO0 | IO1 |
|---|---|---|---|
| CH0 | 0x40 | 0x40 | 0x41 |
| CH1 | 0x50 | 0x50 | 0x51 |
| CH2 | 0x60 | 0x60 | 0x61 |
| CH3 | 0x70 | 0x70 | 0x71 |
| CH4 | 0x80 | 0x80 | 0x81 |
| CH5 | 0xA0 | 0xA0 | 0xA1 |

> 注意 CH5 基址是 **0xA0**(chTable[5]=6,不是 0x90)。寄存器字节与设备地址 0x61
> 数值相同纯属巧合(0x61 是寄存器,不是 I2C 地址),无冲突。
> 来源:核对 `m5stack/M5Unit-HUB` 的 `unit_PbHub.cpp`(`writeDigital0/1`)。

---

## 6. 固件集成

- **状态镜像**(`tlUpdate`):把 `TamaState.sessState[0..2]` 映射到 3 个模块——
  `1 运行=绿 / 2 等输入=黄 / 3 等批准=红 / 0 空闲=灭`,仅在状态变化时写 I2C。
- **自检**:菜单 `GAMES` 旁的 **`lights`** 项 → `tlSelfTest()`:先探测 0x61
  并显示 SDA/SCL,再逐颗点亮 9 路(屏幕同步显示 `L1 RED…L3 GRN`)验线。
- **REACT 游戏**:复用 3 个模块当发车灯(红逐个亮 → 熄红亮绿=GO)。游戏期间
  `tlUpdate` 让位(`main.cpp` 中 `if (!gameActive) tlUpdate(tama)`),退出后
  `tlResync()` 让镜像重绘。
- **无 PbHub 时**:`tlBegin()` 探测失败 → 全程 no-op,屏幕功能不受影响。

---

## 7. 防烧器件检查表

1. 交通灯模块若**无板载限流电阻**,每路串 **220~330Ω**(3.3V 驱动);不确定先加 330Ω。
2. PbHub 输出脚是 STM32 推挽口,单脚电流压在 **≤10mA**(330Ω@3.3V≈4mA,安全)。
3. **必须共地**:模块 GND 接到 PbHub 通道 GND。
4. **别把信号线插到红线(5V)**:本接法 5V 完全不用,误接 5V 到模块信号脚会烧模块。
5. StickS3↔PbHub 是防呆 Grove,放心;**勿用杜邦线手工对接绕过防呆**,以免 5V 错位。
6. 共阴模块**拉高点亮**;若整体反相(该亮不亮),说明是共阳,把固件 `tlSet` 的 0/1 对调。

---

## 8. 刷机与 bridge(运维)

- 刷机:`source ~/.pio-venv/bin/activate && pio run -e m5stick-s3 -t upload`。
- StickS3 无 GPIO-0;若自动复位失败、串口消失,**插 USB 长按侧面电源键 3~5s 至绿灯闪**进下载模式。
- bridge(`bridge/buddy_bridge.py`)已做 **BLE 掉线自愈**:连接掉了即退出进程,
  launchd(KeepAlive)5s 内拉起新进程重连,**无需手动 kickstart**;约 10~15s 自动恢复。
- 手动重启(需要时):`launchctl kickstart -k gui/$(id -u)/com.claude-buddy.bridge`
