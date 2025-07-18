// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * msi-ec.c - MSI Embedded Controller for laptops support.
 *
 * This driver exports a few files in /sys/devices/platform/msi-laptop:
 *   webcam            Integrated webcam activation
 *   fn_key            Function key location
 *   win_key           Windows key location
 *   battery_mode      Battery health options
 *   cooler_boost      Cooler boost function
 *   shift_mode        CPU & GPU performance modes
 *   fan_mode          FAN performance modes
 *   fw_version        Firmware version
 *   fw_release_date   Firmware release date
 *   cpu/..            CPU related options
 *   gpu/..            GPU related options
 *
 * In addition to these platform device attributes the driver
 * registers itself in the Linux power_supply subsystem and is
 * available to userspace under /sys/class/power_supply/<power_supply>:
 *
 *   charge_control_start_threshold
 *   charge_control_end_threshold
 * 
 * This driver also registers available led class devices for
 * mute, micmute and keyboard_backlight leds
 *
 * This driver might not work on other laptops produced by MSI. Also, and until
 * future enhancements, no DMI data are used to identify your compatibility
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "ec_memory_configuration.h"

#include <acpi/battery.h>
#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/version.h>

static DEFINE_MUTEX(ec_set_by_mask_mutex);
static DEFINE_MUTEX(ec_unset_by_mask_mutex);
static DEFINE_MUTEX(ec_set_bit_mutex);

#define SM_ECO_NAME		"eco"
#define SM_COMFORT_NAME		"comfort"
#define SM_SPORT_NAME		"sport"
#define SM_TURBO_NAME		"turbo"

#define FM_AUTO_NAME		"auto"
#define FM_SILENT_NAME		"silent"
#define FM_BASIC_NAME		"basic"
#define FM_ADVANCED_NAME	"advanced"

static const char *ALLOWED_FW_0[] __initconst = {
	"14C1EMS1.012", // Prestige 14 A10SC
	"14C1EMS1.101",
	"14C1EMS1.102",
	NULL
};

static struct msi_ec_conf CONF0 __initdata = {
	.allowed_fw = ALLOWED_FW_0, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNKNOWN, // 0xd5 needs testing
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_BASIC_NAME,    0x4d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = 0x89,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2c,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_1[] __initconst = {
	"17F2EMS1.103", // GF75 Thin 9SC
	"17F2EMS1.104",
	"17F2EMS1.106",
	"17F2EMS1.107",
	NULL
};

static struct msi_ec_conf CONF1 __initdata = {
	.allowed_fw = ALLOWED_FW_1, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNKNOWN,
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_BASIC_NAME,    0x4d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = 0x89,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2c,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_2[] __initconst = {
	"1552EMS1.115", // Modern 15 A11M
	"1552EMS1.118",
	"1552EMS1.119",
	"1552EMS1.120",
	NULL
};

static struct msi_ec_conf CONF2 __initdata = {
	.allowed_fw = ALLOWED_FW_2, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xD2, // because WMI2 device
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_BASIC_NAME,    0x4d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = 0x89,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c, // ?
		.bl_modes         = { 0x00, 0x08 }, // ?
		.max_mode         = 1, // ?
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_3[] __initconst = {
	"1592EMS1.111", // Summit E16 Flip A12UCT / A12MT
	NULL
};

static struct msi_ec_conf CONF3 __initdata = {
	.allowed_fw = ALLOWED_FW_3, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_BASIC_NAME,    0x4d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0xc9,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = 0x89,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2c,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_4[] __initconst = {
	"16V4EMS1.114", // GS66 Stealth 11UE
	NULL
};

static struct msi_ec_conf CONF4 __initdata = {
	.allowed_fw = ALLOWED_FW_4, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = MSI_EC_ADDR_UNKNOWN, // supported, but unknown
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = { // may be supported, but address is unknown
		.address = MSI_EC_ADDR_UNKNOWN,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68, // needs testing
		.rt_fan_speed_address  = 0x71, // needs testing
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = MSI_EC_ADDR_UNKNOWN,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNKNOWN,
		.mute_led_address    = MSI_EC_ADDR_UNKNOWN,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN, // ?
		.bl_modes         = { 0x00, 0x08 }, // ?
		.max_mode         = 1, // ?
		.bl_state_address = MSI_EC_ADDR_UNSUPP, // 0xd3, not functional
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_5[] __initconst = {
	"158LEMS1.103", // Alpha 15 B5EE / B5EEK
	"158LEMS1.105",
	"158LEMS1.106",
	NULL
};

static struct msi_ec_conf CONF5 __initdata = {
	.allowed_fw = ALLOWED_FW_5, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNKNOWN,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_address = MSI_EC_ADDR_UNKNOWN,
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2c,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP, // 0xf3, not functional (RGB)
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_6[] __initconst = {
	"1542EMS1.102", // GP66 Leopard 10UG / 10UE / 10UH
	"1542EMS1.104",
	NULL
};

static struct msi_ec_conf CONF6 __initdata = {
	.allowed_fw = ALLOWED_FW_6, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xd5,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0xc9,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = MSI_EC_ADDR_UNKNOWN,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNSUPP,
		.mute_led_address    = MSI_EC_ADDR_UNSUPP,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP, // not functional (RGB)
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_7[] __initconst = {
	"17FKEMS1.108", // Bravo 17 A4DDR / A4DDK
	"17FKEMS1.109",
	"17FKEMS1.10A",
	NULL
};

static struct msi_ec_conf CONF7 __initdata = {
	.allowed_fw = ALLOWED_FW_7, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNKNOWN, // 0xd5 but has its own set of modes
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d }, // d may not be relevant
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0xc9,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_address = MSI_EC_ADDR_UNKNOWN,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNSUPP,
		.mute_led_address    = 0x2c,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_8[] __initconst = {
	"14F1EMS1.114", // Summit E14 Evo A12M
	"14F1EMS1.115",
	"14F1EMS1.116",
	"14F1EMS1.117",
	"14F1EMS1.118",
	"14F1EMS1.119",
	"14F1EMS1.120",
	NULL
};

static struct msi_ec_conf CONF8 __initdata = {
	.allowed_fw = ALLOWED_FW_8, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNSUPP,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c,
		.bl_modes         = { 0x00, 0x80 }, // 00 - on, 80 - 10 sec auto off
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_9[] __initconst = {
	"14JKEMS1.104", // Modern 14 C5M
	NULL
};

static struct msi_ec_conf CONF9 __initdata = {
	.allowed_fw = ALLOWED_FW_9, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP, // unsupported or enabled by ECO shift
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNSUPP,
		.rt_fan_speed_address = MSI_EC_ADDR_UNSUPP,
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2c,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP, // not presented in MSI app
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_10[] __initconst = {
	"1582EMS1.107", // Katana GF66 11UC / 11UD
	NULL
};

static struct msi_ec_conf CONF10 __initdata = {
	.allowed_fw = ALLOWED_FW_10, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = MSI_EC_ADDR_UNSUPP,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xe5,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_11[] __initconst = {
	"16S6EMS1.111", // Prestige 15 A11SCX
	NULL 
};

static struct msi_ec_conf CONF11 __initdata = {
	.allowed_fw = ALLOWED_FW_11, // WMI2 based
	.charge_control = {
		.address      = 0xD7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNKNOWN,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x4d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNSUPP,
		.rt_fan_speed_address = MSI_EC_ADDR_UNSUPP,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN,
		.bl_modes         = {},
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_12[] __initconst = {
	"16R6EMS1.104", // GF63 Thin 11UC
	"16R6EMS1.106",
	"16R6EMS1.107",
	NULL
};

static struct msi_ec_conf CONF12 __initdata = {
	.allowed_fw = ALLOWED_FW_12, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP, // 0xeb
		.mask    = 0x0f, // 00, 0f
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNSUPP,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNSUPP,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_13[] __initconst = {
	"1594EMS1.109", // Prestige 16 Studio A13VE
	NULL
};

static struct msi_ec_conf CONF13 __initdata = {
	.allowed_fw = ALLOWED_FW_13, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4, // 0x00-0x10
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // super battery
			{ SM_COMFORT_NAME, 0xc1 }, // balanced
			{ SM_TURBO_NAME,   0xc4 }, // extreme
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP,
		.mask    = 0x0f, // 00, 0f
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71, // 0x0-0x96
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c, // KB auto turn off
		.bl_modes         = { 0x00, 0x08 }, // always on; off after 10 sec
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_14[] __initconst = {
	"17L2EMS1.108", // Katana 17 B11UCX, Katana GF76 11UC
	NULL
};

static struct msi_ec_conf CONF14 __initdata = {
	.allowed_fw = ALLOWED_FW_14, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	// .usb_share  {
	// 	.address      = 0xbf, // states: 0x08 || 0x28
	// 	.bit          = 5,
	// }
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8, // states: 0x40 || 0x50
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98, // states: 0x02 || 0x82
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2, // Performance Level
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // Low
			{ SM_COMFORT_NAME, 0xc1 }, // Medium
			{ SM_SPORT_NAME,   0xc0 }, // High
			{ SM_TURBO_NAME,   0xc4 }, // Turbo
			MSI_EC_MODE_NULL
			
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP, // enabled by Low Performance Level
		// .address = 0xeb, // states: 0x00 || 0x0f
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0xc9,
		.rt_fan_speed_base_min = 0x00, // ?
		.rt_fan_speed_base_max = 0x96, // ?
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00, // ?
		.bs_fan_speed_base_max = 0x0f, // ?
		// .rt_temp_table_start_adress = 0x6a,
		// .rt_fan_speed_table_start_address = 0x72,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0xcb,
		// .rt_temp_table_start_adress = 0x82,
		// .rt_fan_speed_table_start_address = 0x8a,
	},
	.leds = {
		.micmute_led_address = 0x2c, // states: 0x00 || 0x02
		.mute_led_address    = 0x2d, // states: 0x04 || 0x06
		.bit                 = 1,
	},
	.kbd_bl = {
		// .bl_mode_address  = 0x2c, // ?
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x08 }, // ? always on; off after 10 sec
		.max_mode         = 1, // ?
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_15[] __initconst = {
	"15CKEMS1.108", // Delta 15 A5EFK
	NULL
};

static struct msi_ec_conf CONF15 __initdata = {
	.allowed_fw = ALLOWED_FW_15, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a, 
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e, 
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2, 
		.modes = {
			{ SM_ECO_NAME,     0xa5 }, // super battery
			{ SM_COMFORT_NAME, 0xa1 }, // balanced
			{ SM_TURBO_NAME,   0xa0 }, // extreme
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNKNOWN,
		.mask    = 0x0f
	},
	.fan_mode = {
		.address = 0xf4, 
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68, 
		.rt_fan_speed_address  = 0xc9, 
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = 0xcd, 
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,  
		.rt_fan_speed_address = 0xcb, 
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2d,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x01 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP, // RGB
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_16[] __initconst = {
	"155LEMS1.105", // Modern 15 A5M
	"155LEMS1.106",
	NULL
};

static struct msi_ec_conf CONF16 __initdata = {
	.allowed_fw = ALLOWED_FW_16, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNKNOWN, // 0xed
		.mask    = 0x0f, // a5, a4, a2
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_address = MSI_EC_ADDR_UNKNOWN,
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2c,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_17[] __initconst = {
	"15K1IMS1.110", // Cyborg 15 A12VF
	NULL
};

static struct msi_ec_conf CONF17 __initdata = {
	.allowed_fw = ALLOWED_FW_17, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	// .usb_share  {
	// 	.address      = 0xbf, // states: 0x08 || 0x28
	// 	.bit          = 5,
	// }, // Like Katana 17 B11UCX
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4, // 0x01-0x11
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // super battery
			{ SM_COMFORT_NAME, 0xc1 }, // balanced
			{ SM_TURBO_NAME,   0xc4 }, // extreme
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb, // 0x0F ( on ) or 0x00 ( off ) on 0xEB
		.mask    = 0x0f, // 00, 0f
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
		// n/rpm register is C9
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c, // KB auto turn off
		.bl_modes         = { 0x00, 0x08 }, // always on; off after 10 sec
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_18[] __initconst = {
	"15HKEMS1.104", // Modern 15 B7M
	NULL
};

static struct msi_ec_conf CONF18 __initdata = {
	.allowed_fw = ALLOWED_FW_18, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP, // unsupported or enabled by ECO shift
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNSUPP,
		.rt_fan_speed_address = MSI_EC_ADDR_UNSUPP,
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2c,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP, // not presented in MSI app
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_19[] __initconst = { 
	"1543EMS1.113", // GP66 Leopard 11UG / 11U*
	NULL 
};

static struct msi_ec_conf CONF19 __initdata = {
	.allowed_fw = ALLOWED_FW_19, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0xc9,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNKNOWN,
		.mute_led_address    = MSI_EC_ADDR_UNKNOWN,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN,
		.bl_modes         = {},
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_20[] __initconst = {
	"1581EMS1.107", // Katana GF66 11UE / 11UG
	NULL
};

static struct msi_ec_conf CONF20 __initdata = {
	.allowed_fw = ALLOWED_FW_20, // WMI2 based
	.charge_control = { // tested
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = { // tested
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = { // tested
		.address = 0xe8,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = { // tested
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = { // tested
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = { // tested
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = { // tested
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68, // tested
		.rt_fan_speed_address  = 0xc9, // tested
		.rt_fan_speed_base_min = 0x00, // ! observed on machine (0x35 when fans was at min), but not working !
		.rt_fan_speed_base_max = 0x96, // ! ^ (0x56 with fans on cooler boost) !
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP, // reason: no such setting in the "MSI Center", checked in version 2.0.35
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80, // tested
		.rt_fan_speed_address = 0xcb, // ! observed the file reporting over 100% fan speed, which should not be possible !
	},
	.leds = { // tested
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = { // tested
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP, // reason: no such setting in the "MSI Center", checked in version 2.0.35
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_21[] __initconst = {
	"16R3EMS1.100", // GF63 Thin 9SC
	"16R3EMS1.102",
	"16R3EMS1.104",
	"16R4EMS2.102", // GF63 Thin 9SCSR
	NULL
};

static struct msi_ec_conf CONF21 __initdata = {
	.allowed_fw = ALLOWED_FW_21, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0xbc,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_BASIC_NAME,    0x4d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x64,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
		// .rt_temp_table_start_adress = ,
		// .rt_fan_speed_table_start_address = ,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
		// .rt_temp_table_start_adress = ,
		// .rt_fan_speed_table_start_address = ,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNSUPP,
		.mute_led_address    = MSI_EC_ADDR_UNSUPP,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP, // Only mode is solid red
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_22[] __initconst = {
	"17LLEMS1.106", // Alpha 17 B5EEK
	NULL
};

static struct msi_ec_conf CONF22 __initdata = {
	.allowed_fw = ALLOWED_FW_22, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // super_battery = 0xa5
			{ SM_COMFORT_NAME, 0xc1 }, // super_battery = 0xa4
			{ SM_SPORT_NAME,   0xc1 }, // super_battery = 0xa1
			{ SM_TURBO_NAME,   0xc4 }, // super_battery = 0xa0
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNKNOWN, // knwon. 0xd5.
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2c,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP, // RGB
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_23[] __initconst = {
	"16WKEMS1.105", // MSI Bravo 15 A4DDR (issue #134)
	NULL
};

static struct msi_ec_conf CONF23 __initdata = {
	.allowed_fw = ALLOWED_FW_23, // WMI1 based
	.charge_control = { // threshold
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4, // 0xe4 = 100%, but 0x80 too?
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP, // not in MSI app
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			// values can also be 0x81... when booting on Linux
			{ SM_COMFORT_NAME, 0xc1 }, // Silent / Balanced / AI
			{ SM_ECO_NAME,     0xc2 }, // Super Battery
			{ SM_TURBO_NAME,   0xc4 }, // Performance
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP, // enabled by "Super Battery" shift mode?
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			// 'd' is not relevant, values can also be 0x00... or 0x03...
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68, // a second value/sensor is at 0x64
		.rt_fan_speed_address  = 0x71, // target speed
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96, // at 150%
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
		// current RPM speed is 480000/x
		// with x 2 bytes at 0xcc and 0xcd
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89, // target speed
		// current RPM speed is 480000/x
		// with x 2 bytes at 0xca and 0xcb
	},
	.leds = {
		// No LED indicator
		.micmute_led_address = MSI_EC_ADDR_UNSUPP,
		.mute_led_address    = MSI_EC_ADDR_UNSUPP,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP, // not in MSI Center
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_24[] __initconst = {
	"14D1EMS1.103", // Modern 14 B10MW (#100)
	NULL
};

static struct msi_ec_conf CONF24 __initdata = {
	.allowed_fw = ALLOWED_FW_24, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2E,
		.block_address = 0x2F,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xBF,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xC2 }, // Super Battery
			{ SM_COMFORT_NAME, 0xC1 }, // + Silent
			{ SM_SPORT_NAME,   0xC0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP, // not 0xD5, tested
		.mask    = 0x0f,
	},
	.fan_mode = { // Creator Center sets 0x?0 instead of 0x?D
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNSUPP,
		.rt_fan_speed_address = MSI_EC_ADDR_UNSUPP,
	},
	.leds = {
		.micmute_led_address = 0x2B,
		.mute_led_address    = 0x2C,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xF3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_25[] __initconst = {
	"14F1EMS1.209", // Summit E14 Flip Evo A13MT
	"14F1EMS1.211",
	NULL
};

static struct msi_ec_conf CONF25 __initdata = {
	.allowed_fw = ALLOWED_FW_25, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c,
		.bl_modes         = { 0x00, 0x08 }, // 00 - on, 08 - 10 sec auto off
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_26[] __initconst = {
	"14DLEMS1.105", // Modern 14 B5M
	NULL
};

static struct msi_ec_conf CONF26 __initdata = {
	.allowed_fw = ALLOWED_FW_26, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0xbc,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // Super Battery
			{ SM_COMFORT_NAME, 0xc1 }, // Silent / Balanced / AI
			{ SM_SPORT_NAME,   0xc0 }, // Performance
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP, // 0x33 switches between 0x0D and 0x05 
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0xcd,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNSUPP,
		.rt_fan_speed_address = MSI_EC_ADDR_UNSUPP,
	},
	.leds = {
		.micmute_led_address = 0x2b,
		.mute_led_address    = 0x2c,
		.bit                 = 2,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP, // not presented in MSI app
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_27[] __initconst = {
	"17S2IMS1.113", // Raider GE78 HX Smart Touchpad 13V
	NULL
};

static struct msi_ec_conf CONF27 __initdata = {
	.allowed_fw = ALLOWED_FW_27, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_28[] __initconst = {
	"1822EMS1.105", // Titan 18 HX A14V
	"1822EMS1.109", // WMI 2.8
	"1822EMS1.111",
	"1822EMS1.112",
// .116 reports as .114
// DMIDECODE Version: E1822IMS.116 but /sys/devices/platform/msi-ec/debug/fw_version reads 1822EMS1.114
	"1822EMS1.114", 
	NULL
};

static struct msi_ec_conf CONF28 __initdata = {
	.allowed_fw = ALLOWED_FW_28,
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	// .usb_share  {
	// 	.address      = 0xbf, // states: 0x08 || 0x28
	// 	.bit          = 5,
	// }, // Like Katana 17 B11UCX
	.webcam = {
		.address       = MSI_EC_ADDR_UNSUPP,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4, // 0x01-0x11
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // super battery
			{ SM_COMFORT_NAME, 0xc1 }, // balanced
			{ SM_TURBO_NAME,   0xc4 }, // extreme
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb, // 0x0F ( on ) or 0x00 ( off ) on 0xEB
		.mask    = 0x0f, // 00, 0f
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
		// n/rpm register is C9
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP, // KB auto turn off
		.bl_modes         = { 0x00, 0x08 }, // always on; off after 10 sec
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x81,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_29[] __initconst = {
	"16V5EMS1.107", // MSI GS66 12UGS
	NULL
};

static struct msi_ec_conf CONF29 __initdata = {
	.allowed_fw = ALLOWED_FW_29,
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	// .usb_share  {
	// 	.address      = 0xbf,
	// 	.bit          = 5,
	// },
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // super battery
			{ SM_COMFORT_NAME, 0xc1 }, // balanced
			{ SM_TURBO_NAME,   0xc4 }, // extreme
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = MSI_EC_ADDR_UNKNOWN, // 0xc9
		.rt_fan_speed_base_min = 0x00, // ?
		.rt_fan_speed_base_max = 0x3d, // ?
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN, // 0xcd
		.bs_fan_speed_base_min = 0x00, // ?
		.bs_fan_speed_base_max = 0x0f, // ?
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0xcb,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNSUPP,
		.mute_led_address    = MSI_EC_ADDR_UNSUPP,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_30[] __initconst = {
	"17Q2IMS1.10D", // Titan GT77HX 13VH
	NULL
};

static struct msi_ec_conf CONF30 __initdata = {
	.allowed_fw = ALLOWED_FW_30, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // eco works as expected (much slower, uses less power and lower fan speeds)
			{ SM_COMFORT_NAME, 0xc1 }, // comfort, sport, and turbo all seem to be the same
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_address = MSI_EC_ADDR_UNKNOWN,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNKNOWN,
		.mute_led_address    = MSI_EC_ADDR_UNKNOWN,
		.bit = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNKNOWN,
		.bl_modes         = {},
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_31[] __initconst = {
	"16Q4EMS1.110", // GS65 Stealth
	NULL
};

static struct msi_ec_conf CONF31 __initdata = {
	.allowed_fw = ALLOWED_FW_31,
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4, // 0x00-0x10
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // super battery
			{ SM_COMFORT_NAME, 0xc1 }, // balanced
			{ SM_TURBO_NAME,   0xc4 }, // extreme
			{ SM_SPORT_NAME,   0xc0 }, // sport
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP, // Function not shown in dragon center
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_BASIC_NAME,    0x4c },
			{ FM_AUTO_NAME,     0x0c },
			{ FM_ADVANCED_NAME, 0x8c },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
		// n/rpm register is C9
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = MSI_EC_ADDR_UNKNOWN,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNSUPP,
		.mute_led_address    = MSI_EC_ADDR_UNSUPP,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP, // KB auto turn off
		.bl_modes         = { 0x00, 0x08 }, // always on; off after 10 sec
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP,
		.state_base_value = 0x81,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_32[] __initconst = {
	"158PIMS1.207", // Bravo 15 B7E
	"158PIMS1.112", // Bravo 15 B7ED
	NULL
};

static struct msi_ec_conf CONF32 __initdata = {
	.allowed_fw = ALLOWED_FW_32,
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = MSI_EC_ADDR_UNSUPP,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNKNOWN,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNSUPP,
		.rt_fan_speed_address = MSI_EC_ADDR_UNKNOWN,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { },
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_33[] __initconst = {
	"17N1EMS1.109", // MSI Creator Z17 A12UGST
	NULL
};

static struct msi_ec_conf CONF33 __initdata = {
	.allowed_fw = ALLOWED_FW_33,
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xD2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x4d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x96,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_34[] __initconst = {
	"14C6EMS1.109", // Prestige 14 Evo A12M
	NULL
};

static struct msi_ec_conf CONF34 __initdata = {
	.allowed_fw = ALLOWED_FW_34,
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // super battery
			{ SM_COMFORT_NAME, 0xc1 }, // silent / balanced
			{ SM_SPORT_NAME,   0xc0 }, // high performance
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d }, // super battery, balanced and auto high performance modes
			{ FM_SILENT_NAME,   0x1d }, // silent mode
			{ FM_ADVANCED_NAME, 0x4d }, // advanced high performance mode
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNKNOWN,
		.rt_fan_speed_address = MSI_EC_ADDR_UNKNOWN,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c,
		.bl_modes         = { 0x00, 0x08 }, // always on / off after 10 sec
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_35[] __initconst = {
	"15M2IMS1.113", // Raider GE68HX 13VG
	NULL
};

static struct msi_ec_conf CONF35 __initdata = {
	.allowed_fw = ALLOWED_FW_35, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	// .usb_share = {
	//  	.address      = 0xbf, // states: 0x08 || 0x28
	//  	.bit          = 5,
	// },
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP, // not in MSI app
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_COMFORT_NAME, 0xc1 }, // Silent / Balanced / AI
			{ SM_ECO_NAME,     0xc2 }, // Super Battery
			{ SM_TURBO_NAME,   0xc4 }, // Performance
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
		// Fan rpm is 480000 / value at combined: c8..c9
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
		// Fan rpm is 480000 / value at combined: ca..cb
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_36[] __initconst = {
	"1585EMS1.115", // MSI Katana 15 B13VFK
	NULL
};

static struct msi_ec_conf CONF36 __initdata = {
	.allowed_fw = ALLOWED_FW_36, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a, // offset 10%
		.offset_end   = 0x80, // offset 0%
		.range_min    = 0x8a, // 10%
		.range_max    = 0xe4, // 100%
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP, // not supported but it is already controlled by hardware
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = true, // true because FN key is on right side
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xD2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68, // CPU temperature
		.rt_fan_speed_address  = 0xc9,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x96,
	},
	.gpu = {
		.rt_temp_address      = 0x80, // GPU temperature
		.rt_fan_speed_address = 0xcb,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_37[] __initconst = {
	"15M1IMS1.113", // Vector GP68 HX 12V
	NULL
};

static struct msi_ec_conf CONF37 __initdata = {
	.allowed_fw = ALLOWED_FW_37, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	// .usb_share  {
	// 	.address      = 0xbf, // states: 0x08 || 0x28
	// 	.bit          = 5,
	// }
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = { // also on address 0x91 (?) = 0x5f - normal, 0x50 - silent
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_38[] __initconst = {
	"17E8IMS1.106", // GL75 Leopard 10SCXR/MS-17E8
	"17E8EMS1.101",
	NULL
};

static struct msi_ec_conf CONF38 __initdata = {
	.allowed_fw = ALLOWED_FW_38, // WMI1 based
	.charge_control = {
		.address      = 0xef,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xbf,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xf2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 },
			{ SM_COMFORT_NAME, 0xc1 },
			{ SM_SPORT_NAME,   0xc0 },
			{ SM_TURBO_NAME,   0xc4 },
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNKNOWN,
	},
	.fan_mode = {
		.address = 0xf4,
		.modes = {
			{ FM_AUTO_NAME,     0x00 },
			{ FM_ADVANCED_NAME, 0x80 },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x19,
		.rt_fan_speed_base_max = 0x37,
		.bs_fan_speed_address  = 0x89,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNKNOWN,
		.mute_led_address    = MSI_EC_ADDR_UNKNOWN,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = 0xf3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};
static const char *ALLOWED_FW_39[] __initconst = {
	"16R8IMS1.117", // Thin GF63 12UC & Thin GF63 12UCX
	NULL
};

static struct msi_ec_conf CONF39 __initdata = {
	.allowed_fw = ALLOWED_FW_39, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,      0xc2},
			{ SM_COMFORT_NAME,  0xc1},
			{ SM_TURBO_NAME,    0xc4},
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,      0x0d},
			{ FM_SILENT_NAME,    0x1d},
			{ FM_ADVANCED_NAME,  0x8d},
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNKNOWN,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = MSI_EC_ADDR_UNSUPP,
	},
	.leds = {
		.micmute_led_address = MSI_EC_ADDR_UNSUPP,
		.mute_led_address    = MSI_EC_ADDR_UNSUPP,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { },
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_40[] __initconst = {
	"17S1IMS1.105", // Raider GE78HX 13VI
	NULL
};

static struct msi_ec_conf CONF40 __initdata = {
	.allowed_fw = ALLOWED_FW_40, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	// .usb_share = {
	//  	.address      = 0xbf, // states: 0x08 || 0x28
	//  	.bit          = 5,
	// },
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP, // not in MSI app
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = true,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_COMFORT_NAME, 0xc1 }, // Silent / Balanced / AI
			{ SM_ECO_NAME,     0xc2 }, // Super Battery
			{ SM_TURBO_NAME,   0xc4 }, // Performance
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = 0x89,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
		// Fan rpm is 480000 / value at combined: c8..c9
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
		// Fan rpm is 480000 / value at combined: ca..cb
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_41[] __initconst = {
	"15M1IMS2.111", // MSI Vector 16 HX A14VHG
	NULL
};

static struct msi_ec_conf CONF41 __initdata = {
	.allowed_fw = ALLOWED_FW_41, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = 0x2e,
		.block_address = MSI_EC_ADDR_UNSUPP,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_COMFORT_NAME, 0xc1 }, // Silent / Balanced / AI
			{ SM_TURBO_NAME,   0xc4 }, // Performance
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = MSI_EC_ADDR_UNSUPP, // Function not shown in dragon center
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0x71,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = 0x89,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x96,
	},
	.gpu = {
		.rt_temp_address      = 0x80,
		.rt_fan_speed_address = 0x89,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = 0x2d,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = MSI_EC_ADDR_UNSUPP,
		.bl_modes         = { 0x00, 0x08 },
		.max_mode         = 1,
		.bl_state_address = MSI_EC_ADDR_UNSUPP,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static const char *ALLOWED_FW_42[] __initconst = {
	"14L1EMS1.307", // Modern 14 H D13M
	"14L1EMS1.308",
	NULL
};

static struct msi_ec_conf CONF42 __initdata = {
	.allowed_fw = ALLOWED_FW_42, // WMI2 based
	.charge_control = {
		.address      = 0xd7,
		.offset_start = 0x8a,
		.offset_end   = 0x80,
		.range_min    = 0x8a,
		.range_max    = 0xe4,
	},
	.webcam = {
		.address       = MSI_EC_ADDR_UNSUPP,
		.block_address = 0x2f,
		.bit           = 1,
	},
	.fn_win_swap = {
		.address = 0xe8,
		.bit     = 4,
		.invert  = false,
	},
	.cooler_boost = {
		.address = 0x98,
		.bit     = 7,
	},
	.shift_mode = {
		.address = 0xd2,
		.modes = {
			{ SM_ECO_NAME,     0xc2 }, // super battery
			{ SM_COMFORT_NAME, 0xc1 }, // balanced + silent + ai
			{ SM_TURBO_NAME,   0xc4 }, // extreme performance
			MSI_EC_MODE_NULL
		},
	},
	.super_battery = {
		.address = 0xeb,
		.mask    = 0x0f,
	},
	.fan_mode = {
		.address = 0xd4,
		.modes = {
			{ FM_AUTO_NAME,     0x0d },
			{ FM_SILENT_NAME,   0x1d },
			{ FM_ADVANCED_NAME, 0x8d },
			MSI_EC_MODE_NULL
		},
	},
	.cpu = {
		.rt_temp_address       = 0x68,
		.rt_fan_speed_address  = 0xc9,
		.rt_fan_speed_base_min = 0x00,
		.rt_fan_speed_base_max = 0x96,
		.bs_fan_speed_address  = MSI_EC_ADDR_UNSUPP,
		.bs_fan_speed_base_min = 0x00,
		.bs_fan_speed_base_max = 0x0f,
	},
	.gpu = {
		.rt_temp_address      = MSI_EC_ADDR_UNSUPP,
		.rt_fan_speed_address = MSI_EC_ADDR_UNSUPP,
	},
	.leds = {
		.micmute_led_address = 0x2c,
		.mute_led_address    = MSI_EC_ADDR_UNSUPP,
		.bit                 = 1,
	},
	.kbd_bl = {
		.bl_mode_address  = 0x2c,
		.bl_modes         = { 0x00, 0x08 }, // 00 - on, 08 - 10 sec auto off
		.max_mode         = 1,
		.bl_state_address = 0xd3,
		.state_base_value = 0x80,
		.max_state        = 3,
	},
};

static struct msi_ec_conf *CONFIGURATIONS[] __initdata = {
	&CONF0,
	&CONF1,
	&CONF2,
	&CONF3,
	&CONF4,
	&CONF5,
	&CONF6,
	&CONF7,
	&CONF8,
	&CONF9,
	&CONF10,
	&CONF11,
	&CONF12,
	&CONF13,
	&CONF14,
	&CONF15,
	&CONF16,
	&CONF17,
	&CONF18,
	&CONF19,
	&CONF20,
	&CONF21,
	&CONF22,
	&CONF23,
	&CONF24,
	&CONF25,
	&CONF26,
	&CONF27,
	&CONF28,
	&CONF29,
	&CONF30,
	&CONF31,
	&CONF32,
	&CONF33,
	&CONF34,
	&CONF35,
	&CONF36,
	&CONF37,
	&CONF38,
	&CONF39,
	&CONF40,
	&CONF41,
	&CONF42,
	NULL
};

static bool conf_loaded = false;
static struct msi_ec_conf conf; // current configuration

struct attribute_support {
	struct attribute *attribute;
	bool supported;
};

static char *firmware = NULL;
module_param(firmware, charp, 0);
MODULE_PARM_DESC(firmware, "Load a configuration for a specified firmware version");

static bool debug = false;
module_param(debug, bool, 0);
MODULE_PARM_DESC(debug, "Load the driver in the debug mode, exporting the debug attributes");

// ============================================================ //
// Helper functions
// ============================================================ //

#define streq(x, y) (strcmp(x, y) == 0 || strcmp(x, y "\n") == 0)

#define set_bit(v, b)   (v |= (1 << b))
#define unset_bit(v, b) (v &= ~(1 << b))
#define check_bit(v, b) ((bool)((v >> b) & 1))

static int ec_read_seq(u8 addr, u8 *buf, u8 len)
{
	int result;
	for (u8 i = 0; i < len; i++) {
		result = ec_read(addr + i, buf + i);
		if (result < 0)
			return result;
	}
	return 0;
}

static int ec_set_by_mask(u8 addr, u8 mask)
{
	int result;
	u8 stored;

	mutex_lock(&ec_set_by_mask_mutex);
	result = ec_read(addr, &stored);
	if (result < 0)
		goto unlock;

	stored |= mask;
	result = ec_write(addr, stored);

unlock:
	mutex_unlock(&ec_set_by_mask_mutex);
	return result;
}

static int ec_unset_by_mask(u8 addr, u8 mask)
{
	int result;
	u8 stored;

	mutex_lock(&ec_unset_by_mask_mutex);
	result = ec_read(addr, &stored);
	if (result < 0)
		goto unlock;

	stored &= ~mask;
	result = ec_write(addr, stored);

unlock:
	mutex_unlock(&ec_unset_by_mask_mutex);
	return result;
}

static int ec_check_by_mask(u8 addr, u8 mask, bool *output)
{
	int result;
	u8 stored;

	result = ec_read(addr, &stored);
	if (result < 0)
		return result;

	*output = ((stored & mask) == mask);

	return 0;
}

static int ec_set_bit(u8 addr, u8 bit, bool value)
{
	int result;
	u8 stored;

	mutex_lock(&ec_set_bit_mutex);
	result = ec_read(addr, &stored);
	if (result < 0)
		goto unlock;

	if (value)
		set_bit(stored, bit);
	else
		unset_bit(stored, bit);

	result = ec_write(addr, stored);

unlock:
	mutex_unlock(&ec_set_bit_mutex);
	return result;
}

static int ec_check_bit(u8 addr, u8 bit, bool *output)
{
	int result;
	u8 stored;

	result = ec_read(addr, &stored);
	if (result < 0)
		return result;

	*output = check_bit(stored, bit);

	return 0;
}

static int ec_get_firmware_version(u8 buf[MSI_EC_FW_VERSION_LENGTH + 1])
{
	int result;

	memset(buf, 0, MSI_EC_FW_VERSION_LENGTH + 1);
	result = ec_read_seq(MSI_EC_FW_VERSION_ADDRESS, buf,
			     MSI_EC_FW_VERSION_LENGTH);
	if (result < 0)
		return result;

	return MSI_EC_FW_VERSION_LENGTH + 1;
}

// ============================================================ //
// Sysfs power_supply subsystem
// ============================================================ //

static ssize_t charge_control_threshold_show(u8 offset, struct device *device,
					     struct device_attribute *attr,
					     char *buf)
{
	u8 rdata;
	int result;

	result = ec_read(conf.charge_control.address, &rdata);
	if (result < 0)
		return result;

	// thresholds are unknown
	if (rdata == 0x80) {
		return sysfs_emit(buf, "0\n");
	}

	return sysfs_emit(buf, "%i\n", rdata - offset);
}

static ssize_t charge_control_threshold_store(u8 offset, struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	u8 wdata;
	int result;

	result = kstrtou8(buf, 10, &wdata);
	if (result < 0)
		return result;

	wdata += offset;
	if (wdata < conf.charge_control.range_min ||
	    wdata > conf.charge_control.range_max)
		return -EINVAL;

	result = ec_write(conf.charge_control.address, wdata);
	if (result < 0)
		return result;

	return count;
}

static ssize_t
charge_control_start_threshold_show(struct device *device,
				    struct device_attribute *attr, char *buf)
{
	return charge_control_threshold_show(conf.charge_control.offset_start,
					     device, attr, buf);
}

static ssize_t
charge_control_start_threshold_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	return charge_control_threshold_store(
		conf.charge_control.offset_start, dev, attr, buf, count);
}

static ssize_t charge_control_end_threshold_show(struct device *device,
						 struct device_attribute *attr,
						 char *buf)
{
	return charge_control_threshold_show(conf.charge_control.offset_end,
					     device, attr, buf);
}

static ssize_t charge_control_end_threshold_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t count)
{
	return charge_control_threshold_store(conf.charge_control.offset_end,
					      dev, attr, buf, count);
}

static DEVICE_ATTR_RW(charge_control_start_threshold);
static DEVICE_ATTR_RW(charge_control_end_threshold);

static struct attribute *msi_battery_attrs[] = {
	&dev_attr_charge_control_start_threshold.attr,
	&dev_attr_charge_control_end_threshold.attr,
	NULL
};

ATTRIBUTE_GROUPS(msi_battery);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,2,0))
static int msi_battery_add(struct power_supply *battery,
			   struct acpi_battery_hook *hook)
#else
static int msi_battery_add(struct power_supply *battery)
#endif
{
	return device_add_groups(&battery->dev, msi_battery_groups);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,2,0))
static int msi_battery_remove(struct power_supply *battery,
			      struct acpi_battery_hook *hook)
#else
static int msi_battery_remove(struct power_supply *battery)
#endif
{
	device_remove_groups(&battery->dev, msi_battery_groups);
	return 0;
}

static struct acpi_battery_hook battery_hook = {
	.add_battery = msi_battery_add,
	.remove_battery = msi_battery_remove,
	.name = MSI_EC_DRIVER_NAME,
};

// ============================================================ //
// Sysfs platform device attributes (root)
// ============================================================ //

static ssize_t webcam_common_show(u8 address,
			          char *buf,
				  const char *str_on_0,
				  const char *str_on_1)
{
	int result;
	bool bit_value;

	result = ec_check_bit(address, conf.webcam.bit, &bit_value);
	if (result < 0)
		return result;

	if (bit_value) {
		return sysfs_emit(buf, "%s\n", str_on_1);
	} else {
		return sysfs_emit(buf, "%s\n", str_on_0);
	}
}

static ssize_t webcam_common_store(u8 address,
				   const char *buf,
				   size_t count,
				   const char *str_for_0,
				   const char *str_for_1)
{
	int result = -EINVAL;

	if (sysfs_streq(str_for_1, buf))
		result = ec_set_bit(address, conf.webcam.bit, true);

	if (sysfs_streq(str_for_0, buf))
		result = ec_set_bit(address, conf.webcam.bit, false);

	if (result < 0)
		return result;

	return count;
}

static ssize_t webcam_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	return webcam_common_show(conf.webcam.address,
				  buf,
				  "off", "on");
}

static ssize_t webcam_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	return webcam_common_store(conf.webcam.address,
				   buf, count,
				   "off", "on");
}

static ssize_t webcam_block_show(struct device *device,
				 struct device_attribute *attr,
				 char *buf)
{
	return webcam_common_show(conf.webcam.block_address,
				  buf,
				  "on", "off");
}

static ssize_t webcam_block_store(struct device *dev,
				  struct device_attribute *attr,
			          const char *buf, size_t count)
{
	return webcam_common_store(conf.webcam.block_address,
				   buf, count,
				   "on", "off");
}

static ssize_t fn_key_show(struct device *device, struct device_attribute *attr,
			   char *buf)
{
	int result;
	bool bit_value;

	result = ec_check_bit(conf.fn_win_swap.address, conf.fn_win_swap.bit, &bit_value);

	if (bit_value ^ conf.fn_win_swap.invert) {
		return sysfs_emit(buf, "%s\n", "right");
	} else {
		return sysfs_emit(buf, "%s\n", "left");
	}
}

static ssize_t fn_key_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int result;

	if (streq(buf, "right")) {
		result = ec_set_bit(conf.fn_win_swap.address,
				    conf.fn_win_swap.bit,
				    true ^ conf.fn_win_swap.invert);
	} else if (streq(buf, "left")) {
		result = ec_set_bit(conf.fn_win_swap.address,
				    conf.fn_win_swap.bit,
				    false ^ conf.fn_win_swap.invert);
	}

	if (result < 0)
		return result;

	return count;
}

static ssize_t win_key_show(struct device *device,
			    struct device_attribute *attr, char *buf)
{
	int result;
	bool bit_value;

	result = ec_check_bit(conf.fn_win_swap.address, conf.fn_win_swap.bit, &bit_value);

	if (bit_value ^ conf.fn_win_swap.invert) {
		return sysfs_emit(buf, "%s\n", "left");
	} else {
		return sysfs_emit(buf, "%s\n", "right");
	}
}

static ssize_t win_key_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int result;

	if (streq(buf, "right")) {
		result = ec_set_bit(conf.fn_win_swap.address,
				    conf.fn_win_swap.bit,
				    false ^ conf.fn_win_swap.invert);
	} else if (streq(buf, "left")) {
		result = ec_set_bit(conf.fn_win_swap.address,
				    conf.fn_win_swap.bit,
				    true ^ conf.fn_win_swap.invert);
	}

	if (result < 0)
		return result;

	return count;
}

static ssize_t battery_mode_show(struct device *device,
				 struct device_attribute *attr, char *buf)
{
	u8 rdata;
	int result;

	result = ec_read(conf.charge_control.address, &rdata);
	if (result < 0)
		return result;

	if (rdata == conf.charge_control.range_max) {
		return sysfs_emit(buf, "%s\n", "max");
	} else if (rdata == conf.charge_control.offset_end + 80) { // up to 80%
		return sysfs_emit(buf, "%s\n", "medium");
	} else if (rdata == conf.charge_control.offset_end + 60) { // up to 60%
		return sysfs_emit(buf, "%s\n", "min");
	} else {
		return sysfs_emit(buf, "%s (%i)\n", "unknown", rdata);
	}
}

static ssize_t battery_mode_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int result = -EINVAL;

	if (streq(buf, "max"))
		result = ec_write(conf.charge_control.address,
				  conf.charge_control.range_max);

	else if (streq(buf, "medium")) // up to 80%
		result = ec_write(conf.charge_control.address,
				  conf.charge_control.offset_end + 80);

	else if (streq(buf, "min")) // up to 60%
		result = ec_write(conf.charge_control.address,
				  conf.charge_control.offset_end + 60);

	if (result < 0)
		return result;

	return count;
}

static ssize_t cooler_boost_show(struct device *device,
				 struct device_attribute *attr, char *buf)
{
	int result;
	bool bit_value;

	result = ec_check_bit(conf.cooler_boost.address, conf.cooler_boost.bit, &bit_value);

	if (bit_value) {
		return sysfs_emit(buf, "%s\n", "on");
	} else {
		return sysfs_emit(buf, "%s\n", "off");
	}
}

static ssize_t cooler_boost_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int result = -EINVAL;

	if (streq(buf, "on"))
		result = ec_set_bit(conf.cooler_boost.address,
				    conf.cooler_boost.bit,
				    true);

	else if (streq(buf, "off"))
		result = ec_set_bit(conf.cooler_boost.address,
				    conf.cooler_boost.bit,
				    false);

	if (result < 0)
		return result;

	return count;
}

static ssize_t available_shift_modes_show(struct device *device,
				          struct device_attribute *attr,
				          char *buf)
{
	int result = 0;
	int count = 0;

	for (int i = 0; conf.shift_mode.modes[i].name; i++) {
		// NULL entries have NULL name

		result = sysfs_emit_at(buf, count, "%s\n", conf.shift_mode.modes[i].name);
		if (result < 0)
			return result;
		count += result;
	}

	return count;
}

static ssize_t shift_mode_show(struct device *device,
			       struct device_attribute *attr,
			       char *buf)
{
	u8 rdata;
	int result;

	result = ec_read(conf.shift_mode.address, &rdata);
	if (result < 0)
		return result;

	if (rdata == 0x80)
		return sysfs_emit(buf, "%s\n", "unspecified");

	for (int i = 0; conf.shift_mode.modes[i].name; i++) {
		// NULL entries have NULL name

		if (rdata == conf.shift_mode.modes[i].value) {
			return sysfs_emit(buf, "%s\n", conf.shift_mode.modes[i].name);
		}
	}

	return sysfs_emit(buf, "%s (%i)\n", "unknown", rdata);
}

static ssize_t shift_mode_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	int result;

	for (int i = 0; conf.shift_mode.modes[i].name; i++) {
		// NULL entries have NULL name

		if (sysfs_streq(conf.shift_mode.modes[i].name, buf)) {
			result = ec_write(conf.shift_mode.address,
					  conf.shift_mode.modes[i].value);
			if (result < 0)
				return result;

			return count;
		}
	}

	return -EINVAL;
}

static ssize_t super_battery_show(struct device *device,
				  struct device_attribute *attr, char *buf)
{
	int result;
	bool enabled;

	result = ec_check_by_mask(conf.super_battery.address,
				  conf.super_battery.mask,
				  &enabled);

	if (enabled) {
		return sysfs_emit(buf, "%s\n", "on");
	} else {
		return sysfs_emit(buf, "%s\n", "off");
	}
}

static ssize_t super_battery_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int result = -EINVAL;

	if (streq(buf, "on"))
		result = ec_set_by_mask(conf.super_battery.address,
				        conf.super_battery.mask);

	else if (streq(buf, "off"))
		result = ec_unset_by_mask(conf.super_battery.address,
					  conf.super_battery.mask);

	if (result < 0)
		return result;

	return count;
}

static ssize_t available_fan_modes_show(struct device *device,
					struct device_attribute *attr,
					char *buf)
{
	int result = 0;
	int count = 0;

	for (int i = 0; conf.fan_mode.modes[i].name; i++) {
		// NULL entries have NULL name

		result = sysfs_emit_at(buf, count, "%s\n", conf.fan_mode.modes[i].name);
		if (result < 0)
			return result;
		count += result;
	}

	return count;
}

static ssize_t fan_mode_show(struct device *device,
			     struct device_attribute *attr, char *buf)
{
	u8 rdata;
	int result;

	result = ec_read(conf.fan_mode.address, &rdata);
	if (result < 0)
		return result;

	for (int i = 0; conf.fan_mode.modes[i].name; i++) {
		// NULL entries have NULL name

		if (rdata == conf.fan_mode.modes[i].value) {
			return sysfs_emit(buf, "%s\n", conf.fan_mode.modes[i].name);
		}
	}

	return sysfs_emit(buf, "%s (%i)\n", "unknown", rdata);
}

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	int result;

	for (int i = 0; conf.fan_mode.modes[i].name; i++) {
		// NULL entries have NULL name

		if (sysfs_streq(conf.fan_mode.modes[i].name, buf)) {
			result = ec_write(conf.fan_mode.address,
					  conf.fan_mode.modes[i].value);
			if (result < 0)
				return result;

			return count;
		}
	}

	return -EINVAL;
}

static ssize_t fw_version_show(struct device *device,
			       struct device_attribute *attr, char *buf)
{
	u8 rdata[MSI_EC_FW_VERSION_LENGTH + 1];
	int result;

	result = ec_get_firmware_version(rdata);
	if (result < 0)
		return result;

	return sysfs_emit(buf, "%s\n", rdata);
}

static ssize_t fw_release_date_show(struct device *device,
				    struct device_attribute *attr, char *buf)
{
	u8 rdate[MSI_EC_FW_DATE_LENGTH + 1];
	u8 rtime[MSI_EC_FW_TIME_LENGTH + 1];
	int result;
	int year, month, day, hour, minute, second;

	memset(rdate, 0, MSI_EC_FW_DATE_LENGTH + 1);
	result = ec_read_seq(MSI_EC_FW_DATE_ADDRESS, rdate,
			     MSI_EC_FW_DATE_LENGTH);
	if (result < 0)
		return result;
	sscanf(rdate, "%02d%02d%04d", &month, &day, &year);

	memset(rtime, 0, MSI_EC_FW_TIME_LENGTH + 1);
	result = ec_read_seq(MSI_EC_FW_TIME_ADDRESS, rtime,
			     MSI_EC_FW_TIME_LENGTH);
	if (result < 0)
		return result;
	sscanf(rtime, "%02d:%02d:%02d", &hour, &minute, &second);

	return sysfs_emit(buf, "%04d/%02d/%02d %02d:%02d:%02d\n", year, month, day,
		          hour, minute, second);
}

static DEVICE_ATTR_RW(webcam);
static DEVICE_ATTR_RW(webcam_block);
static DEVICE_ATTR_RW(fn_key);
static DEVICE_ATTR_RW(win_key);
static DEVICE_ATTR_RW(battery_mode);
static DEVICE_ATTR_RW(cooler_boost);
static DEVICE_ATTR_RO(available_shift_modes);
static DEVICE_ATTR_RW(shift_mode);
static DEVICE_ATTR_RW(super_battery);
static DEVICE_ATTR_RO(available_fan_modes);
static DEVICE_ATTR_RW(fan_mode);
static DEVICE_ATTR_RO(fw_version);
static DEVICE_ATTR_RO(fw_release_date);

static struct attribute *msi_root_attrs[] = {
	&dev_attr_webcam.attr,
	&dev_attr_webcam_block.attr,
	&dev_attr_fn_key.attr,
	&dev_attr_win_key.attr,
	&dev_attr_battery_mode.attr,
	&dev_attr_cooler_boost.attr,
	&dev_attr_available_shift_modes.attr,
	&dev_attr_shift_mode.attr,
	&dev_attr_super_battery.attr,
	&dev_attr_available_fan_modes.attr,
	&dev_attr_fan_mode.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_fw_release_date.attr,
	NULL
};

// ============================================================ //
// Sysfs platform device attributes (cpu)
// ============================================================ //

static ssize_t cpu_realtime_temperature_show(struct device *device,
					     struct device_attribute *attr,
					     char *buf)
{
	u8 rdata;
	int result;

	result = ec_read(conf.cpu.rt_temp_address, &rdata);
	if (result < 0)
		return result;

	return sysfs_emit(buf, "%i\n", rdata);
}

static ssize_t cpu_realtime_fan_speed_show(struct device *device,
					   struct device_attribute *attr,
					   char *buf)
{
	u8 rdata;
	int result;

	result = ec_read(conf.cpu.rt_fan_speed_address, &rdata);
	if (result < 0)
		return result;

	if ((rdata < conf.cpu.rt_fan_speed_base_min ||
	    rdata > conf.cpu.rt_fan_speed_base_max))
		return -EINVAL;

	return sysfs_emit(buf, "%i\n",
		          100 * (rdata - conf.cpu.rt_fan_speed_base_min) /
				  (conf.cpu.rt_fan_speed_base_max -
				   conf.cpu.rt_fan_speed_base_min));
}

static ssize_t cpu_basic_fan_speed_show(struct device *device,
					struct device_attribute *attr,
					char *buf)
{
	u8 rdata;
	int result;

	result = ec_read(conf.cpu.bs_fan_speed_address, &rdata);
	if (result < 0)
		return result;

	if (rdata < conf.cpu.bs_fan_speed_base_min ||
	    rdata > conf.cpu.bs_fan_speed_base_max)
		return -EINVAL;

	return sysfs_emit(buf, "%i\n",
		          100 * (rdata - conf.cpu.bs_fan_speed_base_min) /
				  (conf.cpu.bs_fan_speed_base_max -
				   conf.cpu.bs_fan_speed_base_min));
}

static ssize_t cpu_basic_fan_speed_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	u8 wdata;
	int result;

	result = kstrtou8(buf, 10, &wdata);
	if (result < 0)
		return result;

	if (wdata > 100)
		return -EINVAL;

	result = ec_write(conf.cpu.bs_fan_speed_address,
			  (wdata * (conf.cpu.bs_fan_speed_base_max -
				    conf.cpu.bs_fan_speed_base_min) +
			   100 * conf.cpu.bs_fan_speed_base_min) /
				  100);
	if (result < 0)
		return result;

	return count;
}

static struct device_attribute dev_attr_cpu_realtime_temperature = {
	.attr = {
		.name = "realtime_temperature",
		.mode = 0444,
	},
	.show = cpu_realtime_temperature_show,
};

static struct device_attribute dev_attr_cpu_realtime_fan_speed = {
	.attr = {
		.name = "realtime_fan_speed",
		.mode = 0444,
	},
	.show = cpu_realtime_fan_speed_show,
};

static struct device_attribute dev_attr_cpu_basic_fan_speed = {
	.attr = {
		.name = "basic_fan_speed",
		.mode = 0644,
	},
	.show = cpu_basic_fan_speed_show,
	.store = cpu_basic_fan_speed_store,
};

static struct attribute *msi_cpu_attrs[] = {
	&dev_attr_cpu_realtime_temperature.attr,
	&dev_attr_cpu_realtime_fan_speed.attr,
	&dev_attr_cpu_basic_fan_speed.attr,
	NULL
};

// ============================================================ //
// Sysfs platform device attributes (gpu)
// ============================================================ //

static ssize_t gpu_realtime_temperature_show(struct device *device,
					     struct device_attribute *attr,
					     char *buf)
{
	u8 rdata;
	int result;

	result = ec_read(conf.gpu.rt_temp_address, &rdata);
	if (result < 0)
		return result;

	return sysfs_emit(buf, "%i\n", rdata);
}

static ssize_t gpu_realtime_fan_speed_show(struct device *device,
					   struct device_attribute *attr,
					   char *buf)
{
	u8 rdata;
	int result;

	result = ec_read(conf.gpu.rt_fan_speed_address, &rdata);
	if (result < 0)
		return result;

	return sysfs_emit(buf, "%i\n", rdata);
}

static struct device_attribute dev_attr_gpu_realtime_temperature = {
	.attr = {
		.name = "realtime_temperature",
		.mode = 0444,
	},
	.show = gpu_realtime_temperature_show,
};

static struct device_attribute dev_attr_gpu_realtime_fan_speed = {
	.attr = {
		.name = "realtime_fan_speed",
		.mode = 0444,
	},
	.show = gpu_realtime_fan_speed_show,
};

static struct attribute *msi_gpu_attrs[] = {
	&dev_attr_gpu_realtime_temperature.attr,
	&dev_attr_gpu_realtime_fan_speed.attr,
	NULL
};

// ============================================================ //
// Sysfs platform device attributes (debug)
// ============================================================ //

// Prints an EC memory dump in form of a table
static ssize_t ec_dump_show(struct device *device,
			    struct device_attribute *attr,
			    char *buf)
{
	int count = 0;

	// print header
	count += sysfs_emit(
		buf,
		"     | _0 _1 _2 _3 _4 _5 _6 _7 _8 _9 _a _b _c _d _e _f\n"
		"-----+------------------------------------------------\n");

	// print dump
	for (u8 i = 0x0; i <= 0xf; i++) {
		u8 addr_base = i * 16;

		count += sysfs_emit_at(buf, count, "%#x_ |", i);
		for (u8 j = 0x0; j <= 0xf; j++) {
			u8 rdata;
			int result = ec_read(addr_base + j, &rdata);
			if (result < 0)
				return result;

			count += sysfs_emit_at(buf, count, " %02x", rdata);
		}

		count += sysfs_emit_at(buf, count, "\n");
	}

	return count;
}

// stores a value in the specified EC memory address. Format: "xx=xx", xx - hex u8
static ssize_t ec_set_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	if (count > 6) // "xx=xx\n" - 6 chars
		return -EINVAL;

	int result;

	char addr_s[3], val_s[3];
	result = sscanf(buf, "%2s=%2s", addr_s, val_s);
	if (result != 2)
		return -EINVAL;

	u8 addr, val;

	// convert addr
	result = kstrtou8(addr_s, 16, &addr);
	if (result < 0)
		return result;

	// convert val
	result = kstrtou8(val_s, 16, &val);
	if (result < 0)
		return result;

	// write val to EC[addr]
	result = ec_write(addr, val);
	if (result < 0)
		return result;

	return count;
}

// ec_get. stores the specified EC memory address. MAY BE UNSAFE!!!
static u8 ec_get_addr;

// ec_get. reads and stores the specified EC memory address. Format: "xx", xx - hex u8
static ssize_t ec_get_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	if (count > 3) // "xx\n" - 3 chars
		return -EINVAL;

	int result;
	char addr_s[3];

	result = sscanf(buf, "%2s", addr_s);
	if (result != 1)
		return -EINVAL;

	// convert addr
	result = kstrtou8(addr_s, 16, &ec_get_addr);
	if (result < 0)
		return result;

	return count;
};

// ec_get. prints value of previously stored EC memory address
static ssize_t ec_get_show(struct device *device,
			   struct device_attribute *attr,
			   char *buf)
{
	u8 rdata;
	int result;
	
	result = ec_read(ec_get_addr, &rdata);
	if (result < 0)
		return result;

	//	return sysfs_emit(buf, "%02x=%02x\n", ec_get_addr, rdata);
	return sysfs_emit(buf, "%02x\n", rdata);
};

static DEVICE_ATTR_RO(ec_dump);
static DEVICE_ATTR_WO(ec_set);
static DEVICE_ATTR_RW(ec_get);

static struct attribute *msi_debug_attrs[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_ec_dump.attr,
	&dev_attr_ec_set.attr,
	&dev_attr_ec_get.attr,
	NULL
};

// ============================================================ //
// Sysfs platform driver
// ============================================================ //

static umode_t msi_ec_is_visible(struct kobject *kobj,
				   struct attribute *attr,
				   int idx)
{
	int address;

	/* root group */
	if (attr == &dev_attr_webcam.attr)
		address = conf.webcam.address;

	else if (attr == &dev_attr_webcam_block.attr)
		address = conf.webcam.block_address;

	else if (attr == &dev_attr_fn_key.attr ||
		 attr == &dev_attr_win_key.attr)
		address = conf.fn_win_swap.address;

	else if (attr == &dev_attr_battery_mode.attr)
		address = conf.charge_control.address;

	else if (attr == &dev_attr_cooler_boost.attr)
		address = conf.cooler_boost.address;

	else if (attr == &dev_attr_available_shift_modes.attr ||
		 attr == &dev_attr_shift_mode.attr)
		address = conf.shift_mode.address;

	else if (attr == &dev_attr_super_battery.attr)
		address = conf.super_battery.address;

	else if (attr == &dev_attr_available_fan_modes.attr ||
		 attr == &dev_attr_fan_mode.attr)
		address = conf.fan_mode.address;

	/* cpu group */
	else if (attr == &dev_attr_cpu_realtime_temperature.attr)
		address = conf.cpu.rt_temp_address;

	else if (attr == &dev_attr_cpu_realtime_fan_speed.attr)
		address = conf.cpu.rt_fan_speed_address;

	else if (attr == &dev_attr_cpu_basic_fan_speed.attr)
		address = conf.cpu.bs_fan_speed_address;

	/* gpu group */
	else if (attr == &dev_attr_gpu_realtime_temperature.attr)
		address = conf.gpu.rt_temp_address;

	else if (attr == &dev_attr_gpu_realtime_fan_speed.attr)
		address = conf.gpu.rt_fan_speed_address;

	/* default */
	else
		return attr->mode;

	return address == MSI_EC_ADDR_UNSUPP ? 0 : attr->mode;
}

static struct attribute_group msi_root_group = {
	.is_visible = msi_ec_is_visible,
	.attrs = msi_root_attrs,
};

static struct attribute_group msi_cpu_group = {
	.name = "cpu",
	.is_visible = msi_ec_is_visible,
	.attrs = msi_cpu_attrs,
};
static struct attribute_group msi_gpu_group = {
	.name = "gpu",
	.is_visible = msi_ec_is_visible,
	.attrs = msi_gpu_attrs,
};

static const struct attribute_group msi_debug_group = {
	.name = "debug",
	.attrs = msi_debug_attrs,
};

/* the debug group is created separately if needed */
static const struct attribute_group *msi_platform_groups[] = {
	&msi_root_group,
	&msi_cpu_group,
	&msi_gpu_group,
	NULL
};

static int msi_platform_probe(struct platform_device *pdev)
{
	if (debug) {
		int result = sysfs_create_group(&pdev->dev.kobj,
						&msi_debug_group);
		if (result < 0)
			return result;
	}

	if (!conf_loaded) // an unsupported device loaded in debug mode
		return 0;

	return sysfs_create_groups(&pdev->dev.kobj, msi_platform_groups);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0))
static void msi_platform_remove(struct platform_device *pdev)
#else
static int msi_platform_remove(struct platform_device *pdev)
#endif
{
	if (debug)
		sysfs_remove_group(&pdev->dev.kobj, &msi_debug_group);

	if (conf_loaded)
		sysfs_remove_groups(&pdev->dev.kobj, msi_platform_groups);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0))
	return 0;
#endif
}

static struct platform_device *msi_platform_device;

static struct platform_driver msi_platform_driver = {
	.driver = {
		.name = MSI_EC_DRIVER_NAME,
	},
	.probe = msi_platform_probe,
	.remove = msi_platform_remove,
};

// ============================================================ //
// Sysfs leds subsystem
// ============================================================ //

static int micmute_led_sysfs_set(struct led_classdev *led_cdev,
				 enum led_brightness brightness)
{
	int result;

	result = ec_set_bit(conf.leds.micmute_led_address, conf.leds.bit, brightness);

	if (result < 0)
		return result;

	return 0;
}

static int mute_led_sysfs_set(struct led_classdev *led_cdev,
			      enum led_brightness brightness)
{
	int result;

	result = ec_set_bit(conf.leds.mute_led_address, conf.leds.bit, brightness);

	if (result < 0)
		return result;

	return 0;
}

static enum led_brightness kbd_bl_sysfs_get(struct led_classdev *led_cdev)
{
	u8 rdata;
	int result = ec_read(conf.kbd_bl.bl_state_address, &rdata);
	if (result < 0)
		return 0;
	return rdata & MSI_EC_KBD_BL_STATE_MASK;
}

static int kbd_bl_sysfs_set(struct led_classdev *led_cdev,
			    enum led_brightness brightness)
{
	// By default, on an unregister event, 
	// kernel triggers the setter with 0 brightness.
	if (led_cdev->flags & LED_UNREGISTERING) 
		return 0;

	u8 wdata;
	if (brightness < 0 || brightness > 3)
		return -1;
	wdata = conf.kbd_bl.state_base_value | brightness;
	return ec_write(conf.kbd_bl.bl_state_address, wdata);
}

static struct led_classdev micmute_led_cdev = {
	.name = "platform::micmute",
	.max_brightness = 1,
	.brightness_set_blocking = &micmute_led_sysfs_set,
	.default_trigger = "audio-micmute",
};

static struct led_classdev mute_led_cdev = {
	.name = "platform::mute",
	.max_brightness = 1,
	.brightness_set_blocking = &mute_led_sysfs_set,
	.default_trigger = "audio-mute",
};

static struct led_classdev msiacpi_led_kbdlight = {
	.name = "msiacpi::kbd_backlight",
	.max_brightness = 3,
	.flags = LED_BRIGHT_HW_CHANGED,
	.brightness_set_blocking = &kbd_bl_sysfs_set,
	.brightness_get = &kbd_bl_sysfs_get,
};

// ============================================================ //
// Module load/unload
// ============================================================ //

// must be called before msi_platform_probe()
static int __init load_configuration(void)
{
	int result;

	char *ver;
	char ver_by_ec[MSI_EC_FW_VERSION_LENGTH + 1]; // to store version read from EC

	if (firmware) {
		// use fw version passed as a parameter
		ver = firmware;
	} else {
		// get fw version from EC
		result = ec_get_firmware_version(ver_by_ec);
		if (result < 0) {
			return result;
		}

		ver = ver_by_ec;
	}

	// load the suitable configuration, if exists
	for (int i = 0; CONFIGURATIONS[i]; i++) {
		if (match_string(CONFIGURATIONS[i]->allowed_fw, -1, ver) != -EINVAL) {
			memcpy(&conf,
			       CONFIGURATIONS[i],
			       sizeof(struct msi_ec_conf));
			conf.allowed_fw = NULL;
			conf_loaded = true;
			return 0;
		}
	}

	// debug mode works regardless of whether the firmware is supported
	if (debug)
		return 0;

	pr_err("Your firmware version is not supported!\n");
	return -EOPNOTSUPP;
}

static int __init msi_ec_init(void)
{
	int result;

	result = load_configuration();
	if (result < 0)
		return result;

	result = platform_driver_register(&msi_platform_driver);
	if (result < 0)
		return result;

	msi_platform_device = platform_device_alloc(MSI_EC_DRIVER_NAME, -1);
	if (msi_platform_device == NULL) {
		platform_driver_unregister(&msi_platform_driver);
		return -ENOMEM;
	}

	result = platform_device_add(msi_platform_device);
	if (result < 0) {
		platform_device_del(msi_platform_device);
		platform_driver_unregister(&msi_platform_driver);
		return result;
	}

	if (conf_loaded) {
		battery_hook_register(&battery_hook);

		// register LED classdevs
		if (conf.leds.micmute_led_address != MSI_EC_ADDR_UNSUPP)
			led_classdev_register(&msi_platform_device->dev,
					      &micmute_led_cdev);

		if (conf.leds.mute_led_address != MSI_EC_ADDR_UNSUPP)
			led_classdev_register(&msi_platform_device->dev,
					      &mute_led_cdev);

		if (conf.kbd_bl.bl_state_address != MSI_EC_ADDR_UNSUPP)
			led_classdev_register(&msi_platform_device->dev,
					      &msiacpi_led_kbdlight);
	}

	pr_info("module_init\n");
	return 0;
}

static void __exit msi_ec_exit(void)
{
	if (conf_loaded) {
		// unregister LED classdevs
		if (conf.leds.micmute_led_address != MSI_EC_ADDR_UNSUPP)
			led_classdev_unregister(&micmute_led_cdev);

		if (conf.leds.mute_led_address != MSI_EC_ADDR_UNSUPP)
			led_classdev_unregister(&mute_led_cdev);

		if (conf.kbd_bl.bl_state_address != MSI_EC_ADDR_UNSUPP)
			led_classdev_unregister(&msiacpi_led_kbdlight);

		battery_hook_unregister(&battery_hook);
	}

	platform_driver_unregister(&msi_platform_driver);
	platform_device_del(msi_platform_device);

	pr_info("module_exit\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jose Angel Pastrana <japp0005@red.ujaen.es>");
MODULE_AUTHOR("Aakash Singh <mail@singhaakash.dev>");
MODULE_AUTHOR("Nikita Kravets <teackot@gmail.com>");
MODULE_DESCRIPTION("MSI Embedded Controller");
MODULE_VERSION("0.08");

module_init(msi_ec_init);
module_exit(msi_ec_exit);
