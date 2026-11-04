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

/**
 * lvgl_ui_channel.c — LVGL UI channel for AI Agent
 *
 * Three-page UI on the watch panel:
 *
 *   Home    clock, date, RAM/task/network summary, latest reply
 *   Chat    chat bubbles, quick command chips, push-to-talk
 *   Status  RAM bar, task/network/cron/uptime, one-tap patrol
 *
 * A bottom dock switches pages; the tileview also supports horizontal
 * swipes.  All widget work stays on the LVGL thread.  Slow data (mallinfo,
 * task enumeration, network, cron) is sampled by a low-priority background
 * thread into a mutex-protected snapshot; the LVGL timer only formats it.
 */

#include "ui/lvgl_ui_channel.h"
#include "channels/ws_server.h"
#include "channels/mic_stream.h"
#include "core/health_monitor.h"
#include "core/message_bus.h"
#include "infra/api_handler.h"
#include "infra/network_manager.h"
#include "infra/cron_service.h"
#include "tools/tool_get_time.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_channel.h"

#include <errno.h>
#include <lvgl/lvgl.h>
#include <malloc.h>
#include <nuttx/sched.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* CJK font sources, in order of preference:
 *   1. uikit's font manager — needs CONFIG_UIKIT + LV_USE_FREETYPE and a .ttf
 *      deployed on the device at runtime (the original design).
 *   2. A font compiled into the firmware (CONFIG_AI_AGENT_LVGL_UI_CJK_FONT) —
 *      no runtime dependency, which is what standalone agent firmware uses.
 * Without either, the file must still build: the chat text is Chinese, so it
 * would then render blank with LVGL's built-in Montserrat. This include used
 * to be unconditional, which made the whole channel fail to compile whenever
 * uikit was absent. */
#if defined(CONFIG_UIKIT) && defined(CONFIG_UIKIT_FONT_MANAGER)
#include <uikit/uikit_font_manager.h>
#define LVGL_UI_HAVE_CJK_FONT 1
#else
#define LVGL_UI_HAVE_CJK_FONT 0
#endif

#if defined(CONFIG_AI_AGENT_LVGL_UI_CJK_FONT)
extern const lv_font_t lv_font_misans_18_cjk;
#define LVGL_UI_BUILTIN_CJK_FONT (&lv_font_misans_18_cjk)
#else
#define LVGL_UI_BUILTIN_CJK_FONT NULL
#endif

#if defined(CONFIG_LV_FONT_MONTSERRAT_48)
#define UI_CLOCK_FONT (&lv_font_montserrat_48)
#else
#define UI_CLOCK_FONT (&lv_font_montserrat_14)
#endif

/* ── Constants ────────────────────────────────────────────────── */

static const char* TAG = "lvgl_ui";

#define LVGL_UI_FB_PATH "/dev/lcd0"
#define LVGL_UI_INPUT_PATH "/dev/input0"

#define LVGL_UI_MSG_MAX_LEN 512
#define CHAT_HISTORY_MAX 20
#define UI_QUEUE_MAX 12

#define LVGL_UI_PTT_SIZE 96
#define LVGL_UI_CHAT_GAP 8
#define LVGL_UI_BUBBLE_RADIUS 14
#define LVGL_UI_BUBBLE_PAD 10

#define UI_STATUS_H 24
#define UI_DOCK_H 48
#define UI_DOCK_Y (-12)
#define UI_STATUS_Y 18
#define UI_CONTENT_Y 60

#define PTT_ICON_MIC "\xe8\xaf\xb7\xe8\xaf\xb4" /* "请说" in UTF-8 */

/* Midnight amber palette */
#define UI_C_BG        0x0B0F14
#define UI_C_BG2       0x101820
#define UI_C_SURFACE   0x171E26
#define UI_C_SURFACE2  0x202A33
#define UI_C_BORDER    0x2A3642
#define UI_C_TEXT      0xF5F7FA
#define UI_C_DIM       0x8B98A5
#define UI_C_AMBER     0xF5B942
#define UI_C_TEAL      0x5EEAD4
#define UI_C_RED       0xF87171
#define UI_C_DARKTEXT  0x0B0F14

#if defined(CONFIG_LV_FONT_MONTSERRAT_24)
#define UI_FONT_VALUE (&lv_font_montserrat_24)
#else
#define UI_FONT_VALUE (&lv_font_montserrat_14)
#endif

/* ── Data structures ──────────────────────────────────────────── */

typedef struct {
    char text[LVGL_UI_MSG_MAX_LEN];
    char when[6];             /* HH:MM, or --:-- when the clock is not set */
    bool is_user;
    lv_obj_t* bubble;
} chat_msg_t;

typedef struct {
    chat_msg_t msgs[CHAT_HISTORY_MAX];
    int head;
    int count;
} chat_history_t;

typedef struct {
    bool valid;
    uint32_t seq;
    uint32_t ram_used;
    uint32_t ram_total;
    uint32_t ram_maxfree;
    int task_count;
    int min_prio;
    bool net_up;
    char ip[16];
    int cron_count;
    uint32_t uptime_s;
    health_snapshot_t health;
} ui_status_snapshot_t;

typedef struct {
    bool initialized;
    bool running;
    pthread_mutex_t lock;

    /* Display geometry, read at runtime (390x450 vs 466x466 differs by board) */
    int disp_w;
    int disp_h;
    int safe_w;
    int content_y;
    int content_h;
    int dock_w;

    lv_obj_t* screen;
    lv_obj_t* status_bar;
    lv_obj_t* status_time;
    lv_obj_t* status_net;

    lv_obj_t* tileview;
    lv_obj_t* home_page;
    lv_obj_t* chat_page;
    lv_obj_t* status_page;
    lv_obj_t* settings_page;

    lv_obj_t* home_arc;
    lv_obj_t* home_clock;
    lv_obj_t* home_date;
    lv_obj_t* home_weekday;
    lv_obj_t* home_ram;
    lv_obj_t* home_task;
    lv_obj_t* home_net;
    lv_obj_t* home_cron;
    lv_obj_t* home_latest;
    lv_obj_t* home_chat_btn;
    lv_obj_t* home_patrol_btn;

    lv_obj_t* chat_list;
    lv_obj_t* ptt_btn;
    lv_obj_t* ptt_label;
    lv_obj_t* rec_indicator;
    lv_anim_t rec_anim;

    lv_obj_t* status_health_dot;
    lv_obj_t* status_health_banner;
    lv_obj_t* status_health_label;
    lv_obj_t* status_health_detail;
    lv_obj_t* status_ram_arc;
    lv_obj_t* status_ram_label;
    lv_obj_t* status_ram_bar;
    lv_obj_t* status_task_label;
    lv_obj_t* status_net_label;
    lv_obj_t* status_cron_label;
    lv_obj_t* status_uptime_label;
    lv_obj_t* status_patrol_btn;
    lv_obj_t* mic_gain_label;
    lv_obj_t* sync_label;
    lv_obj_t* llm_mode_label;

    lv_obj_t* dock_btn[4];

    lv_timer_t* clock_timer;
    lv_timer_t* status_timer;

    /* Background status sampler */
    pthread_t sampler_thread;
    volatile bool sampler_running;
    bool sampler_started;
    pthread_mutex_t status_lock;
    ui_status_snapshot_t status_snap;

    lv_font_t* font;

    chat_history_t history;

    bool is_recording;
    bool is_processing;
    uint32_t processing_started_ms;

    bool screen_visible;
    lv_obj_t* prev_screen;

    bool lvgl_owned;
    pthread_t lvgl_thread;
} lvgl_ui_state_t;

static lvgl_ui_state_t s_state;

/* ── Cross-thread message queue ───────────────────────────── */

typedef struct {
    char text[LVGL_UI_MSG_MAX_LEN];
    bool is_user;
} async_msg_t;

/* How long the PTT button may stay in "processing" without any reply. */
#define LVGL_UI_PROCESSING_TIMEOUT_MS 30000

static async_msg_t* s_queue[UI_QUEUE_MAX];
static int s_queue_head;
static int s_queue_count;
static bool s_show_requested;
static pthread_mutex_t s_queue_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── Forward declarations ─────────────────────────────────── */

static void ui_drain(void);
static void chat_view_add_message(const char* text, bool is_user);
static void chat_view_scroll_to_bottom(void);
static void show_screen_async_cb(void* data);
static void ptt_btn_event_cb(lv_event_t* e);
static void recording_indicator_start(void);
static void recording_indicator_stop(void);
static void recording_indicator_show_processing(void);
static void clock_timer_cb(lv_timer_t* t);
static void status_timer_cb(lv_timer_t* t);
static void* status_sampler_thread(void* arg);
static void ui_update_dock(int idx);
static void ui_show_page(int idx);
static void ui_push_command(const char* text);
static void ui_push_command_stay(const char* text);
static void create_settings_page(lv_obj_t* page);

/* ── Small helpers ─────────────────────────────────────────── */

static const lv_font_t* ui_base_font(void)
{
    return LVGL_UI_BUILTIN_CJK_FONT ? LVGL_UI_BUILTIN_CJK_FONT
                                    : &lv_font_montserrat_14;
}

static const char* ui_weekday_cn(int wday)
{
    static const char* names[] = {
        "周日", "周一", "周二", "周三", "周四", "周五", "周六"
    };
    if (wday < 0 || wday > 6) {
        return "";
    }
    return names[wday];
}

static void ui_make_page_opaque(lv_obj_t* obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t* ui_make_label(lv_obj_t* parent, const char* text,
                               const lv_font_t* font, lv_color_t color)
{
    lv_obj_t* lbl = lv_label_create(parent);

    if (!lbl) {
        return NULL;
    }
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    return lbl;
}

static lv_obj_t* ui_make_button(lv_obj_t* parent, const char* text,
                                lv_coord_t w, lv_coord_t h,
                                lv_event_cb_t cb, void* user_data)
{
    lv_obj_t* btn = lv_btn_create(parent);

    if (!btn) {
        return NULL;
    }
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_C_BORDER), 0);

    lv_obj_t* lbl = lv_label_create(btn);

    if (lbl) {
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, ui_base_font(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_C_TEXT), 0);
        lv_obj_center(lbl);
    }
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

static lv_obj_t* ui_make_card(lv_obj_t* parent, lv_coord_t x, lv_coord_t y,
                              lv_coord_t w, lv_coord_t h)
{
    lv_obj_t* card = lv_obj_create(parent);

    if (!card) {
        return NULL;
    }
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_C_BORDER), 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static const char* ui_icon_for_title(const char* title)
{
    if (strcmp(title, "内存") == 0) {
        return LV_SYMBOL_DRIVE;
    }
    if (strcmp(title, "任务") == 0) {
        return LV_SYMBOL_LIST;
    }
    if (strcmp(title, "网络") == 0) {
        return LV_SYMBOL_WIFI;
    }
    if (strcmp(title, "定时") == 0) {
        return LV_SYMBOL_BELL;
    }
    if (strcmp(title, "运行") == 0) {
        return LV_SYMBOL_REFRESH;
    }
    return NULL;
}

static lv_obj_t* ui_make_stat_card(lv_obj_t* parent,
                                   lv_coord_t x, lv_coord_t y,
                                   lv_coord_t w, lv_coord_t h,
                                   const char* title,
                                   const lv_font_t* value_font,
                                   lv_color_t value_color,
                                   lv_obj_t** value_out)
{
    lv_obj_t* card = ui_make_card(parent, x, y, w, h);
    lv_obj_t* lbl;
    lv_obj_t* value;

    if (!card) {
        return NULL;
    }

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char* sym = ui_icon_for_title(title);
    if (sym) {
        lv_obj_t* icon = ui_make_label(card, sym, &lv_font_montserrat_14,
                                       value_color);
        (void)icon;
    }

    lbl = ui_make_label(card, title, ui_base_font(), lv_color_hex(UI_C_DIM));
    if (lbl) {
        lv_obj_set_style_pad_right(lbl, 4, 0);
    }

    value = ui_make_label(card, "--", value_font, value_color);
    if (value) {
        lv_obj_set_width(value, w - 82);
        lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    }

    if (value_out) {
        *value_out = value;
    }
    return card;
}

static bool ui_wait_for_device(const char* path, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        if (access(path, F_OK) == 0) {
            return true;
        }
        usleep(100000);
        waited += 100;
    }
    return access(path, F_OK) == 0;
}

/* ── Ring buffer ──────────────────────────────────────────────── */

static chat_msg_t* chat_history_add(chat_history_t* h, const char* text,
                                    bool is_user)
{
    chat_msg_t* slot = &h->msgs[h->head];
    time_t now;
    struct tm tm;

    if (h->count == CHAT_HISTORY_MAX) {
        if (slot->bubble) {
            lv_obj_del(slot->bubble);
            slot->bubble = NULL;
        }
    } else {
        h->count++;
    }

    strncpy(slot->text, text, LVGL_UI_MSG_MAX_LEN - 1);
    slot->text[LVGL_UI_MSG_MAX_LEN - 1] = '\0';

    if (tool_get_time_is_synced()) {
        now = tool_get_time_now() + 8 * 3600;
        gmtime_r(&now, &tm);
        snprintf(slot->when, sizeof(slot->when), "%02d:%02d",
                 tm.tm_hour, tm.tm_min);
    } else {
        strcpy(slot->when, "--:--");
    }

    slot->is_user = is_user;
    slot->bubble = NULL;
    h->head = (h->head + 1) % CHAT_HISTORY_MAX;
    return slot;
}

/* ── Status sampling (background thread) ────────────────────── */

static void ui_status_sample(void)
{
    ui_status_snapshot_t snap;
    health_snapshot_t hs;
    static int sync_tick;

    memset(&snap, 0, sizeof(snap));

    /* One snapshot, published by the health sampler.  This thread used to
     * call mallinfo(), nxsched_foreach() and cron_list_jobs() itself, so
     * the screen and the REST endpoint could disagree about the same
     * instant -- and the cron copy alone was sixteen full job records.
     */
    health_monitor_get(&hs);
    if (hs.sample_ms == 0) {
        return; /* sampler not up yet; keep the UI on -- */
    }

    snap.ram_used = hs.heap_used;
    snap.ram_total = hs.heap_total;
    snap.ram_maxfree = hs.heap_largest;
    snap.task_count = hs.task_count;
    snap.min_prio = hs.min_prio;
    snap.net_up = hs.net_up;
    if (hs.net_ip[0]) {
        strncpy(snap.ip, hs.net_ip, sizeof(snap.ip) - 1);
    }
    snap.cron_count = hs.cron_jobs;
    snap.uptime_s = hs.uptime_s;
    snap.health = hs;

    /* One-shot clock sync, retried when the link comes up.  The HTTP time
     * call can take seconds, so it runs here, never on the LVGL thread. */
    if (!tool_get_time_is_synced() && snap.net_up) {
        if ((sync_tick++ % 6) == 0) {
            char out[160];

            tool_get_time_execute("{}", out, sizeof(out));
        }
    }

    pthread_mutex_lock(&s_state.status_lock);
    snap.valid = true;
    snap.seq = s_state.status_snap.seq + 1;
    s_state.status_snap = snap;
    pthread_mutex_unlock(&s_state.status_lock);
}

static void* status_sampler_thread(void* arg)
{
    int i;

    (void)arg;
    pthread_setname_np(pthread_self(), "ui_status");
    while (s_state.sampler_running) {
        ui_status_sample();
        for (i = 0; i < 5 && s_state.sampler_running; i++) {
            sleep(1);
        }
    }
    return NULL;
}

static void ui_start_sampler(void)
{
    pthread_attr_t attr;
    int rc;

    if (s_state.sampler_started) {
        return;
    }

    s_state.sampler_running = true;
    pthread_attr_init(&attr);
    /* mallinfo()/nxsched_foreach()/getifaddrs()/the HTTP time sync all run
     * here; 8 KB overflowed into a hard fault in the first boot.  Match the
     * LVGL thread's 32 KB rather than chase the exact frame. */
    pthread_attr_setstacksize(&attr, 32768);
    rc = pthread_create(&s_state.sampler_thread, &attr,
                        status_sampler_thread, NULL);
    pthread_attr_destroy(&attr);

    if (rc == 0) {
        s_state.sampler_started = true;
    } else {
        s_state.sampler_running = false;
        syslog(LOG_ERR, "[%s] status sampler create failed (%d)\n", TAG, rc);
    }
}

static void ui_stop_sampler(void)
{
    if (!s_state.sampler_started) {
        return;
    }

    s_state.sampler_running = false;
    pthread_join(s_state.sampler_thread, NULL);
    s_state.sampler_started = false;
}

/* ── Timers ─────────────────────────────────────────────────── */

static void clock_timer_cb(lv_timer_t* t)
{
    bool synced = tool_get_time_is_synced();
    char clock[8] = "--:--";
    char date[24] = "未校时";
    char weekday[16] = "";
    int sec = 0;

    (void)t;

    if (synced) {
        time_t now = tool_get_time_now() + 8 * 3600;
        struct tm tm;

        gmtime_r(&now, &tm);
        snprintf(clock, sizeof(clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
        snprintf(date, sizeof(date), "%d月%d日", tm.tm_mon + 1, tm.tm_mday);
        snprintf(weekday, sizeof(weekday), "%s", ui_weekday_cn(tm.tm_wday));
        sec = tm.tm_sec;
    }

    if (s_state.home_arc) {
        lv_arc_set_value(s_state.home_arc, sec);
    }
    if (s_state.status_time) {
        lv_label_set_text(s_state.status_time, clock);
    }
    if (s_state.home_clock) {
        lv_label_set_text(s_state.home_clock, clock);
    }
    if (s_state.home_date) {
        lv_label_set_text(s_state.home_date, date);
    }
    if (s_state.home_weekday) {
        lv_label_set_text(s_state.home_weekday, weekday);
    }
    if (s_state.llm_mode_label) {
        static int last_llm_local = -1;
        int llm_local = api_llm_mode_is_local() ? 1 : 0;

        if (llm_local != last_llm_local) {
            lv_label_set_text(s_state.llm_mode_label,
                              llm_local ? "本地 mock" : "云端 LLM");
            lv_obj_set_style_text_color(s_state.llm_mode_label,
                lv_color_hex(llm_local ? UI_C_TEAL : UI_C_AMBER), 0);
            last_llm_local = llm_local;
        }
    }
    if (s_state.sync_label) {
        lv_label_set_text(s_state.sync_label, synced ? "已校时" : "未校时");
        lv_obj_set_style_text_color(s_state.sync_label,
            lv_color_hex(synced ? UI_C_TEAL : UI_C_DIM), 0);
    }
}

static lv_color_t health_state_color(health_state_t state)
{
    switch (state) {
    case HEALTH_STATE_OK:
        return lv_color_hex(UI_C_TEAL);
    case HEALTH_STATE_DEGRADED:
        return lv_color_hex(UI_C_AMBER);
    case HEALTH_STATE_CRITICAL:
        return lv_color_hex(UI_C_RED);
    default:
        return lv_color_hex(UI_C_DIM);
    }
}

static const char* health_state_cn(health_state_t state)
{
    switch (state) {
    case HEALTH_STATE_OK:
        return "正常";
    case HEALTH_STATE_DEGRADED:
        return "降级";
    case HEALTH_STATE_CRITICAL:
        return "严重";
    default:
        return "启动中";
    }
}

static void status_timer_cb(lv_timer_t* t)
{
    ui_status_snapshot_t snap;
    static uint32_t last_seq;
    int ram_pct;

    (void)t;

    pthread_mutex_lock(&s_state.status_lock);
    snap = s_state.status_snap;
    pthread_mutex_unlock(&s_state.status_lock);

    if (!snap.valid || snap.seq == last_seq) {
        return;
    }
    last_seq = snap.seq;

    ram_pct = snap.ram_total ? (int)((snap.ram_used * 100U) / snap.ram_total)
                             : 0;

    /* Health dot and banner.  The dot is a plain circle rather than a
     * glyph, so it renders the same with or without the CJK font.
     */
    if (s_state.status_health_dot) {
        lv_obj_set_style_bg_color(s_state.status_health_dot,
            health_state_color(snap.health.state), 0);
    }
    if (s_state.status_health_label) {
        if (snap.health.stale) {
            lv_label_set_text(s_state.status_health_label, "健康 采样停滞");
            lv_obj_set_style_text_color(s_state.status_health_label,
                lv_color_hex(UI_C_AMBER), 0);
        } else {
            lv_label_set_text_fmt(s_state.status_health_label, "健康 %s",
                health_state_cn(snap.health.state));
            lv_obj_set_style_text_color(s_state.status_health_label,
                health_state_color(snap.health.state), 0);
        }
    }
    if (s_state.status_health_banner) {
        lv_obj_set_style_border_color(s_state.status_health_banner,
            health_state_color(snap.health.state), 0);
    }
    if (s_state.status_health_detail) {
        if (snap.health.issue_count > 0) {
            const health_issue_t* issue = &snap.health.issues[0];

            lv_label_set_text_fmt(s_state.status_health_detail,
                "%s%s • %s",
                issue->detail[0] ? issue->detail : "",
                issue->detail[0] ? " " : "",
                issue->severity >= HEALTH_SEV_CRITICAL ? "严重" : "警告");
        } else {
            lv_label_set_text_fmt(s_state.status_health_detail,
                "内存 %d%% • 任务 %d • 连接 %lu", ram_pct,
                snap.task_count, (unsigned long)snap.health.ws_clients);
        }
    }

    if (s_state.home_ram) {
        if (snap.ram_total) {
            lv_label_set_text_fmt(s_state.home_ram, "%d%%", ram_pct);
        } else {
            lv_label_set_text(s_state.home_ram, "--");
        }
    }
    if (s_state.home_task) {
        lv_label_set_text_fmt(s_state.home_task, "%d", snap.task_count);
    }
    if (s_state.home_net) {
        lv_label_set_text(s_state.home_net,
                          snap.net_up ? "在线" : "离线");
        lv_obj_set_style_text_color(s_state.home_net,
            lv_color_hex(snap.net_up ? UI_C_TEAL : UI_C_DIM), 0);
    }
    if (s_state.home_cron) {
        lv_label_set_text_fmt(s_state.home_cron, "%d", snap.cron_count);
    }

    if (s_state.status_net) {
        lv_label_set_text(s_state.status_net,
                          snap.net_up ? "在线" : "离线");
        lv_obj_set_style_text_color(s_state.status_net,
            lv_color_hex(snap.net_up ? UI_C_TEAL : UI_C_DIM), 0);
    }
    if (s_state.status_ram_label) {
        if (snap.ram_total) {
            lv_label_set_text_fmt(s_state.status_ram_label,
                                  "%d%%", ram_pct);
        } else {
            lv_label_set_text(s_state.status_ram_label, "--");
        }
    }
    if (s_state.status_ram_arc) {
        lv_arc_set_value(s_state.status_ram_arc, ram_pct);
    }
    if (s_state.status_ram_bar) {
        lv_bar_set_value(s_state.status_ram_bar, ram_pct, LV_ANIM_OFF);
    }
    if (s_state.status_task_label) {
        if (snap.min_prio < 255) {
            lv_label_set_text_fmt(s_state.status_task_label,
                "%d/%d", snap.task_count, snap.min_prio);
        } else {
            lv_label_set_text_fmt(s_state.status_task_label,
                                  "%d", snap.task_count);
        }
    }
    if (s_state.status_net_label) {
        if (snap.net_up && snap.ip[0]) {
            lv_label_set_text_fmt(s_state.status_net_label,
                                  "%s", snap.ip);
        } else {
            lv_label_set_text(s_state.status_net_label,
                              snap.net_up ? "在线" : "离线");
        }
        lv_obj_set_style_text_color(s_state.status_net_label,
            lv_color_hex(snap.net_up ? UI_C_TEAL : UI_C_DIM), 0);
    }
    if (s_state.status_cron_label) {
        lv_label_set_text_fmt(s_state.status_cron_label, "%d",
                              snap.cron_count);
    }
    if (s_state.status_uptime_label) {
        lv_label_set_text_fmt(s_state.status_uptime_label,
                              "%luh %lum",
                              (unsigned long)(snap.uptime_s / 3600U),
                              (unsigned long)((snap.uptime_s / 60U) % 60U));
    }
}

/* ── Page builders ──────────────────────────────────────────── */

static void home_chat_cb(lv_event_t* e)
{
    (void)e;
    ui_show_page(1);
}

static void home_patrol_cb(lv_event_t* e)
{
    (void)e;
    ui_push_command("立即巡检设备，采集内存、任务数和存储数据");
}

static void status_patrol_cb(lv_event_t* e)
{
    (void)e;
    ui_push_command("立即巡检设备，采集内存、任务数和存储数据");
}

static void quick_cmd_cb(lv_event_t* e)
{
    const char* cmd = (const char*)lv_event_get_user_data(e);

    if (cmd) {
        ui_push_command(cmd);
    }
}

static void create_status_bar(lv_obj_t* parent)
{
    lv_coord_t w = s_state.safe_w < 180 ? s_state.safe_w : 180;

    s_state.status_bar = lv_obj_create(parent);
    if (!s_state.status_bar) {
        return;
    }

    lv_obj_set_size(s_state.status_bar, w, UI_STATUS_H);
    lv_obj_align(s_state.status_bar, LV_ALIGN_TOP_MID, 0, UI_STATUS_Y);
    lv_obj_set_style_bg_opa(s_state.status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_state.status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_state.status_bar, 0, 0);
    lv_obj_clear_flag(s_state.status_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_state.status_time = ui_make_label(s_state.status_bar, "--:--",
                                        &lv_font_montserrat_14,
                                        lv_color_hex(UI_C_DIM));
    if (s_state.status_time) {
        lv_obj_align(s_state.status_time, LV_ALIGN_LEFT_MID, 4, 0);
    }

    s_state.status_net = ui_make_label(s_state.status_bar, "离线",
                                       ui_base_font(),
                                       lv_color_hex(UI_C_DIM));
    if (s_state.status_net) {
        lv_obj_align(s_state.status_net, LV_ALIGN_RIGHT_MID, -4, 0);
    }

    /* Health dot: shape, not glyph, so it never depends on the font. */
    s_state.status_health_dot = lv_obj_create(s_state.status_bar);
    if (s_state.status_health_dot) {
        lv_obj_set_size(s_state.status_health_dot, 8, 8);
        lv_obj_align(s_state.status_health_dot, LV_ALIGN_RIGHT_MID, -46, 0);
        lv_obj_set_style_radius(s_state.status_health_dot,
                                LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_state.status_health_dot,
                                  lv_color_hex(UI_C_DIM), 0);
        lv_obj_set_style_bg_opa(s_state.status_health_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_state.status_health_dot, 0, 0);
        lv_obj_clear_flag(s_state.status_health_dot, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void create_home_page(lv_obj_t* page)
{
    int arc_size;
    int gap = 8;
    int card_w;
    int card_h = 48;
    int cards_y;
    int latest_y;

    ui_make_page_opaque(page);

    arc_size = LV_MIN(s_state.safe_w - 40, 180);
    s_state.home_arc = lv_arc_create(page);
    if (s_state.home_arc) {
        lv_obj_set_size(s_state.home_arc, arc_size, arc_size);
        lv_obj_align(s_state.home_arc, LV_ALIGN_TOP_MID, 0, 4);
        lv_arc_set_range(s_state.home_arc, 0, 59);
        lv_arc_set_bg_angles(s_state.home_arc, 135, 405);
        lv_arc_set_value(s_state.home_arc, 0);
        lv_obj_set_style_arc_width(s_state.home_arc, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_state.home_arc,
                                   lv_color_hex(UI_C_SURFACE2), LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_state.home_arc, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_state.home_arc,
                                   lv_color_hex(UI_C_AMBER), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(s_state.home_arc, true,
                                     LV_PART_INDICATOR);
        lv_obj_set_style_opa(s_state.home_arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_bg_opa(s_state.home_arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_clear_flag(s_state.home_arc, LV_OBJ_FLAG_CLICKABLE);
    }

    s_state.home_clock = ui_make_label(page, "--:--", UI_CLOCK_FONT,
                                       lv_color_hex(UI_C_TEXT));
    if (s_state.home_clock) {
        lv_obj_set_width(s_state.home_clock, arc_size - 16);
        lv_obj_set_style_text_align(s_state.home_clock,
                                    LV_TEXT_ALIGN_CENTER, 0);
        if (s_state.home_arc) {
            lv_obj_align_to(s_state.home_clock, s_state.home_arc,
                            LV_ALIGN_CENTER, 0, 0);
        } else {
            lv_obj_align(s_state.home_clock, LV_ALIGN_TOP_MID, 0, 20);
        }
    }

    s_state.home_date = ui_make_label(page, "未校时", ui_base_font(),
                                      lv_color_hex(UI_C_DIM));
    if (s_state.home_date) {
        lv_obj_set_width(s_state.home_date, arc_size - 16);
        lv_obj_set_style_text_align(s_state.home_date,
                                    LV_TEXT_ALIGN_CENTER, 0);
        if (s_state.home_arc) {
            lv_obj_align_to(s_state.home_date, s_state.home_arc,
                            LV_ALIGN_CENTER, 0, 30);
        } else {
            lv_obj_align(s_state.home_date, LV_ALIGN_TOP_MID, 0, 82);
        }
    }

    s_state.home_weekday = ui_make_label(page, "", ui_base_font(),
                                         lv_color_hex(UI_C_AMBER));
    if (s_state.home_weekday) {
        lv_obj_set_width(s_state.home_weekday, arc_size - 16);
        lv_obj_set_style_text_align(s_state.home_weekday,
                                    LV_TEXT_ALIGN_CENTER, 0);
        if (s_state.home_arc) {
            lv_obj_align_to(s_state.home_weekday, s_state.home_arc,
                            LV_ALIGN_CENTER, 0, 50);
        } else {
            lv_obj_align(s_state.home_weekday, LV_ALIGN_TOP_MID, 0, 106);
        }
    }

    card_w = (s_state.safe_w - gap) / 2;
    cards_y = arc_size + 12;

    ui_make_stat_card(page, 0, cards_y, card_w, card_h, "内存",
                      UI_FONT_VALUE, lv_color_hex(UI_C_AMBER),
                      &s_state.home_ram);
    ui_make_stat_card(page, card_w + gap, cards_y, card_w, card_h, "任务",
                      UI_FONT_VALUE, lv_color_hex(UI_C_TEAL),
                      &s_state.home_task);
    ui_make_stat_card(page, 0, cards_y + card_h + gap, card_w, card_h,
                      "网络", ui_base_font(), lv_color_hex(UI_C_TEAL),
                      &s_state.home_net);
    ui_make_stat_card(page, card_w + gap, cards_y + card_h + gap,
                      card_w, card_h, "定时", UI_FONT_VALUE,
                      lv_color_hex(UI_C_TEXT), &s_state.home_cron);

    latest_y = cards_y + 2 * card_h + gap + 4;
    s_state.home_latest = ui_make_label(page, "最新：暂无消息", ui_base_font(),
                                        lv_color_hex(UI_C_DIM));
    if (s_state.home_latest) {
        lv_obj_set_pos(s_state.home_latest, 8, latest_y);
        lv_obj_set_width(s_state.home_latest, s_state.safe_w - 16);
        lv_label_set_long_mode(s_state.home_latest, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_state.home_latest,
                                    LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void create_chat_page(lv_obj_t* page)
{
    lv_obj_t* chips;
    static const char* chip_texts[] = {"时间", "内存", "任务", "巡检"};
    static const char* chip_cmds[] = {
        "现在几点了",
        "看一下设备当前的内存和存储使用情况",
        "看看设备上正在运行哪些任务",
        "立即巡检设备，采集内存、任务数和存储数据",
    };
    int i;

    ui_make_page_opaque(page);

    chips = lv_obj_create(page);
    if (chips) {
        lv_obj_set_size(chips, s_state.safe_w, 34);
        lv_obj_align(chips, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(chips, 0, 0);
        lv_obj_set_style_pad_all(chips, 0, 0);
        lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(chips, 6, 0);

        for (i = 0; i < 4; i++) {
            lv_obj_t* chip = ui_make_button(chips, chip_texts[i], 64, 30,
                                            quick_cmd_cb,
                                            (void*)chip_cmds[i]);
            if (chip) {
                lv_obj_t* lbl = lv_obj_get_child(chip, 0);
                lv_obj_set_style_radius(chip, 15, 0);
                if (lbl) {
                    lv_obj_set_style_text_color(lbl,
                        lv_color_hex(UI_C_DIM), 0);
                }
            }
        }
    }

    s_state.chat_list = lv_obj_create(page);
    if (s_state.chat_list) {
        lv_obj_set_size(s_state.chat_list, s_state.safe_w,
                        s_state.content_h - 40 - LVGL_UI_PTT_SIZE - 8);
        lv_obj_align(s_state.chat_list, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_set_scroll_dir(s_state.chat_list, LV_DIR_VER);
        lv_obj_set_flex_flow(s_state.chat_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_state.chat_list, LVGL_UI_CHAT_GAP, 0);
        lv_obj_set_style_bg_opa(s_state.chat_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_state.chat_list, 0, 0);
        lv_obj_set_style_pad_left(s_state.chat_list, 4, 0);
        lv_obj_set_style_pad_right(s_state.chat_list, 4, 0);
        lv_obj_set_style_pad_top(s_state.chat_list, 4, 0);
        lv_obj_set_style_pad_bottom(s_state.chat_list, 4, 0);
        lv_obj_set_scrollbar_mode(s_state.chat_list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(s_state.chat_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    }

    s_state.ptt_btn = lv_btn_create(page);
    if (s_state.ptt_btn) {
        lv_obj_set_size(s_state.ptt_btn, LVGL_UI_PTT_SIZE,
                        LVGL_UI_PTT_SIZE);
        lv_obj_align(s_state.ptt_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_radius(s_state.ptt_btn, LVGL_UI_PTT_SIZE / 2, 0);
        lv_obj_set_style_bg_color(s_state.ptt_btn,
                                  lv_color_hex(UI_C_AMBER), 0);
        lv_obj_set_style_shadow_width(s_state.ptt_btn, 12, 0);
        lv_obj_set_style_shadow_color(s_state.ptt_btn,
                                      lv_color_hex(UI_C_AMBER), 0);
        lv_obj_set_style_shadow_opa(s_state.ptt_btn, LV_OPA_50, 0);

        s_state.ptt_label = lv_label_create(s_state.ptt_btn);
        if (s_state.ptt_label) {
            lv_label_set_text(s_state.ptt_label, PTT_ICON_MIC);
            lv_obj_set_style_text_font(s_state.ptt_label, ui_base_font(), 0);
            lv_obj_set_style_text_color(s_state.ptt_label,
                                        lv_color_hex(UI_C_DARKTEXT), 0);
            lv_obj_center(s_state.ptt_label);
        }

        lv_obj_add_event_cb(s_state.ptt_btn, ptt_btn_event_cb,
                            LV_EVENT_ALL, NULL);
    }

    s_state.rec_indicator = lv_label_create(page);
    if (s_state.rec_indicator) {
        lv_label_set_text(s_state.rec_indicator, "");
        lv_obj_set_style_text_font(s_state.rec_indicator, ui_base_font(), 0);
        if (s_state.ptt_btn) {
            lv_obj_align_to(s_state.rec_indicator, s_state.ptt_btn,
                            LV_ALIGN_OUT_TOP_MID, 0, -6);
        }
        lv_obj_add_flag(s_state.rec_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_status_page(lv_obj_t* page)
{
    lv_obj_t* title;
    lv_obj_t* arc;
    lv_obj_t* lbl;
    lv_obj_t* btn;
    int gap = 8;
    int card_w;
    int arc_size = 88;
    int y1 = 174;
    int y2 = 230;

    ui_make_page_opaque(page);

    title = ui_make_label(page, "设备状态", ui_base_font(),
                          lv_color_hex(UI_C_DIM));
    if (title) {
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    }

    /* Health banner: status on the first line, the top issue or the live
     * numbers on the second.  It is the only place on the device that says
     * "degraded" out loud.
     */
    s_state.status_health_banner = lv_obj_create(page);
    if (s_state.status_health_banner) {
        lv_obj_t* banner = s_state.status_health_banner;

        /* Two lines of the 18 px CJK font are 23 px each.  Keep enough
         * room for both so the second line does not overlap the first.
         */
        lv_obj_set_size(banner, s_state.safe_w - 8, 56);
        lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 22);
        lv_obj_set_style_bg_color(banner, lv_color_hex(UI_C_SURFACE), 0);
        lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(banner, 10, 0);
        lv_obj_set_style_border_width(banner, 1, 0);
        lv_obj_set_style_border_color(banner,
                                      lv_color_hex(UI_C_DIM), 0);
        lv_obj_set_style_pad_all(banner, 4, 0);
        lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);

        s_state.status_health_label = ui_make_label(banner, "健康 启动中",
            ui_base_font(), lv_color_hex(UI_C_DIM));
        if (s_state.status_health_label) {
            lv_obj_align(s_state.status_health_label,
                         LV_ALIGN_TOP_LEFT, 4, 0);
        }

        /* The normal line below says 内存/任务/连接 and the fault line says
         * 严重/警告: Montserrat has no CJK glyphs, so this label must use
         * the same font as the title above it.
         */
        s_state.status_health_detail = ui_make_label(banner, "--",
            ui_base_font(), lv_color_hex(UI_C_DIM));
        if (s_state.status_health_detail) {
            lv_obj_align(s_state.status_health_detail,
                         LV_ALIGN_BOTTOM_LEFT, 4, 0);
        }
    }

    arc = lv_arc_create(page);
    if (arc) {
        s_state.status_ram_arc = arc;
        lv_obj_set_size(arc, arc_size, arc_size);
        lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 80);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_bg_angles(arc, 135, 405);
        lv_arc_set_value(arc, 0);
        lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc,
                                   lv_color_hex(UI_C_SURFACE2), LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc,
                                   lv_color_hex(UI_C_AMBER),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

        lbl = ui_make_label(page, "内存", ui_base_font(),
                            lv_color_hex(UI_C_DIM));
        if (lbl) {
            lv_obj_align_to(lbl, arc, LV_ALIGN_CENTER, 0, -18);
        }
        s_state.status_ram_label = ui_make_label(page, "--",
            UI_FONT_VALUE, lv_color_hex(UI_C_AMBER));
        if (s_state.status_ram_label) {
            lv_obj_set_width(s_state.status_ram_label, arc_size - 16);
            lv_obj_set_style_text_align(s_state.status_ram_label,
                                        LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align_to(s_state.status_ram_label, arc,
                            LV_ALIGN_CENTER, 0, 10);
        }
    }

    card_w = (s_state.safe_w - gap) / 2;
    ui_make_stat_card(page, 0, y1, card_w, 52, "任务", UI_FONT_VALUE,
                      lv_color_hex(UI_C_TEAL),
                      &s_state.status_task_label);
    ui_make_stat_card(page, card_w + gap, y1, card_w, 52, "网络",
                      ui_base_font(), lv_color_hex(UI_C_TEAL),
                      &s_state.status_net_label);
    ui_make_stat_card(page, 0, y2, card_w, 52, "定时", UI_FONT_VALUE,
                      lv_color_hex(UI_C_TEXT),
                      &s_state.status_cron_label);
    ui_make_stat_card(page, card_w + gap, y2, card_w, 52, "运行",
                      UI_FONT_VALUE, lv_color_hex(UI_C_TEXT),
                      &s_state.status_uptime_label);

    btn = ui_make_button(page, "立即巡检", 160, 44, status_patrol_cb, NULL);
    if (btn) {
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 288);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_C_AMBER), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(UI_C_DARKTEXT), 0);
        }
        s_state.status_patrol_btn = btn;
    }
}

static void mic_gain_cb(lv_event_t* e)
{
    lv_obj_t* slider = lv_event_get_target(e);
    int value = (int)lv_slider_get_value(slider);

    mic_stream_set_volume(value);
    if (s_state.mic_gain_label) {
        lv_label_set_text_fmt(s_state.mic_gain_label, "%d dB", value);
    }
}

static void settings_sync_cb(lv_event_t* e)
{
    (void)e;
    if (s_state.sync_label) {
        lv_label_set_text(s_state.sync_label, "校时中...");
        lv_obj_set_style_text_color(s_state.sync_label,
                                    lv_color_hex(UI_C_AMBER), 0);
    }
    ui_push_command_stay("现在几点了");
}

static void settings_llm_cb(lv_event_t* e)
{
    (void)e;

    bool to_local = !api_llm_mode_is_local();

    if (api_llm_set_mode(to_local) == OK) {
        if (s_state.llm_mode_label) {
            lv_label_set_text(s_state.llm_mode_label,
                              to_local ? "本地 mock" : "云端 LLM");
            lv_obj_set_style_text_color(s_state.llm_mode_label,
                lv_color_hex(to_local ? UI_C_TEAL : UI_C_AMBER), 0);
        }
    }
}

static lv_obj_t* settings_slider(lv_obj_t* card, int min, int max, int value,
                                 lv_event_cb_t cb)
{
    lv_obj_t* slider = lv_slider_create(card);

    if (!slider) {
        return NULL;
    }
    lv_obj_set_size(slider, s_state.safe_w - 36, 8);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_C_SURFACE2), 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_C_AMBER),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_C_AMBER),
                              LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 4, 0);
    lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(slider, 0, 0);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return slider;
}

static void create_settings_page(lv_obj_t* page)
{
    lv_obj_t* title;
    lv_obj_t* card;
    lv_obj_t* lbl;
    lv_obj_t* btn;
    char about[64];
    int y = 28;

    ui_make_page_opaque(page);

    title = ui_make_label(page, "设置", ui_base_font(),
                          lv_color_hex(UI_C_DIM));
    if (title) {
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    }

    card = ui_make_card(page, 0, y, s_state.safe_w, 58);
    if (card) {
        lbl = ui_make_label(card, "麦克风增益", ui_base_font(),
                            lv_color_hex(UI_C_DIM));
        if (lbl) {
            lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
        }
        s_state.mic_gain_label = ui_make_label(card, "18 dB",
            UI_FONT_VALUE, lv_color_hex(UI_C_TEAL));
        if (s_state.mic_gain_label) {
            lv_obj_align(s_state.mic_gain_label, LV_ALIGN_TOP_RIGHT, 0, 0);
        }
        settings_slider(card, 0, 30, 18, mic_gain_cb);
    }
    y += 58 + 8;

    card = ui_make_card(page, 0, y, s_state.safe_w, 64);
    if (card) {
        lbl = ui_make_label(card, "时间校准", ui_base_font(),
                            lv_color_hex(UI_C_DIM));
        if (lbl) {
            lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
        }
        s_state.sync_label = ui_make_label(card, "未校时",
            ui_base_font(), lv_color_hex(UI_C_DIM));
        if (s_state.sync_label) {
            lv_obj_align(s_state.sync_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }
        btn = ui_make_button(card, "立即校时", 96, 32,
                             settings_sync_cb, NULL);
        if (btn) {
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
    y += 64 + 8;

    card = ui_make_card(page, 0, y, s_state.safe_w, 64);
    if (card) {
        bool local = api_llm_mode_is_local();

        lbl = ui_make_label(card, "模型来源", ui_base_font(),
                            lv_color_hex(UI_C_DIM));
        if (lbl) {
            lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
        }
        s_state.llm_mode_label = ui_make_label(card,
            local ? "本地 mock" : "云端 LLM",
            ui_base_font(),
            lv_color_hex(local ? UI_C_TEAL : UI_C_AMBER));
        if (s_state.llm_mode_label) {
            lv_obj_align(s_state.llm_mode_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }
        btn = ui_make_button(card, "切换", 80, 32, settings_llm_cb, NULL);
        if (btn) {
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
    y += 64 + 8;

    lbl = ui_make_label(page, "关于", ui_base_font(),
                        lv_color_hex(UI_C_DIM));
    if (lbl) {
        lv_obj_set_pos(lbl, 8, y);
    }
    y += 24;

    snprintf(about, sizeof(about), "固件 %s %s", __DATE__, __TIME__);
    lbl = ui_make_label(page, about, ui_base_font(), lv_color_hex(UI_C_DIM));
    if (lbl) {
        lv_obj_set_width(lbl, s_state.safe_w - 16);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(lbl, 8, y);
    }
    y += 24;

    snprintf(about, sizeof(about), "屏幕 %dx%d", s_state.disp_w,
             s_state.disp_h);
    lbl = ui_make_label(page, about, ui_base_font(), lv_color_hex(UI_C_DIM));
    if (lbl) {
        lv_obj_set_pos(lbl, 8, y);
    }
    y += 24;

    lbl = ui_make_label(page, "界面 Midnight Amber", ui_base_font(),
                        lv_color_hex(UI_C_DIM));
    if (lbl) {
        lv_obj_set_pos(lbl, 8, y);
    }
    y += 24;

    lbl = ui_make_label(page, "ASR FireRedASR2-AED (PC)", ui_base_font(),
                        lv_color_hex(UI_C_DIM));
    if (lbl) {
        lv_obj_set_pos(lbl, 8, y);
    }
}

static void dock_btn_cb(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    ui_show_page(idx);
}

static void tileview_event_cb(lv_event_t* e)
{
    lv_obj_t* active;

    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    active = lv_tileview_get_tile_active(s_state.tileview);
    if (active == s_state.chat_page) {
        ui_update_dock(1);
    } else if (active == s_state.status_page) {
        ui_update_dock(2);
    } else if (active == s_state.settings_page) {
        ui_update_dock(3);
    } else {
        ui_update_dock(0);
    }
}

static void ui_update_dock(int idx)
{
    int i;

    for (i = 0; i < 4; i++) {
        lv_obj_t* btn = s_state.dock_btn[i];
        lv_obj_t* lbl;

        if (!btn) {
            continue;
        }
        lv_obj_set_style_bg_opa(btn,
            i == idx ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_C_AMBER), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lbl = lv_obj_get_child(btn, 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(i == idx ? UI_C_DARKTEXT : UI_C_DIM), 0);
        }
    }
}

static void ui_show_page(int idx)
{
    if (!s_state.tileview) {
        return;
    }
    if (idx < 0 || idx > 3) {
        idx = 0;
    }
    lv_tileview_set_tile_by_index(s_state.tileview, (uint32_t)idx, 0,
                                  LV_ANIM_ON);
    ui_update_dock(idx);
}

static void create_dock(lv_obj_t* parent)
{
    static const char* labels[] = {"主页", "对话", "状态", "设置"};
    lv_obj_t* dock;
    lv_obj_t* btn;
    lv_coord_t bw;
    int i;

    dock = lv_obj_create(parent);
    if (!dock) {
        return;
    }

    s_state.dock_w = s_state.safe_w < 240 ? s_state.safe_w : 240;
    lv_obj_set_size(dock, s_state.dock_w, UI_DOCK_H);
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, UI_DOCK_Y);
    lv_obj_set_style_bg_color(dock, lv_color_hex(UI_C_SURFACE), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dock, 24, 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(UI_C_BORDER), 0);
    lv_obj_set_style_pad_all(dock, 4, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    bw = (s_state.dock_w - 8) / 4;

    for (i = 0; i < 4; i++) {
        btn = ui_make_button(dock, labels[i], bw, 40, dock_btn_cb,
                             (void*)(intptr_t)i);
        if (btn) {
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
        }
        s_state.dock_btn[i] = btn;
    }
    ui_update_dock(0);
}

/* ── Init ──────────────────────────────────────────────────── */

int lvgl_ui_channel_init(void)
{
    lv_display_t* disp;
    lv_obj_t* home;
    lv_obj_t* chat;
    lv_obj_t* status;
    lv_obj_t* settings;
    int min_dim;
    int margin;
    int ret = 0;

    if (s_state.initialized) {
        return 0;
    }

    syslog(LOG_INFO, "[%s] init\n", TAG);

    if (!lv_is_initialized()) {
        lv_nuttx_dsc_t dsc;
        lv_nuttx_result_t res;

        if (!ui_wait_for_device(LVGL_UI_FB_PATH, 8000)) {
            syslog(LOG_ERR, "[%s] %s not ready after 8s\n",
                   TAG, LVGL_UI_FB_PATH);
            ret = -EIO;
            goto cleanup;
        }
        ui_wait_for_device(LVGL_UI_INPUT_PATH, 2000);

        lv_init();
        lv_nuttx_dsc_init(&dsc);
        dsc.fb_path = LVGL_UI_FB_PATH;
        dsc.input_path = LVGL_UI_INPUT_PATH;
        lv_nuttx_init(&dsc, &res);

        if (res.disp == NULL) {
            syslog(LOG_ERR, "[%s] lv_nuttx_init found no display (%s)\n",
                   TAG, LVGL_UI_FB_PATH);
            ret = -EIO;
            goto cleanup;
        }

        s_state.lvgl_owned = true;
        syslog(LOG_INFO, "[%s] LVGL brought up standalone: fb=%s input=%s\n",
               TAG, LVGL_UI_FB_PATH, LVGL_UI_INPUT_PATH);
    }

    disp = lv_display_get_default();
    s_state.disp_w = disp ? lv_display_get_horizontal_resolution(disp) : 466;
    s_state.disp_h = disp ? lv_display_get_vertical_resolution(disp) : 466;
    syslog(LOG_INFO, "[%s] display %dx%d\n", TAG,
           s_state.disp_w, s_state.disp_h);

    if (s_state.lvgl_owned && disp) {
        lv_display_set_theme(disp,
            lv_theme_default_init(disp,
                lv_color_hex(UI_C_AMBER),
                lv_color_hex(UI_C_TEAL), true,
                ui_base_font()));
    }

    min_dim = LV_MIN(s_state.disp_w, s_state.disp_h);
    margin = min_dim >= 440 ? 50 : 30;
    s_state.safe_w = min_dim - 2 * margin;
    s_state.content_y = UI_CONTENT_Y;
    s_state.content_h = s_state.disp_h - s_state.content_y - UI_DOCK_H - 14;

    pthread_mutex_init(&s_state.lock, NULL);
    pthread_mutex_init(&s_state.status_lock, NULL);
    memset(&s_state.history, 0, sizeof(s_state.history));
    memset(&s_state.status_snap, 0, sizeof(s_state.status_snap));

    s_state.screen = lv_obj_create(NULL);
    if (!s_state.screen) {
        syslog(LOG_ERR, "[%s] screen create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }

    lv_obj_set_style_bg_color(s_state.screen, lv_color_hex(UI_C_BG), 0);
    lv_obj_set_style_bg_grad_color(s_state.screen, lv_color_hex(UI_C_BG2), 0);
    lv_obj_set_style_bg_grad_dir(s_state.screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_state.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_state.screen, ui_base_font(), 0);

    create_status_bar(s_state.screen);

    s_state.tileview = lv_tileview_create(s_state.screen);
    if (!s_state.tileview) {
        syslog(LOG_ERR, "[%s] tileview create failed\n", TAG);
        ret = -EIO;
        goto cleanup;
    }
    lv_obj_set_size(s_state.tileview, s_state.safe_w, s_state.content_h);
    lv_obj_align(s_state.tileview, LV_ALIGN_TOP_MID, 0, s_state.content_y);
    lv_obj_set_style_bg_opa(s_state.tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_state.tileview, 0, 0);
    lv_obj_set_style_pad_all(s_state.tileview, 0, 0);
    lv_obj_set_scrollbar_mode(s_state.tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_state.tileview, tileview_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    home = lv_tileview_add_tile(s_state.tileview, 0, 0, LV_DIR_HOR);
    chat = lv_tileview_add_tile(s_state.tileview, 1, 0, LV_DIR_HOR);
    status = lv_tileview_add_tile(s_state.tileview, 2, 0, LV_DIR_HOR);
    settings = lv_tileview_add_tile(s_state.tileview, 3, 0, LV_DIR_HOR);

    s_state.home_page = home;
    s_state.chat_page = chat;
    s_state.status_page = status;
    s_state.settings_page = settings;

    if (home) {
        create_home_page(home);
    }
    if (chat) {
        create_chat_page(chat);
    }
    if (status) {
        create_status_page(status);
    }
    if (settings) {
        create_settings_page(settings);
    }

    create_dock(s_state.screen);

    s_state.clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    s_state.status_timer = lv_timer_create(status_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
    status_timer_cb(NULL);

    s_state.initialized = true;
    syslog(LOG_INFO, "[%s] init OK: safe_w=%d content_h=%d\n",
           TAG, s_state.safe_w, s_state.content_h);
    return 0;

cleanup:
#if LVGL_UI_HAVE_CJK_FONT
    if (s_state.font) {
        vg_font_destroy(s_state.font);
        s_state.font = NULL;
    }
#endif

    if (s_state.screen) {
        lv_obj_del(s_state.screen);
        s_state.screen = NULL;
    }
    s_state.initialized = false;
    syslog(LOG_ERR, "[%s] init failed (rc=%d)\n", TAG, ret);
    return ret;
}

/* ── Event loop / lifecycle ────────────────────────────────── */

static void* lvgl_event_loop(void* arg)
{
    (void)arg;

    pthread_setname_np(pthread_self(), "lvgl");

    while (s_state.initialized) {
        uint32_t idle;

        health_monitor_beat(HEALTH_BEAT_LVGL);
        ui_drain();
        idle = lv_timer_handler();
        if (idle > 50) {
            idle = 50;
        }
        usleep((idle ? idle : 5) * 1000);
    }

    return NULL;
}

int lvgl_ui_channel_start(void)
{
    if (!s_state.initialized) {
        syslog(LOG_ERR, "[%s] start: not initialized\n", TAG);
        return -EINVAL;
    }

    if (s_state.running) {
        return 0;
    }

    s_state.running = true;

    if (s_state.lvgl_owned) {
        pthread_attr_t attr;

        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, AGENT_LVGL_UI_STACK);
        int rc = pthread_create(&s_state.lvgl_thread, &attr, lvgl_event_loop,
                                NULL);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            syslog(LOG_ERR, "[%s] failed to start LVGL event loop\n", TAG);
            s_state.running = false;
            return -EIO;
        }
        syslog(LOG_INFO, "[%s] LVGL event loop thread started\n", TAG);
    }

    ui_start_sampler();
    lvgl_ui_channel_show();
    syslog(LOG_INFO, "[%s] started\n", TAG);
    return 0;
}

static sem_t s_stop_sem;

static void lvgl_ui_do_cleanup(void)
{
    int i;

    if (s_state.clock_timer) {
        lv_timer_del(s_state.clock_timer);
        s_state.clock_timer = NULL;
    }
    if (s_state.status_timer) {
        lv_timer_del(s_state.status_timer);
        s_state.status_timer = NULL;
    }

    ui_stop_sampler();

    if (s_state.screen) {
        if (s_state.screen_visible && s_state.prev_screen) {
            lv_screen_load(s_state.prev_screen);
        }
        lv_obj_del(s_state.screen);
        s_state.screen = NULL;
    }

    s_state.screen_visible = false;
    s_state.prev_screen = NULL;

#if LVGL_UI_HAVE_CJK_FONT
    if (s_state.font) {
        vg_font_destroy(s_state.font);
        s_state.font = NULL;
    }
#endif

    s_state.status_bar = NULL;
    s_state.status_time = NULL;
    s_state.status_net = NULL;
    s_state.status_health_dot = NULL;
    s_state.status_health_banner = NULL;
    s_state.status_health_label = NULL;
    s_state.status_health_detail = NULL;
    s_state.tileview = NULL;
    s_state.home_page = NULL;
    s_state.chat_page = NULL;
    s_state.status_page = NULL;
    s_state.home_arc = NULL;
    s_state.home_clock = NULL;
    s_state.home_date = NULL;
    s_state.home_weekday = NULL;
    s_state.home_ram = NULL;
    s_state.home_task = NULL;
    s_state.home_net = NULL;
    s_state.home_cron = NULL;
    s_state.home_latest = NULL;
    s_state.home_chat_btn = NULL;
    s_state.home_patrol_btn = NULL;
    s_state.chat_list = NULL;
    s_state.ptt_btn = NULL;
    s_state.ptt_label = NULL;
    s_state.rec_indicator = NULL;
    s_state.status_ram_arc = NULL;
    s_state.status_ram_label = NULL;
    s_state.status_ram_bar = NULL;
    s_state.status_task_label = NULL;
    s_state.status_net_label = NULL;
    s_state.status_cron_label = NULL;
    s_state.status_uptime_label = NULL;
    s_state.status_patrol_btn = NULL;
    s_state.settings_page = NULL;
    s_state.mic_gain_label = NULL;
    s_state.sync_label = NULL;
    for (i = 0; i < 4; i++) {
        s_state.dock_btn[i] = NULL;
    }

    for (i = 0; i < CHAT_HISTORY_MAX; i++) {
        s_state.history.msgs[i].bubble = NULL;
    }
    s_state.history.head = 0;
    s_state.history.count = 0;
}

static void stop_screen_async_cb(void* data)
{
    (void)data;

    lvgl_ui_do_cleanup();
    sem_post(&s_stop_sem);
}

static void lvgl_ui_join_event_loop(void)
{
    if (!s_state.lvgl_owned) {
        return;
    }

    if (pthread_equal(pthread_self(), s_state.lvgl_thread)) {
        s_state.lvgl_owned = false;
        return;
    }

    pthread_join(s_state.lvgl_thread, NULL);
    s_state.lvgl_owned = false;
}

void lvgl_ui_channel_stop_sync(void)
{
    if (!s_state.initialized || !s_state.running) {
        return;
    }

    syslog(LOG_INFO, "[%s] stopping (sync)\n", TAG);

    s_state.running = false;
    lvgl_ui_do_cleanup();
    pthread_mutex_destroy(&s_state.lock);
    pthread_mutex_destroy(&s_state.status_lock);
    s_state.initialized = false;

    lvgl_ui_join_event_loop();

    syslog(LOG_INFO, "[%s] stopped\n", TAG);
}

void lvgl_ui_channel_stop(void)
{
    if (!s_state.initialized || !s_state.running) {
        return;
    }

    syslog(LOG_INFO, "[%s] stopping\n", TAG);

    s_state.running = false;

    sem_init(&s_stop_sem, 0, 0);
    lv_async_call(stop_screen_async_cb, NULL);

    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;

    if (sem_timedwait(&s_stop_sem, &ts) != 0) {
        syslog(LOG_WARNING,
            "[%s] async stop timed out, forcing cleanup\n", TAG);
        ui_stop_sampler();
        s_state.screen = NULL;
        s_state.chat_list = NULL;
        s_state.ptt_btn = NULL;
        s_state.ptt_label = NULL;
        s_state.rec_indicator = NULL;
    }

    sem_destroy(&s_stop_sem);
    pthread_mutex_destroy(&s_state.lock);
    pthread_mutex_destroy(&s_state.status_lock);
    s_state.initialized = false;
    lvgl_ui_join_event_loop();

    syslog(LOG_INFO, "[%s] stopped\n", TAG);
}

static void show_screen_async_cb(void* data)
{
    (void)data;
    if (!s_state.initialized || !s_state.running || !s_state.screen) {
        return;
    }
    if (s_state.screen_visible) {
        return;
    }

#if LVGL_UI_HAVE_CJK_FONT
    if (!s_state.font) {
        s_state.font = vg_font_create("MiSans-Medium", 18,
            LV_FREETYPE_FONT_STYLE_NORMAL);
        if (s_state.font) {
            lv_obj_set_style_text_font(s_state.screen, s_state.font, 0);
        } else {
            syslog(LOG_WARNING, "[%s] vg_font_create failed\n", TAG);
        }
    }
#endif

    syslog(LOG_INFO, "[%s] loading UI screen\n", TAG);
    s_state.prev_screen = lv_screen_active();
    lv_screen_load(s_state.screen);
    s_state.screen_visible = true;
}

void lvgl_ui_channel_show(void)
{
    if (!s_state.initialized || !s_state.running) {
        syslog(LOG_ERR, "[%s] show: channel not ready\n", TAG);
        return;
    }

    if (s_state.screen_visible) {
        return;
    }

    pthread_mutex_lock(&s_queue_mtx);
    s_show_requested = true;
    pthread_mutex_unlock(&s_queue_mtx);
}

/* ── Queue / messages ──────────────────────────────────────── */

static int ui_post(const char* text, bool is_user, bool speak)
{
    async_msg_t* payload;
    int slot;

    if (!text || text[0] == '\0') {
        return -EINVAL;
    }
    if (!s_state.initialized || !s_state.running) {
        return -EINVAL;
    }

    payload = malloc(sizeof(async_msg_t));
    if (!payload) {
        syslog(LOG_ERR, "[%s] queue payload alloc failed\n", TAG);
        return -ENOMEM;
    }

    strncpy(payload->text, text, LVGL_UI_MSG_MAX_LEN - 1);
    payload->text[LVGL_UI_MSG_MAX_LEN - 1] = '\0';
    payload->is_user = is_user;

    pthread_mutex_lock(&s_queue_mtx);
    if (s_queue_count == UI_QUEUE_MAX) {
        free(s_queue[s_queue_head]);
        s_queue[s_queue_head] = NULL;
        s_queue_head = (s_queue_head + 1) % UI_QUEUE_MAX;
        s_queue_count--;
    }

    slot = (s_queue_head + s_queue_count) % UI_QUEUE_MAX;
    s_queue[slot] = payload;
    s_queue_count++;
    s_show_requested = true;
    pthread_mutex_unlock(&s_queue_mtx);

    if (speak) {
        int ret = voice_channel_speak(text);

        if (ret != 0) {
            syslog(LOG_ERR, "[%s] voice_channel_speak failed (rc=%d)\n",
                   TAG, ret);
        }
    }

    return 0;
}

static void ui_drain(void)
{
    async_msg_t* batch[UI_QUEUE_MAX];
    bool show;
    bool stale = false;
    int n = 0;
    int i;

    pthread_mutex_lock(&s_state.lock);
    if (s_state.is_processing && s_state.processing_started_ms != 0 &&
        (uint32_t)(lv_tick_get() - s_state.processing_started_ms) >=
            LVGL_UI_PROCESSING_TIMEOUT_MS) {
        s_state.is_processing = false;
        s_state.processing_started_ms = 0;
        stale = true;
    }
    pthread_mutex_unlock(&s_state.lock);

    if (stale) {
        syslog(LOG_WARNING, "[%s] PTT processing timed out\n", TAG);
        recording_indicator_stop();
    }

    pthread_mutex_lock(&s_queue_mtx);
    show = s_show_requested;
    s_show_requested = false;
    while (s_queue_count > 0 && n < UI_QUEUE_MAX) {
        batch[n++] = s_queue[s_queue_head];
        s_queue[s_queue_head] = NULL;
        s_queue_head = (s_queue_head + 1) % UI_QUEUE_MAX;
        s_queue_count--;
    }
    pthread_mutex_unlock(&s_queue_mtx);

    if (show && !s_state.screen_visible) {
        show_screen_async_cb(NULL);
    }

    for (i = 0; i < n; i++) {
        chat_view_add_message(batch[i]->text, batch[i]->is_user);
        free(batch[i]);
        batch[i] = NULL;
    }

    if (n > 0 && s_state.is_processing) {
        s_state.is_processing = false;
        s_state.processing_started_ms = 0;
        recording_indicator_stop();
    }
}

int lvgl_ui_channel_send(const char* text)
{
    return ui_post(text, false, true);
}

void lvgl_ui_channel_send_user(const char* text)
{
    ui_post(text, true, false);
}

int lvgl_ui_channel_post(const char* text)
{
    return ui_post(text, false, false);
}

/* ── Chat view ─────────────────────────────────────────────── */

static void chat_view_add_message(const char* text, bool is_user)
{
    chat_history_t* h = &s_state.history;
    chat_msg_t* slot;
    lv_obj_t* bubble;
    lv_obj_t* lbl;
    lv_obj_t* ts;

    if (!s_state.chat_list) {
        return;
    }

    slot = chat_history_add(h, text, is_user);

    bubble = lv_obj_create(s_state.chat_list);
    if (!bubble) {
        syslog(LOG_ERR, "[%s] bubble create failed\n", TAG);
        return;
    }

    lv_obj_set_width(bubble, lv_pct(85));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_all(bubble, LVGL_UI_BUBBLE_PAD, 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(UI_C_BORDER), 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bubble, 2, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (is_user) {
        lv_obj_set_style_align(bubble, LV_ALIGN_RIGHT_MID, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(UI_C_AMBER), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_align(bubble, LV_ALIGN_LEFT_MID, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(UI_C_SURFACE), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    }

    lbl = lv_label_create(bubble);
    if (!lbl) {
        syslog(LOG_ERR, "[%s] bubble label create failed\n", TAG);
        lv_obj_del(bubble);
        slot->bubble = NULL;
        return;
    }

    lv_label_set_text(lbl, slot->text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_style_text_font(lbl, ui_base_font(), 0);
    lv_obj_set_style_text_color(lbl,
        lv_color_hex(is_user ? UI_C_DARKTEXT : UI_C_TEXT), 0);

    ts = lv_label_create(bubble);
    if (ts) {
        lv_label_set_text(ts, slot->when);
        lv_obj_set_style_text_font(ts, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ts,
            lv_color_hex(is_user ? 0x5A4210 : UI_C_DIM), 0);
        lv_obj_set_width(ts, lv_pct(100));
        lv_obj_set_style_text_align(ts,
            is_user ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    }

    slot->bubble = bubble;

    if (!is_user && s_state.home_latest) {
        lv_label_set_text(s_state.home_latest, slot->text);
    }

    chat_view_scroll_to_bottom();
}

static void chat_view_scroll_to_bottom(void)
{
    if (s_state.chat_list) {
        lv_obj_scroll_to_y(s_state.chat_list, LV_COORD_MAX, LV_ANIM_ON);
    }
}

static void ui_push_command_stay(const char* text)
{
    agent_msg_t msg;

    if (!text || !text[0]) {
        return;
    }

    /* The agent loop mirrors every non-internal inbound message to the UI
     * as a user bubble (agent_loop.c); do not add another one here. */
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, AGENT_CHAN_LVGL_UI, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "ui", sizeof(msg.chat_id) - 1);
    msg.content = strdup(text);
    if (msg.content) {
        if (message_bus_push_inbound(&msg) != OK) {
            free(msg.content);
        }
    }
}

static void ui_push_command(const char* text)
{
    if (!text || !text[0]) {
        return;
    }
    ui_show_page(1);
    ui_push_command_stay(text);
}

/* ── PTT ───────────────────────────────────────────────────── */

static void ptt_btn_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    pthread_mutex_lock(&s_state.lock);
    if (s_state.is_processing) {
        pthread_mutex_unlock(&s_state.lock);
        return;
    }

    if (!s_state.is_recording) {
        s_state.is_recording = true;
        pthread_mutex_unlock(&s_state.lock);

        ws_server_broadcast_json("{\"type\":\"ptt\",\"state\":\"start\"}");
        recording_indicator_start();
    } else {
        s_state.is_recording = false;
        s_state.is_processing = true;
        s_state.processing_started_ms = lv_tick_get();
        pthread_mutex_unlock(&s_state.lock);

        ws_server_broadcast_json("{\"type\":\"ptt\",\"state\":\"stop\"}");
        recording_indicator_show_processing();
    }
}

static void anim_opa_cb(void* obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

static void recording_indicator_start(void)
{
    if (!s_state.ptt_btn || !s_state.rec_indicator) {
        return;
    }

    lv_obj_set_style_bg_color(s_state.ptt_btn, lv_color_hex(UI_C_RED), 0);
    lv_obj_set_style_transform_scale(s_state.ptt_btn, 280, 0);
    if (s_state.ptt_label) {
        lv_obj_set_style_text_color(s_state.ptt_label,
                                    lv_color_hex(UI_C_TEXT), 0);
    }

    lv_label_set_text(s_state.rec_indicator, "录音中...");
    lv_obj_set_style_text_color(s_state.rec_indicator,
                                lv_color_hex(UI_C_RED), 0);
    lv_obj_clear_flag(s_state.rec_indicator, LV_OBJ_FLAG_HIDDEN);

    lv_anim_init(&s_state.rec_anim);
    lv_anim_set_var(&s_state.rec_anim, s_state.ptt_btn);
    lv_anim_set_values(&s_state.rec_anim, LV_OPA_COVER, LV_OPA_70);
    lv_anim_set_time(&s_state.rec_anim, 600);
    lv_anim_set_repeat_count(&s_state.rec_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&s_state.rec_anim, 600);
    lv_anim_set_exec_cb(&s_state.rec_anim, anim_opa_cb);
    lv_anim_start(&s_state.rec_anim);
}

static void recording_indicator_stop(void)
{
    if (!s_state.ptt_btn || !s_state.rec_indicator) {
        return;
    }

    lv_obj_set_style_bg_color(s_state.ptt_btn, lv_color_hex(UI_C_AMBER), 0);
    lv_obj_set_style_transform_scale(s_state.ptt_btn, 256, 0);
    if (s_state.ptt_label) {
        lv_obj_set_style_text_color(s_state.ptt_label,
                                    lv_color_hex(UI_C_DARKTEXT), 0);
    }
    lv_obj_set_style_opa(s_state.ptt_btn, LV_OPA_COVER, 0);
    lv_anim_del(s_state.ptt_btn, anim_opa_cb);
    lv_obj_add_flag(s_state.rec_indicator, LV_OBJ_FLAG_HIDDEN);
}

static void recording_indicator_show_processing(void)
{
    if (!s_state.ptt_btn || !s_state.rec_indicator) {
        return;
    }

    lv_anim_del(s_state.ptt_btn, anim_opa_cb);
    lv_obj_set_style_opa(s_state.ptt_btn, LV_OPA_COVER, 0);
    lv_label_set_text(s_state.rec_indicator, "识别中...");
    lv_obj_set_style_text_color(s_state.rec_indicator,
                                lv_color_hex(UI_C_TEAL), 0);
}
