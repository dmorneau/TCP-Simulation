/*
  Go-back-N transmission example.

  Dominic Morneau 30/06/2010
*/
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "transport.h"

#define	DATASIZE	1024
#define HEADERSIZE  (sizeof(Packet) - DATASIZE)
#define ACKSIZE     sizeof("ACK")

/* Declarations, to remove warnings */
int get_data(void*,int);
int deliver_data(void*, int);
int udt_recv(void*,int,int);

/* Positive modulo (n % b); eg. -1 PMOD 32 will return 31 */
int PMOD(int n, int b) {
	 int mod = n % b;
	 return mod >= 0 ? mod : mod + b;
}

/*
  A packet. The header is made of
  - sequence number
  - size of buffer (filled with valid data)
  This is followed by the data.
*/
typedef struct {
	 int seqn;
	 int nbuffer;
	 char buffer[DATASIZE];
} Packet;

/*  ACK packet. Contains the sequence number and a tiny header ("ACK"). */
typedef struct {
	 char code[ACKSIZE];
	 int  seqn;
} ACKPacket;

/*  A very simple circular FIFO queue for packets. Allocates memory only on
	initialization with pqueue_init, frees it in pqueue_destroy.
	In order to avoid useless copying, instead of taking a Packet argument, 
	pqueue_push returns a pointer to the new packet. Use this pointer to edit 
	the packet in-place. Assume no initialization of the Packet data.

	Push --> [TAIL...HEAD] --> Pop
	We keep the head index, and the length of the queue to compute the tail.
*/
typedef struct {
	 int head;
	 int length;
	 int maxsize;
	 Packet* packets;
} PQueue;

void pqueue_init(PQueue* queue, int windowsize) {
	 queue->head = 0;
	 queue->length = 0;
	 queue->maxsize = windowsize;
	 queue->packets = malloc(sizeof(Packet) * queue->maxsize);
}
void pqueue_destroy(PQueue* queue) {
	 free(queue->packets);
}
int pqueue_length(PQueue* queue) {
	 return queue->length;
}
Packet* pqueue_tail(PQueue* queue) {
	 int i = (queue->head + queue->length - 1) % queue->maxsize;
	 assert (queue->head <= queue->maxsize && queue->head >= 0);
	 assert (queue->length <= queue->maxsize);
	 return &queue->packets[i];
}
Packet* pqueue_head(PQueue* queue) {
	 assert (queue->head <= queue->maxsize && queue->head >= 0);
	 assert (queue->length <= queue->maxsize);
	 return &queue->packets[queue->head];
}
Packet* pqueue_push(PQueue* queue) {
	 assert (pqueue_length(queue) < queue->maxsize);
	 queue->length += 1;
	 return pqueue_tail(queue);
}
Packet* pqueue_pop(PQueue* queue) {
	 Packet* tmp = pqueue_head(queue);
	 assert (pqueue_length(queue) > 0);
	 queue->head++;
	 queue->length--;
	 queue->head = PMOD(queue->head, queue->maxsize);
	 return tmp;
}
Packet* pqueue_poptail(PQueue* queue) {
	 Packet* tmp = pqueue_tail(queue);
	 assert (pqueue_length(queue) > 0);
	 queue->length -= 1;
	 return tmp;
}
bool pqueue_empty(PQueue* queue) {
	 return queue->length == 0;
}
/* Applies a function to every packet in the queue */
void pqueue_map(PQueue* queue, void (*fn)(Packet*) ) {
	 int i = queue->head;
	 int last = (queue->head + queue->length - 1) % queue->maxsize;
	 if (pqueue_empty(queue)) 
		  return;
	 while( i != last ) {
		  fn(&queue->packets[i]);
		  i = PMOD(i+1, queue->maxsize);
	 }
	 if (pqueue_length(queue) > 1)
		  fn(&queue->packets[last]);
}
void pqueue_debug_print(PQueue* queue) {
	 if (pqueue_length(queue) > 0) {
		  printf("Queue head seq#%d, tail seq#%d, size %d, window size %d\n",
				 pqueue_head(queue)->seqn, pqueue_tail(queue)->seqn,
				 pqueue_length(queue), queue->maxsize);
	 } else {
		  printf("Empty queue, window size %d\n", queue->maxsize);
	 }
}

/* Sends a single packet via udt_send */
void send_packet(Packet* packet) {
	 int ret;
	 int packet_size = HEADERSIZE + packet->nbuffer;
	 assert(packet_size > HEADERSIZE);

	 if ((ret = udt_send(packet, packet_size)) != NET_SUCCESS) {
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

/* Attempts to get a packet from the upper layer and add it to the queue.
   Return &packet if the packet was added, NULL if there is no more data. */
Packet* add_packet(PQueue* queue, int seqn) {
	 Packet* packet = pqueue_push(queue);
	 int cnt = get_data(packet->buffer, DATASIZE);

	 /* If the newly pushed packet could be filled with data,
	    we fill in the header. Otherwise we pop it back out and give up. */
	 if (cnt != NET_EOF) {
		  assert(cnt > 0);
		  packet->nbuffer = cnt;
		  packet->seqn = seqn;
		  return packet;
	 } else {
		  pqueue_poptail(queue);
		  return NULL;
	 }
}

/* Attempts to get an ack. Timeout can be -1 (infinite) or any value >= 0.
   Returns -1 in case of timeout, otherwise the ACK sequence number. */
int get_ack(int timeout) {
	 ACKPacket ack;
	 int ret = udt_recv(&ack, sizeof(ACKPacket), timeout);
	 if (ret == NET_EOF) {
		  fprintf(stderr, "Sender: NET_EOF\n");
		  exit(1);
	 } else if (ret == NET_SYSERR) {
		  fprintf(stderr, "Sender: NET_SYSERR\n");
		  exit(1);
	 }
	 return ret == 0 ? -1 : ack.seqn;
}


/* Timer, in milliseconds */
int cnt_time = 0;
int cnt_timeout = 0;
bool cnt_active = false;
void start_timer(int timeout) {
	 cnt_timeout = timeout;
	 cnt_time = 0;
	 cnt_active = true;
}
void stop_timer() {
	 cnt_active = false;
}

/* Main sender function. Taken from the state diagram on slide 6, chapter 5 */
void sender(int window, int timeout) {
	 int base = 1;
	 int nextseqnum = 1;
	 bool allsent = false;
	 PQueue sendQ;
	 pqueue_init(&sendQ, window);

	 while ( !(allsent && pqueue_empty(&sendQ)) ) {
		  int acknum = -1;

		  /* Send new data */
		  if (!allsent && nextseqnum < base + window) {
			   Packet* packet = add_packet(&sendQ, nextseqnum);
			   if (packet == NULL) {
					allsent = true;
			   } else {
					send_packet(packet);
					if (base == nextseqnum)
						 start_timer(timeout);
					nextseqnum++;
			   }
		  }
		  
		  /* Attempt to receive an ACK. */
		  acknum = get_ack(0);
		  if (acknum > 0) {
			   base = acknum + 1;
			   if (base == nextseqnum)
					stop_timer();
			   else
					start_timer(timeout);
		  }

		  /* Clean up the queue */
		  while (!pqueue_empty(&sendQ) && pqueue_head(&sendQ)->seqn < base) {
			   pqueue_pop(&sendQ);
		  }
		  
		  /* Handle timeouts */
		  if (cnt_active && cnt_time >= cnt_timeout) {
			   start_timer(cnt_timeout);
			   pqueue_map(&sendQ, &send_packet);
		  }
		  pqueue_debug_print(&sendQ);
	 }
	 
	 pqueue_destroy(&sendQ);
}

/* Sends an ACK signal back to the sender. */
void receiver_acknowledge(int seqn) {
	 int ret;
	 ACKPacket ack = {"ACK", 0};
	 ack.seqn = seqn;
	 ret = udt_send(&ack, sizeof(ACKPacket));
	 if (ret != NET_SUCCESS) {
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

/* Main receiver function. State diagram: slide 8, chapter 5 */
void receiver() {
	 int ret;
	 int expected = 1;
	 Packet packet;

	 /* Try to receive a packet, check for network errors */
	 while (1) { 
		  ret = udt_recv(&packet, sizeof(packet), -1);
		  if (ret == NET_EOF)
			   break;
		  else if (ret == NET_SYSERR) {
			   fprintf(stderr, "Receiver: NET_SYSERR\n");
			   exit(1);
		  }
		  
		  /* At this point we have a valid packet. Check the sequence number. */
		  assert (ret == HEADERSIZE + packet.nbuffer);
		  /* printf("Receiver: Received packet #%d. ", packet.seqn); */
		  if (packet.seqn == expected) {
			   deliver_data(packet.buffer, packet.nbuffer);
			   receiver_acknowledge(expected);
			   expected++;
		  } else {
			   receiver_acknowledge(expected - 1);
		  }
	 }
}

/* called by timer per 10ms */
void timer_handler() {
	 cnt_time = cnt_time + 10;
}
