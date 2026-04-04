/**
 * @file ai_mcp_tools.c
 * @version 0.1
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 *
 */

#include "tal_api.h"

#include "tuya_ai_agent.h"

#include "ai_manage_mode.h"

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
#include "ai_audio_player.h"
#endif

#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
#include "ai_video_input.h"
#endif

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
#include "ai_picture_convert.h"
#endif

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
#include "ai_ui_manage.h"
#endif

#include "ai_mcp_server.h"

#include "ai_mcp.h"
/***********************************************************
************************macro define************************
***********************************************************/
#if (defined(CONFIG_AI_DISP_VIDEO_ROTATION_0) && (CONFIG_AI_DISP_VIDEO_ROTATION_0 == 1)) || \
    (defined(AI_DISP_VIDEO_ROTATION_0) && (AI_DISP_VIDEO_ROTATION_0 == 1))
#define AI_MCP_PHOTO_ROTATION TUYA_DISPLAY_ROTATION_0
#elif (defined(CONFIG_AI_DISP_VIDEO_ROTATION_90) && (CONFIG_AI_DISP_VIDEO_ROTATION_90 == 1)) || \
    (defined(AI_DISP_VIDEO_ROTATION_90) && (AI_DISP_VIDEO_ROTATION_90 == 1))
#define AI_MCP_PHOTO_ROTATION TUYA_DISPLAY_ROTATION_90
#elif (defined(CONFIG_AI_DISP_VIDEO_ROTATION_180) && (CONFIG_AI_DISP_VIDEO_ROTATION_180 == 1)) || \
    (defined(AI_DISP_VIDEO_ROTATION_180) && (AI_DISP_VIDEO_ROTATION_180 == 1))
#define AI_MCP_PHOTO_ROTATION TUYA_DISPLAY_ROTATION_180
#elif (defined(CONFIG_AI_DISP_VIDEO_ROTATION_270) && (CONFIG_AI_DISP_VIDEO_ROTATION_270 == 1)) || \
    (defined(AI_DISP_VIDEO_ROTATION_270) && (AI_DISP_VIDEO_ROTATION_270 == 1))
#define AI_MCP_PHOTO_ROTATION TUYA_DISPLAY_ROTATION_270
#elif defined(CONFIG_TUYA_T5AI_BOARD_EX_MODULE_35565LCD) && (CONFIG_TUYA_T5AI_BOARD_EX_MODULE_35565LCD == 1)
#define AI_MCP_PHOTO_ROTATION TUYA_DISPLAY_ROTATION_270
#elif defined(TUYA_T5AI_BOARD_EX_MODULE_35565LCD) && (TUYA_T5AI_BOARD_EX_MODULE_35565LCD == 1)
#define AI_MCP_PHOTO_ROTATION TUYA_DISPLAY_ROTATION_270
#else
#define AI_MCP_PHOTO_ROTATION TUYA_DISPLAY_ROTATION_0
#endif

#define AI_MCP_JPEG_SOI_SIZE               2
#define AI_MCP_JPEG_EXIF_SEGMENT_SIZE      38
#define AI_MCP_JPEG_EXIF_PAYLOAD_LEN       34
#define AI_MCP_EXIF_ORIENTATION_NORMAL     1
#define AI_MCP_EXIF_ORIENTATION_ROTATE_180 3
#define AI_MCP_EXIF_ORIENTATION_ROTATE_90  6
#define AI_MCP_EXIF_ORIENTATION_ROTATE_270 8


/***********************************************************
***********************typedef define***********************
***********************************************************/


/***********************************************************
***********************variable define**********************
***********************************************************/


/***********************************************************
***********************function define**********************
***********************************************************/
static uint16_t __get_photo_exif_orientation(void)
{
    switch (AI_MCP_PHOTO_ROTATION) {
    case TUYA_DISPLAY_ROTATION_90:
        return AI_MCP_EXIF_ORIENTATION_ROTATE_270;
    case TUYA_DISPLAY_ROTATION_180:
        return AI_MCP_EXIF_ORIENTATION_ROTATE_180;
    case TUYA_DISPLAY_ROTATION_270:
        return AI_MCP_EXIF_ORIENTATION_ROTATE_90;
    case TUYA_DISPLAY_ROTATION_0:
    default:
        return AI_MCP_EXIF_ORIENTATION_NORMAL;
    }
}

static bool __jpeg_has_exif_orientation(const uint8_t *jpeg_data, uint32_t jpeg_size)
{
    if (!jpeg_data || jpeg_size < 12) {
        return false;
    }

    if (jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return false;
    }

    if (jpeg_data[2] != 0xFF || jpeg_data[3] != 0xE1) {
        return false;
    }

    if (memcmp(&jpeg_data[6], "Exif\0\0", 6) != 0) {
        return false;
    }

    return true;
}

static OPERATE_RET __jpeg_add_exif_orientation(const uint8_t *src,
                                               uint32_t src_len,
                                               uint16_t orientation,
                                               uint8_t **dst,
                                               uint32_t *dst_len)
{
    static const uint8_t exif_prefix[AI_MCP_JPEG_EXIF_SEGMENT_SIZE - AI_MCP_JPEG_SOI_SIZE] = {
        0xFF, 0xE1, 0x00, 0x22,
        0x45, 0x78, 0x69, 0x66, 0x00, 0x00,
        0x49, 0x49, 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
        0x01, 0x00,
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    uint8_t *out = NULL;

    if (!src || !dst || !dst_len || src_len < AI_MCP_JPEG_SOI_SIZE) {
        return OPRT_INVALID_PARM;
    }

    if (src[0] != 0xFF || src[1] != 0xD8) {
        return OPRT_NOT_SUPPORTED;
    }

    out = (uint8_t *)tal_malloc(src_len + AI_MCP_JPEG_EXIF_SEGMENT_SIZE);
    if (!out) {
        return OPRT_MALLOC_FAILED;
    }

    memcpy(out, src, AI_MCP_JPEG_SOI_SIZE);
    memcpy(out + AI_MCP_JPEG_SOI_SIZE,
           exif_prefix,
           sizeof(exif_prefix));
    out[AI_MCP_JPEG_SOI_SIZE + 28] = (uint8_t)(orientation & 0xFF);
    out[AI_MCP_JPEG_SOI_SIZE + 29] = (uint8_t)((orientation >> 8) & 0xFF);
    memcpy(out + AI_MCP_JPEG_EXIF_SEGMENT_SIZE,
           src + AI_MCP_JPEG_SOI_SIZE,
           src_len - AI_MCP_JPEG_SOI_SIZE);

    *dst = out;
    *dst_len = src_len + AI_MCP_JPEG_EXIF_SEGMENT_SIZE;
    return OPRT_OK;
}

static OPERATE_RET __get_device_info(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    cJSON *json = NULL;

    json = cJSON_CreateObject();
    if (!json) {
        PR_ERR("Create JSON object failed");
        return OPRT_MALLOC_FAILED;
    }

    // Implement device info retrieval logic here
    // Add device information
    cJSON_AddStringToObject(json, "model", PROJECT_NAME);
    cJSON_AddStringToObject(json, "serialNumber", "123456789");
    cJSON_AddStringToObject(json, "firmwareVersion", PROJECT_VERSION);

    // Set return value
    ai_mcp_return_value_set_json(ret_val, json);

    return OPRT_OK;
}
#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
static OPERATE_RET __take_photo(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    OPERATE_RET rt = OPRT_OK;
    uint8_t *image_data = NULL;
    uint8_t *upload_image_data = NULL;
    uint8_t *oriented_image_data = NULL;
    uint32_t image_size = 0;
    uint32_t upload_image_size = 0;
    uint16_t exif_orientation = AI_MCP_EXIF_ORIENTATION_NORMAL;

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1) && \
    defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
    AI_PICTURE_INFO_T picture_info;
    memset(&picture_info, 0, sizeof(picture_info));
#endif

    TUYA_CALL_ERR_LOG(ai_video_display_start());

    tal_system_sleep(3000);

    rt = ai_video_get_jpeg_frame(&image_data, &image_size);
    if (OPRT_OK != rt) {
        PR_ERR("get jpeg frame err, rt:%d", rt);
        return rt;
    }

    upload_image_data = image_data;
    upload_image_size = image_size;
    exif_orientation = __get_photo_exif_orientation();

    if (exif_orientation != AI_MCP_EXIF_ORIENTATION_NORMAL &&
        false == __jpeg_has_exif_orientation(image_data, image_size)) {
        rt = __jpeg_add_exif_orientation(image_data,
                                         image_size,
                                         exif_orientation,
                                         &oriented_image_data,
                                         &upload_image_size);
        if (rt == OPRT_OK) {
            upload_image_data = oriented_image_data;
            PR_NOTICE("photo upload: injected EXIF orientation=%u", exif_orientation);
        } else {
            PR_ERR("photo upload: inject EXIF orientation failed, rt:%d", rt);
            upload_image_data = image_data;
            upload_image_size = image_size;
        }
    }

    rt = ai_mcp_return_value_set_image(ret_val, MCP_IMAGE_MIME_TYPE_JPEG, upload_image_data, upload_image_size);
    if (OPRT_OK != rt) {
        PR_ERR("set return image err, rt:%d", rt);
        if (oriented_image_data) {
            tal_free(oriented_image_data);
        }
        ai_video_jpeg_image_free(&image_data);
        return rt;
    }

    if (oriented_image_data) {
        tal_free(oriented_image_data);
        oriented_image_data = NULL;
    }

    TUYA_CALL_ERR_LOG(ai_video_display_stop());

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1) && \
    defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
    AI_PICTURE_CONVERT_CFG_T convert_cfg = {
        .in_fmt = TUYA_FRAME_FMT_JPEG,
        .in_frame_size = image_size,
        .out_fmt = TUYA_FRAME_FMT_RGB565,
    };

    rt = ai_picture_convert_start(&convert_cfg);
    if (rt == OPRT_OK) {
        TUYA_CALL_ERR_LOG(ai_picture_convert_feed(image_data, image_size));
        rt = ai_picture_convert(&picture_info);
        if (rt == OPRT_OK) {
            TUYA_CALL_ERR_LOG(ai_ui_disp_picture(picture_info.fmt, picture_info.width, picture_info.height,
                                                picture_info.frame, picture_info.frame_size));
        } else {
            PR_ERR("convert local picture err, rt:%d", rt);
        }
        TUYA_CALL_ERR_LOG(ai_picture_convert_stop());
    } else {
        PR_ERR("start local picture convert err, rt:%d", rt);
    }
#endif

    ai_video_jpeg_image_free(&image_data);

    return OPRT_OK;
}
#endif

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
static OPERATE_RET __set_volume(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    uint32_t volume = 50; // default volume

    PR_DEBUG("__set_volume enter");

    // Parse properties to get volume
    for (int i = 0; i < properties->count; i++) {
        MCP_PROPERTY_T *prop = properties->properties[i];
        if (strcmp(prop->name, "volume") == 0 && prop->type == MCP_PROPERTY_TYPE_INTEGER) {
            volume = prop->default_val.int_val;
            break;
        }
    }

    // FIXME: Implement actual volume setting logic here
    ai_audio_player_set_vol(volume);
    PR_DEBUG("set volume to %d", volume);

    // Set return value
    ai_mcp_return_value_set_bool(ret_val, TRUE);

    PR_DEBUG("__set_volume exit");

    return OPRT_OK;
}
#endif
static OPERATE_RET __set_mode(const MCP_PROPERTY_LIST_T *properties, MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    AI_CHAT_MODE_E mode = 0;
    
    ai_mode_get_curr_mode(&mode);

    // Parse properties to get volume
    for (int i = 0; i < properties->count; i++) {
        MCP_PROPERTY_T *prop = properties->properties[i];
        if (strcmp(prop->name, "mode") == 0 && prop->type == MCP_PROPERTY_TYPE_INTEGER) {
            mode = prop->default_val.int_val;
            break;
        }
    }

    // Implement actual volume setting logic here
    OPERATE_RET rt = ai_mode_switch(mode);

    PR_DEBUG("set mode to %d rt:%d", mode, rt);

    // Set return value
    ai_mcp_return_value_set_bool(ret_val, (rt == OPRT_OK) ? TRUE : FALSE);

    return OPRT_OK;
}

static OPERATE_RET __ai_mcp_tools_register(void)
{
    OPERATE_RET rt = OPRT_OK;

    // device info get tool
    TUYA_CALL_ERR_GOTO(AI_MCP_TOOL_ADD(
        "device_info_get",
        "Get device information such as model, serial number, and firmware version.",
        __get_device_info,
        NULL
    ), err);

#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
    // device camera take photo tool
    TUYA_CALL_ERR_GOTO(AI_MCP_TOOL_ADD(
        "device_camera_take_photo",
        "Captures one or more photos using the device camera. Use when picture or scene "
        "change is detected, for visitor detection, or when the user asks for a photo.\n"
        "Parameters:\n"
        "- count (int): Number of photos to capture (1-10).\n"
        "Returns: Captured image(s) in Base64 format.",
        __take_photo,
        NULL,
        MCP_PROP_STR("question", "The question prompting the photo capture."),
        MCP_PROP_INT_DEF_RANGE("count", "Number of photos to capture (1-10).", 1, 1, 10)
    ), err);
#endif

#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
    // set volume tool
    TUYA_CALL_ERR_GOTO(AI_MCP_TOOL_ADD(
        "device_audio_volume_set",
        "Sets the device's volume level.\n"
        "Parameters:\n"
        "- volume (int): The volume level to set (0-100).\n"
        "Response:\n"
        "- Returns true if the volume was set successfully.",
        __set_volume,
        NULL,
        MCP_PROP_INT_RANGE("volume", "The volume level to set (0-100).", 0, 100)
    ), err);
#endif

    TUYA_CALL_ERR_GOTO(AI_MCP_TOOL_ADD(
        "device_audio_mode_set",
        "Sets the device's chat mode.\n"
        "Parameters:\n"
        "- mode (integer): The chat mode (0=hold, 1=key_press, 2=wakeup, 3=free).\n"
        "Response:\n"
        "- Returns true if the mode was set successfully.",
        __set_mode,
        NULL,
        MCP_PROP_INT_RANGE("mode", "The chat mode (0=hold, 1=key_press, 2=wakeup, 3=free)", 0, 3)
    ), err);

    return OPRT_OK;

err:
    // destroy MCP server on failure
    ai_mcp_server_destroy();

    return rt;
}

static OPERATE_RET __ai_mcp_init(void *data)
{
    OPERATE_RET rt = OPRT_OK;

    // FIXME: Set actual MCP server name and mcp version
    TUYA_CALL_ERR_RETURN(ai_mcp_server_init("Tuya MCP Server", "1.0"));

    TUYA_CALL_ERR_RETURN(__ai_mcp_tools_register());

    PR_DEBUG("MCP Server initialized successfully");

    return rt;
}

OPERATE_RET ai_mcp_init(void)
{
    return tal_event_subscribe(EVENT_MQTT_CONNECTED, "ai_mcp_init", __ai_mcp_init, SUBSCRIBE_TYPE_ONETIME);
}

OPERATE_RET ai_mcp_deinit(void)
{
    ai_mcp_server_destroy();

    PR_DEBUG("MCP Server deinitialized successfully");

    return OPRT_OK;
}