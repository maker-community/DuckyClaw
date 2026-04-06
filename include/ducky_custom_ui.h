/**
 * @file ducky_custom_ui.h
 * @brief DuckyClaw custom AI chat UI registration API.
 */

#ifndef __DUCKY_CUSTOM_UI_H__
#define __DUCKY_CUSTOM_UI_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

OPERATE_RET ducky_custom_ui_register(void);
OPERATE_RET ducky_custom_ui_set_news(const char *news);
OPERATE_RET ducky_custom_ui_set_image_desc(const char *desc);
OPERATE_RET ducky_custom_ui_set_weather(const char *text);
OPERATE_RET ducky_custom_ui_set_horoscope(const char *text);
OPERATE_RET ducky_custom_ui_set_almanac(const char *text);
OPERATE_RET ducky_custom_ui_set_sys_status(const char *text);

/**
 * @brief Restore wallpaper from persistent storage when available.
 *
 * On SD-card builds this loads `/sdcard/config/wallpaper.jpg` after the
 * filesystem has been mounted. On non-SD builds this is a no-op.
 *
 * @return OPERATE_RET OPRT_OK on success or when no wallpaper is available
 */
OPERATE_RET ducky_custom_ui_restore_wallpaper(void);

/**
 * @brief Set the screen background wallpaper from raw JPEG data.
 *
 * Downloads the full JPEG image (already in memory) and decodes it to
 * RGB565 for display as the LVGL screen background.  The caller retains
 * ownership of @data and may free it after this call returns.
 *
 * @param data  Pointer to JPEG image bytes
 * @param size  Number of bytes in @data
 * @return OPERATE_RET OPRT_OK on success, error code otherwise
 */
OPERATE_RET ducky_custom_ui_set_wallpaper(const uint8_t *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* __DUCKY_CUSTOM_UI_H__ */
