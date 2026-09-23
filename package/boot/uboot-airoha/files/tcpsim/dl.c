/*
 * Drive U-Boot's net/tcp.c against a simulated browser-side TCP.
 *
 * The device serves TOTAL bytes (a known pattern) and closes, the way
 * httpd serves /dump.  The peer ACKs like Linux/Windows do: every second
 * in-order segment at once, a lone one after DELACK ms, anything out of
 * order or filling a hole at once.  Data and ACKs can be dropped at random.
 *
 * usage: dl TOTAL LOSS ACKLOSS SEED DELACK_MS
 */
#include <net.h>
#include <net/tcp.h>
#include <stdlib.h>

void tcp_rx_state_machine(struct tcp_stream *tcp, union tcp_build_pkt *b,
			  unsigned int pkt_len);

static uchar txbuf[2048];
uchar *net_tx_packet = txbuf;
struct in_addr net_ip;

static unsigned long long now_us;

ulong get_timer(ulong base) { return now_us / 1000 - base; }
unsigned long long get_ticks(void) { return now_us; }
unsigned compute_ip_checksum(const void *a, unsigned n) { return 0; }
void net_copy_ip(void *to, const void *from) { memcpy(to, from, 4); }
void net_set_ip_header(uchar *p, struct in_addr d, struct in_addr s, int l,
		       u8 proto) { }

/* ---- parameters and stats ---- */
static u32 TOTAL;
static double LOSS, ACKLOSS;
static int DELACK_MS;
static unsigned long data_bytes_sent, segs_sent, data_errors;

static inline u8 pat(u32 off) { return (u8)((off * 2654435761u) >> 13); }

/* ---- a tiny event queue: packets in flight on the wire ---- */
struct pkt {
	unsigned long long at;
	int to_dev;
	u8 flags;
	u32 seq, ack, len;
	u16 win;
	u8 data[1600];
};

#define QMAX 4096
static struct pkt q[QMAX];
static int qn;
static unsigned long long link_free;	/* device -> peer serialisation */

static void enqueue(struct pkt *p)
{
	if (qn == QMAX) {
		fprintf(stderr, "queue full\n");
		exit(2);
	}
	q[qn++] = *p;
}

static int rnd(double p) { return p > 0 && drand48() < p; }

/* ---- device side ---- */
static struct in_addr peer_ip;
#define PEER_PORT 50000
#define DEV_PORT 80

static int dev_closed, dev_status = -1;
static u32 dev_iss;

int net_send_tcp_packet(int payload_len, struct in_addr dhost, int dport,
			int sport, u8 action, u32 seq, u32 ack)
{
	struct tcp_stream *tcp = tcp_stream_get(0, dhost, dport, sport);
	struct pkt p = { 0 };
	u32 opts;

	if (!tcp)
		return -1;
	tcp->rcv_nxt = ack;	/* what tcp_set_tcp_header() does */
	if (action & TCP_SYN)
		dev_iss = seq;

	opts = ((TCP_TSOPT_SIZE + tcp->lost.len) + 3) & ~3;
	p.to_dev = 0;
	p.flags = action;
	p.seq = seq;
	p.ack = ack;
	p.len = payload_len;
	memcpy(p.data, net_tx_packet + 14 + IP_TCP_HDR_SIZE + opts,
	       payload_len);

	/* 1 Gbit/s wire, 100 us one way; a little CPU per packet */
	now_us += 3;
	if (link_free < now_us)
		link_free = now_us;
	link_free += (payload_len + 78) * 8 / 1000 + 1;
	p.at = link_free + 100;

	if (payload_len) {
		segs_sent++;
		data_bytes_sent += payload_len;
		if (rnd(LOSS))
			return 0;
	}
	enqueue(&p);
	return 0;
}

static int app_tx(struct tcp_stream *tcp, u32 offs, void *buf, int maxlen)
{
	u8 *b = buf;
	int i, n;

	if (offs >= TOTAL)
		return 0;
	n = TOTAL - offs < (u32)maxlen ? TOTAL - offs : maxlen;
	for (i = 0; i < n; i++)
		b[i] = pat(offs + i);
	return n;
}

static void app_una(struct tcp_stream *tcp, u32 bytes)
{
	if (bytes >= TOTAL)
		tcp_stream_close(tcp);
}

static void app_closed(struct tcp_stream *tcp)
{
	dev_closed = 1;
	dev_status = tcp->status;
}

static int app_create(struct tcp_stream *tcp)
{
	tcp->tx = app_tx;
	tcp->on_snd_una_update = app_una;
	tcp->on_closed = app_closed;
	return 1;
}

static void deliver_to_dev(struct pkt *p)
{
	union tcp_build_pkt b;
	struct tcp_stream *tcp;
	unsigned int pkt_len = 40 + p->len;

	memset(&b, 0, sizeof(b));
	b.ip.hdr.tcp_hlen = 0x50;
	b.ip.hdr.tcp_flags = p->flags;
	b.ip.hdr.tcp_seq = htonl(p->seq);
	b.ip.hdr.tcp_ack = htonl(p->ack);
	b.ip.hdr.tcp_win = htons(p->win);
	tcp = tcp_stream_get(p->flags & TCP_SYN, peer_ip, PEER_PORT, DEV_PORT);
	if (!tcp)
		return;
	tcp_rx_state_machine(tcp, &b, pkt_len);
	tcp_stream_put(tcp);
}

/* ---- peer side ---- */
static u8 *got;
static u32 p_rcv;	/* peer rcv_nxt, relative to dev_iss + 1 */
static u32 p_snd = 1001;
static int p_unacked;
static unsigned long long p_delack;	/* 0 = none */
static int p_fin_in, p_fin_out, p_done;

static void peer_send(u8 flags)
{
	struct pkt p = { 0 };

	p.to_dev = 1;
	p.flags = flags;
	p.seq = p_snd;
	p.ack = dev_iss + 1 + p_rcv + (p_fin_in ? 1 : 0);
	p.win = 65535;
	p.at = now_us + 100;
	p_unacked = 0;
	p_delack = 0;
	if ((flags & ~TCP_ACK) == 0 && rnd(ACKLOSS))
		return;
	enqueue(&p);
}

static void deliver_to_peer(struct pkt *p)
{
	u32 off, i, before;

	if (p->flags & TCP_SYN) {		/* SYN-ACK */
		peer_send(TCP_ACK);
		return;
	}
	if (p->flags & TCP_RST) {
		fprintf(stderr, "peer: got RST\n");
		p_done = -1;
		return;
	}

	if (p->len) {
		off = p->seq - dev_iss - 1;
		if (off + p->len > TOTAL) {
			fprintf(stderr, "peer: data past the end at %u\n", off);
			data_errors++;
			return;
		}
		for (i = 0; i < p->len; i++)
			if (p->data[i] != pat(off + i))
				data_errors++;
		if (off + p->len <= p_rcv) {		/* duplicate */
			peer_send(TCP_ACK);
		} else if (off > p_rcv) {		/* out of order */
			memset(got + off, 1, p->len);
			peer_send(TCP_ACK);
		} else {
			memset(got + off, 1, p->len);
			before = p_rcv;
			while (p_rcv < TOTAL && got[p_rcv])
				p_rcv++;
			if (p_rcv > off + p->len || ++p_unacked >= 2)
				peer_send(TCP_ACK);	/* hole filled, or 2nd */
			else if (!p_delack)
				p_delack = now_us + DELACK_MS * 1000ULL;
			(void)before;
		}
	}

	if ((p->flags & TCP_FIN) && p->seq + p->len == dev_iss + 1 + p_rcv &&
	    p_rcv == TOTAL && !p_fin_in) {
		p_fin_in = 1;
		peer_send(TCP_ACK);
		peer_send(TCP_ACK | TCP_FIN);	/* browser closes too */
		p_snd++;
		p_fin_out = 1;
	}
	if (p_fin_out && p->ack == p_snd && (p->flags & TCP_ACK))
		p_done = 1;
}

int main(int argc, char **argv)
{
	struct pkt syn = { 0 };
	unsigned long long limit;
	int i;

	TOTAL = argc > 1 ? strtoul(argv[1], NULL, 0) : 4 << 20;
	LOSS = argc > 2 ? atof(argv[2]) : 0;
	ACKLOSS = argc > 3 ? atof(argv[3]) : 0;
	srand48(argc > 4 ? atol(argv[4]) : 1);
	DELACK_MS = argc > 5 ? atoi(argv[5]) : 40;
	got = calloc(TOTAL + 1, 1);
	peer_ip.s_addr = htonl(0x0a000002);
	limit = 4000ULL * 1000 * 1000;		/* 4000 s */

	tcp_init();
	tcp_stream_set_on_create_handler(app_create);

	syn.to_dev = 1;
	syn.flags = TCP_SYN;
	syn.seq = 1000;
	syn.win = 65535;
	syn.at = 0;
	enqueue(&syn);

	while (now_us < limit && p_done == 0) {
		/* deliver everything that is due, in time order */
		for (;;) {
			int best = -1;

			for (i = 0; i < qn; i++)
				if (q[i].at <= now_us &&
				    (best < 0 || q[i].at < q[best].at))
					best = i;
			if (best < 0)
				break;
			struct pkt p = q[best];

			q[best] = q[--qn];
			if (p.to_dev)
				deliver_to_dev(&p);
			else
				deliver_to_peer(&p);
		}
		if (p_delack && now_us >= p_delack)
			peer_send(TCP_ACK);

		tcp_streams_poll();	/* what net_loop() does each turn */
		now_us += 5;
	}

	/* let the device finish its side of the close */
	for (i = 0; i < 200000 && !dev_closed; i++) {
		for (int j = 0; j < qn; j++)
			if (q[j].to_dev && q[j].at <= now_us) {
				struct pkt p = q[j];

				q[j] = q[--qn];
				deliver_to_dev(&p);
				j--;
			}
		tcp_streams_poll();
		now_us += 5;
	}

	printf("%s: %u bytes in %.3f s = %.1f KiB/s, segs %lu, resent %.2f%%, "
	       "errors %lu, peer %s, dev %s (status %d)\n",
	       p_done == 1 ? "OK" : "FAIL", p_rcv, now_us / 1e6,
	       p_rcv / 1024.0 / (now_us / 1e6), segs_sent,
	       data_bytes_sent ? 100.0 * (data_bytes_sent - TOTAL) / TOTAL : 0,
	       data_errors, p_rcv == TOTAL ? "complete" : "SHORT",
	       dev_closed ? "closed" : "OPEN", dev_status);

	return !(p_done == 1 && p_rcv == TOTAL && !data_errors && dev_closed &&
		 dev_status == 0);
}
