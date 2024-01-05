cc = gcc

TARGET0 = pstest

TARGETS = $(TARGET0)

OBJS = psmux.o \
		utils.o \
		xaac.o \
		main.o

%.o:%.c
	$(cc) -c $^ -o $@

$(TARGET0):$(OBJS)
	$(cc) $^ -o $@

clean:
	clear
	rm -rf $(OBJS) *.o $(TARGETS) *.ps
