.SUFFIXES:

CXX=clang++
CXXFLAGS=-g -pthread -std=c++17
CC=clang
CFLAGS=-g -pthread
BINS=server
OBJS=server.o myqueue.o

all: $(BINS)

server: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

server.o: server.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $

myqueue.o: myqueue.c
	$(CC) $(CFLAGS) -c -o $@ $

clean:
	rm -rf *.dSYM $(BINS) $(OBJS)