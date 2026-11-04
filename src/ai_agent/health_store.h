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
 * health_store.h — the tiny slice of health state that must survive a reset.
 *
 * Only a boot counter, the auto-reset budget, the reset reason and the last
 * anomaly are kept, packed into two RTC backup registers.  Everything else
 * is deliberately ephemeral: the host's watchdog log is the authoritative
 * history, and /data is a tmpfs that a soft reset would take with it.
 */

#include <stdbool.h>
#include <stdint.h>

/* Reset reasons.  Values are packed into four bits; append only. */
typedef enum {
    HEALTH_RESET_UNKNOWN = 0,
    HEALTH_RESET_POWER_ON,
    HEALTH_RESET_PIN,
    HEALTH_RESET_USER,
    HEALTH_RESET_HEALTH_AUTO,
    HEALTH_RESET_WATCHDOG,
    HEALTH_RESET_LOWPOWER,
    HEALTH_RESET_HOST,
} health_reset_reason_t;

typedef struct {
    uint8_t boot_count;          /* saturates at 255 */
    uint8_t auto_reset_count;    /* 0..15 */
    uint8_t reset_reason;        /* health_reset_reason_t */
    bool record_valid;           /* RTC record was intact at boot */
    bool persist_ok;             /* last write verified by read-back */

    uint8_t anomaly_code;        /* health_issue_code_t or 0 */
    uint8_t anomaly_severity;
    bool anomaly_reset_marker;
    uint32_t anomaly_uptime_s;   /* low 16 bits are stored */
} health_boot_info_t;

/** Read the RTC record and increment the boot counter.  Call once. */
void health_store_boot(void);

/** Copy the cached boot record. */
void health_store_get_boot_info(health_boot_info_t *out);

/** Record the most recent anomaly (best effort). */
void health_store_note_anomaly(uint8_t code, uint8_t severity,
    bool reset_marker);

void health_store_set_auto_reset_count(uint8_t count);
void health_store_clear_auto_reset(void);
void health_store_set_reason(uint8_t reason);

/** Force a write and verify it by reading back. */
bool health_store_commit(void);
