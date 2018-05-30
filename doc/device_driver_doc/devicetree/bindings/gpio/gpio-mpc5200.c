/*
 * MPC52xx gpio driver
 *
 * Copyright (c) 2008 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/module.h>

#include <asm/gpio.h>
#include <asm/mpc52xx.h>
#include <sysdev/fsl_soc.h>

static DEFINE_SPINLOCK(gpio_lock);

struct mpc52xx_gpiochip {
	struct of_mm_gpio_chip mmchip;
	unsigned int shadow_dvo;
	unsigned int shadow_gpioe;
	unsigned int shadow_ddr;
};

/*
 * GPIO LIB API implementation for wakeup GPIOs.
 *
 * There's a maximum of 8 wakeup GPIOs. Which of these are available
 * for use depends on your board setup.
  * 웨이크 업 GPIO 용 GPIO LIB API 구현.
  *
  * 최대 8 개의 웨이크 업 GPIO가 있습니다. 이들 중 어느 것을 사용할 수 있는지는 보드 설정에 따라 다릅니다.
 *
 * 0 -> GPIO_WKUP_7
 * 1 -> GPIO_WKUP_6
 * 2 -> PSC6_1
 * 3 -> PSC6_0
 * 4 -> ETH_17
 * 5 -> PSC3_9
 * 6 -> PSC2_4
 * 7 -> PSC1_4
 *
 */
static int mpc52xx_wkup_gpio_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct mpc52xx_gpio_wkup __iomem *regs = mm_gc->regs;
	unsigned int ret;

	ret = (in_8(&regs->wkup_ival) >> (7 - gpio)) & 1;

	pr_debug("%s: gpio: %d ret: %d\n", __func__, gpio, ret);

	return ret;
}

static inline void
__mpc52xx_wkup_gpio_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct mpc52xx_gpiochip *chip = container_of(mm_gc,
			struct mpc52xx_gpiochip, mmchip);
	struct mpc52xx_gpio_wkup __iomem *regs = mm_gc->regs;

	if (val)
		chip->shadow_dvo |= 1 << (7 - gpio);
	else
		chip->shadow_dvo &= ~(1 << (7 - gpio));

	out_8(&regs->wkup_dvo, chip->shadow_dvo);
}

static void
mpc52xx_wkup_gpio_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	unsigned long flags;

	spin_lock_irqsave(&gpio_lock, flags);

	__mpc52xx_wkup_gpio_set(gc, gpio, val);

	spin_unlock_irqrestore(&gpio_lock, flags);

	pr_debug("%s: gpio: %d val: %d\n", __func__, gpio, val);
}

static int mpc52xx_wkup_gpio_dir_in(struct gpio_chip *gc, unsigned int gpio)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct mpc52xx_gpiochip *chip = container_of(mm_gc,
			struct mpc52xx_gpiochip, mmchip);
	struct mpc52xx_gpio_wkup __iomem *regs = mm_gc->regs;
	unsigned long flags;

	spin_lock_irqsave(&gpio_lock, flags);

	/* set the direction */
	chip->shadow_ddr &= ~(1 << (7 - gpio));
	out_8(&regs->wkup_ddr, chip->shadow_ddr);

	/* and enable the pin */
	chip->shadow_gpioe |= 1 << (7 - gpio);
	out_8(&regs->wkup_gpioe, chip->shadow_gpioe);

	spin_unlock_irqrestore(&gpio_lock, flags);

	return 0;
}

static int
mpc52xx_wkup_gpio_dir_out(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct mpc52xx_gpio_wkup __iomem *regs = mm_gc->regs;
	struct mpc52xx_gpiochip *chip = container_of(mm_gc,
			struct mpc52xx_gpiochip, mmchip);
	unsigned long flags;

	spin_lock_irqsave(&gpio_lock, flags);

	__mpc52xx_wkup_gpio_set(gc, gpio, val);

	/* Then set direction */
	chip->shadow_ddr |= 1 << (7 - gpio);
	out_8(&regs->wkup_ddr, chip->shadow_ddr);

	/* Finally enable the pin */
	chip->shadow_gpioe |= 1 << (7 - gpio);
	out_8(&regs->wkup_gpioe, chip->shadow_gpioe);

	spin_unlock_irqrestore(&gpio_lock, flags);

	pr_debug("%s: gpio: %d val: %d\n", __func__, gpio, val);

	return 0;
}

static int mpc52xx_wkup_gpiochip_probe(struct platform_device *ofdev)
{
	struct mpc52xx_gpiochip *chip;
	struct mpc52xx_gpio_wkup __iomem *regs;
	struct gpio_chip *gc;
	int ret;

	chip = devm_kzalloc(&ofdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	platform_set_drvdata(ofdev, chip);

	gc = &chip->mmchip.gc;

	gc->ngpio            = 8;
	gc->direction_input  = mpc52xx_wkup_gpio_dir_in;
	gc->direction_output = mpc52xx_wkup_gpio_dir_out;
	gc->get              = mpc52xx_wkup_gpio_get;
	gc->set              = mpc52xx_wkup_gpio_set;

	ret = of_mm_gpiochip_add(ofdev->dev.of_node, &chip->mmchip);
	if (ret)
		return ret;

	regs = chip->mmchip.regs;
	chip->shadow_gpioe = in_8(&regs->wkup_gpioe);
	chip->shadow_ddr = in_8(&regs->wkup_ddr);
	chip->shadow_dvo = in_8(&regs->wkup_dvo);

	return 0;
}

static int mpc52xx_gpiochip_remove(struct platform_device *ofdev)
{
	struct mpc52xx_gpiochip *chip = platform_get_drvdata(ofdev);

	of_mm_gpiochip_remove(&chip->mmchip);

	return 0;
}

static const struct of_device_id mpc52xx_wkup_gpiochip_match[] = {
	{ .compatible = "fsl,mpc5200-gpio-wkup", },
	{}
};

static struct platform_driver mpc52xx_wkup_gpiochip_driver = {
	.driver = {
		.name = "mpc5200-gpio-wkup",
		.of_match_table = mpc52xx_wkup_gpiochip_match,
	},
	.probe = mpc52xx_wkup_gpiochip_probe,
	.remove = mpc52xx_gpiochip_remove,
};

/*
 * GPIO LIB API implementation for simple GPIOs
 *
 * There's a maximum of 32 simple GPIOs. Which of these are available
 * for use depends on your board setup.
 * The numbering reflects the bit numbering in the port registers:
  * 간단한 GPIO를위한 GPIO LIB API 구현
  *
  * 최대 32 개의 간단한 GPIO가 있습니다. 이들 중 어느 것을 사용할 수 있는지는 보드 설정에 따라 다릅니다.
   번호 매기기는 포트 레지스터의 비트 번호를 반영합니다.
 *
 *  0..1  > reserved
 *  2..3  > IRDA
 *  4..7  > ETHR
 *  8..11 > reserved
 * 12..15 > USB
 * 16..17 > reserved
 * 18..23 > PSC3
 * 24..27 > PSC2
 * 28..31 > PSC1
 */
static int mpc52xx_simple_gpio_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct mpc52xx_gpio __iomem *regs = mm_gc->regs;
	unsigned int ret;

	ret = (in_be32(&regs->simple_ival) >> (31 - gpio)) & 1;

	return ret;
}

static inline void
__mpc52xx_simple_gpio_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct mpc52xx_gpiochip *chip = container_of(mm_gc,
			struct mpc52xx_gpiochip, mmchip);
	struct mpc52xx_gpio __iomem *regs = mm_gc->regs;

	if (val)
		chip->shadow_dvo |= 1 << (31 - gpio);
	else
		chip->shadow_dvo &= ~(1 << (31 - gpio));
	out_be32(&regs->simple_dvo, chip->shadow_dvo);
}

static void
mpc52xx_simple_gpio_set(struct gpio_chip *gc, unsigned int gpio, int val)
{
	unsigned long flags;

	spin_lock_irqsave(&gpio_lock, flags);

	__mpc52xx_simple_gpio_set(gc, gpio, val);

	spin_unlock_irqrestore(&gpio_lock, flags);

	pr_debug("%s: gpio: %d val: %d\n", __func__, gpio, val);
}

static int mpc52xx_simple_gpio_dir_in(struct gpio_chip *gc, unsigned int gpio)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct mpc52xx_gpiochip *chip = container_of(mm_gc,
			struct mpc52xx_gpiochip, mmchip);
	struct mpc52xx_gpio __iomem *regs = mm_gc->regs;
	unsigned long flags;

	spin_lock_irqsave(&gpio_lock, flags);

	/* set the direction */
	chip->shadow_ddr &= ~(1 << (31 - gpio));
	out_be32(&regs->simple_ddr, chip->shadow_ddr);

	/* and enable the pin */
	chip->shadow_gpioe |= 1 << (31 - gpio);
	out_be32(&regs->simple_gpioe, chip->shadow_gpioe);

	spin_unlock_irqrestore(&gpio_lock, flags);

	return 0;
}

static int
mpc52xx_simple_gpio_dir_out(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct of_mm_gpio_chip *mm_gc = to_of_mm_gpio_chip(gc);
	struct mpc52xx_gpiochip *chip = container_of(mm_gc,
			struct mpc52xx_gpiochip, mmchip);
	struct mpc52xx_gpio __iomem *regs = mm_gc->regs;
	unsigned long flags;

	spin_lock_irqsave(&gpio_lock, flags);

	/* First set initial value */
	__mpc52xx_simple_gpio_set(gc, gpio, val);

	/* Then set direction */
	chip->shadow_ddr |= 1 << (31 - gpio);
	out_be32(&regs->simple_ddr, chip->shadow_ddr);

	/* Finally enable the pin */
	chip->shadow_gpioe |= 1 << (31 - gpio);
	out_be32(&regs->simple_gpioe, chip->shadow_gpioe);

	spin_unlock_irqrestore(&gpio_lock, flags);

	pr_debug("%s: gpio: %d val: %d\n", __func__, gpio, val);

	return 0;
}

static int mpc52xx_simple_gpiochip_probe(struct platform_device *ofdev)
{
	struct mpc52xx_gpiochip *chip;
	struct gpio_chip *gc;
	struct mpc52xx_gpio __iomem *regs;
	int ret;

	chip = devm_kzalloc(&ofdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	platform_set_drvdata(ofdev, chip);

	gc = &chip->mmchip.gc;

	gc->ngpio            = 32;
	gc->direction_input  = mpc52xx_simple_gpio_dir_in;
	gc->direction_output = mpc52xx_simple_gpio_dir_out;
	gc->get              = mpc52xx_simple_gpio_get;
	gc->set              = mpc52xx_simple_gpio_set;

	ret = of_mm_gpiochip_add(ofdev->dev.of_node, &chip->mmchip);
	if (ret)
		return ret;

	regs = chip->mmchip.regs;
	chip->shadow_gpioe = in_be32(&regs->simple_gpioe);
	chip->shadow_ddr = in_be32(&regs->simple_ddr);
	chip->shadow_dvo = in_be32(&regs->simple_dvo);

	return 0;
}

static const struct of_device_id mpc52xx_simple_gpiochip_match[] = {
	{ .compatible = "fsl,mpc5200-gpio", },
	{}
};

static struct platform_driver mpc52xx_simple_gpiochip_driver = {
	.driver = {
		.name = "mpc5200-gpio",
		.of_match_table = mpc52xx_simple_gpiochip_match,
	},
	.probe = mpc52xx_simple_gpiochip_probe,
	.remove = mpc52xx_gpiochip_remove,
};

static int __init mpc52xx_gpio_init(void)
{
	if (platform_driver_register(&mpc52xx_wkup_gpiochip_driver))
		printk(KERN_ERR "Unable to register wakeup GPIO driver\n");

	if (platform_driver_register(&mpc52xx_simple_gpiochip_driver))
		printk(KERN_ERR "Unable to register simple GPIO driver\n");

	return 0;
}

/* Make sure we get initialised before anyone else tries to use us
   다른 사용자가 우리를 사용하기 전에 초기화해야합니다.			 */
subsys_initcall(mpc52xx_gpio_init);

static void __exit mpc52xx_gpio_exit(void)
{
	platform_driver_unregister(&mpc52xx_wkup_gpiochip_driver);

	platform_driver_unregister(&mpc52xx_simple_gpiochip_driver);
}
module_exit(mpc52xx_gpio_exit);

MODULE_DESCRIPTION("Freescale MPC52xx gpio driver");
MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de");
MODULE_LICENSE("GPL v2");

