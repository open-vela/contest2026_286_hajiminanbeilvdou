/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/**
 * health_monitor.h — device health snapshot, rules and recovery.
 *
 * One sampler thread owns all judgement.  Services publish counters, gauges
 * and heartbeats; consumers (REST, UI, tools, host watchdog) only read the
 * published snapshot, so a slow reader can never hold up a producer.
 */

#include "agent_compat.h"
#include "agent_config.h"

#include <stdbool.h>
#include <stdint.h>

#define HEALTH_ISSUE_MAX 8
#define HEALTH_ISSUE_NAME_LEN 24

typedef enum {
    HEALTH_STATE_STARTING = 0,
    HEALTH_STATE_OK,
    HEALTH_STATE_DEGRADED,
    HEALTH_STATE_CRITICAL,
} health_state_t;

typedef enum {
    HEALTH_SEV_INFO = 0,
    HEALTH_SEV_WARNING,
    HEALTH_SEV_CRITICAL,
} health_severity_t;

/* Heartbeats published by the threads that have no natural stats owner.
 * Services that already expose a stats getter (ws, mic, cron) carry their
 * own last-activity timestamps instead.
 */
typedef enum {
    HEALTH_BEAT_OUTBOUND = 0,
    HEALTH_BEAT_AGENT_IDLE,
    HEALTH_BEAT_AGENT_LLM,
    HEALTH_BEAT_AGENT_TOOL,
    HEALTH_BEAT_NET,
    HEALTH_BEAT_CRON,
    HEALTH_BEAT_WS_ACCEPT,
    HEALTH_BEAT_MIC,
    HEALTH_BEAT_LVGL,
    HEALTH_BEAT_COUNT
} health_beat_id_t;

/* Issue codes are stable: they travel to the host and into the RTC anomaly
 * record, so append only.
 */
typedef enum {
    HEALTH_ISSUE_NONE = 0,
    HEALTH_ISSUE_HEAP_LOW,
    HEALTH_ISSUE_HEAP_FRAG,
    HEALTH_ISSUE_HEAP_TREND,
    HEALTH_ISSUE_TASK_GROWTH,
    HEALTH_ISSUE_TASK_STALL,
    HEALTH_ISSUE_WS_TX_FAIL,
    HEALTH_ISSUE_WS_ACCEPT_DEAD,
    HEALTH_ISSUE_MIC_STALL,
    HEALTH_ISSUE_MIC_ERR,
    HEALTH_ISSUE_MIC_OVERRUN,
    HEALTH_ISSUE_NET_DOWN,
    HEALTH_ISSUE_NET_RTT,
    HEALTH_ISSUE_CRON_STALL,
    HEALTH_ISSUE_LLM_FAIL,
    HEALTH_ISSUE_LLM_SLOW,
    HEALTH_ISSUE_BUS_BACKLOG,
    HEALTH_ISSUE_PERSIST,
    HEALTH_ISSUE_COUNT
} health_issue_code_t;

typedef struct {
    uint8_t code;
    uint8_t severity;
    uint16_t streak;
    uint32_t since_ms;
    char detail[16]; /* which thread/backend, when the code alone is vague */
} health_issue_t;

typedef struct {
    /* Meta */
    health_state_t state;
    uint32_t seq;
    uint32_t sample_ms;
    uint32_t uptime_s;
    bool stale;
    bool grace;
    bool services_started;
    bool safe_mode;
    bool needs_config;
    bool reboot_pending;
    bool handling;
    bool persist_ok;
    uint8_t boot_count;
    uint8_t auto_reset_count;
    uint8_t reset_reason;

    /* Heap */
    uint32_t heap_total;
    uint32_t heap_free;
    uint32_t heap_used;
    uint32_t heap_largest;
    uint32_t heap_free_min;
    uint32_t heap_ewma;
    int32_t heap_slope_kb_per_min;

    /* Tasks */
    uint16_t task_count;
    uint16_t task_baseline;
    uint8_t min_prio;

    /* Network */
    bool net_up;
    bool net_ever_up;
    uint32_t net_down_s;
    uint32_t net_last_up_age_s;
    char net_ip[16];

    /* WebSocket server */
    bool ws_listening;
    uint32_t ws_clients;
    uint32_t ws_accept_age_ms;
    uint32_t ws_tx_ok;
    uint32_t ws_tx_fail;
    uint32_t ws_tx_timeout;
    uint32_t ws_rx_frames;
    uint32_t ws_client_drops;

    /* Microphone stream */
    bool mic_want_running;
    bool mic_running;
    uint32_t mic_frames;
    uint32_t mic_bytes;
    uint32_t mic_read_errors;
    uint32_t mic_send_errors;
    uint32_t mic_overruns;
    uint32_t mic_restarts;
    uint32_t mic_last_frame_age_ms;

    /* Cron */
    bool cron_running;
    uint16_t cron_jobs;
    uint32_t cron_beat_age_ms;
    uint32_t cron_last_fire_age_s;
    uint32_t cron_fires;
    uint32_t cron_errors;

    /* LLM router */
    uint32_t llm_calls;
    uint32_t llm_failures;
    uint32_t llm_avg_latency_ms;
    uint8_t llm_backends;
    uint8_t llm_backends_backoff;
    uint32_t llm_busy_age_s;
    uint32_t tool_busy_age_s;

    /* Message bus */
    uint16_t bus_in_depth;
    uint16_t bus_out_depth;
    uint32_t bus_in_drops;
    uint32_t bus_out_drops;

    /* Heartbeat ages, indexed by health_beat_id_t */
    uint32_t beat_age_ms[HEALTH_BEAT_COUNT];

    /* Issues */
    uint8_t issue_count;
    health_issue_t issues[HEALTH_ISSUE_MAX];
    uint8_t last_issue_code;
    uint8_t last_severity;
    uint32_t last_anomaly_uptime_s;

    /* Recovery */
    uint8_t recovery_attempts;
} health_snapshot_t;

/* ── Lifecycle ─────────────────────────────────────────────────── */

#ifdef CONFIG_AI_AGENT_HEALTH

/** Initialise state and read the RTC boot record.  Call once, early. */
int health_monitor_init(void);

/** Start the sampler thread.  Call once all services have been created. */
int health_monitor_start(void);

/** Stop and join the sampler thread. */
void health_monitor_stop(void);

bool health_monitor_is_running(void);

/** Monotonic seconds since health_monitor_init(). */
uint32_t health_monitor_uptime_s(void);

/* ── Snapshot ──────────────────────────────────────────────────── */

/** Copy the last published snapshot.  Safe from any thread. */
void health_monitor_get(health_snapshot_t *out);

/** Human-readable status name ("starting", "ok", ...). */
const char *health_monitor_state_name(health_state_t state);

/** Human-readable issue name ("heap_low", ...). */
const char *health_monitor_issue_name(uint8_t code);

/** Human-readable reset reason ("power_on", "health_auto", ...). */
const char *health_monitor_reset_reason_name(uint8_t reason);

/* ── Producer interface ────────────────────────────────────────── */

void health_monitor_beat(health_beat_id_t id);
void health_monitor_beat_busy(health_beat_id_t id, bool busy);

/** Note that all network-dependent services have been started. */
void health_monitor_notify_services_started(void);

/** Configuration has been (re)applied by the host. */
void health_monitor_notify_config_ok(void);

/** Host acknowledged recovery; clears safe_mode/auto_reset budget. */
void health_monitor_notify_host_ack(void);

/** Request a recovery action from another thread (executed by sampler). */
void health_monitor_request_restart(health_beat_id_t id);

#else /* !CONFIG_AI_AGENT_HEALTH */

static inline int health_monitor_init(void)
{
    return OK;
}

static inline int health_monitor_start(void)
{
    return OK;
}

static inline void health_monitor_stop(void)
{
}

static inline bool health_monitor_is_running(void)
{
    return false;
}

static inline uint32_t health_monitor_uptime_s(void)
{
    return 0;
}

static inline void health_monitor_get(health_snapshot_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->state = HEALTH_STATE_OK;
    }
}

static inline const char *health_monitor_state_name(health_state_t state)
{
    (void)state;
    return "ok";
}

static inline const char *health_monitor_issue_name(uint8_t code)
{
    (void)code;
    return "none";
}

static inline const char *health_monitor_reset_reason_name(uint8_t reason)
{
    (void)reason;
    return "unknown";
}

static inline void health_monitor_beat(health_beat_id_t id)
{
    (void)id;
}

static inline void health_monitor_beat_busy(health_beat_id_t id, bool busy)
{
    (void)id;
    (void)busy;
}

static inline void health_monitor_notify_services_started(void)
{
}

static inline void health_monitor_notify_config_ok(void)
{
}

static inline void health_monitor_notify_host_ack(void)
{
}

static inline void health_monitor_request_restart(health_beat_id_t id)
{
    (void)id;
}

#endif /* CONFIG_AI_AGENT_HEALTH */

/* ── Fault injection (HIL only) ────────────────────────────────── */

#ifdef CONFIG_AI_AGENT_HEALTH_TEST

typedef enum {
    HEALTH_FAULT_NONE = 0,
    HEALTH_FAULT_AGENT_STALL,
    HEALTH_FAULT_MIC_READ_FAIL,
    HEALTH_FAULT_WS_SEND_FAIL,
    HEALTH_FAULT_HEAP_HOG,
    HEALTH_FAULT_NET_DOWN,
} health_fault_t;

int health_monitor_fault_set(const char *name, uint32_t seconds,
    uint32_t count, uint32_t kb);

bool health_monitor_fault_agent_stall(void);
bool health_monitor_fault_take_mic_fail(void);
bool health_monitor_fault_take_ws_fail(void);
bool health_monitor_fault_net_down(void);

/** True while a ws_send_fail injection still has failures left.  The mic
 *  thread uses this to keep sending through the injection instead of
 *  stopping on the first failed frame.
 */
bool health_monitor_fault_ws_active(void);

#endif /* CONFIG_AI_AGENT_HEALTH_TEST */
