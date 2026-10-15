/*
 * Mini SMU - firmware for RP2040
 *
 * Sources a programmable voltage (MCP4725 12-bit DAC + op-amp buffer) and
 * measures DUT current/voltage (INA226 + 10 ohm shunt) over I2C.
 * Commands are line-based ASCII over USB CDC. See README for the protocol.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

/* ---------- Board configuration ---------- */
#define I2C_PORT        i2c0
#define PIN_SDA         4
#define PIN_SCL         5
#define I2C_BAUD_HZ     400000

#define DAC_ADDR        0x60      /* MCP4725 (A0 = GND) */
#define INA_ADDR        0x40      /* INA226  (A0 = A1 = GND) */

#define VREF_MV         3300.0f   /* DAC reference = 3V3 rail */
#define DAC_MAX_CODE    4095
#define SHUNT_OHMS      10.0f
#define COMPLIANCE_UA   7500.0f   /* software current limit (INA226 full scale = 8192 uA) */
#define MAX_SWEEP_PTS   2000

/* ---------- INA226 registers ---------- */
#define INA_REG_CONFIG  0x00
#define INA_REG_SHUNT   0x01
#define INA_REG_BUS     0x02
#define INA_REG_MFG_ID  0xFE
#define INA_REG_DIE_ID  0xFF
/* AVG=16, VBUSCT=1.1ms, VSHCT=1.1ms, continuous shunt+bus */
#define INA_CONFIG_VAL  0x4527

#define INA_SHUNT_LSB_UV 2.5f
#define INA_BUS_LSB_MV   1.25f

static bool ina_write_reg(uint8_t reg, uint16_t val) {
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_write_blocking(I2C_PORT, INA_ADDR, buf, 3, false) == 3;
}

static bool ina_read_reg(uint8_t reg, uint16_t *val) {
    uint8_t buf[2];
    if (i2c_write_blocking(I2C_PORT, INA_ADDR, &reg, 1, true) != 1) return false;
    if (i2c_read_blocking(I2C_PORT, INA_ADDR, buf, 2, false) != 2) return false;
    *val = ((uint16_t)buf[0] << 8) | buf[1];
    return true;
}

static bool dac_set_code(uint16_t code) {
    if (code > DAC_MAX_CODE) code = DAC_MAX_CODE;
    /* Fast-mode write: [0 0 PD1 PD0 D11..D8][D7..D0], PD = 00 (normal) */
    uint8_t buf[2] = { (uint8_t)((code >> 8) & 0x0F), (uint8_t)(code & 0xFF) };
    return i2c_write_blocking(I2C_PORT, DAC_ADDR, buf, 2, false) == 2;
}

static bool dac_set_mv(float mv) {
    if (mv < 0) mv = 0;
    if (mv > VREF_MV) mv = VREF_MV;
    return dac_set_code((uint16_t)lroundf(mv * DAC_MAX_CODE / VREF_MV));
}

/* Returns DUT voltage (mV, node after shunt) and current (uA). */
static bool measure(float *v_dut_mv, float *i_ua) {
    uint16_t raw_sh, raw_bus;
    if (!ina_read_reg(INA_REG_SHUNT, &raw_sh)) return false;
    if (!ina_read_reg(INA_REG_BUS, &raw_bus)) return false;
    *i_ua = ((int16_t)raw_sh * INA_SHUNT_LSB_UV) / SHUNT_OHMS;  /* uV / ohm = uA */
    *v_dut_mv = raw_bus * INA_BUS_LSB_MV;
    return true;
}

static bool ina_init(void) {
    uint16_t mfg = 0, die = 0;
    if (!ina_read_reg(INA_REG_MFG_ID, &mfg) || mfg != 0x5449) return false;
    if (!ina_read_reg(INA_REG_DIE_ID, &die) || (die >> 4) != 0x226) return false;
    return ina_write_reg(INA_REG_CONFIG, INA_CONFIG_VAL);
}

static void cmd_sweep(const char *args) {
    int start, stop, step, settle_ms;
    if (sscanf(args, "%d %d %d %d", &start, &stop, &step, &settle_ms) != 4 || step == 0) {
        printf("ERR,USAGE: SWEEP <start_mV> <stop_mV> <step_mV> <settle_ms>\n");
        return;
    }
    if ((stop - start) * step < 0) step = -step;           /* step toward stop */
    int n = (stop - start) / step + 1;
    if (n > MAX_SWEEP_PTS) { printf("ERR,TOO_MANY_POINTS\n"); return; }

    printf("BEGIN,v_set_mV,v_dut_mV,i_uA\n");
    for (int k = 0; k < n; k++) {
        int v_set = start + k * step;
        float v, i;
        if (!dac_set_mv((float)v_set)) { printf("ERR,DAC_I2C\n"); break; }
        sleep_ms(settle_ms);
        if (!measure(&v, &i)) { printf("ERR,INA_I2C\n"); break; }
        if (fabsf(i) > COMPLIANCE_UA) {
            dac_set_mv(0);
            printf("ERR,COMPLIANCE,%d\n", v_set);
            break;
        }
        printf("%d,%.2f,%.3f\n", v_set, v, i);
    }
    dac_set_mv(0);
    printf("END\n");
}

static void handle_line(char *line) {
    if (strcmp(line, "ID?") == 0) {
        printf("MINI-SMU,fw1.0\n");
    } else if (strncmp(line, "SET ", 4) == 0) {
        float mv = strtof(line + 4, NULL);
        printf(dac_set_mv(mv) ? "OK\n" : "ERR,DAC_I2C\n");
    } else if (strcmp(line, "OFF") == 0) {
        printf(dac_set_mv(0) ? "OK\n" : "ERR,DAC_I2C\n");
    } else if (strcmp(line, "MEAS?") == 0) {
        float v, i;
        if (measure(&v, &i)) printf("%.2f,%.3f\n", v, i);
        else printf("ERR,INA_I2C\n");
    } else if (strncmp(line, "SWEEP ", 6) == 0) {
        cmd_sweep(line + 6);
    } else {
        printf("ERR,UNKNOWN_CMD\n");
    }
}

int main(void) {
    stdio_init_all();
    i2c_init(I2C_PORT, I2C_BAUD_HZ);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);

    dac_set_mv(0);                      /* always start with output at 0 V */
    bool ina_ok = ina_init();

    char line[64];
    size_t len = 0;
    while (true) {
        int c = getchar_timeout_us(100000);
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (c == '\r' || c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                if (!ina_ok && !(ina_ok = ina_init())) printf("ERR,INA_NOT_FOUND\n");
                else handle_line(line);
                len = 0;
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}
