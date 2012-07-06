obj-m := proxy.o

path := $(shell uname -r)

all:
	g++ -O3 -Wall -g -o dragon-proxy dragon-proxy.cpp -export-dynamic -lpthread -march=core2 -std=c++0x
	
