/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2015 Intel Corporation
 */

#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_ip.h>
// #include <pthread.h>
#include <unistd.h>

#include <rte_common.h>

#define CLOCK_MONOTONIC 1
// #define PKT_TX_IPV4          (1ULL << 55)
// #define PKT_TX_IP_CKSUM      (1ULL << 54)

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define MAX_FLOWS 8
#define MAX_WIN_SIZE 10


/* optional Lock*/
// #define WIN_LOCK
#ifdef WIN_LOCK
    #define INIT(l) if (pthread_mutex_init(&l, NULL) != 0) printf("\n mutex init has failed\n")
    #define LOCK(l) pthread_mutex_lock(&l)
    #define UNLOCK(l) pthread_mutex_unlock(&l)
#else
    #define INIT(l) {}
    #define LOCK(l) {}
    #define UNLOCK(l) {}
#endif



#define SET(x,y) x = x | y

int NUM_PING = 100;

/* LAB1 define slide window*/
struct tx_window { // TODO add lock
    int head; // seq of the first packet in the window [3,4,5,6|7,8] - 3
    int sent; // how many packet has been sent [3,4,5,6|7,8] - 6
    // int size; // last time ack indicated win size [3,4,5,6|7,8] - 6
    int avail; // max avail to sent packet
    pthread_mutex_t lock;
};

/* Define the mempool globally */
struct rte_mempool *mbuf_pool = NULL;
static struct rte_ether_addr my_eth;

/* static timer */
static uint64_t st[20];
static uint64_t rt[20];

static size_t message_size = 1000;
static uint32_t seconds = 1;

struct tx_window *window_list = NULL;

int flow_size = 10000;
int packet_len = 1000;
int flow_num = 1;


static uint64_t raw_time(void) {
    struct timespec tstart={0,0};
    clock_gettime(CLOCK_MONOTONIC, &tstart);
    uint64_t t = (uint64_t)(tstart.tv_sec*1.0e9 + tstart.tv_nsec);
    return t;

}

static uint64_t time_now(uint64_t offset) {
    return raw_time() - offset;
}

uint32_t
checksum(unsigned char *buf, uint32_t nbytes, uint32_t sum)
{
	unsigned int	 i;

	/* Checksum all the pairs of bytes first. */
	for (i = 0; i < (nbytes & ~1U); i += 2) {
		sum += (uint16_t)ntohs(*((uint16_t *)(buf + i)));
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}

	if (i < nbytes) {
		sum += buf[i] << 8;
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}

	return sum;
}

uint32_t
wrapsum(uint32_t sum)
{
	sum = ~sum & 0xFFFF;
	return htons(sum);
}

static int parse_packet(struct sockaddr_in *src,
                        struct sockaddr_in *dst,
                        int *ack,
                        int *win,
                        // void **payload,
                        // size_t *payload_len,
                        struct rte_mbuf *pkt)
{
    // packet layout order is (from outside -> in):
    // ether_hdr
    // ipv4_hdr
    // udp_hdr
    // client timestamp
    uint8_t *p = rte_pktmbuf_mtod(pkt, uint8_t *);
    size_t header = 0;

    // check the ethernet header
    struct rte_ether_hdr * const eth_hdr = (struct rte_ether_hdr *)(p);
    p += sizeof(*eth_hdr);
    header += sizeof(*eth_hdr);
    uint16_t eth_type = ntohs(eth_hdr->ether_type);
    struct rte_ether_addr mac_addr = {};

    rte_eth_macaddr_get(1, &mac_addr);
    if (!rte_is_same_ether_addr(&mac_addr, &eth_hdr->dst_addr)) {
        printf("Bad MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
            eth_hdr->dst_addr.addr_bytes[0], eth_hdr->dst_addr.addr_bytes[1],
			eth_hdr->dst_addr.addr_bytes[2], eth_hdr->dst_addr.addr_bytes[3],
			eth_hdr->dst_addr.addr_bytes[4], eth_hdr->dst_addr.addr_bytes[5]);
        return 0;
    }
    if (RTE_ETHER_TYPE_IPV4 != eth_type) {
        printf("Bad ether type\n");
        return 0;
    }

    // check the IP header
    struct rte_ipv4_hdr *const ip_hdr = (struct rte_ipv4_hdr *)(p);
    p += sizeof(*ip_hdr);
    header += sizeof(*ip_hdr);

    // In network byte order.
    in_addr_t ipv4_src_addr = ip_hdr->src_addr;
    in_addr_t ipv4_dst_addr = ip_hdr->dst_addr;

    if (IPPROTO_IP != ip_hdr->next_proto_id) {
        printf("Bad next proto_id\n");
        return 0;
    }
    
    src->sin_addr.s_addr = ipv4_src_addr;
    dst->sin_addr.s_addr = ipv4_dst_addr;
    
    // check udp header
    // struct rte_udp_hdr * const udp_hdr = (struct rte_udp_hdr *)(p);
    struct rte_tcp_hdr * const tcp_hdr = (struct rte_tcp_hdr *)(p);
    p += sizeof(*tcp_hdr);
    header += sizeof(*tcp_hdr);

    // In network byte order.
    in_port_t tcp_src_port = tcp_hdr->src_port;
    in_port_t tcp_dst_port = tcp_hdr->dst_port;
    int ret = 0;
	
    for (int i = 1; i < MAX_FLOWS + 1; i++)
        if (tcp_hdr->dst_port == rte_cpu_to_be_16(5000 + i)) {
            ret = i;
            break;
        }

    src->sin_port = tcp_src_port;
    dst->sin_port = tcp_dst_port;
    
    src->sin_family = AF_INET;
    dst->sin_family = AF_INET;
    
    // *payload_len = pkt->pkt_len - header;
    // *payload = (void *)p;

    *ack = (int) tcp_hdr->recv_ack;
    *win = (int) tcp_hdr->rx_win;
    return ret;

}
/* basicfwd.c: Basic DPDK skeleton forwarding example. */

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */

/* Main functional part of port initialization. 8< */
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf;
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

    printf("port avail id: %u\n", port);

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	memset(&port_conf, 0, sizeof(struct rte_eth_conf));

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0)
	{
		printf("Error during getting device (port %u) info: %s\n",
			   port, strerror(-retval));
		return retval;
	}

	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++)
	{
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
										rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++)
	{
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
										rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	/* Starting Ethernet port. 8< */
	retval = rte_eth_dev_start(port);
	/* >8 End of starting of ethernet port. */
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	retval = rte_eth_macaddr_get(port, &my_eth);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
		   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
		   port, RTE_ETHER_ADDR_BYTES(&my_eth));

	/* Enable RX in promiscuous mode for the Ethernet device. */
	retval = rte_eth_promiscuous_enable(port);
	/* End of setting RX port in promiscuous mode. */
	if (retval != 0)
		return retval;

	return 0;
}
/* >8 End of main functional part of port initialization. */

/* >8 End Basic forwarding application lcore. */

static int
init_window(size_t flow_num){
    window_list = (struct tx_window*) malloc(sizeof(struct tx_window) * (flow_num));
    if (window_list == NULL) {
        printf("fail to create tx window list.\n");
        return 1;
    }
    for (int i = 0; i<flow_num ; i++){ 
        window_list[i].head = 0;
        window_list[i].sent = -1;
        // window_list[i].size = MAX_WIN_SIZE; // since no hand shake, we dont know the inital rwnd, just max it
        window_list[i].avail = MAX_WIN_SIZE - 1;
        INIT(window_list[i].lock);
    }
    return 0;
}

static bool
check_window(size_t flow_id){
    return window_list[flow_id].avail > window_list[flow_id].sent;
}

static void
slide_window_onair(size_t flow_id){
    LOCK(window_list[flow_id].lock);
    window_list[flow_id].sent += 1;
    UNLOCK(window_list[flow_id].lock);
}

static void
slide_window_ack(size_t flow_id, uint16_t ack, uint16_t new_size){
    printf("Receive acks of #%d in flow #%d\n", ack, flow_id);
    if (ack < window_list[flow_id].head) {
        printf("already acked %u\n", ack);
        return;
    }
    if (ack > window_list[flow_id].sent) {
        printf("get ack about not sent packet(%d/%d) for flow#%d.\n",
            ack, window_list[flow_id].sent, flow_id);
        return;
    }

    LOCK(window_list[flow_id].lock);
    window_list[flow_id].head = ack + 1;
    window_list[flow_id].avail = ack + new_size;
    if (window_list[flow_id].sent > window_list[flow_id].avail) 
        printf("the window shrinks too much\n");

    UNLOCK(window_list[flow_id].lock);
}

static uint64_t receive_once();
static int
lcore_main()
{
    struct rte_mbuf *pkt;
    // char *buf_ptr;
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    // struct rte_udp_hdr *udp_hdr;
    struct rte_tcp_hdr *tcp_hdr;

    // Specify the dst mac address here: 
    // struct rte_ether_addr dst = {{0x14,0x58,0xD0,0x58,0x2F,0x32}}; // eno1
    struct rte_ether_addr dst = {{0x14,0x58,0xD0,0x58,0x2F,0x33}}; // eno1d1

	struct sliding_hdr *sld_h_ack;
    // uint64_t reqs = 0;
    // uint64_t cycle_wait = intersend_time * rte_get_timer_hz() / (1e9);
    
    // TODO: add in scaffolding for timing/printing out quick statistics
    // int outstanding[flow_num];
    // uint16_t seq[flow_num];
    size_t flow_id = 0;
    // for(size_t i = 0; i < flow_num; i++)
    // {
    //     // outstanding[i] = 0;
    //     seq[i] = 0;
    // }  // flow[i] : 500i -> 500i

    while (window_list[flow_id].sent < NUM_PING-1) {
        if (!check_window(flow_id)) { // skip this flow sending when its slidewindow is full
            flow_id = (flow_id+1) % flow_num;
            receive_once();
            continue;
        }
        // send a packet
        pkt = rte_pktmbuf_alloc(mbuf_pool);
        if (pkt == NULL) {
            printf("Error allocating tx mbuf\n");
            return -EINVAL;
        }
        size_t header_size = 0;

        uint8_t *ptr = rte_pktmbuf_mtod(pkt, uint8_t *);
        /* add in an ethernet header */
        eth_hdr = (struct rte_ether_hdr *)ptr;
        
        rte_ether_addr_copy(&my_eth, &eth_hdr->src_addr);
        rte_ether_addr_copy(&dst, &eth_hdr->dst_addr);
        eth_hdr->ether_type = rte_be_to_cpu_16(RTE_ETHER_TYPE_IPV4);
        ptr += sizeof(*eth_hdr);
        header_size += sizeof(*eth_hdr);

        /* add in ipv4 header*/
        ipv4_hdr = (struct rte_ipv4_hdr *)ptr;
        ipv4_hdr->version_ihl = 0x45;
        ipv4_hdr->type_of_service = 0x0;
        ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + message_size);
        ipv4_hdr->packet_id = rte_cpu_to_be_16(1);
        ipv4_hdr->fragment_offset = 0;
        ipv4_hdr->time_to_live = 64;
        ipv4_hdr->next_proto_id = IPPROTO_IP;
        ipv4_hdr->src_addr = rte_cpu_to_be_32("127.0.0.1");
        ipv4_hdr->dst_addr = rte_cpu_to_be_32("127.0.0.1");

        uint32_t ipv4_checksum = wrapsum(checksum((unsigned char *)ipv4_hdr, sizeof(struct rte_ipv4_hdr), 0));
        // printf("Checksum is %u\n", (unsigned)ipv4_checksum);
        ipv4_hdr->hdr_checksum = rte_cpu_to_be_32(ipv4_checksum);
        header_size += sizeof(*ipv4_hdr);
        ptr += sizeof(*ipv4_hdr);

        // /* add in UDP hdr*/
        // udp_hdr = (struct rte_udp_hdr *)ptr;
        // uint16_t srcp = 5001 + flow_id;
        // uint16_t dstp = 5001 + flow_id;
        // udp_hdr->src_port = rte_cpu_to_be_16(srcp);
        // udp_hdr->dst_port = rte_cpu_to_be_16(dstp);
        // udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) + packet_len);

        // uint16_t udp_cksum =  rte_ipv4_udptcp_cksum(ipv4_hdr, (void *)udp_hdr);

        // // printf("Udp checksum is %u\n", (unsigned)udp_cksum);
        // udp_hdr->dgram_cksum = rte_cpu_to_be_16(udp_cksum);
        // ptr += sizeof(*udp_hdr);
        // header_size += sizeof(*udp_hdr);

        // LAB1 add in TCP hdr
        tcp_hdr = (struct rte_tcp_hdr *)ptr;
        uint16_t srcp = 5001 + flow_id;
        uint16_t dstp = 5001 + flow_id;
        tcp_hdr->src_port = rte_cpu_to_be_16(srcp);
        tcp_hdr->dst_port = rte_cpu_to_be_16(dstp);
        tcp_hdr->sent_seq = window_list[flow_id].sent + 1;               // not use a byte based but only use a 1000bytes based
        // ignore rev_ack, client dont receive anything
        if (tcp_hdr->sent_seq == NUM_PING - 1)
            SET(tcp_hdr->tcp_flags, RTE_TCP_FIN_FLAG);  // last packet ends a TCP flow, farewell is ignored
        // ignore offset, i.e. header size, it is not used 
        // ignore rx_win, client dont receive anything
        uint16_t tcp_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, (void *)tcp_hdr);

        tcp_hdr->cksum = rte_cpu_to_be_16(tcp_cksum);
        ptr += sizeof(*tcp_hdr);
        header_size += sizeof(*tcp_hdr);

        /* set the payload */
        memset(ptr, 'a', packet_len);

        pkt->l2_len = RTE_ETHER_HDR_LEN;
        pkt->l3_len = sizeof(struct rte_ipv4_hdr);
        // pkt->ol_flags = PKT_TX_IP_CKSUM | PKT_TX_IPV4;
        pkt->data_len = header_size + packet_len;
        pkt->pkt_len = header_size + packet_len; // since no segmentation
        pkt->nb_segs = 1;
        int pkts_sent = 0;

        unsigned char *pkt_buffer = rte_pktmbuf_mtod(pkt, unsigned char *);
        if (check_window(flow_id)) {
            pkts_sent = rte_eth_tx_burst(1, 0, &pkt, 1);
            if(pkts_sent == 1)
            {
                // outstanding[flow_id] ++;
                st[tcp_hdr->sent_seq] = raw_time();
                slide_window_onair(flow_id); //slide the window according to its seq
            }
        }
        
        // uint64_t last_sent = rte_get_timer_cycles();
        // printf("Sent packet at %u, %d is outstanding, intersend is %u\n", (unsigned)last_sent, outstanding, (unsigned)intersend_time);
        receive_once();
        rte_pktmbuf_free(pkt);
        flow_id = (flow_id+1) % flow_num;
    }
    // printf("Sent %"PRIu64" packets.\n", reqs);
    // dump_latencies(&latency_dist);
    // return 0;

}

static inline void
window_status(){
    // printf("\n");
    // for (int i=0; i<flow_num; i++){
    //     printf("flow #%d:", i);
    //     for (int j=0; j<window_list[i].head; j++) printf("*");
    //     for (int j=window_list[i].head; j<=window_list[i].sent; j++) printf("o");
    //     for (int j=window_list[i].sent+1; j<=window_list[i].avail; j++) printf("-");
    //     printf("\n");
    // }
    // printf("\n");
}


static void
receive_once() {
    uint16_t nb_rx;
    struct rte_mbuf *r_pkts[BURST_SIZE];
    uint64_t ret;
    // LAB1: receiving packets should be implemented in another thread
    /* now poll on receiving packets */

    nb_rx = 0;
    nb_rx = rte_eth_rx_burst(1, 0, r_pkts, BURST_SIZE);
    if (nb_rx == 0) {
        // printf("nothing reveived.\n");
        return;
    }

    for (int i = 0; i < nb_rx; i++) {
        struct sockaddr_in src, dst;
        // void *payload = NULL;
        // size_t payload_length = 0;
        int ack_seq;
        int window;
        int index = parse_packet(&src, &dst, &ack_seq, &window, r_pkts[i]);
        int flow_id = index - 1;
        if (index != 0) {
            slide_window_ack(flow_id, ack_seq, window);  // slide and resize the window according to ack ???ack: ack+window???
                                        // resize by the window in the ack, not a fix number
            rt[ack_seq] = raw_time();
        }
        rte_pktmbuf_free(r_pkts[i]);
    }
    window_status();
}

static void
lcore_main_rev(__rte_unused void *arg)
{
    uint16_t nb_rx;
    struct rte_mbuf *r_pkts[BURST_SIZE];
    // LAB1: receiving packets should be implemented in another thread
    /* now poll on receiving packets */
    for (;;) {
        window_status();
        nb_rx = 0;
        for (;;) {
            nb_rx = rte_eth_rx_burst(1, 0, r_pkts, BURST_SIZE);
            if (nb_rx == 0) {
                printf("nothing reveived.\n");
                sleep(1);
                continue;
            }
        }

        printf("Receive %u acks\n", (unsigned)nb_rx);
        for (int i = 0; i < nb_rx; i++) {
            struct sockaddr_in src, dst;
            // void *payload = NULL;
            // size_t payload_length = 0;
            int ack_seq;
            int window;
            int index = parse_packet(&src, &dst, &ack_seq, &window, r_pkts[i]);
            int flow_id = index - 1;
            if (index != 0) 
                slide_window_ack(flow_id, ack_seq, window);  // slide and resize the window according to ack ???ack: ack+window???
                                            // resize by the window in the ack, not a fix number
            rte_pktmbuf_free(r_pkts[i]);

        }

        for (int i=0; i<flow_num; i++) {
            if (window_list[i].head < NUM_PING)
                goto keep;
        }
        break;
        keep: {}
    }
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */

int main(int argc, char *argv[])
{

	unsigned nb_ports;
	uint16_t portid;

    if (argc == 3) {
        flow_num = (int) atoi(argv[1]);
        flow_size =  (int) atoi(argv[2]);
    } else {
        printf( "usage: ./lab1-client <flow_num> <flow_size>\n");
        return 1;
    }

    NUM_PING = 1 + (flow_size-1) / packet_len; // ceiling round instead of floor round 

	/* Initializion the Environment Abstraction Layer (EAL). 8< */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
	/* >8 End of initialization the Environment Abstraction Layer (EAL). */

	argc -= ret;
	argv += ret;

    nb_ports = rte_eth_dev_count_avail();
	/* Allocates mempool to hold the mbufs. 8< */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
										MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	/* >8 End of allocating mempool to hold mbuf. */

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initializing all ports. 8< */
	RTE_ETH_FOREACH_DEV(portid) 
	if (portid == 1 && port_init(portid, mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n",
				 portid);
	/* >8 End of initializing all ports. */

    init_window(flow_num);
    // printf("i am core %u\n", rte_lcore_id);
    // standalone thread for rev
    unsigned int id = rte_get_next_lcore(-1, 1, 0);
    // printf("target lcore is %u\n", id);
    // printf("\nstart receving threads\n");
    // rte_eal_remote_launch(lcore_main_rev, NULL, id);
    // pthread_t tid;
    // pthread_create(&tid, NULL, lcore_main_rev, NULL);

    // send thread in main lcore
    printf("start main sending threads\n");
	lcore_main();
	/* >8 End of called on single lcore. */
    printf("all sending done! waiting for receiving ack ...\n");
    for (;;) {
        for (int i=0; i<flow_num; i++) {
            if (window_list[i].head < NUM_PING)
                receive_once();
                continue;
        }
        break;
    }
    // rte_eal_wait_lcore(id);
    // pthread_join(tid, NULL);
    // printf("all acked!");
    free(window_list);
	/* clean up the EAL */
	rte_eal_cleanup();
    for (int i =0; i<20; i++) {
        printf("%d\t", st[i]);
    }
    printf("\n");
    for (int i =0; i<20; i++) {
        printf("%d\t", rt[i]);
    }
	return 0;
}
