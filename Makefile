
.PHONY: all

all: publisher subscriber

publisher: publisher.o
	gcc -g -Wall -lpthread -lmosquitto -o publisher publisher.o

publisher.o: publisher.c
	gcc -g -Wall -c publisher.c

subscriber: subscriber.o
	gcc -g -Wall -lpthread -lmosquitto -o subscriber subscriber.o

subscriber.o: subscriber.c
	gcc -g -Wall -c subscriber.c

clean:
	rm -f publisher.o publisher subscriber.o subscriber
