CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-result
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
USB_LIBS   := $(shell pkg-config --variable=libdir libusb-1.0)/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security
OBJS = main.o rt2570.o ap.o ieee80211.o netutil.o netbridge.o control.o log.o

all: wfcd

wfcd: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(USB_LIBS) -framework vmnet -lpthread

%.o: %.c *.h
	$(CC) $(CFLAGS) $(USB_CFLAGS) -c $< -o $@

hwtest: hwtest.o rt2570.o ieee80211.o log.o
	$(CC) $(CFLAGS) -o $@ $^ $(USB_LIBS)

ledtest: ledtest.o rt2570.o ieee80211.o log.o
	$(CC) $(CFLAGS) -o $@ $^ $(USB_LIBS)

tests: tests.o ap.o ieee80211.o netutil.o log.o
	$(CC) $(CFLAGS) -o $@ $^ -lpthread
	./tests

clean:
	rm -f *.o wfcd hwtest ledtest tests

.PHONY: all clean tests
