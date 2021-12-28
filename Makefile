all:
	g++ -Wall -O3 -march=native -std=c++17 spsc_queue.cc -lpthread -o spsc_queue

clean:
	rm spsc_queue
