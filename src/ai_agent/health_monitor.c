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

#include "core/health_monitor.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "channels/mic_stream.h"
#include "channels/ws_server.h"
#include "core/message_bus.h"
#include "infra/cron_service.h"
#include "infra/health_store.h"
#include "infra/network_manager.h"
#include "llm/llm_router.h"

#include <malloc.h>
#include <nuttx/sched.h>
#include <string.h>
#include <time.h>

#ifdef CONFIG_AI_AGENT_HEALTH_AUTORESET
#include <sys/boardctl.h>
#endif

static const char *TAG = "health";

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static health_snapshot_t s_snap;

static volatile uint32_t s_beat_ms[HEALTH_BEAT_COUNT];
static volatile uint32_t s_busy_since_ms[HEALTH_BEAT_COUNT];
static volatile bool s_restart_request[HEALTH_BEAT_COUNT];

static volatile bool s_running;
static bool s_started;
static pthread_t s_thread;
static uint32_t s_boot_ms;
static bool s_services_started;
static bool s_safe_mode;
static bool s_net_ever_up;
static uint32_t s_net_last_up_ms;
static bool s_prev_mic_want;
static uint32_t s_mic_want_since_ms;
static uint8_t s_recovery_attempts;
static uint32_t s_heap_free_min;
static uint16_t s_task_baseline;
static uint8_t s_auto_reset_count;
static bool s_needs_config;

/* Recovery budgets.  Both services get three attempts with increasing
 * spacing; only after that does the issue become reset-class.
 */
static uint8_t s_mic_restart_attempts;
static uint32_t s_mic_last_restart_ms;
static uint8_t s_ws_restart_attempts;
static uint32_t s_ws_last_restart_ms;

/* An issue has to fire on consecutive samples and clear on consecutive
 * healthy samples; the gap between the two is the hysteresis that keeps a
 * one-sample spike from waking the screen or the host.
 */
#define HEALTH_ISSUE_CLEAR_SAMPLES 3

/* 64 samples at the default 5 s interval cover a little over five minutes,
 * which is the window every trend rule below is expressed in.
 */
#define HEALTH_HISTORY_LEN 64

typedef struct {
    uint32_t ms;
    uint32_t heap_free;
    uint32_t llm_calls;
    uint32_t llm_failures;
    uint32_t ws_ok;
    uint32_t ws_fail;
    uint32_t ws_total;
    uint32_t mic_frames;
    uint32_t mic_read_errors;
    uint32_t mic_send_errors;
} health_hist_t;

typedef struct {
    uint8_t severity;
    uint16_t streak;
    uint16_t clear;
    uint32_t since_ms;
    char detail[16];
} health_issue_state_t;

static health_hist_t s_hist[HEALTH_HISTORY_LEN];
static int s_hist_head;
static int s_hist_count;

static health_issue_state_t s_issue_state[HEALTH_ISSUE_COUNT];
static health_state_t s_last_state = HEALTH_STATE_STARTING;
static uint32_t s_last_alert_ms;

static uint32_t health_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint32_t)ts.tv_sec * 1000u
        + (uint32_t)ts.tv_nsec / 1000000u);
}

static uint32_t health_age_ms(uint32_t now, uint32_t then)
{
    uint32_t age;

    if (then == 0) {
        return UINT32_MAX;
    }

    age = now - then;

    /* A producer can beat between the start of the sample and the moment
     * this field is read; a wrapped value would otherwise show up as a
     * multi-decade stall in /api/health.
     */
    return (int32_t)age < 0 ? 0 : age;
}

uint32_t health_monitor_uptime_s(void)
{
    return (health_now_ms() - s_boot_ms) / 1000u;
}

const char *health_monitor_state_name(health_state_t state)
{
    switch (state) {
    case HEALTH_STATE_STARTING:
        return "starting";
    case HEALTH_STATE_OK:
        return "ok";
    case HEALTH_STATE_DEGRADED:
        return "degraded";
    case HEALTH_STATE_CRITICAL:
        return "critical";
    default:
        return "unknown";
    }
}

const char *health_monitor_issue_name(uint8_t code)
{
    switch (code) {
    case HEALTH_ISSUE_NONE:
        return "none";
    case HEALTH_ISSUE_HEAP_LOW:
        return "heap_low";
    case HEALTH_ISSUE_HEAP_FRAG:
        return "heap_frag";
    case HEALTH_ISSUE_HEAP_TREND:
        return "heap_trend";
    case HEALTH_ISSUE_TASK_GROWTH:
        return "task_growth";
    case HEALTH_ISSUE_TASK_STALL:
        return "task_stall";
    case HEALTH_ISSUE_WS_TX_FAIL:
        return "ws_tx_fail";
    case HEALTH_ISSUE_WS_ACCEPT_DEAD:
        return "ws_accept_dead";
    case HEALTH_ISSUE_MIC_STALL:
        return "mic_stall";
    case HEALTH_ISSUE_MIC_ERR:
        return "mic_err";
    case HEALTH_ISSUE_MIC_OVERRUN:
        return "mic_overrun";
    case HEALTH_ISSUE_NET_DOWN:
        return "net_down";
    case HEALTH_ISSUE_NET_RTT:
        return "net_rtt";
    case HEALTH_ISSUE_CRON_STALL:
        return "cron_stall";
    case HEALTH_ISSUE_LLM_FAIL:
        return "llm_fail";
    case HEALTH_ISSUE_LLM_SLOW:
        return "llm_slow";
    case HEALTH_ISSUE_BUS_BACKLOG:
        return "bus_backlog";
    case HEALTH_ISSUE_PERSIST:
        return "persist";
    default:
        return "unknown";
    }
}

const char *health_monitor_reset_reason_name(uint8_t reason)
{
    switch (reason) {
    case HEALTH_RESET_UNKNOWN:
        return "unknown";
    case HEALTH_RESET_POWER_ON:
        return "power_on";
    case HEALTH_RESET_PIN:
        return "pin";
    case HEALTH_RESET_USER:
        return "user";
    case HEALTH_RESET_HEALTH_AUTO:
        return "health_auto";
    case HEALTH_RESET_WATCHDOG:
        return "watchdog";
    case HEALTH_RESET_LOWPOWER:
        return "lowpower";
    case HEALTH_RESET_HOST:
        return "host_reset";
    default:
        return "unknown";
    }
}

/* ── Sampling ─────────────────────────────────────────────────── */

typedef struct {
    uint16_t count;
    uint8_t min_prio;
} task_walk_t;

static void task_walk_cb(struct tcb_s *tcb, void *arg)
{
    task_walk_t *walk = (task_walk_t *)arg;

    if (tcb == NULL || walk == NULL) {
        return;
    }

    walk->count++;
    if ((uint8_t)tcb->sched_priority < walk->min_prio) {
        walk->min_prio = (uint8_t)tcb->sched_priority;
    }
}

/* ── History windows ──────────────────────────────────────────── */

static void hist_push(const health_snapshot_t *s)
{
    health_hist_t *h = &s_hist[s_hist_head];

    h->ms = s->sample_ms;
    h->heap_free = s->heap_free;
    h->llm_calls = s->llm_calls;
    h->llm_failures = s->llm_failures;
    h->ws_ok = s->ws_tx_ok;
    h->ws_fail = s->ws_tx_fail + s->ws_tx_timeout;
    h->ws_total = s->ws_tx_ok + s->ws_tx_fail + s->ws_tx_timeout;
    h->mic_frames = s->mic_frames;
    h->mic_read_errors = s->mic_read_errors;
    h->mic_send_errors = s->mic_send_errors;

    s_hist_head = (s_hist_head + 1) % HEALTH_HISTORY_LEN;
    if (s_hist_count < HEALTH_HISTORY_LEN) {
        s_hist_count++;
    }
}

/* Sample n slots before the one just pushed; n is clamped to the oldest
 * sample that exists.  The caller still checks how far back it really got.
 */
static const health_hist_t *hist_n_back(int n)
{
    int idx;

    if (s_hist_count == 0) {
        return NULL;
    }
    if (n >= s_hist_count) {
        n = s_hist_count - 1;
    }
    idx = (s_hist_head - 1 - n + HEALTH_HISTORY_LEN * 2)
        % HEALTH_HISTORY_LEN;
    return &s_hist[idx];
}

static uint32_t counter_delta(uint32_t now, uint32_t before)
{
    return now - before; /* both wrap modulo 2^32 */
}

/* ── Rules ────────────────────────────────────────────────────── */

static void issue_update(uint8_t code, uint8_t severity, uint8_t debounce,
    uint32_t now, const char *detail)
{
    health_issue_state_t *st;

    if (code == HEALTH_ISSUE_NONE || code >= HEALTH_ISSUE_COUNT) {
        return;
    }

    st = &s_issue_state[code];

    if (severity == HEALTH_SEV_INFO) {
        st->streak = 0;
        if (st->severity != 0) {
            if (++st->clear >= HEALTH_ISSUE_CLEAR_SAMPLES) {
                st->severity = HEALTH_SEV_INFO;
                st->clear = 0;
                st->detail[0] = '\0';
            }
        }
        return;
    }

    st->clear = 0;
    if (st->streak < UINT16_MAX) {
        st->streak++;
    }
    if (st->streak < debounce) {
        return;
    }

    if (severity > st->severity) {
        bool was_active = st->severity != HEALTH_SEV_INFO;

        st->severity = severity;
        st->since_ms = now;
        if (detail != NULL) {
            strncpy(st->detail, detail, sizeof(st->detail) - 1);
            st->detail[sizeof(st->detail) - 1] = '\0';
        }

        if (!was_active) {
            health_store_note_anomaly(code, severity, false);
        } else {
            /* Escalation is worth recording even though the issue is not
             * new: the reset decision depends on the severity.
             */
            health_store_note_anomaly(code, severity, false);
        }
    } else if (detail != NULL && st->detail[0] == '\0') {
        strncpy(st->detail, detail, sizeof(st->detail) - 1);
        st->detail[sizeof(st->detail) - 1] = '\0';
    }
}

static uint8_t stall_severity(uint32_t age_ms, uint32_t warn_s,
    uint32_t crit_s)
{
    if (age_ms == UINT32_MAX) {
        return HEALTH_SEV_CRITICAL; /* never beat at all */
    }
    if (age_ms >= crit_s * 1000u) {
        return HEALTH_SEV_CRITICAL;
    }
    if (age_ms >= warn_s * 1000u) {
        return HEALTH_SEV_WARNING;
    }
    return HEALTH_SEV_INFO;
}

static void health_eval_rules(health_snapshot_t *s)
{
    const health_hist_t *old;
    uint32_t now = s->sample_ms;
    bool heap_rules = s->uptime_s >= AGENT_HEALTH_GRACE_S + 60u;
    bool llm_busy = s->llm_busy_age_s > 0;
    bool tool_busy = s->tool_busy_age_s > 0;
    uint8_t sev;
    uint8_t worst;
    const char *worst_detail;

    if (s->grace) {
        return;
    }

    /* ── Heap ─────────────────────────────────────────────────── */

    if (heap_rules && s->heap_total > 0) {
        uint32_t warn = s->heap_total / 16u;
        uint32_t crit = s->heap_total / 64u;

        if (warn < 512u * 1024u) {
            warn = 512u * 1024u;
        }
        if (crit < 128u * 1024u) {
            crit = 128u * 1024u;
        }

        sev = s->heap_free < crit ? HEALTH_SEV_CRITICAL
            : s->heap_free < warn ? HEALTH_SEV_WARNING : HEALTH_SEV_INFO;
        issue_update(HEALTH_ISSUE_HEAP_LOW, sev,
            sev == HEALTH_SEV_CRITICAL ? 1 : 2, now, NULL);
    } else {
        issue_update(HEALTH_ISSUE_HEAP_LOW, HEALTH_SEV_INFO, 1, now, NULL);
    }

    if (heap_rules
        && ((s->heap_largest < 64u * 1024u && s->heap_free > 512u * 1024u)
            || (s->heap_free > 0
                && s->heap_largest * 10u < s->heap_free))) {
        issue_update(HEALTH_ISSUE_HEAP_FRAG, HEALTH_SEV_WARNING, 3, now,
            NULL);
    } else {
        issue_update(HEALTH_ISSUE_HEAP_FRAG, HEALTH_SEV_INFO, 1, now, NULL);
    }

    if (heap_rules && s->heap_slope_kb_per_min < -1024) {
        issue_update(HEALTH_ISSUE_HEAP_TREND, HEALTH_SEV_CRITICAL, 3, now,
            NULL);
    } else if (heap_rules && s->heap_slope_kb_per_min < -256) {
        issue_update(HEALTH_ISSUE_HEAP_TREND, HEALTH_SEV_WARNING, 3, now,
            NULL);
    } else {
        issue_update(HEALTH_ISSUE_HEAP_TREND, HEALTH_SEV_INFO, 1, now, NULL);
    }

    /* ── Tasks ────────────────────────────────────────────────── */

    if (heap_rules && s->task_baseline > 0 && s->task_count
        > s->task_baseline + 16) {
        issue_update(HEALTH_ISSUE_TASK_GROWTH, HEALTH_SEV_CRITICAL, 3, now,
            NULL);
    } else if (heap_rules && s->task_baseline > 0 && s->task_count
        > s->task_baseline + 8) {
        issue_update(HEALTH_ISSUE_TASK_GROWTH, HEALTH_SEV_WARNING, 3, now,
            NULL);
    } else {
        issue_update(HEALTH_ISSUE_TASK_GROWTH, HEALTH_SEV_INFO, 1, now,
            NULL);
    }

    /* Thread stalls, aggregated into one issue with the worst offender in
     * detail.  The idle heartbeat is only meaningful while the agent is
     * genuinely idle; during an LLM or tool call it is supposed to be old.
     */

    worst = HEALTH_SEV_INFO;
    worst_detail = NULL;

    sev = stall_severity(s->beat_age_ms[HEALTH_BEAT_OUTBOUND], 30, 60);
    if (sev > worst) {
        worst = sev;
        worst_detail = "outbound";
    }

    if (s->services_started && !llm_busy && !tool_busy) {
        sev = stall_severity(s->beat_age_ms[HEALTH_BEAT_AGENT_IDLE], 30, 60);
        if (sev > worst) {
            worst = sev;
            worst_detail = "agent_idle";
        }
    }

    if (s->uptime_s > 30) {
        sev = stall_severity(s->beat_age_ms[HEALTH_BEAT_NET], 10, 30);
        if (sev > worst) {
            worst = sev;
            worst_detail = "net_watch";
        }
    }

    if (tool_busy) {
        sev = stall_severity(s->tool_busy_age_s * 1000u, 120, 300);
        if (sev > worst) {
            worst = sev;
            worst_detail = "tool";
        }
    }

    if (s->beat_age_ms[HEALTH_BEAT_LVGL] >= 5000u
        && s->beat_age_ms[HEALTH_BEAT_LVGL] != UINT32_MAX) {
        sev = HEALTH_SEV_WARNING; /* display only: never a reset reason */
        if (sev > worst) {
            worst = sev;
            worst_detail = "lvgl";
        }
    }

    issue_update(HEALTH_ISSUE_TASK_STALL, worst, 2, now, worst_detail);

    /* ── WebSocket ────────────────────────────────────────────── */

    if (s->services_started) {
        if (!s->ws_listening) {
            sev = HEALTH_SEV_CRITICAL;
        } else {
            sev = stall_severity(s->beat_age_ms[HEALTH_BEAT_WS_ACCEPT],
                10, 30);
        }
        issue_update(HEALTH_ISSUE_WS_ACCEPT_DEAD, sev, 2, now, NULL);
    } else {
        issue_update(HEALTH_ISSUE_WS_ACCEPT_DEAD, HEALTH_SEV_INFO, 1, now,
            NULL);
    }

    old = hist_n_back(12);
    if (s->services_started && s->ws_listening && old != NULL
        && now > old->ms + 20000u) {
        uint32_t total = counter_delta(s->ws_tx_ok + s->ws_tx_fail
            + s->ws_tx_timeout, old->ws_total);
        uint32_t failed = counter_delta(s->ws_tx_fail + s->ws_tx_timeout,
            old->ws_fail);

        /* Rate alone is not enough: a busy audio stream can absorb a long
         * run of failures and still look like a small percentage.  An
         * absolute count catches a peer that has stopped reading.
         */
        if ((total >= 10 && failed * 2u > total) || failed >= 30) {
            sev = HEALTH_SEV_CRITICAL;
        } else if ((total >= 5 && failed * 5u > total) || failed >= 10) {
            sev = HEALTH_SEV_WARNING;
        } else {
            sev = HEALTH_SEV_INFO;
        }
        issue_update(HEALTH_ISSUE_WS_TX_FAIL, sev, 2, now, NULL);
    } else {
        issue_update(HEALTH_ISSUE_WS_TX_FAIL, HEALTH_SEV_INFO, 1, now,
            NULL);
    }

    /* ── Microphone ───────────────────────────────────────────── */

    if (s->mic_want_running) {
        uint32_t age = s->mic_last_frame_age_ms;

        if (!s->mic_running) {
            sev = HEALTH_SEV_CRITICAL;
        } else if (s->mic_frames == 0 && s_mic_want_since_ms != 0
            && now - s_mic_want_since_ms < 5000u) {
            sev = HEALTH_SEV_INFO; /* codec open + first block */
        } else {
            sev = stall_severity(age, 3, 10);
        }
        issue_update(HEALTH_ISSUE_MIC_STALL, sev, 1, now, NULL);
    } else {
        issue_update(HEALTH_ISSUE_MIC_STALL, HEALTH_SEV_INFO, 1, now, NULL);
    }

    old = hist_n_back(12);
    if (s->mic_want_running && s->mic_running && old != NULL
        && now > old->ms + 20000u) {
        uint32_t read_errors = counter_delta(s->mic_read_errors,
            old->mic_read_errors);
        uint32_t frames = counter_delta(s->mic_frames, old->mic_frames);
        uint32_t send_errors = counter_delta(s->mic_send_errors,
            old->mic_send_errors);

        if (read_errors > 3
            || (frames + send_errors >= 10
                && send_errors * 5u > frames + send_errors)) {
            sev = HEALTH_SEV_WARNING;
        } else {
            sev = HEALTH_SEV_INFO;
        }
        issue_update(HEALTH_ISSUE_MIC_ERR, sev, 2, now, NULL);
    } else {
        issue_update(HEALTH_ISSUE_MIC_ERR, HEALTH_SEV_INFO, 1, now, NULL);
    }

    issue_update(HEALTH_ISSUE_MIC_OVERRUN, HEALTH_SEV_INFO, 1, now, NULL);

    /* ── Network ──────────────────────────────────────────────── */

    if (s->net_ever_up && !s->net_up && s->net_down_s > 180u) {
        issue_update(HEALTH_ISSUE_NET_DOWN, HEALTH_SEV_CRITICAL, 2, now,
            NULL);
    } else if (s->net_ever_up && !s->net_up && s->net_down_s > 30u) {
        issue_update(HEALTH_ISSUE_NET_DOWN, HEALTH_SEV_WARNING, 2, now,
            NULL);
    } else {
        issue_update(HEALTH_ISSUE_NET_DOWN, HEALTH_SEV_INFO, 1, now, NULL);
    }

    issue_update(HEALTH_ISSUE_NET_RTT, HEALTH_SEV_INFO, 1, now, NULL);

    /* ── Cron ─────────────────────────────────────────────────── */

    if (s->cron_running) {
        sev = stall_severity(s->cron_beat_age_ms, 60, 300);
        issue_update(HEALTH_ISSUE_CRON_STALL, sev, 2, now, NULL);
    } else {
        issue_update(HEALTH_ISSUE_CRON_STALL, HEALTH_SEV_INFO, 1, now, NULL);
    }

    /* ── LLM ──────────────────────────────────────────────────── */

    old = hist_n_back(60);
    if (s->net_up && s->llm_backends > 0 && old != NULL
        && now > old->ms + 240000u) {
        uint32_t calls = counter_delta(s->llm_calls, old->llm_calls);
        uint32_t failures = counter_delta(s->llm_failures,
            old->llm_failures);
        uint32_t total = calls + failures;

        if ((failures >= 3 && total > 0 && failures * 2u > total)
            || (s->llm_backends_backoff == s->llm_backends)) {
            sev = HEALTH_SEV_WARNING; /* cloud problems never reset us */
        } else {
            sev = HEALTH_SEV_INFO;
        }
        issue_update(HEALTH_ISSUE_LLM_FAIL, sev, 3, now, NULL);
    } else {
        issue_update(HEALTH_ISSUE_LLM_FAIL, HEALTH_SEV_INFO, 1, now, NULL);
    }

    if (llm_busy) {
        sev = stall_severity(s->llm_busy_age_s * 1000u, 300, 960);
        issue_update(HEALTH_ISSUE_LLM_SLOW, sev, 1, now, NULL);
    } else {
        issue_update(HEALTH_ISSUE_LLM_SLOW, HEALTH_SEV_INFO, 1, now, NULL);
    }

    /* ── Message bus ──────────────────────────────────────────── */

    if (s->bus_out_depth > 12) {
        issue_update(HEALTH_ISSUE_BUS_BACKLOG, HEALTH_SEV_WARNING,
            (uint8_t)(60000u / AGENT_HEALTH_INTERVAL_MS), now, NULL);
    } else {
        issue_update(HEALTH_ISSUE_BUS_BACKLOG, HEALTH_SEV_INFO, 1, now,
            NULL);
    }

    /* ── Persistence ──────────────────────────────────────────── */

#ifdef CONFIG_AI_AGENT_HEALTH_PERSIST
    issue_update(HEALTH_ISSUE_PERSIST, s->persist_ok ? HEALTH_SEV_INFO
        : HEALTH_SEV_WARNING, 3, now, NULL);
#else
    issue_update(HEALTH_ISSUE_PERSIST, HEALTH_SEV_INFO, 1, now, NULL);
#endif
}

static void health_publish_issues(health_snapshot_t *s)
{
    uint8_t worst = HEALTH_SEV_INFO;

    s->issue_count = 0;

    /* Two passes so the snapshot's first entry is always the worst one:
     * the UI and the alert line show issues[0] and nothing else.
     */
    for (uint8_t pass = HEALTH_SEV_CRITICAL; pass >= HEALTH_SEV_WARNING;
         pass--) {
        for (uint8_t code = HEALTH_ISSUE_NONE + 1; code < HEALTH_ISSUE_COUNT;
             code++) {
            if (s_issue_state[code].severity != pass) {
                continue;
            }

            if (s->issue_count < HEALTH_ISSUE_MAX) {
                health_issue_t *dst = &s->issues[s->issue_count++];

                dst->code = code;
                dst->severity = s_issue_state[code].severity;
                dst->streak = s_issue_state[code].streak;
                dst->since_ms = s_issue_state[code].since_ms;
                strncpy(dst->detail, s_issue_state[code].detail,
                    sizeof(dst->detail) - 1);
                dst->detail[sizeof(dst->detail) - 1] = '\0';
            }

            if (pass > worst) {
                worst = pass;
            }
        }
    }

    if (s->grace) {
        s->state = HEALTH_STATE_STARTING;
    } else if (worst >= HEALTH_SEV_CRITICAL) {
        s->state = HEALTH_STATE_CRITICAL;
    } else if (worst >= HEALTH_SEV_WARNING) {
        s->state = HEALTH_STATE_DEGRADED;
    } else {
        s->state = HEALTH_STATE_OK;
    }

    if (s->issue_count > 0) {
        s->last_issue_code = s->issues[0].code;
        s->last_severity = s->issues[0].severity;
        s->last_anomaly_uptime_s = s->issues[0].since_ms / 1000u;
    }
}

static void health_alert_maybe(const health_snapshot_t *s, uint32_t now,
    bool force)
{
    ws_server_stats_t ws;
    char json[384];
    bool transition = s->state != s_last_state;
    bool due;

    ws_server_get_stats(&ws);

    due = force || transition
        || (s->state != HEALTH_STATE_OK
            && now - s_last_alert_ms >= 60000u)
        || (s->state == HEALTH_STATE_OK
            && now - s_last_alert_ms >= 300000u);

    if (!due) {
        return;
    }

    /* Do not spend a broadcast on a device nobody is attached to. */
    if (ws.clients > 0) {
        snprintf(json, sizeof(json),
            "{\"type\":\"health\",\"status\":\"%s\",\"ok\":%s,"
            "\"heap_free\":%lu,\"issues\":%u,\"issue\":\"%s\","
            "\"detail\":\"%s\",\"boot_count\":%u,"
            "\"reboot_pending\":%s,\"handling\":%s}",
            health_monitor_state_name(s->state),
            s->state == HEALTH_STATE_OK ? "true" : "false",
            (unsigned long)s->heap_free, (unsigned)s->issue_count,
            s->issue_count > 0
                ? health_monitor_issue_name(s->issues[0].code) : "none",
            s->issue_count > 0 ? s->issues[0].detail : "",
            (unsigned)s->boot_count,
            s->reboot_pending ? "true" : "false",
            s->handling ? "true" : "false");

        ws_server_broadcast_json(json);
    }

    s_last_alert_ms = now;

    if (transition) {
        syslog(LOG_INFO, "[%s] state %s -> %s (%u issues)\n", TAG,
            health_monitor_state_name(s_last_state),
            health_monitor_state_name(s->state), (unsigned)s->issue_count);
        s_last_state = s->state;
    }
}

/* ── Recovery ─────────────────────────────────────────────────── */

static const uint32_t s_backoff_ms[3] = {
    10000u, 30000u, 90000u
};

static uint8_t issue_severity(uint8_t code)
{
    return code < HEALTH_ISSUE_COUNT ? s_issue_state[code].severity
                                     : HEALTH_SEV_INFO;
}

static void health_recover(health_snapshot_t *s, uint32_t now)
{
    uint8_t mic_sev = issue_severity(HEALTH_ISSUE_MIC_STALL);
    uint8_t mic_err = issue_severity(HEALTH_ISSUE_MIC_ERR);
    uint8_t ws_sev = issue_severity(HEALTH_ISSUE_WS_ACCEPT_DEAD);
    uint8_t ws_tx = issue_severity(HEALTH_ISSUE_WS_TX_FAIL);

    /* Microphone: a restart is safe (stop joins the capture thread) and
     * keeps the user's stream identity, so it is always tried before the
     * problem is allowed to look like a reset reason.
     */
    if (s->mic_want_running) {
        if (s->mic_running && s->mic_last_frame_age_ms < 3000u) {
            s_mic_restart_attempts = 0;
        } else if ((mic_sev >= HEALTH_SEV_WARNING
                       || mic_err >= HEALTH_SEV_WARNING)
            && s_mic_restart_attempts < 3
            && (s_mic_last_restart_ms == 0
                || now - s_mic_last_restart_ms
                    >= s_backoff_ms[s_mic_restart_attempts])) {
            s_mic_last_restart_ms = now;
            s_mic_restart_attempts++;
            s_recovery_attempts++;
            s->handling = true;

            syslog(LOG_WARNING, "[%s] restarting mic (attempt %u)\n", TAG,
                (unsigned)s_mic_restart_attempts);
            if (mic_stream_restart() != OK) {
                syslog(LOG_ERR, "[%s] mic restart failed\n", TAG);
            }

            s->handling = false;
        }
    } else {
        s_mic_restart_attempts = 0;
        s_mic_last_restart_ms = 0;
    }

    /* WebSocket listener: rebind only.  The client table and existing
     * sessions are not touched, because a full ws_server_stop/start would
     * orphan the detached client threads and their fds.
     */
    if (s->services_started) {
        if (s->ws_listening
            && s->beat_age_ms[HEALTH_BEAT_WS_ACCEPT] < 2000u) {
            s_ws_restart_attempts = 0;
        } else if (ws_sev >= HEALTH_SEV_CRITICAL
            && s_ws_restart_attempts < 3
            && (s_ws_last_restart_ms == 0
                || now - s_ws_last_restart_ms
                    >= s_backoff_ms[s_ws_restart_attempts])) {
            s_ws_last_restart_ms = now;
            s_ws_restart_attempts++;
            s_recovery_attempts++;
            s->handling = true;

            syslog(LOG_WARNING, "[%s] restarting ws listener (attempt %u)\n",
                TAG, (unsigned)s_ws_restart_attempts);
            if (ws_server_listener_restart() != OK) {
                syslog(LOG_ERR, "[%s] ws listener restart failed\n", TAG);
            }

            s->handling = false;
        }
    }

    /* A peer that has stopped reading is the one case where dropping the
     * connection is the fix.  Only the worst offender is disconnected.
     */
    if (s->services_started && ws_tx >= HEALTH_SEV_WARNING
        && ws_server_drop_worst_client() == OK) {
        s_recovery_attempts++;
    }

    s->recovery_attempts = s_recovery_attempts;
    s->auto_reset_count = s_auto_reset_count;
}

#ifdef CONFIG_AI_AGENT_HEALTH_AUTORESET

static bool issue_is_reset_class(uint32_t now)
{
    if (issue_severity(HEALTH_ISSUE_HEAP_LOW) >= HEALTH_SEV_CRITICAL
        || issue_severity(HEALTH_ISSUE_HEAP_TREND) >= HEALTH_SEV_CRITICAL
        || issue_severity(HEALTH_ISSUE_TASK_STALL) >= HEALTH_SEV_CRITICAL
        || issue_severity(HEALTH_ISSUE_CRON_STALL) >= HEALTH_SEV_CRITICAL) {
        return true;
    }

    /* A service problem only becomes reset-class once its own recovery
     * budget is exhausted: a mic restart is cheap, a soft reset is not.
     * The extra 30 s after the last attempt lets the freshly started
     * service publish its first heartbeat before being judged again.
     */
    if (issue_severity(HEALTH_ISSUE_MIC_STALL) >= HEALTH_SEV_CRITICAL
        && s_mic_restart_attempts >= 3
        && now - s_mic_last_restart_ms >= 30000u) {
        return true;
    }
    if (issue_severity(HEALTH_ISSUE_WS_ACCEPT_DEAD) >= HEALTH_SEV_CRITICAL
        && s_ws_restart_attempts >= 3
        && now - s_ws_last_restart_ms >= 30000u) {
        return true;
    }

    return false;
}

static void health_try_reset(health_snapshot_t *s, uint32_t now)
{
    uint8_t code;
    uint8_t severity;

    if (s->state != HEALTH_STATE_CRITICAL || s->safe_mode
        || s->reboot_pending) {
        return;
    }
    if (s->uptime_s < AGENT_HEALTH_GRACE_S || !issue_is_reset_class(now)) {
        return;
    }

    if (s_auto_reset_count >= AGENT_HEALTH_AUTORESET_MAX) {
        if (!s_safe_mode) {
            s_safe_mode = true;
            s->safe_mode = true;
            syslog(LOG_ERR,
                "[%s] reset budget exhausted; entering safe mode\n", TAG);
        }
        return;
    }

    /* One reset is allowed as soon as the grace window ends; a second needs
     * the device to have survived five minutes, so a crash loop cannot turn
     * into a reset loop.
     */
    if (s_auto_reset_count > 0 && s->uptime_s < 300) {
        return;
    }

    /* Never reset into a void: the host is what re-provisions /data, and a
     * device with no link would just boot, lose config, and fault again.
     */
    if (!s->net_ever_up) {
        return;
    }
    if (!s->net_up && s->net_down_s > 120u) {
        return;
    }

    code = s->issue_count > 0 ? s->issues[0].code : HEALTH_ISSUE_NONE;
    severity = s->issue_count > 0 ? s->issues[0].severity
                                  : HEALTH_SEV_CRITICAL;

    s_auto_reset_count++;
    s->auto_reset_count = s_auto_reset_count;
    s->reboot_pending = true;
    s->handling = true;

    health_store_note_anomaly(code, severity, true);
    health_store_set_reason(HEALTH_RESET_HEALTH_AUTO);
    health_store_set_auto_reset_count(s_auto_reset_count);

    pthread_mutex_lock(&s_lock);
    s_snap.reboot_pending = true;
    s_snap.handling = true;
    s_snap.auto_reset_count = s_auto_reset_count;
    pthread_mutex_unlock(&s_lock);

    health_alert_maybe(s, now, true);

    syslog(LOG_ERR,
        "[%s] auto reset %u/%u: issue=%s uptime=%lu\n", TAG,
        (unsigned)s_auto_reset_count, (unsigned)AGENT_HEALTH_AUTORESET_MAX,
        health_monitor_issue_name(code), (unsigned long)s->uptime_s);

    /* Let the last frame reach the host before the reset cuts the link. */
    sleep(1);
    boardctl(BOARDIOC_RESET, 0);
}

#else /* !CONFIG_AI_AGENT_HEALTH_AUTORESET */

static void health_try_reset(health_snapshot_t *s, uint32_t now)
{
    (void)s;
    (void)now;
}

#endif /* CONFIG_AI_AGENT_HEALTH_AUTORESET */

static void health_sample(void)
{
    health_snapshot_t s;
    health_boot_info_t boot;
    ws_server_stats_t ws;
    mic_stream_stats_t mic;
    cron_service_health_t cron;
    message_bus_stats_t bus;
    llm_router_health_t llm;
    struct mallinfo mi;
    task_walk_t walk;
    uint32_t now = health_now_ms();
    const char *ip;

    memset(&s, 0, sizeof(s));
    memset(&walk, 0, sizeof(walk));
    walk.min_prio = 255;

    s.sample_ms = now;
    s.uptime_s = (now - s_boot_ms) / 1000u;
    s.grace = s.uptime_s < AGENT_HEALTH_GRACE_S;
    s.services_started = s_services_started;
    s.safe_mode = s_safe_mode;
    s.recovery_attempts = s_recovery_attempts;

    /* Heap */

    mi = mallinfo();
    s.heap_total = (uint32_t)mi.arena;
    s.heap_free = (uint32_t)mi.fordblks;
    s.heap_used = (uint32_t)mi.uordblks;
    s.heap_largest = (uint32_t)mi.mxordblk;

    if (s_heap_free_min == 0 || s.heap_free < s_heap_free_min) {
        s_heap_free_min = s.heap_free;
    }
    s.heap_free_min = s_heap_free_min;

    if (s.heap_ewma == 0) {
        s.heap_ewma = s.heap_free;
    } else {
        /* alpha = 0.3, integer arithmetic */
        s.heap_ewma = (s.heap_ewma * 7u + s.heap_free * 3u) / 10u;
    }

    /* Tasks */

    nxsched_foreach(task_walk_cb, &walk);
    s.task_count = walk.count;
    s.min_prio = walk.min_prio;

    if (s_task_baseline == 0 && s.uptime_s >= 60) {
        s_task_baseline = walk.count;
    }
    s.task_baseline = s_task_baseline;

    /* Network */

    s.net_up = network_is_connected();
    if (s.net_up) {
        ip = network_get_ip();
        if (ip != NULL) {
            strncpy(s.net_ip, ip, sizeof(s.net_ip) - 1);
        }

        s_net_ever_up = true;
        s_net_last_up_ms = now;
    }

    s.net_ever_up = s_net_ever_up;

    /* The rules and the reset gate read this snapshot field, not the value
     * that health_monitor_get() derives for the HTTP caller: keep the real
     * outage age here as well or NET_DOWN can never fire.
     */
    if (!s.net_up && s_net_ever_up) {
        uint32_t down = health_age_ms(now, s_net_last_up_ms);

        s.net_down_s = down == UINT32_MAX ? UINT32_MAX : down / 1000u;
    }

    /* Component counters */

    ws_server_get_stats(&ws);
    s.ws_listening = ws.running && ws.listen_fd >= 0;
    s.ws_clients = ws.clients;
    s.ws_accept_age_ms = health_age_ms(now, ws.last_accept_ms);
    s.ws_tx_ok = ws.tx_ok;
    s.ws_tx_fail = ws.tx_fail;
    s.ws_tx_timeout = ws.tx_timeout;
    s.ws_rx_frames = ws.rx_frames;
    s.ws_client_drops = ws.client_drops;

    mic_stream_get_stats(&mic);
    s.mic_want_running = mic.want_running;
    s.mic_running = mic.running;
    s.mic_frames = mic.frames;
    s.mic_bytes = mic.bytes;
    s.mic_read_errors = mic.read_errors;
    s.mic_send_errors = mic.send_errors;
    s.mic_overruns = mic.overruns;
    s.mic_restarts = mic.restarts;
    s.mic_last_frame_age_ms = health_age_ms(now, mic.last_frame_ms);

    cron_service_get_health(&cron);
    s.cron_running = cron.running;
    s.cron_jobs = cron.job_count;
    s.cron_beat_age_ms = health_age_ms(now, cron.beat_ms);
    s.cron_last_fire_age_s = cron.last_fire_ms == 0 ? 0
        : health_age_ms(now, cron.last_fire_ms) / 1000u;
    s.cron_fires = cron.fires;
    s.cron_errors = cron.errors;

    llm_router_get_health(&llm);
    s.llm_calls = llm.calls;
    s.llm_failures = llm.failures;
    s.llm_avg_latency_ms = llm.avg_latency_ms;
    s.llm_backends = llm.backends;
    s.llm_backends_backoff = llm.backends_backoff;

    message_bus_get_stats(&bus);
    s.bus_in_depth = bus.in_depth;
    s.bus_out_depth = bus.out_depth;
    s.bus_in_drops = bus.in_drops;
    s.bus_out_drops = bus.out_drops;

    /* Heartbeats */

    for (int i = 0; i < HEALTH_BEAT_COUNT; i++) {
        s.beat_age_ms[i] = health_age_ms(now,
            __atomic_load_n(&s_beat_ms[i], __ATOMIC_RELAXED));
    }

    /* cron and mic publish their ages through their own stats rather than
     * health_monitor_beat(), so fill those two slots from the values just
     * sampled instead of leaving the diagnostic block full of 0xFFFFFFFF.
     */
    s.beat_age_ms[HEALTH_BEAT_CRON] = s.cron_beat_age_ms;
    s.beat_age_ms[HEALTH_BEAT_MIC] = s.mic_last_frame_age_ms;
    {
        uint32_t busy = __atomic_load_n(
            &s_busy_since_ms[HEALTH_BEAT_AGENT_LLM], __ATOMIC_RELAXED);

        s.llm_busy_age_s = busy != 0 ? health_age_ms(now, busy) / 1000u : 0;
    }
    {
        uint32_t busy = __atomic_load_n(
            &s_busy_since_ms[HEALTH_BEAT_AGENT_TOOL], __ATOMIC_RELAXED);

        s.tool_busy_age_s = busy != 0 ? health_age_ms(now, busy) / 1000u : 0;
    }

    /* Boot record */

    health_store_get_boot_info(&boot);
    s.boot_count = boot.boot_count;
    s.auto_reset_count = s_auto_reset_count;
    s.reset_reason = boot.reset_reason;
    s.persist_ok = boot.persist_ok;
    s.needs_config = s_needs_config;
    s.last_issue_code = boot.anomaly_code;
    s.last_severity = boot.anomaly_severity;
    s.last_anomaly_uptime_s = boot.anomaly_uptime_s;

    /* A stream that has just been asked to start gets a few seconds to
     * open the codec before "no frames yet" is treated as a stall.
     */

    if (s.mic_want_running && !s_prev_mic_want) {
        s_mic_want_since_ms = now;
    } else if (!s.mic_want_running) {
        s_mic_want_since_ms = 0;
    }
    s_prev_mic_want = s.mic_want_running;

    /* Trend window: the heap slope needs the sample from one window ago,
     * so the current one is pushed first.
     */

    hist_push(&s);
    {
        const health_hist_t *old = hist_n_back(60);

        /* The rule is a five-minute slope; do not report one from the
         * first few samples, where allocator warm-up makes the line steep.
         */
        if (old != NULL && now > old->ms + 240000u) {
            int64_t dfree = (int64_t)s.heap_free - (int64_t)old->heap_free;
            int64_t dt = (int64_t)now - (int64_t)old->ms;

            s.heap_slope_kb_per_min = (int32_t)((dfree * 60000)
                / (1024 * dt));
        }
    }

    health_eval_rules(&s);
    health_recover(&s, now);
    health_publish_issues(&s);

    pthread_mutex_lock(&s_lock);
    s.seq = s_snap.seq + 1u;
    s_snap = s;
    pthread_mutex_unlock(&s_lock);

    /* The reset gate reads the published issue list and may not return. */
    health_try_reset(&s, now);
    health_alert_maybe(&s, now, false);
}

static void *health_task(void *arg)
{
    (void)arg;

    pthread_setname_np(pthread_self(), "health");

    while (s_running) {
        health_sample();

        for (int i = 0; i < AGENT_HEALTH_INTERVAL_MS / 100; i++) {
            if (!s_running) {
                break;
            }
            usleep(100 * 1000);
        }
    }

    return NULL;
}

/* ── Lifecycle ────────────────────────────────────────────────── */

int health_monitor_init(void)
{
    health_boot_info_t boot;

    memset(&s_snap, 0, sizeof(s_snap));
    memset((void *)s_beat_ms, 0, sizeof(s_beat_ms));
    memset((void *)s_busy_since_ms, 0, sizeof(s_busy_since_ms));
    memset((void *)s_restart_request, 0, sizeof(s_restart_request));
    memset(s_hist, 0, sizeof(s_hist));
    memset(s_issue_state, 0, sizeof(s_issue_state));
    s_hist_head = 0;
    s_hist_count = 0;
    s_last_state = HEALTH_STATE_STARTING;
    s_last_alert_ms = 0;
    s_prev_mic_want = false;
    s_mic_want_since_ms = 0;
    s_mic_restart_attempts = 0;
    s_mic_last_restart_ms = 0;
    s_ws_restart_attempts = 0;
    s_ws_last_restart_ms = 0;

    s_boot_ms = health_now_ms();
    s_heap_free_min = 0;
    s_task_baseline = 0;
    s_services_started = false;
    s_net_ever_up = false;
    s_net_last_up_ms = 0;
    s_recovery_attempts = 0;

    health_store_boot();
    health_store_get_boot_info(&boot);

    s_safe_mode = boot.auto_reset_count >= AGENT_HEALTH_AUTORESET_MAX;
    s_auto_reset_count = boot.auto_reset_count;

    /* /data is a tmpfs: any boot after the first starts without the LLM
     * config, cron jobs and skills the host pushed.  The host clears this
     * after provisioning.
     */
    s_needs_config = boot.boot_count > 1;

    s_snap.state = HEALTH_STATE_STARTING;
    s_snap.boot_count = boot.boot_count;
    s_snap.auto_reset_count = boot.auto_reset_count;
    s_snap.reset_reason = boot.reset_reason;
    s_snap.persist_ok = boot.persist_ok;
    s_snap.safe_mode = s_safe_mode;
    s_snap.needs_config = s_needs_config;
    s_snap.auto_reset_count = s_auto_reset_count;
    s_snap.seq = 1;

    syslog(LOG_INFO,
        "[%s] init: boot=%u reason=%u safe=%d persist=%d\n", TAG,
        boot.boot_count, boot.reset_reason, s_safe_mode, boot.persist_ok);

    return OK;
}

int health_monitor_start(void)
{
    pthread_attr_t attr;
    int rc;

    if (s_started) {
        return OK;
    }

    s_running = true;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, AGENT_HEALTH_STACK);

    rc = pthread_create(&s_thread, &attr, health_task, NULL);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        s_running = false;
        syslog(LOG_ERR, "[%s] sampler create failed (%d)\n", TAG, rc);
        return ERROR;
    }

    s_started = true;
    syslog(LOG_INFO, "[%s] sampler started (%d ms)\n", TAG,
        AGENT_HEALTH_INTERVAL_MS);
    return OK;
}

void health_monitor_stop(void)
{
    if (!s_started) {
        return;
    }

    s_running = false;
    pthread_join(s_thread, NULL);
    s_started = false;
}

bool health_monitor_is_running(void)
{
    return s_running;
}

/* ── Snapshot ─────────────────────────────────────────────────── */

void health_monitor_get(health_snapshot_t *out)
{
    uint32_t now;

    if (out == NULL) {
        return;
    }

    pthread_mutex_lock(&s_lock);
    *out = s_snap;
    pthread_mutex_unlock(&s_lock);

    now = health_now_ms();
    out->stale = s_started && out->sample_ms != 0
        && health_age_ms(now, out->sample_ms) > AGENT_HEALTH_INTERVAL_MS * 3;

    /* Network ages are derived here so the sampler does not have to keep
     * two more pieces of mutable state in sync.
     */

    if (out->net_up) {
        out->net_down_s = 0;
        out->net_last_up_age_s = 0;
    } else if (out->net_ever_up) {
        uint32_t age = health_age_ms(now, s_net_last_up_ms);

        out->net_last_up_age_s = age == UINT32_MAX ? UINT32_MAX : age / 1000u;
        out->net_down_s = out->net_last_up_age_s;
    }
}

/* ── Producer interface ───────────────────────────────────────── */

void health_monitor_beat(health_beat_id_t id)
{
    if ((unsigned)id >= HEALTH_BEAT_COUNT) {
        return;
    }

    __atomic_store_n(&s_beat_ms[id], health_now_ms(), __ATOMIC_RELAXED);
}

void health_monitor_beat_busy(health_beat_id_t id, bool busy)
{
    uint32_t now;

    if ((unsigned)id >= HEALTH_BEAT_COUNT) {
        return;
    }

    now = health_now_ms();
    __atomic_store_n(&s_beat_ms[id], now, __ATOMIC_RELAXED);

    if (busy) {
        uint32_t expected = 0;

        /* Keep the first timestamp: a re-entrant or repeated enter must not
         * make a stuck call look young again.
         */
        __atomic_compare_exchange_n(&s_busy_since_ms[id], &expected, now,
            false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&s_busy_since_ms[id], 0, __ATOMIC_RELAXED);
    }
}

void health_monitor_notify_services_started(void)
{
    s_services_started = true;
}

void health_monitor_notify_config_ok(void)
{
    s_needs_config = false;

    pthread_mutex_lock(&s_lock);
    s_snap.needs_config = false;
    pthread_mutex_unlock(&s_lock);
}

void health_monitor_notify_host_ack(void)
{
    s_safe_mode = false;
    s_needs_config = false;
    s_recovery_attempts = 0;
    s_auto_reset_count = 0;
    s_mic_restart_attempts = 0;
    s_mic_last_restart_ms = 0;
    s_ws_restart_attempts = 0;
    s_ws_last_restart_ms = 0;

    pthread_mutex_lock(&s_lock);
    s_snap.safe_mode = false;
    s_snap.auto_reset_count = 0;
    s_snap.needs_config = false;
    pthread_mutex_unlock(&s_lock);

    health_store_clear_auto_reset();
}

void health_monitor_request_restart(health_beat_id_t id)
{
    if ((unsigned)id >= HEALTH_BEAT_COUNT) {
        return;
    }

    __atomic_store_n(&s_restart_request[id], true, __ATOMIC_RELAXED);
}

/* ── Fault injection (HIL only) ───────────────────────────────── */

#ifdef CONFIG_AI_AGENT_HEALTH_TEST

#define HEALTH_TEST_HOG_BLOCK (64u * 1024u)
#define HEALTH_TEST_HOG_MAX 128

static volatile health_fault_t s_fault;
static volatile uint32_t s_fault_until_ms;
static volatile uint32_t s_fault_count;
static void *s_hog_blocks[HEALTH_TEST_HOG_MAX];
static int s_hog_count;

static void health_fault_clear_hog(void)
{
    for (int i = 0; i < s_hog_count; i++) {
        free(s_hog_blocks[i]);
        s_hog_blocks[i] = NULL;
    }
    s_hog_count = 0;
}

int health_monitor_fault_set(const char *name, uint32_t seconds,
    uint32_t count, uint32_t kb)
{
    uint32_t now = health_now_ms();

    if (name == NULL) {
        return ERROR;
    }

    health_fault_clear_hog();
    s_fault = HEALTH_FAULT_NONE;
    s_fault_until_ms = 0;
    s_fault_count = 0;

    if (strcmp(name, "fault_off") == 0) {
        syslog(LOG_WARNING, "[%s] fault injection cleared\n", TAG);
        return OK;
    }

    if (strcmp(name, "agent_stall") == 0) {
        s_fault = HEALTH_FAULT_AGENT_STALL;
        s_fault_until_ms = seconds > 0 ? now + seconds * 1000u : 0;
    } else if (strcmp(name, "mic_read_fail") == 0) {
        s_fault = HEALTH_FAULT_MIC_READ_FAIL;
        s_fault_count = count > 0 ? count : 3;
    } else if (strcmp(name, "ws_send_fail") == 0) {
        s_fault = HEALTH_FAULT_WS_SEND_FAIL;
        s_fault_count = count > 0 ? count : 20;
    } else if (strcmp(name, "net_down") == 0) {
        s_fault = HEALTH_FAULT_NET_DOWN;
        s_fault_until_ms = seconds > 0 ? now + seconds * 1000u
                                       : now + 60000u;
    } else if (strcmp(name, "heap_hog") == 0) {
        uint32_t blocks;

        s_fault = HEALTH_FAULT_HEAP_HOG;
        blocks = kb / 64u;
        if (blocks > HEALTH_TEST_HOG_MAX) {
            blocks = HEALTH_TEST_HOG_MAX;
        }

        for (uint32_t i = 0; i < blocks; i++) {
            void *p = malloc(HEALTH_TEST_HOG_BLOCK);

            if (p == NULL) {
                break;
            }
            /* Touch every page: an untouched allocation would let the
             * allocator report success without actually taking the memory.
             */
            memset(p, 0xA5, HEALTH_TEST_HOG_BLOCK);
            s_hog_blocks[s_hog_count++] = p;
        }

        syslog(LOG_WARNING, "[%s] heap_hog: %d x 64 KB held\n", TAG,
            s_hog_count);
        return OK;
    } else {
        return ERROR;
    }

    syslog(LOG_WARNING, "[%s] fault=%s seconds=%lu count=%lu kb=%lu\n",
        TAG, name, (unsigned long)seconds, (unsigned long)count,
        (unsigned long)kb);
    return OK;
}

bool health_monitor_fault_agent_stall(void)
{
    if (s_fault != HEALTH_FAULT_AGENT_STALL) {
        return false;
    }

    if (s_fault_until_ms != 0
        && health_now_ms() >= s_fault_until_ms) {
        s_fault = HEALTH_FAULT_NONE;
        s_fault_until_ms = 0;
        return false;
    }

    return true;
}

bool health_monitor_fault_take_mic_fail(void)
{
    if (s_fault != HEALTH_FAULT_MIC_READ_FAIL || s_fault_count == 0) {
        return false;
    }

    if (--s_fault_count == 0) {
        s_fault = HEALTH_FAULT_NONE;
    }
    return true;
}

bool health_monitor_fault_take_ws_fail(void)
{
    if (s_fault != HEALTH_FAULT_WS_SEND_FAIL || s_fault_count == 0) {
        return false;
    }

    if (--s_fault_count == 0) {
        s_fault = HEALTH_FAULT_NONE;
    }
    return true;
}

bool health_monitor_fault_ws_active(void)
{
    return s_fault == HEALTH_FAULT_WS_SEND_FAIL;
}

bool health_monitor_fault_net_down(void)
{
    if (s_fault != HEALTH_FAULT_NET_DOWN) {
        return false;
    }

    if (health_now_ms() >= s_fault_until_ms) {
        s_fault = HEALTH_FAULT_NONE;
        s_fault_until_ms = 0;
        return false;
    }

    return true;
}

#endif /* CONFIG_AI_AGENT_HEALTH_TEST */
