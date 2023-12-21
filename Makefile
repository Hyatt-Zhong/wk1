
#├─main.c
#├─bar.c
#├─foo.c

#==> main


cc=gcc

SRC := $(wildcard *.c)
OBJ = $(patsubst %.c,%.o,$(SRC))

APP = main
ALL = *.o *.a $(APP)

all: $(APP)

%.o:%.c
	$(cc) -c $^ -o $@

%.a: %.o
	ar r $@ $^

$(APP): $(OBJ)
	$(cc) $^ -o $@

clean:
	rm -rf $(ALL)


#├─main.c
#├─xxx.c
#├─yyy.c

#==> test

# cc=gcc

# all: test

# %.o:%.c
# 	$(cc) -c $^ -o $@

# %.a: %.o
# 	ar r $@ $^

# test: libxxx.a libyyy.a main.o
# 	$(cc) main.o -L. -lxxx -lyyy -o $@

# clean:
# 	rm -rf *.o *.a test