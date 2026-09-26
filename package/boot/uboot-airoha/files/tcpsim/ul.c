/*
 * Upload + small response, the shape of every flashing page in httpd.
 *
 * The peer uploads UP bytes (a known pattern); the device checks each byte
 * in rx(), and once the answer is ready sends RESP bytes.  When the answer
 * is fully acknowledged it "acts" (where httpd sets flash_pending) and
 * closes -- the same place httpd_on_snd_una_update() does both.
 *
 * EARLY=1: the device answers after the first 1000 bytes, while the body is
 * still coming, like a 400 on bad headers; it must still swallow the rest.
 *
 * usage: ul UP RESP LOSS ACKLOSS SEED EARLY
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

static u32 UP, RESP;
static double LOSS, ACKLOSS;
static int EARLY;
static unsigned long up_errors, resp_errors, up_sent;
static int acted;

static inline u8 pat_up(u32 off) { return (u8)((off * 40503u) >> 7); }
static inline u8 pat_resp(u32 off) { return (u8)((off * 2654435761u) >> 13); }
static int rnd(double p) { return p > 0 && drand48() < p; }

struct pkt {
	unsigned long long at;
	int to_dev;
	u8 flags;
	u32 seq, ack, len;
	u16 win;
	u8 data[1600];
};

#define QMAX 8192
static struct pkt q[QMAX];
static int qn;
static unsigned long long link_free;

static void enqueue(struct pkt *p)
{
	if (qn == QMAX) {
		fprintf(stderr, "queue full\n");
		exit(2);
	}
	q[qn++] = *p;
}

static struct in_addr peer_ip;
#define PEER_PORT 50000
#define DEV_PORT 80
#define PEER_ISS 1000

static int dev_closed, dev_status = -1;
static u32 dev_iss;

/* ---- device application, shaped like httpd ---- */
static u8 *dev_got;
static int resp_ready;

int net_send_tcp_packet(int payload_len, struct in_addr dhost, int dport,
			int sport, u8 action, u32 seq, u32 ack)
{
	struct tcp_stream *tcp = tcp_stream_get(0, dhost, dport, sport);
	struct pkt p = { 0 };
	u32 opts;

	if (!tcp)
		return -1;
	tcp->rcv_nxt = ack;
	if (action & TCP_SYN)
		dev_iss = seq;

	opts = ((TCP_TSOPT_SIZE + tcp->lost.len) + 3) & ~3;
	p.flags = action;
	p.seq = seq;
	p.ack = ack;
	p.len = payload_len;
	memcpy(p.data, net_tx_packet + 14 + IP_TCP_HDR_SIZE + opts,
	       payload_len);

	now_us += 3;
	if (link_free < now_us)
		link_free = now_us;
	link_free += (payload_len + 78) * 8 / 1000 + 1;
	p.at = link_free + 100;

	if (payload_len ? rnd(LOSS) : (!(action & (TCP_SYN | TCP_FIN)) &&
				       rnd(ACKLOSS)))
		return 0;
	enqueue(&p);
	return 0;
}

static int app_rx(struct tcp_stream *tcp, u32 offs, void *buf, int len)
{
	u8 *b = buf;
	int i;

	for (i = 0; i < len; i++) {
		if (offs + i >= UP || b[i] != pat_up(offs + i))
			up_errors++;
		else
			dev_got[offs + i] = 1;
	}
	return len;
}

static void app_rcv(struct tcp_stream *tcp, u32 rx_bytes)
{
	if (rx_bytes >= (EARLY ? (UP < 1000 ? UP : 1000) : UP))
		resp_ready = 1;
}

static int app_tx(struct tcp_stream *tcp, u32 offs, void *buf, int maxlen)
{
	u8 *b = buf;
	int i, n;

	if (!resp_ready || offs >= RESP)
		return 0;
	n = RESP - offs < (u32)maxlen ? RESP - offs : maxlen;
	for (i = 0; i < n; i++)
		b[i] = pat_resp(offs + i);
	return n;
}

static void app_una(struct tcp_stream *tcp, u32 bytes)
{
	if (bytes < RESP)
		return;
	tcp_stream_close(tcp);
	acted++;		/* flash_pending = 1 */
}

static void app_closed(struct tcp_stream *tcp)
{
	dev_closed = 1;
	dev_status = tcp->status;
}

static int app_create(struct tcp_stream *tcp)
{
	tcp->rx = app_rx;
	tcp->tx = app_tx;
	tcp->on_rcv_nxt_update = app_rcv;
	tcp->on_snd_una_update = app_una;
	tcp->on_closed = app_closed;
	return 1;
}

static void deliver_to_dev(struct pkt *p)
{
	union tcp_build_pkt b;
	struct tcp_stream *tcp;

	memset(&b, 0, sizeof(b));
	b.ip.hdr.tcp_hlen = 0x50;
	b.ip.hdr.tcp_flags = p->flags;
	b.ip.hdr.tcp_seq = htonl(p->seq);
	b.ip.hdr.tcp_ack = htonl(p->ack);
	b.ip.hdr.tcp_win = htons(p->win);
	memcpy((u8 *)&b + 40, p->data, p->len);
	tcp = tcp_stream_get(p->flags & TCP_SYN, peer_ip, PEER_PORT, DEV_PORT);
	if (!tcp)
		return;
	tcp_rx_state_machine(tcp, &b, 40 + p->len);
	tcp_stream_put(tcp);
}

/* ---- peer: a sender with a window, and a receiver with delayed ACK ---- */
#define PMSS 1460
#define PWND 5840		/* what the device advertises, unscaled */
#define PRTO 300000ULL		/* us */

static u32 s_una, s_nxt, s_max;	/* peer send side, offsets into the upload */
static int s_dups;
static unsigned long long s_last;	/* last progress */
static int established;

static u8 *got;
static u32 p_rcv;		/* response bytes in order */
static int p_unacked;
static unsigned long long p_delack;
static int p_fin_in, p_fin_out, p_done;

static u32 peer_ack(void)
{
	return dev_iss + 1 + p_rcv + (p_fin_in ? 1 : 0);
}

static void peer_emit(u8 flags, u32 off, u32 len)
{
	struct pkt p = { 0 };
	u32 i;

	p.to_dev = 1;
	p.flags = flags;
	/* a pure ACK carries SND.NXT, the highest sent, even after a go-back */
	p.seq = PEER_ISS + 1 + (len || (flags & TCP_FIN) ? off : s_max);
	p.ack = peer_ack();
	p.win = 65535;
	p.len = len;
	for (i = 0; i < len; i++)
		p.data[i] = pat_up(off + i);
	p.at = now_us + 100;
	p_unacked = 0;		/* any segment carries our ACK */
	p_delack = 0;
	if (len) {
		up_sent += len;
		if (rnd(LOSS))
			return;
	} else if (flags == TCP_ACK && rnd(ACKLOSS)) {
		return;
	}
	enqueue(&p);
}

static void peer_send_more(void)
{
	u32 len;

	if (!established)
		return;
	while (s_nxt < UP && s_nxt < s_una + PWND) {
		len = UP - s_nxt < PMSS ? UP - s_nxt : PMSS;
		if (s_nxt + len > s_una + PWND)
			break;
		peer_emit(TCP_ACK | TCP_PUSH, s_nxt, len);
		s_nxt += len;
		if (s_nxt > s_max)
			s_max = s_nxt;
	}
	if (p_fin_in && !p_fin_out && s_una == UP) {
		peer_emit(TCP_ACK | TCP_FIN, UP, 0);
		p_fin_out = 1;
	}
}

static void peer_on_ack(u32 ack, u32 len)
{
	u32 a = ack - (PEER_ISS + 1);

	if (a > UP + 1)		/* not an ack of ours */
		return;
	if (a > UP)
		a = UP;
	if (a > s_una) {
		s_una = a;
		s_dups = 0;
		s_last = now_us;
	} else if (a == s_una && s_una < s_nxt && !len && ++s_dups == 3) {
		u32 l = UP - s_una < PMSS ? UP - s_una : PMSS;

		peer_emit(TCP_ACK | TCP_PUSH, s_una, l);	/* fast rexmit */
	}
}

static void deliver_to_peer(struct pkt *p)
{
	u32 off, i;

	if (p->flags & TCP_SYN) {
		established = 1;
		s_last = now_us;
		peer_emit(TCP_ACK, 0, 0);
		return;
	}
	if (p->flags & TCP_RST) {
		fprintf(stderr, "peer: got RST\n");
		p_done = -1;
		return;
	}

	peer_on_ack(p->ack, p->len);

	if (p->len) {
		off = p->seq - dev_iss - 1;
		for (i = 0; i < p->len; i++)
			if (off + i >= RESP || p->data[i] != pat_resp(off + i))
				resp_errors++;
		if (off + p->len <= p_rcv) {
			peer_emit(TCP_ACK, s_nxt, 0);
		} else if (off > p_rcv) {
			if (off + p->len <= RESP)
				memset(got + off, 1, p->len);
			peer_emit(TCP_ACK, s_nxt, 0);
		} else {
			if (off + p->len <= RESP)
				memset(got + off, 1, p->len);
			while (p_rcv < RESP && got[p_rcv])
				p_rcv++;
			if (p_rcv > off + p->len || ++p_unacked >= 2)
				peer_emit(TCP_ACK, s_nxt, 0);
			else if (!p_delack)
				p_delack = now_us + 40000;
		}
	}

	if ((p->flags & TCP_FIN) && p->seq + p->len == dev_iss + 1 + p_rcv &&
	    p_rcv == RESP && !p_fin_in) {
		p_fin_in = 1;
		peer_emit(TCP_ACK, s_nxt, 0);
	}
	if (p_fin_out && p->ack == PEER_ISS + 1 + UP + 1)
		p_done = 1;
}

int main(int argc, char **argv)
{
	struct pkt syn = { 0 };
	unsigned long long limit = 4000ULL * 1000 * 1000;
	u32 i, have;

	UP = argc > 1 ? strtoul(argv[1], NULL, 0) : 1 << 20;
	RESP = argc > 2 ? strtoul(argv[2], NULL, 0) : 9;
	LOSS = argc > 3 ? atof(argv[3]) : 0;
	ACKLOSS = argc > 4 ? atof(argv[4]) : 0;
	srand48(argc > 5 ? atol(argv[5]) : 1);
	EARLY = argc > 6 ? atoi(argv[6]) : 0;
	got = calloc(RESP + 1, 1);
	dev_got = calloc(UP + 1, 1);
	peer_ip.s_addr = htonl(0x0a000002);

	tcp_init();
	tcp_stream_set_on_create_handler(app_create);

	syn.to_dev = 1;
	syn.flags = TCP_SYN;
	syn.seq = PEER_ISS;
	syn.win = 65535;
	enqueue(&syn);

	/*
	 * The device has no TIME_WAIT: once it is closed, a lost last ACK
	 * would leave the peer resending its FIN into nothing.  Its being
	 * closed after our FIN is the end of the exchange.
	 */
	while (now_us < limit && p_done == 0 && !(dev_closed && p_fin_out)) {
		for (;;) {
			int best = -1, k;

			for (k = 0; k < qn; k++)
				if (q[k].at <= now_us &&
				    (best < 0 || q[k].at < q[best].at))
					best = k;
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
			peer_emit(TCP_ACK, s_nxt, 0);
		if (established && s_una < s_nxt && now_us - s_last > PRTO) {
			s_nxt = s_una;		/* peer RTO: go back */
			s_last = now_us;
		}
		/* peer FIN lost: resend it */
		if (p_fin_out && !p_done && now_us - s_last > PRTO) {
			peer_emit(TCP_ACK | TCP_FIN, UP, 0);
			s_last = now_us;
		}
		peer_send_more();
		tcp_streams_poll();
		now_us += 5;
	}
	for (int n = 0; n < 200000 && !dev_closed; n++) {
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

	for (i = 0, have = 0; i < UP; i++)
		have += dev_got[i];

	printf("%s: up %u/%u in %.3f s, resp %u/%u, acted %d, up_err %lu, "
	       "resp_err %lu, peer resent %.2f%%, dev %s (status %d)\n",
	       p_done == 1 || (dev_closed && p_fin_out) ? "OK" : "FAIL", have, UP, now_us / 1e6, p_rcv,
	       RESP, acted, up_errors, resp_errors,
	       UP ? 100.0 * (up_sent - UP) / UP : 0,
	       dev_closed ? "closed" : "OPEN", dev_status);

	return !((p_done == 1 || (dev_closed && p_fin_out)) && have == UP && p_rcv == RESP && acted == 1 &&
		 !up_errors && !resp_errors && dev_closed && dev_status == 0);
}
