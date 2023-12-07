#compiler = g++

TOOLCHAIN_DIR=/home/mars/workspace/code/qzsdk/cross_compiler/toolchain
CROSS_COMPILE=$(TOOLCHAIN_DIR)/bin/arm-openwrt-linux-
CC=$(CROSS_COMPILE)gcc
CXX=$(CROSS_COMPILE)g++
STRIP=$(CROSS_COMPILE)strip
OBJCOPY=$(CROSS_COMPILE)objcopy

compiler = $(CXX)

SRC := $(wildcard *.cc)
OBJ = $(patsubst %.cc,%.o,$(SRC))
ALL = $(OBJ) tcp_client x.dbg x_with_dbg

all:first app

first:
	@for var in $(SRC); do \
			$(compiler) -g -c $$var; \
    	done

app:
	$(compiler) $(OBJ) -o tcp_client
	cp tcp_client x_with_dbg
	$(OBJCOPY) --only-keep-debug tcp_client x.dbg
	$(OBJCOPY) --strip-debug --strip-unneeded tcp_client

clean:
	-rm -rf $(ALL)
