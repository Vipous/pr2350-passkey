#include "buttons.h"

#include "DEV_Config.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "flash_store.h"

// AXP2101 power chip: PWR key short press shows up as an interrupt status bit.
#define AXP_ADDR       0x34
#define AXP_CHIP_ID    0x03   // reads 0x4A
#define AXP_INT_EN2    0x41
#define AXP_INT_STS2   0x49
#define AXP_PKEY_SHORT 0x08
#define I2C_TIMEOUT_US 2000

static bool have_axp;
static bool boot_was_down;

static bool axp_read(uint8_t reg, uint8_t *val) {
    if (i2c_write_timeout_us(I2C_PORT, AXP_ADDR, &reg, 1, true, I2C_TIMEOUT_US) != 1) return false;
    return i2c_read_timeout_us(I2C_PORT, AXP_ADDR, val, 1, false, I2C_TIMEOUT_US) == 1;
}

static bool axp_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_write_timeout_us(I2C_PORT, AXP_ADDR, buf, 2, false, I2C_TIMEOUT_US) == 2;
}

void buttons_init(void) {
    uint8_t id, en;
    have_axp = axp_read(AXP_CHIP_ID, &id) && id == 0x4A && axp_read(AXP_INT_EN2, &en) &&
               axp_write(AXP_INT_EN2, en | AXP_PKEY_SHORT) && axp_write(AXP_INT_STS2, AXP_PKEY_SHORT);
}

// The BOOT button pulls the flash chip select low. To see it, stop driving CS for a moment;
// nothing may run from flash meanwhile, so this runs from RAM with the other core paused.
static void __no_inline_not_in_flash_func(read_boot_cb)(void *param) {
    const uint cs = 1;
    hw_write_masked(&ioqspi_hw->io[cs].ctrl, GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i) {
    }
    *(bool *)param = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
    hw_write_masked(&ioqspi_hw->io[cs].ctrl, GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
}

bool buttons_pressed(void) {
    bool pressed = false;

    bool boot_down = false;
    if (flash_safe_call(read_boot_cb, &boot_down)) {
        if (boot_down && !boot_was_down) pressed = true;
        boot_was_down = boot_down;
    }

    uint8_t sts;
    if (have_axp && axp_read(AXP_INT_STS2, &sts) && (sts & AXP_PKEY_SHORT)) {
        axp_write(AXP_INT_STS2, AXP_PKEY_SHORT);   // write 1 to clear
        pressed = true;
    }
    return pressed;
}
