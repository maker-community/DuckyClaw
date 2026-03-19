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

#include "ducky_custom_ui.h"

/* ── Dimensions ── */
#define DUCKY_TOP_BAR_H   24
#define DUCKY_TIME_H      80
#define DUCKY_AI_H        120
#define DUCKY_INFO_H      (LV_VER_RES - DUCKY_TOP_BAR_H - DUCKY_TIME_H - DUCKY_AI_H)

/* ── Colours ── */
#define COLOR_BG_TOP      0x060B14
#define COLOR_BG_MID      0x0D1B33
#define COLOR_BG_BOTTOM   0x070D1C
#define COLOR_ACCENT      0x2979FF
#define COLOR_ACCENT2     0x00E5FF
#define COLOR_TEXT_DIM    0xB0C4DE
#define COLOR_TEXT_CYAN   0x4FC3F7
#define COLOR_TEXT_YELLOW 0xFFEB3B
#define COLOR_CARD_BG     0x102040

/* ── Extended display type IDs ── */
typedef enum {
    DUCKY_UI_DISP_NEWS_TEXT  = AI_UI_DISP_SYS_MAX + 100,
    DUCKY_UI_DISP_IMAGE_DESC,
    DUCKY_UI_DISP_WEATHER,
    DUCKY_UI_DISP_HOROSCOPE,
    DUCKY_UI_DISP_ALMANAC,
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

    /* AI panel */
    lv_obj_t *ai_panel;
    lv_obj_t *ai_msg_label;
} DUCKY_CUSTOM_UI_T;

static DUCKY_CUSTOM_UI_T sg_ui;
static bool              sg_is_streaming    = false;
static lv_timer_t       *sg_notification_tm = NULL;
static lv_timer_t       *sg_clock_tm        = NULL;

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

/* Create a rounded info card. Returns card; sets *out_body. */
static lv_obj_t *__make_card(lv_obj_t *parent, int w, int h,
                              lv_obj_t **out_body,
                              const char *icon_sym,
                              const char *title_text,
                              const char *body_text)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(card);
    lv_obj_set_style_text_font(icon, ai_ui_get_icon_font(), 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_ACCENT2), 0);
    lv_label_set_text(icon, icon_sym);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT_CYAN), 0);
    lv_label_set_text(title, title_text);
    lv_obj_align_to(title, icon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_obj_set_width(body, w - 12);
    lv_obj_set_style_text_color(body, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body, body_text);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 20);

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

    lv_obj_set_style_bg_color(sg_ui.screen, lv_color_hex(COLOR_BG_BOTTOM), 0);
    lv_obj_set_style_bg_opa(sg_ui.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(sg_ui.screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(sg_ui.screen, 0, 0);

    /* ── TOP STATUS BAR ── */
    sg_ui.top_bar = lv_obj_create(sg_ui.screen);
    lv_obj_set_size(sg_ui.top_bar, LV_HOR_RES, DUCKY_TOP_BAR_H);
    lv_obj_align(sg_ui.top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(sg_ui.top_bar, 0, 0);
    lv_obj_set_style_bg_color(sg_ui.top_bar, lv_color_hex(COLOR_BG_TOP), 0);
    lv_obj_set_style_bg_opa(sg_ui.top_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(sg_ui.top_bar, 0, 0);
    lv_obj_set_style_border_color(sg_ui.top_bar, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(sg_ui.top_bar, 1, 0);
    lv_obj_set_style_border_side(sg_ui.top_bar, LV_BORDER_SIDE_BOTTOM, 0);

    sg_ui.chat_mode_label = lv_label_create(sg_ui.top_bar);
    lv_label_set_text(sg_ui.chat_mode_label, "Chat");
    lv_obj_set_style_text_color(sg_ui.chat_mode_label, lv_color_white(), 0);
    lv_obj_align(sg_ui.chat_mode_label, LV_ALIGN_LEFT_MID, 6, 0);

    sg_ui.notification_label = lv_label_create(sg_ui.top_bar);
    lv_label_set_text(sg_ui.notification_label, "");
    lv_obj_set_width(sg_ui.notification_label, LV_HOR_RES - 100);
    lv_obj_set_style_text_align(sg_ui.notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(sg_ui.notification_label, lv_color_hex(COLOR_TEXT_YELLOW), 0);
    lv_obj_align(sg_ui.notification_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(sg_ui.notification_label, LV_OBJ_FLAG_HIDDEN);

    sg_ui.status_label = lv_label_create(sg_ui.top_bar);
    lv_obj_set_width(sg_ui.status_label, LV_HOR_RES - 100);
    lv_label_set_long_mode(sg_ui.status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(sg_ui.status_label, "Initializing");
    lv_obj_set_style_text_align(sg_ui.status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(sg_ui.status_label, lv_color_hex(COLOR_TEXT_CYAN), 0);
    lv_obj_align(sg_ui.status_label, LV_ALIGN_CENTER, 0, 0);

    /* WiFi icon: explicitly white so it shows on the dark bar */
    sg_ui.network_label = lv_label_create(sg_ui.top_bar);
    lv_obj_set_style_text_font(sg_ui.network_label, ai_ui_get_icon_font(), 0);
    lv_obj_set_style_text_color(sg_ui.network_label, lv_color_white(), 0);
    lv_label_set_text(sg_ui.network_label, ai_ui_get_wifi_icon(AI_UI_WIFI_STATUS_DISCONNECTED));
    lv_obj_align(sg_ui.network_label, LV_ALIGN_RIGHT_MID, -6, 0);

    /* ── TIME PANEL ── */
    sg_ui.time_panel = lv_obj_create(sg_ui.screen);
    lv_obj_set_size(sg_ui.time_panel, LV_HOR_RES, DUCKY_TIME_H);
    lv_obj_align(sg_ui.time_panel, LV_ALIGN_TOP_MID, 0, DUCKY_TOP_BAR_H);
    lv_obj_set_style_radius(sg_ui.time_panel, 0, 0);
    lv_obj_set_style_border_width(sg_ui.time_panel, 0, 0);
    lv_obj_set_style_bg_color(sg_ui.time_panel, lv_color_hex(COLOR_BG_MID), 0);
    lv_obj_set_style_bg_opa(sg_ui.time_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(sg_ui.time_panel, 4, 0);
    lv_obj_clear_flag(sg_ui.time_panel, LV_OBJ_FLAG_SCROLLABLE);

    sg_ui.time_label = lv_label_create(sg_ui.time_panel);
    lv_obj_set_style_text_color(sg_ui.time_label, lv_color_white(), 0);
    lv_label_set_text(sg_ui.time_label, "--:--");
    lv_obj_align(sg_ui.time_label, LV_ALIGN_LEFT_MID, 8, -6);

    sg_ui.date_label = lv_label_create(sg_ui.time_panel);
    lv_obj_set_style_text_color(sg_ui.date_label, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_label_set_text(sg_ui.date_label, "----年--月--日");
    lv_obj_align(sg_ui.date_label, LV_ALIGN_LEFT_MID, 8, 14);

    sg_ui.lunar_label = lv_label_create(sg_ui.time_panel);
    lv_obj_set_width(sg_ui.lunar_label, 130);
    lv_label_set_long_mode(sg_ui.lunar_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(sg_ui.lunar_label, lv_color_hex(COLOR_TEXT_CYAN), 0);
    lv_label_set_text(sg_ui.lunar_label, "农历加载中…");
    lv_obj_align(sg_ui.lunar_label, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ── INFO CARDS PANEL ── */
    sg_ui.info_panel = lv_obj_create(sg_ui.screen);
    lv_obj_set_size(sg_ui.info_panel, LV_HOR_RES, DUCKY_INFO_H);
    lv_obj_align(sg_ui.info_panel, LV_ALIGN_TOP_MID, 0, DUCKY_TOP_BAR_H + DUCKY_TIME_H);
    lv_obj_set_style_radius(sg_ui.info_panel, 0, 0);
    lv_obj_set_style_border_width(sg_ui.info_panel, 0, 0);
    lv_obj_set_style_bg_color(sg_ui.info_panel, lv_color_hex(COLOR_BG_BOTTOM), 0);
    lv_obj_set_style_bg_opa(sg_ui.info_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(sg_ui.info_panel, 6, 0);
    lv_obj_set_style_pad_row(sg_ui.info_panel, 6, 0);
    lv_obj_set_flex_flow(sg_ui.info_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(sg_ui.info_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(sg_ui.info_panel, LV_OBJ_FLAG_SCROLLABLE);

    int card_w = LV_HOR_RES - 12;
    int card_h = (DUCKY_INFO_H - 6 - 12) / 3;

    __make_card(sg_ui.info_panel, card_w, card_h, &sg_ui.horoscope_body,
                FONT_AWESOME_EMOJI_HAPPY, "星座运势", "今日运势加载中…");

    __make_card(sg_ui.info_panel, card_w, card_h, &sg_ui.almanac_body,
                FONT_AWESOME_CHECK, "黄历宜忌", "宜: 加载中…  忌: 加载中…");

    __make_card(sg_ui.info_panel, card_w, card_h, &sg_ui.weather_body,
                FONT_AWESOME_GLOBE, "天气", "天气信息加载中…");

    /* ── AI MESSAGE PANEL ── */
    sg_ui.ai_panel = lv_obj_create(sg_ui.screen);
    lv_obj_set_size(sg_ui.ai_panel, LV_HOR_RES, DUCKY_AI_H);
    lv_obj_align(sg_ui.ai_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(sg_ui.ai_panel, 0, 0);
    lv_obj_set_style_bg_color(sg_ui.ai_panel, lv_color_hex(COLOR_BG_MID), 0);
    lv_obj_set_style_bg_opa(sg_ui.ai_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sg_ui.ai_panel, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(sg_ui.ai_panel, 2, 0);
    lv_obj_set_style_border_side(sg_ui.ai_panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(sg_ui.ai_panel, 6, 0);
    lv_obj_clear_flag(sg_ui.ai_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ai_icon = lv_label_create(sg_ui.ai_panel);
    lv_obj_set_style_text_font(ai_icon, ai_ui_get_icon_font(), 0);
    lv_obj_set_style_text_color(ai_icon, lv_color_hex(COLOR_ACCENT2), 0);
    lv_label_set_text(ai_icon, FONT_AWESOME_AI_CHIP);
    lv_obj_align(ai_icon, LV_ALIGN_TOP_LEFT, 0, 0);

    sg_ui.ai_msg_label = lv_label_create(sg_ui.ai_panel);
    lv_obj_set_size(sg_ui.ai_msg_label, LV_HOR_RES - 30, DUCKY_AI_H - 12);
    lv_label_set_long_mode(sg_ui.ai_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(sg_ui.ai_msg_label, lv_color_white(), 0);
    lv_label_set_text(sg_ui.ai_msg_label, "DuckyClaw 已就绪，有什么可以帮你？");
    lv_obj_align(sg_ui.ai_msg_label, LV_ALIGN_TOP_LEFT, 22, 0);

    lv_vendor_disp_unlock();

    /* Clock timer: fires every 30s, immediately on first run */
    sg_clock_tm = lv_timer_create(__clock_update_cb, 30000, NULL);
    lv_timer_ready(sg_clock_tm);

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

#else   /* ENABLE_AI_CHAT_CUSTOM_UI */

OPERATE_RET ducky_custom_ui_register(void)                       { return OPRT_OK; }
OPERATE_RET ducky_custom_ui_set_news(const char *n)              { (void)n; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_image_desc(const char *d)        { (void)d; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_weather(const char *t)           { (void)t; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_horoscope(const char *t)         { (void)t; return OPRT_NOT_SUPPORTED; }
OPERATE_RET ducky_custom_ui_set_almanac(const char *t)           { (void)t; return OPRT_NOT_SUPPORTED; }

#endif  /* ENABLE_AI_CHAT_CUSTOM_UI */
#endif  /* ENABLE_COMP_AI_DISPLAY */
