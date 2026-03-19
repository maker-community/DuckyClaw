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

#ifdef __cplusplus
}
#endif

#endif /* __DUCKY_CUSTOM_UI_H__ */
