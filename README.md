Transport layer simulation
==========================

This program simulates transmitting data via TCP through a network
with a given rate of packet loss. I implemented two algorithms for
ARQ, sw.c (Stop-and-wait), and gbn.c (Go-back-N). The latter includes
a custom implementation of a circular queue for storing packets.
