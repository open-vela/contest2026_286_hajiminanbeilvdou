# 黄山派语音主动 Agent（Huangshan Pi Voice-First Agent）

## 一、作品简介

一个跑在黄山派（SiFli SF32LB52）上的**端云协同语音 Agent**：
对着板子说话，设备本地理解、云端推理、屏幕用中文回你；同时它还会**按计划主动巡检设备状态**，
不需要任何人开口。

- **说一句话就能下命令**：设备麦克风采集 → PC 端离线 ASR（sherpa-onnx Paraformer）→
  命令路由 → LLM 理解 → 设备执行（查时间 / 设定时任务 / 读内存 / 上屏）
- **主动巡检**：定时唤醒 agent，自动执行巡检并把结果推上屏幕与 PC 端，无需唤醒词
- **屏上有中文**：自研 LVGL 聊天 UI + 内置 GB2312 中文字体（openvela 自带字体无 CJK 字形）
- **零 WiFi 依赖**：板载无 WiFi，网络走 PPP over 串口；PC 端做 NAT 网关与 LLM 代理

亮点在于**把一条不可能的组合跑通了**：无网卡设备 + 串口 12.7 KB/s 的实际带宽预算下，
用 G.711 μ-law 把 16 kHz 音频压到 8 KB/s 无损上行，配合 PC 端零拷贝解码与离线识别，
实现了「按住说话 → 秒级上屏 → 设备执行」的完整闭环，并连续运行 6 分钟零掉线。

## 二、选题方向

**AI 硬件产品创新**（主）+ 新硬件平台适配（辅，点亮了 openvela 从未跑通的 AUDCODEC 录音路径）。

选题理由：赛题要求「能主动、会执行」——本作品两条都做到了，且执行链路完整落在 openvela 设备上：
语音是交互入口（不是聊天机器人），定时巡检是主动能力（不是被动问答），
屏幕执行结果是可观察的硬件输出（不是纯软件演示）。

## 三、目录结构

```
app/agent_voice/        应用层入口（manifest 映射到 packages/demos/）
board/sf32lb52_agent/   板级 defconfig（录音 / LVGL UI / 工具集 / 网络 全部开关）
src/ai_agent/           自研源文件：LVGL 聊天 UI、麦克风上行 mic_stream、
                        cmd_mic 调试命令、GB2312 中文字体
src/ai_agent/docs/sidecar/          既有文件的"改动后"版本（免打补丁即可阅读）
src/vendor_sifli/       AUDCODEC ADC 录音驱动（openvela 全树首个可用的录音实现）
src/apps_pppd/          pppd 直连（空 modem）改动后的源文件
docs/upstream/          ★ 部署说明 + 三个补丁（复现本作品从这里开始）
docs/*.md|txt           设计与开发计划文档
tools/host/             PC 端完整工具链：串口桥、PPP 服务端、LLM 代理、
                        语音桥、看门狗、诊断脚本（共 37 个）
logs/Oliweitz/          AI Coding 全量对话日志
```

上游公共仓（packages/ai_agent、vendor/sifli、apps）的改动**不在本仓直接修改**，
以 `docs/upstream/patches/` 下的三个补丁提交，同时按赛事要求另为各公共仓准备 PR。

## 四、运行方式

### 4.1 环境

| 硬件 | 说明 |
|---|---|
| 黄山派开发板（SF32LB52-LCHSPI-ULP） | 板载 MEMS 麦克风 + 圆屏 + CH340 串口 |
| PC（Linux / WSL2 Ubuntu） | 本作品的"云侧"：ASR、LLM 代理、PPP 服务端 |
| USB 数据线 | 唯一连接：串口既是控制台/烧录口，也是 PPP 网络链路 |

### 4.2 编译与烧录

```bash
# 1) 拉取 openvela 全量工程（赛题提供），应用三个补丁
cd <工作树>/packages/ai_agent && git apply <本仓>/docs/upstream/patches/01-ai_agent.patch
cd <工作树>/vendor/sifli     && git apply <本仓>/docs/upstream/patches/02-vendor_sifli.patch
cd <工作树>/apps             && git apply <本仓>/docs/upstream/patches/03-apps-pppd.patch

# 2) 编译 agent 配置
cd <工作树>
./build.sh vendor/sifli/boards/sf32lb52/sf32lb52_lchspi_ulp/configs/agent \
  -e "-Wno-return-mismatch -Wno-implicit-function-declaration" --cmake -j8
# 产物: cmake_out/sf32lb52_lchspi_ulp_agent/nuttx.bin
```

⚠️ 修改 defconfig 后必须 `rm -rf cmake_out/sf32lb52_lchspi_ulp_agent` 再编译，
否则配置阶段不会重跑、宏静默不生效（详见 docs/upstream/README.md）。

烧录（Windows 侧，板载 CH340）：先以 RTS 脉冲复位进入 ROM bootloader 的 2 秒窗口，
再 `sftool --before no_reset` 烧入。仅 USB 供电时若屏幕不亮，请换 5V/2A 电源。

### 4.3 启动（PC 一键，全部软件触发，无需按键）

```bash
# 0) 一次性：把 tools/host/ 下的脚本拷到 WSL 家目录，建好语音 Python 环境
cp <本仓>/tools/host/* ~/ && cd ~ && python3 -m venv voiceenv
~/voiceenv/bin/pip install sherpa-onnx numpy websockets pyserial

# 1) 串口透传进 WSL（Windows 管理员 PowerShell，一次性 bind 后每次 attach）
#    usbipd attach --wsl --busid <你的CH340总线号>

# 2) 起串口桥（必须先于复位板子：桥独占 CH340，避免打开串口时的 RTS 抖动复位）
bash ~/bridge_restart.sh

# 3) 一键拉起全链路 + 自检（复位板子 → 无头 agent → pppd → PC 侧 PPP/NAT/LLM → 语音桥）
bash ~/voice_ready.sh
```

看到 `READY` 即全链路就绪（自检包含：NSH 起来 → PPP 通 → agent 端口可达 → LLM 配置下发 → 音频流心跳）。
LLM 通道默认走本地 mock（离线可演示）；要接真实 API：

```bash
bash ~/setup_llm.sh        # 交互式填写 API 地址/Key，只存在 PC 本地，不进任何提交物
bash ~/proxy_restart.sh    # 设备走明文 HTTP 到 PC，PC 转 HTTPS 出网
```

### 4.4 使用

**a) 语音（主打）**——按住手表上的 PTT 按键说话，松开即识别。示例：

| 说什么 | 发生什么 |
|---|---|
| 现在几点了 | 设备报时（已修正 RTC 时钟偏移） |
| 请每两分钟巡检一次设备 | 建立定时任务，到点自动巡检并推送上屏 |
| 看一下设备的内存 | 读取内存占用，中文结果显示在屏幕聊天区 |

**b) WebSocket / REST**（PC 脚本）：

```bash
python3 ~/ws_chat.py "你好"          # 文本直发 agent
curl http://192.168.223.2:28789/api/config   # 查看/下发 LLM 配置
```

**c) 主动能力**——不碰设备，等待定时任务触发即可在屏幕上看到巡检结果。

**网络拓扑**（PPP over 串口）：

```
设备 192.168.223.2  ──── CH340/USB ────  PC 192.168.223.1
  agent: WS+REST 28789                     pppd 服务端 + NAT + LLM 代理
```

### 4.5 常见问题

- 链路断了：`bash ~/pipeline_watchdog.sh` 自动检测恢复；重来一遍 `voice_ready.sh`
- 重启后 LLM 配置丢失：正常现象（`/data` 是 tmpfs），`voice_ready.sh` 会自动重新下发
- 想看设备日志：控制台已让给 PPP（RAMLOG 方案），用 `run_shell dmesg` 或 REST `GET /api/logs`
- 串口速率实测只有 ~12.7 KB/s：这是 CH340+USB/IP 的物理上限，音频码率已按此预算设计

## 五、AI Coding 使用说明

本作品全程用 Claude Code 开发（赛事规定时间内完成于 2026-09-12 ~ 09-16 共 5 天）：

- **需求拆解 / 方案设计**：用 AI 对比「设备直连 LLM vs PC 代理」「有无 WiFi 的可行路径」，
  最终定下 PPP-over-串口 + PC 代理的架构
- **编码**：驱动（AUDCODEC 录音）、UI（LVGL 中文字体）、网络（PPP/WS）几乎全部由 AI 生成初稿后人工验证修改
- **调试**：最有价值的部分——用 AI 做根因定位，把「看起来是死机」的现象逐层缩小到
  HAL 里一个被注释掉的调用点；期间挖出 23 处上游平台缺陷与 5 处配置问题
- **文档**：本 README、部署说明、PR 描述草稿均由 AI 起草

**完整对话日志见 `logs/Oliweitz/`**（5 天，含全部思考过程与工具调用）。
日志由组委会采集器自动落盘、手工提交，未做任何修改。
