/*
 *	Network Engineering: home work
 *	main.c
 *
 *	last update: 2009/05/26 by tera
 */

#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/time.h>
#include "transport.h"

/*
 *	packet format	-- lower layer header + user data
 */
struct lowerpkt {
	int lp_type;			/* packet type */
	char lp_buf[MTU];		/* upper layer data */
};

/* lp_type */
#define	LP_USERDATA	0		/* user data */
#define LP_EOF		1		/* no more user data */

#define	LP_HEADERSIZE	4		/* header size */

/*
 *	packet buffer	-- one per packet
 */
struct pktbuf {
	struct pktbuf *pb_next;
	int pb_stat;			/* status */
	int pb_size;			/* data size */
	int pb_txtime;			/* time when this packet to be sent */
	struct lowerpkt pb_lowerpkt;	/* lower layer packet */
};

#define	PKT_ERR		0x0001		/* packet error */

/*
 *	line buffer	-- emulate communication channel
 */
struct linebuf {
	struct pktbuf *lbuf_head;	/* pktbuf head */
	struct pktbuf *lbuf_tail;	/* pktbuf tail */
	int lbuf_size;			/* buffered data size */
	int lbuf_stat;			/* status */
};

#define LBUF_FULL	0x0001		/* tx channel is full */

#define	ALARM_TICK	(10*1000)	/* 10,000 micro sec (10 msec) */
#define	ALARM_TICK_MS	10		/* 10 msec */

#define	WATCHDOG_TIMER	(5*60*1000)		/* (5 min) in msec */

static struct linebuf lbuf;

static int bw;		/* bandwidth: 1, 10, 100 (Mbps) */
static int delay;	/* delay: 10, 20, 50 (msec) */
static int bdp;		/* bandwidth-delay product (byte) */
static int erate;	/* error rate (0, 10, 100, 1,000, 10,000) */

static int elapsed_time = 0;	/* elapsed time (msec) */

#ifdef DEBUG
static int ppid;	/* pid of parent process (receiver) */
#endif

static int sock_s;	/* socket for tx */
static int sock_r;	/* socket for rx */
static int fd_s;	/* file for tx */
static int fd_r;	/* file for rx */

static sigset_t sigs;	/* sigset_t for SIGALRM */

static void print_help(char *);
static void send_pkt();
static void alarm_handler();
void timer_handler();

/*
 * syntax: sw file bandwidth delay error_rate
 * syntax: gbn file bandwidth delay error_rate
 *
 *	bandwidth: 1, 10, 100 (Mbps)
 *	delay:     10, 20, 50 (msec)
 *	error rate: 0, -4 (1*10^-4), -3 (1*10^-3), -2 (1*10^-2), -1 (1*10^-1)
 */
main(int argc, char *argv[])
{
	char *file_s;
	char file_r[256];
	int pid;
	int sender_stat;
	int sv1[2];
	int sv2[2];
	struct timeval tv;
	struct itimerval tt;
	time_t o_sec, n_sec, sec;
	long o_msec, n_msec, msec;
	struct tm *date;
	struct lowerpkt *lpp;

	if (argc != 5) {
		print_help(argv[0]);
		exit(1);
	}
	argv++;
	file_s = *argv++;
	bw = atoi(*argv++);
	delay = atoi(*argv++);
	erate = atoi(*argv++);

	/* check arguments */
	if (bw != 1 && bw != 10 && bw != 100) {
		print_help(argv[0]);
		exit(1);
	}
	if (delay != 10 && delay != 20 && delay != 50) {
		print_help(argv[0]);
		exit(1);
	}
	if (erate != 0 && erate != -4 && erate != -3 && erate != -2 &&
			erate != -1) {
		print_help(argv[0]);
		exit(1);
	}

	/* open source file */
	if((fd_s = open(file_s, O_RDONLY)) < 0) {
		fprintf(stderr, "source file `%s': ", file_s);
		perror("open");
		exit(1);
	}

	/* create destination file */
	strcpy(file_r, file_s);
	strcat(file_r, "_r");
	if ((fd_r = open(file_r, O_WRONLY|O_CREAT|O_TRUNC, 0644)) < 0) {
		fprintf(stderr, "destination file `%s': ", file_r);
		perror("open");
		exit(1);
	}

	/* setup several parameters */
	bdp = bw * delay * 1024/8;	/* bandwidth-delay product (byte) */
	if (erate == -1)
		erate = 10;		/* drop 1 pkt per 10 pkts */
	else if (erate == -2)
		erate = 100;		/* drop 1 pkt per 100 pkts */
	else if (erate == -3)
		erate = 1000;		/* drop 1 pkt per 1,000 pkts */
	else if (erate == -4)
		erate = 10000;		/* drop 1 pkt per 10,000 pkts */
	lbuf.lbuf_head = lbuf.lbuf_tail = NULL;
	lbuf.lbuf_stat = lbuf.lbuf_size = 0;

	/* setup communication channel between 2 processes */
	if (socketpair(PF_LOCAL, SOCK_DGRAM, 0, sv1) < 0) {
		perror("socketpair");
		exit(1);
	}
	if (socketpair(PF_LOCAL, SOCK_DGRAM, 0, sv2) < 0) {
		perror("socketpair");
		exit(1);
	}

	/* set signal handler */
	signal(SIGALRM, alarm_handler);
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGALRM);

#ifdef DEBUG
	ppid = getpid();
#endif
	/* fork to 2 processes */
	if ((pid = fork()) == 0) {	/* child process: sender */
		sock_s = sv1[1];
		sock_r = sv2[0];
		close(sv1[0]);
		close(sv2[1]);
		srandom(getpid());	/* set seed of random() */

		/* get start time */
		gettimeofday(&tv, NULL);
		o_sec = tv.tv_sec;
		o_msec = tv.tv_usec/1000;
		date = localtime(&o_sec);
	       	printf("start time\t: %02d:%02d:%02d.%03ld\n",
		date->tm_hour, date->tm_min, date->tm_sec, o_msec);
	
		tt.it_interval.tv_sec = 0;
		tt.it_interval.tv_usec = ALARM_TICK;
		tt.it_value.tv_sec = 0;
		tt.it_value.tv_usec = ALARM_TICK;
		if (setitimer(ITIMER_REAL, &tt, NULL) < 0) {
			perror("sender: setitimer");
			exit(1);
		}

		sender(WINDOWSIZE, delay*4);	/* call student's routine */
		close(fd_s);			/* close source file */

		/* wait for send buffer becomes empty */
		while (lbuf.lbuf_head) {
			pause();
		}

		/* stop interval timer */
		tt.it_interval.tv_sec = 0;
		tt.it_interval.tv_usec = 0;
		tt.it_value.tv_sec = 0;
		tt.it_value.tv_usec = 0;
		if (setitimer(ITIMER_REAL, &tt, NULL) < 0) {
			perror("sender: setitimer");
			exit(1);
		}

		/* send LP_EOF control packet */
		if ((lpp = (struct lowerpkt *)malloc(sizeof(struct lowerpkt)))
				== NULL) {
			perror("sender: malloc");
			exit(1);
		}
		lpp->lp_type = LP_EOF;
		if (write(sock_s, (char *)lpp, LP_HEADERSIZE) < 0) {
			perror("sender: write (LP_EOF)");
			exit(1);
		}

		/* close communication channel */
		close(sv1[1]);
		close(sv2[0]);
	
		/* get end time */
		gettimeofday(&tv, NULL);
		n_sec = tv.tv_sec;
		n_msec = tv.tv_usec/1000;
	
		/* print start time */
		date = localtime(&o_sec);
	       	printf("start time\t: %02d:%02d:%02d.%03ld\n",
		date->tm_hour, date->tm_min, date->tm_sec, o_msec);
	
		/* print end time */
		date = localtime(&n_sec);
	       	printf("  end time\t: %02d:%02d:%02d.%03ld\n",
		date->tm_hour, date->tm_min, date->tm_sec, n_msec);
	
		/* caclculate elapsed time */
		msec = n_msec - o_msec;
		sec = n_sec - o_sec;
		if (msec < 0) {
			sec--;
			msec += 1000;
		}
	
		/* print elapsed time */
		date = gmtime(&sec);
	       	printf("    result\t: %02d:%02d:%02d.%03ld\n",
		date->tm_hour, date->tm_min, date->tm_sec, msec);

		exit(0);
	} else {		/* parent process: receiver */
		sock_r = sv1[0];
		sock_s = sv2[1];
		close(sv1[1]);
		close(sv2[0]);
		srandom(getpid());	/* set seed of random() */

		tt.it_interval.tv_sec = 0;
		tt.it_interval.tv_usec = ALARM_TICK;
		tt.it_value.tv_sec = 0;
		tt.it_value.tv_usec = ALARM_TICK;
		if (setitimer(ITIMER_REAL, &tt, NULL) < 0) {
			perror("receiver: setitimer");
			exit(1);
		}

		receiver();		/* call student's routine */

		tt.it_interval.tv_sec = 0;
		tt.it_interval.tv_usec = 0;
		tt.it_value.tv_sec = 0;
		tt.it_value.tv_usec = 0;
		if (setitimer(ITIMER_REAL, &tt, NULL) < 0) {
			perror("receiver: setitimer");
			exit(1);
		}

		/* close destination file and communication channel */
		close(fd_r);
		close(sv1[0]);
		close(sv2[1]);

		wait(&sender_stat);
		exit(0);
	}
}

static void
print_help(char *command)
{
	printf("%s file bandwidth delay error_rate\n", command);
	printf("\tbandwidth: 1, 10, 100 (Mbps)\n");
	printf("\tdelay: 10, 20, 50 (msec)\n");
	printf("\terror rate: 0, -4 (1*10^-4), -3 (1*10^-3), -2 (1*10^-2), -1 (1*10^-1)\n");
}

/* ======================================================================
 *
 * subroutines for students
 */

/*
 * int
 * udt_send(void *buf, int size)
 *	char *buf;
 *	int size;	size must be less than 1500 (== MTU)
 *
 * return value:
 *	NET_SUCCESS	success
 *	NET_TOOBIG	message size is too big
 *	NET_SYSERR	system call error
 */
int
udt_send(void *buf, int size)
{
	int cnt;
	struct pktbuf *pbuf;
	struct lowerpkt *lpp;
	long rnd;

	if (size > MTU)
		return NET_TOOBIG;

	/* allocate packet buffer */
	if ((pbuf = (struct pktbuf *)malloc(sizeof(struct pktbuf))) == NULL) {
		perror("udt_send: malloc");
		return NET_SYSERR;
	}
	lpp = &pbuf->pb_lowerpkt;
	pbuf->pb_next = NULL;
	bcopy(buf, lpp->lp_buf, size);
	lpp->lp_type = LP_USERDATA;
	pbuf->pb_size = size + LP_HEADERSIZE;
	pbuf->pb_stat = 0;

	if (erate) {
		rnd = random();
		if ((rnd % erate) == 0) {
#ifdef DEBUG
			if (getpid() == ppid)
				fprintf(stderr, "** ACK LOSS **\n");
			else
				fprintf(stderr, "** DATA LOSS **\n");
#endif
			pbuf->pb_stat |= PKT_ERR;
		}
	}
	pbuf->pb_txtime = elapsed_time + delay;

	/* block SIGALRM */
	if (sigprocmask(SIG_BLOCK, &sigs, NULL) < 0) {
		perror("sigprocmask");
		return NET_SYSERR;
	}

	/* append packet buffer to line buffer */
	if (lbuf.lbuf_head == NULL) {	/* line buffer is empty */
		lbuf.lbuf_head = lbuf.lbuf_tail = pbuf;
		lbuf.lbuf_size = pbuf->pb_size;
	} else {			/* line buffer is not empty */
		lbuf.lbuf_tail->pb_next = pbuf;
		lbuf.lbuf_tail = pbuf;
		lbuf.lbuf_size += pbuf->pb_size;
retry:
		if (lbuf.lbuf_size >= bdp) {
			/* communication path is full! */
			if (sigprocmask(SIG_UNBLOCK, &sigs, NULL) < 0) {
				perror("sigprocmask");
				return NET_SYSERR;
			}
			lbuf.lbuf_stat |= LBUF_FULL;
#ifdef DEBUG0
			fprintf(stderr,
				"udt_send: comm. path full, goes to sleep\n");
#endif
			pause();
#ifdef DEBUG0
			fprintf(stderr, "udt_send: comm. path full, wakeup\n");
#endif
			goto retry;
		}
	}

	/* unblock SIGALRM */
	if (sigprocmask(SIG_UNBLOCK, &sigs, NULL) < 0) {
		perror("sigprocmask");
		return NET_SYSERR;
	}
	return NET_SUCCESS;
}

/*
 * int
 * udt_recv(void *buf, int size, int timeout)
 *	char *buf;
 *	int size;		buffer size
 *	int timeout;
 *		positive int:	timeout in msec
 *		0:		return immediately even if there is no data
 *		-1:		wait until data is received 
 *
 * return value:
 *	positive int:		received data size
 *	0			there is no data
 *	NET_EOF			EOF
 *	NET_SYSERR		system call error
 */
int
udt_recv(void *buf, int size, int timeout)
{
	int cnt;
	int nfd;
	long t;
	fd_set rdfds;
	struct lowerpkt lpkt;

	if (timeout == -1) {	/* wait until packet is received */
		if ((cnt = read(sock_r, &lpkt, size+LP_HEADERSIZE)) < 0) {
			if (errno == ECONNRESET)
				return NET_EOF;
			perror("udt_recv: read");
			return NET_SYSERR;
		}
		goto recvd;
	}

	FD_ZERO(&rdfds);
	FD_SET(sock_r, &rdfds);

again:
	if ((nfd = select(sock_r + 1, &rdfds, NULL, NULL, NULL)) < 0) {
		if (errno == EINTR) {	/* SIGALRM received */
			timeout -= ALARM_TICK_MS;
			if (timeout <= 0)
				return 0;
			goto again;
		} else {
			perror("udt_recv: select");
			return NET_SYSERR;
		}
	} else if (nfd > 0) {
		if (!FD_ISSET(sock_r, &rdfds))	/* not received */
			goto again;
		if ((cnt = read(sock_r, &lpkt, size+LP_HEADERSIZE)) < 0) {
			if (errno == ECONNRESET)
				return NET_EOF;

			perror("udt_recv: read");
			return NET_SYSERR;
		}
		goto recvd;
	} else {
		return 0;		/* timeout */
	}

recvd:
	if (lpkt.lp_type == LP_EOF)
		return NET_EOF;

	cnt -= LP_HEADERSIZE;
	bcopy(lpkt.lp_buf, buf, cnt);
	return cnt;
}

/*
 * int get_data(void *buf, int size)
 *
 * return value:
 *	NET_EOF:	end of file
 *	positive int:	size of data from upper layer
 */
int
get_data(void *buf, int size)
{
	int cnt;

	if ((cnt = read(fd_s, buf, size)) < 0) {
		perror("get_data: read");
		exit(1);
	}
	if (cnt == 0)
		return NET_EOF;
	else
		return cnt;
}

/*
 *	int deliver_data(void *buf, int size)
 */
int
deliver_data(void *buf, int size)
{
	int cnt;

	if ((cnt = write(fd_r, buf, size)) < 0) {
		perror("deliver_data: write");
		exit(1);
	}
	return cnt;
}

/*
 *	end of routines provided to students
 * ======================================================================
 */

/*
 *	signal handler routine -- called by SIGALRM
 */
static void
alarm_handler()
{
	static int watchdog = WATCHDOG_TIMER;	/* msec */

	watchdog -= ALARM_TICK_MS;
	if (watchdog < 0) {
		fprintf(stderr, "Watchdog timer expired!\n");
		exit(1);
	}

	send_pkt();
	elapsed_time += ALARM_TICK_MS;		/* current time (msec) */
	timer_handler();
}

/*
 *	send packet from line buffer -- called by alarm_handler
 */
static void
send_pkt()
{
	struct pktbuf *pb;

	while ((pb = lbuf.lbuf_head) != NULL &&
				pb->pb_txtime <= elapsed_time) {
retry:
		if (!(pb->pb_stat & PKT_ERR)) {
			if (write(sock_s, (char *)&pb->pb_lowerpkt,
							pb->pb_size) < 0) {
				if (errno == ENOBUFS) {
#ifdef DEBUG0
					fprintf(stderr,
						"send_pkt: no buf, sleep\n");
#endif
					usleep(1000);	/* wait 10 micro sec */
					goto retry;
				}
				if (errno == ENOTCONN || errno == ECONNREFUSED)
					return;
				perror("send_pkt: write");
				exit(1);
			}
		}
		lbuf.lbuf_size -= pb->pb_size;
		lbuf.lbuf_head = pb->pb_next;
		free(pb);

		if (lbuf.lbuf_head == NULL)
			lbuf.lbuf_tail = NULL;

		if (lbuf.lbuf_stat & LBUF_FULL)
			lbuf.lbuf_stat &= ~LBUF_FULL;
	}
}
