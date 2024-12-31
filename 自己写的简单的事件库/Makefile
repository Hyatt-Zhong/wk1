CC = /home/mars/workspace/code/qzsdk/cross_compiler/toolchain/bin/arm-openwrt-linux-gcc
CPPC = /home/mars/workspace/code/qzsdk/cross_compiler/toolchain/bin/arm-openwrt-linux-g++
# CC = gcc
# CPPC = g++

ARM_LINK = -llsquic -L../lib 
X86_LINK = -llsquic_x86 -L../lib 


INC = -I../include -I./event

all: 


simu_native:native_io.c
	g++ -c -g event/simple_event_array.cc -o event/simple_event_array.o
	gcc -c -g event/simple_event.c  -o event/simple_event.o
	gcc -c -g native_io.c $(INC) -o native_io.o
	g++ event/simple_event_array.o event/simple_event.o native_io.o   $(X86_LINK) -g -o simu_native


svr_native: native_svr.c
	g++ -c -g event/simple_event_array.cc -o event/simple_event_array.o
	gcc -c -g event/simple_event.c  -o event/simple_event.o
	gcc -c -g native_svr.c $(INC) -o native_svr.o
	g++ event/simple_event_array.o event/simple_event.o native_svr.o   $(X86_LINK) -g -o svr_native

