SAMPLEPROG=	sample
SWPROG=		sw
GBNPROG=	gbn
SAMPLEOBJS=	main.o sample.o
SWOBJS=		main.o sw.o
GBNOBJS=	main.o gbn.o
CC=		gcc

PROGS=		$(SAMPLEPROG) $(SWPROG) $(GBNPROG)

CFLAGS=	-O -Wall -pedantic
#CFLAGS=	-O -g -Wall -Werror

all: $(SAMPLEPROG) $(SWPROG) $(GBNPROG)

$(SAMPLEPROG): $(SAMPLEOBJS) $(LIBS)
	$(CC) $(CFLAGS) -o $(SAMPLEPROG) $(SAMPLEOBJS)

$(SWPROG): $(SWOBJS) $(LIBS)
	$(CC) $(CFLAGS) -o $(SWPROG) $(SWOBJS)

$(GBNPROG): $(GBNOBJS) $(LIBS)
	$(CC) $(CFLAGS) -o $(GBNPROG) $(GBNOBJS)

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c $*.c

clean:
	rm -f $(PROGS) *.o core *.core *.bak *_r *~
