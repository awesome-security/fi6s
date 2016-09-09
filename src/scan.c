#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h> // rand()
#include <string.h>
#include <unistd.h> // usleep()
#include <stdatomic.h>
#include <pthread.h>

#include "scan.h"
#include "target.h"
#include "util.h"
#include "rawsock.h"
#include "tcp.h"

static uint8_t source_addr[16];
static int source_port;
static struct ports ports;
static int max_rate;

static atomic_uint pkts_sent, pkts_recv;

static inline int source_port_rand(void);
static void *send_thread(void *unused);
static void *recv_thread(void *unused);

#define ETH_FRAME(buf) ( (struct frame_eth*) &(buf)[0] )
#define IP_FRAME(buf) ( (struct frame_ip*) &(buf)[FRAME_ETH_SIZE] )
#define TCP_HEADER(buf) ( (struct tcp_header*) &(buf)[FRAME_ETH_SIZE + FRAME_IP_SIZE] )

#if ATOMIC_INT_LOCK_FREE != 2
#warning Non lock-free atomic types will severely affect performance.
#endif

void scan_settings(const uint8_t *_source_addr, int _source_port, const struct ports *_ports, int _max_rate)
{
	memcpy(source_addr, _source_addr, 16);
	source_port = _source_port;
	memcpy(&ports, _ports, sizeof(struct ports));
	max_rate = _max_rate - 1;
}

int scan_main(const char *interface, int quiet)
{
	if(rawsock_open(interface, 2048) < 0)
		return -1;
	setvbuf(stdout, NULL, _IONBF, 0);
	atomic_store(&pkts_sent, 0);
	atomic_store(&pkts_recv, 0);

	// Set capture filters
	int fflags = RAWSOCK_FILTER_IPTYPE | RAWSOCK_FILTER_DSTADDR;
	if(source_port != -1)
		fflags |= RAWSOCK_FILTER_DSTPORT;
	if(rawsock_setfilter(fflags, IP_TYPE_TCP, source_addr, source_port) < 0)
		goto err;

	// Start threads
	pthread_t ts, tr;
	if(pthread_create(&ts, NULL, send_thread, NULL) < 0)
		goto err;
	pthread_detach(ts);
	if(pthread_create(&tr, NULL, recv_thread, NULL) < 0)
		goto err;
	pthread_detach(tr);

	// Stats
	while(1) {
		if(quiet)
			goto skip;

		unsigned int cur_sent, cur_recv;
		cur_sent = atomic_exchange(&pkts_sent, 0);
		cur_recv = atomic_exchange(&pkts_recv, 0);
		printf("snt:%4u rcv:%4u\r", cur_sent, cur_recv);

		skip:
		usleep(1000 * 1000);
	}

	int r = 0;
	ret:
	rawsock_close();
	return r;
	err:
	r = 1;
	goto ret;
}


static void *send_thread(void *unused)
{
	uint8_t _Alignas(long int) packet[FRAME_ETH_SIZE + FRAME_IP_SIZE + TCP_HEADER_SIZE];
	uint8_t dstaddr[16];
	struct ports_iter it;

	(void) unused;
	rawsock_eth_prepare(ETH_FRAME(packet), ETH_TYPE_IPV6);
	rawsock_ip_prepare(IP_FRAME(packet), IP_TYPE_TCP);
	if(target_gen_next(dstaddr) < 0)
		return NULL;
	rawsock_ip_modify(IP_FRAME(packet), TCP_HEADER_SIZE, dstaddr);
	ports_iter_begin(&ports, &it);

	while(1) {
		// Next port number/target
		if(ports_iter_next(&it) == 0) {
			if(target_gen_next(dstaddr) < 0)
				break; // no more targets
			rawsock_ip_modify(IP_FRAME(packet), TCP_HEADER_SIZE, dstaddr);
			ports_iter_begin(NULL, &it);
			continue;
		}

		// Make, checksum and send syn packet
		make_a_syn_pkt_pls(TCP_HEADER(packet), it.val, source_port==-1?source_port_rand():source_port);
		checksum_pkt_pls(IP_FRAME(packet), TCP_HEADER(packet));
		rawsock_send(packet, sizeof(packet));

		// Rate control
		if(atomic_fetch_add(&pkts_sent, 1) >= max_rate) {
			// FIXME: this doesn't seem like a good idea
			while(atomic_load(&pkts_sent) != 0)
				usleep(1000);
		}
	}

	return NULL;
}

static void *recv_thread(void *unused)
{
	int ret, len;
	uint64_t ts;
	const uint8_t *packet;

	int v;
	const uint8_t *csrcaddr;

	(void) unused;
	while(1) {
		// Wait for packet
		do {
			ret = rawsock_sniff(&ts, &len, &packet);
			if(ret < 0)
				return NULL;
		} while(ret == 0);
		atomic_fetch_add(&pkts_recv, 1);
		printf("<< @%lu -- %d bytes\n", ts, len);

		// Decode it and output results
		if(len < FRAME_ETH_SIZE)
			goto perr;
		rawsock_eth_decode(ETH_FRAME(packet), &v);
		if(v != ETH_TYPE_IPV6 || len < FRAME_ETH_SIZE + FRAME_IP_SIZE)
			goto perr;
		rawsock_ip_decode(IP_FRAME(packet), &v, NULL, &csrcaddr, NULL);
		if(v != IP_TYPE_TCP || len < FRAME_ETH_SIZE + FRAME_IP_SIZE + TCP_HEADER_SIZE)
			goto perr;
		if(TCP_HEADER(packet)->f_ack && (TCP_HEADER(packet)->f_syn || TCP_HEADER(packet)->f_rst)) {
			decode_pkt_pls(TCP_HEADER(packet), &v, NULL);
			char tmp[IPV6_STRING_MAX];
			ipv6_string(tmp, csrcaddr);
			printf("%s port %d %s\n", tmp, v, TCP_HEADER(packet)->f_syn?"open":"closed");
		}

		continue;
		perr:
		printf("Packet decoding error...\n");
	}
}

static inline int source_port_rand(void)
{
	int v;
	v = rand() & 0xffff; // random 16-bit number
	v |= 4096; // ensure that 1) it's not zero 2) it's >= 4096
	return v;
}
