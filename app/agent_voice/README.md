# agent_voice — 应用层代码入口

本目录是「屏幕聊天 UI + PTT 按键 + 中文显示」这一应用层的落地位置，
对应我们作品里运行在设备上的可见交互部分。

manifest 已把它映射到编译树：

```
app/agent_voice  ->  packages/demos/contest2026_286_agent_voice
```

## 为什么实际源文件不在这个目录里

这套 UI 不是独立 app，而是 **ai_agent 包内部的一个消息通道**
（`lvgl_ui_channel.c` 与 agent 的消息总线、上下文构建、工具循环直接交互），
因此它的源文件必须与 ai_agent 同目录编译，无法作为一个独立 package 编译。

本仓的完整代码布局是：

| 目录 | 内容 |
|---|---|
| `app/agent_voice/` | 本入口（映射到 packages/demos） |
| `src/ai_agent/` | 自研源文件：LVGL 聊天 UI、麦克风上行 `mic_stream`、`cmd_mic` 调试命令、GB2312 中文字体 |
| `src/vendor_sifli/` | 自研 AUDCODEC ADC 驱动（openvela 全树首个录音驱动） |
| `src/apps_pppd/` | pppd 直连（空 modem）改动后的源文件 |
| `board/sf32lb52_agent/defconfig` | 板级配置（录音/UI/工具/网络全部开关） |
| `docs/upstream/` | 让上述源文件落到 openvela 工程正确位置的补丁 |
| `tools/host/` | PC 端联调脚本（PPP 服务端、LLM 代理、语音桥、看门狗） |

复现方式见仓库根目录 `README.md` 第四节。
