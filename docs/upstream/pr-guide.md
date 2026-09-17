# 上游 PR 操作手册（三个公共仓）

本作品对 openvela 公共仓的改动，除随参赛仓提交外，还需按赛事要求
**fork 对应公共仓 → PR 到 `dev-ai-contest-2026` 分支**（由组委会 review 合入）。

三个仓的补丁已实测可干净应用（`git apply --check` 通过），提交素材全部就绪。

---

## 一、动作清单

对每个仓库重复以下流程（以 `packages/ai_agent` 为例，裸仓名即 fork 名单）：

```bash
# ① 网页 fork（已登录 Oliweitz 账号）
#    https://github.com/open-vela/packages_ai_agent  -> Fork
#    https://github.com/open-vela/vendor_sifli        -> Fork
#    https://github.com/open-vela/apps                -> Fork

# ② WSL 里加远端并拉取
cd ~/openvela/packages/ai_agent
git remote add myfork https://github.com/Oliweitz/packages_ai_agent.git
git fetch myfork dev-ai-contest-2026

# ③ 确认身份（提交会带上你的名字）
git config user.name  Oliweitz
git config user.email Oliweitz@users.noreply.github.com

# ④ 建分支（基于本地当前分支 HEAD，即干净的上游提交）
git stash -u                                   # 暂存我们的工作树改动
git checkout -b contest/agent-voice myfork/dev-ai-contest-2026
git apply <本仓>/docs/upstream/patches/01-ai_agent.patch
git add -A && git commit -s -m "<见下方提交信息>"
git push myfork contest/agent-voice

# ⑤ 网页发 PR：compare 页选 base = dev-ai-contest-2026，填标题与描述
#    https://github.com/open-vela/packages_ai_agent/compare/dev-ai-contest-2026...Oliweitz:contest/agent-voice

# ⑥ 恢复工作树
git checkout - && git stash pop
```

> 也可以在 WSL 装 `gh`（`sudo apt install gh && gh auth login`）后
> `gh pr create --repo open-vela/packages_ai_agent --base dev-ai-contest-2026 --fill`。

## 二、三个 PR 的标题与描述

### PR 1 — `packages_ai_agent`

- 分支名：`contest/agent-voice`
- 标题：**agent: fix REST/TLS/skill-loading defects and add voice + LVGL channels (SF32LB52)**
- 描述要点：
  - 本 PR 同时包含「新功能」与「缺陷修复」，缺陷部分建议单独 review：
    - `vela_tls`：请求体超过 16 KB 时 `mbedtls_ssl_write` 必然失败（单次写上限）
    - `agent_loop`：缓存命中跳过工具循环，执行过工具的回合不得写缓存
    - `skill_loader`：描述解析死代码导致所有内置技能描述为空
    - `tool_get_time`：时钟校验只查下界 + `%*[^,]` 扫描集在 NuttX 下不支持
    - `ws_server`：pong 无锁写 + 帧分两次 send 导致帧交错与 Nagle 互锁
    - `api_handler`：`CONFIG_AI_AGENT_REST_API=y` 时编译不过（缺 include/映射）
    - `tool_registry`/`tool_shell`：工具白名单与实现不一致
  - 新功能：LVGL 聊天 UI（含 PTT）、麦克风上行 `mic_stream`、`cmd_mic` 调试命令、内置 CJK 字体
  - 验证平台：黄山派 SF32LB52，连续运行 6 分钟无掉线

### PR 2 — `vendor_sifli`

- 分支名：`contest/audcodec-adc`
- 标题：**sf32lb52: add AUDCODEC ADC capture driver and fix GCC14 build warnings**
- 描述要点：
  - 新增 `sf32lb_audcodec.c/.h`：openvela 全树首个可用的录音驱动
    （调用 `HAL_AUCODEC_Refgen_Init` 与 `Config_ADCPath_Volume`——两者在 HAL 头文件中均无声明，
    驱动内以显式原型声明；**建议后续在 HAL 头文件中补上声明**）
  - 新增板级配置 `configs/agent/`（录音 + UI + PPP 相关开关）
  - GCC14 修复：`arm_lowprintf` 缺原型、`sifli_i2cbus_initialize` 无公开声明

### PR 3 — `apps`

- 分支名：`contest/pppd-null-modem`
- 标题：**pppd: support null-modem (direct host) links, fix dial-up-only assumptions**
- 描述要点：
  - `pppd_main.c`：`connect_script` 是蜂窝拨号序列，直连线（null-modem）场景会超时；
    `ttyname` 写死 `/dev/ttyS1`，该板上不存在（控制台 UART 只注册 `/dev/console`）
  - `pppd.h`/`ipcp.c`：新增 `local_ip` 字段，否则 IPCP 只能靠对端 NAK 分配
  - `ppp.c`/`ppp_conf.h`：`AHDLC_TX_OFFLINE` 计数与 `ip_no_data_time` 在无 modem
    场景下会误判链路离线并主动 `ppp_reconnect()`（向 PPP 字节流写 `+++`/`ATE1`，拆掉正常链路）。
    修复后连续放音频 6 分钟 0 掉线（修复前 30~60 秒必死）

## 三、未包含在补丁中的上游问题（可选，独立 PR）

开发中还发现以下平台缺陷，**不在本作品补丁里**，可另开小 PR 或写进 PR 描述供组委会参考：

| 位置 | 问题 |
|---|---|
| `packages/ai_agent/Kconfig` | 参考 defconfig 都写了 `CONFIG_NET_TCPBACKLOG=y`，但 Kconfig 无 `select`，缺了 poll+accept 永不返回 |
| `packages/ai_agent` 退出路径 | heartbeat 线程未 join，退出时 `mm_free.c:244` 断言崩溃 |
| `vendor/sifli` AUDCODEC HAL | 5 处：ADC_ENABLE 调用点被注释、Refgen/Volume 无头文件声明、`Config_ADCPath` 实现被 `#if 0`、MspInit 空实现 |
| 板级 defconfig 参考值 | IOB 缓冲默认 64 个 vs agent 单请求 ~15.8 KB，建议参考配置提高；syslog 抢 PPP 串口，建议 RAMLOG 方案 |
