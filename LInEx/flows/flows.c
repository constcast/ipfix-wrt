#include "flows.h"
#include "../ipfixlolib/msg.h"
#include "iface.h"
#include "ip_helper.h"

#include "../event_loop.h"

#include <arpa/inet.h>
#include <netinet/ether.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/filter.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <net/if.h>
#include <stdbool.h>
#include <fcntl.h>

static int parse_ipv4(flow_capture_session *session, struct pktinfo *pkt);
#ifdef SUPPORT_IPV6
static int parse_ipv6(flow_capture_session *session, struct pktinfo *pkt);
#endif
static int parse_udp(flow_capture_session *session, struct pktinfo *pkt, flow_key *flow);
static int parse_tcp(flow_capture_session *session, struct pktinfo *pkt, flow_key *flow);

struct flow_capture_callback_param {
	flow_capture_session *session;
	struct capture_info *info;
};

void capture_callback(int fd, struct flow_capture_callback_param *param);

/**
  * Compiled BPF filter: tcpdump -dd "not ether src de:ad:be:ef:aa:aa and (ip or ip6)"
  */
static struct sock_filter egress_filter[] = {
    { 0x20, 0, 0, 0x00000008 },
    { 0x15, 0, 2, 0xbeefaaaa },
    { 0x28, 0, 0, 0x00000006 },
    { 0x15, 4, 0, 0x0000dead },
    { 0x28, 0, 0, 0x0000000c },
    { 0x15, 1, 0, 0x00000800 },
    { 0x15, 0, 1, 0x000086dd },
    { 0x6, 0, 0, 0x0000ffff },
    { 0x6, 0, 0, 0x00000000 }
};

/**
  * Compiled BPF filter: tcpdump -dd "ip or ip6"
  */
static struct sock_filter ip_filter[] = {
    { 0x28, 0, 0, 0x0000000c },
    { 0x15, 1, 0, 0x00000800 },
    { 0x15, 0, 1, 0x000086dd },
    { 0x6, 0, 0, 0x0000ffff },
    { 0x6, 0, 0, 0x00000000 },
};

int start_flow_capture_session(flow_capture_session *session,
							   uint16_t export_timeout,
							   uint16_t max_flow_lifetime) {
	session->capture_session = start_capture_session();
	if (!session->capture_session)
		return -1;

    session->export_timeout = export_timeout;
	session->max_flow_lifetime = max_flow_lifetime;
	session->ipv4_flow_database = kh_init(1);
#ifdef SUPPORT_IPV6
	session->ipv6_flow_database = kh_init(1);
#endif

    return 0;
}



static struct sock_fprog build_filter(const struct sockaddr *hwaddr) {
    struct sock_fprog prog = { 0, NULL };


    if (hwaddr->sa_family == ARPHRD_ETHER) {
        struct sock_filter *filter = egress_filter;

        const char *macaddr = hwaddr->sa_data;

        // Last 32 bit of MAC address
        filter[1].k = ntohl(*((uint32_t *) macaddr + 2));
        // First 16 bit of MAC address
        filter[3].k = ntohs(*((uint16_t *) macaddr));

        prog.len = sizeof(egress_filter) / sizeof(struct sock_filter);
        prog.filter = filter;

    } else {
        prog.filter = ip_filter;
        prog.len = sizeof(ip_filter) / sizeof(struct sock_filter);
    }

    return prog;
}

/**
  * Adds the given interface to the capture list.
  */
int add_interface(flow_capture_session *session, char *device_name, bool enable_promisc) {
	struct ifreq req;
	int fd = -1;

	if (iface_info(device_name, &req, &fd) == -1) {
		return -1;
	}

	struct sockaddr hwaddr;
	if (iface_hwaddr(&req, fd, &hwaddr)) {
		close(fd);
		return -1;
	}

	close(fd);

	struct sock_fprog filter = build_filter(&hwaddr);
	struct capture_info *info = start_capture(session->capture_session,
											  device_name, 256, &filter);
	if (!info) {
		return -1;
	}

	struct flow_capture_callback_param *param =
			(struct flow_capture_callback_param *) malloc(sizeof(struct flow_capture_callback_param));

	param->session = session;
	param->info = info;

	event_loop_add_fd(info->fd, (void (*) (int, void *)) &capture_callback, param);

    return 0;
}

static void free_flow_database(khash_t(1) *flow_database) {
	if (flow_database == NULL)
		return;

	khiter_t k;
	for (k = kh_begin(flow_database); k != kh_end(flow_database); ++k) {
		if (!kh_exist(flow_database, k))
			continue;
		flow_key *key = kh_key(flow_database, k);
		free(kh_value(flow_database, k));

		kh_del(1, flow_database, k);
		free(key);

	}
}

/**
  * Stops the given capture session. It is not possible to use this session from the
  * capture call afterwards.
  */
void stop_flow_capture_session(flow_capture_session *session) {
	free_flow_database(session->ipv4_flow_database);
	session->ipv4_flow_database = NULL;

#ifdef SUPPORT_IPV6
	free_flow_database(session->ipv6_flow_database);
	session->ipv6_flow_database = NULL;
#endif
}


static inline int parse_ethernet(flow_capture_session *session, struct pktinfo *pkt) {
    if (pkt->data + sizeof(struct ether_header) > pkt->end_data) {
        msg(MSG_ERROR, "Packet too short to be a valid ethernet packet.");
        return -1;
    }
	// DPRINTF("Parsing ethernet header");

    const struct ether_header * const hdr = (const struct ether_header * const) pkt->data;

    pkt->data += sizeof(struct ether_header);

    switch (ntohs(hdr->ether_type)) {
    case ETHERTYPE_IP:
		return parse_ipv4(session, pkt);
#ifdef SUPPORT_IPV6
    case ETHERTYPE_IPV6:
		return parse_ipv6(session, pkt);
#endif
    default:
		DPRINTF("Unsupported link layer protocol (%x).", ntohs(hdr->ether_type));
        return 0;
    }
}

/*

static int parse_ip(capture_session *session, const struct pcap_pkthdr *const pkthdr, const u_char *const data, const u_char *const end_data) {
    if (data + 1 > end_data) {
        msg(MSG_ERROR, "Packet too short to be a valid IP packet.");
        return -1;
    }

    u_char version = (*data & 0xf0) >> 4;

    switch (version) {
    case 4:
        return parse_ipv4(session, pkthdr, data, end_data);
    case 6:
        return parse_ipv6(session, pkthdr, data, end_data);
    default:
        msg(MSG_ERROR, "Unknown IP header version (%d).", version);
        return -1;
    }
}
*/

static inline int parse_ipv4(flow_capture_session *session, struct pktinfo *pkt) {
    if (pkt->data + sizeof(struct iphdr) > pkt->end_data) {
        msg(MSG_ERROR, "Packet too short to be a valid IPv4 packet (by %t bytes).", (pkt->data + sizeof(struct iphdr) - pkt->end_data));
        return -1;
    }

    const struct iphdr * const hdr = (const struct iphdr * const) pkt->data;

    // Determine start address of payload based on the IHL header.
    pkt->data += hdr->ihl * 4;

    if (pkt->data > pkt->end_data) {
        msg(MSG_ERROR, "Packet payload points beyond capture end.");
        return -1;
    }

	flow_key key;

	key.protocol = IPv4;
	key.src_addr.v4.s_addr = hdr->saddr;
	key.dst_addr.v4.s_addr = hdr->daddr;

    switch (hdr->protocol) {
    case SOL_UDP:
		return parse_udp(session, pkt, &key);
        break;
    case SOL_TCP:
		return parse_tcp(session, pkt, &key);
    default:
        return 0;
    }
}

#ifdef SUPPORT_IPV6
static inline int parse_ipv6(flow_capture_session *session, struct pktinfo *pkt) {
	// No need to check the length - ipv6_extract_transport_protocol does that
	// for us.
	const struct ip6_hdr * const hdr = (const struct ip6_hdr * const) pkt->data;
	int transport_protocol = ipv6_extract_transport_protocol(pkt);

	if (transport_protocol == -1)
		return -1;

	struct flow_key_t flow;

	memcpy(&flow.dst_addr, &hdr->ip6_dst, sizeof(hdr->ip6_dst));
	memcpy(&flow.src_addr, &hdr->ip6_src, sizeof(hdr->ip6_src));
	flow.protocol = IPv6;

	switch (transport_protocol) {
	case 6:
		return parse_tcp(session, pkt, &flow);
	case 17:
		return parse_udp(session, pkt, &flow);
	default:
		return -1;
	}
}
#endif

static inline int parse_udp(flow_capture_session *session, struct pktinfo *pkt, flow_key *flow) {
    if (pkt->data + sizeof(struct udphdr) > pkt->end_data) {
        msg(MSG_ERROR, "Packet too short to be a valid UDP packet.");
        return -1;
    }

    const struct udphdr * const hdr = (const struct udphdr * const) pkt->data;

    flow->t_protocol = TRANSPORT_UDP;
    flow->src_port = hdr->source;
    flow->dst_port = hdr->dest;

    pkt->data += sizeof(struct udphdr);

    flow_info *info = NULL;
	khash_t(1) *flow_database = NULL;
	khiter_t k;

	switch (flow->protocol) {
	case IPv4:
		flow_database = session->ipv4_flow_database;
		break;
#ifdef SUPPORT_IPV6
	case IPv6:
		flow_database = session->ipv6_flow_database;
		break;
#endif
	}

	k = kh_get(1, flow_database, flow);

	if (k == kh_end(flow_database)) {
        int ret;

		info = (flow_info *) calloc(1, sizeof(flow_info));
        if (info == NULL) {
            msg(MSG_ERROR, "Failed to allocate memory for flow info structure.");
            return -1;
        }

		// Create a copy of the key on the heap on the first insertion
		flow_key *old_flow = flow;
		flow = (flow_key *) malloc (sizeof(flow_key));
		memcpy(flow, old_flow, sizeof(flow_key));

        info->first_packet_timestamp = time(NULL);

		k = kh_put(1, flow_database, flow, &ret);
		kh_value(flow_database, k) = info;
    } else {
		info = (flow_info *) kh_value(flow_database, k);
    }

    info->last_packet_timestamp = time(NULL);
	info->total_bytes += pkt->orig_len;

    return 0;
}

static inline int parse_tcp(flow_capture_session *session, struct pktinfo *pkt, flow_key *flow) {
    if (pkt->data + sizeof(struct tcphdr) > pkt->end_data) {
        msg(MSG_ERROR, "Packet too short to be a valid UDP packet.");
        return -1;
    }

    const struct tcphdr * const hdr = (const struct tcphdr * const) pkt->data;

    flow->t_protocol = TRANSPORT_TCP;
    flow->src_port = hdr->source;
    flow->dst_port = hdr->dest;

    flow_info *info = NULL;
	khash_t(1) *flow_database = NULL;
    khiter_t k;

	switch (flow->protocol) {
	case IPv4:
		flow_database = session->ipv4_flow_database;
		break;
#ifdef SUPPORT_IPV6
	case IPv6:
		flow_database = session->ipv6_flow_database;
		break;
#endif
	}

	k = kh_get(1, flow_database, flow);

	if (k == kh_end(flow_database)) {
        int ret;

        if (!(hdr->syn == 1 && hdr->ack == 0)) {
            // This is not a new connection - ignore it
            return -1;
        }

        info = (flow_info *) calloc(1, sizeof(flow_info));

        if (info == NULL) {
            msg(MSG_ERROR, "Failed to allocate memory for flow info structure.");
            return -1;
        }

        info->first_packet_timestamp = time(NULL);

		// Create a copy of the key on the heap on the first insertion
		flow_key *old_flow = flow;
		flow = (flow_key *) malloc (sizeof(flow_key));
		memcpy(flow, old_flow, sizeof(flow_key));

		k = kh_put(1, flow_database, flow, &ret);
		kh_value(flow_database, k) = info;


    } else {
		info = (flow_info *) kh_value(flow_database, k);
    }

    info->last_packet_timestamp = time(NULL);
	info->total_bytes += pkt->orig_len;

    return 0;
}

void capture_callback(int fd, struct flow_capture_callback_param *param) {
	size_t len;
	size_t orig_len;
	uint8_t *buffer;

	while ((buffer = capture_packet(param->info, &len, &orig_len))) {
		struct pktinfo pkt = { buffer, buffer + len, buffer, orig_len };
		parse_ethernet(param->session, &pkt);

		capture_packet_done(param->info);
	}

}

static uint32_t flow_key_hash_code_ipv4(flow_key *key, uint32_t hashcode) {
    uint32_t addr1;
    uint32_t addr2;

	if (key->src_addr.v4.s_addr < key->dst_addr.v4.s_addr) {
		addr1 = key->src_addr.v4.s_addr;
		addr2 = key->dst_addr.v4.s_addr;
    } else {
		addr1 = key->dst_addr.v4.s_addr;
		addr2 = key->src_addr.v4.s_addr;
    }

    hashcode = hashcode * 23 + addr1;
    hashcode = hashcode * 23 + addr2;

    return hashcode;
}

#ifdef SUPPORT_IPV6
static uint32_t flow_key_hash_code_ipv6(flow_key *key, uint32_t hashcode) {
    uint8_t *addr1;
    uint8_t *addr2;

    if (memcmp(&key->src_addr, &key->dst_addr, sizeof(key->src_addr)) <= 0) {
		addr1 = (uint8_t *) key->src_addr.v6.s6_addr;
		addr2 = (uint8_t *) key->dst_addr.v6.s6_addr;
    } else {
		addr1 = (uint8_t *) key->dst_addr.v6.s6_addr;
		addr2 = (uint8_t *) key->src_addr.v6.s6_addr;
    }
    int i;

    for (i = 0; i < 4; i++) {
        hashcode = hashcode * 23 + *(addr1 + i);
        hashcode = hashcode * 23 + *(addr2 + i);
    }

    return hashcode;
}
#endif

uint32_t flow_key_hash_code(struct flow_key_t *key) {
    uint32_t hashcode = 17;

    uint16_t port1;
    uint16_t port2;

    if (key->src_port < key->dst_port) {
        port1 = key->src_port;
        port2 = key->dst_port;
    } else {
        port1 = key->dst_port;
        port2 = key->src_port;
    }

    hashcode = hashcode * 23 + ((port1 << 16) | port2);
    hashcode = hashcode * 23 + (((char) key->protocol) << 8 | (char) key->t_protocol);

    switch (key->protocol) {
    case IPv4:
		return flow_key_hash_code_ipv4(key, hashcode);
#ifdef SUPPORT_IPV6
    case IPv6:
		return flow_key_hash_code_ipv6(key, hashcode);
#endif
    default:
        msg(MSG_ERROR, "Hashcode was called for unsupported flow key type.");
        return hashcode;
    }
}

static int flow_key_equals_ipv4(const flow_key *a, const flow_key *b) {
	return (a->src_addr.v4.s_addr == b->src_addr.v4.s_addr &&
			a->dst_addr.v4.s_addr == b->dst_addr.v4.s_addr &&
			a->src_port == b->src_port &&
			a->dst_port == b->dst_port)
            ||
			(a->src_addr.v4.s_addr == b->dst_addr.v4.s_addr &&
			 a->dst_addr.v4.s_addr == b->src_addr.v4.s_addr &&
			 a->src_port == b->dst_port &&
			 a->dst_port == b->src_port);
}

#ifdef SUPPORT_IPV6
static int flow_key_equals_ipv6(const flow_key *a, const flow_key *b) {
	return (memcmp(&a->src_addr.v6, &b->src_addr.v6, sizeof(a->src_addr.v6)) == 0 &&
			memcmp(&a->dst_addr.v6, &b->dst_addr.v6, sizeof(a->dst_addr.v6)) == 0 &&
			a->src_port == b->src_port &&
			a->dst_port == b->dst_port)
            ||
			(memcmp(&a->src_addr.v6, &b->dst_addr.v6, sizeof(a->src_addr.v6)) == 0 &&
			 memcmp(&a->dst_addr.v6, &b->src_addr.v6, sizeof(a->dst_addr.v6)) == 0 &&
			 a->src_port == b->dst_port &&
			 a->dst_port == b->src_port);
}
#endif


int flow_key_equals(struct flow_key_t *a, struct flow_key_t *b) {
    if (a->protocol != b->protocol ||
            a->t_protocol != b->t_protocol) {
        return 0;
    }

    switch (a->protocol) {
    case IPv4:
		return flow_key_equals_ipv4(a, b);
#ifdef SUPPORT_IPV6
    case IPv6:
		return flow_key_equals_ipv6(a, b);
#endif
    default:
        msg(MSG_ERROR, "Equals was called for unsupported flow key type.");
        return 0;
    }
}
