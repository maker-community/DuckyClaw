/**
 * @file tool_wallpaper.h
 * @brief MCP tool to set the UI background wallpaper from an image URL
 * @version 0.1
 * @date 2026-03-19
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __TOOL_WALLPAPER_H__
#define __TOOL_WALLPAPER_H__

#include "tuya_cloud_types.h"
#include "ai_mcp_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the set_wallpaper MCP tool
 *
 * Registers the set_wallpaper tool that downloads an image from a given URL
 * and applies it as the device UI background wallpaper.
 *
 * @return OPERATE_RET OPRT_OK on success, error code on failure
 */
OPERATE_RET tool_wallpaper_register(void);

/**
 * @brief Restore wallpaper from KV (no-SD-card) or file (SD card) after boot.
 *
 * Must be called after WiFi/MQTT is connected (download needs network).
 * On SD card builds this is a no-op (file restore happens at UI init).
 *
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET tool_wallpaper_restore(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOOL_WALLPAPER_H__ */
