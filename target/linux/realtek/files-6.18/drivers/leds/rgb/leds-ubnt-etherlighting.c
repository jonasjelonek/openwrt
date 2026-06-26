// SPDX-License-Identifier: GPL-2.0-only

/*
 * UBNT EtherLighting — per-port RGBW status LED driver
 * ======================================================
 *
 * Hardware overview
 * ------------------
 * Each switch port has an RGBW status LED. These are not driven directly
 * by the switch ASIC or this driver; they are controlled by a satellite
 * microcontroller (Nuvoton M031LE3AE, ARM Cortex-M0+, I2C address 0x66)
 * that this driver talks to directly.
 *
 * The MCU's I2C application protocol is a small, undocumented binary
 * command set, recovered by reverse-engineering the MCU firmware image
 * (v1.19.5, Nov 2024) and cross-checking against real traffic captured
 * from the vendor userspace daemon ("etherlightd").
 *
 * Confidence levels used in this file:
 *   CONFIRMED — directly observed in etherlightd I2C captures or
 *               empirically verified on hardware
 *   INFERRED  — deduced from Ghidra decompile of MCU APROM; not yet
 *               tested on hardware
 *   SPECULATIVE — not deduced reliably; marked for empirical verification
 *                 via the debugfs interface below
 *
 * Key commands (all CONFIRMED unless noted):
 *
 *   0x20 <port> <flags> <Rhi Rlo> <Ghi Glo> <Bhi Blo> <Whi Wlo> <flags2> <crc>
 *       Stages a 16-bit RGBW color for one port. Takes effect after 0x1f.
 *       'flags' selects the effect: 0x00=scan-dependent breathing,
 *       0x01-0x03=pulse with a shared period, 0x08=static.
 *       'flags2' bit 0 auto-commits without a separate 0x1f
 *       (INFERRED from FUN_0000a614 in firmware; not yet empirically tested).
 *
 *   0x1f 0x00  — commit all pending RGBW values to active display
 *   0x02 0x01  — stop welcome animation, begin transition to ready state
 *
 *   0x00 <subcmd> <flags> <pad×5> <mask[6]>..<mask[0]> <crc> — mode selection
 *
 * CRC-8: poly 0x07, MSB-first, no reflection. Computed over the command's
 * data bytes only (not the command register byte). Uses crc8_populate_msb().
 * Transport: i2c_smbus_write_i2c_block_data() — no extra SMBus length byte.
 *
 * Animation mode (verified on USW Pro Max 24 PoE, MCU fw 1.19.4)
 * ------------------------------------------------------
 * Each port has pending/active mode bytes at firmware struct offsets 2/3.
 * Commit copies the pending mode to the active mode, which controls output:
 *
 *   0x01 — hardware-scan: RTL9313 serial scan bit gates output; RGBW ×
 *           brightness is applied only when the scan bit is set (link up)
 *   0x00 — static: no link gating; the selected color effect still applies
 *   0xFF/0xFE/0xFD — welcome-animation modes: the MCU reads from global
 *           animation tables, ignoring per-port RGBW entirely
 *
 * After stop_welcome, ports may stay in animation mode. A mode-bitmask
 * command (MCU handler FUN_0000a708, confirmed I2C byte 0x00) resets the
 * mode byte for selected ports, re-applies stored colors, and commits. This
 * is sent in probe before the baseline writes.
 *
 * What this driver does NOT do
 * -----------------------------
 * Link visibility in hardware-scan mode is controlled by the RTL9313 serial
 * LED-scan output, configured by the switch/DSA driver — not this driver.
 * Colors are set exclusively through the LED class device (sysfs). The DSA
 * driver does not set LED colors.
 */

#include <linux/crc8.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

/* I2C command bytes */
#define ETHERLIGHT_CMD_SET_COLOR	0x20	/* CONFIRMED */
#define ETHERLIGHT_CMD_COMMIT		0x1f	/* CONFIRMED */
/*
 * ETHERLIGHT_CMD_MODE_BITMASK — bitmask animation-mode reset.
 * MCU handler: FUN_0000a708 (CONFIRMED by firmware dispatch table at 0x758c).
 * Command byte 0x00: CONFIRMED — dispatch: bVar1==0 → FUN_0000a708.
 * The receive layer validates CRC before dispatching to FUN_0000a708.
 * Send 14 data bytes (subcmd, flags, pad×5, mask×7), followed by CRC8.
 * Note: cmd 0x21 (previously speculated) actually routes to FUN_0000a454
 * which is a "Set Color Range" command — completely different purpose.
 */
#define ETHERLIGHT_CMD_MODE_BITMASK	0x00	/* CONFIRMED */

/*
 * Effect values verified on MCU fw 1.19.4. Pulse selection is per port, but
 * values 0x01-0x03 set one controller-wide period. The 1.19.5 effect setter
 * updates the shared oscillator while staging color, before commit.
 * Effect 0x00 breathes in HW_SCAN mode and is steady in STATIC mode.
 */
#define ETHERLIGHT_FLAGS_BREATHING	0x00
#define ETHERLIGHT_FLAGS_PULSE_1S	0x01
#define ETHERLIGHT_FLAGS_PULSE_2S	0x02
#define ETHERLIGHT_FLAGS_PULSE_3S	0x03
#define ETHERLIGHT_FLAGS_STATIC		0x08

/*
 * flags2 byte in cmd 0x20 — INFERRED from MCU firmware (FUN_0000a614):
 * if (flags2 & 1) → call FUN_0000ba94(port) = per-port commit inside handler.
 * Not yet empirically tested; the debugfs interface uses explicit commits.
 */
#define ETHERLIGHT_FLAGS2_AUTOCOMMIT	BIT(0)	/* INFERRED */

/*
 * MCU port animation mode byte values (offset[2]/offset[3] in firmware struct).
 * These are the values stored in the MCU — not the I2C sub-command bytes.
 * Verified on USW Pro Max 24 PoE (fw 1.19.4), and in the 1.19.5 firmware:
 * FUN_0000ba94 copies offset[2] to offset[3]; FUN_0000bca8 uses offset[3].
 */
#define ETHERLIGHT_MODE_STATIC		0x00	/* output independent of serial scan */
#define ETHERLIGHT_MODE_HW_SCAN		0x01	/* RTL9313 serial scan gates output */

/*
 * sub-commands for ETHERLIGHT_CMD_MODE_BITMASK — verified on MCU fw 1.19.4:
 * sub-cmd 2 → local_24 = 2-2 = 0 → FUN_0000c168(port, 0) → offset[2] = 0 (STATIC)
 * sub-cmd 3 → local_24 = 3-2 = 1 → FUN_0000c168(port, 1) → offset[2] = 1 (HW_SCAN)
 */
#define ETHERLIGHT_SUBCMD_MODE_STATIC	0x02
#define ETHERLIGHT_SUBCMD_MODE_HW_SCAN	0x03
#define ETHERLIGHT_MODE_AUTOCOMMIT	BIT(4)

#define ETHERLIGHT_MAX_PORTS		56	/* 7-byte bitmask in mode commands = 56 ports max */
#define ETHERLIGHT_NUM_COLORS		4	/* R, G, B, W */
#define ETHERLIGHT_CRC8_POLY		0x07

enum etherlight_effect {
	ETHERLIGHT_EFFECT_STATIC,
	ETHERLIGHT_EFFECT_BREATHING,
	ETHERLIGHT_EFFECT_PULSE,
};

struct etherlight_port {
	struct etherlight *priv;
	struct led_classdev_mc mc_cdev;
	struct mc_subled subled[ETHERLIGHT_NUM_COLORS];
	u8 port;
	enum etherlight_effect effect;
	u8 mode;	/* last mode sent via CMD_MODE_BITMASK; default HW_SCAN */
	u16 r, g, b, w;	/* last RGBW requested through the LED class */
};

struct etherlight {
	struct i2c_client *client;
	struct mutex lock;		/* serializes the set-color + commit sequence */
	unsigned int num_ports;
	u8 pulse_flags;			/* shared ETHERLIGHT_FLAGS_PULSE_* period */
	struct etherlight_port ports[];
};

DECLARE_CRC8_TABLE(etherlight_crc8_table);

/* Must be called with priv->lock held. Pulse ports always use shared timing. */
static u8 etherlight_effect_flags(struct etherlight_port *port)
{
	if (port->effect == ETHERLIGHT_EFFECT_PULSE)
		return port->priv->pulse_flags;
	if (port->effect == ETHERLIGHT_EFFECT_BREATHING)
		return ETHERLIGHT_FLAGS_BREATHING;
	return ETHERLIGHT_FLAGS_STATIC;
}

static int etherlight_write_color(struct etherlight *priv, u8 port, u8 flags,
				  u16 r, u16 g, u16 b, u16 w, u8 flags2)
{
	u8 buf[12];

	buf[0] = port;
	buf[1] = flags;
	buf[2] = r >> 8;
	buf[3] = r & 0xff;
	buf[4] = g >> 8;
	buf[5] = g & 0xff;
	buf[6] = b >> 8;
	buf[7] = b & 0xff;
	buf[8] = w >> 8;
	buf[9] = w & 0xff;
	buf[10] = flags2;
	buf[11] = crc8(etherlight_crc8_table, buf, 11, 0);

	return i2c_smbus_write_i2c_block_data(priv->client, ETHERLIGHT_CMD_SET_COLOR,
					      sizeof(buf), buf);
}

static int etherlight_commit(struct etherlight *priv)
{
	static const u8 buf[1] = { 0x00 };

	return i2c_smbus_write_i2c_block_data(priv->client, ETHERLIGHT_CMD_COMMIT,
					      sizeof(buf), buf);
}

/*
 * etherlight_set_mode_bitmask - reset animation mode for a set of ports
 * @subcmd: ETHERLIGHT_SUBCMD_MODE_STATIC (0x02) or MODE_HW_SCAN (0x03)
 * @flags:  color effect in the low nibble, plus optional auto-commit bit
 * @mask:   port bitmask, bit N = port N; MSB-first in the 7-byte packet field
 *
 * Verified packet layout (15 data bytes after cmd byte 0x00):
 *   [subcmd, flags, pad×5, mask_byte[6..0], crc8]
 *   byte 1 bit4 = auto-commit flag (FUN_0000a708 calls FUN_0000ba94 per port)
 *   bytes 7-13: 7-byte bitmask, MSB-first (byte 7 = ports 55-48, byte 13 = ports 7-0)
 *   byte 14: CRC8 over bytes 0-13, checked by the receive layer
 *
 * The MCU handler re-applies stored pending colors after mode change, so
 * calling this before baseline writes is safe — the baseline overwrites them.
 */
static int etherlight_set_mode_bitmask(struct etherlight *priv, u8 subcmd,
				       u8 flags, u64 mask)
{
	u8 buf[15];

	buf[0] = subcmd;
	buf[1] = flags;
	memset(buf + 2, 0, 5);
	buf[7]  = (mask >> 48) & 0xff;
	buf[8]  = (mask >> 40) & 0xff;
	buf[9]  = (mask >> 32) & 0xff;
	buf[10] = (mask >> 24) & 0xff;
	buf[11] = (mask >> 16) & 0xff;
	buf[12] = (mask >> 8) & 0xff;
	buf[13] = mask & 0xff;
	buf[14] = crc8(etherlight_crc8_table, buf, 14, 0);

	return i2c_smbus_write_i2c_block_data(priv->client, ETHERLIGHT_CMD_MODE_BITMASK,
					      sizeof(buf), buf);
}

/* Must be called with priv->lock held. */
static int etherlight_set_mode_port(struct etherlight_port *port, u8 mode)
{
	struct etherlight *priv = port->priv;
	u8 subcmd, flags;
	int ret;

	subcmd = mode == ETHERLIGHT_MODE_STATIC ? ETHERLIGHT_SUBCMD_MODE_STATIC :
						ETHERLIGHT_SUBCMD_MODE_HW_SCAN;
	flags = etherlight_effect_flags(port) | ETHERLIGHT_MODE_AUTOCOMMIT;

	ret = etherlight_set_mode_bitmask(priv, subcmd, flags, BIT_ULL(port->port));
	if (!ret)
		port->mode = mode;
	return ret;
}

static inline u16 etherlight_scale16(u8 val)
{
	return val | ((u16)val << 8);
}

static int etherlight_brightness_set(struct led_classdev *cdev,
				     enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	struct etherlight_port *port = container_of(mc_cdev, struct etherlight_port, mc_cdev);
	struct etherlight *priv = port->priv;
	int ret;

	led_mc_calc_color_components(mc_cdev, brightness);
	mutex_lock(&priv->lock);

	port->r = etherlight_scale16(mc_cdev->subled_info[0].brightness);
	port->g = etherlight_scale16(mc_cdev->subled_info[1].brightness);
	port->b = etherlight_scale16(mc_cdev->subled_info[2].brightness);
	port->w = etherlight_scale16(mc_cdev->subled_info[3].brightness);

	ret = etherlight_write_color(priv, port->port, etherlight_effect_flags(port),
				     port->r, port->g, port->b, port->w, 0);
	if (!ret)
		ret = etherlight_commit(priv);

	mutex_unlock(&priv->lock);
	return ret;
}

/*
 * etherlight_set_port_effect - change animation style for one port
 *
 * Pulse timing changes affect all pulsing ports. Re-send the current color
 * and commit the selected effect. Must be called with priv->lock held.
 */
static int etherlight_set_port_effect(struct etherlight_port *port,
				      enum etherlight_effect effect, u8 flags)
{
	struct etherlight *priv = port->priv;
	int ret;

	ret = etherlight_write_color(priv, port->port, flags,
				     port->r, port->g, port->b, port->w, 0);
	if (ret)
		return ret;

	/* The shared timer changes before commit, even if commit later fails. */
	if (effect == ETHERLIGHT_EFFECT_PULSE)
		priv->pulse_flags = flags;

	ret = etherlight_commit(priv);
	if (!ret)
		port->effect = effect;
	return ret;
}

/*
 * Selecting pulse-Xs sets the period for every pulsing port; reads on those
 * ports report that shared period. Static and scan-breathing selections remain
 * per port. Scan-breathing requires HW_SCAN mode; STATIC mode makes it steady.
 */
static const struct {
	const char *name;
	enum etherlight_effect effect;
	u8 flags;
} etherlight_effects[] = {
	{ "static",	ETHERLIGHT_EFFECT_STATIC,	ETHERLIGHT_FLAGS_STATIC },
	{ "scan-breathing", ETHERLIGHT_EFFECT_BREATHING,	ETHERLIGHT_FLAGS_BREATHING },
	{ "pulse-1s",	ETHERLIGHT_EFFECT_PULSE,		ETHERLIGHT_FLAGS_PULSE_1S },
	{ "pulse-2s",	ETHERLIGHT_EFFECT_PULSE,		ETHERLIGHT_FLAGS_PULSE_2S },
	{ "pulse-3s",	ETHERLIGHT_EFFECT_PULSE,		ETHERLIGHT_FLAGS_PULSE_3S },
};

static struct etherlight_port *etherlight_dev_to_port(struct device *dev)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);

	return container_of(lcdev_to_mccdev(cdev), struct etherlight_port, mc_cdev);
}

static ssize_t effect_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct etherlight_port *port = etherlight_dev_to_port(dev);
	int len = 0;
	u8 flags;
	int i;

	mutex_lock(&port->priv->lock);
	flags = etherlight_effect_flags(port);
	mutex_unlock(&port->priv->lock);

	for (i = 0; i < ARRAY_SIZE(etherlight_effects); i++)
		len += sysfs_emit_at(buf, len,
				     etherlight_effects[i].flags == flags ?
				     "[%s] " : "%s ", etherlight_effects[i].name);
	buf[len - 1] = '\n';

	return len;
}

static ssize_t effect_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t size)
{
	struct etherlight_port *port = etherlight_dev_to_port(dev);
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(etherlight_effects); i++)
		if (sysfs_streq(buf, etherlight_effects[i].name))
			break;
	if (i == ARRAY_SIZE(etherlight_effects))
		return -EINVAL;

	mutex_lock(&port->priv->lock);
	ret = etherlight_set_port_effect(port, etherlight_effects[i].effect,
					 etherlight_effects[i].flags);
	mutex_unlock(&port->priv->lock);

	return ret ? ret : size;
}
static DEVICE_ATTR_RW(effect);

/* Mode controls scan gating; effect 0x00 only breathes in HW_SCAN mode. */
static const char * const etherlight_modes[] = {
	[ETHERLIGHT_MODE_STATIC] = "static",
	[ETHERLIGHT_MODE_HW_SCAN] = "hw_scan",
};

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct etherlight_port *port = etherlight_dev_to_port(dev);
	int len = 0;
	u8 mode;
	int i;

	mutex_lock(&port->priv->lock);
	mode = port->mode;
	mutex_unlock(&port->priv->lock);

	for (i = 0; i < ARRAY_SIZE(etherlight_modes); i++)
		len += sysfs_emit_at(buf, len, i == mode ? "[%s] " : "%s ",
				     etherlight_modes[i]);
	buf[len - 1] = '\n';

	return len;
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t size)
{
	struct etherlight_port *port = etherlight_dev_to_port(dev);
	int mode, ret;

	mode = sysfs_match_string(etherlight_modes, buf);
	if (mode < 0)
		return mode;

	mutex_lock(&port->priv->lock);
	ret = etherlight_set_mode_port(port, mode);
	mutex_unlock(&port->priv->lock);

	return ret ? ret : size;
}
static DEVICE_ATTR_RW(mode);

static struct attribute *etherlight_port_attrs[] = {
	&dev_attr_effect.attr,
	&dev_attr_mode.attr,
	NULL
};

/* Added after registration: the multicolor core overwrites led_cdev->groups. */
static const struct attribute_group etherlight_port_group = {
	.attrs = etherlight_port_attrs,
};

/* ---- debugfs interface -------------------------------------------------- */

/*
 * /sys/kernel/debug/<dev>/raw_cmd: arbitrary writes for protocol exploration.
 * Format: space-separated hex bytes; first byte = I2C command register,
 * remainder = data payload. No CRC is added — include it explicitly if
 * needed, or omit to test MCU CRC rejection behavior.
 *
 * Correct raw_cmd format for cmd 0x00 mode selection (16 hex values total):
 *   cmd=0x00, subcmd, flags=effect|0x10, 5×pad, 7-byte-mask MSB-first, CRC8
 *   port 0 hw_scan: echo "00 03 18 00 00 00 00 00 00 00 00 00 00 00 01 ec" > raw_cmd
 *   port 0 static:  echo "00 02 18 00 00 00 00 00 00 00 00 00 00 00 01 09" > raw_cmd
 * These select steady color and auto-commit. Raw writes bypass cached state.
 * An explicit global commit can be sent with "1f 00".
 */
static ssize_t etherlight_dbg_raw_write(struct file *file, const char __user *ubuf,
					size_t count, loff_t *ppos)
{
	struct etherlight *priv = file->private_data;
	u8 cmd, data[I2C_SMBUS_BLOCK_MAX];
	char buf[128];
	unsigned int val;
	char *p, *tok;
	ssize_t len;
	int n = 0, ret;

	len = min_t(ssize_t, count, sizeof(buf) - 1);
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	p = buf;
	tok = strsep(&p, " \t\n");
	if (!tok || !*tok || kstrtouint(tok, 16, &val) || val > 0xff)
		return -EINVAL;
	cmd = val;

	while ((tok = strsep(&p, " \t\n")) != NULL && n < I2C_SMBUS_BLOCK_MAX) {
		if (!*tok)
			continue;
		if (kstrtouint(tok, 16, &val) || val > 0xff)
			return -EINVAL;
		data[n++] = val;
	}

	mutex_lock(&priv->lock);
	ret = i2c_smbus_write_i2c_block_data(priv->client, cmd, n, data);
	mutex_unlock(&priv->lock);

	return ret ? ret : count;
}

static const struct file_operations etherlight_dbg_raw_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = etherlight_dbg_raw_write,
	.llseek = noop_llseek,
};

static void etherlight_debugfs_remove(void *root)
{
	debugfs_remove_recursive(root);
}

static void etherlight_debugfs_init(struct etherlight *priv)
{
	struct dentry *root;

	root = debugfs_create_dir(dev_name(&priv->client->dev), NULL);
	if (IS_ERR(root))
		return;

	if (devm_add_action_or_reset(&priv->client->dev, etherlight_debugfs_remove, root))
		return;

	debugfs_create_file("raw_cmd", 0200, root, priv, &etherlight_dbg_raw_fops);
}

/* ---- driver core -------------------------------------------------------- */

static int etherlight_stop_welcome(struct etherlight *priv)
{
	static const u8 buf[1] = { 0x01 };

	return i2c_smbus_write_i2c_block_data(priv->client, 0x02, sizeof(buf), buf);
}

static int etherlight_read_fw_version(struct etherlight *priv, u8 ver[3])
{
	return i2c_smbus_read_i2c_block_data(priv->client, 0x70, 3, ver);
}

static int etherlight_wait_ready(struct etherlight *priv)
{
	int ret, tries = 15;
	u8 val;

	/*
	 * The MCU needs ~4s to transition from welcome to normal operation.
	 * Poll register 0x03 — 0x00 means ready.
	 */
	do {
		msleep(500);
		ret = i2c_smbus_read_i2c_block_data(priv->client, 0x03, 1, &val);
		if (ret == 1 && val == 0x00)
			return 0;
	} while (--tries);

	return -ETIMEDOUT;
}

static int etherlight_reset_mcu(struct etherlight *priv)
{
	struct gpio_desc *reset_gpio, *boot_mode_gpio;
	int ret = 0;

	reset_gpio = gpiod_get_optional(&priv->client->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio))
		return dev_err_probe(&priv->client->dev, PTR_ERR(reset_gpio),
				     "failed to get reset GPIO\n");
	if (!reset_gpio)
		return 0;

	boot_mode_gpio = gpiod_get_optional(&priv->client->dev, "boot-mode", GPIOD_OUT_HIGH);
	if (IS_ERR(boot_mode_gpio)) {
		ret = dev_err_probe(&priv->client->dev, PTR_ERR(boot_mode_gpio),
				    "failed to get boot-mode GPIO\n");
		goto put_reset;
	}

	gpiod_set_value_cansleep(reset_gpio, 1);
	msleep(10);
	gpiod_set_value_cansleep(reset_gpio, 0);
	msleep(1000);

	gpiod_put(boot_mode_gpio);
put_reset:
	gpiod_put(reset_gpio);
	return ret;
}

static int etherlight_probe_port(struct etherlight *priv, struct device_node *np,
				 struct etherlight_port *port)
{
	struct led_init_data init_data = { .fwnode = of_fwnode_handle(np) };
	int ret, tries = 5;
	u32 reg;

	ret = of_property_read_u32(np, "reg", &reg);
	if (ret)
		return ret;
	if (reg >= priv->num_ports)
		return -EINVAL;

	port->priv = priv;
	port->port = reg;
	port->effect = ETHERLIGHT_EFFECT_STATIC;
	port->mode = ETHERLIGHT_MODE_HW_SCAN;

	/* Stage zero baseline; committed globally after the probe loop. */
	do {
		ret = etherlight_write_color(priv, port->port, ETHERLIGHT_FLAGS_STATIC,
					     0, 0, 0, 0, 0);
		if (!ret)
			break;
		msleep(50);
	} while (--tries);

	if (ret) {
		dev_err_probe(&priv->client->dev, ret, "port %d: init write failed\n", port->port);
		return ret;
	}

	port->subled[0].color_index = LED_COLOR_ID_RED;
	port->subled[1].color_index = LED_COLOR_ID_GREEN;
	port->subled[2].color_index = LED_COLOR_ID_BLUE;
	port->subled[3].color_index = LED_COLOR_ID_WHITE;

	port->mc_cdev.led_cdev.brightness_set_blocking = etherlight_brightness_set;
	port->mc_cdev.led_cdev.max_brightness = 255;
	port->mc_cdev.num_colors = ETHERLIGHT_NUM_COLORS;
	port->mc_cdev.subled_info = port->subled;

	ret = devm_led_classdev_multicolor_register_ext(&priv->client->dev,
						      &port->mc_cdev, &init_data);
	if (ret)
		return ret;

	return device_add_group(port->mc_cdev.led_cdev.dev, &etherlight_port_group);
}

static int etherlight_probe(struct i2c_client *client)
{
	struct device_node *np = client->dev.of_node;
	unsigned int i = 0, num_ports = 0;
	struct etherlight *priv;
	u8 ver[3];
	int ret;

	num_ports = of_get_child_count(np);
	if (!num_ports || num_ports > ETHERLIGHT_MAX_PORTS)
		return -EINVAL;

	priv = devm_kzalloc(&client->dev, struct_size(priv, ports, num_ports),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->num_ports = num_ports;
	priv->pulse_flags = ETHERLIGHT_FLAGS_PULSE_1S;
	mutex_init(&priv->lock);
	crc8_populate_msb(etherlight_crc8_table, ETHERLIGHT_CRC8_POLY);

	ret = etherlight_reset_mcu(priv);
	if (ret)
		return ret;

	ret = etherlight_read_fw_version(priv, ver);
	if (ret != 3)
		dev_warn(&client->dev, "could not read MCU firmware version: %d\n", ret);

	ret = etherlight_stop_welcome(priv);
	if (ret)
		dev_warn(&client->dev, "failed to send welcome-exit command: %d\n", ret);

	ret = etherlight_wait_ready(priv);
	if (ret)
		return ret;

	/* Establish the mode reported by each LED before registering it. */
	ret = etherlight_set_mode_bitmask(priv, ETHERLIGHT_SUBCMD_MODE_HW_SCAN,
					 ETHERLIGHT_FLAGS_STATIC | ETHERLIGHT_MODE_AUTOCOMMIT,
					 BIT_ULL(num_ports) - 1);
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to initialize port modes\n");

	for_each_child_of_node_scoped(np, child) {
		ret = etherlight_probe_port(priv, child, &priv->ports[i++]);
		if (ret) {
			dev_err(&client->dev, "port probe failed\n");
			return ret;
		}
	}

	ret = etherlight_commit(priv);
	if (ret) {
		dev_err_probe(&client->dev, ret, "failed to commit init settings\n");
		return ret;
	}

	dev_info(&client->dev, "MCU firmware v%u.%u.%u\n", ver[0], ver[1], ver[2]);

	i2c_set_clientdata(client, priv);
	etherlight_debugfs_init(priv);
	return 0;
}

static const struct of_device_id etherlight_of_match[] = {
	{ .compatible = "ubnt,etherlighting" },
	{ }
};
MODULE_DEVICE_TABLE(of, etherlight_of_match);

static struct i2c_driver etherlight_driver = {
	.driver = {
		.name = "leds-ubnt-etherlighting",
		.of_match_table = etherlight_of_match,
	},
	.probe = etherlight_probe,
};
module_i2c_driver(etherlight_driver);

MODULE_DESCRIPTION("UBNT EtherLighting per-port RGBW LED driver");
MODULE_LICENSE("GPL");
