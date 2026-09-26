/* Just enough of U-Boot's net.h for net/tcp.c on the host. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef unsigned long ulong;
typedef unsigned char uchar;

#define __packed __attribute__((packed))
#define fallthrough __attribute__((fallthrough))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IS_ENABLED(x) (x)
#define CONFIG_PROT_TCP_SACK 0
#define CONFIG_SYS_HZ 1000
#define PKTBUFSRX 8
#define IP_HDR_SIZE 20
#define DEBUG_DEV_PKT 0
#define debug_cond(c, ...) do { } while (0)

extern uchar *net_tx_packet;
extern struct in_addr net_ip;
static inline int net_eth_hdr_size(void) { return 14; }
int net_send_tcp_packet(int payload_len, struct in_addr dhost, int dport,
			int sport, u8 action, u32 tcp_seq_num, u32 tcp_ack_num);
ulong get_timer(ulong base);
unsigned long long get_ticks(void);
unsigned compute_ip_checksum(const void *addr, unsigned nbytes);
void net_copy_ip(void *to, const void *from);
void net_set_ip_header(uchar *pkt, struct in_addr dest, struct in_addr source,
		       int pkt_len, u8 proto);
