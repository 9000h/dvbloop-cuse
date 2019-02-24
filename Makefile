all: dvbloopd

dvbloopd: dvbloopd.o dvbcuse.o
	gcc -Wall -s -o dvbloopd dvbloopd.o dvbcuse.o `pkg-config fuse --libs`

dvbloopd.o: dvbloopd.c dvbcuse.h
	gcc -Wall -O3 -c dvbloopd.c

dvbcuse.o: dvbcuse.c dvbcuse.h
	gcc -Wall `pkg-config fuse --cflags` -c dvbcuse.c

clean:
	rm -f dvbloopd *.o
