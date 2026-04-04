/**
 * @file bq27220.c
 * @brief BQ27220 single-cell Li-ion fuel gauge driver for TuyaOpen / T5AI board.
 *
 * Hardware connection: P42 = IIC1_SCL, P43 = IIC1_SDA (TUYA_I2C_NUM_1)
 * I2C address: 0x55 (fixed, not configurable on BQ27220)
 *
 * All multi-byte registers are little-endian.
 * Control sub-command responses are read back from CMD_MAC_DATA (0x40) after
 * a 15 ms settling delay.
 */

#include "bq27220.h"

#include "tkl_gpio.h"
#include "tal_log.h"
#include "tal_system.h"

/***********************************************************
************************macro define************************
***********************************************************/

/* Standard commands */
#define CMD_CONTROL              0x00
#define CMD_TEMPERATURE          0x06
#define CMD_VOLTAGE              0x08
#define CMD_BATTERY_STATUS       0x0A
#define CMD_CURRENT              0x0C
#define CMD_REMAINING_CAPACITY   0x10
#define CMD_FULL_CHARGE_CAPACITY 0x12
#define CMD_AVERAGE_CURRENT      0x14
#define CMD_TIME_TO_EMPTY        0x16
#define CMD_TIME_TO_FULL         0x18
#define CMD_STANDBY_CURRENT      0x1A
#define CMD_MAX_LOAD_CURRENT     0x1E
#define CMD_AVERAGE_POWER        0x24
#define CMD_CYCLE_COUNT          0x2A
#define CMD_STATE_OF_CHARGE      0x2C
#define CMD_STATE_OF_HEALTH      0x2E
#define CMD_DESIGN_CAPACITY      0x3C
#define CMD_MAC_DATA             0x40

/* Control sub-commands */
#define CTRL_DEVICE_NUMBER         0x0001
#define CTRL_FW_VERSION            0x0002
#define CTRL_HW_VERSION            0x0003
#define CTRL_SEAL                  0x0030
#define CTRL_RESET                 0x0041
#define CTRL_ENTER_CFG_UPDATE      0x0090
#define CTRL_EXIT_CFG_UPDATE_REINIT 0x0091

/* Data Memory addresses (for WriteDataMemory) */
#define DM_FULL_CHARGE_CAPACITY    0x929D
#define DM_DESIGN_CAPACITY         0x929F
#define DM_DESIGN_ENERGY           0x92A1

/* Default unseal keys */
#define UNSEAL_KEY1                0x0414
#define UNSEAL_KEY2                0x3672

/* Expected device ID returned by CTRL_DEVICE_NUMBER */
#define BQ27220_DEVICE_ID          0x0220

/* Charging current threshold to distinguish noise (mA) */
#define CHARGING_THRESHOLD_MA      50

/***********************************************************
***********************variable define**********************
***********************************************************/
static BOOL_T s_initialized = FALSE;
static const char *s_last_bb_error = "none";

#define BQ27220_BB_DELAY_CYCLES 3000
#define BQ27220_BB_SCL_WAIT_CYCLES 30000

/***********************************************************
***********************function define**********************
***********************************************************/

static OPERATE_RET __read_reg16(uint8_t reg, uint16_t *out);

/* ------------------------------------------------------------------ */
/* Low-level I2C helpers                                               */
/* ------------------------------------------------------------------ */

static void __bb_delay(void)
{
    volatile uint32_t count = BQ27220_BB_DELAY_CYCLES;
    while (count--) {
    }
}

static void __bb_scl_release(void)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };
    tkl_gpio_init(BQ27220_SCL_PIN, &cfg);
}

static void __bb_scl_low(void)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_LOW,
    };
    tkl_gpio_init(BQ27220_SCL_PIN, &cfg);
}

static void __bb_sda_release(void)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };
    tkl_gpio_init(BQ27220_SDA_PIN, &cfg);
}

static void __bb_sda_low(void)
{
    TUYA_GPIO_BASE_CFG_T cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_LOW,
    };
    tkl_gpio_init(BQ27220_SDA_PIN, &cfg);
}

static TUYA_GPIO_LEVEL_E __bb_sda_read(void)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    tkl_gpio_read(BQ27220_SDA_PIN, &level);
    return level;
}

static TUYA_GPIO_LEVEL_E __bb_scl_read(void)
{
    TUYA_GPIO_LEVEL_E level = TUYA_GPIO_LEVEL_HIGH;
    tkl_gpio_read(BQ27220_SCL_PIN, &level);
    return level;
}

static BOOL_T __bb_wait_scl_high(void)
{
    volatile uint32_t count = BQ27220_BB_SCL_WAIT_CYCLES;
    while ((__bb_scl_read() == TUYA_GPIO_LEVEL_LOW) && (count-- > 0)) {
    }
    return (__bb_scl_read() == TUYA_GPIO_LEVEL_HIGH) ? TRUE : FALSE;
}

static void __bb_start(void)
{
    __bb_sda_release();
    __bb_scl_release();
    __bb_wait_scl_high();
    __bb_delay();
    __bb_sda_low();
    __bb_delay();
    __bb_scl_low();
    __bb_delay();
}

static void __bb_stop(void)
{
    __bb_sda_low();
    __bb_delay();
    __bb_scl_release();
    __bb_wait_scl_high();
    __bb_delay();
    __bb_sda_release();
    __bb_delay();
}

static BOOL_T __bb_write_byte(uint8_t value)
{
    for (uint8_t bit = 0; bit < 8; bit++) {
        if (value & 0x80) {
            __bb_sda_release();
        } else {
            __bb_sda_low();
        }
        __bb_delay();
        __bb_scl_release();
        __bb_wait_scl_high();
        __bb_delay();
        __bb_scl_low();
        __bb_delay();
        value <<= 1;
    }

    __bb_sda_release();
    __bb_delay();
    __bb_scl_release();
    __bb_wait_scl_high();
    __bb_delay();
    BOOL_T ack = (__bb_sda_read() == TUYA_GPIO_LEVEL_LOW);
    __bb_scl_low();
    __bb_delay();
    return ack;
}

static uint8_t __bb_read_byte(BOOL_T ack)
{
    uint8_t value = 0;

    __bb_sda_release();
    for (uint8_t bit = 0; bit < 8; bit++) {
        value <<= 1;
        __bb_delay();
        __bb_scl_release();
        __bb_wait_scl_high();
        __bb_delay();
        if (__bb_sda_read() == TUYA_GPIO_LEVEL_HIGH) {
            value |= 1;
        }
        __bb_scl_low();
        __bb_delay();
    }

    if (ack) {
        __bb_sda_low();
    } else {
        __bb_sda_release();
    }
    __bb_delay();
    __bb_scl_release();
    __bb_wait_scl_high();
    __bb_delay();
    __bb_scl_low();
    __bb_delay();
    __bb_sda_release();

    return value;
}

static OPERATE_RET __bb_probe_addr(uint8_t addr)
{
    __bb_start();
    BOOL_T ack = __bb_write_byte((uint8_t)(addr << 1));
    __bb_stop();
    PR_DEBUG("bq27220: [bb] probe addr byte ack=%d", ack ? 1 : 0);
    return ack ? OPRT_OK : OPRT_COM_ERROR;
}

static OPERATE_RET __bb_write(const uint8_t *data, uint32_t len, BOOL_T ignore_last_nack)
{
    __bb_start();
    if (!__bb_write_byte((uint8_t)(BQ27220_I2C_ADDR << 1))) {
        __bb_stop();
        return OPRT_COM_ERROR;
    }

    for (uint32_t index = 0; index < len; index++) {
        BOOL_T ack = __bb_write_byte(data[index]);
        if (!ack) {
            BOOL_T allow_nack = (ignore_last_nack && (index == len - 1));
            __bb_stop();
            return allow_nack ? OPRT_OK : OPRT_COM_ERROR;
        }
    }

    __bb_stop();
    return OPRT_OK;
}

static OPERATE_RET __bb_read_reg16(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2] = {0, 0};

    __bb_start();
    BOOL_T addr_w_ack = __bb_write_byte((uint8_t)(BQ27220_I2C_ADDR << 1));
    if (!addr_w_ack) {
        s_last_bb_error = "addr+W NACK";
        PR_ERR("bq27220: [bb] reg 0x%02X addr+W NACK", reg);
        __bb_stop();
        return OPRT_COM_ERROR;
    }
    BOOL_T reg_ack = __bb_write_byte(reg);
    if (!reg_ack) {
        s_last_bb_error = "command byte NACK";
        PR_ERR("bq27220: [bb] reg 0x%02X command byte NACK", reg);
        __bb_stop();
        return OPRT_COM_ERROR;
    }

    __bb_start();
    BOOL_T addr_r_ack = __bb_write_byte((uint8_t)((BQ27220_I2C_ADDR << 1) | 1));
    if (!addr_r_ack) {
        s_last_bb_error = "addr+R NACK";
        PR_ERR("bq27220: [bb] reg 0x%02X addr+R NACK", reg);
        __bb_stop();
        return OPRT_COM_ERROR;
    }

    buf[0] = __bb_read_byte(TRUE);
    buf[1] = __bb_read_byte(FALSE);
    __bb_stop();

        s_last_bb_error = "none";
        PR_DEBUG("bq27220: [bb] reg 0x%02X addrW=%d reg=%d addrR=%d raw=[0x%02X,0x%02X]",
                 reg, addr_w_ack ? 1 : 0, reg_ack ? 1 : 0, addr_r_ack ? 1 : 0, buf[0], buf[1]);

    *out = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    return OPRT_OK;
}

static void __dump_basic_regs(void)
{
    static const struct {
        uint8_t reg;
        const char *name;
    } reg_list[] = {
        {CMD_CONTROL, "Control"},
        {CMD_TEMPERATURE, "Temperature"},
        {CMD_VOLTAGE, "Voltage"},
        {CMD_BATTERY_STATUS, "BatteryStatus"},
        {CMD_CURRENT, "Current"},
        {CMD_STATE_OF_CHARGE, "StateOfCharge"},
        {0x3A, "OperationStatus"},
    };

    for (unsigned int index = 0; index < sizeof(reg_list) / sizeof(reg_list[0]); index++) {
        uint16_t value = 0;
        OPERATE_RET rt = __read_reg16(reg_list[index].reg, &value);
        PR_DEBUG("bq27220: [dump] %-16s reg=0x%02X val=0x%04X rt=%d",
                 reg_list[index].name, reg_list[index].reg, value, rt);
    }
}

/**
 * @brief Read a 16-bit little-endian register from BQ27220.
 *        Uses repeated-start: WRITE(reg) → RESTART → READ(2 bytes).
 *
 * @param reg  Register address.
 * @param[out] out  Pointer to receive the 16-bit value.
 * @return OPRT_OK on success.
 */
static OPERATE_RET __read_reg16(uint8_t reg, uint16_t *out)
{
    OPERATE_RET rt;
    uint8_t buf[2] = {0, 0};

    /* BQ27220 follows the SMBus register-read format: write command code,
     * then repeated-start into read. Using STOP+START on this chip produced
     * stable garbage values like 0xA4A4, so prefer the standard combined
     * transaction here. */
    rt = __bb_read_reg16(reg, out);
    PR_DEBUG("bq27220: send reg=0x%02X xfer_pending=1 rt=%d", reg, rt);
    if (rt != OPRT_OK) {
        PR_ERR("bq27220: i2c send reg 0x%02X failed: %d (%s)", reg, rt, s_last_bb_error);
        return rt;
    }
    buf[0] = (uint8_t)(*out & 0xFF);
    buf[1] = (uint8_t)((*out >> 8) & 0xFF);
    PR_DEBUG("bq27220: recv reg=0x%02X rt=%d raw=[0x%02X, 0x%02X]", reg, rt, buf[0], buf[1]);
    return OPRT_OK;
}

/**
 * @brief Send a raw byte buffer to BQ27220 (no repeated-start).
 *
 * NOTE: BQ27220 NAKs the ACK slot after the last byte of a Control() command
 * (matches Flipper Zero behavior — their i2c_write_mem does not check per-byte
 * ACK either). We therefore ignore a write failure and only treat it as a
 * diagnostic warning so the caller can proceed to read the response.
 */
static OPERATE_RET __write_raw(const uint8_t *data, uint32_t len)
{
    OPERATE_RET rt;
    if (len >= 3) {
        PR_DEBUG("bq27220: write len=%lu [0x%02X, 0x%02X, 0x%02X ...]",
                 (unsigned long)len, data[0], data[1], data[2]);
    } else if (len == 2) {
        PR_DEBUG("bq27220: write len=2 [0x%02X, 0x%02X]", data[0], data[1]);
    } else if (len == 1) {
        PR_DEBUG("bq27220: write len=1 [0x%02X]", data[0]);
    }
    rt = __bb_write(data, len, (len >= 3 && data[0] == CMD_CONTROL) ? TRUE : FALSE);
    PR_DEBUG("bq27220: write rt=%d%s", rt, rt != OPRT_OK ? " (ignored – BQ27220 NAKs last byte)" : "");
    /* Do not propagate write error: BQ27220 frequently NAKs the ACK of the
     * last byte of a 3-byte Control() write. The command is still latched
     * internally. Flipper Zero's reference driver does not check per-byte ACK
     * at all. */
    return OPRT_OK;
}

/**
 * @brief Send a Control sub-command and read back the 16-bit response
 *        from CMD_MAC_DATA after a settling delay.
 */
static OPERATE_RET __control_cmd(uint16_t sub_cmd, uint16_t *response)
{
    uint8_t cmd_buf[3];
    cmd_buf[0] = CMD_CONTROL;
    cmd_buf[1] = (uint8_t)(sub_cmd & 0xFF);
    cmd_buf[2] = (uint8_t)((sub_cmd >> 8) & 0xFF);

    PR_DEBUG("bq27220: ctrl_cmd sub=0x%04X → [0x%02X, 0x%02X, 0x%02X]",
             sub_cmd, cmd_buf[0], cmd_buf[1], cmd_buf[2]);

    OPERATE_RET rt = __write_raw(cmd_buf, 3);
    if (rt != OPRT_OK) {
        PR_ERR("bq27220: ctrl_cmd 0x%04X write failed: %d", sub_cmd, rt);
        return rt;
    }

    /* Flipper Zero reference: BQ27220_SELECT_DELAY_US = 1000 µs.
     * bqStudio uses ~15 ms. Use 50 ms to be safe on this platform. */
    tal_system_sleep(50);

    if (response) {
        /* Control() sub-command responses are read back from 0x40 (CommandMACData).
         * Confirmed by Flipper Zero driver: bq27220_read_word(handle, CommandMACData). */
        rt = __read_reg16(CMD_MAC_DATA, response);
        if (rt == OPRT_OK) {
            PR_DEBUG("bq27220: ctrl_cmd 0x%04X response=0x%04X", sub_cmd, *response);
        }
        return rt;
    }
    return OPRT_OK;
}

/**
 * @brief Send a Control sub-command without reading a response.
 */
static OPERATE_RET __control_cmd_no_read(uint16_t sub_cmd)
{
    return __control_cmd(sub_cmd, NULL);
}

/* ------------------------------------------------------------------ */
/* Configuration helpers (Unseal / ConfigUpdate / DataMemory)          */
/* ------------------------------------------------------------------ */

static OPERATE_RET __unseal(void)
{
    PR_DEBUG("bq27220: unsealing...");
    OPERATE_RET rt;
    rt = __control_cmd_no_read(UNSEAL_KEY1);
    if (rt != OPRT_OK) return rt;
    rt = __control_cmd_no_read(UNSEAL_KEY2);
    if (rt != OPRT_OK) return rt;
    tal_system_sleep(100);
    PR_DEBUG("bq27220: unsealed");
    return OPRT_OK;
}

static OPERATE_RET __seal(void)
{
    PR_DEBUG("bq27220: sealing...");
    OPERATE_RET rt = __control_cmd_no_read(CTRL_SEAL);
    tal_system_sleep(100);
    PR_DEBUG("bq27220: sealed");
    return rt;
}

static OPERATE_RET __enter_cfg_update(void)
{
    PR_DEBUG("bq27220: entering config update mode...");
    OPERATE_RET rt = __control_cmd_no_read(CTRL_ENTER_CFG_UPDATE);
    tal_system_sleep(1000);
    PR_DEBUG("bq27220: config update mode active");
    return rt;
}

static OPERATE_RET __exit_cfg_update(void)
{
    PR_DEBUG("bq27220: exiting config update mode (reinit)...");
    OPERATE_RET rt = __control_cmd_no_read(CTRL_EXIT_CFG_UPDATE_REINIT);
    tal_system_sleep(1000);
    PR_DEBUG("bq27220: config update mode exited");
    return rt;
}

/**
 * @brief Write data to BQ27220 Data Memory.
 *
 * Protocol:
 *   1. Write [0x3E, addr_lo, addr_hi, data...] to SelectSubclass.
 *   2. Write checksum + length to MACDataSum (0x60):
 *      checksum = 0xFF - (addr_lo + addr_hi + sum(data))
 *      length   = 2 + len(data) + 1 + 1
 */
static OPERATE_RET __write_data_memory(uint16_t addr, const uint8_t *data, uint8_t len)
{
    /* Build SelectSubclass packet: [0x3E, addr_lo, addr_hi, data...] */
    uint8_t buf[64];
    if (len + 3 > (int)sizeof(buf)) {
        return OPRT_INVALID_PARM;
    }
    buf[0] = 0x3E;
    buf[1] = (uint8_t)(addr & 0xFF);
    buf[2] = (uint8_t)((addr >> 8) & 0xFF);
    for (uint8_t i = 0; i < len; i++) {
        buf[3 + i] = data[i];
    }

    OPERATE_RET rt = __write_raw(buf, (uint32_t)(len + 3));
    if (rt != OPRT_OK) return rt;
    tal_system_sleep(10);

    /* Checksum = 0xFF - (addr_lo + addr_hi + sum of data bytes) */
    uint8_t checksum = 0;
    checksum += buf[1];
    checksum += buf[2];
    for (uint8_t i = 0; i < len; i++) {
        checksum += data[i];
    }
    checksum = 0xFF - checksum;

    uint8_t sum_buf[3];
    sum_buf[0] = 0x60; /* MACDataSum register */
    sum_buf[1] = checksum;
    sum_buf[2] = (uint8_t)(len + 4); /* 2(addr) + len + 1(cksum) + 1(length) */

    rt = __write_raw(sum_buf, 3);
    tal_system_sleep(10);
    return rt;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

OPERATE_RET bq27220_init(void)
{
    OPERATE_RET rt;

    PR_DEBUG("bq27220: init start (P42=SCL P43=SDA gpio bit-bang)");

    __bb_scl_release();
    __bb_sda_release();
    tal_system_sleep(1);

    /* Address is fixed at 0x55, so avoid a full bus scan here.
     * Probing every address on this software I2C implementation produces many
     * expected NAK timeouts and can disturb the gauge state machine. */
    {
        OPERATE_RET probe_rt = __bb_probe_addr(BQ27220_I2C_ADDR);
        PR_DEBUG("bq27220: probe addr=0x%02X rt=%d", BQ27220_I2C_ADDR, probe_rt);
        if (probe_rt != OPRT_OK) {
            PR_ERR("bq27220: probe failed at 0x%02X", BQ27220_I2C_ADDR);
            tkl_gpio_deinit(BQ27220_SCL_PIN);
            tkl_gpio_deinit(BQ27220_SDA_PIN);
            return probe_rt;
        }
    }

    tal_system_sleep(10);

    /* === Diagnostic read: Voltage (0x08) and SoC (0x2C) ===
     * These are standard registers that should always return data regardless
     * of INITCOMP state. If they return non-zero, I2C reads work.
     * If they return 0x0000, either I2C reads are broken or chip is in POR. */
    {
        uint16_t diag_volt = 0, diag_soc = 0;
        OPERATE_RET v_rt = __read_reg16(CMD_VOLTAGE, &diag_volt);
        OPERATE_RET s_rt = __read_reg16(CMD_STATE_OF_CHARGE, &diag_soc);
        PR_DEBUG("bq27220: [diag] Voltage=0x%04X (%dmV) rt=%d  SoC=0x%04X (%d%%) rt=%d",
                 diag_volt, (int)diag_volt, v_rt, diag_soc, (int)diag_soc, s_rt);
        if (diag_volt == 0 && diag_soc == 0) {
            PR_WARN("bq27220: [diag] Voltage and SoC both 0 – chip may be in POR/shutdown state");
            __dump_basic_regs();
        } else if (diag_volt > 2000 && diag_volt < 5000) {
            PR_INFO("bq27220: [diag] Voltage %dmV looks valid – battery is connected", (int)diag_volt);
        }
    }

    /* Read OperationStatus (0x3A) to check INITCOMP bit (bit5) and SEC bits (bits[2:1]).
     * INITCOMP=0 means the gauge has NOT entered Normal mode yet.
     * Per BQ27220 TRM: "INITCOMP is set when gauge enters Normal mode.
     *  Prior to Normal mode, the gauge MUST sense a battery voltage."
     * Without a battery connected, INITCOMP stays 0 indefinitely and
     * CommandMACData (0x40) is inaccessible (device NAKs it). */
    {
        uint16_t op_status = 0;
        OPERATE_RET os_rt = __read_reg16(0x3A, &op_status);
        int initcomp = (op_status >> 5) & 1;   /* bit5 = INITCOMP */
        int sec      = (op_status >> 1) & 3;   /* bits[2:1] = SEC */
        PR_DEBUG("bq27220: OperationStatus 0x3A = 0x%04X rt=%d (INITCOMP=%d SEC=%d)",
                 op_status, os_rt, initcomp, sec);

        if (os_rt != OPRT_OK) {
            PR_ERR("bq27220: failed to read OperationStatus (rt=%d) – chip unresponsive after scan", os_rt);
            tkl_gpio_deinit(BQ27220_SCL_PIN);
            tkl_gpio_deinit(BQ27220_SDA_PIN);
            return os_rt;
        }

        if (!initcomp) {
            PR_WARN("bq27220: INITCOMP=0 – gauge not in Normal mode.");
            PR_WARN("bq27220: Possible causes:");
            PR_WARN("bq27220:  1. Battery NOT connected to BQ27220 BAT+ pin");
            PR_WARN("bq27220:  2. Battery voltage too low (<2.8V)");
            PR_WARN("bq27220:  3. BQ27220 still in POR/init state – check [diag] Voltage log above");
            PR_WARN("bq27220: MAC data (0x40) inaccessible – skipping device ID check.");
            /* Mark initialized so callers can still call get_*() without crashing. */
            s_initialized = TRUE;
            return OPRT_OK;
        }
    }

    /* Send CTRL_DEVICE_NUMBER control command.
     * Per Flipper Zero reference driver: write sub-cmd to 0x00, wait, read
     * response from 0x40 (CommandMACData). */
    PR_DEBUG("bq27220: sending CTRL_DEVICE_NUMBER (0x0001)");

    /* Read 0x00 before sending to see current control status */
    {
        uint16_t ctrl_before = 0;
        __read_reg16(CMD_CONTROL, &ctrl_before);
        PR_DEBUG("bq27220: ControlStatus (0x00) before cmd = 0x%04X", ctrl_before);
    }

    uint16_t device_id = 0;
    rt = __control_cmd(CTRL_DEVICE_NUMBER, &device_id);

    /* Also read back from 0x00 for comparison */
    {
        uint16_t ctrl_after = 0;
        __read_reg16(CMD_CONTROL, &ctrl_after);
        PR_DEBUG("bq27220: ControlStatus (0x00) after cmd = 0x%04X (should echo 0x0001)", ctrl_after);
    }

    PR_DEBUG("bq27220: device_id from 0x40 = 0x%04X (expected 0x%04X)", device_id, BQ27220_DEVICE_ID);

    if (rt != OPRT_OK) {
        PR_ERR("bq27220: failed to read device ID (rt=%d)", rt);
        tkl_gpio_deinit(BQ27220_SCL_PIN);
        tkl_gpio_deinit(BQ27220_SDA_PIN);
        return rt;
    }
    if (device_id != BQ27220_DEVICE_ID) {
        PR_ERR("bq27220: unexpected device ID 0x%04X (expected 0x%04X)",
               device_id, BQ27220_DEVICE_ID);
        tkl_gpio_deinit(BQ27220_SCL_PIN);
        tkl_gpio_deinit(BQ27220_SDA_PIN);
        return OPRT_COM_ERROR;
    } else {
        PR_DEBUG("bq27220: device ID OK 0x%04X", device_id);
    }

    uint16_t fw = 0, hw = 0;
    __control_cmd(CTRL_FW_VERSION, &fw);
    __control_cmd(CTRL_HW_VERSION, &hw);
    PR_DEBUG("bq27220: FW=0x%04X HW=0x%04X", fw, hw);

    s_initialized = TRUE;

    {
        int design_capacity = bq27220_get_design_capacity();
        if (design_capacity < 0) {
            PR_WARN("bq27220: failed to read design capacity after init");
        } else if (design_capacity != BQ27220_DEFAULT_DESIGN_CAPACITY_MAH) {
            PR_WARN("bq27220: design capacity is %d mAh, syncing to %d mAh",
                    design_capacity, BQ27220_DEFAULT_DESIGN_CAPACITY_MAH);
            rt = bq27220_set_design_capacity(BQ27220_DEFAULT_DESIGN_CAPACITY_MAH);
            if (rt != OPRT_OK) {
                PR_WARN("bq27220: failed to sync design capacity (rt=%d)", rt);
            }
        } else {
            PR_INFO("bq27220: design capacity confirmed at %d mAh",
                    BQ27220_DEFAULT_DESIGN_CAPACITY_MAH);
        }
    }

    bq27220_print_status();

    return OPRT_OK;
}

void bq27220_print_status(void)
{
    if (!s_initialized) {
        PR_WARN("bq27220: not initialized");
        return;
    }

    int soc       = bq27220_get_soc();
    int voltage   = bq27220_get_voltage();
    int current   = bq27220_get_current();
    int temp      = bq27220_get_temperature();
    int remaining = bq27220_get_remaining_capacity();
    int full      = bq27220_get_full_capacity();
    int soh       = bq27220_get_soh();
    int cycles    = bq27220_get_cycle_count();
    int tte       = bq27220_get_time_to_empty();

        PR_INFO("[BQ27220] SOC: %d pct  Voltage: %d mV  Current: %d mA  Temp: %d C",
            soc, voltage, current, temp);
        if (tte < 0) {
        PR_INFO("[BQ27220] Remaining: %d mAh / %d mAh  SOH: %d pct  Cycles: %d  TTE: N/A",
            remaining, full, soh, cycles);
        } else {
        PR_INFO("[BQ27220] Remaining: %d mAh / %d mAh  SOH: %d pct  Cycles: %d  TTE: %d min",
            remaining, full, soh, cycles, tte);
        }
    PR_INFO("[BQ27220] Status: %s",
            bq27220_is_charging() ? "CHARGING" :
            bq27220_is_fully_charged() ? "FULL" : "DISCHARGING");
}

void bq27220_deinit(void)
{
    if (s_initialized) {
        tkl_gpio_deinit(BQ27220_SCL_PIN);
        tkl_gpio_deinit(BQ27220_SDA_PIN);
        s_initialized = FALSE;
    }
}

int bq27220_get_soc(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_STATE_OF_CHARGE, &val) != OPRT_OK) return -1;
    return (val > 100) ? 100 : (int)val;
}

int bq27220_get_voltage(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_VOLTAGE, &val) != OPRT_OK) return -1;
    return (int)val;
}

int bq27220_get_current(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_CURRENT, &val) != OPRT_OK) return (int)INT16_MIN;
    return (int)(int16_t)val;
}

int bq27220_get_temperature(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_TEMPERATURE, &val) != OPRT_OK) return (int)INT16_MIN;
    /* Register unit: 0.1 K — convert to °C */
    return (int)((int)(val / 10) - 273);
}

int bq27220_get_remaining_capacity(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_REMAINING_CAPACITY, &val) != OPRT_OK) return -1;
    return (int)val;
}

int bq27220_get_full_capacity(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_FULL_CHARGE_CAPACITY, &val) != OPRT_OK) return -1;
    return (int)val;
}

int bq27220_get_design_capacity(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_DESIGN_CAPACITY, &val) != OPRT_OK) return -1;
    return (int)val;
}

int bq27220_get_soh(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_STATE_OF_HEALTH, &val) != OPRT_OK) return -1;
    return (val > 100) ? 100 : (int)val;
}

int bq27220_get_average_power(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_AVERAGE_POWER, &val) != OPRT_OK) return (int)INT16_MIN;
    return (int)(int16_t)val;
}

int bq27220_get_time_to_empty(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_TIME_TO_EMPTY, &val) != OPRT_OK) return -1;
    if (val == 0xFFFF) return -1;
    return (int)val;
}

int bq27220_get_time_to_full(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_TIME_TO_FULL, &val) != OPRT_OK) return -1;
    if (val == 0xFFFF) return -1;
    return (int)val;
}

int bq27220_get_cycle_count(void)
{
    uint16_t val = 0;
    if (__read_reg16(CMD_CYCLE_COUNT, &val) != OPRT_OK) return -1;
    return (int)val;
}

OPERATE_RET bq27220_get_battery_status(bq27220_status_t *status)
{
    if (!status) return OPRT_INVALID_PARM;
    uint16_t val = 0;
    OPERATE_RET rt = __read_reg16(CMD_BATTERY_STATUS, &val);
    if (rt != OPRT_OK) return rt;
    /* Overlay raw register value onto the bitfield struct */
    *((uint16_t *)status) = val;
    return OPRT_OK;
}

BOOL_T bq27220_is_charging(void)
{
    return (bq27220_get_current() > CHARGING_THRESHOLD_MA) ? TRUE : FALSE;
}

BOOL_T bq27220_is_fully_charged(void)
{
    bq27220_status_t status = {0};
    if (bq27220_get_battery_status(&status) != OPRT_OK) return FALSE;
    return status.fc ? TRUE : FALSE;
}

OPERATE_RET bq27220_set_design_capacity(uint16_t capacity_mah)
{
    OPERATE_RET rt;

    PR_DEBUG("bq27220: setting design capacity to %u mAh...", capacity_mah);

    rt = __unseal();
    if (rt != OPRT_OK) return rt;

    rt = __enter_cfg_update();
    if (rt != OPRT_OK) { __seal(); return rt; }

    /* BQ27220 Data Memory expects big-endian byte order */
    uint8_t cap_data[2];
    cap_data[0] = (uint8_t)((capacity_mah >> 8) & 0xFF);
    cap_data[1] = (uint8_t)(capacity_mah & 0xFF);

    rt = __write_data_memory(DM_FULL_CHARGE_CAPACITY, cap_data, 2);
    if (rt != OPRT_OK) { __exit_cfg_update(); __seal(); return rt; }

    rt = __write_data_memory(DM_DESIGN_CAPACITY, cap_data, 2);
    if (rt != OPRT_OK) { __exit_cfg_update(); __seal(); return rt; }

    /* Design energy = capacity × 3.7 V nominal (unit: mWh) */
    uint16_t design_energy = (uint16_t)(((uint32_t)capacity_mah * 37) / 10);
    uint8_t energy_data[2];
    energy_data[0] = (uint8_t)((design_energy >> 8) & 0xFF);
    energy_data[1] = (uint8_t)(design_energy & 0xFF);

    rt = __write_data_memory(DM_DESIGN_ENERGY, energy_data, 2);
    if (rt != OPRT_OK) { __exit_cfg_update(); __seal(); return rt; }

    rt = __exit_cfg_update();
    if (rt != OPRT_OK) { __seal(); return rt; }

    __seal();

    tal_system_sleep(100);
    int verified = bq27220_get_design_capacity();
    if (verified == (int)capacity_mah) {
        PR_DEBUG("bq27220: design capacity verified: %d mAh", verified);
    } else {
        PR_WARN("bq27220: capacity mismatch — expected %u got %d (may need power cycle)", capacity_mah, verified);
    }
    PR_DEBUG("bq27220: full charge cycle needed for gauge to recalibrate");

    return OPRT_OK;
}

OPERATE_RET bq27220_reset_learning(void)
{
    PR_DEBUG("bq27220: resetting fuel gauge learning...");

    OPERATE_RET rt = __unseal();
    if (rt != OPRT_OK) return rt;

    rt = __control_cmd_no_read(CTRL_RESET);
    tal_system_sleep(500);

    __seal();

    PR_DEBUG("bq27220: fuel gauge reset complete");
    return rt;
}
