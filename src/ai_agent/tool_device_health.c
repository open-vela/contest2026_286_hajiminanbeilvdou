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

#include "tools/tool_device_health.h"

#include "agent_compat.h"
#include "core/health_monitor.h"

#include <stdio.h>
#include <string.h>

int tool_get_health_execute(const char *input_json, char *output,
    size_t output_size)
{
    health_snapshot_t h;
    int n;

    (void)input_json;

    if (output == NULL || output_size == 0) {
        return ERROR;
    }

    health_monitor_get(&h);

    /* Deliberately small: this string is read by a model that has to
     * answer in one or two lines, and every extra field is a token the
     * watch screen never shows.
     */
    n = snprintf(output, output_size,
        "{\"status\":\"%s\",\"stale\":%s,\"uptime_s\":%lu,"
        "\"heap_free_kb\":%lu,\"heap_largest_kb\":%lu,"
        "\"tasks\":%u,\"net\":\"%s\",\"ws_clients\":%lu,"
        "\"mic\":\"%s\",\"cron_jobs\":%u,\"issues\":%u,"
        "\"boot_count\":%u,\"reset_reason\":\"%s\",\"persist_ok\":%s}",
        health_monitor_state_name(h.state),
        h.stale ? "true" : "false",
        (unsigned long)h.uptime_s,
        (unsigned long)(h.heap_free / 1024u),
        (unsigned long)(h.heap_largest / 1024u),
        (unsigned)h.task_count,
        h.net_up ? "up" : (h.net_ever_up ? "down" : "never"),
        (unsigned long)h.ws_clients,
        h.mic_want_running ? (h.mic_running ? "running" : "stalled")
                           : "idle",
        (unsigned)h.cron_jobs, (unsigned)h.issue_count,
        (unsigned)h.boot_count,
        health_monitor_reset_reason_name(h.reset_reason),
        h.persist_ok ? "true" : "false");

    if (n < 0 || (size_t)n >= output_size) {
        return ERROR;
    }

    return OK;
}
