/*
  Stop-and-wait transmission example.

  Dominic Morneau 30/06/2010
*/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h> /* for exit() */
#include <assert.h>
#include "transport.h"

#define	DATASIZE	1024
#define HEADERSIZE  (sizeof(Packet) - DATASIZE)
#define ACKSIZE     sizeof("ACK")

/* Declarations, to remove warnings */
int get_data(void*,int);
int deliver_data(void*, int);
int udt_recv(void*,int,int);

/* A packet. The header is made of
   - sequence number
   - size of buffer (filled with valid data)
   This is followed by the data. */
typedef struct {
	 int seqn;
	 int nbuffer;
	 char buffer[DATASIZE];
} Packet;

/* ACK packet. Contains the sequence number and a tiny header. */
typedef struct {
	 char code[ACKSIZE];
	 int  seqn;
} ACKPacket;

/* A session_sender is the state maintained by a sender.
   It contains
   - state, indicating what to do next.
   - Number of packets sent so far (ie. sequence number)
   - A packet, used as the sending buffer */
typedef struct {
	 int  state;
	 int  nsent;
	 Packet packet;
} Session_sender;

#define SEND_GETDATA    1
#define SEND_WAITACK    2
#define SEND_SENDPACKET 3
#define SEND_COMPLETE   4

/* Sends a packet.
   Assumes that the data in the session buffer has already been obtained
   from the upper layer. */
void sender_send_packet(Session_sender* session) {
	 session->packet.seqn = session->nsent;

	 switch (udt_send(&session->packet, HEADERSIZE + session->packet.nbuffer)) {
	 case NET_SUCCESS:
		  session->state = SEND_WAITACK;
		  break;
	 case NET_TOOBIG:
		  fprintf(stderr, "sender: NET_TOOBIG\b");
		  exit(1);
	 case NET_SYSERR:
		  fprintf(stderr, "sender: NET_SYSERR\n");
		  exit(1);
	 default:
		  fprintf(stderr, "sender: unknown\n");
		  exit(1);
	 }	 
}

/* Obtains data from the upper layer. */
void sender_getdata(Session_sender* session) {
	 int count = get_data(&session->packet.buffer, DATASIZE);
	 
	 if (count != NET_EOF) {
		  session->state = SEND_SENDPACKET;
		  session->packet.nbuffer = count;
	 } else {
		  session->state = SEND_COMPLETE;
		  session->packet.nbuffer = 0;
	 }
}

/* Waits for an ack. */
void sender_waitack(Session_sender* session, int timeout) {
	 ACKPacket ack;
	 int ret = udt_recv(&ack, sizeof(ACKPacket), timeout);
	 
	 if (ret == NET_EOF) {
		  fprintf(stderr, "Sender: NET_EOF\n");
		  exit(1);
	 } else if (ret == NET_SYSERR) {
		  fprintf(stderr, "Sender: NET_SYSERR\n");
		  exit(1);
	 } else if (ret == 0) { /* ACK timeout: resend the packet */
		  session->state = SEND_SENDPACKET;
	 } else {
	   	  assert (ret == sizeof(ACKPacket));
		  assert (ack.seqn <= session->packet.seqn);
		  
		  if (ack.seqn == session->packet.seqn) {
			   session->state = SEND_GETDATA;
			   session->packet.nbuffer = 0;
			   session->nsent += 1;
		  } else {
			   session->state = SEND_SENDPACKET;
		  }
	 }
}

/* Main sender function. Window size is unused. */
void sender(int window, int timeout) {
	 Session_sender session = {SEND_GETDATA, 0};
	 session.packet.seqn = 0;
	 while (session.state != SEND_COMPLETE) {
		  switch (session.state) {
		  case SEND_GETDATA:
			   sender_getdata(&session);
			   break;
		  case SEND_SENDPACKET:
			   sender_send_packet(&session);
			   break;
		  case SEND_WAITACK:
			   sender_waitack(&session, timeout);
			   break;
		  default:
			   fprintf(stderr, "Sender: invalid state.");
			   exit(1);
		  }
	 }
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

/* Main receiver function. */
void receiver()
{
	 int ret = 0, rxseq = 0;
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
		  assert (ret == sizeof(packet));
		  receiver_acknowledge(packet.seqn);
		  if (packet.seqn > rxseq)
			   printf("Receiver: Error: did not receive #%d\n", rxseq);
		  else if (packet.seqn < rxseq)
			   printf("Receiver: Received packet #%d again\n", packet.seqn);
		  else {
			   printf("Receiver: Received packet #%d\n", packet.seqn);
			   rxseq++;
			   deliver_data(packet.buffer, packet.nbuffer);
		  }
	 }
}

/* called by timer per 10ms */
void timer_handler() {
	 /* NOP */;
}
