/**
 * @file bq27220.h
 * @brief BQ27220 single-cell Li-ion fuel gauge driver for TuyaOpen / T5AI board.
 *
 * Hardware connection: P0 = SCL, P1 = SDA
 * I2C address: 0x55 (fixed)
 */

#ifndef __BQ27220_H__
#define __BQ27220_H__

#include <stdint.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/
#define BQ27220_I2C_PORT        TUYA_I2C_NUM_2
#define BQ27220_I2C_ADDR        0x55

#define BQ27220_SCL_PIN         TUYA_GPIO_NUM_0
#define BQ27220_SDA_PIN         TUYA_GPIO_NUM_1

/* Default battery pack nominal capacity used to sync BQ27220 design data. */
#ifdef BATTERY_MONITOR_DESIGN_CAPACITY_MAH
#define BQ27220_DEFAULT_DESIGN_CAPACITY_MAH BATTERY_MONITOR_DESIGN_CAPACITY_MAH
#else
#define BQ27220_DEFAULT_DESIGN_CAPACITY_MAH 1000
#endif

/***********************************************************
***********************typedef define***********************
***********************************************************/

/**
 * @brief Battery status flags mapped to BQ27220 BatteryStatus register (0x0A).
 */
typedef struct {
    uint16_t dsg      : 1;  /* Device is in DISCHARGE */
    uint16_t sysdwn   : 1;  /* System down bit */
    uint16_t tda      : 1;  /* Terminate Discharge Alarm */
    uint16_t battpres : 1;  /* Battery Present detected */
    uint16_t auth_gd  : 1;  /* Detect inserted battery */
    uint16_t ocvgd    : 1;  /* Good OCV measurement taken */
    uint16_t tca      : 1;  /* Terminate Charge Alarm */
    uint16_t rsvd     : 1;  /* Reserved */
    uint16_t chginh   : 1;  /* Charge inhibit */
    uint16_t fc       : 1;  /* Full-charged is detected */
    uint16_t otd      : 1;  /* Overtemperature in discharge */
    uint16_t otc      : 1;  /* Overtemperature in charge */
    uint16_t sleep    : 1;  /* Device is in SLEEP mode */
    uint16_t ocvfail  : 1;  /* OCV reading failed */
    uint16_t ocvcomp  : 1;  /* OCV measurement complete */
    uint16_t fd       : 1;  /* Full-discharge is detected */
} bq27220_status_t;

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Initialize BQ27220: configure pinmux, init I2C, verify device ID.
 * @return OPRT_OK on success.
 */
OPERATE_RET bq27220_init(void);

/**
 * @brief Deinitialize I2C bus used by BQ27220.
 */
void bq27220_deinit(void);

/**
 * @brief Get state of charge (0-100 %).
 * @return SOC percentage, or -1 on error.
 */
int bq27220_get_soc(void);

/**
 * @brief Get battery voltage in mV.
 * @return Voltage in mV, or -1 on error.
 */
int bq27220_get_voltage(void);

/**
 * @brief Get instantaneous current in mA (positive = charging, negative = discharging).
 * @return Current in mA, or INT16_MIN on error.
 */
int bq27220_get_current(void);

/**
 * @brief Get battery temperature in degrees Celsius.
 * @return Temperature in °C, or INT16_MIN on error.
 */
int bq27220_get_temperature(void);

/**
 * @brief Get remaining capacity in mAh.
 * @return Remaining capacity in mAh, or -1 on error.
 */
int bq27220_get_remaining_capacity(void);

/**
 * @brief Get full charge capacity in mAh.
 * @return Full charge capacity in mAh, or -1 on error.
 */
int bq27220_get_full_capacity(void);

/**
 * @brief Get design capacity in mAh.
 * @return Design capacity in mAh, or -1 on error.
 */
int bq27220_get_design_capacity(void);

/**
 * @brief Get state of health (0-100 %).
 * @return SOH percentage, or -1 on error.
 */
int bq27220_get_soh(void);

/**
 * @brief Get average power in mW (signed).
 * @return Average power in mW, or INT16_MIN on error.
 */
int bq27220_get_average_power(void);

/**
 * @brief Get estimated time to empty in minutes.
 * @return Minutes to empty, or -1 on error.
 */
int bq27220_get_time_to_empty(void);

/**
 * @brief Get estimated time to full in minutes.
 * @return Minutes to full, or -1 on error.
 */
int bq27220_get_time_to_full(void);

/**
 * @brief Get charge/discharge cycle count.
 * @return Cycle count, or -1 on error.
 */
int bq27220_get_cycle_count(void);

/**
 * @brief Read the battery status register.
 * @param[out] status Pointer to status struct to fill.
 * @return OPRT_OK on success.
 */
OPERATE_RET bq27220_get_battery_status(bq27220_status_t *status);

/**
 * @brief Return true if battery is currently charging (current > 50 mA).
 */
BOOL_T bq27220_is_charging(void);

/**
 * @brief Return true if battery is fully charged (FC flag set).
 */
BOOL_T bq27220_is_fully_charged(void);

/**
 * @brief Set the design capacity (requires Unseal + ConfigUpdate cycle).
 *        A full charge cycle is needed for the gauge to recalibrate.
 * @param capacity_mah New design capacity in mAh.
 * @return OPRT_OK on success.
 */
OPERATE_RET bq27220_set_design_capacity(uint16_t capacity_mah);

/**
 * @brief Reset the fuel gauge learning data.
 * @return OPRT_OK on success.
 */
OPERATE_RET bq27220_reset_learning(void);

/**
 * @brief Print a full battery status summary via PR_INFO logs.
 *        Safe to call any time after bq27220_init() succeeds.
 */
void bq27220_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* __BQ27220_H__ */
