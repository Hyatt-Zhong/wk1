
cc=gcc

all: test

%.o:%.c
	$(cc) -c $^ -o $@

%.a: %.o
	ar r $@ $^

test: libxxx.a libyyy.a main.o
	$(cc) main.o -L. -lxxx -lyyy -o $@

clean:
	rm -rf *.o *.a test