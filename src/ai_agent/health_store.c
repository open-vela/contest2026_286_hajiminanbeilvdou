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

#include "infra/health_store.h"
#include "agent_compat.h"
#include "agent_config.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#ifdef CONFIG_AI_AGENT_HEALTH_PERSIST
#include <sf32lb_bbram.h>
#endif

#ifdef CONFIG_BOARDCTL_RESET_CAUSE
#include <nuttx/board.h>
#include <sys/boardctl.h>
#endif

static const char *TAG = "health_store";

/* RTC backup register indices.  Five and six are named by the SDK for OTA
 * force mode and WDT status, neither of which this firmware uses: there is
 * no OTA path on the device and the hardware watchdog is never armed.
 */
#ifndef CONFIG_AI_AGENT_HEALTH_BBRAM_PRIMARY_IDX
#define CONFIG_AI_AGENT_HEALTH_BBRAM_PRIMARY_IDX 5
#endif
#ifndef CONFIG_AI_AGENT_HEALTH_BBRAM_ANOMALY_IDX
#define CONFIG_AI_AGENT_HEALTH_BBRAM_ANOMALY_IDX 6
#endif

#define STORE_PRIMARY_IDX CONFIG_AI_AGENT_HEALTH_BBRAM_PRIMARY_IDX
#define STORE_ANOMALY_IDX CONFIG_AI_AGENT_HEALTH_BBRAM_ANOMALY_IDX

/* Primary word layout:
 *   [7:0]   0xA5 magic
 *   [15:8]  0x5A inverse magic
 *   [23:16] boot count
 *   [27:24] auto-reset count
 *   [31:28] reset reason
 */
#define PRIMARY_MAGIC 0xA5u
#define PRIMARY_MAGIC_INV 0x5Au

#define PRIMARY_PACK(boot, resets, reason) \
    (PRIMARY_MAGIC | (PRIMARY_MAGIC_INV << 8) | \
        ((uint32_t)(boot) << 16) | ((uint32_t)(resets) << 24) | \
        ((uint32_t)(reason) << 28))

/* Anomaly word layout:
 *   [7:0]   issue code
 *   [9:8]   severity
 *   [10]    reset marker
 *   [11]    valid
 *   [27:12] uptime seconds, low 16 bits
 *   [31:28] 0xA magic nibble
 */
#define ANOMALY_MAGIC 0xAu

#define ANOMALY_PACK(code, sev, marker, valid, uptime) \
    ((uint32_t)(code) | ((uint32_t)(sev) << 8) | \
        ((uint32_t)(marker) << 10) | ((uint32_t)(valid) << 11) | \
        (((uint32_t)(uptime) & 0xFFFFu) << 12) | \
        ((uint32_t)ANOMALY_MAGIC << 28))

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static health_boot_info_t s_info;
static bool s_booted;

static uint32_t store_uptime_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

static uint8_t store_hw_reset_reason(void)
{
#ifdef CONFIG_BOARDCTL_RESET_CAUSE
    struct boardioc_reset_cause_s cause;

    if (board_reset_cause(&cause) == OK) {
        switch (cause.cause) {
        case BOARDIOC_RESETCAUSE_SYS_CHIPPOR:
            return HEALTH_RESET_POWER_ON;
        case BOARDIOC_RESETCAUSE_PIN:
            return HEALTH_RESET_PIN;
        case BOARDIOC_RESETCAUSE_LOWPOWER:
            return HEALTH_RESET_LOWPOWER;
        case BOARDIOC_RESETCAUSE_CORE_SOFT:
        case BOARDIOC_RESETCAUSE_CPU_SOFT:
            return HEALTH_RESET_USER;
        case BOARDIOC_RESETCAUSE_SYS_RWDT:
        case BOARDIOC_RESETCAUSE_CORE_MWDT:
        case BOARDIOC_RESETCAUSE_CORE_RWDT:
        case BOARDIOC_RESETCAUSE_CPU_MWDT:
        case BOARDIOC_RESETCAUSE_CPU_RWDT:
            return HEALTH_RESET_WATCHDOG;
        default:
            return HEALTH_RESET_UNKNOWN;
        }
    }
#endif

    return HEALTH_RESET_UNKNOWN;
}

#ifdef CONFIG_AI_AGENT_HEALTH_PERSIST

static bool store_write(uint8_t idx, uint32_t value)
{
    uint32_t readback = 0;

    if (sf32lb_bbram_write(idx, value) != 0) {
        return false;
    }
    if (sf32lb_bbram_read(idx, &readback) != 0) {
        return false;
    }
    return readback == value;
}

static uint32_t store_primary_word(void)
{
    return PRIMARY_PACK(s_info.boot_count, s_info.auto_reset_count,
        s_info.reset_reason);
}

static uint32_t store_anomaly_word(void)
{
    bool valid = s_info.anomaly_code != 0;

    return ANOMALY_PACK(s_info.anomaly_code, s_info.anomaly_severity,
        s_info.anomaly_reset_marker, valid,
        s_info.anomaly_uptime_s);
}

bool health_store_commit(void)
{
    bool ok;

    pthread_mutex_lock(&s_lock);
    ok = store_write(STORE_PRIMARY_IDX, store_primary_word());
    ok = store_write(STORE_ANOMALY_IDX, store_anomaly_word()) && ok;
    s_info.persist_ok = ok;
    pthread_mutex_unlock(&s_lock);

    if (!ok) {
        syslog(LOG_WARNING, "[%s] RTC backup write failed\n", TAG);
    }
    return ok;
}

void health_store_boot(void)
{
    uint32_t primary = 0;
    uint32_t anomaly = 0;
    bool primary_ok;
    bool anomaly_ok;

    pthread_mutex_lock(&s_lock);
    if (s_booted) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    s_booted = true;

    memset(&s_info, 0, sizeof(s_info));
    s_info.persist_ok = true;

    primary_ok = sf32lb_bbram_read(STORE_PRIMARY_IDX, &primary) == 0;
    anomaly_ok = sf32lb_bbram_read(STORE_ANOMALY_IDX, &anomaly) == 0;

    if (primary_ok && (primary & 0xFFu) == PRIMARY_MAGIC
        && ((primary >> 8) & 0xFFu) == PRIMARY_MAGIC_INV) {
        uint8_t prev_boot = (uint8_t)((primary >> 16) & 0xFFu);

        s_info.record_valid = true;
        s_info.boot_count = prev_boot == 0xFFu ? 0xFFu
                                              : (uint8_t)(prev_boot + 1u);
        s_info.auto_reset_count = (uint8_t)((primary >> 24) & 0x0Fu);
        s_info.reset_reason = (uint8_t)((primary >> 28) & 0x0Fu);
        if (s_info.reset_reason > HEALTH_RESET_HOST) {
            s_info.reset_reason = HEALTH_RESET_UNKNOWN;
        }
    } else {
        s_info.boot_count = 1;
        s_info.auto_reset_count = 0;
        s_info.reset_reason = HEALTH_RESET_POWER_ON;
    }

    if (anomaly_ok && (anomaly >> 28) == ANOMALY_MAGIC
        && (anomaly & (1u << 11)) != 0) {
        s_info.anomaly_code = (uint8_t)(anomaly & 0xFFu);
        s_info.anomaly_severity = (uint8_t)((anomaly >> 8) & 0x03u);
        s_info.anomaly_uptime_s = (anomaly >> 12) & 0xFFFFu;
        if ((anomaly & (1u << 10)) != 0) {
            s_info.anomaly_reset_marker = true;
            s_info.reset_reason = HEALTH_RESET_HEALTH_AUTO;
        }
    }

    /* The marker has served its purpose once consumed; clearing it keeps a
     * later unrelated reset from being reported as a health reset.
     */
    s_info.anomaly_reset_marker = false;

    /* The hardware knows how this boot started.  It wins over a stale RTC
     * reason (which survives a power cycle along with the counter), but a
     * marker written immediately before a health reset wins over both.
     */
    if (s_info.reset_reason != HEALTH_RESET_HEALTH_AUTO) {
        uint8_t hw_reason = store_hw_reset_reason();

        if (hw_reason != HEALTH_RESET_UNKNOWN) {
            s_info.reset_reason = hw_reason;
        } else if (!s_info.record_valid) {
            s_info.reset_reason = HEALTH_RESET_POWER_ON;
        }
    }

    s_info.persist_ok = store_write(STORE_PRIMARY_IDX, store_primary_word())
        && store_write(STORE_ANOMALY_IDX, store_anomaly_word());

    pthread_mutex_unlock(&s_lock);

    syslog(LOG_INFO,
        "[%s] boot=%u resets=%u reason=%u valid=%d persist=%d\n", TAG,
        s_info.boot_count, s_info.auto_reset_count, s_info.reset_reason,
        s_info.record_valid, s_info.persist_ok);
}

#else /* !CONFIG_AI_AGENT_HEALTH_PERSIST */

bool health_store_commit(void)
{
    pthread_mutex_lock(&s_lock);
    s_info.persist_ok = false;
    pthread_mutex_unlock(&s_lock);
    return false;
}

void health_store_boot(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_booted) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    s_booted = true;
    memset(&s_info, 0, sizeof(s_info));
    s_info.boot_count = 1;
    s_info.reset_reason = HEALTH_RESET_POWER_ON;
    s_info.persist_ok = false;
    pthread_mutex_unlock(&s_lock);

    syslog(LOG_INFO, "[%s] persistence disabled\n", TAG);
}

#endif /* CONFIG_AI_AGENT_HEALTH_PERSIST */

void health_store_get_boot_info(health_boot_info_t *out)
{
    if (out == NULL) {
        return;
    }

    pthread_mutex_lock(&s_lock);
    *out = s_info;
    pthread_mutex_unlock(&s_lock);
}

void health_store_note_anomaly(uint8_t code, uint8_t severity,
    bool reset_marker)
{
    pthread_mutex_lock(&s_lock);
    s_info.anomaly_code = code;
    s_info.anomaly_severity = severity;
    s_info.anomaly_reset_marker = reset_marker;
    s_info.anomaly_uptime_s = store_uptime_s();
    pthread_mutex_unlock(&s_lock);

    health_store_commit();
}

void health_store_set_auto_reset_count(uint8_t count)
{
    pthread_mutex_lock(&s_lock);
    s_info.auto_reset_count = count & 0x0Fu;
    pthread_mutex_unlock(&s_lock);

    health_store_commit();
}

void health_store_clear_auto_reset(void)
{
    pthread_mutex_lock(&s_lock);
    s_info.auto_reset_count = 0;
    pthread_mutex_unlock(&s_lock);

    health_store_commit();
}

void health_store_set_reason(uint8_t reason)
{
    pthread_mutex_lock(&s_lock);
    s_info.reset_reason = reason & 0x0Fu;
    pthread_mutex_unlock(&s_lock);

    health_store_commit();
}
