/*
 *	transport.h
 */

/*
 *	return value of udt_send() and udt_recv()
 */
#define	NET_SUCCESS	0	/* successful */
#define	NET_EOF		-1	/* EOF */
#define	NET_TOOBIG	-2	/* message is too big */
#define	NET_SYSERR	-3	/* system call error */

#define	MTU		1500	/* max transmission unit */
#define	WINDOWSIZE	32	/* window size */

#define TIMER_TICK	10	/* 10 msec */

int udt_send(void *, int);	/* send function */
int udt_recv(void *, int, int);	/* receive function */

void sender(int, int);		/* sender function written by student */
