/*
 *	sample source
 */
#include <stdio.h>
#include "transport.h"

#define	DATASIZE	1024
#define PKTTYPE_DATA	1

struct pkt {
	unsigned short	pkt_type;		/* data or ack */
	unsigned short	pkt_len;		/* payload length */
	unsigned int	pkt_seqnum;		/* sequence number */
	char		pkt_data[DATASIZE];	/* data area */
};

#define HEADERLEN	(sizeof(struct pkt) - DATASIZE)

static int elapsed_time = 0;		/* elapsed time (ms) */
					/* incremented in 10ms interval */

/*
 * void
 * sender(int window, int tmeout)
 *	int window:	window size (not used in this sample)
 *	int timeout:	timeout value (not used in this sample)
 */
void
sender(int window, int timeout)
{
	int cnt, ret;
	struct pkt packet;
#ifdef DEBUG
	int txseq = 0;
#endif

	packet.pkt_type = PKTTYPE_DATA;
	while ((cnt = get_data(packet.pkt_data, DATASIZE)) != NET_EOF) {
#ifdef DEBUG
		packet.pkt_seqnum = txseq++;
		packet.pkt_len = cnt;
#endif
		if ((ret = udt_send(&packet, cnt+HEADERLEN)) != NET_SUCCESS) {
			switch (ret) {
			case NET_TOOBIG:
				fprintf(stderr, "sender: NET_TOOBIG\n");
				exit(1);
			case NET_SYSERR:
				fprintf(stderr, "sender: NET_SYSERR\n");
				exit(1);
			default:
				fprintf(stderr, "sender: unknown\n");
				exit(1);
			}
		}
	}
}

/*
 * void
 * receiver()
 */
void
receiver()
{
	int cnt;
	static int rxseq = 0;
	struct pkt packet;

	while ((cnt = udt_recv(&packet, sizeof packet, -1)) != NET_EOF) {
		if (cnt == 0)
			continue;
		else if (cnt == NET_SYSERR) {
			fprintf(stderr, "receiver: NET_SYSERR\n");
			exit(1);
		} else if (cnt < 0) {
			fprintf(stderr, "receiver: unknown error\n");
			exit(1);
		}

		if (rxseq < packet.pkt_seqnum) {
			printf("lost:");
			while (rxseq < packet.pkt_seqnum)
				printf(" %d", rxseq++);
			printf("\n");
		}
		rxseq = packet.pkt_seqnum + 1;
		deliver_data(packet.pkt_data, packet.pkt_len);
	}
}

/* called by timer per 10ms */
void
timer_handler()
{
	elapsed_time += 10;
}
