obj-m := proxy.o

path := $(shell uname -r)

all:
	g++ -O2 -Wall -g -o dragon-proxy dragon-proxy.cpp -export-dynamic -lpthread -std=c++0x
	
