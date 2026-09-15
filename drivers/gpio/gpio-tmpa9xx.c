// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIOC and Port T support for Toshiba TMPA9xx.
 *
 * Register layout and interrupt mux programming are taken from the
 * TMPA910CRA hardware manual.  PC7 is the active-low AK4182A PENIRQ input
 * on Sharp Brain Gen1.  Port T has no interrupt block on this SoC and is
 * used here for the software-controlled SSP0 chip-select GPIO.
 */

#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/gpio/driver.h>

#define TMPA9XX_GPIO_DATA	0x03fc
#define TMPA9XX_GPIO_DIR	0x0400
#define TMPA9XX_GPIO_FR1	0x0424
#define TMPA9XX_GPIO_FR2	0x0428
#define TMPA9XX_GPIO_IS		0x0804
#define TMPA9XX_GPIO_IBE	0x0808
#define TMPA9XX_GPIO_IEV	0x080c
#define TMPA9XX_GPIO_IE		0x0810
#define TMPA9XX_GPIO_MIS		0x0818
#define TMPA9XX_GPIO_IC		0x081c

#define TMPA9XX_TOUCH_IRQ	BIT(7)

struct tmpa9xx_gpio {
	struct gpio_chip chip;
	struct irq_chip irq_chip;
	void __iomem *base;
	int parent_irq;
	bool has_irq;
	raw_spinlock_t lock;
};

static struct tmpa9xx_gpio *to_tmpa9xx_gpio(struct gpio_chip *chip)
{
	return gpiochip_get_data(chip);
}

static int tmpa9xx_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);

	return !!(readl(gpio->base + TMPA9XX_GPIO_DATA) & BIT(offset));
}

static void tmpa9xx_gpio_set(struct gpio_chip *chip, unsigned int offset,
				     int value)
{
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);
	u32 data = readl(gpio->base + TMPA9XX_GPIO_DATA) & 0xff;

	if (value)
		data |= BIT(offset);
	else
		data &= ~BIT(offset);
	writel(data, gpio->base + TMPA9XX_GPIO_DATA);
}

static int tmpa9xx_gpio_direction_input(struct gpio_chip *chip,
					unsigned int offset)
{
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);
	u32 dir = readl(gpio->base + TMPA9XX_GPIO_DIR) & 0xff;

	dir &= ~BIT(offset);
	writel(dir, gpio->base + TMPA9XX_GPIO_DIR);
	return 0;
}

static int tmpa9xx_gpio_direction_output(struct gpio_chip *chip,
					 unsigned int offset, int value)
{
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);
	u32 dir;

	tmpa9xx_gpio_set(chip, offset, value);
	dir = readl(gpio->base + TMPA9XX_GPIO_DIR) & 0xff;
	dir |= BIT(offset);
	writel(dir, gpio->base + TMPA9XX_GPIO_DIR);
	return 0;
}

static int tmpa9xx_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(d);
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);
	unsigned int offset = irqd_to_hwirq(d);
	unsigned long flags;
	u32 bit, fr1, dir, is, ibe, iev, ie;

	if (!gpio->has_irq || (offset != 5 && offset != 7))
		return -EINVAL;
	if (type != IRQ_TYPE_EDGE_RISING && type != IRQ_TYPE_EDGE_FALLING)
		return -EINVAL;

	bit = BIT(offset);
	raw_spin_lock_irqsave(&gpio->lock, flags);

	/* TMPA910 requires: direction, disable, mode, clear, enable. */
	fr1 = readl(gpio->base + TMPA9XX_GPIO_FR1) & 0xff;
	writel(fr1 & ~bit, gpio->base + TMPA9XX_GPIO_FR1);
	dir = readl(gpio->base + TMPA9XX_GPIO_DIR) & 0xff;
	writel(dir & ~bit, gpio->base + TMPA9XX_GPIO_DIR);
	ie = readl(gpio->base + TMPA9XX_GPIO_IE) & 0xff;
	writel(ie & ~bit, gpio->base + TMPA9XX_GPIO_IE);

	is = readl(gpio->base + TMPA9XX_GPIO_IS) & 0xff;
	ibe = readl(gpio->base + TMPA9XX_GPIO_IBE) & 0xff;
	iev = readl(gpio->base + TMPA9XX_GPIO_IEV) & 0xff;
	is &= ~bit; /* edge-sensitive */
	ibe &= ~bit; /* single edge */
	if (type == IRQ_TYPE_EDGE_RISING)
		iev |= bit;
	else
		iev &= ~bit;
	writel(is, gpio->base + TMPA9XX_GPIO_IS);
	writel(ibe, gpio->base + TMPA9XX_GPIO_IBE);
	writel(iev, gpio->base + TMPA9XX_GPIO_IEV);
	writel(bit, gpio->base + TMPA9XX_GPIO_IC);

	raw_spin_unlock_irqrestore(&gpio->lock, flags);
	irq_set_handler_locked(d, handle_edge_irq);
	return 0;
}

static void tmpa9xx_gpio_irq_ack(struct irq_data *d)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(d);
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);

	/* GPIOCIC is write-one-to-clear for the edge latch. */
	writel(BIT(irqd_to_hwirq(d)), gpio->base + TMPA9XX_GPIO_IC);
}

static void tmpa9xx_gpio_irq_mask(struct irq_data *d)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(d);
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);
	unsigned long flags;
	u32 value;

	raw_spin_lock_irqsave(&gpio->lock, flags);
	value = readl(gpio->base + TMPA9XX_GPIO_IE) & 0xff;
	writel(value & ~BIT(irqd_to_hwirq(d)), gpio->base + TMPA9XX_GPIO_IE);
	raw_spin_unlock_irqrestore(&gpio->lock, flags);
}

static void tmpa9xx_gpio_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(d);
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);
	unsigned long flags;
	u32 value;

	raw_spin_lock_irqsave(&gpio->lock, flags);
	value = readl(gpio->base + TMPA9XX_GPIO_IE) & 0xff;
	writel(value | BIT(irqd_to_hwirq(d)), gpio->base + TMPA9XX_GPIO_IE);
	raw_spin_unlock_irqrestore(&gpio->lock, flags);
}

static void tmpa9xx_gpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *chip = irq_desc_get_handler_data(desc);
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);
	struct irq_chip *parent = irq_desc_get_chip(desc);
	unsigned long pending;
	unsigned int offset;

	chained_irq_enter(parent, desc);
	pending = readl(gpio->base + TMPA9XX_GPIO_MIS) & 0xff;
	for_each_set_bit(offset, &pending, chip->ngpio)
		generic_handle_irq(irq_find_mapping(chip->irq.domain, offset));
	chained_irq_exit(parent, desc);
}

static int tmpa9xx_gpio_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(d);
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);

	return irq_set_irq_wake(gpio->parent_irq, on);
}

static int tmpa9xx_gpio_init_hw(struct gpio_chip *chip)
{
	struct tmpa9xx_gpio *gpio = to_tmpa9xx_gpio(chip);

	if (!gpio->has_irq)
		return 0;

	/* Do not leave stale GPIOC edge requests asserted across probe. */
	writel(0, gpio->base + TMPA9XX_GPIO_IE);
	writel(BIT(5) | BIT(7), gpio->base + TMPA9XX_GPIO_IC);
	return 0;
}

static int tmpa9xx_gpio_probe(struct platform_device *pdev)
{
	struct tmpa9xx_gpio *gpio;
	struct gpio_irq_chip *girq;
	int ret;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gpio->base))
		return PTR_ERR(gpio->base);

	raw_spin_lock_init(&gpio->lock);
	gpio->has_irq = !of_device_is_compatible(pdev->dev.of_node,
						"toshiba,tmpa910-gpiot");
	if (gpio->has_irq) {
		gpio->parent_irq = platform_get_irq(pdev, 0);
		if (gpio->parent_irq < 0)
			return gpio->parent_irq;
	}

	gpio->chip.label = dev_name(&pdev->dev);
	gpio->chip.parent = &pdev->dev;
	gpio->chip.owner = THIS_MODULE;
	gpio->chip.base = -1;
	gpio->chip.ngpio = 8;
	gpio->chip.get = tmpa9xx_gpio_get;
	gpio->chip.set = tmpa9xx_gpio_set;
	gpio->chip.direction_input = tmpa9xx_gpio_direction_input;
	gpio->chip.direction_output = tmpa9xx_gpio_direction_output;
	gpio->chip.can_sleep = false;

	if (gpio->has_irq) {
		gpio->irq_chip.name = dev_name(&pdev->dev);
		gpio->irq_chip.irq_ack = tmpa9xx_gpio_irq_ack;
		gpio->irq_chip.irq_mask = tmpa9xx_gpio_irq_mask;
		gpio->irq_chip.irq_unmask = tmpa9xx_gpio_irq_unmask;
		gpio->irq_chip.irq_set_type = tmpa9xx_gpio_irq_set_type;
		gpio->irq_chip.irq_set_wake = tmpa9xx_gpio_irq_set_wake;

		girq = &gpio->chip.irq;
		girq->chip = &gpio->irq_chip;
		girq->parent_handler = tmpa9xx_gpio_irq_handler;
		girq->num_parents = 1;
		girq->parents = devm_kcalloc(&pdev->dev, 1,
						    sizeof(*girq->parents), GFP_KERNEL);
		if (!girq->parents)
			return -ENOMEM;
		girq->parents[0] = gpio->parent_irq;
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_bad_irq;
		girq->init_hw = tmpa9xx_gpio_init_hw;
	}

	ret = devm_gpiochip_add_data(&pdev->dev, &gpio->chip, gpio);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id tmpa9xx_gpio_of_match[] = {
	{ .compatible = "toshiba,tmpa910-gpioc" },
	{ .compatible = "toshiba,tmpa910-gpiot" },
	{ }
};
MODULE_DEVICE_TABLE(of, tmpa9xx_gpio_of_match);

static struct platform_driver tmpa9xx_gpio_driver = {
	.probe = tmpa9xx_gpio_probe,
	.driver = {
		.name = "tmpa9xx-gpio",
		.of_match_table = tmpa9xx_gpio_of_match,
	},
};
module_platform_driver(tmpa9xx_gpio_driver);

MODULE_DESCRIPTION("Toshiba TMPA9xx GPIOC/Port T driver");
MODULE_LICENSE("GPL");
