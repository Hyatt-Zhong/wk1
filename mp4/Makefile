cc = gcc

TARGETS = main test

all:$(TARGETS)

LIBS = source/fmp4-reader.o\
source/fmp4-writer.o\
source/mov-avc1.o\
source/mov-dinf.o\
source/mov-elst.o\
source/mov-esds.o\
source/mov-ftyp.o\
source/mov-hdlr.o\
source/mov-hdr.o\
source/mov-iods.o\
source/mov-leva.o\
source/mov-mdhd.o\
source/mov-mehd.o\
source/mov-mfhd.o\
source/mov-minf.o\
source/mov-mvhd.o\
source/mov-opus.o\
source/mov-reader.o\
source/mov-sidx.o\
source/mov-stco.o\
source/mov-stsc.o\
source/mov-stsd.o\
source/mov-stss.o\
source/mov-stsz.o\
source/mov-stts.o\
source/mov-tag.o\
source/mov-tfdt.o\
source/mov-tfhd.o\
source/mov-tfra.o\
source/mov-tkhd.o\
source/mov-track.o\
source/mov-trex.o\
source/mov-trun.o\
source/mov-tx3g.o\
source/mov-udta.o\
source/mov-vpcc.o\
source/mov-writer.o\
avc/mpeg4-annexbtomp4.o\
avc/mpeg4-avc.o\
avc/mpeg4-mp4toannexb.o

INC = -Iavc -Iinclude

%.o:%.c
	$(cc) -c $^ $(INC) -o $@ -g


main:main.o $(LIBS)
	$(cc) $^ -o $@ -g

test:test.o $(LIBS)
	$(cc) $^ -o $@ -g

clean:
	clear
	rm -rf $(LIBS) *.o $(TARGETS) *.ps