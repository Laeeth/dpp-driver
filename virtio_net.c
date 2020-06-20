/* A network driver using virtio.
 *
 * Copyright 2007 Rusty Russell <rusty@rustcorp.com.au> IBM Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
//#define DEBUG
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_net.h>
#include <linux/bpf.h>
#include <linux/bpf_trace.h>
#include <linux/scatterlist.h>
#include <linux/if_vlan.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/average.h>
#include <linux/filter.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <net/route.h>
#include <net/xdp.h>
#include <net/net_failover.h>

void abort(void);

// D Bindings

// Macros

inline void __dbind__rcu_assign_pointer(struct bpf_prog *p, struct bpf_prog* v) {
    rcu_assign_pointer(p, v);
}

inline unsigned int __dbind__clamp_t(unsigned long len_read, uint buf_len, size_t diff) {
    return clamp_t(unsigned int, len_read, buf_len, diff);
}

inline unsigned int __dbind__smp_processor_id(void) {
    return smp_processor_id();
}

inline int __dbind__raw_smp_processor_id(void) {
    return raw_smp_processor_id();
}

inline void __dbind__print_bug(void) {
    pr_info("%s\n", "BUG, nasol, plm");
}

//create a template from both!!
inline int __dbind__virtio_cread_feature_1(struct virtio_device *vdev,
       int fbit, unsigned short *ptr) {
    return virtio_cread_feature(vdev, fbit, struct virtio_net_config, status, ptr);
}

inline int __dbind__virtio_cread_feature_2(struct virtio_device *vdev,
       int fbit, unsigned short *ptr) {
    return virtio_cread_feature(vdev, fbit, struct virtio_net_config, max_virtqueue_pairs, ptr);
}

inline void __dbind__RCU_INIT_POINTER_null(struct bpf_prog *xdp) {
    RCU_INIT_POINTER(xdp, NULL);
}

// End Macros
// inceput

inline struct failover* __dbind__net_failover_create(struct net_device *standby_dev) {
    return net_failover_create(standby_dev);
}

void __assert (const char *__assertion, const char *__file, int __line)
{
    panic("Assert failed in file %s at line %d\n. The system will halt.", __file, __line);
}

// End of D Bindings

/*static int napi_weight = NAPI_POLL_WEIGHT;*/
/*module_param(napi_weight, int, 0444);*/

static bool csum = true, gso = true;
/*static bool napi_tx;*/
module_param(csum, bool, 0444);
module_param(gso, bool, 0444);
/*module_param(napi_tx, bool, 0644);*/

/* RX packet size EWMA. The average packet size is used to determine the packet
 * buffer size when refilling RX rings. As the entire RX ring may be refilled
 * at once, the weight is chosen so that the EWMA will be insensitive to short-
 * term, transient changes in packet size.
 */
DECLARE_EWMA(pkt_len, 0, 64)
inline void __dbind__ewma_pkt_len_init(struct ewma_pkt_len *e) {
    ewma_pkt_len_init(e);
}
inline void __dbind__ewma_pkt_len_add(struct ewma_pkt_len *e, unsigned long val) {
    ewma_pkt_len_add(e, val);
}

inline void __dbind__ewma_pkt_len_read(struct ewma_pkt_len *e) {
    ewma_pkt_len_read(e);
}

#define VIRTNET_DRIVER_VERSION "1.0.0"

static const unsigned long guest_offloads[] = {
    VIRTIO_NET_F_GUEST_TSO4,
    VIRTIO_NET_F_GUEST_TSO6,
    VIRTIO_NET_F_GUEST_ECN,
    VIRTIO_NET_F_GUEST_UFO
};

struct virtnet_sq_stats {
    struct u64_stats_sync syncp;
    u64 packets;
    u64 bytes;
    u64 xdp_tx;
    u64 xdp_tx_drops;
    u64 kicks;
};

struct virtnet_rq_stats {
    struct u64_stats_sync syncp;
    u64 packets;
    u64 bytes;
    u64 drops;
    u64 xdp_packets;
    u64 xdp_tx;
    u64 xdp_redirects;
    u64 xdp_drops;
    u64 kicks;
};
/* Internal representation of a send virtqueue */
struct send_queue {
    /* Virtqueue associated with this send _queue */
    struct virtqueue *vq;

    /* TX: fragments + linear part + virtio header */
    struct scatterlist sg[MAX_SKB_FRAGS + 2];

    /* Name of the send queue: output.$index */
    char name[40];

    struct virtnet_sq_stats stats;

    struct napi_struct napi;
};

/* Internal representation of a receive virtqueue */
struct receive_queue {
    /* Virtqueue associated with this receive_queue */
    struct virtqueue *vq;

    struct napi_struct napi;

    struct bpf_prog __rcu *xdp_prog;

    struct virtnet_rq_stats stats;

    /* Chain pages by the private ptr. */
    struct page *pages;

    /* Average packet length for mergeable receive buffers. */
    struct ewma_pkt_len mrg_avg_pkt_len;

    /* Page frag for packet buffer allocation. */
    struct page_frag alloc_frag;

    /* RX: fragments + linear part + virtio header */
    struct scatterlist sg[MAX_SKB_FRAGS + 2];

    /* Min single buffer size for mergeable buffers case. */
    unsigned int min_buf_len;

    /* Name of this receive queue: input.$index */
    char name[40];

    struct xdp_rxq_info xdp_rxq;
};

/* Control VQ buffers: protected by the rtnl lock */
struct control_buf {
    struct virtio_net_ctrl_hdr hdr;
    virtio_net_ctrl_ack status;
    struct virtio_net_ctrl_mq mq;
    u8 promisc;
    u8 allmulti;
    __virtio16 vid;
    __virtio64 offloads;
};

struct virtnet_info {
    struct virtio_device *vdev;
    struct virtqueue *cvq;
    struct net_device *dev;
    struct send_queue *sq;
    struct receive_queue *rq;
    unsigned int status;

    /* Max # of queue pairs supported by the device */
    u16 max_queue_pairs;

    /* # of queue pairs currently used by the driver */
    u16 curr_queue_pairs;

    /* # of XDP queue pairs currently used by the driver */
    u16 xdp_queue_pairs;

    /* I like... big packets and I cannot lie! */
    bool big_packets;

    /* Host will merge rx buffers for big packets (shake it! shake it!) */
    bool mergeable_rx_bufs;

    /* Has control virtqueue */
    bool has_cvq;

    /* Host can handle any s/g split between our header and packet data */
    bool any_header_sg;

    /* Packet virtio header size */
    u8 hdr_len;

    /* Work struct for refilling if we run low on memory. */
    struct delayed_work refill;

    /* Work struct for config space updates */
    struct work_struct config_work;

    /* Does the affinity hint is set for virtqueues? */
    bool affinity_hint_set;

    /* CPU hotplug instances for online & dead */
    struct hlist_node node;
    struct hlist_node node_dead;

    struct control_buf *ctrl;

    /* Ethtool settings */
    u8 duplex;
    u32 speed;

    unsigned long guest_offloads;

    /* failover when STANDBY feature enabled */
    struct failover *failover;
};

struct padded_vnet_hdr {
    struct virtio_net_hdr_mrg_rxbuf hdr;
    /*
     * hdr is in a separate sg buffer, and data sg buffer shares same page
     * with this header sg. This padding makes next sg 16 byte aligned
     * after the header.
     */
    char padding[4];
};

/* Converting between virtqueue no. and kernel tx/rx queue no.
 * 0:rx0 1:tx0 2:rx1 3:tx1 ... 2N:rxN 2N+1:txN 2N+2:cvq
 */
int vq2txq(struct virtqueue *vq);

int txq2vq(int txq);

int vq2rxq(struct virtqueue *vq);

int rxq2vq(int rxq);

struct virtio_net_hdr_mrg_rxbuf *skb_vnet_hdr(struct sk_buff *skb);

/*
 * private is used to chain pages for big packets, put the whole
 * most recent used list in the beginning for reuse
 */

void skb_xmit_done(struct virtqueue *vq);

inline void *addr_skb_xmit_done(void) {
    return skb_xmit_done;
}

int virtnet_xdp_xmit(struct net_device *dev,
            int n, struct xdp_frame **frames, u32 flags);

unsigned int virtnet_get_headroom(struct virtnet_info *vi);

unsigned int get_mergeable_buf_len(struct receive_queue *rq,
                      struct ewma_pkt_len *avg_pkt_len,
                      unsigned int room);

void skb_recv_done(struct virtqueue *rvq);

inline void *addr_skb_recv_done(void) {
    return skb_recv_done;
}

void refill_work(struct work_struct *work);

inline void __dbind__INIT_DELAYED_WORK(struct delayed_work* dw) {
    INIT_DELAYED_WORK(dw, refill_work);
}

int virtnet_poll(struct napi_struct *napi, int budget);

inline void __dbind__netif_napi_add(struct net_device *dev, struct napi_struct *napi, 
        int weight) {
    netif_napi_add(dev, napi, virtnet_poll, weight);
}

int virtnet_open(struct net_device *dev);

int virtnet_poll_tx(struct napi_struct *napi, int budget);

inline void __dbind__netif_tx_napi_add(struct net_device *dev,
        struct napi_struct *napi, int weight) {
    netif_tx_napi_add(dev, napi, virtnet_poll_tx, weight);
}

netdev_tx_t start_xmit(struct sk_buff *skb, struct net_device *dev);

/*
 * Send command via the control virtqueue and check status.  Commands
 * supported by the hypervisor, as indicated by feature bits, should
 * never fail unless improperly formatted.
 */

int virtnet_set_mac_address(struct net_device *dev, void *p);

void virtnet_stats(struct net_device *dev, struct rtnl_link_stats64 *tot);

int virtnet_close(struct net_device *dev);

void virtnet_set_rx_mode(struct net_device *dev);

void __dbind__netdev_for_each_uc_addr(struct netdev_hw_addr *ha, struct net_device *dev, struct virtio_net_ctrl_mac *mac_data)
{
    int i = 0;
    netdev_for_each_uc_addr(ha, dev)
        memcpy(&mac_data->macs[i++][0], ha->addr, ETH_ALEN);
}

void __dbind__netdev_for_each_mc_addr(struct netdev_hw_addr *ha, struct net_device *dev, struct virtio_net_ctrl_mac *mac_data)
{
    int i = 0;
    netdev_for_each_mc_addr(ha, dev)
        memcpy(&mac_data->macs[i++][0], ha->addr, ETH_ALEN);
}

int virtnet_vlan_rx_add_vid(struct net_device *dev,
                   __be16 proto, u16 vid);

int virtnet_vlan_rx_kill_vid(struct net_device *dev,
                    __be16 proto, u16 vid);

int virtnet_cpu_online(unsigned int cpu, struct hlist_node *node);

int virtnet_cpu_dead(unsigned int cpu, struct hlist_node *node);

int virtnet_cpu_down_prep(unsigned int cpu, struct hlist_node *node);

static enum cpuhp_state virtionet_online;

inline enum cpuhp_state __dbind__get_virtionet_online(void) {
    return virtionet_online;
}

void virtnet_get_ringparam(struct net_device *dev, struct ethtool_ringparam *ring);

void virtnet_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info);

/* TODO: Eliminate OOO packets during switching */
int virtnet_set_channels(struct net_device *dev, struct ethtool_channels *channels);

void virtnet_get_strings(struct net_device *dev, u32 stringset, u8 *data);

int virtnet_get_sset_count(struct net_device *dev, int sset);

void virtnet_get_ethtool_stats(struct net_device *dev, struct ethtool_stats *stats, u64 *data);

void virtnet_get_channels(struct net_device *dev, struct ethtool_channels *channels);

int virtnet_set_link_ksettings(struct net_device *dev, const struct ethtool_link_ksettings *cmd);

int virtnet_get_link_ksettings(struct net_device *dev, struct ethtool_link_ksettings *cmd);

const struct ethtool_ops virtnet_ethtool_ops = {
    .get_drvinfo = virtnet_get_drvinfo,
    .get_link = ethtool_op_get_link,
    .get_ringparam = virtnet_get_ringparam,
    .get_strings = virtnet_get_strings,
    .get_sset_count = virtnet_get_sset_count,
    .get_ethtool_stats = virtnet_get_ethtool_stats,
    .set_channels = virtnet_set_channels,
    .get_channels = virtnet_get_channels,
    .get_ts_info = ethtool_op_get_ts_info,
    .get_link_ksettings = virtnet_get_link_ksettings,
    .set_link_ksettings = virtnet_set_link_ksettings,
};

inline const void* __dbind__get_ethtool_ops_addr(void) {
    return &virtnet_ethtool_ops;
}

int virtnet_xdp(struct net_device *dev, struct netdev_bpf *xdp);

int virtnet_get_phys_port_name(struct net_device *dev, char *buf, size_t len);

const struct net_device_ops virtnet_netdev = {
    .ndo_open            = virtnet_open,
    .ndo_stop   	     = virtnet_close,
    .ndo_start_xmit      = start_xmit,
    .ndo_validate_addr   = eth_validate_addr,
    .ndo_set_mac_address = virtnet_set_mac_address,
    .ndo_set_rx_mode     = virtnet_set_rx_mode,
    .ndo_get_stats64     = virtnet_stats,
    .ndo_vlan_rx_add_vid = virtnet_vlan_rx_add_vid,
    .ndo_vlan_rx_kill_vid = virtnet_vlan_rx_kill_vid,
    .ndo_bpf		= virtnet_xdp,
    .ndo_xdp_xmit		= virtnet_xdp_xmit,
    .ndo_features_check	= passthru_features_check,
    .ndo_get_phys_port_name	= virtnet_get_phys_port_name,
};

inline const void* __dbind__get_virtnet_netdev_addr(void) {
    return &virtnet_netdev;
}

void virtnet_config_changed_work(struct work_struct *work);

inline void __dbind__INIT_WORK(struct work_struct *ws) {
    INIT_WORK(ws, virtnet_config_changed_work);
}

void virtnet_config_changed(struct virtio_device *vdev);

#ifdef CONFIG_SYSFS
static ssize_t mergeable_rx_buffer_size_show(struct netdev_rx_queue *queue,
        char *buf)
{
    struct virtnet_info *vi = netdev_priv(queue->dev);
    unsigned int queue_index = get_netdev_rx_queue_index(queue);
    unsigned int headroom = virtnet_get_headroom(vi);
    unsigned int tailroom = headroom ? sizeof(struct skb_shared_info) : 0;
    struct ewma_pkt_len *avg;

    BUG_ON(queue_index >= vi->max_queue_pairs);
    avg = &vi->rq[queue_index].mrg_avg_pkt_len;
    return sprintf(buf, "%u\n",
               get_mergeable_buf_len(&vi->rq[queue_index], avg,
                       SKB_DATA_ALIGN(headroom + tailroom)));
}

static struct rx_queue_attribute mergeable_rx_buffer_size_attribute =
    __ATTR_RO(mergeable_rx_buffer_size);

static struct attribute *virtio_net_mrg_rx_attrs[] = {
    &mergeable_rx_buffer_size_attribute.attr,
    NULL
};

const struct attribute_group virtio_net_mrg_rx_group = {
    .name = "virtio_net",
    .attrs = virtio_net_mrg_rx_attrs
};
#endif

inline const struct attribute_group * __dbind__get_mrg_rx_group(void) {
    return &virtio_net_mrg_rx_group;
}

bool virtnet_fail_on_feature(struct virtio_device *vdev,
                    unsigned int fbit,
                    const char *fname, const char *dname);

#define VIRTNET_FAIL_ON(vdev, fbit, dbit)			\
    virtnet_fail_on_feature(vdev, fbit, #fbit, dbit)

int virtnet_validate(struct virtio_device *vdev);

int virtnet_probe(struct virtio_device *vdev);

void virtnet_remove(struct virtio_device *vdev);

__maybe_unused int virtnet_freeze(struct virtio_device *vdev);

__maybe_unused int virtnet_restore(struct virtio_device *vdev);

static struct virtio_device_id id_table[] = {
    { VIRTIO_ID_NET, VIRTIO_DEV_ANY_ID },
    { 0 },
};

#define VIRTNET_FEATURES \
    VIRTIO_NET_F_CSUM, VIRTIO_NET_F_GUEST_CSUM, \
    VIRTIO_NET_F_MAC, \
    VIRTIO_NET_F_HOST_TSO4, VIRTIO_NET_F_HOST_UFO, VIRTIO_NET_F_HOST_TSO6, \
    VIRTIO_NET_F_HOST_ECN, VIRTIO_NET_F_GUEST_TSO4, VIRTIO_NET_F_GUEST_TSO6, \
    VIRTIO_NET_F_GUEST_ECN, VIRTIO_NET_F_GUEST_UFO, \
    VIRTIO_NET_F_MRG_RXBUF, VIRTIO_NET_F_STATUS, VIRTIO_NET_F_CTRL_VQ, \
    VIRTIO_NET_F_CTRL_RX, VIRTIO_NET_F_CTRL_VLAN, \
    VIRTIO_NET_F_GUEST_ANNOUNCE, VIRTIO_NET_F_MQ, \
    VIRTIO_NET_F_CTRL_MAC_ADDR, \
    VIRTIO_NET_F_MTU, VIRTIO_NET_F_CTRL_GUEST_OFFLOADS, \
    VIRTIO_NET_F_SPEED_DUPLEX, VIRTIO_NET_F_STANDBY

static unsigned int features[] = {
    VIRTNET_FEATURES,
};

static unsigned int features_legacy[] = {
    VIRTNET_FEATURES,
    VIRTIO_NET_F_GSO,
    VIRTIO_F_ANY_LAYOUT,
};

static struct virtio_driver virtio_net_driver = {
    .feature_table = features,
    .feature_table_size = ARRAY_SIZE(features),
    .feature_table_legacy = features_legacy,
    .feature_table_size_legacy = ARRAY_SIZE(features_legacy),
    .driver.name =	KBUILD_MODNAME,
    .driver.owner =	THIS_MODULE,
    .id_table =	id_table,
    .validate =	virtnet_validate,
    .probe =	virtnet_probe,
    .remove =	virtnet_remove,
    .config_changed = virtnet_config_changed,
#ifdef CONFIG_PM_SLEEP
    .freeze =	virtnet_freeze,
    .restore =	virtnet_restore,
#endif
};

static __init int virtio_net_driver_init(void)
{
    int ret;
    /*pr_info("sizeof struct_mac: %ld\n", sizeof(struct virtio_net_ctrl_mac));*/
    /*msleep(10000);*/
    /*pr_info("stat_desc sizeof: %d", sizeof(struct virtnet_stat_desc));*/
    /*pr_info("rq vect sizeof: %d", sizeof(virtnet_rq_stats_desc));*/
    /*pr_info("sq vect sizeof: %d", sizeof(virtnet_sq_stats_desc));*/
    /*pr_info("sizeof netdev_bpf: %ld\n", sizeof(struct netdev_bpf));*/
    /*pr_info("offset vqs:%ld\n", offsetof(struct virtio_pci_device, vqs));*/

    /*pr_info("sizeof ethtool_link_settings: %ld\n", sizeof(struct ethtool_link_settings));*/
    /*pr_info("sizeof ethtool_link_ksettings: %ld\n", sizeof(struct ethtool_link_ksettings));*/
    /*pr_info("kbuild name: %s\n", KBUILD_MODNAME);*/
    /*pr_info("virtio_net_ctrl_hdr: %ld\n", sizeof(struct virtio_net_ctrl_hdr));*/
    /*pr_info("HZ: %d\n", HZ);*/
    /*pr_info("Sizeof napi_struct: %ld\n", sizeof(struct napi_struct));*/
    /*pr_info("Sizeof virtio_device: %ld\n", sizeof(struct virtio_device));*/
    /*pr_info("Sizeof bool: %ld\n", sizeof(bool));*/
    /*pr_info("Sizeof spinlock_t: %ld\n", sizeof(spinlock_t));*/
    /*pr_info("Sizeof device: %ld\n", sizeof(struct device));*/
    /*pr_info("Sizeof virtio-device_id: %ld\n", sizeof(struct virtio_device_id));*/
    /*pr_info("Sizeof mutex: %ld\n", sizeof(struct mutex));*/
    /*pr_info("Sizeof kobject: %ld\n", sizeof(struct kobject));*/
    /*pr_info("Sizeof dev_links_info : %ld\n", sizeof(struct dev_links_info));*/
    /*pr_info("Sizeof dev_pm_info : %ld\n", sizeof(struct dev_pm_info));*/
    /*pr_info("Sizeof dev_archdata : %ld\n", sizeof(struct dev_archdata));*/
    /*pr_info("Sizeof klist_node : %ld\n", sizeof(struct klist_node));*/
    /*pr_info("Sizeof net_device : %ld\n", sizeof(struct net_device));*/
    /*pr_info("Sizeof netdev_tc_txq:%ld\n", sizeof(struct netdev_tc_txq));*/
    /*pr_info("Sizeof possible_net_t:%ld\n", sizeof(possible_net_t));*/
    /*pr_info("Sizeof netdev_hw_addr_list:%ld\n", sizeof(struct netdev_hw_addr_list));*/
    /*pr_info("Sizeof atomic_t:%ld\n", sizeof(atomic_t));*/
    /*pr_info("Sizeof atomic_long_t:%ld\n", sizeof(atomic_long_t));*/
    /*pr_info("Sizeof timer_list:%ld\n", sizeof(struct timer_list));*/
    /*pr_info("Sizeof net_device_stats:%ld\n", sizeof(struct net_device_stats));*/
    /*pr_info("Sizeof sock_filter:%ld\n", sizeof(struct sock_filter));*/
    /*pr_info("Sizeof bpf_insn:%ld\n", sizeof(struct bpf_insn));*/
    /*pr_info("Sizeof sk_buff:%ld\n", sizeof(struct sk_buff));*/
    /*pr_info("offset sk_buff.csum:%ld\n", offsetof(struct sk_buff, csum));*/
    /*pr_info("offset sk_buff.queue_mapping:%ld\n", offsetof(struct sk_buff, queue_mapping));*/
    /*pr_info("offset sk_buff.headers_start:%ld\n", offsetof(struct sk_buff, headers_start));*/
    /*pr_info("offset sk_buff.cb:%ld\n", offsetof(struct sk_buff, cb));*/
    /*pr_info("offset sk_buff.tcp_tsorted_anchor:%ld\n", offsetof(struct sk_buff, tcp_tsorted_anchor));*/
    /*pr_info("offset sk_buff.len:%ld\n", offsetof(struct sk_buff, len));*/
    /*pr_info("offset sk_buff.data_len:%ld\n", offsetof(struct sk_buff, data_len));*/
    /*pr_info("offset sk_buff.mac_len:%ld\n", offsetof(struct sk_buff, mac_len));*/
    /*pr_info("offset sk_buff.hdr_len:%ld\n", offsetof(struct sk_buff, hdr_len));*/
    /*pr_info("offset sk_buff.priority:%ld\n", offsetof(struct sk_buff, priority));*/
    /*pr_info("offset sk_buff.mac_header:%ld\n", offsetof(struct sk_buff, mac_header));*/
    /*pr_info("offset sk_buff.mac_header:%ld\n", offsetof(struct sk_buff, mac_header));*/
    /*pr_info("Sizeof bpf_prog:%ld\n", sizeof(struct bpf_prog));*/
    /*pr_info("offset bpf_prog.tag:%ld\n", offsetof(struct bpf_prog, tag));*/
    /*pr_info("offset bpf_prog.bpf_func:%ld\n", offsetof(struct bpf_prog, bpf_func));*/
    /*pr_info("offset bpf_prog.insns:%ld\n", offsetof(struct bpf_prog, insns));*/
    /*pr_info("offset bpf_prog.insnsi:%ld\n", offsetof(struct bpf_prog, insnsi));*/
    /*pr_info("Sizeof send_queue:%ld\n", sizeof(struct send_queue));*/
    /*pr_info("offset send_queue.name:%ld\n", offsetof(struct send_queue, name));*/
    /*pr_info("offset send_queue.napi:%ld\n", offsetof(struct send_queue, napi));*/
    /*pr_info("Sizeof page_struct:%ld\n", sizeof(struct page));*/
    /*pr_info("offset page_struct.private:%ld\n", offsetof(struct page, private));*/
    /*pr_info("offset page_struct.pmd_huge_pte:%ld\n", offsetof(struct page, pmd_huge_pte));*/
    /*pr_info("Sizeof receive_queue:%ld\n", sizeof(struct receive_queue));*/
    /*pr_info("offset receive_queue.vq:%ld\n", offsetof(struct receive_queue, vq));*/
    /*pr_info("offset receive_queue.napi:%ld\n", offsetof(struct receive_queue, napi));*/
    /*pr_info("offset receive_queue.xdp_prog:%ld\n", offsetof(struct receive_queue, xdp_prog));*/
    /*pr_info("offset receive_queue.stats:%ld\n", offsetof(struct receive_queue, stats));*/
    /*pr_info("offset receive_queue.pages:%ld\n", offsetof(struct receive_queue, pages));*/
    /*pr_info("offset receive_queue.mrg_avg_pkt_len:%ld\n", offsetof(struct receive_queue, mrg_avg_pkt_len));*/
    /*pr_info("offset receive_queue.alloc_frag:%ld\n", offsetof(struct receive_queue, alloc_frag));*/
    /*pr_info("offset receive_queue.sg:%ld\n", offsetof(struct receive_queue, sg));*/
    /*pr_info("offset receive_queue.xdp_rxq:%ld\n", offsetof(struct receive_queue, xdp_rxq));*/
    /*pr_info("Sizeof scatterlist:%ld\n", sizeof(struct scatterlist));*/
    /*pr_info("Sizeof control_buf:%ld\n", sizeof(struct control_buf));*/
    /*pr_info("Sizeof virtnet_info:%ld\n", sizeof(struct virtnet_info));*/
    /*pr_info("Sizeof virtnet_sq_stats:%ld\n", sizeof(struct virtnet_sq_stats));*/
    /*pr_info("Sizeof virtnet_rq_stats:%ld\n", sizeof(struct virtnet_rq_stats));*/
    /*pr_info("offset virtnet_sq_stats.syncp:%ld\n", offsetof(struct virtnet_sq_stats, syncp));*/
    /*pr_info("offset virtnet_rq_stats.syncp:%ld\n", offsetof(struct virtnet_rq_stats, syncp));*/
    /*pr_info("offset virnet_info.rq:%ld\n", offsetof(struct virtnet_info, rq));*/
    /*pr_info("offset virnet_info.refill:%ld\n", offsetof(struct virtnet_info, refill));*/
    /*pr_info("offset virnet_info.failover:%ld\n", offsetof(struct virtnet_info, failover));*/

    ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "virtio/net:online",
                      virtnet_cpu_online,
                      virtnet_cpu_down_prep);
    if (ret < 0)
        goto out;
    virtionet_online = ret;
    ret = cpuhp_setup_state_multi(CPUHP_VIRT_NET_DEAD, "virtio/net:dead",
                      NULL, virtnet_cpu_dead);
    if (ret)
        goto err_dead;

    ret = register_virtio_driver(&virtio_net_driver);
    if (ret)
        goto err_virtio;
    return 0;
err_virtio:
    cpuhp_remove_multi_state(CPUHP_VIRT_NET_DEAD);
err_dead:
    cpuhp_remove_multi_state(virtionet_online);
out:
    return ret;
}
module_init(virtio_net_driver_init);

static __exit void virtio_net_driver_exit(void)
{
    unregister_virtio_driver(&virtio_net_driver);
    cpuhp_remove_multi_state(CPUHP_VIRT_NET_DEAD);
    cpuhp_remove_multi_state(virtionet_online);
}
module_exit(virtio_net_driver_exit);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio network driver");
MODULE_LICENSE("GPL");
