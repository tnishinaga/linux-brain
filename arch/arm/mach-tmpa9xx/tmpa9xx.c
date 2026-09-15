// SPDX-License-Identifier: GPL-2.0-only
/*
 * TMPA910 board-level pin mux setup.
 *
 * The TMPA910CRA hardware manual assigns PT1..PT3 to SSP0 when the
 * corresponding GPIOTFR1 bits are set.  Sharp Brain Gen1 connects these
 * pins to the AK4182A touch controller; PT0 is its GPIO chip-select.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/bitops.h>

#define TMPA910_GPIOTFR1		0xf080f424
#define TMPA910_GPIOTFR1_SSP0	GENMASK(3, 1)
#define TMPA910_CLKCR0		0xf0050040
#define TMPA910_CLKCR0_SSP0	BIT(2)

static int __init tmpa9xx_pinmux_init(void)
{
	void __iomem *reg;
	void __iomem *clkcr0;
	u32 value;

	reg = ioremap(TMPA910_GPIOTFR1, sizeof(u32));
	if (!reg)
		return -ENOMEM;
	clkcr0 = ioremap(TMPA910_CLKCR0, sizeof(u32));
	if (!clkcr0) {
		iounmap(reg);
		return -ENOMEM;
	}

	/*
	 * PT0 is the AK4182A chip-select GPIO.  The TMPA910 SSP0 FSS output
	 * cannot be used here because it returns high between FIFO words.  PT1,
	 * PT2 and PT3 remain SSP0 CLK, DO and DI alternate functions.
	 * Preserve PT4..PT7, which may be used by another alternate function.
	 */
	value = readl(reg);
	value = (value & ~GENMASK(3, 0)) | TMPA910_GPIOTFR1_SSP0;
	writel(value, reg);
	/* CLKCR0 bit 2 is the undocumented SSP0 peripheral clock gate. */
	value = readl(clkcr0);
	writel(value | TMPA910_CLKCR0_SSP0, clkcr0);
	iounmap(reg);
	iounmap(clkcr0);
	return 0;
}
arch_initcall(tmpa9xx_pinmux_init);
