# MOSS 语音助手（社团嘉年华展示版）

> 🎯 一个面向学校「社团嘉年华」现场演示的桌面语音助手机器人。用户按一下按钮，说一句话，MOSS 就能识别中文、调用 DeepSeek 大模型理解意图，**安全地打开桌面上的指定文件夹/文件**，并用语音播报回复。
>
> 本仓库为**多人共创**项目：硬件、固件、上位机、文档、测试等方向都欢迎参与。请先读 [如何参与贡献](#-如何参与贡献) 和 [文档索引](#-文档索引)。

> 📦 **当前版本**：`0.1.0` —— 见仓库根目录 [`VERSION`](VERSION) 文件；由 [`scripts/version.py`](scripts/version.py) 自动维护（git 标签驱动，`vX.Y.Z`）。运行 `python scripts/version.py` 即可查看。

---

## 📌 项目简介

| 项 | 内容 |
|---|---|
| 形态 | 桌面机器人（MOSS 3D 打印外壳 + ESP32-C3） |
| 语音链路 | 按键录音 → 本地 ASR 识别 → DeepSeek 理解 + 工具调用 → 安全打开桌面文件 → 本地 TTS 播报 |
| 能力上限 | **仅允许打开/查看**桌面目录下已有的文件/文件夹；一切修改、删除、移动、写入及系统危险命令均被拒绝 |
| 目标日期 | **9 月 9 日**前可现场演示 |
| 当前阶段 | 最小闭环优先（P0 功能），硬件已到货 |

**最小演示闭环**（唯一必须跑通的链路）：

```text
按键录音 → ESP32-C3 采 16kHz PCM → Wi-Fi 上传上位机
  → faster-whisper 中文识别
  → DeepSeek Function Calling（普通问答 或 open_desktop_item）
  → 安全校验路径是否在桌面下 → os.startfile() 打开
  → Windows SAPI TTS 生成 WAV
  → 上位机返回 reply + tts_url → ESP32 下载 → MAX98357A 播放
  → LED 恢复待机
```

---

## 🏗️ 系统架构与数据流

```text
用户说话
   │
   ▼
ESP32-C3（硬件端）
- I2S 麦克风采音（INMP441）
- 录音开始/结束（轻触开关）
- 上传 PCM 音频
- 播放上位机返回的 WAV
- LED / 按钮 / 舵机控制
   │ Wi-Fi HTTP / USB 串口
   ▼
Windows 上位机（电脑端）
- ASR 语音识别（faster-whisper）
- 调用 DeepSeek
- Function Calling 意图解析
- 安全校验 + 打开桌面文件
- TTS 语音合成（pyttsx3）
   │
   ▼
DeepSeek API
- 普通问答
- 将「打开文件夹」等意图转成结构化工具调用
```

**为什么 ASR 放在电脑端而非 ESP32？**

1. ESP32-C3 的 RAM、算力和 Flash 有限，跑不起高质量中文 ASR。
2. 电脑可运行 `faster-whisper`/Vosk 本地模型，**现场断网也能识别**。
3. 电脑更适合处理 WAV、模型推理、缓存与异常重试。
4. ESP32 只负责实时音频采集与播放，固件复杂度低，几天内更容易跑通。

**通信方式（按优先级）**

1. Wi-Fi HTTP：ESP32 向电脑上传 PCM，电脑返回 JSON 和 TTS 地址（首选）。
2. USB 串口备用：传输状态、文本命令或小段音频。
3. 电脑与 ESP32 必须同局域网，电脑设固定 IP 或使用 mDNS。推荐上位机地址：`http://192.168.1.100:8765`。

---

## 🧰 硬件清单

### 演示必需（已到货 ✅）

| 部件 | 型号/规格 | 数量 | 接线要点 | 参考价 |
|---|---|---:|---|---:|
| 主控 | ESP32-C3-DevKitM-1 | 1 | USB 供电和烧录 | 25~45 |
| 麦克风 | **INMP441 全向麦克风模块** | 1 | SCK/WS/SD/3.3V/GND | 11.81 ✅ |
| 功放 | **MAX98357 I2S 音频放大器模块（已焊排针）** | 1 | DIN/BCLK/LRC/VIN/GND | 13.06 ✅ |
| 扬声器 | **小喇叭 5W 3寸全频** | 1 | 接 MAX98357A 输出 +/− | 12.77 ✅ |
| 按钮 | **轻触开关（10 种常用直插包）** | 1 | GPIO 上拉，按下录音 | 8.54 ✅ |
| 状态灯 | **16 位 WS2812 5050 RGB 灯环** | 1 | 待机/录音/联网/报警状态 | 6.6 ✅ |
| 电源 | USB 移动电源 / 5V 2A 适配器 | 1 | 最稳定，先不做锂电池 | 20~50 |
| 连线 | 杜邦线、排针、洞洞板 | 若干 | 电源线独立走线 | 10~20 |

**必需硬件合计：约 85~175 元（含已到货）。**

### 可选增强（后续迭代）

| 部件 | 型号 | 用途 | 参考价 |
|---|---|---|---:|
| 舵机 | SG90 | 头部左右转动 | 8~15 |
| 舵机驱动板 | PCA9685 | 多路舵机控制 | 15~25 |
| OLED | 0.96" SSD1306 I2C | 显示「聆听中/思考中」等 | 10~20 |
| 眼睛灯 | 5mm LED / WS2812灯环 | 增加视觉感 | 5~20 |
| 锂电池模块 | 18650 + TP4056 + 升压 | 脱离 USB 使用 | 30~60 |
| 麦克风阵列 | INMP441 双麦 | 改善远场收音 | 20~50 |

> ⚠️ 按「最小闭环优先」原则：P0 只保证「录音→识别→回答→播报」；屏幕、唤醒词、舵机等靠后，9/8 之后不再新增复杂功能。

---

## 🔌 接线与端口定义（ESP32-C3）

> 所有 GPIO 以**实际开发板丝印/原理图为准**，接线前用万用表核对。已避开启动/下载引脚（GPIO0=BOOT、GPIO2=strapping、GPIO18/19=USB）。

### GPIO 分配总表

| 信号 | GPIO | 方向 | 连接对象 | 备注 |
|---|---|---:|---|---|
| I2S BCLK | **GPIO4** | OUT | 麦克风 SCK **和** 功放 BCLK 共用 | 一根线分叉到两器件 |
| I2S WS / LRCLK | **GPIO5** | OUT | 麦克风 WS **和** 功放 LRC 共用 | 一根线分叉到两器件 |
| I2S 麦克风数据 | **GPIO6** | IN | INMP441 SD → ESP32 | 录音输入 |
| I2S 功放数据 | **GPIO7** | OUT | ESP32 → MAX98357A DIN | 播放输出 |
| 录音按钮 | **GPIO1** | IN | 轻触开关一端 GPIO1，另一端 GND | 内部上拉，按下 LOW |
| WS2812 灯环数据 | **GPIO10** | OUT | WS2812 灯环 DIN | 状态指示 |
| （备用）舵机 PWM | GPIO3 | OUT | 后续可接 SG90 | 本轮可不接 |
| （备用）OLED | 预留 | — | 后续可选 | 本轮不做 |

### 各器件速查

- **麦克风 INMP441**：VDD→3.3V，GND→GND(共地)，SCK→GPIO4，WS→GPIO5，SD→GPIO6，**L/R→GND**（左声道）。
- **功放 MAX98357A**：VIN→5V，GND→共地，DIN→GPIO7，BCLK→GPIO4，LRC→GPIO5，**SD→VIN**（使能），**GAIN→悬空/GND**。
- **扬声器（5W 3寸全频）**：单声道 BTL 输出，两根线直连功放 +/−，**不需接 GND**；与麦克风保持距离防啸叫。
- **WS2812 灯环（16 位）**：5V→供电，GND→共地，DIN→GPIO10，DOUT 不接。
- **轻触开关**：4 脚对角两组相连，一组脚接 GPIO1，另一组脚接 GND。

### 供电与共地（重点）

1. **所有器件必须共地**，否则 I2S 数据异常。
2. **3.3V**：INMP441 VDD（电流很小）；**5V（VBUS）**：MAX98357A VIN 与 WS2812 5V。
3. 功放/灯环**不要**从 ESP32 的 3.3V 引脚取电（电流会超）；整机建议 **5V 2A**，灯环控制亮度避免供电不足。
4. **逻辑阈值兜底**：MAX98357A 与 WS2812 若用 5V 供电，其数据高阈值约 0.7×VDD≈3.5V，略高于 ESP32 的 3.3V。多数可直连工作；若**无声/不亮**，把这两件的电源降到 **3.3V** 即可（音量/亮度略降但信号稳）。

> 📄 完整接线说明（含上电自检清单、易踩坑）见 [`docs/接线方案与端口定义.md`](docs/接线方案与端口定义.md)。
> 📷 完整接线示意图（按实购硬件绘制，含供电/共地/注意事项）见 [`docs/接线图-ESP32C3-MOSS.png`](docs/接线图-ESP32C3-MOSS.png)。

---

## 🛠️ ESP32-C3 固件

- 开发环境：**Arduino IDE 2.x** + `esp32 by Espressif Systems`，板卡选 `ESP32C3 Dev Module`，库：`ArduinoJson`、`Adafruit NeoPixel`。
- 功能：Wi-Fi 连接 → 按钮触发 4 秒录音 → I2S 采 16kHz PCM → HTTP 上传 → 接收 JSON → 下载 WAV 播放 → LED 状态。
- 源码：见 [`firmware/esp32c3/moss_firmware.ino`](firmware/esp32c3/moss_firmware.ino)。
- 说明：ESP32-C3 只有 1 个 I2S0，录音/播放共用 BCLK/WS，切换到不同数据 GPIO（GPIO6 进 / GPIO7 出）。

---

## 💻 电脑端上位机

- 依赖：`py -3.11 -m venv .venv` → `pip install fastapi uvicorn python-multipart openai faster-whisper pyttsx3`
- 环境变量：`$env:DEEPSEEK_API_KEY = "你的 Key"`
- 启动：`.venv\Scripts\activate` → `python server.py`
- 源码：见 [`server/server.py`](server/server.py)。
- ESP32 与电脑端的请求、响应、TTS 和联调约定：见 [`docs/软件接口协议.md`](docs/软件接口协议.md)。

### ASR 选型

| 方案 | 优点 | 缺点 | 建议 |
|---|---|---|---|
| `faster-whisper base/small` 本地 | 无单次费用、断网可用、中文好 | 首次下载模型、CPU 有延迟 | **首选** |
| Vosk 中文模型 | 轻量、启动快、离线 | 连续语音准确率一般 | 备用 |
| 云端 ASR | 识别成熟 | 依赖网络/账号 | 备用 |
| Windows SAPI | 无需额外服务 | 中文语音包不一定装 | 应急 |

建议：预先下载 `faster-whisper base` 模型；录音固定 **16kHz/单声道/16bit PCM**；演示用 USB 麦克风与 ESP32 麦克风**二选一**避免双重回声。

### TTS 选型

| 方案 | 优点 | 缺点 | 建议 |
|---|---|---|---|
| Windows SAPI + pyttsx3 | 本地、免费、稳定 | 音色普通 | **首选** |
| Edge TTS | 中文音色自然 | 依赖网络 | 可选 |
| 云端 TTS | 音质最好 | 网络/费用 | 不作唯一方案 |

---

## 🔒 安全机制（三层必须同时存在）

1. **模型层限制**：系统提示词 + Function Calling 只暴露 `open_desktop_item` 一个工具。
2. **参数层限制**：拒绝绝对路径、`..`、UNC 路径、命令字符、桌面外路径。
3. **执行层限制**：只调用 `os.startfile()`，禁止 `subprocess`/`os.system`/PowerShell/cmd。

**禁止实现**：删除、写入、修改、移动、复制、重命名、下载、安装、运行任意程序、执行系统命令、关机、格式化、注册表修改、打开桌面以外路径。

> 安全边界是**红线**：任何人提交的 PR 若绕过以上任何一层（例如引入 subprocess 打开任意文件），将被直接拒绝。

---

## ⏱️ 演示功能与里程碑

### 最终演示功能（P0 优先）

| 优先级 | 功能 | 验收标准 |
|---|---|---|
| P0 | 按键录音 | 按下按钮录音 4 秒 |
| P0 | 中文 ASR | 识别「打开桌面的社团嘉年华文件夹」 |
| P0 | DeepSeek 普通问答 | 答 3~5 个预设问题 |
| P0 | 打开桌面文件夹 | 只打开白名单内目录 |
| P0 | 语音回复 | ESP32 扬声器播报结果 |
| P1 | LED 状态 | 待机/录音/思考/错误四态 |
| P1 | 头部舵机 | 回答时左右转动 |
| P2 | OLED 屏幕 | 显示识别文本/状态 |
| P2 | 唤醒词 | 「你好 MOSS」后录音 |

### 冲刺计划

| 日期 | 任务 | 交付物 |
|---|---|---|
| 9/5 | 硬件到货核对；Arduino 烧录 Blink；装 Python 环境 | 硬件通电、ESP32 联网 |
| 9/6 | ESP32 录音上传；电脑 ASR；建桌面演示文件夹 | 能看到识别文本 |
| 9/7 | DeepSeek Function Calling；路径白名单；os.startfile | 语音打开指定文件夹 |
| 9/8 | TTS；连测 30 次；离线固定话术与备用操作 | 端到端闭环稳定 |
| 9/9 | 现场部署、线路固定、备用电源与备用 ESP32 | 可连续演示 2 小时 |

---

## ⚠️ 风险与备用方案

- **API 延迟**：超时 12s、temperature 0.1、常见问题本地缓存；准备固定问答。若 DeepSeek 不可用，进入本地关键词模式（含「打开」+「社团嘉年华」则打开固定目录）。
- **网络中断**：本地 ASR（faster-whisper）+ 本地 TTS（SAPI）；准备手机热点和网线。
- **识别不准**：按键录音比持续监听可靠；录音前 LED 亮起提示开始说；统一演示话术；USB 麦克风备用。
- **电源/机械**：首次用 USB 移动电源，不用锂电池；舵机单独供电并共地；热熔胶/扎带固定线缆；备好备用 USB 线、开发板、喇叭、按键。
- **异常兜底**（详见 `docs/MOSS语音助手方案.md`）：Wi-Fi 断→LED 快闪+手机热点；TTS 失败→电脑扬声器直接播；ESP32 断线→电脑网页/键盘模拟。

---

## 🚀 快速开始

```powershell
# 1) 电脑端环境
py -3.11 -m venv .venv
.venv\Scripts\activate
pip install fastapi uvicorn python-multipart openai faster-whisper pyttsx3
$env:DEEPSEEK_API_KEY = "你的 Key"

# 2) 启动上位机
python server/server.py
# 监听 0.0.0.0:8765，确保防火墙放行

# 3) 固件
#    Arduino IDE 打开 firmware/esp32c3/moss_firmware.ino
#    填好 WIFI_SSID / WIFI_PASS / SERVER_URL，选 COM 口烧录
```

---

## 🤝 如何参与贡献

这是一个**多人共创**仓库，非常欢迎各种贡献。请先了解当前架构与安全红线，再选一个方向入手。

**适合新贡献者的方向（Good first issues）**

- 🤖 固件：I2S 采集稳定化、WAV 头解析、双缓冲、断线重连、GPIO 可按手头板子适配。
- 💻 上位机：ASR 模型选择/参数调优、TTS 音色、请求缓存、超时重试、日志。
- 🔒 安全：白名单扩展、危险指令拦截测试、越权用例（路径穿越/绝对路径/命令注入）。
- 🧪 测试：端到端脚本、30 连测、异常注入、离线关键词模式。
- 📄 文档/硬件：接线图、外壳开孔建议、采购清单、FAQ。

**提交规范**

1. 先开 `Issue` 说明要做的改动与验收标准，认领后再动手。
2. 分支：`feat/<功能>`, `fix/<bug>`, `docs/<主题>`。
3. Commi `feat:` / `fix:` / `docs:` / `refactor:` / `test:` 前缀，中文或英文均可，说明做了什么。
4. **PR 必须附**：改动说明、验证结果（如固件烧录截图、连测记录）、是否触及安全边界。
5. 安全相关改动（涉及文件打开/执行权限）需至少 1 位 reviewer 通过。

> 详细协作约定见 [`CONTRIBUTING.md`](CONTRIBUTING.md)。

---

## 📄 文档索引

| 文档 | 说明 |
|---|---|
| [`README.md`](README.md) | 本文件：项目入口与汇总 |
| [`docs/MOSS语音助手方案.md`](docs/MOSS语音助手方案.md) | 完整开发方案（含代码模板、排期、风险） |
| [`docs/接线方案与端口定义.md`](docs/接线方案与端口定义.md) | 已购硬件接线与 GPIO 映射 |
| [`docs/接线图-ESP32C3-MOSS.png`](docs/接线图-ESP32C3-MOSS.png) | ESP32-C3 完整接线示意图（信号颜色图例、供电/共地/注意事项） |
| [`docs/软件接口协议.md`](docs/软件接口协议.md) | ESP32 与电脑端上位机的 HTTP、音频、TTS、安全和联调协议 |
| [`docs/AI开发提示词-MOSS语音助手项目.md`](docs/AI开发提示词-MOSS语音助手项目.md) | 生成方案的原始提示词（需求背景） |
| [`firmware/esp32c3/moss_firmware.ino`](firmware/esp32c3/moss_firmware.ino) | ESP32-C3 固件源码 |
| [`server/server.py`](server/server.py) | 电脑端上位机源码 |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | 多人协作规范 |
| [`AGENTS.md`](AGENTS.md) | 给其他 AI Agent 的协作 / GitHub 推送 / 文档管理约定 |
| [`VERSION`](VERSION) | 当前版本（语义化版本，`scripts/version.py` 维护） |
| [`scripts/version.py`](scripts/version.py) | 版本管理脚本（查看 / 写入 / bump / 打标签） |

---

## 📜 许可

建议采用 **MIT License**（`LICENSE` 已提供）。若你想限制二次商用或要求署名，可在 `LICENSE` 中改为 `Apache-2.0` 或 `CC BY-NC` 等，并在 issue 中告知。

---

<!-- 
原始三份文档内容已合并至本文；如需追溯最初版本，见 docs/ 下对应原始文件。
-->
