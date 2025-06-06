// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BCM6368 Ethernet Switch Controller Driver
 *
 * Copyright (C) 2021 Álvaro Fernández Rojas <noltari@gmail.com>
 * Copyright (C) 2015 Jonas Gorski <jonas.gorski@gmail.com>
 * Copyright (C) 2008 Maxime Bizon <mbizon@freebox.fr>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/of_clk.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

/* TODO: Bigger frames may work but we do not trust that they are safe on all
 * platforms so more research is needed, a max frame size of 2048 has been
 * tested. We use the safe frame size 1542 which is 1532 plus DSA and VLAN
 * overhead.
 */
#define ENETSW_MAX_FRAME		1542
#define ENETSW_DSA_TAG_SIZE		6
/* The MTU in Linux does not include ethernet or VLAN headers, but it DOES
 * include the DSA overhead (the framework will increase the MTU to fit
 * any DSA header).
 */
#define ENETSW_MAX_MTU			(ENETSW_MAX_FRAME - VLAN_ETH_HLEN - \
					 VLAN_HLEN)
#define ENETSW_FRAG_SIZE(x)		(SKB_DATA_ALIGN(NET_SKB_PAD + x + \
					 SKB_DATA_ALIGN(sizeof(struct skb_shared_info))))

/* default number of descriptor */
#define ENETSW_DEF_RX_DESC		64
#define ENETSW_DEF_TX_DESC		32
#define ENETSW_DEF_CPY_BREAK		128

/* maximum burst len for dma (4 bytes unit) */
#define ENETSW_DMA_MAXBURST		8

/* DMA channels */
#define DMA_CHAN_WIDTH			0x10

/* Controller Configuration Register */
#define DMA_CFG_REG			0x0
#define DMA_CFG_EN_SHIFT		0
#define DMA_CFG_EN_MASK			(1 << DMA_CFG_EN_SHIFT)
#define DMA_CFG_FLOWCH_MASK(x)		(1 << ((x >> 1) + 1))

/* Flow Control Descriptor Low Threshold register */
#define DMA_FLOWCL_REG(x)		(0x4 + (x) * 6)

/* Flow Control Descriptor High Threshold register */
#define DMA_FLOWCH_REG(x)		(0x8 + (x) * 6)

/* Flow Control Descriptor Buffer Alloca Threshold register */
#define DMA_BUFALLOC_REG(x)		(0xc + (x) * 6)
#define DMA_BUFALLOC_FORCE_SHIFT	31
#define DMA_BUFALLOC_FORCE_MASK		(1 << DMA_BUFALLOC_FORCE_SHIFT)

/* Channel Configuration register */
#define DMAC_CHANCFG_REG		0x0
#define DMAC_CHANCFG_EN_SHIFT		0
#define DMAC_CHANCFG_EN_MASK		(1 << DMAC_CHANCFG_EN_SHIFT)
#define DMAC_CHANCFG_PKTHALT_SHIFT	1
#define DMAC_CHANCFG_PKTHALT_MASK	(1 << DMAC_CHANCFG_PKTHALT_SHIFT)
#define DMAC_CHANCFG_BUFHALT_SHIFT	2
#define DMAC_CHANCFG_BUFHALT_MASK	(1 << DMAC_CHANCFG_BUFHALT_SHIFT)
#define DMAC_CHANCFG_CHAINING_SHIFT	2
#define DMAC_CHANCFG_CHAINING_MASK	(1 << DMAC_CHANCFG_CHAINING_SHIFT)
#define DMAC_CHANCFG_WRAP_EN_SHIFT	3
#define DMAC_CHANCFG_WRAP_EN_MASK	(1 << DMAC_CHANCFG_WRAP_EN_SHIFT)
#define DMAC_CHANCFG_FLOWC_EN_SHIFT	4
#define DMAC_CHANCFG_FLOWC_EN_MASK	(1 << DMAC_CHANCFG_FLOWC_EN_SHIFT)

/* Interrupt Control/Status register */
#define DMAC_IR_REG			0x4
#define DMAC_IR_BUFDONE_MASK		(1 << 0)
#define DMAC_IR_PKTDONE_MASK		(1 << 1)
#define DMAC_IR_NOTOWNER_MASK		(1 << 2)

/* Interrupt Mask register */
#define DMAC_IRMASK_REG			0x8

/* Maximum Burst Length */
#define DMAC_MAXBURST_REG		0xc

/* Ring Start Address register */
#define DMAS_RSTART_REG			0x0

/* State Ram Word 2 */
#define DMAS_SRAM2_REG			0x4

/* State Ram Word 3 */
#define DMAS_SRAM3_REG			0x8

/* State Ram Word 4 */
#define DMAS_SRAM4_REG			0xc

struct bcm6368_enetsw_desc {
	u32 len_stat;
	u32 address;
};

/* control */
#define DMADESC_LENGTH_SHIFT		16
#define DMADESC_LENGTH_MASK		(0xfff << DMADESC_LENGTH_SHIFT)
#define DMADESC_OWNER_MASK		(1 << 15)
#define DMADESC_EOP_MASK		(1 << 14)
#define DMADESC_SOP_MASK		(1 << 13)
#define DMADESC_ESOP_MASK		(DMADESC_EOP_MASK | DMADESC_SOP_MASK)
#define DMADESC_WRAP_MASK		(1 << 12)
#define DMADESC_USB_NOZERO_MASK 	(1 << 1)
#define DMADESC_USB_ZERO_MASK		(1 << 0)

/* status */
#define DMADESC_UNDER_MASK		(1 << 9)
#define DMADESC_APPEND_CRC		(1 << 8)
#define DMADESC_OVSIZE_MASK		(1 << 4)
#define DMADESC_RXER_MASK		(1 << 2)
#define DMADESC_CRC_MASK		(1 << 1)
#define DMADESC_OV_MASK			(1 << 0)
#define DMADESC_ERR_MASK		(DMADESC_UNDER_MASK | \
					 DMADESC_OVSIZE_MASK | \
					 DMADESC_RXER_MASK | \
					 DMADESC_CRC_MASK | \
					 DMADESC_OV_MASK)

struct bcm6368_enetsw {
	void __iomem *dma_base;
	void __iomem *dma_chan;
	void __iomem *dma_sram;

	struct device **pm;
	struct device_link **link_pm;
	int num_pms;

	struct clk **clock;
	unsigned int num_clocks;

	struct reset_control **reset;
	unsigned int num_resets;

	int copybreak;

	int irq_rx;
	int irq_tx;

	/* hw view of rx & tx dma ring */
	dma_addr_t rx_desc_dma;
	dma_addr_t tx_desc_dma;

	/* allocated size (in bytes) for rx & tx dma ring */
	unsigned int rx_desc_alloc_size;
	unsigned int tx_desc_alloc_size;

	struct napi_struct napi;

	/* dma channel id for rx */
	int rx_chan;

	/* number of dma desc in rx ring */
	int rx_ring_size;

	/* cpu view of rx dma ring */
	struct bcm6368_enetsw_desc *rx_desc_cpu;

	/* current number of armed descriptor given to hardware for rx */
	int rx_desc_count;

	/* next rx descriptor to fetch from hardware */
	int rx_curr_desc;

	/* next dirty rx descriptor to refill */
	int rx_dirty_desc;

	/* size of allocated rx buffer */
	unsigned int rx_buf_size;

	/* size of allocated rx frag */
	unsigned int rx_frag_size;

	/* list of buffer given to hw for rx */
	unsigned char **rx_buf;

	/* used when rx buffer allocation failed, so we defer rx queue
	 * refill */
	struct timer_list rx_timeout;

	/* lock rx_timeout against rx normal operation */
	spinlock_t rx_lock;

	/* dma channel id for tx */
	int tx_chan;

	/* number of dma desc in tx ring */
	int tx_ring_size;

	/* cpu view of rx dma ring */
	struct bcm6368_enetsw_desc *tx_desc_cpu;

	/* number of available descriptor for tx */
	int tx_desc_count;

	/* next tx descriptor avaiable */
	int tx_curr_desc;

	/* next dirty tx descriptor to reclaim */
	int tx_dirty_desc;

	/* list of skb given to hw for tx */
	struct sk_buff **tx_skb;

	/* lock used by tx reclaim and xmit */
	spinlock_t tx_lock;

	/* network device reference */
	struct net_device *net_dev;

	/* platform device reference */
	struct platform_device *pdev;
};

static inline void dma_writel(struct bcm6368_enetsw *priv, u32 val, u32 off)
{
	__raw_writel(val, priv->dma_base + off);
}

static inline u32 dma_readl(struct bcm6368_enetsw *priv, u32 off, int chan)
{
	return __raw_readl(priv->dma_chan + off + chan * DMA_CHAN_WIDTH);
}

static inline void dmac_writel(struct bcm6368_enetsw *priv, u32 val, u32 off,
			       int chan)
{
	__raw_writel(val, priv->dma_chan + off + chan * DMA_CHAN_WIDTH);
}

static inline void dmas_writel(struct bcm6368_enetsw *priv, u32 val,
				    u32 off, int chan)
{
	__raw_writel(val, priv->dma_sram + off + chan * DMA_CHAN_WIDTH);
}

/*
 * refill rx queue
 */
static int bcm6368_enetsw_refill_rx(struct net_device *ndev, bool napi_mode)
{
	struct bcm6368_enetsw *priv = netdev_priv(ndev);
	struct platform_device *pdev = priv->pdev;
	struct device *dev = &pdev->dev;

	while (priv->rx_desc_count < priv->rx_ring_size) {
		struct bcm6368_enetsw_desc *desc;
		int desc_idx;
		u32 len_stat;

		desc_idx = priv->rx_dirty_desc;
		desc = &priv->rx_desc_cpu[desc_idx];

		if (!priv->rx_buf[desc_idx]) {
			unsigned char *buf;
			dma_addr_t p;

			if (likely(napi_mode))
				buf = napi_alloc_frag(priv->rx_frag_size);
			else
				buf = netdev_alloc_frag(priv->rx_frag_size);

			if (unlikely(!buf))
				break;

			p = dma_map_single(dev, buf + NET_SKB_PAD,
					   priv->rx_buf_size, DMA_FROM_DEVICE);
			if (unlikely(dma_mapping_error(dev, p))) {
				skb_free_frag(buf);
				break;
			}

			priv->rx_buf[desc_idx] = buf;
			desc->address = p;
		}

		len_stat = priv->rx_buf_size << DMADESC_LENGTH_SHIFT;
		len_stat |= DMADESC_OWNER_MASK;
		if (priv->rx_dirty_desc == priv->rx_ring_size - 1) {
			len_stat |= DMADESC_WRAP_MASK;
			priv->rx_dirty_desc = 0;
		} else {
			priv->rx_dirty_desc++;
		}
		wmb();
		desc->len_stat = len_stat;

		priv->rx_desc_count++;

		/* tell dma engine we allocated one buffer */
		dma_writel(priv, 1, DMA_BUFALLOC_REG(priv->rx_chan));
	}

	/* If rx ring is still empty, set a timer to try allocating
	 * again at a later time. */
	if (priv->rx_desc_count == 0 && netif_running(ndev)) {
		dev_warn(dev, "unable to refill rx ring\n");
		priv->rx_timeout.expires = jiffies + HZ;
		add_timer(&priv->rx_timeout);
	}

	return 0;
}

/*
 * timer callback to defer refill rx queue in case we're OOM
 */
static void bcm6368_enetsw_refill_rx_timer(struct timer_list *t)
{
	struct bcm6368_enetsw *priv = from_timer(priv, t, rx_timeout);
	struct net_device *ndev = priv->net_dev;

	spin_lock(&priv->rx_lock);
	bcm6368_enetsw_refill_rx(ndev, false);
	spin_unlock(&priv->rx_lock);
}

/*
 * extract packet from rx queue
 */
static int bcm6368_enetsw_receive_queue(struct net_device *ndev, int budget)
{
	struct bcm6368_enetsw *priv = netdev_priv(ndev);
	struct platform_device *pdev = priv->pdev;
	struct device *dev = &pdev->dev;
	struct list_head rx_list;
	struct sk_buff *skb;
	int processed = 0;

	INIT_LIST_HEAD(&rx_list);

	/* don't scan ring further than number of refilled
	 * descriptor */
	if (budget > priv->rx_desc_count)
		budget = priv->rx_desc_count;

	do {
		struct bcm6368_enetsw_desc *desc;
		unsigned int frag_size;
		unsigned char *buf;
		int desc_idx;
		u32 len_stat;
		unsigned int len;

		desc_idx = priv->rx_curr_desc;
		desc = &priv->rx_desc_cpu[desc_idx];

		/* make sure we actually read the descriptor status at
		 * each loop */
		rmb();

		len_stat = desc->len_stat;

		/* break if dma ownership belongs to hw */
		if (len_stat & DMADESC_OWNER_MASK)
			break;

		processed++;
		priv->rx_curr_desc++;
		if (priv->rx_curr_desc == priv->rx_ring_size)
			priv->rx_curr_desc = 0;

		/* if the packet does not have start of packet _and_
		 * end of packet flag set, then just recycle it */
		if ((len_stat & DMADESC_ESOP_MASK) != DMADESC_ESOP_MASK) {
			ndev->stats.rx_dropped++;
			continue;
		}

		/* valid packet */
		buf = priv->rx_buf[desc_idx];
		len = (len_stat & DMADESC_LENGTH_MASK)
		      >> DMADESC_LENGTH_SHIFT;
		/* don't include FCS */
		len -= 4;

		if (len < priv->copybreak) {
			unsigned int nfrag_size = ENETSW_FRAG_SIZE(len);
			unsigned char *nbuf = napi_alloc_frag(nfrag_size);

			if (unlikely(!nbuf)) {
				/* forget packet, just rearm desc */
				ndev->stats.rx_dropped++;
				continue;
			}

			dma_sync_single_for_cpu(dev, desc->address,
						len, DMA_FROM_DEVICE);
			memcpy(nbuf + NET_SKB_PAD, buf + NET_SKB_PAD, len);
			dma_sync_single_for_device(dev, desc->address,
						   len, DMA_FROM_DEVICE);
			buf = nbuf;
			frag_size = nfrag_size;
		} else {
			dma_unmap_single(dev, desc->address,
					 priv->rx_buf_size, DMA_FROM_DEVICE);
			priv->rx_buf[desc_idx] = NULL;
			frag_size = priv->rx_frag_size;
		}

		skb = napi_build_skb(buf, frag_size);
		if (unlikely(!skb)) {
			skb_free_frag(buf);
			ndev->stats.rx_dropped++;
			continue;
		}

		skb_reserve(skb, NET_SKB_PAD);
		skb_put(skb, len);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
		list_add_tail(&skb->list, &rx_list);
	} while (processed < budget);

	list_for_each_entry(skb, &rx_list, list)
		skb->protocol = eth_type_trans(skb, ndev);
	netif_receive_skb_list(&rx_list);
	priv->rx_desc_count -= processed;

	if (processed || !priv->rx_desc_count) {
		bcm6368_enetsw_refill_rx(ndev, true);

		/* kick rx dma */
		dmac_writel(priv, DMAC_CHANCFG_EN_MASK,
			    DMAC_CHANCFG_REG, priv->rx_chan);
	}

	return processed;
}

/*
 * try to or force reclaim of transmitted buffers
 */
static int bcm6368_enetsw_tx_reclaim(struct net_device *ndev, int force,
				     int budget)
{
	struct bcm6368_enetsw *priv = netdev_priv(ndev);
	struct platform_device *pdev = priv->pdev;
	struct device *dev = &pdev->dev;
	unsigned int bytes = 0;
	int released = 0;

	while (priv->tx_desc_count < priv->tx_ring_size) {
		struct bcm6368_enetsw_desc *desc;
		struct sk_buff *skb;

		/* We run in a bh and fight against start_xmit, which
		 * is called with bh disabled */
		spin_lock(&priv->tx_lock);

		desc = &priv->tx_desc_cpu[priv->tx_dirty_desc];

		if (!force && (desc->len_stat & DMADESC_OWNER_MASK)) {
			spin_unlock(&priv->tx_lock);
			break;
		}

		/* ensure other field of the descriptor were not read
		 * before we checked ownership */
		rmb();

		skb = priv->tx_skb[priv->tx_dirty_desc];
		priv->tx_skb[priv->tx_dirty_desc] = NULL;
		dma_unmap_single(dev, desc->address, skb->len,
				 DMA_TO_DEVICE);

		priv->tx_dirty_desc++;
		if (priv->tx_dirty_desc == priv->tx_ring_size)
			priv->tx_dirty_desc = 0;
		priv->tx_desc_count++;

		spin_unlock(&priv->tx_lock);

		if (desc->len_stat & DMADESC_UNDER_MASK)
			ndev->stats.tx_errors++;

		bytes += skb->len;
		napi_consume_skb(skb, budget);
		released++;
	}

	netdev_completed_queue(ndev, released, bytes);

	if (netif_queue_stopped(ndev) && released)
		netif_wake_queue(ndev);

	return released;
}

/*
 * poll func, called by network core
 */
static int bcm6368_enetsw_poll(struct napi_struct *napi, int budget)
{
	struct bcm6368_enetsw *priv = container_of(napi, struct bcm6368_enetsw, napi);
	struct net_device *ndev = priv->net_dev;
	int rx_work_done;

	/* ack interrupts */
	dmac_writel(priv, DMAC_IR_PKTDONE_MASK,
			 DMAC_IR_REG, priv->rx_chan);
	dmac_writel(priv, DMAC_IR_PKTDONE_MASK,
			 DMAC_IR_REG, priv->tx_chan);

	/* reclaim sent skb */
	bcm6368_enetsw_tx_reclaim(ndev, 0, budget);

	spin_lock(&priv->rx_lock);
	rx_work_done = bcm6368_enetsw_receive_queue(ndev, budget);
	spin_unlock(&priv->rx_lock);

	if (rx_work_done >= budget) {
		/* rx queue is not yet empty/clean */
		return rx_work_done;
	}

	/* no more packet in rx/tx queue, remove device from poll
	 * queue */
	napi_complete_done(napi, rx_work_done);

	/* restore rx/tx interrupt */
	dmac_writel(priv, DMAC_IR_PKTDONE_MASK,
		    DMAC_IRMASK_REG, priv->rx_chan);
	dmac_writel(priv, DMAC_IR_PKTDONE_MASK,
		    DMAC_IRMASK_REG, priv->tx_chan);

	return rx_work_done;
}

/*
 * rx/tx dma interrupt handler
 */
static irqreturn_t bcm6368_enetsw_isr_dma(int irq, void *dev_id)
{
	struct net_device *ndev = dev_id;
	struct bcm6368_enetsw *priv = netdev_priv(ndev);

	/* mask rx/tx interrupts */
	dmac_writel(priv, 0, DMAC_IRMASK_REG, priv->rx_chan);
	dmac_writel(priv, 0, DMAC_IRMASK_REG, priv->tx_chan);

	napi_schedule(&priv->napi);

	return IRQ_HANDLED;
}

/*
 * tx request callback
 */
static netdev_tx_t
bcm6368_enetsw_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct bcm6368_enetsw *priv = netdev_priv(ndev);
	struct platform_device *pdev = priv->pdev;
	struct device *dev = &pdev->dev;
	struct bcm6368_enetsw_desc *desc;
	u32 len_stat;
	netdev_tx_t ret;
	dma_addr_t p;

	/* lock against tx reclaim */
	spin_lock(&priv->tx_lock);

	/* make sure the tx hw queue is not full, should not happen
	 * since we stop queue before it's the case */
	if (unlikely(!priv->tx_desc_count)) {
		netif_stop_queue(ndev);
		dev_err(dev, "xmit called with no tx desc available?\n");
		ret = NETDEV_TX_BUSY;
		goto out_unlock;
	}

	/* pad small packets */
	if (skb->len < (ETH_ZLEN + ETH_FCS_LEN)) {
		int needed = (ETH_ZLEN + ETH_FCS_LEN) - skb->len;
		char *data;

		if (unlikely(skb_tailroom(skb) < needed)) {
			struct sk_buff *nskb;

			nskb = skb_copy_expand(skb, 0, needed, GFP_ATOMIC);
			if (!nskb) {
				ret = NETDEV_TX_BUSY;
				goto out_unlock;
			}

			dev_kfree_skb(skb);
			skb = nskb;
		}
		data = skb_put_zero(skb, needed);
	}

	/* fill descriptor */
	p = dma_map_single(dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(dev, p))) {
		dev_kfree_skb(skb);
		ret = NETDEV_TX_OK;
		goto out_unlock;
	}

	/* point to the next available desc */
	desc = &priv->tx_desc_cpu[priv->tx_curr_desc];
	priv->tx_skb[priv->tx_curr_desc] = skb;
	desc->address = p;

	len_stat = (skb->len << DMADESC_LENGTH_SHIFT) & DMADESC_LENGTH_MASK;
	len_stat |= DMADESC_ESOP_MASK | DMADESC_APPEND_CRC |
		    DMADESC_OWNER_MASK;

	priv->tx_curr_desc++;
	if (priv->tx_curr_desc == priv->tx_ring_size) {
		priv->tx_curr_desc = 0;
		len_stat |= DMADESC_WRAP_MASK;
	}
	priv->tx_desc_count--;

	/* dma might be already polling, make sure we update desc
	 * fields in correct order */
	wmb();
	desc->len_stat = len_stat;
	wmb();

	netdev_sent_queue(ndev, skb->len);

	/* kick tx dma */
	dmac_writel(priv, DMAC_CHANCFG_EN_MASK, DMAC_CHANCFG_REG,
		    priv->tx_chan);

	/* stop queue if no more desc available */
	if (!priv->tx_desc_count)
		netif_stop_queue(ndev);

	ndev->stats.tx_bytes += skb->len;
	ndev->stats.tx_packets++;
	ret = NETDEV_TX_OK;

out_unlock:
	spin_unlock(&priv->tx_lock);
	return ret;
}

/*
 * disable dma in given channel
 */
static void bcm6368_enetsw_disable_dma(struct bcm6368_enetsw *priv, int chan)
{
	int limit = 1000;

	dmac_writel(priv, 0, DMAC_CHANCFG_REG, chan);

	do {
		u32 val;

		val = dma_readl(priv, DMAC_CHANCFG_REG, chan);
		if (!(val & DMAC_CHANCFG_EN_MASK))
			break;

		udelay(1);
	} while (limit--);
}

static int bcm6368_enetsw_open(struct net_device *ndev)
{
	struct bcm6368_enetsw *priv = netdev_priv(ndev);
	struct platform_device *pdev = priv->pdev;
	struct device *dev = &pdev->dev;
	int i, ret;
	unsigned int size;
	void *p;
	u32 val;

	/* mask all interrupts and request them */
	dmac_writel(priv, 0, DMAC_IRMASK_REG, priv->rx_chan);
	dmac_writel(priv, 0, DMAC_IRMASK_REG, priv->tx_chan);

	ret = request_irq(priv->irq_rx, bcm6368_enetsw_isr_dma,
			  0, ndev->name, ndev);
	if (ret)
		goto out_freeirq;

	if (priv->irq_tx != -1) {
		ret = request_irq(priv->irq_tx, bcm6368_enetsw_isr_dma,
				  0, ndev->name, ndev);
		if (ret)
			goto out_freeirq_rx;
	}

	/* allocate rx dma ring */
	size = priv->rx_ring_size * sizeof(struct bcm6368_enetsw_desc);
	p = dma_alloc_coherent(dev, size, &priv->rx_desc_dma, GFP_KERNEL);
	if (!p) {
		dev_err(dev, "cannot allocate rx ring %u\n", size);
		ret = -ENOMEM;
		goto out_freeirq_tx;
	}

	memset(p, 0, size);
	priv->rx_desc_alloc_size = size;
	priv->rx_desc_cpu = p;

	/* allocate tx dma ring */
	size = priv->tx_ring_size * sizeof(struct bcm6368_enetsw_desc);
	p = dma_alloc_coherent(dev, size, &priv->tx_desc_dma, GFP_KERNEL);
	if (!p) {
		dev_err(dev, "cannot allocate tx ring\n");
		ret = -ENOMEM;
		goto out_free_rx_ring;
	}

	memset(p, 0, size);
	priv->tx_desc_alloc_size = size;
	priv->tx_desc_cpu = p;

	priv->tx_skb = kzalloc(sizeof(struct sk_buff *) * priv->tx_ring_size,
			       GFP_KERNEL);
	if (!priv->tx_skb) {
		dev_err(dev, "cannot allocate tx skb queue\n");
		ret = -ENOMEM;
		goto out_free_tx_ring;
	}

	priv->tx_desc_count = priv->tx_ring_size;
	priv->tx_dirty_desc = 0;
	priv->tx_curr_desc = 0;
	spin_lock_init(&priv->tx_lock);

	/* init & fill rx ring with buffers */
	priv->rx_buf = kzalloc(sizeof(unsigned char *) * priv->rx_ring_size,
			       GFP_KERNEL);
	if (!priv->rx_buf) {
		dev_err(dev, "cannot allocate rx buffer queue\n");
		ret = -ENOMEM;
		goto out_free_tx_skb;
	}

	priv->rx_desc_count = 0;
	priv->rx_dirty_desc = 0;
	priv->rx_curr_desc = 0;

	/* initialize flow control buffer allocation */
	dma_writel(priv, DMA_BUFALLOC_FORCE_MASK | 0,
		   DMA_BUFALLOC_REG(priv->rx_chan));

	if (bcm6368_enetsw_refill_rx(ndev, false)) {
		dev_err(dev, "cannot allocate rx buffer queue\n");
		ret = -ENOMEM;
		goto out;
	}

	/* write rx & tx ring addresses */
	dmas_writel(priv, priv->rx_desc_dma,
		    DMAS_RSTART_REG, priv->rx_chan);
	dmas_writel(priv, priv->tx_desc_dma,
		    DMAS_RSTART_REG, priv->tx_chan);

	/* clear remaining state ram for rx & tx channel */
	dmas_writel(priv, 0, DMAS_SRAM2_REG, priv->rx_chan);
	dmas_writel(priv, 0, DMAS_SRAM2_REG, priv->tx_chan);
	dmas_writel(priv, 0, DMAS_SRAM3_REG, priv->rx_chan);
	dmas_writel(priv, 0, DMAS_SRAM3_REG, priv->tx_chan);
	dmas_writel(priv, 0, DMAS_SRAM4_REG, priv->rx_chan);
	dmas_writel(priv, 0, DMAS_SRAM4_REG, priv->tx_chan);

	/* set dma maximum burst len */
	dmac_writel(priv, ENETSW_DMA_MAXBURST,
		    DMAC_MAXBURST_REG, priv->rx_chan);
	dmac_writel(priv, ENETSW_DMA_MAXBURST,
		    DMAC_MAXBURST_REG, priv->tx_chan);

	/* set flow control low/high threshold to 1/3 / 2/3 */
	val = priv->rx_ring_size / 3;
	dma_writel(priv, val, DMA_FLOWCL_REG(priv->rx_chan));
	val = (priv->rx_ring_size * 2) / 3;
	dma_writel(priv, val, DMA_FLOWCH_REG(priv->rx_chan));

	/* all set, enable mac and interrupts, start dma engine and
	 * kick rx dma channel
	 */
	wmb();
	dma_writel(priv, DMA_CFG_EN_MASK, DMA_CFG_REG);
	dmac_writel(priv, DMAC_CHANCFG_EN_MASK,
		    DMAC_CHANCFG_REG, priv->rx_chan);

	/* watch "packet transferred" interrupt in rx and tx */
	dmac_writel(priv, DMAC_IR_PKTDONE_MASK,
		    DMAC_IR_REG, priv->rx_chan);
	dmac_writel(priv, DMAC_IR_PKTDONE_MASK,
		    DMAC_IR_REG, priv->tx_chan);

	/* make sure we enable napi before rx interrupt  */
	napi_enable(&priv->napi);

	dmac_writel(priv, DMAC_IR_PKTDONE_MASK,
		    DMAC_IRMASK_REG, priv->rx_chan);
	dmac_writel(priv, DMAC_IR_PKTDONE_MASK,
		    DMAC_IRMASK_REG, priv->tx_chan);

	netif_carrier_on(ndev);
	netif_start_queue(ndev);

	return 0;

out:
	for (i = 0; i < priv->rx_ring_size; i++) {
		struct bcm6368_enetsw_desc *desc;

		if (!priv->rx_buf[i])
			continue;

		desc = &priv->rx_desc_cpu[i];
		dma_unmap_single(dev, desc->address, priv->rx_buf_size,
				 DMA_FROM_DEVICE);
		skb_free_frag(priv->rx_buf[i]);
	}
	kfree(priv->rx_buf);

out_free_tx_skb:
	kfree(priv->tx_skb);

out_free_tx_ring:
	dma_free_coherent(dev, priv->tx_desc_alloc_size,
			  priv->tx_desc_cpu, priv->tx_desc_dma);

out_free_rx_ring:
	dma_free_coherent(dev, priv->rx_desc_alloc_size,
			  priv->rx_desc_cpu, priv->rx_desc_dma);

out_freeirq_tx:
	if (priv->irq_tx != -1)
		free_irq(priv->irq_tx, ndev);

out_freeirq_rx:
	free_irq(priv->irq_rx, ndev);

out_freeirq:
	return ret;
}

static int bcm6368_enetsw_stop(struct net_device *ndev)
{
	struct bcm6368_enetsw *priv = netdev_priv(ndev);
	struct platform_device *pdev = priv->pdev;
	struct device *dev = &pdev->dev;
	int i;

	netif_stop_queue(ndev);
	napi_disable(&priv->napi);
	del_timer_sync(&priv->rx_timeout);

	/* mask all interrupts */
	dmac_writel(priv, 0, DMAC_IRMASK_REG, priv->rx_chan);
	dmac_writel(priv, 0, DMAC_IRMASK_REG, priv->tx_chan);

	/* disable dma & mac */
	bcm6368_enetsw_disable_dma(priv, priv->tx_chan);
	bcm6368_enetsw_disable_dma(priv, priv->rx_chan);

	/* force reclaim of all tx buffers */
	bcm6368_enetsw_tx_reclaim(ndev, 1, 0);

	/* free the rx buffer ring */
	for (i = 0; i < priv->rx_ring_size; i++) {
		struct bcm6368_enetsw_desc *desc;

		if (!priv->rx_buf[i])
			continue;

		desc = &priv->rx_desc_cpu[i];
		dma_unmap_single_attrs(dev, desc->address, priv->rx_buf_size,
				       DMA_FROM_DEVICE,
				       DMA_ATTR_SKIP_CPU_SYNC);
		skb_free_frag(priv->rx_buf[i]);
	}

	/* free remaining allocated memory */
	kfree(priv->rx_buf);
	kfree(priv->tx_skb);
	dma_free_coherent(dev, priv->rx_desc_alloc_size,
			  priv->rx_desc_cpu, priv->rx_desc_dma);
	dma_free_coherent(dev, priv->tx_desc_alloc_size,
			  priv->tx_desc_cpu, priv->tx_desc_dma);
	if (priv->irq_tx != -1)
		free_irq(priv->irq_tx, ndev);
	free_irq(priv->irq_rx, ndev);

	netdev_reset_queue(ndev);

	return 0;
}

static const struct net_device_ops bcm6368_enetsw_ops = {
	.ndo_open = bcm6368_enetsw_open,
	.ndo_stop = bcm6368_enetsw_stop,
	.ndo_start_xmit = bcm6368_enetsw_start_xmit,
};

static int bcm6368_enetsw_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct bcm6368_enetsw *priv;
	struct net_device *ndev;
	struct resource *res;
	unsigned char dev_addr[ETH_ALEN];
	unsigned i;
	int num_resets;
	int ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	platform_set_drvdata(pdev, ndev);
	SET_NETDEV_DEV(ndev, dev);

	priv = netdev_priv(ndev);
	priv->pdev = pdev;
	priv->net_dev = ndev;

	priv->num_pms = of_count_phandle_with_args(node, "power-domains",
						   "#power-domain-cells");
	if (priv->num_pms > 1) {
		priv->pm = devm_kcalloc(dev, priv->num_pms,
					sizeof(struct device *), GFP_KERNEL);
		if (!priv->pm)
			return -ENOMEM;

		priv->link_pm = devm_kcalloc(dev, priv->num_pms,
					     sizeof(struct device_link *),
					     GFP_KERNEL);
		if (!priv->link_pm)
			return -ENOMEM;

		for (i = 0; i < priv->num_pms; i++) {
			priv->pm[i] = genpd_dev_pm_attach_by_id(dev, i);
			if (IS_ERR(priv->pm[i])) {
				dev_err(dev, "error getting pm %d\n", i);
				return -EINVAL;
			}

			priv->link_pm[i] = device_link_add(dev, priv->pm[i],
				DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME |
				DL_FLAG_RPM_ACTIVE);
		}
	}

	pm_runtime_enable(dev);
	pm_runtime_no_callbacks(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pm_runtime_disable(dev);
		dev_info(dev, "PM prober defer: ret=%d\n", ret);
		return -EPROBE_DEFER;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dma");
	priv->dma_base = devm_ioremap_resource(dev, res);
	if (IS_ERR_OR_NULL(priv->dma_base))
		return PTR_ERR(priv->dma_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "dma-channels");
	priv->dma_chan = devm_ioremap_resource(dev, res);
	if (IS_ERR_OR_NULL(priv->dma_chan))
		return PTR_ERR(priv->dma_chan);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dma-sram");
	priv->dma_sram = devm_ioremap_resource(dev, res);
	if (IS_ERR_OR_NULL(priv->dma_sram))
		return PTR_ERR(priv->dma_sram);

	priv->irq_rx = platform_get_irq_byname(pdev, "rx");
	if (!priv->irq_rx)
		return -ENODEV;

	priv->irq_tx = platform_get_irq_byname(pdev, "tx");
	if (!priv->irq_tx)
		return -ENODEV;
	else if (priv->irq_tx < 0)
		priv->irq_tx = -1;

	if (device_property_read_u32(dev, "dma-rx", &priv->rx_chan))
		return -ENODEV;

	if (device_property_read_u32(dev, "dma-tx", &priv->tx_chan))
		return -ENODEV;

	priv->rx_ring_size = ENETSW_DEF_RX_DESC;
	priv->tx_ring_size = ENETSW_DEF_TX_DESC;
	priv->copybreak = ENETSW_DEF_CPY_BREAK;

	of_get_mac_address(node, dev_addr);
	if (is_valid_ether_addr(dev_addr)) {
		dev_addr_set(ndev, dev_addr);
		dev_info(dev, "mtd mac %pM\n", dev_addr);
	} else {
		eth_hw_addr_random(ndev);
		dev_info(dev, "random mac\n");
	}

	priv->rx_buf_size = ALIGN(ENETSW_MAX_FRAME,
				  ENETSW_DMA_MAXBURST * 4);

	priv->rx_frag_size = ENETSW_FRAG_SIZE(priv->rx_buf_size);

	priv->num_clocks = of_clk_get_parent_count(node);
	if (priv->num_clocks) {
		priv->clock = devm_kcalloc(dev, priv->num_clocks,
					   sizeof(struct clk *), GFP_KERNEL);
		if (IS_ERR_OR_NULL(priv->clock))
			return PTR_ERR(priv->clock);
	}
	for (i = 0; i < priv->num_clocks; i++) {
		priv->clock[i] = of_clk_get(node, i);
		if (IS_ERR(priv->clock[i])) {
			dev_err(dev, "error getting clock %d\n", i);
			return PTR_ERR(priv->clock[i]);
		}

		ret = clk_prepare_enable(priv->clock[i]);
		if (ret) {
			dev_err(dev, "error enabling clock %d\n", i);
			return ret;
		}
	}

	num_resets = of_count_phandle_with_args(node, "resets",
						"#reset-cells");
	if (num_resets > 0)
		priv->num_resets = num_resets;
	else
		priv->num_resets = 0;
	if (priv->num_resets) {
		priv->reset = devm_kcalloc(dev, priv->num_resets,
					   sizeof(struct reset_control *),
					   GFP_KERNEL);
		if (IS_ERR_OR_NULL(priv->reset))
			return PTR_ERR(priv->reset);
	}
	for (i = 0; i < priv->num_resets; i++) {
		priv->reset[i] = devm_reset_control_get_by_index(dev, i);
		if (IS_ERR(priv->reset[i])) {
			dev_err(dev, "error getting reset %d\n", i);
			return PTR_ERR(priv->reset[i]);
		}

		ret = reset_control_reset(priv->reset[i]);
		if (ret) {
			dev_err(dev, "error performing reset %d\n", i);
			return ret;
		}
	}

	spin_lock_init(&priv->rx_lock);

	timer_setup(&priv->rx_timeout, bcm6368_enetsw_refill_rx_timer, 0);

	/* register netdevice */
	ndev->netdev_ops = &bcm6368_enetsw_ops;
	ndev->min_mtu = ETH_ZLEN;
	ndev->mtu = ETH_DATA_LEN;
	ndev->max_mtu = ENETSW_MAX_MTU;
	netif_napi_add_weight(ndev, &priv->napi, bcm6368_enetsw_poll, 16);

	ret = devm_register_netdev(dev, ndev);
	if (ret) {
		netif_napi_del(&priv->napi);
		goto out_disable_clk;
	}

	netif_carrier_off(ndev);

	dev_info(dev, "%s at 0x%px, IRQ %d\n", ndev->name, priv->dma_base, ndev->irq);

	return 0;

out_disable_clk:
	for (i = 0; i < priv->num_resets; i++)
		reset_control_assert(priv->reset[i]);

	for (i = 0; i < priv->num_clocks; i++)
		clk_disable_unprepare(priv->clock[i]);

	return ret;
}

static void bcm6368_enetsw_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct bcm6368_enetsw *priv = netdev_priv(ndev);
	unsigned int i;

	pm_runtime_put_sync(dev);
	for (i = 0; priv->pm && i < priv->num_pms; i++) {
		dev_pm_domain_detach(priv->pm[i], true);
		device_link_del(priv->link_pm[i]);
	}

	for (i = 0; i < priv->num_resets; i++)
		reset_control_assert(priv->reset[i]);

	for (i = 0; i < priv->num_clocks; i++)
		clk_disable_unprepare(priv->clock[i]);
}

static const struct of_device_id bcm6368_enetsw_of_match[] = {
	{ .compatible = "brcm,bcm6318-enetsw", },
	{ .compatible = "brcm,bcm6328-enetsw", },
	{ .compatible = "brcm,bcm6362-enetsw", },
	{ .compatible = "brcm,bcm6368-enetsw", },
	{ .compatible = "brcm,bcm63268-enetsw", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, bcm6368_enetsw_of_match);

static struct platform_driver bcm6368_enetsw_driver = {
	.driver = {
		.name = "bcm6368-enetsw",
		.of_match_table = bcm6368_enetsw_of_match,
	},
	.probe	= bcm6368_enetsw_probe,
	.remove_new	= bcm6368_enetsw_remove,
};
module_platform_driver(bcm6368_enetsw_driver);

MODULE_AUTHOR("Álvaro Fernández Rojas <noltari@gmail.com>");
MODULE_DESCRIPTION("BCM6368 Ethernet Switch Controller Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:bcm6368-enetsw");
