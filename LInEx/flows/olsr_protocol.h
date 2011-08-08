/**
  * Data structures and helpers for the OLSR protocol. Heavily based on the definitions from
  * the OLSRd project.
  *
  */
#ifndef OLSR_PROTOCOL_H_
#define OLSR_PROTOCOL_H_

#include <stdint.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mantissa.h"

#define OLSR_PACKET_HEADER_LEN 4
// Message header len without the originator address
#define OLSR_MESSAGE_HEADER_LEN 8

#define OLSR_TC_MESSAGE_HEADER_LEN 4
#define OLSR_TC_LQ_MESSAGE_HEADER_LEN OLSR_TC_MESSAGE_HEADER_LEN + 2

#define OLSR_HELLO_MESSAGE_HEADER_LEN 4

#define OLSR_HELLO_INFO_HEADER_LEN 4

/* Seqnos are 16 bit values */

#define MAXVALUE 0xFFFF

/* Macro for checking seqnos "wraparound" */
#define SEQNO_GREATER_THAN(s1, s2)                \
    (((s1 > s2) && (s1 - s2 <= (MAXVALUE/2))) \
    || ((s2 > s1) && (s2 - s1 > (MAXVALUE/2))))

enum olsr_message_type {
    HELLO_MESSAGE=1,
    TC_MESSAGE=2,
    HELLO_LQ_MESSAGE=201,
    TC_LQ_MESSAGE=202
};

union olsr_ip_addr {
    struct in_addr v4;
    struct in6_addr v6;
};

typedef uint32_t olsr_reltime;

/**
  * Definitions from OLSRd sources.
  */

/* deserialized OLSR packet header */

struct olsr_packet {
    uint16_t size;
    uint16_t seqno;
};

/* deserialized OLSR header */

struct olsr_common {
    uint8_t type;
    olsr_reltime vtime;
    uint16_t size;
    union olsr_ip_addr orig;
    uint8_t ttl;
    uint8_t hops;
    uint16_t seqno;

    /**
    * Pointer to the end of the message.
    */
    const u_char *end;
};

/* deserialized LQ_HELLO */

struct olsr_hello_message {
    struct olsr_common comm;
    olsr_reltime htime;
    uint8_t will;
    struct lq_hello_neighbor *neigh;
};

/* serialized LQ_HELLO */
struct olsr_hello_message_info {
    union {
        uint8_t link_type:2;
        uint8_t neigh_type:2;
        uint8_t val;
    } link_code;
    uint16_t size;
};

/* deserialized LQ_TC */
struct olsr_tc_message {
    struct olsr_common comm;
    uint16_t ansn;
    uint8_t lower_border;
    uint8_t upper_border;
};

static inline void
pkt_get_u8(const uint8_t ** p, uint8_t * var)
{
    *var = *(const uint8_t *)(*p);
    *p += sizeof(uint8_t);
}
static inline void
pkt_get_u16(const uint8_t ** p, uint16_t * var)
{
    *var = ntohs(**((const uint16_t **)p));
    *p += sizeof(uint16_t);
}
static inline void
pkt_get_u32(const uint8_t ** p, uint32_t * var)
{
    *var = ntohl(**((const uint32_t **)p));
    *p += sizeof(uint32_t);
}
static inline void
pkt_get_s8(const uint8_t ** p, int8_t * var)
{
    *var = *(const int8_t *)(*p);
    *p += sizeof(int8_t);
}
static inline void
pkt_get_s16(const uint8_t ** p, int16_t * var)
{
    *var = ntohs(**((const int16_t **)p));
    *p += sizeof(int16_t);
}
static inline void
pkt_get_s32(const uint8_t ** p, int32_t * var)
{
    *var = ntohl(**((const int32_t **)p));
    *p += sizeof(int32_t);
}

static inline void
pkt_get_reltime(const uint8_t ** p, olsr_reltime * var)
{
    *var = me_to_reltime(**p);
    *p += sizeof(uint8_t);
}

static inline void
pkt_get_ip_address(const uint8_t ** p, union olsr_ip_addr * var, enum network_protocol_t type) {
    if (type == IPv4) {
        pkt_get_u32(p, &var->v4.s_addr);
    } else {
        memcpy(&var->v6.s6_addr, *p, sizeof(var->v6));
        *p += sizeof(var->v6);
    }
}

static inline void
pkt_ignore_u8(const uint8_t ** p)
{
    *p += sizeof(uint8_t);
}
static inline void
pkt_ignore_u16(const uint8_t ** p)
{
    *p += sizeof(uint16_t);
}
static inline void
pkt_ignore_u32(const uint8_t ** p)
{
    *p += sizeof(uint32_t);
}
static inline void
pkt_ignore_s8(const uint8_t ** p)
{
    *p += sizeof(int8_t);
}
static inline void
pkt_ignore_s16(const uint8_t ** p)
{
    *p += sizeof(int16_t);
}
static inline void
pkt_ignore_s32(const uint8_t ** p)
{
    *p += sizeof(int32_t);
}

static inline uint16_t ip_addr_len(enum network_protocol_t type) {
    switch (type) {
    case IPv4:
        return sizeof(struct in_addr);
    case IPv6:
        return sizeof(struct in6_addr);
    default:
        return 0;
    }
}

#endif