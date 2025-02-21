/* MICROCHIP Level Driver for PolarFire SoC
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2020 Microchip, Inc.
 */

#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/kernel.h>
#include <linux/io.h>

/* Clock output phandle list
 * 0: CPU Clock
 * 1: AXI Clock
 * 2: AHB Clock (APB Clock)
 *
 * 3 + Bit Position of peripheral_subblock_name_array ::
 * 3: ENVM Clock
 * 4: MAC0 Clock
 * 5: MAC1 Clock
 * 6: MMC Clock
 * 7: TIMER Clock
 * 8: MMUART0 Clock
 * 9: MMUART1 Clock
 * 10: MMUART2 Clock
 * 11: MMUART3 Clock
 * 12: MMUART4 Clock
 * 13: SPI0 Clock
 * 14: SPI1 Clock
 * 15: I2C0 Clock
 * 16: I2C1 Clock
 * 17: CAN0 Clock
 * 18: CAN1 Clock
 * 19: USB Clock
 * 20: RESERVED
 * 21: RTC Clock
 * 22: QSPI Clock
 * 23: GPIO0 Clock
 * 24: GPIO1 Clock
 * 25: GPIO2 Clock
 * 26: DDRC Clock
 * 27: FIC0 Clock
 * 28: FIC1 Clock
 * 29: FIC2 Clock
 * 30: FIC3 Clock
 * 31: ATHENA Clock
 * 32: CFM Clock
 */

/*Address offset of control registers*/
#define REG_CLOCK_CONFIG_CR 	0x08u
#define REG_SUBBLK_CLOCK_CR 	0x84u
/*SOFT_RESET_CR register renamed for clarity*/
#define REG_SUBBLK_RESET_CR 	0x88u

/*Definition of main clocks*/
#define CPU_CLOCK 0
#define AXI_CLOCK 1
#define AHB_CLOCK 2
/*Number of main clocks*/
#define CFG_CLOCKS 3
#define NO_CLOCK CFG_CLOCKS
#define NO_SUPPORT 32

/*MAX Frequency in Hz*/
#define CPU_CLOCK_MAX (800*1000*1000)
#define AXI_CLOCK_MAX (400*1000*1000)
#define AHB_CLOCK_MAX (200*1000*1000)

/*Non-Zero value allows for Clock Set capability*/
#define CPU_CLOCK_SET 1
#define AXI_CLOCK_SET 1
#define AHB_CLOCK_SET 1

#define NAME_LEN 40ULL

// Number of Gated peripheral sub blocks
#define NUM_SUBBLOCKS 30

/*Total number of clocks handled by this driver*/
#define TOTAL_CLOCKS (CFG_CLOCKS+NUM_SUBBLOCKS)

/*List of peripheral subblock names & associated refclk*/
/*(order is the bit offset order)*/
const char peripheral_subblock_name_array[][10] = {
	"ENVM",
	"MAC0",
	"MAC1",
	"MMC",
	"TIMER",
	"MMUART0",
	"MMUART1",
	"MMUART2",
	"MMUART3",
	"MMUART4",
	"SPI0",
	"SPI1",
	"I2C0",
	"I2C1",
	"CAN0",
	"CAN1",
	"USB",
	"RESERVED",
	"RTC",
	"QSPI",
	"GPIO0",
	"GPIO1",
	"GPIO2",
	"DDRC",
	"FIC0",
	"FIC1",
	"FIC2",
	"FIC3",
	"ATHENA",
	"CFM"
};
const int peripheral_subblock_refclk_array[NUM_SUBBLOCKS] = {
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	NO_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	NO_CLOCK,
	NO_SUPPORT,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	AHB_CLOCK,
	NO_CLOCK,
	NO_CLOCK,
	NO_CLOCK,
	NO_CLOCK,
	AHB_CLOCK,
	NO_CLOCK
};

struct microchip_clk_pfsoc_cfg {
	struct clk_hw hw;
	struct microchip_clk_pfsoc_driver *driver;
	char name[NAME_LEN];
	u32 subblk_offset;
	u32 divider_offset;
	unsigned long max;
};

struct microchip_clk_pfsoc_driver {
	struct clk_onecell_data table;
	struct clk *clks[TOTAL_CLOCKS];
	struct microchip_clk_pfsoc_cfg cfg[TOTAL_CLOCKS];
	void __iomem *reg;
};

#define to_microchip_clk_pfsoc_cfg(hw) container_of(hw, struct microchip_clk_pfsoc_cfg, hw)

/* Helper function to calculate the clock rate
 *
 * reg = register value of clock configuration
 * bit_offset = bit position for clock divider value
 * parent_rate = reference frequency
 *
 * returns calculated clock rate
 */
static unsigned long microchip_clk_pfsoc_rate(u32 reg, u32 bit_offset, unsigned long parent_rate)
{
	return parent_rate/(1 << ((reg >> bit_offset) & 0x00000003));
}

/* Determines closet rate supported by clock
 *
 */
static long microchip_clk_pfsoc_round_rate_helper(struct clk_hw *hw, unsigned long rate, unsigned long *parent_rate, u32 *divider_setting)
{

	struct microchip_clk_pfsoc_cfg *cfg = to_microchip_clk_pfsoc_cfg(hw);
	int divider_start;
	int min = INT_MAX;
	int setting;
	int i;
	int val;
	long rounded_rate;

	/* Max Frequency placed on rate depending on main clock type*/
	if (cfg->max < rate) {
		rate = cfg->max;
	}


	/* AHB CLOCK divider cannot be set to 0*/
	if (cfg->divider_offset == (AHB_CLOCK*2u)) {
		divider_start = 1;
	} else {
		divider_start = 0;
	}

	setting = 3;
	for (i = divider_start; i < 4; i++) {
		val = (*parent_rate / (1 << i)) - rate;
		if (val < 0)
			val = val*-1; // convert to absolute
		if (val < min) {
			min = val;
			setting = i;
		}
	}

	rounded_rate = *parent_rate / (1 << setting);

	*divider_setting = (u32)setting;

	return rounded_rate;
}

/* Determines closet rate supported by clock
 *
 */
static long microchip_clk_pfsoc_round_rate(struct clk_hw *hw, unsigned long rate, unsigned long *parent_rate)
{
	int divider_setting;
	return microchip_clk_pfsoc_round_rate_helper(hw, rate, parent_rate, &divider_setting);
}


/* Sets the main clock to a specific rate
 *
 */
static int microchip_clk_pfsoc_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{

	struct microchip_clk_pfsoc_cfg *cfg = to_microchip_clk_pfsoc_cfg(hw);
	struct microchip_clk_pfsoc_driver *driver = cfg->driver;
	u32 divider_setting, reg, val;

	/*determine closet clock rate to desired rate*/
	microchip_clk_pfsoc_round_rate_helper(hw, rate, &parent_rate, &divider_setting);
	divider_setting = divider_setting & 0x00000003u;

	/* set appropriate divider setting within REG_CLOCK_CONFIG_CR*/
	reg = readl(driver->reg + REG_CLOCK_CONFIG_CR);
	val = (reg & ~(3u << cfg->divider_offset)) | (divider_setting << cfg->divider_offset);
	writel(val, driver->reg + REG_CLOCK_CONFIG_CR);
	reg = readl(driver->reg + REG_CLOCK_CONFIG_CR);

	return (reg == val) ? 0 : -EIO;
}

/* Calculates the clock rate of one of the main clocks
 *
 */
static unsigned long microchip_clk_pfsoc_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct microchip_clk_pfsoc_cfg *cfg = to_microchip_clk_pfsoc_cfg(hw);
	struct microchip_clk_pfsoc_driver *driver = cfg->driver;
	u32 reg;

	reg = readl(driver->reg + REG_CLOCK_CONFIG_CR);
	return microchip_clk_pfsoc_rate(reg, cfg->divider_offset, parent_rate);
}


/* Enables one of the peripheral sub blocks
 *
 * First takes sub block out of reset & then proceeds to enable its clock
 *
 */
static int microchip_clk_pfsoc_enable(struct clk_hw *hw)
{
	u32 reg, val;

	struct microchip_clk_pfsoc_cfg *cfg = to_microchip_clk_pfsoc_cfg(hw);
	struct microchip_clk_pfsoc_driver *driver = cfg->driver;

	reg = readl(driver->reg + REG_SUBBLK_RESET_CR);
	val = reg & ~(1u << cfg->subblk_offset);
	writel(val, driver->reg + REG_SUBBLK_RESET_CR);

	reg = readl(driver->reg + REG_SUBBLK_CLOCK_CR);
	val = reg | (1u << cfg->subblk_offset);
	reg = readl(driver->reg + REG_SUBBLK_CLOCK_CR);

	return 0;
}

/* Disables one of the peripheral sub blocks
 *
 * First places sub block into reset & then proceeds to disable its clock
 *
 */
static void microchip_clk_pfsoc_disable(struct clk_hw *hw)
{
	u32 reg, val;

	struct microchip_clk_pfsoc_cfg *cfg = to_microchip_clk_pfsoc_cfg(hw);
	struct microchip_clk_pfsoc_driver *driver = cfg->driver;

	reg = readl(driver->reg + REG_SUBBLK_RESET_CR);
	val = reg | (1 << cfg->subblk_offset);
	reg = readl(driver->reg + REG_SUBBLK_RESET_CR);

	reg = readl(driver->reg + REG_SUBBLK_CLOCK_CR);
	val = reg & ~(1 << cfg->subblk_offset);
	reg = readl(driver->reg + REG_SUBBLK_CLOCK_CR);
}

/* Checks if one of the peripheral sub blocks is enabled
 *
 * For sub block to be considered enabled it must not be
 * in reset and have its clock enabled
 *
 * Returns 1 is enabled & 0 otherwise
 */
static int microchip_clk_pfsoc_is_enabled(struct clk_hw *hw)
{
	u32 reg;

	struct microchip_clk_pfsoc_cfg *cfg = to_microchip_clk_pfsoc_cfg(hw);
	struct microchip_clk_pfsoc_driver *driver = cfg->driver;

	/* check if subblk is out of reset */
	reg = readl(driver->reg + REG_SUBBLK_RESET_CR);
	if ((reg & (1u << cfg->subblk_offset)) == 0u) {
		/* check if subblk clock is enabled*/
		reg = readl(driver->reg + REG_SUBBLK_CLOCK_CR);
		if (reg & (1u << cfg->subblk_offset)) {
			return 1;
		} else {
			return 0;
		}
	}
	return 0;
}

/* definition for main clocks with capability of setting clock
 */
static const struct clk_ops microchip_clk_pfsoc_ops_rw = {
	.recalc_rate = microchip_clk_pfsoc_recalc_rate,
	.round_rate = microchip_clk_pfsoc_round_rate,
	.set_rate = microchip_clk_pfsoc_set_rate,
};

/* definition for main clocks without capability of setting clock
 */

static const struct clk_ops microchip_clk_pfsoc_ops_ro = {
	.recalc_rate = microchip_clk_pfsoc_recalc_rate,
};

/* definition for sub block clocks with capability of calculating its reference clock rate
 */

static const struct clk_ops microchip_clk_pfsoc_ops_en_recalc = {
	.enable = microchip_clk_pfsoc_enable,
	.disable = microchip_clk_pfsoc_disable,
	.is_enabled = microchip_clk_pfsoc_is_enabled,
	.recalc_rate = microchip_clk_pfsoc_recalc_rate,
};

/* definition for sub block clocks without capability of calculating its reference clock rate
 */

static const struct clk_ops microchip_clk_pfsoc_ops_en = {
	.enable = microchip_clk_pfsoc_enable,
	.disable = microchip_clk_pfsoc_disable,
	.is_enabled = microchip_clk_pfsoc_is_enabled,
};

/* empty definition
 */
static const struct clk_ops microchip_clk_pfsoc_ops_none = {
};


static int microchip_clk_pfsoc_probe(struct platform_device *pdev)
{

	struct device *dev = &pdev->dev;
	struct clk_init_data init;
	struct microchip_clk_pfsoc_driver *driver;
	struct resource *res;
	const char *parent;
	int i;

	parent = of_clk_get_parent_name(dev->of_node, 0);
	if (!parent) {
		dev_err(dev, "No OF parent clocks found\n");
		return -EINVAL;
	}

	driver = devm_kzalloc(dev, sizeof(*driver), GFP_KERNEL);
	if (!driver) {
		dev_err(dev, "Out of memory\n");
		return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	driver->reg = devm_ioremap_resource(dev, res);
	if (IS_ERR(driver->reg))
		return PTR_ERR(driver->reg);

	/* Link the data structure */
	driver->table.clk_num = TOTAL_CLOCKS;
	driver->table.clks = &driver->clks[0];
	dev_set_drvdata(dev, driver);

	/* Describe the main clocks */
	snprintf(driver->cfg[CPU_CLOCK].name, NAME_LEN, "%s.cpuclk", dev->of_node->name);
	driver->cfg[CPU_CLOCK].divider_offset = CPU_CLOCK*2u;
	driver->cfg[CPU_CLOCK].max = (CPU_CLOCK_SET) ? CPU_CLOCK_MAX : 0;

	snprintf(driver->cfg[AXI_CLOCK].name, NAME_LEN, "%s.axiclk", dev->of_node->name);
	driver->cfg[AXI_CLOCK].divider_offset = AXI_CLOCK*2u;
	driver->cfg[AXI_CLOCK].max = (AXI_CLOCK_SET) ? AXI_CLOCK_MAX : 0;

	snprintf(driver->cfg[AHB_CLOCK].name, NAME_LEN, "%s.ahbclk", dev->of_node->name);
	driver->cfg[AHB_CLOCK].divider_offset = AHB_CLOCK*2u;
	driver->cfg[AHB_CLOCK].max = (AHB_CLOCK_SET) ? AHB_CLOCK_MAX : 0;

	/* Describe the sub block clocks */
	for (i = CFG_CLOCKS; i < TOTAL_CLOCKS; i++) {
		snprintf(driver->cfg[i].name, NAME_LEN, "%s.%sclk", dev->of_node->name, peripheral_subblock_name_array[i - CFG_CLOCKS]);
		driver->cfg[i].subblk_offset = i - CFG_CLOCKS;
		driver->cfg[i].divider_offset = peripheral_subblock_refclk_array[i - CFG_CLOCKS]*2u;
		driver->cfg[i].max = 0;
	}

	/* Export the clocks */
	for (i = 0; i < TOTAL_CLOCKS; i++) {
		init.name = &driver->cfg[i].name[0];
		if (i < CFG_CLOCKS) {
			/* cpu, axi, or ahb clocks*/
			init.ops = driver->cfg[i].max ? &microchip_clk_pfsoc_ops_rw : &microchip_clk_pfsoc_ops_ro;
		} else {
			/* gated peripheral sub block clocks*/
			if (driver->cfg[i].divider_offset == (NO_CLOCK*2u)) {
				/*if there is no reference clock (one of the main clocks)
				for the sub block then reduce capability of driver
				*/
				init.ops = &microchip_clk_pfsoc_ops_en;
			} else if (driver->cfg[i].divider_offset == (NO_SUPPORT*2u)) {
				init.ops = &microchip_clk_pfsoc_ops_none;
			} else {
				init.ops = &microchip_clk_pfsoc_ops_en_recalc;
			}
		}

		init.num_parents = 1;
		init.parent_names = &parent;
		init.flags = 0;

		driver->cfg[i].driver = driver;
		driver->cfg[i].hw.init = &init;

		driver->clks[i] = devm_clk_register(dev, &driver->cfg[i].hw);
		if (IS_ERR(driver->clks[i])) {
			dev_err(dev, "Failed to register clock %d, %ld\n", i, PTR_ERR(driver->clks[i]));
			return PTR_ERR(driver->clks[i]);
		}
	}

	of_clk_add_provider(dev->of_node, of_clk_src_onecell_get, &driver->table);

	dev_info(dev, "Registered PFSOC core clocks\n");

	return 0;
}

static const struct of_device_id microchip_clk_pfsoc_of_match[] = {
	{ .compatible = "microchip,pfsoc-clkcfg", },
	{}
};

static struct platform_driver microchip_clk_pfsoc_driver = {
	.driver	= {
		.name = "microchip-pfsoc-clkcfg",
		.of_match_table = microchip_clk_pfsoc_of_match,
	},
	.probe = microchip_clk_pfsoc_probe,
};

static int __init microchip_clk_pfsoc_init(void)
{
	return platform_driver_register(&microchip_clk_pfsoc_driver);
}
