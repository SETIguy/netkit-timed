all: libtimed.a

include ../../MCONFIG
include ../../MRULES

CFLAGS += -I../timed
OBJS = byteorder.o measure.o cksum.o

libtimed.a: $(OBJS)
	ar -cruv $@ $^

install: ;
clean:
	rm -f *.o *.a

