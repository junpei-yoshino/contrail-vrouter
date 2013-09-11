/*
 * vr_host_interface.c -- linux specific handling of vrouter interfaces
 *
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */
#include <linux/init.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_vlan.h>
#include <linux/if_arp.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39))
#include <linux/if_bridge.h>
#endif

#include <net/rtnetlink.h>
#include "vr_compat.h"
#include "vr_packet.h"
#include "vr_interface.h"
#include "vrouter.h"
#include "vr_linux.h"
#include "vr_os.h"

extern int vhost_init(void);
extern void vhost_exit(void);
extern void vhost_if_add(struct vr_interface *);
extern void vhost_if_del(struct net_device *dev);

static int vr_napi_poll(struct napi_struct *, int);
static rx_handler_result_t pkt_gro_dev_rx_handler(struct sk_buff **);
static int linux_xmit_segments(struct vr_interface *, struct sk_buff *,
        unsigned short);

/*
 * Structure to store information required to be sent across CPU cores
 * when RPS is performed on the physical interface (vr_perfr3 is 1).
 */
typedef struct vr_rps_ {
    unsigned int vif_idx;
    unsigned short vif_rid;
} vr_rps_t;

/*
 *  pkt_gro_dev - this is a device used to do receive offload on packets
 *  destined over a TAP interface to a VM.
 */
static struct net_device *pkt_gro_dev = NULL;

/*
 * pkt_gro_dev_ops - netdevice operations on GRO packet device. Currently,
 * no operations are needed, but an empty structure is required to
 * register the device.
 *
 */
static struct net_device_ops pkt_gro_dev_ops;

/*
 * pkt_rps_dev - this is a device used to perform RPS on packets coming in
 * on a physical interface.
 */
static struct net_device *pkt_rps_dev = NULL;

/*
 * pkt_rps_dev_ops - netdevice operations on RPS packet device. Currently,
 * no operations are needed, but an empty structure is required to
 * register the device.
 *
 */
static struct net_device_ops pkt_rps_dev_ops;

/*
 * vr_skb_set_rxhash - set the rxhash on a skb if the kernel version
 * allows it.
 */
void
vr_skb_set_rxhash(struct sk_buff *skb, __u32 val)
{
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,32))
#ifndef CONFIG_XEN
    skb->rxhash = val;
#endif
#else
    skb->rxhash = val;
#endif
}

/*
 * vr_skb_get_rxhash - get the rxhash on a skb if the kernel version
 * allows it.
 */
__u32
vr_skb_get_rxhash(struct sk_buff *skb)
{
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,32))
#ifndef CONFIG_XEN
    return skb->rxhash;
#else
    return 0;
#endif
#else
    return skb->rxhash;
#endif
}

static int
linux_if_rx(struct vr_interface *vif, struct vr_packet *pkt)
{
    int rc;
    struct net_device *dev = (struct net_device *)vif->vif_os;
    struct sk_buff *skb = vp_os_packet(pkt);
    struct vr_ip *ip;
    unsigned short network_off, transport_off, cksum_off = 0;

    skb->data = pkt->vp_head + pkt->vp_data;
    skb->len = pkt_len(pkt);
    skb_set_tail_pointer(skb, pkt_head_len(pkt));

    if (!dev) {
        kfree_skb(skb);
        goto exit_rx;
    }

    (void)__sync_fetch_and_add(&dev->stats.rx_bytes, skb->len);
    (void)__sync_fetch_and_add(&dev->stats.rx_packets, 1);

    /* this is only needed for mirroring */
    if ((pkt->vp_flags & VP_FLAG_FROM_DP) &&
            (pkt->vp_flags & VP_FLAG_CSUM_PARTIAL)) {
    	network_off = pkt_get_network_header_off(pkt);
    	ip = (struct vr_ip *)(pkt_data_at_offset(pkt, network_off));
    	transport_off = network_off + (ip->ip_hl * 4);

        if (ip->ip_proto == VR_IP_PROTO_TCP)
            cksum_off = offsetof(struct vr_tcp, tcp_csum);
        else if (ip->ip_proto == VR_IP_PROTO_UDP)
            cksum_off = offsetof(struct vr_udp, udp_csum);
 
        if (cksum_off)
            *(unsigned short *)
                (pkt_data_at_offset(pkt, transport_off + cksum_off))
                = 0;
    }

    skb->protocol = eth_type_trans(skb, dev);
    skb->pkt_type = PACKET_HOST;
    rc = netif_rx(skb);

exit_rx:
    return RX_HANDLER_CONSUMED;
}

static long
linux_inet_fragment(struct vr_interface *vif, struct sk_buff *skb,
        unsigned short type)
{
    struct iphdr *ip = ip_hdr(skb);
    unsigned int ip_hlen = ip->ihl * 4;
    bool fragmented = ntohs(ip->frag_off) & IP_MF ? true : false;
    unsigned int offset = (ntohs(ip->frag_off) & IP_OFFSET) << 3;
    unsigned short ip_id = ntohs(ip->id);
    unsigned int payload_size = skb->len - skb->mac_len - ip_hlen;
    unsigned int frag_size = skb->dev->mtu - skb->mac_len - ip_hlen;
    unsigned int num_frags, last_frag_len;
    struct sk_buff *segs;
    netdev_features_t features;

    features = netif_skb_features(skb);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39))
    features &= (~(NETIF_F_ALL_TSO | NETIF_F_UFO | NETIF_F_GSO));
#else
    features &= ~(NETIF_F_TSO | NETIF_F_UFO | NETIF_F_GSO);
#endif
    /*
     * frag size has to be a multiple of 8 and last fragment has
     * to be >= 64 bytes (approx)
     */
    frag_size &= ~7U;
    num_frags = payload_size / frag_size;
    last_frag_len = payload_size % frag_size;
    if (last_frag_len && last_frag_len < 64) {
        frag_size -= ((64 - last_frag_len) / num_frags);
        /*
         * the previous division could have produced 0. to cover
         * that case make some change to frag_size
         */
        frag_size -= 1;
        /* by doing this, we will get 8 bytes in the worst case */
        frag_size &= ~7U;
    }

    skb_shinfo(skb)->gso_size = 0;

    /*
     * for packets that need checksum help, checksum has to be
     * calculated here, since post fragmentation, checksum of
     * individual fragments will be wrong
     *
     * FIXME - we may not need to calculate checksum of inner packet if
     * outer encap is UDP (as the UDP checksum will be calculated by
     * the NIC and this should cover the inner packet too).
     */
    if (skb->ip_summed == CHECKSUM_PARTIAL) {
        if (skb_checksum_help(skb)) {
            kfree_skb(skb);
            return 0;
        }
    }

    skb_shinfo(skb)->gso_size = frag_size;

    /* pull till transport header */
    skb_pull(skb, skb->mac_len + ip_hlen);
    segs = skb_segment(skb, features);
    if (IS_ERR(segs))
        return PTR_ERR(segs);

    kfree_skb(skb);
    skb = segs;
    do {
        ip = ip_hdr(skb);
        ip->id = htons(ip_id);
        ip->frag_off = htons(offset >> 3);
        if (skb->next != NULL || fragmented)
            ip->frag_off |= htons(IP_MF);
        offset += (skb->len - skb->mac_len - ip->ihl * 4);
        ip->tot_len = htons(skb->len - skb->mac_len);
        ip->check = 0;
        ip->check = ip_fast_csum(skb_network_header(skb), ip->ihl);
    } while ((skb = skb->next));


    return linux_xmit_segments(vif, segs, type);
}

static int
linux_xmit(struct vr_interface *vif, struct sk_buff *skb,
        unsigned short type)
{
    unsigned short proto = ntohs(skb->protocol);

    if (vif->vif_type != VIF_TYPE_PHYSICAL ||
            skb->len <= skb->dev->mtu + skb->dev->hard_header_len)
        return dev_queue_xmit(skb);

    if (proto == ETH_P_IP)
        return linux_inet_fragment(vif, skb, type);

    kfree_skb(skb);
    return -ENOMEM;
}

static int
linux_xmit_segment(struct vr_interface *vif, struct sk_buff *seg,
        unsigned short type)
{
    int err = -ENOMEM;
    struct vr_ip *iph;
    unsigned short iphlen;
    struct udphdr *udph;

    /* we will do tunnel header updates after the fragmentation */
    if (seg->len > seg->dev->mtu + seg->dev->hard_header_len
            || type != VP_TYPE_IPOIP)
        return linux_xmit(vif, seg, type);

    /* FIXME - this assumes MAC header length is ETH_HLEN */
    if (!pskb_may_pull(seg, ETH_HLEN + sizeof(struct vr_ip))) {
        goto exit_xmit;
    }

    iph = (struct vr_ip *)(seg->data + ETH_HLEN);
    iphlen = (iph->ip_hl << 2);
    iph->ip_len = htons(seg->len - ETH_HLEN);
    iph->ip_id = htons(vr_generate_unique_ip_id());

    if (!pskb_may_pull(seg, ETH_HLEN + iphlen)) {
        goto exit_xmit;
    }

    if (iph->ip_proto == VR_IP_PROTO_UDP) {
        skb_set_network_header(seg, ETH_HLEN);
        iph->ip_csum = 0;

        if (!pskb_may_pull(seg, ETH_HLEN + iphlen +
                    sizeof(struct udphdr))) {
            goto exit_xmit;
        }

        skb_set_transport_header(seg,  iphlen + ETH_HLEN);
        if (!skb_partial_csum_set(seg, skb_transport_offset(seg),
                    offsetof(struct udphdr, check))) {
            goto exit_xmit;
        }

        udph = (struct udphdr *) skb_transport_header(seg);
        udph->len = htons(seg->len - skb_transport_offset(seg));
        iph->ip_csum = ip_fast_csum(iph, iph->ip_hl);
        udph->check = ~csum_tcpudp_magic(iph->ip_saddr, iph->ip_daddr,
                                         htons(udph->len),
                                         IPPROTO_UDP, 0);
    } else if (iph->ip_proto == VR_IP_PROTO_GRE) {
        iph->ip_csum = 0;
        iph->ip_csum = ip_fast_csum(iph, iph->ip_hl);
    }

    return linux_xmit(vif, seg, type);

exit_xmit:
    kfree_skb(seg);
    return err;
}

static int
linux_xmit_segments(struct vr_interface *vif, struct sk_buff *segs,
        unsigned short type)
{
    int err;
    struct sk_buff *nskb = NULL;

    do {
        nskb = segs->next;
        segs->next = NULL;
        if ((err = linux_xmit_segment(vif, segs, type)))
            break;
        segs = nskb;
    } while (segs);

    segs = nskb;
    while (segs) {
        nskb = segs->next;
        segs->next = NULL;
        kfree_skb(segs);
        segs = nskb;
    }

    return err;
}

/*
 * linux_gso_xmit - perform segmentation of the inner packet in software
 * and send each segment out the wire after fixing the outer header.
 */
static void
linux_gso_xmit(struct vr_interface *vif, struct sk_buff *skb,
        unsigned short type)
{
    netdev_features_t features;
    struct sk_buff *segs;
    unsigned short seg_size = skb_shinfo(skb)->gso_size;
    struct iphdr *ip = ip_hdr(skb);
    struct tcphdr *th;
    struct net_device *ndev = (struct net_device *)vif->vif_os;

    features = netif_skb_features(skb);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39))
    features &= (~(NETIF_F_ALL_TSO | NETIF_F_UFO | NETIF_F_GSO));
#else
    features &= (~(NETIF_F_TSO | NETIF_F_UFO | NETIF_F_GSO));
#endif
    seg_size += skb->mac_len + skb_network_header_len(skb);
    /*
     * We are trying to find whether the total size of the packet will
     * overshoot the mtu. Above, we have accounted for the tunnel headers,
     * the inner ip header, and the segment size. However, there is a
     * subtle difference in deciding whether transport header is part of
     * GSO size or not.
     *
     * For TCP, segment size (gso size) is the ip data length - tcp header
     * length (since each segment goes with tcp header), while for udp, there
     * are only fragments (and no segments) and the segment size (fragment
     * size) is the ip data length adjusted to mtu (since udp header goes
     * only with the first fragment). Hence the following condition
     */
    if (ip->protocol == IPPROTO_TCP) {
        th = tcp_hdr(skb);
        seg_size += (th->doff * 4);
    }

    /*
     * avoid fragmentation after segmentation. please note that this
     * can potentially lead to duplicate IP id problem, since we are
     * messing with protocol stack's idea of the number of segments for
     * a given GSO size to accomodate our tunnel headers,  one that was
     * deemed OK for the time being.
     */ 
    if (seg_size > ndev->mtu + ndev->hard_header_len) {
        skb_shinfo(skb)->gso_size -= (seg_size - ndev->mtu -
                ndev->hard_header_len);
        if (ip->protocol == IPPROTO_UDP)
            skb_shinfo(skb)->gso_size &= ~7;
    }

    segs = skb_gso_segment(skb, features);
    kfree_skb(skb);
    if ((IS_ERR(segs)) || (segs == NULL)) {
        return;
    }

    linux_xmit_segments(vif, segs, type);

    return;
}

#ifdef CONFIG_RPS

/*
 * linux_get_rxq - get a receive queue for the packet on an interface that
 * has RPS enabled. The receive queue is picked such that it is different
 * from the current CPU core and the previous CPU core that handled the
 * packet (if the previous core is specified). The receive queue has a 1-1
 * mapping to the receiving CPU core (i.e. queue 1 corresponds to CPU core 0,
 * queue 2 to CPU core 1 and so on). The CPU core is chosen such that it is
 * on the same NUMA node as the  current core (to minimize memory access
 * latency across NUMA nodes), except that hyper-threads of the current
 * and previous core are excluded as choices for the next CPU to process the
 * packet.
 */
static void
linux_get_rxq(struct sk_buff *skb, u16 *rxq, unsigned int curr_cpu,
              unsigned int prev_cpu)
{
    unsigned int next_cpu;
    int numa_node = cpu_to_node(curr_cpu);
    const struct cpumask *node_cpumask = cpumask_of_node(numa_node);
    struct cpumask noht_cpumask;
    unsigned int num_cpus, cpu, count = 0;
    __u32 rxhash;

    /*
     * We are running in softirq context, so CPUs can't be offlined
     * underneath us. So, it is safe to use the NUMA node CPU bitmaps.
     * Clear the bits corresponding to the current core and its hyperthreads
     * in the node CPU mask.
     */
    cpumask_andnot(&noht_cpumask, node_cpumask, cpu_sibling_mask(curr_cpu));

    /*
     * If the previous CPU is specified, clear the bits corresponding to
     * that core and its hyperthreads in the CPU mask.
     */
    if (prev_cpu && (prev_cpu <= nr_cpu_ids)) {
        cpumask_andnot(&noht_cpumask, &noht_cpumask,
                       cpu_sibling_mask(prev_cpu-1));
    }

    num_cpus = cpumask_weight(&noht_cpumask);

    if (num_cpus) {
        rxhash = skb_get_rxhash(skb);
        next_cpu = ((u64)rxhash * num_cpus) >> 32;

        /*
         * next_cpu is between 0 and (num_cpus - 1). Find the CPU corresponding
         * to next_cpu in the CPU bitmask.
         */
        for_each_cpu(cpu, &noht_cpumask) {
            if (count == next_cpu) {
                break;
            }

            count++;
        }

        if (cpu >= nr_cpu_ids) {
            /*
             * Shouldn't happen
             */
            *rxq = curr_cpu;
        } else {
            *rxq = cpu;
        }
    } else {
        /*
         * Not enough CPU cores available in this NUMA node. Continue
         * processing the packet on the same CPU core.
         */
        *rxq = curr_cpu;
    }

    return;
}   

#endif

/*
 * linux_enqueue_pkt_for_gro - enqueue packet on a list of skbs and schedule a
 * NAPI event on the NAPI structure of the vif.
 *
 * FIXME - the length of this queue needs to be bounded.
 *
 */
void
linux_enqueue_pkt_for_gro(struct sk_buff *skb, struct vr_interface *vif)
{
#ifdef CONFIG_RPS
    u16 rxq;
    unsigned int curr_cpu = 0;

    /*
     * vr_perfr1 only takes effect if vr_perfr3 is not set. Also, if we are
     * coming here after RPS (skb->dev is pkt_rps_dev), vr_perfr1 is a no-op
     */
    if (vr_perfr1 && (!vr_perfr3) && (skb->dev != pkt_rps_dev)) {
        curr_cpu = vr_get_cpu();
        if (vr_perfq1) {
            rxq = vr_perfq1;
        } else {
            linux_get_rxq(skb, &rxq, curr_cpu, 0);
        }

        skb_record_rx_queue(skb, rxq);
        /*
         * Store current CPU in rxhash of skb
         */
        vr_skb_set_rxhash(skb, curr_cpu);
        skb->dev = pkt_rps_dev;

        /*
         * Clear the vr_rps_t vif_idx field in the skb->cb. This is to handle
         * the corner case of vr_perfr3 being enabled after a packet has
         * been scheduled for RPS with vr_perfr1 set, but before the
         * earlier RPS has completed. After RPS completes, linux_rx_handler()
         * will drop the packet as vif_idx is 0 (which corresponds to pkt0).
         */
        ((vr_rps_t *)skb->cb)->vif_idx = 0;

        netif_receive_skb(skb);

        return;
    }

    if (vr_perfr2) {
        if (vr_perfq2) {
            rxq = vr_perfq2;
        } else {
            /*
             * If RPS happened earlier (perfr1 or perfr3 is set),
             * prev_cpu was already been set in skb->rxhash.
             */
            linux_get_rxq(skb, &rxq, vr_get_cpu(),
                          (vr_perfr1 || vr_perfr3) ? 
                              vr_skb_get_rxhash(skb)+1 : 0);
        }

        skb_record_rx_queue(skb, rxq);
    } else {
        skb_set_queue_mapping(skb, 0);
    }

#endif /* CONFIG_RPS */

    skb->dev = pkt_gro_dev;

    skb_queue_tail(&vif->vr_skb_inputq, skb);
    napi_schedule(&vif->vr_napi);

    return;
}

#if 0
static void __skb_dump_info(const char *prefix, const struct sk_buff *skb,
	struct vr_interface *vif)
{
#ifdef CONFIG_XEN
    int i, nr = skb_shinfo(skb)->nr_frags;
#endif
    struct ethhdr *ethh = eth_hdr(skb);
    struct iphdr *iph = NULL;
    struct tcphdr *tcph = NULL;

    printk("vif info: type=%d id=%d os_id=%d\n",
            vif->vif_type, vif->vif_idx, vif->vif_os_idx);

    printk(KERN_CRIT "%s: len is %#x (data:%#x mac:%#x) truesize %#x\n", prefix,
            skb->len, skb->data_len, skb->mac_len, skb->truesize);

    printk(KERN_CRIT "%s: linear:%s\n", prefix,
            skb_is_nonlinear(skb) ? "No" : "Yes");
    printk(KERN_CRIT "%s: data %p head %p tail %p end %p\n", prefix,
            skb->data, skb->head, skb->tail, skb->end);
    printk(KERN_CRIT "%s: flags are local_df:%d cloned:%d ip_summed:%d"
            "nohdr:%d\n", prefix, skb->local_df, skb->cloned,
            skb->ip_summed, skb->nohdr);
    printk(KERN_CRIT "%s: nfctinfo:%d pkt_type:%d fclone:%d ipvs_property:%d\n",
            prefix, skb->nfctinfo, skb->pkt_type,
            skb->nohdr, skb->ipvs_property);
    printk(KERN_CRIT "%s: shared info %p ref %#x\n", prefix,
            skb_shinfo(skb), atomic_read(&skb_shinfo(skb)->dataref));
    printk(KERN_CRIT "%s: frag_list %p\n", prefix,
            skb_shinfo(skb)->frag_list);

    if (ethh) {
        printk(KERN_CRIT "%s: eth: (%p) src:%pM dest:%pM proto %u\n",
                prefix, ethh, ethh->h_source, ethh->h_dest, ntohs(ethh->h_proto));
        if (ethh->h_proto == __constant_htons(ETH_P_IP))
            iph = ip_hdr(skb);
        } else
            printk(KERN_CRIT "%s: eth: header not present\n", prefix);

    if (iph) {
        printk(KERN_CRIT "%s: ip: (%p) saddr "NIPQUAD_FMT" daddr "NIPQUAD_FMT"\
             protocol %d frag_off %d\n", prefix, iph, NIPQUAD(iph->saddr),
             NIPQUAD(iph->daddr), iph->protocol, iph->frag_off);

        if (iph->protocol == IPPROTO_TCP)
            tcph = tcp_hdr(skb);
    } else
        printk(KERN_CRIT "%s: ip: header not present\n", prefix);

    if (tcph) {
        printk(KERN_CRIT "%s: tcp: (%p) source %d dest %d seq %u ack %u\n",
                prefix, tcph, ntohs(tcph->source), ntohs(tcph->dest),
                ntohl(tcph->seq), ntohl(tcph->ack_seq));
    } else
        printk(KERN_CRIT "%s: tcp: header not present\n", prefix);

#ifdef CONFIG_XEN
    printk(KERN_CRIT "%s: nr_frags %d\n", prefix, nr);
    for(i=0; i<nr; i++) {
        skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
        unsigned long pfn = page_to_pfn(frag->page);
        unsigned long mfn = pfn_to_mfn(pfn);
        printk(KERN_CRIT "%s: %d/%d page:%p count:%d offset:%#x size:%#x \
                virt:%p pfn:%#lx mfn:%#lx%s flags:%lx%s%s)\n",
                prefix, i + 1, nr, frag->page,
                atomic_read(&frag->page->_count),
                frag->page_offset, frag->size,
                phys_to_virt(page_to_pseudophys(frag->page)), pfn, mfn,
                phys_to_machine_mapping_valid(pfn) ? "" : "(BAD)",
                frag->page->flags,
                PageForeign(frag->page) ? " FOREIGN" : "",
                PageBlkback(frag->page) ? " BLKBACK" : "");
    }
#endif
}
#endif

static int
linux_if_tx(struct vr_interface *vif, struct vr_packet *pkt)
{
    struct net_device *dev = (struct net_device *)vif->vif_os;
    struct sk_buff *skb = vp_os_packet(pkt);
    struct vr_ip *ip;
    unsigned short network_off, transport_off, cksum_off;

    skb->data = pkt_data(pkt);
    skb->len = pkt_len(pkt);
    skb_set_tail_pointer(skb, pkt_head_len(pkt));

    skb->dev = dev;
    if (!dev) {
        kfree_skb(skb);
        return 0;
    }

    if ((pkt->vp_flags & VP_FLAG_GRO) &&
            (vif->vif_type == VIF_TYPE_VIRTUAL)) {

        skb_push(skb, VR_MPLS_HDR_LEN);
        skb_reset_mac_header(skb);

        if (!skb_pull(skb, pkt->vp_network_h - (skb->data - skb->head))) {
            kfree_skb(skb);
            return 0;
        }

        skb_reset_network_header(skb);

        linux_enqueue_pkt_for_gro(skb, vif);
        return 0;
    }

    skb_reset_mac_header(skb);

    /*
     * Set the network header and trasport header of skb only if the type is
     * IP (tunnel or non tunnel). This is required for those packets where
     * a new buffer is added at the head
     */
    if (pkt->vp_type == VP_TYPE_IPOIP || pkt->vp_type == VP_TYPE_IP) {
        network_off = pkt_get_inner_network_header_off(pkt);
        ip = (struct vr_ip *)(pkt_data_at_offset(pkt, network_off));
        transport_off = network_off + (ip->ip_hl * 4);

        skb_set_network_header(skb, (network_off - skb_headroom(skb)));
        skb_reset_mac_len(skb);
        skb_set_transport_header(skb, (transport_off - skb_headroom(skb)));

        /* 
         * Manipulate partial checksum fields. 
         * There are cases like mirroring where the UDP headers are newly added
         * and skb needs to be filled with proper offsets. The vr_packet's fields
         * are latest values and they need to be reflected in skb
         */
        if (pkt->vp_flags & VP_FLAG_CSUM_PARTIAL) {
            cksum_off = skb->csum_offset;
            if (ip->ip_proto == VR_IP_PROTO_TCP)
                cksum_off = offsetof(struct vr_tcp, tcp_csum);
            else if (ip->ip_proto == VR_IP_PROTO_UDP)
                cksum_off = offsetof(struct vr_udp, udp_csum);

            skb_partial_csum_set(skb, (transport_off - skb_headroom(skb)), cksum_off);
        } else {
            skb->ip_summed = CHECKSUM_NONE;
            skb->csum = 0;
        }

        /*
         * Invoke segmentation only incase of both vr_packet and skb having gso
         */
        if ((pkt->vp_flags & VP_FLAG_GSO) && skb_is_gso(skb) &&
                (vif->vif_type == VIF_TYPE_PHYSICAL)) {
            linux_gso_xmit(vif, skb, pkt->vp_type);
            return 0;
        }
    }


    linux_xmit_segment(vif, skb, pkt->vp_type); 

    return 0;
}

inline struct vr_packet *
linux_get_packet(struct sk_buff *skb, struct vr_interface *vif)
{
    struct vr_packet *pkt;
    unsigned int length;

    pkt = (struct vr_packet *)skb->cb;
    pkt->vp_cpu = vr_get_cpu();
    pkt->vp_head = skb->head;

    length = skb_tail_pointer(skb) - skb->head;
    if (length >= (1 << (sizeof(pkt->vp_tail) * 8)))
        goto drop;
    pkt->vp_tail = length;

    length = skb->data - skb->head;
    if (length >= (1 << (sizeof(pkt->vp_data) * 8)))
        goto drop;
    pkt->vp_data = length;

    length = skb_end_pointer(skb) - skb->head;
    if (length >= (1 << (sizeof(pkt->vp_end) * 8)))
        goto drop;
    pkt->vp_end = length;

    pkt->vp_len = skb_headlen(skb);
    pkt->vp_if = vif;
    pkt->vp_network_h = pkt->vp_inner_network_h = 0;
    pkt->vp_nh = NULL;
    pkt->vp_flags = 0;
    if (skb->ip_summed == CHECKSUM_PARTIAL)
        pkt->vp_flags |= VP_FLAG_CSUM_PARTIAL;

    pkt->vp_type = VP_TYPE_NULL;

    return pkt;

drop:
    vr_pfree(pkt, VP_DROP_INVALID_PACKET);
    return NULL;
}

int
linux_to_vr(struct vr_interface *vif, struct sk_buff *skb)
{
    struct vr_packet *pkt;

    if ((skb = skb_share_check(skb, GFP_ATOMIC)) == NULL)
        return 0;

    pkt = linux_get_packet(skb, vif);
    if (!pkt)
        return 0;

    vif->vif_rx(vif, pkt, VLAN_ID_INVALID);

    return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39))
rx_handler_result_t
linux_rx_handler(struct sk_buff **pskb)
{
    int ret;
    unsigned short vlan_id = VLAN_ID_INVALID;
    struct sk_buff *skb = *pskb;
    struct vr_packet *pkt;
    struct net_device *dev = skb->dev;
    struct vr_interface *vif;
    unsigned int curr_cpu;
    u16 rxq;
    int rpsdev = 0;
    struct vrouter *router;

    /*
     * If we did RPS immediately after the packet was received from the
     * physical interface (vr_perfr3 is set), we are now running on a
     * new core. Extract the vif information that was saved in the skb
     * on the previous core.
     */
    if (skb->dev == pkt_rps_dev) {
        router = vrouter_get(((vr_rps_t *)skb->cb)->vif_rid);
        if (router == NULL) {
            goto error;
        }
            
        vif = __vrouter_get_interface(router,
                  ((vr_rps_t *)skb->cb)->vif_idx);
        if (vif && (vif->vif_type == VIF_TYPE_PHYSICAL) && vif->vif_os) {
            dev = (struct net_device *) vif->vif_os;
            rpsdev = 1;
        } else {
            goto error;
        }
    }

    vif = rcu_dereference(dev->rx_handler_data);

    if ((skb = skb_share_check(skb, GFP_ATOMIC)) == NULL)
        return RX_HANDLER_PASS;

#ifdef CONFIG_RPS
    /*
     * Send the packet to another CPU core if vr_perfr3 is set. The new
     * CPU core is chosen based on a hash of the outer header. This only needs
     * to be done for packets arriving on a physical interface. Also, we
     * only need to do this if RPS hasn't already happened.
     */
    if (vr_perfr3 && (!rpsdev) && (vif->vif_type == VIF_TYPE_PHYSICAL)) {
        curr_cpu = vr_get_cpu();
        if (vr_perfq3) {
            rxq = vr_perfq3;
        } else {
            linux_get_rxq(skb, &rxq, curr_cpu, 0);
        }

        skb_record_rx_queue(skb, rxq);
        vr_skb_set_rxhash(skb, curr_cpu);
        skb->dev = pkt_rps_dev;

        /*
         * Store vif information in skb for later retrieval
         */
        ((vr_rps_t *)skb->cb)->vif_idx = vif->vif_idx;
        ((vr_rps_t *)skb->cb)->vif_rid = vif->vif_rid;

        netif_receive_skb(skb);
        return RX_HANDLER_CONSUMED;
    }
#endif

    skb_push(skb, ETH_HLEN);

    pkt = linux_get_packet(skb, vif);
    if (!pkt)
        return RX_HANDLER_CONSUMED;

    if (skb->vlan_tci & VLAN_TAG_PRESENT) {
        vlan_id = skb->vlan_tci & 0xFFF;
        skb->vlan_tci = 0; 
    }

    ret = vif->vif_rx(vif, pkt, vlan_id);
    if (!ret)
        ret = RX_HANDLER_CONSUMED;

    return ret;

error:

     pkt = (struct vr_packet *)skb->cb;
     vr_pfree(pkt, VP_DROP_MISC);

     return RX_HANDLER_CONSUMED;

}
#else
/*
 * vr_interface_bridge_hook
 *
 * Intercept packets received on virtual interfaces in kernel versions that
 * do not support the netdev_rx_handler_register API. This makes the vrouter
 * module incompatible with the bridge module.
 */
static struct sk_buff *
vr_interface_bridge_hook(struct net_bridge_port *port, struct sk_buff *skb)
{
    unsigned short vlan_id = VLAN_ID_INVALID;
    struct vr_interface *vif;
    struct vr_packet *pkt;
    struct vlan_hdr *vhdr;

    /*
     * LACP packets should go to the protocol handler. Hence do not
     * claim those packets. This action is not needed in 3.x kernels
     * because packets are claimed by the protocol handler from the
     * component interface itself by means of netdev_rx_handler
     */
    if (skb->protocol == __be16_to_cpu(ETH_P_SLOW))
        return skb;

    if (skb->dev == pkt_gro_dev) {
        pkt_gro_dev_rx_handler(&skb);
        return NULL;
    }

    vif = (struct vr_interface *)port;

#if 0
    if(vrouter_dbg) {
        __skb_dump_info("vr_intf_br_hk:", skb, vif);
    }
#endif

    if ((skb = skb_share_check(skb, GFP_ATOMIC)) == NULL)
        return skb;

    if (skb->protocol == htons(ETH_P_8021Q)) {
        vhdr = (struct vlan_hdr *)skb->data;
        vlan_id = ntohs(vhdr->h_vlan_TCI) & VLAN_VID_MASK;
    }

    skb_push(skb, ETH_HLEN);

    pkt = linux_get_packet(skb, vif);
    if (!pkt)
        return NULL;

    vif->vif_rx(vif, pkt, vlan_id);
    return NULL;
}
#endif
/*
 * both add tap and del tap can come from multiple contexts. one is
 * obviously when interface is deleted from vrouter on explicit request
 * from agent. other is when the physical interface underlying the
 * vrouter interface dies, in which case we will be notified and
 * have to take corrective actions. hence, it is vital that we check
 * whether 'rtnl' was indeed locked before trying to acquire the lock
 * and unlock iff we locked it in the first place.
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36))
static int
linux_if_del_tap(struct vr_interface *vif)
{
    struct net_device *dev = (struct net_device *)vif->vif_os;
    bool i_locked = false;

    if (!dev)
        return -EINVAL;

    if (!rtnl_is_locked()) {
        i_locked = true;
        rtnl_lock();
    }

    if (rcu_dereference(dev->rx_handler) == linux_rx_handler)
        netdev_rx_handler_unregister(dev);

    if (i_locked)
        rtnl_unlock();

    return 0;
}
#else
static int
linux_if_del_tap(struct vr_interface *vif)
{
    struct net_device *dev = (struct net_device *) vif->vif_os;

    rcu_assign_pointer(dev->br_port, NULL);

    return 0;
}
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39))
static int
linux_if_add_tap(struct vr_interface *vif)
{
    int ret;
    bool i_locked = false;
    struct net_device *dev = (struct net_device *)vif->vif_os;

    if (!dev)
        return -EINVAL;

    if (!rtnl_is_locked()) {
        i_locked = true;
        rtnl_lock();
    }

    ret = netdev_rx_handler_register(dev, linux_rx_handler, (void *)vif);

    if (i_locked)
        rtnl_unlock();

    return ret;
}
#else
static int
linux_if_add_tap(struct vr_interface *vif)
{
    struct net_device *dev = (struct net_device *) vif->vif_os;

    rcu_assign_pointer(dev->br_port, (void *) vif);

    return 0;
}
#endif

static int
linux_if_get_settings(struct vr_interface *vif,
        struct vr_interface_settings *settings)
{
    struct net_device *dev = (struct net_device *)vif->vif_os;
    int ret = -EINVAL;

    if (vif->vif_type != VIF_TYPE_PHYSICAL || !dev)
        return ret;

    rtnl_lock();

    if (netif_running(dev)) {
        struct ethtool_cmd cmd;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0))
        /* As per lxr, this API was introduced in 3.2.0 */ 
        if (!(ret = __ethtool_get_settings(dev, &cmd))) {
            settings->vis_speed = ethtool_cmd_speed(&cmd);
#else
        cmd.cmd = ETHTOOL_GSET;
        if  (!(ret = dev_ethtool_get_settings(dev, &cmd))) {
            settings->vis_speed = cmd.speed;
#endif
            settings->vis_duplex = cmd.duplex;
        }
    }

    rtnl_unlock();

    return ret;
}


static int
linux_if_del(struct vr_interface *vif)
{
    if (vif->vif_type == VIF_TYPE_HOST ||
            vif->vif_type == VIF_TYPE_XEN_LL_HOST)
        vhost_if_del((struct net_device *)vif->vif_os);

    if (vif->vif_type == VIF_TYPE_VIRTUAL) {
        napi_disable(&vif->vr_napi);
        netif_napi_del(&vif->vr_napi);
        skb_queue_purge(&vif->vr_skb_inputq);
    }

    if (vif->vif_os)
        dev_put((struct net_device *)vif->vif_os);

    vif->vif_os = NULL;
    vif->vif_os_idx = 0;

    return 0;
}

static int
linux_if_add(struct vr_interface *vif)
{
    struct net_device *dev;

    if (vif->vif_os_idx) {
        dev = dev_get_by_index(&init_net, vif->vif_os_idx);
        if (!dev)
            return -ENODEV;
        vif->vif_os = (void *)dev;
    }

    if (vif->vif_type == VIF_TYPE_HOST ||
            vif->vif_type == VIF_TYPE_XEN_LL_HOST)
        vhost_if_add(vif);

    if (vif->vif_type == VIF_TYPE_VIRTUAL) {
        skb_queue_head_init(&vif->vr_skb_inputq);
        netif_napi_add(pkt_gro_dev, &vif->vr_napi, vr_napi_poll, 64);
        napi_enable(&vif->vr_napi);
    }

    return 0;
}

/*
 * linux_pkt_dev_free_helper - free the packet device
 */
static void
linux_pkt_dev_free_helper(struct net_device **dev)
{
    if (*dev == NULL) {
        return;
    }

    unregister_netdev(*dev);
    free_netdev(*dev);
    *dev = NULL;

    return;
}

/*
 * linux_pkt_dev_free - free the packet device used for GRO/RPS
 */
static void
linux_pkt_dev_free(void)
{
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39))
    rcu_assign_pointer(pkt_gro_dev->br_port, NULL);
#endif
    linux_pkt_dev_free_helper(&pkt_gro_dev);
    linux_pkt_dev_free_helper(&pkt_rps_dev);
 
    return;
}

/*
 * pkt_gro_dev_setup - fill in the relevant fields of the GRO packet device
 */
static void
pkt_gro_dev_setup(struct net_device *dev)
{
    /*
     * The hard header length is used by the GRO code to compare the
     * MAC header of the incoming packet with the MAC header of packets
     * undergoing GRO at the moment. In our case, each vif will have a
     * unique MPLS label associated with it, so we can use the MPLS header
     * as the MAC header to combine packets destined for the same vif.
     */
    dev->hard_header_len = VR_MPLS_HDR_LEN;

    dev->type = ARPHRD_VOID;
    dev->netdev_ops = &pkt_gro_dev_ops;
    dev->features |= NETIF_F_GRO;
    dev->mtu = 65535;

    return;
}

/*
 * pkt_rps_dev_setup - fill in the relevant fields of the RPS packet device
 */
static void
pkt_rps_dev_setup(struct net_device *dev)
{
    dev->hard_header_len = ETH_HLEN;

    dev->type = ARPHRD_VOID;
    dev->netdev_ops = &pkt_rps_dev_ops;
    dev->mtu = 65535;

    return;
}

/*
 * linux_pkt_dev_init - initialize the packet device used for GRO. Returns 
 * pointer to packet device if no errors, NULL otherwise.
 */
static struct net_device *
linux_pkt_dev_init(char *name, void (*setup)(struct net_device *),
                   rx_handler_result_t (*handler)(struct sk_buff **))
{
    int err = 0, rtnl_was_locked;
    struct net_device *pdev = NULL;

    if (!(pdev = alloc_netdev_mqs(0, name, setup,
                                  1, num_possible_cpus()))) {
        vr_module_error(-ENOMEM, __FUNCTION__, __LINE__, 0);
        return NULL;
    }

    rtnl_was_locked = rtnl_is_locked();
    if (!rtnl_was_locked) {
        rtnl_lock();
    }

    if ((err = register_netdevice(pdev))) {
        vr_module_error(err, __FUNCTION__, __LINE__, 0);
    } else {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,39))
        if ((err = netdev_rx_handler_register(pdev,
                                              handler, NULL))) {
            vr_module_error(err, __FUNCTION__, __LINE__, 0);
            unregister_netdev(pdev);
        }
#endif
    }

    if (!rtnl_was_locked) {
        rtnl_unlock();
    }

    if (err) {
        free_netdev(pdev);
        return NULL;
    }

    return pdev;
} 

static rx_handler_result_t 
pkt_gro_dev_rx_handler(struct sk_buff **pskb)
{
    unsigned int label;
    unsigned short vrf;
    struct vr_nexthop *nh;
    struct vr_interface *vif;
    struct sk_buff *skb = *pskb;
    struct vr_packet *pkt;
    struct vrouter *router = vrouter_get(0);   // FIXME

    label = ntohl(*((unsigned int *) skb_mac_header(skb)));
    label >>= VR_MPLS_LABEL_SHIFT;

    if (label >= router->vr_max_labels) {
        kfree_skb(skb);
        return RX_HANDLER_CONSUMED;
    }

    nh = router->vr_ilm[label];
    if (!nh) {
        kfree_skb(skb);
        return RX_HANDLER_CONSUMED;
    }

    vif = nh->nh_dev;
    if ((vif == NULL) || (vif->vif_type != VIF_TYPE_VIRTUAL)) {
        kfree_skb(skb);
        return RX_HANDLER_CONSUMED;
    }

    vrf = nh->nh_dev->vif_vrf;

    pkt = linux_get_packet(skb, vif);
    if (!pkt)
        return RX_HANDLER_CONSUMED;

    pkt_set_network_header(pkt, pkt->vp_data);
    pkt_set_inner_network_header(pkt, pkt->vp_data);
    /*
     * All flow handling has been done prior to GRO
     */
    pkt->vp_flags |= VP_FLAG_FLOW_SET;

    nh_output(vrf, pkt, nh, NULL);
    return RX_HANDLER_CONSUMED;
}

/*
 * pkt_rps_dev_rx_handler - receive a packet after RPS
 */
static rx_handler_result_t
pkt_rps_dev_rx_handler(struct sk_buff **pskb)
{
    struct sk_buff *skb = *pskb;
    struct vr_packet *pkt;
    unsigned int label;
    struct vr_nexthop *nh;
    struct vr_interface *vif;
    struct vrouter *router = vrouter_get(0);   // FIXME

    if (vr_perfr3) {
#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,32))
        vr_interface_bridge_hook(NULL, *pskb);
        return RX_HANDLER_CONSUMED;
#else
        return linux_rx_handler(&skb);
#endif
    }

    pkt = (struct vr_packet *)skb->cb;

    /*
     * If RPS was scheduled earlier because of vr_perfr1 being set, the
     * vif_idx in skb->cb should be 0. If it is non-zero, RPS was scheduled
     * because of vr_perfr3 being set earlier (and now vr_perfr3 has been
     * cleared). Drop the packet in this corner case.
     */
    if (((vr_rps_t *)skb->cb)->vif_idx) {
        vr_pfree(pkt, VP_DROP_MISC);

        return RX_HANDLER_CONSUMED;
    }

    label = ntohl(*((unsigned int *) skb_mac_header(skb)));
    label >>= VR_MPLS_LABEL_SHIFT;

    if (label >= router->vr_max_labels) {
        vr_pfree(pkt, VP_DROP_INVALID_LABEL);

        return RX_HANDLER_CONSUMED;
    }

    nh = router->vr_ilm[label];
    if (!nh) {
        vr_pfree(pkt, VP_DROP_INVALID_NH);
        return RX_HANDLER_CONSUMED;
    }

    vif = nh->nh_dev;
    if ((vif == NULL) || (vif->vif_type != VIF_TYPE_VIRTUAL)) {
        vr_pfree(pkt, VP_DROP_MISC);
        return RX_HANDLER_CONSUMED;
    }

    linux_enqueue_pkt_for_gro(skb, vif);

    return RX_HANDLER_CONSUMED;
}

/*
 * vif_from_napi - given a NAPI structure, return the corresponding vif
 */
static struct vr_interface *
vif_from_napi(struct napi_struct *napi)
{
    int offset;
    struct vr_interface *vif;

    offset = offsetof(struct vr_interface, vr_napi);
    vif = (struct vr_interface *) (((char *)napi) - offset);

    return vif;
}

/*
 * vr_napi_poll - NAPI poll routine to receive packets and perform
 * GRO.
 */
static int
vr_napi_poll(struct napi_struct *napi, int budget)
{
    struct sk_buff *skb;
    struct vr_interface *vif;
    int quota = 0;

    vif = vif_from_napi(napi);

    while ((skb = skb_dequeue(&vif->vr_skb_inputq))) {
        vr_skb_set_rxhash(skb, 0);

        napi_gro_receive(napi, skb);

        quota++;
        if (quota == budget) {
            break;
        }
    }

    if (quota != budget) {
        napi_complete(napi);

        return 0;
    }

    return budget;
}

struct vr_host_interface_ops vr_linux_interface_ops = {
    .hif_add            =       linux_if_add,
    .hif_del            =       linux_if_del,
    .hif_add_tap        =       linux_if_add_tap,
    .hif_del_tap        =       linux_if_del_tap,
    .hif_tx             =       linux_if_tx,
    .hif_rx             =       linux_if_rx,
    .hif_get_settings   =       linux_if_get_settings,
};

static int
linux_if_notifier(struct notifier_block * __unused,
        unsigned long event, void *arg)
{
    struct net_device *dev = (struct net_device *)arg;
    /* for now, get router id 0 */
    struct vrouter *router = vrouter_get(0);
    struct vr_interface *agent_if;

    if (!router)
        return NOTIFY_DONE;

    agent_if = router->vr_agent_if;
    if (!agent_if)
        return NOTIFY_DONE;

    if ((event == NETDEV_UNREGISTER) && (dev ==
            (struct net_device *)agent_if->vif_os)) {
        vif_delete(agent_if);
        return NOTIFY_OK;
    }

    return NOTIFY_DONE;
}


static struct notifier_block host_if_nb = {
    .notifier_call      =       linux_if_notifier,
};


void
vr_host_interface_exit(void)
{
    vhost_exit();
    unregister_netdevice_notifier(&host_if_nb);
    if (pkt_gro_dev) {
        linux_pkt_dev_free();
    }

    return;
}

struct vr_host_interface_ops *
vr_host_interface_init(void)
{
    int ret;

    if (pkt_gro_dev == NULL) {
        pkt_gro_dev = linux_pkt_dev_init("pkt1", &pkt_gro_dev_setup,
                                         &pkt_gro_dev_rx_handler);
        if (pkt_gro_dev == NULL) {
            return NULL;
        }
    }

    if (pkt_rps_dev == NULL) {
        pkt_rps_dev = linux_pkt_dev_init("pkt2", &pkt_rps_dev_setup,
                                        &pkt_rps_dev_rx_handler);
        if (pkt_rps_dev == NULL) {
            linux_pkt_dev_free();

            return NULL;
        }
    }

    ret = register_netdevice_notifier(&host_if_nb);
    if (ret) {
        vr_module_error(ret, __FUNCTION__, __LINE__, 0);
        linux_pkt_dev_free();

        return NULL;
    }  

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39))
    rcu_assign_pointer(pkt_gro_dev->br_port, (void *) pkt_gro_dev);
    br_handle_frame_hook = vr_interface_bridge_hook;
#endif

    vhost_init();

    return &vr_linux_interface_ops;
}
