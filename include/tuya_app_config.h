/**
 * @file tuya_app_config.h
 * @brief tuya_app_config module is used to 
 * @version 0.1
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __TUYA_APP_CONFIG_H__
#define __TUYA_APP_CONFIG_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// TODO: PID Copy Link
#ifndef TUYA_PRODUCT_ID
#define TUYA_PRODUCT_ID "xxxxxxxxxxxxxxxx"
#endif

#define TUYA_OPENSDK_UUID    "uuidxxxxxxxxxxxxxxxx"             // Please change the correct uuid
#define TUYA_OPENSDK_AUTHKEY "keyxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" // Please change the correct authkey

// IM configuration
// feishu | telegram | discord
#define IM_SECRET_CHANNEL_MODE      "feishu"

#define IM_SECRET_FS_APP_ID         ""
#define IM_SECRET_FS_APP_SECRET     ""

#define IM_SECRET_DC_TOKEN          ""
#define IM_SECRET_DC_CHANNEL_ID     ""

#define IM_SECRET_TG_TOKEN          ""

#ifdef __cplusplus
}
#endif

#endif /* __TUYA_APP_CONFIG_H__ */
