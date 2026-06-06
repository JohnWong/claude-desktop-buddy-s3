# StickS3 构建 / 刷机笔记（实测）

## 环境
- 机器：Apple M3 Pro (arm64)，macOS。Rosetta 已装（riscv32 工具链仅 x86_64，靠 Rosetta 跑）。
- PlatformIO Core 6.1.19，装在隔离 venv：`~/.pio-venv`（用 `~/.pio-venv/bin/pio`）。
- 网络需代理：`export https_proxy=http://127.0.0.1:8118 http_proxy=http://127.0.0.1:8118`

## 关键坑：代理损坏大文件
代理上游是 Cloudflare Gateway，会**截断/损坏 >100MB 的包下载**（checksum 不匹配 / IncompleteRead），
PlatformIO 自带下载器无续传，反复换镜像也下不下来。直连 github 不通，但 PlatformIO 的
CDN 镜像（dl.registry.platformio.org / contabostorage）直连可用。

### 解决办法：手动下大包 + 喂给 pio
1. 查权威直链：`https://api.registry.platformio.org/v3/packages/<owner>/tool/<name>?version=<ver>`
   （version 里 `+` 要 URL 编码为 `%2B`）
2. 直连用 axel 多线程下载并校验 sha256：
   `axel -n 16 -a -o <file> <download_url>`
3. 装包：`pio pkg install -g -t "file:///abs/path.tar.gz"`

### framework-arduinoespressif32 的额外坑
- 平台要的是带 build 后缀的精确版本 `3.20017.241212+sha.dcc1105b`（非 `3.20017.0`）。
- `pio pkg install file://` 装出来 `.piopm` 的 `spec.owner=null`，不被平台的
  `platformio/framework-arduinoespressif32` 依赖匹配 → pio 仍重下。
- 修复：手改 `~/.platformio/packages/framework-arduinoespressif32/.piopm` 为 registry 格式：
  `{"type":"tool","name":"framework-arduinoespressif32","version":"3.20017.241212+sha.dcc1105b","spec":{"owner":"platformio","id":8070,"name":"framework-arduinoespressif32","requirements":null,"uri":null}}`

## 手动预置的大包（sha256）
- toolchain-riscv32-esp 8.4.0+2021r2-patch5 darwin_x86_64: d0cea8c9a5a1661e98156d907a3037f9c3f2f46419523be1ad2540c900a7637c
- framework-arduinoespressif32 3.20017.241212+sha.dcc1105b: 7dbcfb86f9dfd5ecf6c881ed8226239d9ef63f27e1e1a2ddd87a87762e2ffbc9

## 编译 / 刷机
```
~/.pio-venv/bin/pio run -e m5stick-s3                       # 编译
~/.pio-venv/bin/pio run -e m5stick-s3 -t upload --upload-port /dev/cu.usbmodem2101
```
- 设备：`/dev/cu.usbmodem2101`，VID:PID 303A:1001（ESP32-S3 原生 USB JTAG/serial），MAC 14:C1:9F:D5:18:D8
- 下载模式：StickS3 无 GPIO-0，插 USB 后长按侧边电源键 3-5s 至绿灯闪
- 实测结果：编译 SUCCESS（Flash 60.2% / RAM 24.6%），刷机 SUCCESS，Hash verified，自动复位

## 待办
- 桌面 App 配对验证（Developer Mode → Open Hardware Buddy → Connect）
- 阶段二：直连 Claude Code CLI（见 STICKS3_PLAN.md）
