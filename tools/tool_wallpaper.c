/**
 * @file tool_wallpaper.c
 * @brief MCP tool to set the UI background wallpaper from an image URL
 * @version 0.1
 * @date 2026-03-19
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Workflow:
 *   1. AI (e.g. Feishu message) calls set_wallpaper with an image URL
 *   2. This tool spawns a background thread to download the image
 *   3. The image is saved to CLAW_FS_ROOT_PATH/config/wallpaper.jpg
 *   4. If the custom UI is enabled, ducky_custom_ui_set_wallpaper() is called
 *      to apply the image as the screen background
 */

#include "tool_wallpaper.h"
#include "tool_files.h"

#include "tal_api.h"
#include "tal_kv.h"
#include "http_session.h"

#include <string.h>
#include <stdlib.h>

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
#if defined(ENABLE_AI_CHAT_CUSTOM_UI) && (ENABLE_AI_CHAT_CUSTOM_UI == 1)
#include "ducky_custom_ui.h"
#define WALLPAPER_HAS_UI 1
#endif
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* Maximum wallpaper file size (256 KB is generous for a compressed JPEG) */
#define WALLPAPER_MAX_SIZE    (256 * 1024)
#define WALLPAPER_CHUNK_SIZE  1024

/* KV key used to persist the wallpaper URL when SD card is not available */
#define WALLPAPER_KV_KEY      "ducky_wallpaper_url"

/* Persisted wallpaper path (SD card only). */
#if defined(CLAW_USE_SDCARD) && (CLAW_USE_SDCARD == 1)
#define WALLPAPER_FILE        CLAW_FS_ROOT_PATH "/config/wallpaper.jpg"
#endif

/***********************************************************
***********************typedef define***********************
***********************************************************/

typedef struct {
    char url[512];
} WALLPAPER_TASK_ARG_T;

/***********************************************************
***********************function define**********************
***********************************************************/

/**
 * @brief Background thread: download image from URL, save to FS and apply.
 *
 * The thread is self-cleaning: it frees task_arg before exit.
 */
static void __wallpaper_download_task(void *arg)
{
    WALLPAPER_TASK_ARG_T *task_arg = (WALLPAPER_TASK_ARG_T *)arg;
    if (!task_arg) {
        return;
    }

    http_session_t  session  = NULL;
    http_resp_t    *response = NULL;
    uint8_t        *buf      = NULL;
    uint32_t        total    = 0;

    PR_NOTICE("wallpaper: [1/5] 开始下载, url=%s", task_arg->url);

    /* --- Open HTTP session --- */
    OPERATE_RET rt = http_open_session(&session, task_arg->url, 10000);
    if (rt != OPRT_OK) {
        PR_ERR("wallpaper: [1/5] http_open_session 失败, rt=%d", rt);
        goto __cleanup;
    }
    PR_NOTICE("wallpaper: [1/5] HTTP session 已建立");

    /* --- Send GET request --- */
    http_req_t req;
    memset(&req, 0, sizeof(req));
    req.type    = HTTP_GET;
    req.version = HTTP_VER_1_1;

    rt = http_send_request(session, &req, 0);
    if (rt != OPRT_OK) {
        PR_ERR("wallpaper: [1/5] http_send_request 失败, rt=%d", rt);
        goto __cleanup;
    }
    PR_NOTICE("wallpaper: [1/5] GET 请求已发送");

    /* --- Read response header --- */
    rt = http_get_response_hdr(session, &response);
    if (rt != OPRT_OK) {
        PR_ERR("wallpaper: [1/5] http_get_response_hdr 失败, rt=%d", rt);
        goto __cleanup;
    }
    PR_NOTICE("wallpaper: [1/5] HTTP 状态码=%d, content_length=%u",
              response->status_code, response->content_length);
    if (response->status_code != 200) {
        PR_ERR("wallpaper: [1/5] HTTP 状态码非200, 中止");
        goto __cleanup;
    }

    uint32_t content_len = response->content_length;
    http_free_response_hdr(&response);
    response = NULL;

    if (content_len == 0 || content_len > WALLPAPER_MAX_SIZE) {
        PR_ERR("wallpaper: [1/5] content_length=%u 不合法 (限制=%u)", content_len, WALLPAPER_MAX_SIZE);
        goto __cleanup;
    }

    /* --- Allocate download buffer --- */
    PR_NOTICE("wallpaper: [2/5] 分配下载缓冲区 %u bytes", content_len);
    buf = (uint8_t *)claw_malloc(content_len);
    if (!buf) {
        PR_ERR("wallpaper: [2/5] malloc 失败, 需要 %u bytes", content_len);
        goto __cleanup;
    }

    /* --- Read body --- */
    PR_NOTICE("wallpaper: [2/5] 开始读取 body...");
    while (total < content_len) {
        uint32_t want = WALLPAPER_CHUNK_SIZE;
        if (total + want > content_len) {
            want = content_len - total;
        }
        int len = http_read_content(session, buf + total, (unsigned int)want);
        if (len <= 0) {
            PR_ERR("wallpaper: [2/5] http_read_content 返回 %d, 已读 %u/%u", len, total, content_len);
            break;
        }
        total += (uint32_t)len;
    }

    if (total != content_len) {
        PR_ERR("wallpaper: [2/5] 下载不完整 %u/%u bytes", total, content_len);
        goto __cleanup;
    }
    PR_NOTICE("wallpaper: [2/5] 下载完成, 共 %u bytes", total);

    /* --- Persist to filesystem --- */
#if defined(CLAW_USE_SDCARD) && (CLAW_USE_SDCARD == 1)
    PR_NOTICE("wallpaper: [3/5] 保存到文件 %s", WALLPAPER_FILE);
    /* SD card: save JPEG file for direct restore on next boot */
    claw_fs_mkdir(CLAW_CONFIG_DIR);
    TUYA_FILE f = claw_fopen(WALLPAPER_FILE, "w");
    if (f) {
        int written = claw_fwrite(buf, (int)total, f);
        claw_fclose(f);
        int verify = claw_fgetsize(WALLPAPER_FILE);
        PR_NOTICE("wallpaper: [3/5] 文件保存成功, written=%d, verify_size=%d", written, verify);
    } else {
        PR_ERR("wallpaper: [3/5] 打开文件失败: %s", WALLPAPER_FILE);
    }
#else
    /* No SD card: skip file save, URL will be persisted to KV after UI apply */
    PR_NOTICE("wallpaper: [3/5] 无SD卡, 跳过文件保存, 将通过KV持久化URL");
#endif

    /* --- Apply to UI (only if custom UI is compiled in) --- */
#if defined(WALLPAPER_HAS_UI)
    PR_NOTICE("wallpaper: [4/5] 调用 ducky_custom_ui_set_wallpaper, size=%u", total);
    rt = ducky_custom_ui_set_wallpaper(buf, total);
    if (rt != OPRT_OK) {
        PR_ERR("wallpaper: [4/5] ducky_custom_ui_set_wallpaper 失败, rt=%d", rt);
    } else {
        PR_NOTICE("wallpaper: [4/5] 壁纸设置成功");
#if !defined(CLAW_USE_SDCARD) || (CLAW_USE_SDCARD == 0)
        /* No SD card: persist URL to KV so we can re-download on next boot */
        int kv_rt = tal_kv_set(WALLPAPER_KV_KEY,
                               (const uint8_t *)task_arg->url,
                               strlen(task_arg->url) + 1);
        if (kv_rt == 0) {
            PR_NOTICE("wallpaper: [4/5] URL 已保存到KV: %s", task_arg->url);
        } else {
            PR_ERR("wallpaper: [4/5] URL 保存KV失败, rt=%d", kv_rt);
        }
#endif
    }
#else
    PR_WARN("wallpaper: [4/5] WALLPAPER_HAS_UI 未定义, 跳过 UI 设置");
#endif
    PR_NOTICE("wallpaper: [5/5] 任务完成");

__cleanup:
    if (response) {
        http_free_response_hdr(&response);
    }
    if (session) {
        http_close_session(&session);
    }
    if (buf) {
        claw_free(buf);
    }
    claw_free(task_arg);
}

/**
 * @brief MCP tool callback: set_wallpaper
 *
 * Validates the URL, spawns a background download thread, and returns
 * immediately so the MCP transport is not blocked.
 */
static OPERATE_RET __tool_set_wallpaper(const MCP_PROPERTY_LIST_T *properties,
                                        MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    /* --- Extract url parameter --- */
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, "url");
    const char *url = NULL;
    if (prop && prop->type == MCP_PROPERTY_TYPE_STRING) {
        url = prop->default_val.str_val;
    }

    PR_NOTICE("wallpaper: set_wallpaper 工具被调用, url=%s", url ? url : "(null)");

    if (!url || url[0] == '\0') {
        PR_ERR("wallpaper: 缺少 url 参数");
        ai_mcp_return_value_set_str(ret_val, "错误: 缺少必要参数 url");
        return OPRT_INVALID_PARM;
    }

    if (strlen(url) >= 512) {
        ai_mcp_return_value_set_str(ret_val, "错误: url 参数过长（最大512字符）");
        return OPRT_INVALID_PARM;
    }

    /* Reject non-HTTP URLs to prevent SSRF to internal resources */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        ai_mcp_return_value_set_str(ret_val,
            "错误: url 必须以 http:// 或 https:// 开头");
        return OPRT_INVALID_PARM;
    }

    /* --- Allocate task argument (freed by the background thread) --- */
    WALLPAPER_TASK_ARG_T *task_arg =
        (WALLPAPER_TASK_ARG_T *)claw_malloc(sizeof(WALLPAPER_TASK_ARG_T));
    if (!task_arg) {
        ai_mcp_return_value_set_str(ret_val, "错误: 内存不足，无法启动下载");
        return OPRT_MALLOC_FAILED;
    }
    strncpy(task_arg->url, url, sizeof(task_arg->url) - 1);
    task_arg->url[sizeof(task_arg->url) - 1] = '\0';

    /* --- Spawn background download thread --- */
    THREAD_CFG_T cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = 1024 * 8;
    cfg.priority   = THREAD_PRIO_1;
    cfg.thrdname   = "wallpaper_dl";

    THREAD_HANDLE thrd;
    OPERATE_RET rt = tal_thread_create_and_start(&thrd, NULL, NULL,
                                                 __wallpaper_download_task,
                                                 task_arg, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("wallpaper: tal_thread_create_and_start 失败, rt=%d", rt);
        claw_free(task_arg);
        ai_mcp_return_value_set_str(ret_val, "错误: 启动下载线程失败");
        return rt;
    }

    PR_NOTICE("wallpaper: 下载线程已启动, url=%s", url);

#if defined(CLAW_USE_SDCARD) && (CLAW_USE_SDCARD == 1)
    ai_mcp_return_value_set_str(ret_val,
        "壁纸下载已开始，完成后自动应用到屏幕背景。"
        "图片将保存到SD卡。");
#else
    ai_mcp_return_value_set_str(ret_val,
        "壁纸下载已开始，完成后自动应用到屏幕背景。"
        "URL将保存到KV存储，重启后联网会自动恢复。");
#endif
    return OPRT_OK;
}

/**
 * @brief Register the set_wallpaper MCP tool with the MCP server.
 *
 * @return OPERATE_RET OPRT_OK on success, error code on failure
 */
OPERATE_RET tool_wallpaper_register(void)
{
    return AI_MCP_TOOL_ADD(
        "set_wallpaper",
        "从指定的图片 URL 下载图片，并将其设置为设备 UI 的背景壁纸。\n"
        "典型使用场景：用户通过飞书或其他 IM 工具发送图片消息，AI 提取图片直链后\n"
        "调用此工具，设备自动下载并将图片应用为屏幕背景。\n"
        "下载为异步操作，工具立即返回，图片完成下载后自动生效。\n"
        "注意：\n"
        "- url 必须是可直接访问的 HTTP/HTTPS 图片直链\n"
        "- 支持 JPEG 格式，文件大小限制为 256 KB\n"
        "- 壁纸会持久化保存到设备存储中，重启后仍可恢复",
        __tool_set_wallpaper,
        NULL,
        MCP_PROP_STR(
            "url",
            "图片的 HTTP 或 HTTPS 直链地址，例如从飞书消息中提取的图片 URL。"
            "必须以 http:// 或 https:// 开头，最大长度 512 字符。"
        )
    );
}

/**
 * @brief Restore wallpaper from KV storage (no-SD-card path).
 *
 * Called after MQTT connects (WiFi is up). Reads the persisted URL from KV
 * and spawns a background download thread to re-apply the wallpaper.
 * On SD card builds this is a no-op (file-based restore is done at UI init).
 *
 * @return OPERATE_RET OPRT_OK on success or if nothing to restore
 */
OPERATE_RET tool_wallpaper_restore(void)
{
#if !defined(CLAW_USE_SDCARD) || (CLAW_USE_SDCARD == 0)
    uint8_t *url_buf = NULL;
    size_t   url_len = 0;

    int kv_rt = tal_kv_get(WALLPAPER_KV_KEY, &url_buf, &url_len);
    if (kv_rt != 0 || !url_buf || url_len == 0) {
        PR_NOTICE("wallpaper: 无保存的URL (KV rt=%d), 跳过恢复", kv_rt);
        if (url_buf) tal_kv_free(url_buf);
        return OPRT_OK;
    }

    /* Ensure null-terminated */
    url_buf[url_len - 1] = '\0';
    PR_NOTICE("wallpaper: 从 KV 恢复壁纸, url=%s", (char *)url_buf);

    WALLPAPER_TASK_ARG_T *task_arg =
        (WALLPAPER_TASK_ARG_T *)claw_malloc(sizeof(WALLPAPER_TASK_ARG_T));
    if (!task_arg) {
        PR_ERR("wallpaper: restore malloc 失败");
        tal_kv_free(url_buf);
        return OPRT_MALLOC_FAILED;
    }
    strncpy(task_arg->url, (char *)url_buf, sizeof(task_arg->url) - 1);
    task_arg->url[sizeof(task_arg->url) - 1] = '\0';
    tal_kv_free(url_buf);

    THREAD_CFG_T cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = 1024 * 8;
    cfg.priority   = THREAD_PRIO_1;
    cfg.thrdname   = "wp_restore";

    THREAD_HANDLE thrd;
    OPERATE_RET rt = tal_thread_create_and_start(&thrd, NULL, NULL,
                                                 __wallpaper_download_task,
                                                 task_arg, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("wallpaper: restore 启动下载线程失败, rt=%d", rt);
        claw_free(task_arg);
        return rt;
    }
    PR_NOTICE("wallpaper: 恢复下载线程已启动");
#endif
    return OPRT_OK;
}
