/**
 * @file ducky_custom_ui.c
 * @brief DuckyClaw desktop-widget UI.
 *
 * Layout (320x480):
 *   [top_bar  24px] mode | AI status | WiFi icon  (white text on dark bar)
 *   [time_panel 80px] big clock + date + lunar hint
 *   [info_panel ~256px] 3 cards: 星座运势 / 黄历宜忌 / 天气
 *   [ai_panel  120px] latest AI message
 *
 * All panels are fully opaque (LV_OPA_COVER) to avoid LCD flicker on T5AI.
 */

#include "tal_api.h"

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
#if defined(ENABLE_AI_CHAT_CUSTOM_UI) && (ENABLE_AI_CHAT_CUSTOM_UI == 1)
#include "lvgl.h"
#include "lv_vendor.h"
#include "tal_time_service.h"

#include "ai_ui_manage.h"
#include "ai_ui_icon_font.h"
#include "font_awesome_symbols.h"
#include "cron_service.h"

#include "ducky_custom_ui.h"

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
#include "tkl_jpeg_codec.h"
#include "tool_files.h"

/* SD card: save/restore JPEG file directly.
 * SPIFFS: URL is persisted to KV by tool_wallpaper.c;
 *         restore is triggered after MQTT connects (tool_wallpaper_restore). */
#if defined(CLAW_USE_SDCARD) && (CLAW_USE_SDCARD == 1)
#define WALLPAPER_FILE             CLAW_FS_ROOT_PATH "/config/wallpaper.jpg"
#define WALLPAPER_MAX_RESTORE_SIZE (256 * 1024)
#endif
#endif

/* ── Dimensions for Glass Layout ── */
#define DUCKY_MARGIN      12
#define DUCKY_TOP_BAR_H   28
#define DUCKY_TIME_H      80
#define DUCKY_AI_H        140
#define DUCKY_INFO_H      172 /* 2 rows of 80px cards + 12px gap */
#define DUCKY_CARD_W      ((LV_HOR_RES - (DUCKY_MARGIN * 2) - 12) / 2)
#define DUCKY_CARD_H      80

/* ── Extended display type IDs ── */
typedef enum {
    DUCKY_UI_DISP_NEWS_TEXT  = AI_UI_DISP_SYS_MAX + 100,
    DUCKY_UI_DISP_IMAGE_DESC,
    DUCKY_UI_DISP_WEATHER,
    DUCKY_UI_DISP_HOROSCOPE,
    DUCKY_UI_DISP_ALMANAC,
    DUCKY_UI_DISP_SYS_STATUS,
} DUCKY_UI_DISP_TYPE_E;

/* ── Widget tree ── */
typedef struct {
    lv_obj_t *screen;

    /* Top status bar */
    lv_obj_t *top_bar;
    lv_obj_t *chat_mode_label;
    lv_obj_t *notification_label;
    lv_obj_t *status_label;
    lv_obj_t *network_label;

    /* Time panel */
    lv_obj_t *time_panel;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *lunar_label;

    /* Info cards */
    lv_obj_t *info_panel;
    lv_obj_t *horoscope_body;
    lv_obj_t *almanac_body;
    lv_obj_t *weather_body;
    lv_obj_t *sys_body;

    /* AI panel */
    lv_obj_t *ai_panel;
    lv_obj_t *ai_msg_label;
} DUCKY_CUSTOM_UI_T;

static DUCKY_CUSTOM_UI_T sg_ui;
static bool              sg_is_streaming    = false;
static lv_timer_t       *sg_notification_tm = NULL;
static lv_timer_t       *sg_clock_tm        = NULL;
static lv_timer_t       *sg_sys_poll_tm     = NULL;

/* ── Wallpaper state ── */
#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
static lv_obj_t      *sg_wallpaper_img     = NULL;
static uint8_t       *sg_wallpaper_pixels  = NULL;
static lv_image_dsc_t sg_wallpaper_dsc;

/* Load persisted wallpaper from filesystem and apply to screen.
 * Only active on SD card builds — SPIFFS restore is handled by
 * tool_wallpaper_restore() after MQTT connects. */
static void __wallpaper_restore(void)
{
#if defined(CLAW_USE_SDCARD) && (CLAW_USE_SDCARD == 1)
    PR_NOTICE("wallpaper: restore check %s", WALLPAPER_FILE);

    TUYA_FILE f = claw_fopen(WALLPAPER_FILE, "r");
    if (!f) {
        PR_NOTICE("wallpaper: no saved wallpaper file, skip restore");
        return;
    }

    int fsize = claw_fgetsize(WALLPAPER_FILE);
    if (fsize <= 0 || fsize > WALLPAPER_MAX_RESTORE_SIZE) {
        claw_fclose(f);
        PR_WARN("wallpaper: restore skipped, fsize=%d", fsize);
        return;
    }

    uint8_t *buf = (uint8_t *)tal_malloc((uint32_t)fsize);
    if (!buf) {
        claw_fclose(f);
        PR_ERR("wallpaper: restore malloc failed for %d bytes", fsize);
        return;
    }

    int rd = claw_fread(buf, fsize, f);
    claw_fclose(f);

    if (rd != fsize) {
        PR_ERR("wallpaper: restore read %d/%d bytes", rd, fsize);
        tal_free(buf);
        return;
    }

    PR_NOTICE("wallpaper: restoring %d bytes from SD card", fsize);
    OPERATE_RET rt = ducky_custom_ui_set_wallpaper(buf, (uint32_t)fsize);
    if (rt != OPRT_OK) {
        PR_ERR("wallpaper: restore apply failed, rt=%d", rt);
    }
    tal_free(buf);
#else
    PR_NOTICE("wallpaper: SPIFFS build, wallpaper restore via KV after WiFi connects");
#endif
}
#endif

/* ── Helpers ── */

static void __notification_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    if (sg_notification_tm) {
        lv_timer_del(sg_notification_tm);
        sg_notification_tm = NULL;
    }
    lv_obj_add_flag(sg_ui.notification_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(sg_ui.status_label, LV_OBJ_FLAG_HIDDEN);
}

/* Poll cron job count and heartbeat status every 15 s */
static void __sys_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!sg_ui.sys_body) return;
    const cron_job_t *jobs = NULL;
    int count = 0;
    cron_list_jobs(&jobs, &count);
    char buf[64];
    snprintf(buf, sizeof(buf), "心跳:运行中  定时:%d个", count);
    lv_vendor_disp_lock();
    lv_label_set_text(sg_ui.sys_body, buf);
    lv_vendor_disp_unlock();
}

static void __clock_update_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!sg_ui.time_label || !sg_ui.date_label) return;

    TIME_T posix = tal_time_get_posix();
    POSIX_TM_S tm = {0};
    tal_time_get_local_time_custom(posix, &tm);

    static const char *wdays[] = {"日","一","二","三","四","五","六"};
    int wd = (tm.tm_wday >= 0 && tm.tm_wday <= 6) ? tm.tm_wday : 0;

    lv_vendor_disp_lock();
    lv_label_set_text_fmt(sg_ui.time_label, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text_fmt(sg_ui.date_label, "%d年%d月%d日  周%s",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, wdays[wd]);
    lv_vendor_disp_unlock();
}

/* ── Glass Style Helper ── */
static void __apply_glass_style(lv_obj_t *obj, lv_coord_t radius)
{
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_40, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_20, 0);
}

/* Create a rounded info card. Returns card; sets *out_body. */
static lv_obj_t *__make_card(lv_obj_t *parent, int w, int h,
                              lv_obj_t **out_body,
                              const char *icon_sym,
                              lv_color_t icon_color,
                              const char *title_text,
                              const char *body_text)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    __apply_glass_style(card, 16);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(card);
    lv_obj_set_style_text_font(icon, ai_ui_get_icon_font(), 0);
    lv_obj_set_style_text_color(icon, icon_color, 0);
    lv_label_set_text(icon, icon_sym);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(title, LV_OPA_80, 0);
    lv_label_set_text(title, title_text);
    lv_obj_align_to(title, icon, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_obj_set_width(body, w - 20);
    lv_obj_set_style_text_color(body, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(body, LV_OPA_COVER, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body, body_text);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 24);

    if (out_body) *out_body = body;
    return card;
}

/* ── LVGL init ── */
static void __lvgl_init(void)
{
    lv_vendor_init(DISPLAY_NAME);
    lv_vendor_start(5, 1024 * 8);
}

/* ── UI init ── */
static OPERATE_RET __ui_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    __lvgl_init();
    lv_vendor_disp_lock();

    memset(&sg_ui, 0, sizeof(sg_ui));
    sg_ui.screen = lv_screen_active();

    /* Platform text font ensures CJK + icon glyph support on all labels */
    const lv_font_t *text_font = ai_ui_get_text_font();
    if (text_font) {
        lv_obj_set_style_text_font(sg_ui.screen, text_font, 0);
    }

    lv_obj_set_style_bg_color(sg_ui.screen, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(sg_ui.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(sg_ui.screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(sg_ui.screen, 0, 0);

    /* ── TOP STATUS BAR ── */
    sg_ui.top_bar = lv_obj_create(sg_ui.screen);
    lv_obj_set_size(sg_ui.top_bar, LV_HOR_RES - DUCKY_MARGIN * 2, DUCKY_TOP_BAR_H);
    lv_obj_align(sg_ui.top_bar, LV_ALIGN_TOP_MID, 0, DUCKY_MARGIN);
    __apply_glass_style(sg_ui.top_bar, DUCKY_TOP_BAR_H / 2);
    lv_obj_set_style_pad_all(sg_ui.top_bar, 0, 0);
    lv_obj_clear_flag(sg_ui.top_bar, LV_OBJ_FLAG_SCROLLABLE);

    sg_ui.chat_mode_label = lv_label_create(sg_ui.top_bar);
    lv_label_set_text(sg_ui.chat_mode_label, "Chat");
    lv_obj_set_style_text_color(sg_ui.chat_mode_label, lv_color_white(), 0);
    lv_obj_align(sg_ui.chat_mode_label, LV_ALIGN_LEFT_MID, 12, 0);

    sg_ui.notification_label = lv_label_create(sg_ui.top_bar);
    lv_label_set_text(sg_ui.notification_label, "");
    lv_obj_set_width(sg_ui.notification_label, LV_HOR_RES - 120);
    lv_obj_set_style_text_align(sg_ui.notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(sg_ui.notification_label, lv_color_hex(0xFFD60A), 0);
    lv_obj_align(sg_ui.notification_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(sg_ui.notification_label, LV_OBJ_FLAG_HIDDEN);

    sg_ui.status_label = lv_label_create(sg_ui.top_bar);
    lv_obj_set_width(sg_ui.status_label, LV_HOR_RES - 120);
    lv_label_set_long_mode(sg_ui.status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(sg_ui.status_label, "Initializing");
    lv_obj_set_style_text_align(sg_ui.status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(sg_ui.status_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(sg_ui.status_label, LV_OPA_80, 0);
    lv_obj_align(sg_ui.status_label, LV_ALIGN_CENTER, 0, 0);

    /* WiFi icon */
    sg_ui.network_label = lv_label_create(sg_ui.top_bar);
    lv_obj_set_style_text_font(sg_ui.network_label, ai_ui_get_icon_font(), 0);
    lv_obj_set_style_text_color(sg_ui.network_label, lv_color_white(), 0);
    lv_label_set_text(sg_ui.network_label, ai_ui_get_wifi_icon(AI_UI_WIFI_STATUS_DISCONNECTED));
    lv_obj_align(sg_ui.network_label, LV_ALIGN_RIGHT_MID, -12, 0);

    /* ── TIME PANEL ── */
    sg_ui.time_panel = lv_obj_create(sg_ui.screen);
    lv_obj_set_size(sg_ui.time_panel, LV_HOR_RES - DUCKY_MARGIN * 2, DUCKY_TIME_H);
    lv_obj_align(sg_ui.time_panel, LV_ALIGN_TOP_MID, 0, DUCKY_MARGIN + DUCKY_TOP_BAR_H + DUCKY_MARGIN);
    __apply_glass_style(sg_ui.time_panel, 16);
    lv_obj_set_style_pad_all(sg_ui.time_panel, 12, 0);
    lv_obj_clear_flag(sg_ui.time_panel, LV_OBJ_FLAG_SCROLLABLE);

    sg_ui.time_label = lv_label_create(sg_ui.time_panel);
    lv_obj_set_style_text_color(sg_ui.time_label, lv_color_white(), 0);
    lv_label_set_text(sg_ui.time_label, "--:--");
    lv_obj_align(sg_ui.time_label, LV_ALIGN_LEFT_MID, 8, 0);

    sg_ui.date_label = lv_label_create(sg_ui.time_panel);
    lv_obj_set_style_text_color(sg_ui.date_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(sg_ui.date_label, LV_OPA_80, 0);
    lv_label_set_text(sg_ui.date_label, "----年--月--日");
    lv_obj_align(sg_ui.date_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    sg_ui.lunar_label = lv_label_create(sg_ui.time_panel);
    lv_obj_set_width(sg_ui.lunar_label, 150);
    lv_obj_set_style_text_align(sg_ui.lunar_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(sg_ui.lunar_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(sg_ui.lunar_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(sg_ui.lunar_label, LV_OPA_60, 0);
    lv_label_set_text(sg_ui.lunar_label, "农历加载中…");
    lv_obj_align(sg_ui.lunar_label, LV_ALIGN_BOTTOM_RIGHT, -4, -4);

    /* ── INFO CARDS PANEL ── */
    sg_ui.info_panel = lv_obj_create(sg_ui.screen);
    lv_obj_set_size(sg_ui.info_panel, LV_HOR_RES - DUCKY_MARGIN * 2, DUCKY_INFO_H);
    lv_obj_align(sg_ui.info_panel, LV_ALIGN_TOP_MID, 0, DUCKY_MARGIN + DUCKY_TOP_BAR_H + DUCKY_MARGIN + DUCKY_TIME_H + DUCKY_MARGIN);
    lv_obj_set_style_bg_opa(sg_ui.info_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sg_ui.info_panel, 0, 0);
    lv_obj_set_style_pad_all(sg_ui.info_panel, 0, 0);
    lv_obj_set_style_pad_row(sg_ui.info_panel, 12, 0);
    lv_obj_set_style_pad_column(sg_ui.info_panel, 12, 0);
    lv_obj_set_flex_flow(sg_ui.info_panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_clear_flag(sg_ui.info_panel, LV_OBJ_FLAG_SCROLLABLE);

    __make_card(sg_ui.info_panel, DUCKY_CARD_W, DUCKY_CARD_H, &sg_ui.horoscope_body,
                FONT_AWESOME_EMOJI_HAPPY, lv_color_hex(0xFF9F0A), "星座", "运势加载中…");

    __make_card(sg_ui.info_panel, DUCKY_CARD_W, DUCKY_CARD_H, &sg_ui.almanac_body,
                FONT_AWESOME_CHECK, lv_color_hex(0x32D74B), "黄历", "宜忌加载中…");

    __make_card(sg_ui.info_panel, DUCKY_CARD_W, DUCKY_CARD_H, &sg_ui.weather_body,
                FONT_AWESOME_GLOBE, lv_color_hex(0x0A84FF), "天气", "信息加载中…");

    __make_card(sg_ui.info_panel, DUCKY_CARD_W, DUCKY_CARD_H, &sg_ui.sys_body,
                FONT_AWESOME_GEAR, lv_color_hex(0xFF453A), "系统", "状态加载中…");

    /* ── AI MESSAGE PANEL ── */
    sg_ui.ai_panel = lv_obj_create(sg_ui.screen);
    lv_obj_set_size(sg_ui.ai_panel, LV_HOR_RES - DUCKY_MARGIN * 2, DUCKY_AI_H);
    lv_obj_align(sg_ui.ai_panel, LV_ALIGN_BOTTOM_MID, 0, -DUCKY_MARGIN);
    __apply_glass_style(sg_ui.ai_panel, 16);
    lv_obj_set_style_pad_all(sg_ui.ai_panel, 12, 0);
    lv_obj_clear_flag(sg_ui.ai_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ai_icon = lv_label_create(sg_ui.ai_panel);
    lv_obj_set_style_text_font(ai_icon, ai_ui_get_icon_font(), 0);
    lv_obj_set_style_text_color(ai_icon, lv_color_hex(0xAF52DE), 0);
    lv_label_set_text(ai_icon, FONT_AWESOME_AI_CHIP);
    lv_obj_align(ai_icon, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *ai_title = lv_label_create(sg_ui.ai_panel);
    lv_label_set_text(ai_title, "AI Assistant");
    lv_obj_set_style_text_color(ai_title, lv_color_white(), 0);
    lv_obj_set_style_text_opa(ai_title, LV_OPA_80, 0);
    lv_obj_align_to(ai_title, ai_icon, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    sg_ui.ai_msg_label = lv_label_create(sg_ui.ai_panel);
    lv_obj_set_size(sg_ui.ai_msg_label, LV_HOR_RES - DUCKY_MARGIN * 2 - 24, DUCKY_AI_H - 36);
    lv_label_set_long_mode(sg_ui.ai_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(sg_ui.ai_msg_label, lv_color_white(), 0);
    lv_label_set_text(sg_ui.ai_msg_label, "DuckyClaw 已就绪\n有什么可以帮你？");
    lv_obj_align(sg_ui.ai_msg_label, LV_ALIGN_TOP_LEFT, 0, 24);

    lv_vendor_disp_unlock();

    /* Clock timer: fires every 30s, immediately on first run */
    sg_clock_tm = lv_timer_create(__clock_update_cb, 30000, NULL);
    lv_timer_ready(sg_clock_tm);

    /* Sys status poll timer: fires every 15s, immediately on first run */
    sg_sys_poll_tm = lv_timer_create(__sys_poll_cb, 15000, NULL);
    lv_timer_ready(sg_sys_poll_tm);

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
    /* Restore wallpaper saved from previous session */
    __wallpaper_restore();
#endif

    return rt;
}

/* ── Display callbacks ── */

static void __ui_disp_user_msg(char *string)
{
    if (!sg_ui.ai_msg_label || !string) return;
    lv_vendor_disp_lock();
    lv_label_set_text_fmt(sg_ui.ai_msg_label, LV_SYMBOL_RIGHT " %s", string);
    lv_vendor_disp_unlock();
}

static void __ui_disp_ai_msg(char *string)
{
    if (!sg_ui.ai_msg_label || !string) return;
    lv_vendor_disp_lock();
    lv_label_set_text(sg_ui.ai_msg_label, string);
    lv_vendor_disp_unlock();
}

static void __ui_disp_ai_msg_stream_start(void)
{
    if (!sg_ui.ai_msg_label) return;
    sg_is_streaming = true;
    lv_vendor_disp_lock();
    lv_label_set_text(sg_ui.ai_msg_label, "");
    lv_vendor_disp_unlock();
}

static void __ui_disp_ai_msg_stream_data(char *string)
{
    if (!sg_ui.ai_msg_label || !string || !sg_is_streaming) return;
    lv_vendor_disp_lock();
    lv_label_ins_text(sg_ui.ai_msg_label, LV_LABEL_POS_LAST, string);
    lv_vendor_disp_unlock();
}

static void __ui_disp_ai_msg_stream_end(void)
{
    sg_is_streaming = false;
}

static void __ui_disp_ai_mode_state(char *string)
{
    if (!sg_ui.status_label || !string) return;
    lv_vendor_disp_lock();
    lv_obj_add_flag(sg_ui.notification_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(sg_ui.status_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(sg_ui.status_label, string);
    lv_vendor_disp_unlock();
}

static void __ui_disp_wifi_state(AI_UI_WIFI_STATUS_E wifi_status)
{
    char *wifi_icon = ai_ui_get_wifi_icon(wifi_status);
    if (!sg_ui.network_label || !wifi_icon) return;
    lv_vendor_disp_lock();
    lv_label_set_text(sg_ui.network_label, wifi_icon);
    lv_vendor_disp_unlock();
}

static void __ui_disp_other_msg(uint32_t type, uint8_t *data, int len)
{
    char tmp[256] = {0};
    if (!data || len <= 0) return;
    if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
    memcpy(tmp, data, len);
    tmp[len] = '\0';

    lv_vendor_disp_lock();
    switch (type) {
    case DUCKY_UI_DISP_NEWS_TEXT:
        if (sg_ui.ai_msg_label)
            lv_label_set_text_fmt(sg_ui.ai_msg_label, FONT_AWESOME_BELL " %s", tmp);
        break;
    case DUCKY_UI_DISP_IMAGE_DESC:
        if (sg_ui.ai_msg_label)
            lv_label_set_text(sg_ui.ai_msg_label, tmp);
        break;
    case DUCKY_UI_DISP_WEATHER:
        if (sg_ui.weather_body)
            lv_label_set_text(sg_ui.weather_body, tmp);
        break;
    case DUCKY_UI_DISP_HOROSCOPE:
        if (sg_ui.horoscope_body)
            lv_label_set_text(sg_ui.horoscope_body, tmp);
        break;
    case DUCKY_UI_DISP_ALMANAC:
        if (sg_ui.almanac_body)
            lv_label_set_text(sg_ui.almanac_body, tmp);
        if (sg_ui.lunar_label) {
            char hint[32] = {0};
            int hlen = len < 31 ? len : 31;
            memcpy(hint, tmp, hlen);
            lv_label_set_text(sg_ui.lunar_label, hint);
        }
        break;
    case DUCKY_UI_DISP_SYS_STATUS:
        if (sg_ui.sys_body)
            lv_label_set_text(sg_ui.sys_body, tmp);
        break;
    default:
        break;
    }
    lv_vendor_disp_unlock();
}

static void __ui_disp_notification(char *string)
{
    if (!sg_ui.notification_label || !sg_ui.status_label || !string) return;
    lv_vendor_disp_lock();
    lv_label_set_text(sg_ui.notification_label, string);
    lv_obj_clear_flag(sg_ui.notification_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sg_ui.status_label, LV_OBJ_FLAG_HIDDEN);
    if (!sg_notification_tm) {
        sg_notification_tm = lv_timer_create(__notification_timeout_cb, 3000, NULL);
    } else {
        lv_timer_reset(sg_notification_tm);
    }
    lv_vendor_disp_unlock();
}

static void __ui_disp_chat_mode(char *string)
{
    if (!sg_ui.chat_mode_label || !string) return;
    lv_vendor_disp_lock();
    lv_label_set_text(sg_ui.chat_mode_label, string);
    lv_vendor_disp_unlock();
}

/* ── Registration ── */
OPERATE_RET ducky_custom_ui_register(void)
{
    AI_UI_INTFS_T intfs;
    memset(&intfs, 0, sizeof(intfs));

    intfs.disp_init                = __ui_init;
    intfs.disp_user_msg            = __ui_disp_user_msg;
    intfs.disp_ai_msg              = __ui_disp_ai_msg;
    intfs.disp_ai_msg_stream_start = __ui_disp_ai_msg_stream_start;
    intfs.disp_ai_msg_stream_data  = __ui_disp_ai_msg_stream_data;
    intfs.disp_ai_msg_stream_end   = __ui_disp_ai_msg_stream_end;
    intfs.disp_ai_mode_state       = __ui_disp_ai_mode_state;
    intfs.disp_notification        = __ui_disp_notification;
    intfs.disp_wifi_state          = __ui_disp_wifi_state;
    intfs.disp_ai_chat_mode        = __ui_disp_chat_mode;
    intfs.disp_other_msg           = __ui_disp_other_msg;

    return ai_ui_register(&intfs);
}

/* ── Public data-push APIs ── */
OPERATE_RET ducky_custom_ui_set_news(const char *news)
{
    if (!news) return OPRT_INVALID_PARM;
    return ai_ui_disp_msg((AI_UI_DISP_TYPE_E)DUCKY_UI_DISP_NEWS_TEXT,
                          (uint8_t *)news, strlen(news));
}

OPERATE_RET ducky_custom_ui_set_image_desc(const char *desc)
{
    if (!desc) return OPRT_INVALID_PARM;
    return ai_ui_disp_msg((AI_UI_DISP_TYPE_E)DUCKY_UI_DISP_IMAGE_DESC,
                          (uint8_t *)desc, strlen(desc));
}

OPERATE_RET ducky_custom_ui_set_weather(const char *text)
{
    if (!text) return OPRT_INVALID_PARM;
    return ai_ui_disp_msg((AI_UI_DISP_TYPE_E)DUCKY_UI_DISP_WEATHER,
                          (uint8_t *)text, strlen(text));
}

OPERATE_RET ducky_custom_ui_set_horoscope(const char *text)
{
    if (!text) return OPRT_INVALID_PARM;
    return ai_ui_disp_msg((AI_UI_DISP_TYPE_E)DUCKY_UI_DISP_HOROSCOPE,
                          (uint8_t *)text, strlen(text));
}

OPERATE_RET ducky_custom_ui_set_almanac(const char *text)
{
    if (!text) return OPRT_INVALID_PARM;
    return ai_ui_disp_msg((AI_UI_DISP_TYPE_E)DUCKY_UI_DISP_ALMANAC,
                          (uint8_t *)text, strlen(text));
}

OPERATE_RET ducky_custom_ui_set_sys_status(const char *text)
{
    if (!text) return OPRT_INVALID_PARM;
    return ai_ui_disp_msg((AI_UI_DISP_TYPE_E)DUCKY_UI_DISP_SYS_STATUS,
                          (uint8_t *)text, strlen(text));
}

OPERATE_RET ducky_custom_ui_set_wallpaper(const uint8_t *data, uint32_t size)
{
    if (!data || size == 0) return OPRT_INVALID_PARM;

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
    /* ── Decode JPEG → RGB565 using T5AI hardware JPEG decoder ── */
    TKL_JPEG_CODEC_INFO_T info;
    memset(&info, 0, sizeof(info));

    OPERATE_RET rt = tkl_jpeg_codec_img_info_get((uint8_t *)data, size, &info);
    if (rt != OPRT_OK) {
        PR_ERR("wallpaper: tkl_jpeg_codec_img_info_get failed: %d", rt);
        return rt;
    }

    if (info.out_width == 0 || info.out_height == 0) {
        PR_ERR("wallpaper: invalid image dimensions %ux%u", info.out_width, info.out_height);
        return OPRT_INVALID_PARM;
    }

    uint32_t pixels_size = (uint32_t)info.out_width * (uint32_t)info.out_height * 2u; /* RGB565 */
    uint8_t *new_pixels = (uint8_t *)tal_malloc(pixels_size);
    if (!new_pixels) {
        PR_ERR("wallpaper: malloc failed for %u bytes", pixels_size);
        return OPRT_MALLOC_FAILED;
    }

    info.in_size = size;
    rt = tkl_jpeg_codec_convert((uint8_t *)data, new_pixels, &info, JPEG_DEC_OUT_RGB565);
    if (rt != OPRT_OK) {
        PR_ERR("wallpaper: tkl_jpeg_codec_convert failed: %d", rt);
        tal_free(new_pixels);
        return rt;
    }

    /* ── Apply decoded image inside LVGL lock ── */
    lv_vendor_disp_lock();

    /* Hide old widget so LVGL stops referencing old pixel data */
    if (sg_wallpaper_img) {
        lv_obj_add_flag(sg_wallpaper_img, LV_OBJ_FLAG_HIDDEN);
    }

    /* Swap buffers and update descriptor */
    uint8_t *old_pixels   = sg_wallpaper_pixels;
    sg_wallpaper_pixels   = new_pixels;

    memset(&sg_wallpaper_dsc, 0, sizeof(sg_wallpaper_dsc));
    sg_wallpaper_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    sg_wallpaper_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    sg_wallpaper_dsc.header.w      = info.out_width;
    sg_wallpaper_dsc.header.h      = info.out_height;
    sg_wallpaper_dsc.header.stride = (uint32_t)info.out_width * 2u;
    sg_wallpaper_dsc.data_size     = pixels_size;
    sg_wallpaper_dsc.data          = sg_wallpaper_pixels;

    /* Create or update background image widget */
    if (!sg_wallpaper_img) {
        sg_wallpaper_img = lv_image_create(sg_ui.screen);
        lv_obj_set_pos(sg_wallpaper_img, 0, 0);
        lv_obj_set_size(sg_wallpaper_img, LV_HOR_RES, LV_VER_RES);
    }
    lv_image_set_src(sg_wallpaper_img, &sg_wallpaper_dsc);
    lv_obj_clear_flag(sg_wallpaper_img, LV_OBJ_FLAG_HIDDEN);
    /* Keep it at index 0 so it renders first (behind all panels) */
    lv_obj_move_to_index(sg_wallpaper_img, 0);

    /* Make screen bg transparent so wallpaper shows through the glass panels */
    lv_obj_set_style_bg_opa(sg_ui.screen, LV_OPA_TRANSP, 0);

    /* Free the superseded buffer while still holding the lock */
    if (old_pixels) {
        tal_free(old_pixels);
    }

    lv_vendor_disp_unlock();

    PR_NOTICE("wallpaper: applied %ux%u image to background", info.out_width, info.out_height);
    return OPRT_OK;
#else
    /* No hardware JPEG decoder on this platform — image was saved to FS only */
    PR_WARN("wallpaper: JPEG decode unavailable (ENABLE_COMP_AI_PICTURE not set)");
    return OPRT_NOT_SUPPORTED;
#endif
}

#else   /* ENABLE_AI_CHAT_CUSTOM_UI */

OPERATE_RET ducky_custom_ui_register(void)                       { return OPRT_OK; }
OPERATE_RET ducky_custom_ui_set_news(const char *n)              { (void)n; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_image_desc(const char *d)        { (void)d; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_weather(const char *t)           { (void)t; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_horoscope(const char *t)         { (void)t; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_almanac(const char *t)           { (void)t; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_sys_status(const char *t)        { (void)t; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_wallpaper(const uint8_t *d, uint32_t s) { (void)d; (void)s; return OPRT_NOT_SUPPORTED; }

#endif  /* ENABLE_AI_CHAT_CUSTOM_UI */
#endif  /* ENABLE_COMP_AI_DISPLAY */
