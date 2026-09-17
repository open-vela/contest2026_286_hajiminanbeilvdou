# 参赛项目开发计划：《随身 AI 腕上管家 —— 基于 openvela + ai_agent 的主动型 AI Agent 应用》

> 目标平台：立创·黄山派（SF32LB52-MOD-1-N16R8，手表原型机）
> 技术栈：openvela（`dev-ai-contest-2026` 分支）+ ai_agent 框架
> 赛题：2026 openvela AI 硬件开发者大赛 ——「能主动、会执行」的嵌入式 AI Agent 应用
> 计划编制日期：2026-08-20（作品提交截止 2026-09-20）

---

## 1. 赛题理解与应对策略

**赛题核心**：不是又一个聊天机器人——Agent 必须能**主动**（定时/阈值/事件/上下文触发）且**会执行**（Tool/Shell 调用、写文件、发通知、读传感器）。

| 赛题基础要求 | 本方案对应设计 |
|---|---|
| 固件跑起来 | 官方板级 + ai_agent 组件编译烧录（黄山派为官方竞赛支持板） |
| LLM 后端 + 基础对话 | MiMo Token / OpenAI 兼容 API，USB 桥或本地调用 |
| ≥1 交互渠道 | **语音**（MIC→云 LLM→播报，主渠道）+ **CLI/NSH**（辅）+ **WebSocket**（USB 桥，收飞书/微信消息） |
| ≥1 自定义 Skill | **4 个**：`reminder`（定时提醒）、`daily-briefing`（每日简报）、`health-nudge`（久坐/活动建议）、`memo`（语音备忘） |
| **≥1「主动+执行」场景** | **四类全做**：定时主动（早 8 点简报推送）、阈值主动（久坐超 1h 提醒）、事件主动（新消息到达播报）、上下文主动（连续 3 天低活动→建议） |
| 应用场景说明 | 用户故事 + 功能清单 + 技术实现（文档模板见 §8） |

**进阶加分项覆盖**：记忆机制（/data 轻量 JSON）、端端协作（PC 大模型↔设备 Agent 任务拆分）、独立 LVGL 应用 UI、飞书/微信多渠道（WebSocket 桥）。

---

## 2. 用户故事（场景定位）

> **小张**是一名经常加班的产品经理，戴着一块黄山派智能手表原型机。每天早 8 点，手表主动推送当天日程简报；工作时久坐 1 小时，手表振动提醒他起来活动；他对着手表说"下午 3 点提醒我跟设计对稿"，Agent 自动解析成提醒任务；电脑上收到飞书消息，手表立刻播报；连续 3 天运动量不足，Agent 主动建议他周末去散步并自动把建议存成备忘。

**解决什么问题**：把"被动问答"升级为"贴身管家"——该说的主动说，该做的自动做（建提醒、存备忘、查日程）。

---

## 3. 核心设计：Agent 能力矩阵

### 3.1 主动场景（赛题核心区分点，四类各一）

| 主动类型 | 具体实现 | 触发机制 | 执行动作（Tool 调用） |
|---|---|---|---|
| **定时主动** | 每日 08:00 推送 `daily-briefing`（日期、日程、待办、天气） | ai_agent 定时任务 | 屏幕弹通知 + TTS 播报 |
| **阈值主动** | 久坐 >60min（IMU 静止检测）→ `health-nudge` 告警 | 传感器阈值任务 | 马达振动 + 屏幕提示 + 播报 |
| **事件主动** | WebSocket 桥收到新消息（飞书/微信模拟）→ 主动播报 | 消息总线事件 | 播报内容 + 通知卡片 |
| **上下文主动** | 连续 3 天活动量低于阈值 → 主动建议运动 | 记忆 + 周期评估任务 | 播报建议 + 写入备忘文件 |

### 3.2 Skill 体系（存放 `/data/agent/skills/`，Markdown 定义）

**Skill 1：reminder（定时提醒）——核心 Skill**
- 自然语言意图：`"3 点提醒我开会"` → Router 路由到 reminder → 结构化（时间+内容）→ 登记定时任务 → 到点触发通知+播报
- 示例骨架（最终格式以 ai_agent 框架文档为准，W1 核实）：
```markdown
---
name: reminder
description: 解析"X点提醒我Y"类指令，创建定时提醒任务，到点主动播报并弹通知
triggers: [schedule]
tools: [notification, tts, file]
---

## 流程
1. 意图解析：提取时间、提醒内容
2. 写入 /data/agent/tasks/ 任务文件
3. 到点触发：notification 弹窗 + tts 播报
```

**Skill 2：daily-briefing（每日简报）**——早 8 点定时主动；读取日程/待办/备忘录文件 + 传感器数据，生成简报文本播报。
**Skill 3：health-nudge（久坐/活动建议）**——IMU 静止时间统计（阈值主动）+ 活动量记忆（上下文主动）。
**Skill 4：memo（语音备忘）**——"记一下周五交周报" → 结构化存入 /data/agent/memos/，Router 分流"记一下 vs 提醒我"的演示 Skill。

### 3.3 Agent「会执行」的工具面（Tool/Shell）

| 工具 | 实现 | 演示点 |
|---|---|---|
| 读传感器 | IMU 静止检测（久坐）、光感（环境变化）、ADC（电池电量） | "我电量多少？"→ 读 ADC0 回答 |
| 写文件 | 备忘/待办/偏好写入 /data/agent/ | "记一下…"→ 文件可见 |
| 发通知 | 屏幕通知卡片 + TTS 播报 + 马达振动 | 所有主动场景 |
| 执行命令 | NSH 命令 / LED 控制 / RGB 状态 | "把灯调成蓝色"→ GPIO 生效 |

### 3.4 交互渠道

- **语音（主，USB 桥双轨架构）**：按键/抬腕唤醒 → PC 桥录音 → ASR → LLM → TTS → PC 播放 + 设备端屏幕展示（详见 §3.5）
- **CLI（辅）**：NSH 命令与 ai_agent 对话（保底渠道，断网可用）
- **WebSocket（桥）**：PC 桥程序 ↔ USB CDC ACM，收发消息（模拟飞书/微信/远程指令 → 事件主动场景）
- **离线保底**：ASR 降级 PC 本地 Vosk（离线中文识别）+ 端侧规则路由，无网仍可演示完整交互

### 3.5 语音方案（已核实）

**核实结论**：

| 核实项 | 结论 | 证据 |
|---|---|---|
| openvela 音频框架 | ✅ 存在：nxaudio + ALSA 兼容，设备节点 `/dev/audio/pcm0p`(播放)/`pcm0c`(录音)；`nxplayer` 工具支持 `playraw` 验证播放 | openvela 音频测试工具文档 |
| ai_agent 语音能力 | ✅ 框架内建：Skill 支持 Voice ASR/Voice TTS（官方示例 voice-translate.md），偏好存 `/data/agent/config/USER.md` | packages_ai_agent 仓库 |
| 端到端语音 demo | ✅ 现成参考：`packages_demos/bailian`（ASR→LLM→TTS 语音对话，NSH 启动，WebSocket TLS） | packages_demos 仓库 |
| 黄山派音频**硬件**能力 | ✅ 板载 MEMS MIC → 芯片 ADC 采样（MIC_ADC_IN/MIC_BIAS）+ DAC 输出（AU_DAC1P/N + PA_42 功放使能）——官网确认"支持板上 mic 音频输入" | 立创·黄山派使用指南 |
| 黄山派音频**openvela 驱动** | ❌ **源码级确认缺失**（2026-08-20 git log 核实）：chips/sf32lb52/ 无任何 audio/dac/i2s 文件；全仓库零音频提交 | vendor_sifli `dev-ai-contest-2026` 分支 |
| 音频驱动**地基（仓库内已有）** | ✅ **源码级确认**（2026-08-20 全量拉取核实，Apache-2.0）：① CMSIS 寄存器层 `cmsis/sf32lb52x/audcodec.h` ② HAL 层 `hal/bf0_hal_audcodec.c`（**DAC_CH0/1 播放 + ADC_CH0/1 录音 + DMA + 状态机**，能力完备）+ `bf0_hal_i2s.c` + `bf0_hal_pdm.c` ③ 板级配置 `boards/.../sf32lb52x/audio_config.h`（DMA 通道映射可参考） | vendor_sifli `chips/drivers/` |
| **缺的一层** | ❌ NuttX audio 驱动封装（/dev/audio/pcm0p）未实现——需自写，**样板齐全**：nuttx `drivers/audio/audio_i2s.c`/`audio_dma.c` + cxd56_nxaudio 写法 | 两仓库比对 |

**实现架构：USB 桥双轨**（语音 I/O 在 PC 桥，Agent 理解/主动/执行在设备端——赛题得分点全在端侧）：

```
设备按键/抬腕 → USB CDC 命令 → PC 桥
  → PC 录音 → ASR（云端 MiMo/百炼；断网降级 PC 本地 Vosk）→ 文本
  → 回传设备端 → ai_agent Router 意图路由（端侧）→ Skill/Tool 执行（端侧）
  → 屏幕展示执行结果 + TTS 回传 → PC 播放 + 设备端通知卡片
```

| 环节 | 首选 | 断网表现 | 备选 |
|---|---|---|---|
| ASR | 云端（MiMo/百炼 WebSocket） | 不可用 | **PC 本地 Vosk**（离线中文） |
| LLM | MiMo Token | 不可用 | 端侧规则兜底路由 |
| TTS | 云端 TTS | 不可用 | PC 本地 pyttsx3/SAPI |
| 提示音 | PC 播放预录音频 | ✅ | 设备端屏幕+马达 |

**唤醒**：按键/抬腕唤醒（手表场景天然适用，免唤醒词驱动、省功耗）。

**升级链路（已确认可行，升级为 W1 并行主线）**：驱动地基（CMSIS 寄存器 + HAL 播放/录音 + 板级 DMA 配置）**已在 vendor_sifli 仓库内**（Apache-2.0），缺的只是 NuttX audio 封装层（约 500~1000 行，仿 nuttx `cxd56_nxaudio.c` / `audio_i2s.c` 写法）→ 挂 `/dev/audio/pcm0p`（播放）+ `pcm0c`（录音）→ `nxplayer playraw` 验证播放、录一段验证采集 → 语音完全本地化（复用 bailian 配置路径）。SiFli SDK 申请降级为锦上添花（可拿 RT-Thread 上层驱动封装参考）。

### 3.6 KWS 端侧实现（2026-08-20 源码级核实：可行）

**可行性四要素（全部本地源码确认）**：

| 要素 | 结论 | 证据 |
|---|---|---|
| 推理框架 | ✅ NuttX 自带 TFLite Micro（`CONFIG_TFLITEMICRO=y`，sim 板验证） | nuttx `boards/sim/.../tflm/defconfig` |
| DSP 库 | ✅ CMSIS-DSP 全套（arm_math：FFT/MFCC） | vendor_sifli `chips/external/CMSIS/DSP_Lib/` |
| 硬件加速 | 🟡 芯片 BSP 有 CNN 加速器 HAL（`bf0_hal_nn_acc.c`，CONV2D 模式），待确认 SF32LB52 是否实例化（查数据手册） | vendor_sifli `chips/drivers/hal/` |
| 算力/内存 | ✅ M33@240MHz + 512KB SRAM + 8MB PSRAM，KWS int8 模型（几十 KB）无压力 | 芯片规格 |

**三条路线**：
- **R1 纯 DSP 保底**：VAD + MFCC（CMSIS-DSP）+ DTW 模板匹配，零框架零模型，300~500 行 C，W2 可跑（演示保底，5~10 命令词）
- **R2 标准主线（推荐）**：`CONFIG_TFLITEMICRO` + 开源 KWS 模型（Speech Commands，DS-CNN int8，PC 可下载/自训），业界标配，M33 推理 <50ms，W2-W3 集成
- **R3 硬件加速（加分）**：NN_ACC 外设确认存在后，CNN 推理上硬件加速器（答辩亮点：功耗/时延实测）

**前提**：共用 §3.5 的 NuttX 音频封装层（MIC→ADC→PCM 数据通路，1~2 周）。

**W1 语音专项核实清单**：
1. `packages_demos/bailian` 源码：录音/播放流程、WebSocket 协议（直接参考移植）
2. `packages_ai_agent` 仓库：voice skill 接口、ASR/TTS 服务接入方式
3. SiFli SDK：SF32LB52 音频驱动参考（决定升级链路）
4. `dev-ai-contest-2026` 分支最新状态：音频驱动是否已合入（竞赛期活跃维护）

---

## 4. 技术架构

```
┌─────────────── PC 侧（AI 增强 + 端端协作） ───────────────┐
│  Python 桥：USB CDC ⇄ LLM(MiMo/OpenAI API) ⇄ WebSocket   │
│  PC 端 Agent（Claude/大模型）：生成任务 → 下发设备端执行    │
└──────────────────────────┬───────────────────────────────┘
                     USB CDC ACM (/dev/ttyACM0)
┌──────────────────────────┴───────────────────────────────┐
│  黄山派 · openvela (dev-ai-contest-2026)                  │
│  ┌────────────────────────────────────────────────────┐  │
│  │ LVGL 应用（自定义 UI：表盘/对话/通知卡片/任务列表）  │  │
│  │        ↓ ai_agent 消息总线 ↓                        │  │
│  │ ai_agent 框架：Router 意图路由 / 主动任务引擎        │  │
│  │  Skill 引擎（/data/agent/skills/*.md）              │  │
│  │  Tool 层：sensor / file / notify / tts / shell      │  │
│  ├────────────────────────────────────────────────────┤  │
│  │ 驱动：LCD/触摸 ✅ RTC ✅ ADC ✅ USB ✅ 按键 ✅      │  │
│  │       MIC/PA ⚠️ IMU 🟡 马达 🟡（W1 核实/补齐）      │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

---

## 5. 功能清单

**MVP（必做）**
- [ ] openvela + ai_agent 固件在黄山派运行，LLM 后端连通，CLI 对话正常
- [ ] 语音交互渠道（MIC 录音→LLM→TTS 播报）
- [ ] `reminder` Skill：语音建提醒 → 到点主动播报（定时主动 ✅）
- [ ] `memo` Skill：语音备忘 → 文件落盘（Router 分流演示）
- [ ] `daily-briefing` Skill：早 8 点主动简报（定时主动 ✅）
- [ ] 场景说明文档（用户故事/功能清单/技术实现）

**进阶（加分）**
- [ ] 久坐提醒（IMU 阈值主动）+ 活动量记忆（上下文主动）
- [ ] 记忆机制：偏好/备忘跨会话（/data/agent/ JSON）
- [ ] 端端协作：PC 大模型生成任务 → 设备端 Agent 执行 → 结果回传
- [ ] WebSocket 消息事件 → 主动播报（事件主动）
- [ ] 独立 LVGL 应用 UI（表盘 + 对话 + 通知卡片）
- [ ] 低功耗实测（息屏待机电流，板载功耗测试点）

---

## 6. 硬件适配（对照使用指南，openvela 驱动状态）

| 功能 | 硬件资源（模组脚位） | openvela 状态 | 备注 |
|---|---|---|---|
| 表盘 UI | AMOLED 390×450（PA_00~08）+ 触摸（PA_33/37/41） | ✅ 已驱动 | 演示主力 |
| 语音输入 | MEMS MIC（37 脚 MIC_ADC_IN） | ⚠️ W1 核实 | 缺失则走 USB 桥录音 |
| 语音播报 | 模拟 DAC（33/34 脚）+ Class-D PA（PA_42） | ⚠️ W1 核实 | 缺失则先只弹通知 |
| 久坐检测 | IMU LSM6DS3TR-C（I2C1：PA_39/40） | 🟡 W1 核实 | 缺失则用按键模拟事件 |
| 电量查询 | ADC0 + VBATS（61 脚） | ✅ 已驱动 | |
| 时间/定时 | RTC / 硬件定时器 | ✅ 已驱动 | 主动任务载体 |
| 交互/反馈 | KEY1/2（PA_34/43）、马达（PA_20）、RGB（PA_32） | ✅/🟡 | 马达/RGB 待核实 |
| 桥接/通信 | USB2.0 FS（PA_35/36）+ CH340N UART | ✅ 已驱动 | 端端协作通道 |

---

## 7. 一个月开发计划（8/21 → 9/20）

| 周 | 任务 | 里程碑（可演示） |
|---|---|---|
| **W1** 8/21–27 | 报名；`repo init -b dev-ai-contest-2026` 环境；官方板级 + ai_agent 组件编译烧录；**驱动核实**（§6 表 + 框架文档：Skill 格式、消息总线 API、主动任务配置）；LLM 后端（MiMo Token）配置 | **固件带 ai_agent 跑通，CLI 对话成功** |
| **W2** 8/28–9/3 | USB 桥语音链路（PC 录音→ASR→LLM→TTS→PC 播放，参考 bailian 流程）；`reminder` Skill 落地；Router 分流（记一下 vs 提醒我）；SiFli SDK 音频驱动评估（升级链路可行性） | **语音建提醒 → 到点主动播报**（赛题核心 ✅，USB 桥双轨） |
| **W3** 9/4–10 | `daily-briefing` + `memo` Skill；主动任务引擎（定时/阈值）；IMU 久坐检测；LVGL 应用 UI（表盘/通知卡片） | **早 8 点主动简报 + 久坐提醒** |
| **W4** 9/11–17 | 记忆机制；端端协作（PC↔设备）；WebSocket 事件主动；低功耗实测；demo 视频 + 场景说明文档 + README | **端端协作完整演示** |
| **收尾** 9/18–20 | 提交专属仓（fork→PR→合入）、AI Coding 日志导出至 logs/、视频上传 | —— |

---

## 8. 交付物清单（对照赛题）

1. **专属 GitHub 仓库**：LVGL 应用/快应用源码、`skills/` 下自定义 Skill（reminder/daily-briefing/health-nudge/memo）、文档
2. **应用场景说明**（仓库内 MD）：用户故事 / 功能清单 / 技术实现（用了 ai_agent 哪些核心能力：主动任务、Router、Tool/Shell、消息总线）
3. **演示视频**（3–5 分钟）：CLI 对话 → 语音建提醒 → 到点主动播报 → 每日简报 → 久坐提醒 → （加分）端端协作
4. **AI Coding 日志**：导出选定会话到仓库 `logs/` 目录（按《AI Coding 日志归集与提交手册》）
5. 板级/驱动改动若涉及 vendor_sifli 或 nuttx，按《参赛代码提交指南》处理（优先只改专属仓，减少公共仓依赖）

---

## 9. 风险与降级

| 风险 | 等级 | 应对 |
|---|---|---|
| 语音链路（MIC/DAC-PA 驱动缺失） | 🟡 | 已核实：采用 USB 桥双轨（§3.5），语音 I/O 走 PC 桥零驱动依赖；音频驱动作为升级链路而非必做 |
| ai_agent 框架细节（Skill 格式/主动任务 API）与预期不符 | 🟡 | W1 以框架文档/源码为准重新对齐；Skill 设计保持"自然语言意图→结构化→执行"通用思路 |
| LLM 网络依赖 | 🟡 | USB 桥方案断网时退化：本地 KWS 命令词 + 预置简报，演示脚本化 |
| 只能在 `dev-ai-contest-2026` 分支构建 | 🟢 | 严格锁定分支，README 标注版本 |
| IMU/马达未驱动 | 🟢 | 久坐场景用"定时器模拟静止"兜底 |

---

## 10. 本周行动清单

- [ ] 报名提交作品方向（黄山派 + ai_agent「主动+执行」腕上管家）
- [ ] 按《参赛代码提交指南》获取专属仓库并 fork → 准备 PR 工作流
- [ ] 环境搭建：`repo init -b dev-ai-contest-2026` + `repo sync`（nuttx + vendor_sifli + manifest）
- [ ] 编译官方板级（含 ai_agent 组件）→ SifliTrace/CH340N 烧录验证
- [ ] 配置 MiMo/LLM 后端，CLI 对话跑通
- [ ] 核实：§6 驱动表（MIC/PA/IMU/马达）+ ai_agent 框架文档（Skill 格式、主动任务、Tool 接口）
- [ ] 准备硬件：3W/4Ω 喇叭（GH-1.25mm）、450~500mAh 锂电池

---

## 附：参考资料

- 赛题说明（用户提供）：openvela AI 大赛——「能主动、会执行」的嵌入式 AI Agent 应用
- openvela 大赛官方说明：https://raw.githubusercontent.com/open-vela/docs/dev-ai-contest-2026/zh-cn/contest_2026/contest_overview.md
- 立创·黄山派使用指南（用户提供）：wiki.sifli.com/board/sf32lb52x/SF32LB52-黄山派.html
- openvela/vendor_sifli 板级 README：https://gitcode.com/open-vela/vendor_sifli/blob/dev-ai-contest-2026/boards/sf32lb52/lckfb_huangshan_pi/README.md
- 同文件夹：《黄山派-AI智能终端-开发计划.md》（通用版，可作硬件参考）、《双USB智能摆渡与AI审计网关-开发计划.md》（GD32 备选）
