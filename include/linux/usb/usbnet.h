// SPDX-License-Identifier: GPL-2.0+
/*
 * USB Networking Link Interface
 *
 * Copyright (C) 2000-2005 by David Brownell <dbrownell@users.sourceforge.net>
 * Copyright (C) 2003-2005 David Hollis <dhollis@davehollis.com>
 */

#ifndef	__LINUX_USB_USBNET_H
#define	__LINUX_USB_USBNET_H

#include <linux/mii.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/spinlock.h>

struct xsk_buff_pool;
struct dma_pool;
struct usbnet;
struct usbnet_xsk_tx_urb_ctx;

/* AF_XDP zero-copy TX support (see usbnet_xsk_*() in usbnet.c).
 *
 * Minidrivers that can batch multiple AF_XDP TX descriptors into a single
 * USB transfer (e.g. CDC NCM's NTB aggregation) opt in by filling out
 * driver_info.xsk_ops. usbnet.c provides the generic xsk pool binding
 * (ndo_bpf), persistent UMEM DMA mapping, and USB SG-URB submission /
 * completion plumbing; the minidriver only describes its own framing
 * format.
 */
struct usbnet_xsk_limits {
	/* max number of AF_XDP TX descriptors batched into one USB
	 * transfer/completion (e.g. NCM's tx_max_datagrams). */
	unsigned int max_frames;
	/* max bytes needed for the framing header(s) built by tx_build()
	 * (e.g. NTH + NDP for max_frames datagrams). */
	unsigned int max_hdr_size;
	/* max bytes needed for a single alignment padding run between (or
	 * after) datagrams. */
	unsigned int max_pad_size;
	/* soft byte budget for one aggregated USB transfer (e.g. NCM's
	 * tx_curr_size); used to decide when to stop batching more
	 * descriptors into the current transfer. A single descriptor is
	 * always sent even if it alone exceeds this budget. */
	unsigned int max_tx_size;
};

/* Scratch buffers/scatterlist handed to tx_build() by usbnet.c. hdr_buf and
 * pad_buf are DMA-coherent (no cache sync needed when written/read).
 * pad_buf is zero-filled and read-only (shared across concurrent in-flight
 * URBs); hdr_buf is private to this call and safe to overwrite freely.
 */
struct usbnet_xsk_tx_ctx {
	struct scatterlist	*sg;
	unsigned int		sg_max;
	void			*hdr_buf;
	dma_addr_t		hdr_buf_dma;
	unsigned int		hdr_buf_size;
	void			*pad_buf;
	dma_addr_t		pad_buf_dma;
	unsigned int		pad_buf_size;
};

struct usbnet_xsk_ops {
	/* report batching limits; called once when the xsk pool is bound */
	int (*get_limits)(struct usbnet *dev, struct usbnet_xsk_limits *lim);

	/* Build one USB transfer out of up to @n descriptors already peeked
	 * from the xsk pool (persistent DMA addresses in @dma, lengths in
	 * @len, both already synced for device via
	 * xsk_buff_raw_dma_sync_for_device()). Must fill in up to
	 * tx->sg_max scatterlist entries (zero-copy for payloads, using
	 * @dma/@len directly; tx->hdr_buf/tx->pad_buf for framing/padding)
	 * and return the number of descriptors actually consumed (<= n,
	 * > 0 on success), the number of sg entries used via *nsg and the
	 * total transfer length via *total_len. Returns a negative errno
	 * if not even one descriptor could be built (e.g. n == 0).
	 *
	 * @pages/@offs give the struct page and in-page offset backing each
	 * @dma entry (needed to build sg entries with a valid struct page:
	 * some USB host controllers (e.g. xhci) copy through a "bounce
	 * buffer" via sg_pcopy_to_buffer() when a TD isn't aligned to the
	 * endpoint's max packet size, which requires sg_page() to resolve to
	 * real memory -- a bare dma_addr_t with no page is silently treated
	 * as a zero-length copy, corrupting the transfer).
	 */
	int (*tx_build)(struct usbnet *dev, struct usbnet_xsk_tx_ctx *tx,
			const dma_addr_t *dma, struct page **pages,
			const unsigned int *offs, const u32 *len,
			unsigned int n, unsigned int *nsg,
			unsigned int *total_len);
};

/* interface from usbnet core to each USB networking link we handle */
struct usbnet {
	/* housekeeping */
	struct usb_device	*udev;
	struct usb_interface	*intf;
	const struct driver_info *driver_info;
	const char		*driver_name;
	void			*driver_priv;
	wait_queue_head_t	wait;
	struct mutex		phy_mutex;
	unsigned char		suspend_count;
	unsigned char		pkt_cnt, pkt_err;
	unsigned short		rx_qlen, tx_qlen;
	unsigned		can_dma_sg:1;

	/* i/o info: pipes etc */
	unsigned		in, out;
	struct usb_host_endpoint *status;
	unsigned		maxpacket;
	struct timer_list	delay;
	const char		*padding_pkt;

	/* protocol/interface state */
	struct net_device	*net;
	int			msg_enable;
	unsigned long		data[5];
	u32			xid;
	u32			hard_mtu;	/* count any extra framing */
	size_t			rx_urb_size;	/* size for rx urbs */
	struct mii_if_info	mii;
	long			rx_speed;	/* If MII not used */
	long			tx_speed;	/* If MII not used */
#		define SPEED_UNSET	-1

	/* various kinds of pending driver work */
	struct sk_buff_head	rxq;
	struct sk_buff_head	txq;
	struct sk_buff_head	done;
	struct sk_buff_head	rxq_pause;
	struct urb		*interrupt;
	unsigned		interrupt_count;
	struct mutex		interrupt_mutex;
	struct usb_anchor	deferred;
	struct work_struct	bh_work;

	struct work_struct	kevent;
	unsigned long		flags;

	/* AF_XDP zero-copy TX (optional, see driver_info->xsk_ops) */
	struct xsk_buff_pool	*xsk_pool;
	spinlock_t		xsk_tx_lock;
	bool			xsk_tx_draining;
	bool			xsk_tx_rerun;
	atomic_t		xsk_tx_inflight;
	struct dma_pool		*xsk_hdr_pool;
	struct usbnet_xsk_tx_urb_ctx *xsk_tx_slots;
	struct page		**xsk_umem_pages;
	unsigned int		xsk_umem_pages_cnt;
	unsigned int		xsk_hdr_size;
	unsigned int		xsk_max_frames;
	unsigned int		xsk_sg_max;
	unsigned int		xsk_max_tx_size;
	dma_addr_t		*xsk_tx_dma;
	u32			*xsk_tx_len;
	struct page		**xsk_tx_page;
	unsigned int		*xsk_tx_off;
	bool			xsk_tx_have_spill;
	dma_addr_t		xsk_tx_spill_dma;
	u32			xsk_tx_spill_len;
	struct page		*xsk_tx_spill_page;
	unsigned int		xsk_tx_spill_off;
	void			*xsk_pad_buf;
	dma_addr_t		xsk_pad_buf_dma;
	unsigned int		xsk_pad_buf_size;
#		define EVENT_TX_HALT	0
#		define EVENT_RX_HALT	1
#		define EVENT_RX_MEMORY	2
#		define EVENT_STS_SPLIT	3
#		define EVENT_LINK_RESET	4
#		define EVENT_RX_PAUSED	5
#		define EVENT_DEV_ASLEEP 6
#		define EVENT_DEV_OPEN	7
#		define EVENT_DEVICE_REPORT_IDLE	8
#		define EVENT_NO_RUNTIME_PM	9
#		define EVENT_RX_KILL	10
#		define EVENT_LINK_CHANGE	11
#		define EVENT_SET_RX_MODE	12
#		define EVENT_NO_IP_ALIGN	13
#		define EVENT_LINK_CARRIER_ON	14
/* This one is special, as it indicates that the device is going away
 * there are cyclic dependencies between tasklet, timer and bh
 * that must be broken
 */
#		define EVENT_UNPLUG		31
};

static inline bool usbnet_going_away(struct usbnet *ubn)
{
	return test_bit(EVENT_UNPLUG, &ubn->flags);
}

static inline void usbnet_mark_going_away(struct usbnet *ubn)
{
	set_bit(EVENT_UNPLUG, &ubn->flags);
}

static inline struct usb_driver *driver_of(struct usb_interface *intf)
{
	return to_usb_driver(intf->dev.driver);
}

/* interface from the device/framing level "minidriver" to core */
struct driver_info {
	char		*description;

	int		flags;
/* framing is CDC Ethernet, not writing ZLPs (hw issues), or optionally: */
#define FLAG_FRAMING_NC	0x0001		/* guard against device dropouts */
#define FLAG_FRAMING_GL	0x0002		/* genelink batches packets */
#define FLAG_FRAMING_Z	0x0004		/* zaurus adds a trailer */
#define FLAG_FRAMING_RN	0x0008		/* RNDIS batches, plus huge header */

#define FLAG_NO_SETINT	0x0010		/* device can't set_interface() */
#define FLAG_ETHER	0x0020		/* maybe use "eth%d" names */

#define FLAG_FRAMING_AX 0x0040		/* AX88772/178 packets */
#define FLAG_WLAN	0x0080		/* use "wlan%d" names */
#define FLAG_AVOID_UNLINK_URBS 0x0100	/* don't unlink urbs at usbnet_stop() */
#define FLAG_SEND_ZLP	0x0200		/* hw requires ZLPs are sent */
#define FLAG_WWAN	0x0400		/* use "wwan%d" names */

#define FLAG_LINK_INTR	0x0800		/* updates link (carrier) status */

#define FLAG_POINTTOPOINT 0x1000	/* possibly use "usb%d" names */

/*
 * Indicates to usbnet, that USB driver accumulates multiple IP packets.
 * Affects statistic (counters) and short packet handling.
 */
#define FLAG_MULTI_PACKET	0x2000
#define FLAG_RX_ASSEMBLE	0x4000	/* rx packets may span >1 frames */
#define FLAG_NOARP		0x8000	/* device can't do ARP */

	/* init device ... can sleep, or cause probe() failure */
	int	(*bind)(struct usbnet *, struct usb_interface *);

	/* cleanup device ... can sleep, but can't fail */
	void	(*unbind)(struct usbnet *, struct usb_interface *);

	/* reset device ... can sleep */
	int	(*reset)(struct usbnet *);

	/* stop device ... can sleep */
	int	(*stop)(struct usbnet *);

	/* see if peer is connected ... can sleep */
	int	(*check_connect)(struct usbnet *);

	/* (dis)activate runtime power management */
	int	(*manage_power)(struct usbnet *, int);

	/* for status polling */
	void	(*status)(struct usbnet *, struct urb *);

	/* link reset handling, called from defer_kevent */
	int	(*link_reset)(struct usbnet *);

	/* fixup rx packet (strip framing) */
	int	(*rx_fixup)(struct usbnet *dev, struct sk_buff *skb);

	/* fixup tx packet (add framing) */
	struct sk_buff	*(*tx_fixup)(struct usbnet *dev,
				struct sk_buff *skb, gfp_t flags);

	/* recover from timeout */
	void	(*recover)(struct usbnet *dev);

	/* early initialization code, can sleep. This is for minidrivers
	 * having 'subminidrivers' that need to do extra initialization
	 * right after minidriver have initialized hardware. */
	int	(*early_init)(struct usbnet *dev);

	/* called by minidriver when receiving indication */
	void	(*indication)(struct usbnet *dev, void *ind, int indlen);

	/* rx mode change (device changes address list filtering) */
	void	(*set_rx_mode)(struct usbnet *dev);

	/* for new devices, use the descriptor-reading code instead */
	int		in;		/* rx endpoint */
	int		out;		/* tx endpoint */

	unsigned long	data;		/* Misc driver specific data */

	/* optional: AF_XDP zero-copy TX support, see usbnet_xsk_ops above */
	const struct usbnet_xsk_ops *xsk_ops;
};

/* Minidrivers are just drivers using the "usbnet" core as a powerful
 * network-specific subroutine library ... that happens to do pretty
 * much everything except custom framing and chip-specific stuff.
 */
extern int usbnet_probe(struct usb_interface *, const struct usb_device_id *);
extern int usbnet_suspend(struct usb_interface *, pm_message_t);
extern int usbnet_resume(struct usb_interface *);
extern void usbnet_disconnect(struct usb_interface *);
extern void usbnet_device_suggests_idle(struct usbnet *dev);

extern int usbnet_read_cmd(struct usbnet *dev, u8 cmd, u8 reqtype,
		    u16 value, u16 index, void *data, u16 size);
extern int usbnet_write_cmd(struct usbnet *dev, u8 cmd, u8 reqtype,
		    u16 value, u16 index, const void *data, u16 size);
extern int usbnet_read_cmd_nopm(struct usbnet *dev, u8 cmd, u8 reqtype,
		    u16 value, u16 index, void *data, u16 size);
extern int usbnet_write_cmd_nopm(struct usbnet *dev, u8 cmd, u8 reqtype,
		    u16 value, u16 index, const void *data, u16 size);
extern int usbnet_write_cmd_async(struct usbnet *dev, u8 cmd, u8 reqtype,
		    u16 value, u16 index, const void *data, u16 size);

/* Drivers that reuse some of the standard USB CDC infrastructure
 * (notably, using multiple interfaces according to the CDC
 * union descriptor) get some helper code.
 */
struct cdc_state {
	struct usb_cdc_header_desc	*header;
	struct usb_cdc_union_desc	*u;
	struct usb_cdc_ether_desc	*ether;
	struct usb_interface		*control;
	struct usb_interface		*data;
};

extern void usbnet_cdc_update_filter(struct usbnet *dev);
extern int usbnet_generic_cdc_bind(struct usbnet *, struct usb_interface *);
extern int usbnet_ether_cdc_bind(struct usbnet *dev, struct usb_interface *intf);
extern int usbnet_cdc_bind(struct usbnet *, struct usb_interface *);
extern void usbnet_cdc_unbind(struct usbnet *, struct usb_interface *);
extern void usbnet_cdc_status(struct usbnet *, struct urb *);
extern int usbnet_cdc_zte_rx_fixup(struct usbnet *dev, struct sk_buff *skb);

/* CDC and RNDIS support the same host-chosen packet filters for IN transfers */
#define	DEFAULT_FILTER	(USB_CDC_PACKET_TYPE_BROADCAST \
			|USB_CDC_PACKET_TYPE_ALL_MULTICAST \
			|USB_CDC_PACKET_TYPE_PROMISCUOUS \
			|USB_CDC_PACKET_TYPE_DIRECTED)


/* we record the state for each of our queued skbs */
enum skb_state {
	illegal = 0,
	tx_start, tx_done,
	rx_start, rx_done, rx_cleanup,
	unlink_start
};

struct skb_data {	/* skb->cb is one of these */
	struct urb		*urb;
	struct usbnet		*dev;
	enum skb_state		state;
	long			length;
	unsigned long		packets;
};

/* Drivers that set FLAG_MULTI_PACKET must call this in their
 * tx_fixup method before returning an skb.
 */
static inline void
usbnet_set_skb_tx_stats(struct sk_buff *skb,
			unsigned long packets, long bytes_delta)
{
	struct skb_data *entry = (struct skb_data *) skb->cb;

	entry->packets = packets;
	entry->length = bytes_delta;
}

extern int usbnet_open(struct net_device *net);
extern int usbnet_stop(struct net_device *net);
extern netdev_tx_t usbnet_start_xmit(struct sk_buff *skb,
				     struct net_device *net);
extern void usbnet_tx_timeout(struct net_device *net, unsigned int txqueue);
extern int usbnet_change_mtu(struct net_device *net, int new_mtu);
extern int usbnet_ndo_bpf(struct net_device *net, struct netdev_bpf *bpf);
extern int usbnet_xsk_wakeup(struct net_device *net, u32 queue_id, u32 flags);

extern int usbnet_get_endpoints(struct usbnet *, struct usb_interface *);
extern int usbnet_get_ethernet_addr(struct usbnet *, int);
extern void usbnet_defer_kevent(struct usbnet *, int);
extern void usbnet_skb_return(struct usbnet *, struct sk_buff *);
extern void usbnet_unlink_rx_urbs(struct usbnet *);

extern void usbnet_pause_rx(struct usbnet *);
extern void usbnet_resume_rx(struct usbnet *);
extern void usbnet_purge_paused_rxq(struct usbnet *);

extern int usbnet_get_link_ksettings_mii(struct net_device *net,
				     struct ethtool_link_ksettings *cmd);
extern int usbnet_set_link_ksettings_mii(struct net_device *net,
				     const struct ethtool_link_ksettings *cmd);
extern int usbnet_get_link_ksettings_internal(struct net_device *net,
				     struct ethtool_link_ksettings *cmd);
extern u32 usbnet_get_link(struct net_device *net);
extern u32 usbnet_get_msglevel(struct net_device *);
extern void usbnet_set_msglevel(struct net_device *, u32);
extern void usbnet_set_rx_mode(struct net_device *net);
extern void usbnet_get_drvinfo(struct net_device *, struct ethtool_drvinfo *);
extern int usbnet_nway_reset(struct net_device *net);

extern int usbnet_manage_power(struct usbnet *, int);
extern void usbnet_link_change(struct usbnet *, bool, bool);

extern int usbnet_status_start(struct usbnet *dev, gfp_t mem_flags);
extern void usbnet_status_stop(struct usbnet *dev);

extern void usbnet_update_max_qlen(struct usbnet *dev);

#endif /* __LINUX_USB_USBNET_H */
