// SPDX-License-Identifier: GPL-2.0+
/*
 * Qualcomm UART driver
 *
 * (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>
 *
 * UART will work in Data Mover mode.
 * Based on Linux driver.
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <errno.h>
#include <malloc.h>
#include <serial.h>
#include <watchdog.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <dm/pinctrl.h>

/* Serial registers - this driver works in uartdm mode*/

#define UARTDM_DMRX             0x34 /* Max RX transfer length */
#define UARTDM_DMEN             0x3C /* DMA/data-packing mode */
#define UARTDM_NCF_TX           0x40 /* Number of chars to TX */

#define UARTDM_RXFS             0x50 /* RX channel status register */
#define UARTDM_RXFS_BUF_SHIFT   0x7  /* Number of bytes in the packing buffer */
#define UARTDM_RXFS_BUF_MASK    0x7
#define UARTDM_MR1				 0x00
#define UARTDM_MR2				 0x04
#define UARTDM_CSR				 0xA0

#define UARTDM_SR                0xA4 /* Status register */
#define UARTDM_SR_RX_READY       (1 << 0) /* Word is the receiver FIFO */
#define UARTDM_SR_TX_EMPTY       (1 << 3) /* Transmitter underrun */
#define UARTDM_SR_UART_OVERRUN   (1 << 4) /* Receive overrun */

#define UARTDM_CR                         0xA8 /* Command register */
#define UARTDM_CR_CMD_RESET_ERR           (3 << 4) /* Clear overrun error */
#define UARTDM_CR_CMD_RESET_STALE_INT     (8 << 4) /* Clears stale irq */
#define UARTDM_CR_CMD_RESET_TX_READY      (3 << 8) /* Clears TX Ready irq*/
#define UARTDM_CR_CMD_FORCE_STALE         (4 << 8) /* Causes stale event */
#define UARTDM_CR_CMD_STALE_EVENT_DISABLE (6 << 8) /* Disable stale event */

#define UARTDM_IMR                0xB0 /* Interrupt mask register */
#define UARTDM_ISR                0xB4 /* Interrupt status register */
#define UARTDM_ISR_TX_READY       0x80 /* TX FIFO empty */

#define UARTDM_TF               0x100 /* UART Transmit FIFO register */
#define UARTDM_RF               0x140 /* UART Receive FIFO register */

#define UART_DM_CLK_RX_TX_BIT_RATE 0xCC
#define MSM_BOOT_UART_DM_8_N_1_MODE 0x34
#define MSM_BOOT_UART_DM_CMD_RESET_RX 0x10
#define MSM_BOOT_UART_DM_CMD_RESET_TX 0x20

DECLARE_GLOBAL_DATA_PTR;

struct msm_serial_data {
	phys_addr_t base;
	unsigned chars_cnt; /* number of buffered chars */
	uint32_t chars_buf; /* buffered chars */
	uint32_t clk_bit_rate; /* data mover mode bit rate register value */
};

static int msm_serial_fetch(struct udevice *dev)
{
	struct msm_serial_data *priv = dev_get_priv(dev);
	unsigned sr;

	if (priv->chars_cnt)
		return priv->chars_cnt;

	/* Clear error in case of buffer overrun */
	if (readl(priv->base + UARTDM_SR) & UARTDM_SR_UART_OVERRUN)
		writel(UARTDM_CR_CMD_RESET_ERR, priv->base + UARTDM_CR);

	/* We need to fetch new character */
	sr = readl(priv->base + UARTDM_SR);

	if (sr & UARTDM_SR_RX_READY) {
		/* There are at least 4 bytes in fifo */
		priv->chars_buf = readl(priv->base + UARTDM_RF);
		priv->chars_cnt = 4;
	} else {
		/* Check if there is anything in fifo */
		priv->chars_cnt = readl(priv->base + UARTDM_RXFS);
		/* Extract number of characters in UART packing buffer*/
		priv->chars_cnt = (priv->chars_cnt >>
				   UARTDM_RXFS_BUF_SHIFT) &
				  UARTDM_RXFS_BUF_MASK;
		if (!priv->chars_cnt)
			return 0;

		/* There is at least one charcter, move it to fifo */
		writel(UARTDM_CR_CMD_FORCE_STALE,
		       priv->base + UARTDM_CR);

		priv->chars_buf = readl(priv->base + UARTDM_RF);
		writel(UARTDM_CR_CMD_RESET_STALE_INT,
		       priv->base + UARTDM_CR);
		writel(0x7, priv->base + UARTDM_DMRX);
	}

	return priv->chars_cnt;
}

static int msm_serial_getc(struct udevice *dev)
{
	struct msm_serial_data *priv = dev_get_priv(dev);
	char c;

	if (!msm_serial_fetch(dev))
		return -EAGAIN;

	c = priv->chars_buf & 0xFF;
	priv->chars_buf >>= 8;
	priv->chars_cnt--;

	return c;
}

static int msm_serial_putc(struct udevice *dev, const char ch)
{
	struct msm_serial_data *priv = dev_get_priv(dev);

	if (!(readl(priv->base + UARTDM_SR) & UARTDM_SR_TX_EMPTY) &&
	    !(readl(priv->base + UARTDM_ISR) & UARTDM_ISR_TX_READY))
		return -EAGAIN;

	writel(UARTDM_CR_CMD_RESET_TX_READY, priv->base + UARTDM_CR);

	writel(1, priv->base + UARTDM_NCF_TX);
	writel(ch, priv->base + UARTDM_TF);

	return 0;
}

static int msm_serial_pending(struct udevice *dev, bool input)
{
	if (input) {
		if (msm_serial_fetch(dev))
			return 1;
	}

	return 0;
}

static const struct dm_serial_ops msm_serial_ops = {
	.putc = msm_serial_putc,
	.pending = msm_serial_pending,
	.getc = msm_serial_getc,
};

static int msm_uart_clk_init(struct udevice *dev)
{
	uint clk_rate = fdtdec_get_uint(gd->fdt_blob, dev_of_offset(dev),
					"clock-frequency", 115200);
	uint clkd[2]; /* clk_id and clk_no */
	int clk_offset;
	struct udevice *clk_dev;
	struct clk clk;
	int ret;

	ret = fdtdec_get_int_array(gd->fdt_blob, dev_of_offset(dev), "clock",
				   clkd, 2);
	if (ret)
		return ret;

	clk_offset = fdt_node_offset_by_phandle(gd->fdt_blob, clkd[0]);
	if (clk_offset < 0)
		return clk_offset;

	ret = uclass_get_device_by_of_offset(UCLASS_CLK, clk_offset, &clk_dev);
	if (ret)
		return ret;

	clk.id = clkd[1];
	ret = clk_request(clk_dev, &clk);
	if (ret < 0)
		return ret;

	ret = clk_set_rate(&clk, clk_rate);
	clk_free(&clk);
	if (ret < 0)
		return ret;

	return 0;
}

static void uart_dm_init(struct msm_serial_data *priv)
{
	/* Delay initialization for a bit to let pins stabilize if necessary */
	mdelay(5);

	writel(priv->clk_bit_rate, priv->base + UARTDM_CSR);
	writel(0x0, priv->base + UARTDM_MR1);
	writel(MSM_BOOT_UART_DM_8_N_1_MODE, priv->base + UARTDM_MR2);
	writel(MSM_BOOT_UART_DM_CMD_RESET_RX, priv->base + UARTDM_CR);
	writel(MSM_BOOT_UART_DM_CMD_RESET_TX, priv->base + UARTDM_CR);

	/* Make sure BAM/single character mode is disabled */
	writel(0x0, priv->base + UARTDM_DMEN);
}
static int msm_serial_probe(struct udevice *dev)
{
	int ret;
	struct msm_serial_data *priv = dev_get_priv(dev);

	/* No need to reinitialize the UART after relocation */
	if (gd->flags & GD_FLG_RELOC)
		return 0;

	ret = msm_uart_clk_init(dev);
	if (ret)
		return ret;

	pinctrl_select_state(dev, "uart");
	uart_dm_init(priv);

	return 0;
}

static int msm_serial_of_to_plat(struct udevice *dev)
{
	struct msm_serial_data *priv = dev_get_priv(dev);

	priv->base = dev_read_addr(dev);
	if (priv->base == FDT_ADDR_T_NONE)
		return -EINVAL;

	priv->clk_bit_rate = fdtdec_get_int(gd->fdt_blob, dev_of_offset(dev),
							"bit-rate", UART_DM_CLK_RX_TX_BIT_RATE);

	return 0;
}

static const struct udevice_id msm_serial_ids[] = {
	{ .compatible = "qcom,msm-uartdm-v1.4" },
	{ }
};

U_BOOT_DRIVER(serial_msm) = {
	.name	= "serial_msm",
	.id	= UCLASS_SERIAL,
	.of_match = msm_serial_ids,
	.of_to_plat = msm_serial_of_to_plat,
	.priv_auto	= sizeof(struct msm_serial_data),
	.probe = msm_serial_probe,
	.ops	= &msm_serial_ops,
};

#ifdef CONFIG_DEBUG_UART_MSM_UARTDM

#include <debug_uart.h>

static inline void _debug_uart_init(void)
{
	/* we rely on whatever was configured by first-stage
	   bootloader, so we don't have any special init here */
}

static inline void msm_write(unsigned int val, unsigned int off)
{
	writel_relaxed(val, (uintptr_t)CONFIG_DEBUG_UART_BASE + off);
}

static inline unsigned int msm_read(unsigned int off)
{
	return readl_relaxed((uintptr_t)CONFIG_DEBUG_UART_BASE + off);
}

/* UART_ values are different from UARTDM_ ones? */
#define UART_SR				0x08	/* status register */
#define UART_SR_HUNT_CHAR		BIT(7)
#define UART_SR_RX_BREAK		BIT(6)
#define UART_SR_PAR_FRAME_ERR		BIT(5)
#define UART_SR_OVERRUN			BIT(4)
#define UART_SR_TX_EMPTY		BIT(3)
#define UART_SR_TX_READY		BIT(2)
#define UART_SR_RX_FULL			BIT(1)
#define UART_SR_RX_READY		BIT(0)

#define UART_ISR			0x14
#define UART_ISR_TX_READY		BIT(7)

#define UART_CR				0x10	/* command register */
#define UART_CR_CMD_NULL		(0 << 4)
#define UART_CR_CMD_RESET_RX		(1 << 4)
#define UART_CR_CMD_RESET_TX		(2 << 4)
#define UART_CR_CMD_RESET_ERR		(3 << 4)
#define UART_CR_CMD_RESET_BREAK_INT	(4 << 4)
#define UART_CR_CMD_START_BREAK		(5 << 4)
#define UART_CR_CMD_STOP_BREAK		(6 << 4)
#define UART_CR_CMD_RESET_CTS		(7 << 4)
#define UART_CR_CMD_RESET_STALE_INT	(8 << 4)
#define UART_CR_CMD_PACKET_MODE		(9 << 4)
#define UART_CR_CMD_MODE_RESET		(12 << 4)
#define UART_CR_CMD_SET_RFR		(13 << 4)
#define UART_CR_CMD_RESET_RFR		(14 << 4)
#define UART_CR_CMD_PROTECTION_EN	(16 << 4)
#define UART_CR_CMD_STALE_EVENT_DISABLE	(6 << 8)
#define UART_CR_CMD_STALE_EVENT_ENABLE	(80 << 4)
#define UART_CR_CMD_FORCE_STALE		(4 << 8)
#define UART_CR_CMD_RESET_TX_READY	(3 << 8)
#define UART_CR_TX_DISABLE		BIT(3)
#define UART_CR_TX_ENABLE		BIT(2)
#define UART_CR_RX_DISABLE		BIT(1)
#define UART_CR_RX_ENABLE		BIT(0)
#define UART_CR_CMD_RESET_RXBREAK_START	((1 << 11) | (2 << 4))

/* another UARTDM_TF value, which differs from 0x100 in this driver */
#define UART_TF				0x70

static inline void msm_wait_for_xmitr(void)
{
	unsigned int timeout = 500000;

	while (!(msm_read(UART_SR) & UART_SR_TX_EMPTY)) {
		if (msm_read(UART_ISR) & UART_ISR_TX_READY)
			break;
		udelay(1);
		if (!timeout--)
			break;
	}
	msm_write(UART_CR_CMD_RESET_TX_READY, UART_CR);
}

static void msm_reset_dm_count(int count)
{
	msm_wait_for_xmitr();
	msm_write(count, UARTDM_NCF_TX);
	msm_read(UARTDM_NCF_TX);
}

static void _debug_uart_putc(char ch)
{
	msm_reset_dm_count(1);

	while (!(msm_read(UART_SR) & UART_SR_TX_READY))
		cpu_relax();

	msm_write(ch,UART_TF);
}

DEBUG_UART_FUNCS

#endif
