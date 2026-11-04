# 端侧健康监测、告警与自治恢复

> 2026-11-03 设计并实现，2026-11-04 完成 Phase 4 HIL 故障注入验证。
> 这是作品提交后的扩展能力：让无人值守的设备能自己发现异常、先做服务级恢复，
> 必要时在门禁内软复位，并把极少量证据留在 RTC 备份寄存器里。

## 一、为什么需要它

原固件的 `/api/health` 只有 `uptime / free_heap / time_synced` 三个字段，
`ok` 还是硬编码的 `true`；真正的内存、任务、网络快照只存在于 LVGL 采样线程。
设备无头运行（控制台让给 PPP），一旦麦克风停采、WS 线程卡住或网络掉线，
主机只能看到「HTTP 200 + 音频字节不涨」，无法区分「降级但还活着」和「真挂死」。

本子系统在设备端建立统一快照，用规则 + 趋势判断异常，先做服务级恢复，
再按门禁软复位；只持久化 boot 次数、复位原因和最近一次异常摘要。

**范围约定**：不修 `sem_waitirq.c:137` 内核竞态、不启用硬件看门狗、
不做完整 LittleFS 健康历史。

## 二、架构

```
各服务心跳/计数器 ──> health_monitor（独立线程，5 s）
                        ├─ 规则 + 趋势 + 状态机
                        ├─ health_snapshot（mutex + seq）
                        ├─ 恢复状态机（mic 重启 / WS listener 重启 / drop client）
                        └─ health_store（RTC BKP5/BKP6）
                              ↓
     /api/health、WS {"type":"health"}、状态页圆点/横幅、get_health 工具
                              ↓
               主机 pipeline_watchdog / voice_ready
```

新增文件：

| 文件 | 作用 |
|---|---|
| `packages/ai_agent/src/core/health_monitor.c/.h` | 采样线程、规则、状态机、恢复、故障注入 |
| `packages/ai_agent/src/infra/health_store.c/.h` | RTC 备份寄存器持久化 |
| `packages/ai_agent/src/tools/tool_device_health.c/.h` | `get_health` Agent 工具 |
| `vendor/sifli/chips/sf32lb52/sf32lb_bbram.c` + `include/sf32lb_bbram.h` | `HAL_*_backup` 封装 |
| `tools/host/health_fault.sh` | HIL 故障注入脚本 |

同时改造了 `ws_server`、`mic_stream`、`cron_service`、`message_bus`、`llm_router`、
`agent_loop`、`agent_main`、`network_manager`、`api_handler`、`lvgl_ui_channel`、
`tool_registry`、`skill_loader` 和板级 `sifli_ap.c`（reset reason 捕获）。

## 三、接口

### GET /api/health

保留四个 legacy 字段，其余为新增分组：

```json
{
  "ok": true, "uptime": 202, "free_heap": 7343688, "time_synced": true,
  "status": "ok", "stale": false, "grace": false,
  "boot_count": 2, "auto_reset_count": 0, "reset_reason": "health_auto",
  "safe_mode": false, "needs_config": false, "persist_ok": true,
  "heap": { "total": 8456416, "free": 7343688, "used": 1112728,
            "largest": 7339328, "free_min": 7342712, "ewma": 7343688,
            "slope_kb_per_min": -1 },
  "tasks": { "count": 17, "baseline": 15, "min_prio": 0 },
  "net": { "up": true, "ever_up": true, "down_s": 0, "ip": "10.0.0.2" },
  "ws": { "listening": true, "clients": 1, "tx_ok": 1742, "tx_fail": 0,
          "tx_timeout": 0, "rx_frames": 5, "accept_age_ms": 281 },
  "mic": { "want": true, "running": true, "frames": 1731, "bytes": 1772544,
           "read_errors": 3, "send_errors": 0, "restarts": 3,
           "last_frame_age_ms": 372 },
  "cron": { "running": true, "jobs": 0, "fires": 0, "errors": 0,
            "beat_age_ms": 9882 },
  "llm": { "calls": 0, "failures": 0, "avg_latency_ms": 0,
           "backends": 1, "backoff": 0, "busy_s": 0 },
  "bus": { "in_depth": 0, "out_depth": 0, "in_drops": 0, "out_drops": 0 },
  "beats": { "outbound": 1894, "agent_idle": 1620, "agent_llm": 4294967295,
             "agent_tool": 4294967295, "net": 600, "cron": 1500,
             "ws_accept": 42, "mic": 16, "lvgl": 12 },
  "issues": [],
  "last_anomaly": { "code": "task_stall", "severity": 2, "uptime_s": 349 },
  "recovery": { "attempts": 2, "reboot_pending": false, "handling": false }
}
```

- `ok` 等于 `status == "ok"`，不再硬编码。
- `beats.agent_llm / agent_tool` 是 busy 标记：未忙时为 `0xFFFFFFFF`（从未进入），
  忙时 `llm.busy_s / tool_busy_age_s` 给出持续时间；其余心跳是「距今毫秒数」。
- HTTP 始终返回 200（避免主机 watchdog 误判），语义看 `status`。
- boot 后 120 s 为 grace：只采集不判定；heap/task 规则再晚 60 s 启用。

### POST /api/health/ack

主机完成重新配置后调用，清除 `auto_reset_count` 与 `safe_mode`。

### POST /api/health/fault（仅 `CONFIG_AI_AGENT_HEALTH_TEST=y`）

```json
{"fault":"agent_stall","seconds":30}   // 30 s 后自恢复；0 表示永久
{"fault":"mic_read_fail","count":3}    // 接下来 3 次读失败
{"fault":"ws_send_fail","count":20}    // 接下来 20 次发送失败
{"fault":"heap_hog","kb":7000}         // 按住 7000 KB，直到 fault_off
{"fault":"net_down","seconds":60}      // 60 s 内 network_is_connected() 返回 false
{"fault":"fault_off"}                  // 清空
```

> 设备很忙时（尤其 `heap_hog` 要在 MCU 上 memset 7 MB），POST 的响应可能晚于
> 客户端的超时；注入本身已经生效，可用 `/api/health` 的字段变化确认。

### WS / UI / Agent

- WS 新增 `{"type":"health",...}`：状态迁移立即发，非 ok 每 60 s，ok 每 5 min。
- 状态栏 8×8 健康圆点（teal/amber/red），状态页健康横幅 + top issue。
- Agent 工具 `get_health` 返回一行摘要；`device-guardian` 技能优先调用它。

## 四、规则与阈值

默认值集中在 `packages/ai_agent/Kconfig`，HIL 可调。

| 规则 | warning | critical | 去抖 | 动作 |
|---|---|---|---|---|
| HEAP_LOW | free < max(512 KB, arena/16) | free < max(128 KB, arena/64) | 2 / 1 | critical 可复位 |
| HEAP_FRAG | largest < 64 KB 且 free > 512 KB，或 largest < free/10 | — | 3 | 报告 |
| HEAP_TREND | 5 min 斜率 < -256 KB/min | < -1 MB/min | 3 | critical 可复位 |
| TASK_GROWTH | count > baseline + 8 | > baseline + 16 | 3 | 报告 |
| TASK_STALL | 关键线程超时（见下） | 同左 | 2 | agent/tool 可复位 |
| WS_ACCEPT_DEAD | accept 心跳 > 10 s | > 30 s | 2 | listener-only 重启 |
| WS_TX_FAIL | 60 s 失败率 > 20% 且 ≥5，或失败 ≥10 | 失败率 > 50% 且 ≥10，或失败 ≥30 | 2 | drop worst client |
| MIC_STALL | want && (!running 或 age > 3 s) | > 10 s | 1 | 重启 mic |
| MIC_ERR | 60 s 读错误 > 3 或发送失败率 > 20% | 同左 | 2 | 重启 mic |
| NET_DOWN | 曾 up 且 down > 30 s | > 180 s | 2 | **只报告，不复位** |
| CRON_STALL | 心跳 > 60 s | > 300 s | 2 | critical 可复位 |
| LLM_FAIL | 5 min 失败 ≥3 且失败率 > 50%，或全 backend backoff | — | 3 | **只报告** |
| LLM_SLOW | in_llm > 300 s | > 960 s | 1 | 只报告 |
| BUS_BACKLOG | outbound 深度 > 12 持续 60 s | — | 3 | 报告 |

关键线程期望周期：

| 线程 | 心跳 | warning / critical |
|---|---|---|
| outbound | 5 s | 30 / 60 s |
| agent_loop idle | 5 s | 30 / 60 s |
| agent_loop LLM | 240 s socket 上限 | 300 / 960 s（只报告） |
| agent_loop tool | 120 s | 120 / 300 s（可复位） |
| net_watch | 1 s | 10 / 30 s |
| cron | 10 s | 60 / 300 s |
| ws_accept | 0.1 s（非阻塞轮询） | 10 / 30 s |
| mic | 每帧 | 3 / 10 s |
| lvgl | 事件循环 | 5 / 30 s（只 warning） |

状态机：`OK → DEGRADED → CRITICAL → (RECOVERING) → OK`；
每个 issue 独立 streak，降级需连续触发，恢复需连续 3 次健康才清除（滞回）；
LLM/tool busy 时豁免 idle 规则；mic 只在 `want_running` 时算 expected。

## 五、自治恢复与持久化

### 服务级恢复

- **mic**：`mic_stream_restart()`，退避 10/30/90 s，最多 3 次；
  无 WS client 时不反复重启，只报 degraded 等主机重连。
- **WS listener-only 重启**：新增 `s_accept_running`，accept 线程改 joinable；
  只 `shutdown/close` 监听 fd 再重新 bind，**不触碰已有 client 表**。
  2 s 内退不出就放弃本次重启，交给复位预算。
- **问题 client**：`ws_server_drop_worst_client()`，只断开失败最多的那一个。

### 软复位门禁

以下条件全部满足才允许复位：

```
CONFIG_AI_AGENT_HEALTH_AUTORESET
&& status == CRITICAL && issue_is_reset_class
&& uptime >= 120 s
&& auto_reset_count < 3 && !safe_mode
&& net_ever_up && (net_up || net_down_s <= 120 s)
&& (auto_reset_count == 0 || uptime >= 300 s)
&& !reboot_pending
```

复位前置 `reboot_pending` 并广播、写 BBRAM、回读校验、sleep 1 s，
再 `boardctl(BOARDIOC_RESET, 0)`。连续 3 次后进入 `safe_mode`，只告警；
主机 `POST /api/health/ack` 清除预算。

> `/data` 是 tmpfs：软复位后 LLM 配置/技能/cron 会丢。health 报 `needs_config`，
> 主机 `voice_ready.sh --no-reset` 重新下发配置并 ack。

### RTC 备份寄存器（BKP5/BKP6）

- BKP5：magic `0xA5` + inverse `0x5A`、boot_count(8b)、auto_reset_count(4b)、
  reset_reason(4b)。
- BKP6：issue_code(8b)、severity(2b)、reset_marker(1b)、valid(1b)、uptime 低 16 位。
- 写后回读，失败置 `persist_ok=false`；主机日志兜底。
- **注意**：RTC 域掉电（拔电/重新烧录）会丢失，boot_count 从 1 重新开始；
  软复位（`boardctl reset`）可正常保持，已实测 `boot=2/3, reason=health_auto`。

## 六、Phase 4 HIL 实测（2026-11-04）

设备：黄山派 SF32LB52 + 8.46 kHz μ-law 音频流 + voice_bridge 在线。

| 注入 | 期望 | 实测 |
|---|---|---|
| 基线静置 6 min | status=ok，心跳新鲜 | ✅ 全程 ok；accept 心跳最大 1.8 s；无复位 |
| `mic_read_fail 3` | mic 重启并恢复 | ✅ `read_errors=3`，`restarts=3`（前两次重启又吃到注入），最后 ok |
| `agent_stall 45` | warning 后自恢复 | ✅ 约 47 s 出现 `task_stall/1` degraded；45 s 后 idle 心跳恢复，15 s 内回 ok |
| `net_down 60` | 只报告，不复位 | ✅ 41 s 出现 `net_down/1` degraded；60 s 到期后 15 s 内回 ok；无复位 |
| `heap_hog 7000` | 触发 heap 规则 | ✅ 触发 `heap_trend/2` critical → 软复位；`boot=2, reason=health_auto, last_anomaly=heap_trend` |
| `ws_send_fail 20` | warning + drop client | ✅ `tx_timeout=20`，`ws_tx_fail/1`；clients 1→0→1（问题连接被断开、主机自动重连），mic 重启后 ok；无复位 |
| `agent_stall 0` | critical → 软复位 | ✅ `task_stall/2` critical → 软复位；`boot=3, reason=health_auto, last_anomaly=task_stall` |
| `/api/health/ack` | 清预算 | ✅ `auto_reset_count` 清零，`needs_config` 清除 |
| BBRAM 回读 | persist_ok | ✅ `persist_ok=true` |
| WS health 告警 | starting/ok/degraded/critical | ✅ `[health]` 行按预期出现 |
| 主机 watchdog 协同 | 恢复期间不重复复位 | ✅ 约 4 分钟缩短空跑（替代 1 h）：3 次瞬时探针失败均由设备自恢复，0 次 stall/复位，`boot=1` 不变；1 h soak 未跑 |

## 七、HIL 发现并修复的问题

1. **agent_loop 空闲误判**（11-03）：空闲时 `message_bus_pop_inbound(..., UINT32_MAX)`
   只 beat 一次就永久阻塞，60 s 后被判 `task_stall` 并软复位。改为 5 s 超时 beat。
2. **WS listener 重启挂死**（11-03）：`pthread_join` 无限等待可能卡死的 accept 线程，
   把健康线程一起挂死。改为最多等 2 s，未退出则放弃本次重启。
3. **LLM/tool busy age 溢出**（11-03）：空闲时 `now - 0` 被当成 4294967 s，
   误触发 `LLM_SLOW`。busy 时间戳为 0 时输出 0。
4. **heap 斜率窗口过早**（11-03）：30 s 就出斜率，噪声大；改为历史窗口 ≥240 s。
5. **WS_TX_FAIL 单音频流难触发**（11-03）：增加 60 s 内绝对失败数；注入期间 mic 不因首帧失败退出。
6. **NET_DOWN 永不触发**（11-04）：规则和复位门禁读的是内部快照的 `net_down_s`，
   而该字段只在 `health_monitor_get()` 里派生，采样时从未赋值。修复后实测 41 s 触发。
7. **心跳负年龄**（11-04）：采样起点 `now` 早于某些心跳的读取，`beats.lvgl` 会显示
   4294967265 这类回绕值。`health_age_ms()` 统一把「未来时间戳」钳到 0。
8. **WS accept 的 poll 超时/post 竞态**（11-04）：HIL 中 accept 线程多次因
   `poll(500 ms)` 的 timed sem 卡住 20 s 以上，健康系统误判 `ws_accept_dead` 并复位。
   改为**非阻塞 accept + usleep(100 ms)**，消除「网络 post 与超时同时发生」的路径；
   修改后 6 min 基线 accept 心跳稳定。
9. **cron/mic 心跳占位**（11-04）：`beats.cron/mic` 原为 `0xFFFFFFFF` 占位，
   现分别填 `cron.beat_age_ms` / `mic.last_frame_age_ms`，诊断输出不再误导。
10. **状态页中文显示成方框**（11-04）：两个原因叠加。
    ① UI 用的 U+00B7 中点在 GB2312 字体里没有字形，换成字体已包含的 `•`（U+2022）；
    ② 状态页第二行标签用的是 Montserrat 14（只有拉丁字形），正常态的
    「内存 / 任务 / 连接」和故障态的「严重 / 警告」都没有字形。改为 CJK 字体，
    并把 banner 高度 44→56、arc/卡片/按钮各下移 12 px，容纳两行 23 px 文本。
    解析字体 cmap 后校验 UI 字符串中的 109 个非 ASCII 字符全部有字形。

## 八、已知限制与未完成项

- **内核竞态未修**：`sem_waitirq.c:137` 的「超时 vs post」竞态仍在。健康系统只能
  发现症状并恢复；accept 循环已用非阻塞方式绕开，但 `message_bus` 等 timed wait
  仍可能中招。根因修复属内核范围，不在本作品。
- **safe_mode 完整 HIL 未跑**：需要连续 3 次永久故障复位，且第 2 次起要求 uptime ≥ 300 s，
  完整约 15–20 分钟。代码路径已静态复核：boot 从 BBRAM 恢复预算、预算耗尽后只置
  `safe_mode` 不再复位、`ack` 清零并 `health_store_clear_auto_reset()` 写回 BBRAM；
  端到端的 3 次复位序列留作后续。
- **主机 watchdog 只做了约 4 分钟缩短空跑**：期间 3 次瞬时探针失败均由设备自恢复，
  未达到 3 次连续失败，未触发主机复位；1 h 长跑仍待补。
- **UI 视觉未人工确认**：状态栏圆点与状态页横幅已编译进固件，需看屏确认。
- **BBRAM 不跨掉电**：RTC 域掉电即丢；软复位可保持。生产中若需跨掉电，应落 NVS。
- **软复位丢 /data**：由主机重新下发配置并 ack 兜底，这是演示架构的已知取舍。

## 九、如何复测

```bash
# 基线
curl -sS http://192.168.223.2:28789/api/health | python3 -m json.tool

# 故障注入（仅 HIL 固件）
~/health_fault.sh health
~/health_fault.sh mic_read_fail 3
~/health_fault.sh agent_stall 45
~/health_fault.sh net_down 60
~/health_fault.sh ws_send_fail 20
~/health_fault.sh heap_hog 7000
~/health_fault.sh agent_stall 0      # 永久，约 70 s 后软复位
~/health_fault.sh fault_off

# 设备自己软复位后（不要再用 RTS 复位，否则会覆盖 reset_reason）
bash ~/voice_ready.sh --no-reset

# 主机 watchdog
bash ~/pipeline_watchdog.sh
```

构建开关（`vendor/sifli/.../configs/agent/defconfig`）：

```
CONFIG_AI_AGENT_HEALTH=y
CONFIG_AI_AGENT_HEALTH_TEST=y
CONFIG_AI_AGENT_HEALTH_PERSIST=y
CONFIG_AI_AGENT_HEALTH_AUTORESET=y
CONFIG_BOARDCTL_RESET=y
CONFIG_BOARDCTL_RESET_CAUSE=y
```

修改 defconfig 后必须删除 `cmake_out/sf32lb52_lchspi_ulp_agent/` 再构建。
