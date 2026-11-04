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

#include "infra/api_handler.h"
#include "agent_config.h"
#include "core/health_monitor.h"
#include "infra/agent_logbuf.h"
#include "infra/config_store.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "tools/skill_loader.h"
#include "tools/tool_get_time.h"

#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "cJSON.h"

static const char* TAG = "api";

/* Config keys exposed via REST API */

static const char* s_config_keys[] = {
    AGENT_CFG_KEY_API_KEY,
    AGENT_CFG_KEY_MODEL,
    AGENT_CFG_KEY_LLM_HOST,
    AGENT_CFG_KEY_LLM_PATH,
    AGENT_CFG_KEY_TAVILY_KEY,
    AGENT_CFG_KEY_VOLC_APPKEY,
    AGENT_CFG_KEY_VOLC_TOKEN,
    AGENT_CFG_KEY_VOLC_API_KEY,
    AGENT_CFG_KEY_VOLC_CLUSTER,
    AGENT_CFG_KEY_VOLC_ASR_CLUSTER,
    AGENT_CFG_KEY_PROXY_HOST,
    AGENT_CFG_KEY_PROXY_PORT,
};

#define NUM_CONFIG_KEYS (sizeof(s_config_keys) / sizeof(s_config_keys[0]))

/* -- HTTP helpers ----------------------------------------------- */

static void send_response(int fd, int code, const char* body)
{
    const char* status = (code == 200) ? "OK" : "Bad Request";
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        code, status, (int)strlen(body));
    send(fd, hdr, hlen, 0);
    send(fd, body, strlen(body), 0);
}

static const char* find_body(const char* buf, int buf_len)
{
    const char* p = strstr(buf, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/**
 * Read full HTTP body. peek_buf may contain partial body after headers.
 * Reads remaining bytes from fd based on Content-Length.
 * Returns malloc'd buffer (caller must free), or NULL on failure.
 */
static char* read_full_body(int fd, const char* buf, int buf_len)
{
    const char* hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end)
        return NULL;
    hdr_end += 4;

    /* Find Content-Length */
    const char* cl = strcasestr(buf, "\r\nContent-Length: ");
    int content_len = cl ? atoi(cl + 18) : 0;
    if (content_len <= 0 || content_len > 32 * 1024) {
        /* No Content-Length or too large - use what's already in buffer */
        int already = buf_len - (int)(hdr_end - buf);
        if (already <= 0)
            return NULL;
        char* body = malloc(already + 1);
        if (!body)
            return NULL;
        memcpy(body, hdr_end, already);
        body[already] = '\0';
        return body;
    }

    char* body = malloc(content_len + 1);
    if (!body)
        return NULL;

    /* Copy what's already peeked */
    int already = buf_len - (int)(hdr_end - buf);
    if (already > content_len)
        already = content_len;
    if (already > 0)
        memcpy(body, hdr_end, already);

    /* Read remaining from socket.  A peer that announces a body and then
     * stalls -- or a lost semaphore wakeup under the known NuttX race --
     * must not pin this client thread forever, or the POST never answers
     * and every later request looks like a dead device.
     */
    struct timeval tv = { 5, 0 };
    struct timeval zero = { 0, 0 };

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int remaining = content_len - already;
    int offset = already;
    while (remaining > 0) {
        int n = recv(fd, body + offset, remaining, 0);
        if (n <= 0) {
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
            free(body);
            return NULL;
        }
        offset += n;
        remaining -= n;
    }

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
    body[content_len] = '\0';
    return body;
}

/* -- Config handlers -------------------------------------------- */

/* Keys that live inside llm_backend_0 JSON (not flat config) */

static const char* s_backend_key_map[][2] = {
    { "api_key", "api_key" },
    { "model", "model" },
    { "llm_host", "host" },
    { "llm_port", "port" },
    { "llm_path", "path" },
};

#define NUM_BACKEND_KEYS (sizeof(s_backend_key_map) / sizeof(s_backend_key_map[0]))

static bool is_backend_key(const char* key, const char** backend_field)
{
    for (int i = 0; i < (int)NUM_BACKEND_KEYS; i++) {
        if (strcmp(key, s_backend_key_map[i][0]) == 0) {
            *backend_field = s_backend_key_map[i][1];
            return true;
        }
    }
    return false;
}

/* Keys whose values are secrets and must be masked in GET responses */

static const char* s_secret_keys[] = {
    AGENT_CFG_KEY_API_KEY,
    AGENT_CFG_KEY_TAVILY_KEY,
    AGENT_CFG_KEY_VOLC_APPKEY,
    AGENT_CFG_KEY_VOLC_TOKEN,
    AGENT_CFG_KEY_VOLC_API_KEY,
};

#define NUM_SECRET_KEYS (sizeof(s_secret_keys) / sizeof(s_secret_keys[0]))

static bool is_secret_key(const char* key)
{
    for (int i = 0; i < (int)NUM_SECRET_KEYS; i++) {
        if (strcmp(key, s_secret_keys[i]) == 0)
            return true;
    }
    return false;
}

/**
 * Mask a secret value for display. Keeps the first and last 2 characters
 * and replaces the middle with "****" so the companion App can tell
 * whether a key is configured without exposing the actual secret over
 * the wire. Values of 4 characters or fewer are fully masked. An empty
 * value stays empty.
 */
static void mask_value(const char* in, char* out, size_t out_size)
{
    size_t len = in ? strlen(in) : 0;
    if (len == 0) {
        if (out_size > 0)
            out[0] = '\0';
        return;
    }
    if (len <= 4) {
        snprintf(out, out_size, "****");
        return;
    }
    snprintf(out, out_size, "%c%c****%c%c",
        in[0], in[1], in[len - 2], in[len - 1]);
}

static bool handle_config_get(int fd)
{
    cJSON* obj = cJSON_CreateObject();
    char val[512];
    char masked[32];

    /* Read llm_backend_0 and extract LLM fields */
    char backend_json[1024] = { 0 };
    cJSON* backend_obj = NULL;
    if (claw_config_get("llm_backend_0", backend_json, sizeof(backend_json)) == OK) {
        backend_obj = cJSON_Parse(backend_json);
    }

    for (int i = 0; i < (int)NUM_CONFIG_KEYS; i++) {
        const char* backend_field = NULL;
        const char* raw = "";
        if (is_backend_key(s_config_keys[i], &backend_field) && backend_obj) {
            cJSON* item = cJSON_GetObjectItem(backend_obj, backend_field);
            raw = (item && cJSON_IsString(item)) ? item->valuestring : "";
        } else {
            val[0] = '\0';
            if (claw_config_get(s_config_keys[i], val, sizeof(val)) == OK)
                raw = val;
            else
                raw = "";
        }

        if (is_secret_key(s_config_keys[i])) {
            mask_value(raw, masked, sizeof(masked));
            cJSON_AddStringToObject(obj, s_config_keys[i], masked);
        } else {
            cJSON_AddStringToObject(obj, s_config_keys[i], raw);
        }
    }

    if (backend_obj)
        cJSON_Delete(backend_obj);

    char* json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) {
        send_response(fd, 500, "{\"error\":\"out of memory\"}");
        return true;
    }

    send_response(fd, 200, json);
    free(json);
    return true;
}

#define LLM_MODE_HOST "192.168.223.1"
#define LLM_MODE_PATH "/v1/chat/completions"
#define LLM_MODE_LOCAL_PORT "8081"
#define LLM_MODE_CLOUD_PORT "8080"
#define LLM_MODE_LOCAL_MODEL "mock-model"
#define LLM_MODE_CLOUD_MODEL "proxy-model"
#define LLM_MODE_DEVICE_KEY "dummy-key"

bool api_llm_mode_is_local(void)
{
    char port[8] = { 0 };

    return claw_config_get("llm_port", port, sizeof(port)) == OK
        && strcmp(port, LLM_MODE_LOCAL_PORT) == 0;
}

int api_llm_set_mode(bool local)
{
    const char* port = local ? LLM_MODE_LOCAL_PORT : LLM_MODE_CLOUD_PORT;
    const char* model = local ? LLM_MODE_LOCAL_MODEL : LLM_MODE_CLOUD_MODEL;

    /* Keep the host/path already configured (a custom proxy is allowed);
     * only fall back to the demo defaults when the board has no config yet.
     */
    char host[128] = { 0 };
    char path[128] = { 0 };

    if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, host, sizeof(host)) != OK
        || host[0] == '\0') {
        strncpy(host, LLM_MODE_HOST, sizeof(host) - 1);
    }
    if (claw_config_get(AGENT_CFG_KEY_LLM_PATH, path, sizeof(path)) != OK
        || path[0] == '\0') {
        strncpy(path, LLM_MODE_PATH, sizeof(path) - 1);
    }

    claw_config_set(AGENT_CFG_KEY_LLM_HOST, host);
    claw_config_set(AGENT_CFG_KEY_LLM_PATH, path);
    claw_config_set("llm_port", port);
    claw_config_set(AGENT_CFG_KEY_API_KEY, LLM_MODE_DEVICE_KEY);
    claw_config_set(AGENT_CFG_KEY_MODEL, model);

    /* The router only consults llm_backend_0, so keep it in sync with the
     * flat keys.  Without this a mode change would update the proxy but the
     * next request would apply the old backend again.
     */
    {
        cJSON* obj = cJSON_CreateObject();
        char* json;

        if (obj == NULL) {
            return ERROR;
        }
        cJSON_AddStringToObject(obj, "host", host);
        cJSON_AddStringToObject(obj, "path", path);
        cJSON_AddStringToObject(obj, "port", port);
        cJSON_AddStringToObject(obj, "api_key", LLM_MODE_DEVICE_KEY);
        cJSON_AddStringToObject(obj, "model", model);
        json = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        if (json == NULL) {
            return ERROR;
        }
        claw_config_set("llm_backend_0", json);
        free(json);
    }

    llm_router_init();
    llm_set_all(host, path, port, LLM_MODE_DEVICE_KEY, model);
    syslog(LOG_INFO, "[%s] LLM mode -> %s (%s:%s, model %s)\n", TAG,
        local ? "local mock" : "cloud proxy", host, port, model);
    return OK;
}

static bool handle_config_put(int fd, const char* body)
{
    syslog(LOG_INFO, "[%s] PUT /api/config body=%s\n", TAG,
        body ? body : "(null)");

    if (!body || *body == '\0') {
        syslog(LOG_WARNING, "[%s] PUT config: empty body\n", TAG);
        send_response(fd, 400, "{\"error\":\"empty body\"}");
        return true;
    }

    cJSON* req = cJSON_Parse(body);
    if (!req) {
        send_response(fd, 400, "{\"error\":\"invalid json\"}");
        return true;
    }

    /* Collect backend field updates */
    bool backend_dirty = false;
    char backend_json[1024] = { 0 };
    claw_config_get("llm_backend_0", backend_json, sizeof(backend_json));
    cJSON* backend_obj = cJSON_Parse(backend_json);
    if (!backend_obj) {
        backend_obj = cJSON_CreateObject();
    }

    cJSON* item = NULL;
    cJSON_ArrayForEach(item, req)
    {
        if (!cJSON_IsString(item) || !item->string) {
            continue;
        }
        /* Reject path traversal */
        if (strchr(item->string, '/') || strstr(item->string, "..")) {
            continue;
        }

        /* Skip secret fields whose value is still the GET mask, so a
         * client that read masked values and PUTs the whole object back
         * does not overwrite the stored secret with "****".
         */
        if (is_secret_key(item->string) && item->valuestring
            && strstr(item->valuestring, "****")) {
            continue;
        }

        const char* backend_field = NULL;
        if (is_backend_key(item->string, &backend_field)) {
            /* Update inside llm_backend_0 */
            cJSON_DeleteItemFromObject(backend_obj, backend_field);
            cJSON_AddStringToObject(backend_obj, backend_field, item->valuestring);
            backend_dirty = true;
            /* Also write flat key so config_show can display it */
            claw_config_set(item->string, item->valuestring);
        } else {
            /* Flat config key */
            claw_config_set(item->string, item->valuestring);
        }
    }

    /* Persist backend changes */
    if (backend_dirty) {
        char* bstr = cJSON_PrintUnformatted(backend_obj);
        if (bstr) {
            claw_config_set("llm_backend_0", bstr);
            free(bstr);
        }
        /* Reload LLM router so changes take effect immediately */
        llm_router_init();

        /* The router only applies a backend when it is selected at request
         * time.  The proxy itself keeps the host/port/key in its own statics,
         * populated at boot when /data was still empty; without this refresh
         * llm_chat_tools() sees an empty api_key and returns before opening a
         * socket, which looks like a network failure from the outside.
         */
        {
            char host[128] = { 0 };
            char path[128] = { 0 };
            char port[8] = { 0 };
            char key[128] = { 0 };
            char model[64] = { 0 };

            claw_config_get(AGENT_CFG_KEY_LLM_HOST, host, sizeof(host));
            claw_config_get(AGENT_CFG_KEY_LLM_PATH, path, sizeof(path));
            claw_config_get("llm_port", port, sizeof(port));
            claw_config_get(AGENT_CFG_KEY_API_KEY, key, sizeof(key));
            claw_config_get(AGENT_CFG_KEY_MODEL, model, sizeof(model));
            llm_set_all(host, path, port, key, model);
        }
    }

    cJSON_Delete(backend_obj);
    cJSON_Delete(req);
    send_response(fd, 200, "{\"ok\":true}");
    return true;
}

/* -- Skills handlers -------------------------------------------- */

static void url_decode(const char* src, char* dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 1; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = { src[si + 1], src[si + 2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static bool handle_skills_get(int fd)
{
    cJSON* arr = cJSON_CreateArray();
    DIR* dir = opendir(AGENT_SKILLS_DIR);

    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t nlen = strlen(ent->d_name);
            if (nlen < 4 || strcmp(ent->d_name + nlen - 3, ".md") != 0) {
                continue;
            }

            /* Build full path */
            char path[256];
            snprintf(path, sizeof(path), "%s%s", AGENT_SKILLS_DIR,
                ent->d_name);

            struct stat st;
            if (stat(path, &st) != 0) {
                continue;
            }

            /* Extract name (without .md) */
            char name[128];
            snprintf(name, sizeof(name), "%.*s", (int)(nlen - 3),
                ent->d_name);

            /* Try to read description and content */
            char desc[256] = "";
            char content[2048] = "";
            FILE* f = fopen(path, "r");
            if (f) {
                char line[256];
                bool in_front = false;
                int content_len = 0;
                while (fgets(line, sizeof(line), f)) {
                    if (strncmp(line, "---", 3) == 0) {
                        in_front = !in_front;
                        continue;
                    }
                    if (in_front && strncmp(line, "description:", 12) == 0) {
                        char* v = line + 12;
                        while (*v == ' ')
                            v++;
                        size_t vlen = strlen(v);
                        if (vlen > 0 && v[vlen - 1] == '\n')
                            v[vlen - 1] = '\0';
                        snprintf(desc, sizeof(desc), "%s", v);
                    }
                    /* Accumulate content */
                    size_t ll = strlen(line);
                    if (content_len + (int)ll < (int)sizeof(content) - 1) {
                        memcpy(content + content_len, line, ll);
                        content_len += (int)ll;
                    }
                }
                content[content_len] = '\0';
                fclose(f);
            }

            cJSON* skill = cJSON_CreateObject();
            cJSON_AddStringToObject(skill, "name", name);
            cJSON_AddStringToObject(skill, "description", desc);
            cJSON_AddStringToObject(skill, "content", content);
            cJSON_AddStringToObject(skill, "file", ent->d_name);
            cJSON_AddNumberToObject(skill, "size", (double)st.st_size);

            /* mtime as ISO string */
            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            char mtime[32];
            snprintf(mtime, sizeof(mtime), "%04d-%02d-%02dT%02d:%02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
            cJSON_AddStringToObject(skill, "mtime", mtime);

            cJSON_AddItemToArray(arr, skill);
        }
        closedir(dir);
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "skills", arr);
    char* json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (!json) {
        send_response(fd, 500, "{\"error\":\"out of memory\"}");
        return true;
    }

    send_response(fd, 200, json);
    free(json);
    return true;
}

static bool handle_skills_post(int fd, const char* body)
{
    if (!body || *body == '\0') {
        send_response(fd, 400, "{\"error\":\"empty body\"}");
        return true;
    }

    cJSON* req = cJSON_Parse(body);
    if (!req) {
        send_response(fd, 400, "{\"error\":\"invalid json\"}");
        return true;
    }

    cJSON* name_item = cJSON_GetObjectItem(req, "name");
    cJSON* content_item = cJSON_GetObjectItem(req, "content");

    if (!cJSON_IsString(name_item) || !cJSON_IsString(content_item)) {
        cJSON_Delete(req);
        send_response(fd, 400, "{\"error\":\"name and content required\"}");
        return true;
    }

    const char* name = name_item->valuestring;
    const char* content = content_item->valuestring;

    /* Validate name: no path traversal */
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        cJSON_Delete(req);
        send_response(fd, 400, "{\"error\":\"invalid skill name\"}");
        return true;
    }

    /* Write file */
    char path[256];
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, name);

    FILE* f = fopen(path, "w");
    if (!f) {
        cJSON_Delete(req);
        send_response(fd, 500, "{\"error\":\"cannot write file\"}");
        return true;
    }
    fputs(content, f);
    fclose(f);
    cJSON_Delete(req);

    /* Hot-reload skills */
    skill_loader_refresh();

    syslog(LOG_INFO, "[%s] Skill pushed: %s\n", TAG, name);
    send_response(fd, 200, "{\"ok\":true}");
    return true;
}

static bool handle_skills_delete(int fd, const char* name)
{
    if (!name || *name == '\0') {
        send_response(fd, 400, "{\"error\":\"skill name required\"}");
        return true;
    }

    /* URL decode the name (browser/OkHttp encodes Chinese chars) */
    char decoded[128];
    url_decode(name, decoded, sizeof(decoded));

    /* Validate name */
    if (strchr(decoded, '/') || strchr(decoded, '\\') || strstr(decoded, "..")) {
        send_response(fd, 400, "{\"error\":\"invalid skill name\"}");
        return true;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, decoded);

    if (unlink(path) != 0 && errno != ENOENT) {
        send_response(fd, 500, "{\"error\":\"cannot delete file\"}");
        return true;
    }

    skill_loader_refresh();

    syslog(LOG_INFO, "[%s] Skill deleted: %s\n", TAG, decoded);
    send_response(fd, 200, "{\"ok\":true}");
    return true;
}

/* -- Main dispatch ---------------------------------------------- */

/* -- Logs handler ----------------------------------------------- */

static bool handle_logs_get(int fd)
{
    cJSON* arr = agent_logbuf_dump();
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "logs", arr);
    char* json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (!json) {
        send_response(fd, 500, "{\"error\":\"out of memory\"}");
        return true;
    }

    send_response(fd, 200, json);
    free(json);
    return true;
}

/* -- Main dispatch ---------------------------------------------- */

/* Append to a fixed JSON buffer without ever writing past the end.  A
 * truncated answer is still valid JSON here only if the caller reserves the
 * final bytes for the closing braces, which the layout below does.
 */
static int json_append(char *buf, size_t size, int off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (off < 0 || (size_t)off >= size) {
        return (int)size - 1;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf + off, size - (size_t)off, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return off;
    }
    if ((size_t)n >= size - (size_t)off) {
        return (int)size - 1;
    }
    return off + n;
}

static bool handle_health_get(int fd)
{
    health_snapshot_t h;
    char body[2048];
    int off = 0;
    bool ok;

    health_monitor_get(&h);
    ok = h.state == HEALTH_STATE_OK;

    /* The four legacy fields stay byte-compatible; everything after them is
     * additive so older hosts keep working.
     */
    off = json_append(body, sizeof(body), off,
        "{\"ok\":%s,\"uptime\":%lu,\"free_heap\":%lu,\"time_synced\":%s,"
        "\"status\":\"%s\",\"stale\":%s,\"grace\":%s,"
        "\"boot_count\":%u,\"auto_reset_count\":%u,"
        "\"reset_reason\":\"%s\",\"safe_mode\":%s,\"needs_config\":%s,"
        "\"persist_ok\":%s,",
        ok ? "true" : "false",
        (unsigned long)h.uptime_s, (unsigned long)h.heap_free,
        tool_get_time_is_synced() ? "true" : "false",
        health_monitor_state_name(h.state),
        h.stale ? "true" : "false", h.grace ? "true" : "false",
        (unsigned)h.boot_count, (unsigned)h.auto_reset_count,
        health_monitor_reset_reason_name(h.reset_reason),
        h.safe_mode ? "true" : "false", h.needs_config ? "true" : "false",
        h.persist_ok ? "true" : "false");

    off = json_append(body, sizeof(body), off,
        "\"heap\":{\"total\":%lu,\"free\":%lu,\"used\":%lu,"
        "\"largest\":%lu,\"free_min\":%lu,\"ewma\":%lu,"
        "\"slope_kb_per_min\":%ld},",
        (unsigned long)h.heap_total, (unsigned long)h.heap_free,
        (unsigned long)h.heap_used, (unsigned long)h.heap_largest,
        (unsigned long)h.heap_free_min, (unsigned long)h.heap_ewma,
        (long)h.heap_slope_kb_per_min);

    off = json_append(body, sizeof(body), off,
        "\"tasks\":{\"count\":%u,\"baseline\":%u,\"min_prio\":%u},"
        "\"net\":{\"up\":%s,\"ever_up\":%s,\"down_s\":%lu,\"ip\":\"%s\"},",
        (unsigned)h.task_count, (unsigned)h.task_baseline,
        (unsigned)h.min_prio,
        h.net_up ? "true" : "false", h.net_ever_up ? "true" : "false",
        (unsigned long)h.net_down_s, h.net_ip);

    off = json_append(body, sizeof(body), off,
        "\"ws\":{\"listening\":%s,\"clients\":%lu,\"tx_ok\":%lu,"
        "\"tx_fail\":%lu,\"tx_timeout\":%lu,\"rx_frames\":%lu,"
        "\"accept_age_ms\":%lu},",
        h.ws_listening ? "true" : "false",
        (unsigned long)h.ws_clients, (unsigned long)h.ws_tx_ok,
        (unsigned long)h.ws_tx_fail, (unsigned long)h.ws_tx_timeout,
        (unsigned long)h.ws_rx_frames, (unsigned long)h.ws_accept_age_ms);

    off = json_append(body, sizeof(body), off,
        "\"mic\":{\"want\":%s,\"running\":%s,\"frames\":%lu,"
        "\"bytes\":%lu,\"read_errors\":%lu,\"send_errors\":%lu,"
        "\"restarts\":%lu,\"last_frame_age_ms\":%lu},",
        h.mic_want_running ? "true" : "false",
        h.mic_running ? "true" : "false",
        (unsigned long)h.mic_frames, (unsigned long)h.mic_bytes,
        (unsigned long)h.mic_read_errors, (unsigned long)h.mic_send_errors,
        (unsigned long)h.mic_restarts,
        (unsigned long)h.mic_last_frame_age_ms);

    off = json_append(body, sizeof(body), off,
        "\"cron\":{\"running\":%s,\"jobs\":%u,\"fires\":%lu,"
        "\"errors\":%lu,\"beat_age_ms\":%lu},"
        "\"llm\":{\"calls\":%lu,\"failures\":%lu,"
        "\"avg_latency_ms\":%lu,\"backends\":%u,\"backoff\":%u,"
        "\"busy_s\":%lu},",
        h.cron_running ? "true" : "false", (unsigned)h.cron_jobs,
        (unsigned long)h.cron_fires, (unsigned long)h.cron_errors,
        (unsigned long)h.cron_beat_age_ms,
        (unsigned long)h.llm_calls, (unsigned long)h.llm_failures,
        (unsigned long)h.llm_avg_latency_ms, (unsigned)h.llm_backends,
        (unsigned)h.llm_backends_backoff, (unsigned long)h.llm_busy_age_s);

    off = json_append(body, sizeof(body), off,
        "\"bus\":{\"in_depth\":%u,\"out_depth\":%u,"
        "\"in_drops\":%lu,\"out_drops\":%lu},"
        "\"beats\":{\"outbound\":%lu,\"agent_idle\":%lu,"
        "\"agent_llm\":%lu,\"agent_tool\":%lu,\"net\":%lu,"
        "\"cron\":%lu,\"ws_accept\":%lu,\"mic\":%lu,\"lvgl\":%lu},",
        (unsigned)h.bus_in_depth, (unsigned)h.bus_out_depth,
        (unsigned long)h.bus_in_drops, (unsigned long)h.bus_out_drops,
        (unsigned long)h.beat_age_ms[HEALTH_BEAT_OUTBOUND],
        (unsigned long)h.beat_age_ms[HEALTH_BEAT_AGENT_IDLE],
        (unsigned long)h.beat_age_ms[HEALTH_BEAT_AGENT_LLM],
        (unsigned long)h.beat_age_ms[HEALTH_BEAT_AGENT_TOOL],
        (unsigned long)h.beat_age_ms[HEALTH_BEAT_NET],
        (unsigned long)h.beat_age_ms[HEALTH_BEAT_CRON],
        (unsigned long)h.beat_age_ms[HEALTH_BEAT_WS_ACCEPT],
        (unsigned long)h.beat_age_ms[HEALTH_BEAT_MIC],
        (unsigned long)h.beat_age_ms[HEALTH_BEAT_LVGL]);

    off = json_append(body, sizeof(body), off, "\"issues\":[");
    for (int i = 0; i < h.issue_count && i < HEALTH_ISSUE_MAX; i++) {
        off = json_append(body, sizeof(body), off,
            "%s{\"code\":\"%s\",\"severity\":%u,\"streak\":%u,"
            "\"detail\":\"%s\"}",
            i == 0 ? "" : ",", health_monitor_issue_name(h.issues[i].code),
            (unsigned)h.issues[i].severity, (unsigned)h.issues[i].streak,
            h.issues[i].detail);
    }

    off = json_append(body, sizeof(body), off,
        "],\"last_anomaly\":{\"code\":\"%s\",\"severity\":%u,"
        "\"uptime_s\":%lu},"
        "\"recovery\":{\"attempts\":%u,\"reboot_pending\":%s,"
        "\"handling\":%s}}\n",
        health_monitor_issue_name(h.last_issue_code),
        (unsigned)h.last_severity, (unsigned long)h.last_anomaly_uptime_s,
        (unsigned)h.recovery_attempts,
        h.reboot_pending ? "true" : "false",
        h.handling ? "true" : "false");

    body[sizeof(body) - 1] = '\0';
    send_response(fd, 200, body);
    return true;
}

static bool handle_health_ack_post(int fd)
{
    health_monitor_notify_host_ack();
    syslog(LOG_INFO, "[%s] Host acknowledged health recovery\n", TAG);
    send_response(fd, 200, "{\"ok\":true}");
    return true;
}

#ifdef CONFIG_AI_AGENT_HEALTH_TEST
static bool handle_health_fault_post(int fd, const char *body)
{
    cJSON *req;
    cJSON *fault;
    const char *name;
    uint32_t seconds = 0;
    uint32_t count = 0;
    uint32_t kb = 0;
    int rc;

    if (body == NULL || body[0] == '\0') {
        send_response(fd, 400, "{\"error\":\"empty body\"}");
        return true;
    }

    req = cJSON_Parse(body);
    if (req == NULL) {
        send_response(fd, 400, "{\"error\":\"invalid json\"}");
        return true;
    }

    fault = cJSON_GetObjectItem(req, "fault");
    if (!cJSON_IsString(fault) || fault->valuestring[0] == '\0') {
        cJSON_Delete(req);
        send_response(fd, 400, "{\"error\":\"fault required\"}");
        return true;
    }
    name = fault->valuestring;

    cJSON *item = cJSON_GetObjectItem(req, "seconds");
    if (cJSON_IsNumber(item)) {
        seconds = (uint32_t)item->valuedouble;
    }
    item = cJSON_GetObjectItem(req, "count");
    if (cJSON_IsNumber(item)) {
        count = (uint32_t)item->valuedouble;
    }
    item = cJSON_GetObjectItem(req, "kb");
    if (cJSON_IsNumber(item)) {
        kb = (uint32_t)item->valuedouble;
    }

    rc = health_monitor_fault_set(name, seconds, count, kb);

    /* The name points into req, so it must not be used after this. */
    cJSON_Delete(req);

    if (rc != OK) {
        send_response(fd, 400, "{\"error\":\"unknown fault\"}");
        return true;
    }

    send_response(fd, 200, "{\"ok\":true}");
    return true;
}
#endif /* CONFIG_AI_AGENT_HEALTH_TEST */

bool api_try_handle(int fd, const char* buf, int buf_len)
{
    /* Quick check: must start with a valid HTTP method targeting /api/ */
    if (!buf || buf_len < 10) {
        return false;
    }

    if (strncmp(buf, "GET /api/", 9) != 0 && strncmp(buf, "PUT /api/", 9) != 0 && strncmp(buf, "POST /api/", 10) != 0 && strncmp(buf, "DELETE /api/", 12) != 0) {
        return false;
    }

    /* Parse method and path */
    char method[8] = "";
    char path[128] = "";
    sscanf(buf, "%7s %127s", method, path);

    syslog(LOG_INFO, "[%s] api_try_handle: %s %s\n", TAG, method, path);

    /* Route: /api/health */
    if (strcmp(path, "/api/health") == 0 && strcmp(method, "GET") == 0) {
        return handle_health_get(fd);
    }

    /* Route: /api/health/ack */
    if (strcmp(path, "/api/health/ack") == 0
        && strcmp(method, "POST") == 0) {
        return handle_health_ack_post(fd);
    }

#ifdef CONFIG_AI_AGENT_HEALTH_TEST
    /* Route: /api/health/fault (HIL builds only) */
    if (strcmp(path, "/api/health/fault") == 0
        && strcmp(method, "POST") == 0) {
        char *body = read_full_body(fd, buf, buf_len);
        bool ret = handle_health_fault_post(fd, body);

        free(body);
        return ret;
    }
#endif

    /* Route: /api/config */
    if (strcmp(path, "/api/config") == 0) {
        if (strcmp(method, "GET") == 0) {
            return handle_config_get(fd);
        }
        if (strcmp(method, "PUT") == 0) {
            char* body = read_full_body(fd, buf, buf_len);
            bool ret = handle_config_put(fd, body);
            free(body);
            return ret;
        }
    }

    /* Route: /api/llm/mode */
    if (strncmp(path, "/api/llm/mode", 13) == 0) {
        const char* query = strchr(path, '?');

        /* Body-less form for flaky links and for the UI selftest:
         *   POST /api/llm/mode?mode=cloud
         * It calls the same function as the settings-page button.
         */
        if (strcmp(method, "POST") == 0 && query != NULL
            && strncmp(query, "?mode=", 6) == 0) {
            const char* mode_str = query + 6;
            bool local;
            bool valid;
            char resp[128];

            if (strncmp(mode_str, "local", 5) == 0
                || strncmp(mode_str, "mock", 4) == 0) {
                local = true;
                valid = true;
            } else if (strncmp(mode_str, "cloud", 5) == 0
                || strncmp(mode_str, "real", 4) == 0) {
                local = false;
                valid = true;
            } else {
                valid = false;
                local = true;
            }

            if (!valid || api_llm_set_mode(local) != OK) {
                send_response(fd, 400, "{\"error\":\"mode switch failed\"}");
                return true;
            }

            snprintf(resp, sizeof(resp),
                "{\"ok\":true,\"mode\":\"%s\",\"port\":\"%s\",\"model\":\"%s\"}",
                local ? "local" : "cloud",
                local ? LLM_MODE_LOCAL_PORT : LLM_MODE_CLOUD_PORT,
                local ? LLM_MODE_LOCAL_MODEL : LLM_MODE_CLOUD_MODEL);
            send_response(fd, 200, resp);
            return true;
        }

        if (strcmp(path, "/api/llm/mode") == 0) {
        if (strcmp(method, "GET") == 0) {
            bool local = api_llm_mode_is_local();
            char body[128];

            snprintf(body, sizeof(body),
                "{\"mode\":\"%s\",\"port\":\"%s\",\"model\":\"%s\"}",
                local ? "local" : "cloud",
                local ? LLM_MODE_LOCAL_PORT : LLM_MODE_CLOUD_PORT,
                local ? LLM_MODE_LOCAL_MODEL : LLM_MODE_CLOUD_MODEL);
            send_response(fd, 200, body);
            return true;
        }
        if (strcmp(method, "POST") == 0) {
            char* body = read_full_body(fd, buf, buf_len);
            cJSON* req = body ? cJSON_Parse(body) : NULL;
            cJSON* item = req ? cJSON_GetObjectItem(req, "mode") : NULL;
            bool local;
            char resp[128];

            if (!item || !cJSON_IsString(item)
                || (strcmp(item->valuestring, "local") != 0
                    && strcmp(item->valuestring, "mock") != 0
                    && strcmp(item->valuestring, "cloud") != 0
                    && strcmp(item->valuestring, "real") != 0)) {
                cJSON_Delete(req);
                free(body);
                send_response(fd, 400,
                    "{\"error\":\"mode must be local or cloud\"}");
                return true;
            }

            local = strcmp(item->valuestring, "local") == 0
                || strcmp(item->valuestring, "mock") == 0;
            cJSON_Delete(req);
            free(body);

            if (api_llm_set_mode(local) != OK) {
                send_response(fd, 400, "{\"error\":\"mode switch failed\"}");
                return true;
            }

            snprintf(resp, sizeof(resp),
                "{\"ok\":true,\"mode\":\"%s\",\"port\":\"%s\",\"model\":\"%s\"}",
                local ? "local" : "cloud",
                local ? LLM_MODE_LOCAL_PORT : LLM_MODE_CLOUD_PORT,
                local ? LLM_MODE_LOCAL_MODEL : LLM_MODE_CLOUD_MODEL);
            send_response(fd, 200, resp);
            return true;
        }
        } /* exact /api/llm/mode (no query) */
    }

    /* Route: /api/skills */
    if (strcmp(path, "/api/skills") == 0) {
        if (strcmp(method, "GET") == 0) {
            return handle_skills_get(fd);
        }
        if (strcmp(method, "POST") == 0) {
            char* body = read_full_body(fd, buf, buf_len);
            bool ret = handle_skills_post(fd, body);
            free(body);
            return ret;
        }
    }

    /* Route: /api/skills/{name} */
    if (strncmp(path, "/api/skills/", 12) == 0 && strcmp(method, "DELETE") == 0) {
        return handle_skills_delete(fd, path + 12);
    }

    /* Route: /api/logs */
    if (strncmp(path, "/api/logs", 9) == 0 && strcmp(method, "GET") == 0) {
        return handle_logs_get(fd);
    }

    return false;
}
