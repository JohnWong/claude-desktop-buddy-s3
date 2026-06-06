# StickS3 上运行 Claude Buddy — 开发计划

本仓库基线 fork 自 [openalchemy/claude-desktop-buddy-s3](https://github.com/openalchemy/claude-desktop-buddy-s3)
（其又 fork 自官方 [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)）。

目标硬件：**M5Stack StickS3**（ESP32-S3，8MB Flash / 8MB PSRAM，1.14" 240×135 屏，BMI270 IMU）。

## 两阶段路线

### 阶段一（当前）：基于 Desktop App 的 StickS3 版
- 走官方 BLE 协议（Nordic UART Service），通过 **Claude Desktop App** 的
  `Developer → Open Hardware Buddy…` 桥接。
- 基线已是验证过的 StickS3 移植，预计代码改动极小，重点是：
  1. 安装 PlatformIO Core
  2. `pio run -e m5stick-s3` 编译
  3. 进下载模式刷机（StickS3 无 GPIO-0：插 USB 后长按侧边电源键 3-5s 至绿灯闪）
  4. Desktop App 开 Developer Mode → 配对 → 验证七种状态 + 审批按键
- **验收标准**：设备能配对、显示审批提示、A 同意 / B 拒绝生效、宠物状态随会话切换。

### 阶段二（验证通过后）：直连 Claude Code CLI（不依赖 Desktop App）
- 参考 [srokaw/terminal-claude-code-buddy-m5stack](https://github.com/srokaw/terminal-claude-code-buddy-m5stack)：
  Claude Code hooks → 本地 Python BLE 桥（bleak）→ 设备。
- 关键工作：把 srokaw 的主机端桥（与板型无关）接到本固件，对齐 JSON 协议字段
  （srokaw 扩展了 multi-select question 等，需与本固件 `data.h` schema diff）。

## Git 流程
- `upstream` → openalchemy 基线，便于后续 rebase 取更新。
- `main` → 跟踪 upstream 的干净基线。
- `sticks3-desktop` → 阶段一开发分支（当前）。
- 阶段二预计新开 `cli-direct` 分支。

## 环境备注
- GitHub 直连超时，需走代理：`export https_proxy=http://127.0.0.1:8118 http_proxy=http://127.0.0.1:8118`
- PlatformIO 尚未安装（刷机阶段需要）。
