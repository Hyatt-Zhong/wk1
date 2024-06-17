OBJS_mp4 += \
source/fmp4-reader.c\
source/fmp4-writer.c\
source/mov-avc1.c\
source/mov-dinf.c\
source/mov-elst.c\
source/mov-esds.c\
source/mov-ftyp.c\
source/mov-hdlr.c\
source/mov-hdr.c\
source/mov-iods.c\
source/mov-leva.c\
source/mov-mdhd.c\
source/mov-mehd.c\
source/mov-mfhd.c\
source/mov-minf.c\
source/mov-mvhd.c\
source/mov-opus.c\
source/mov-reader.c\
source/mov-sidx.c\
source/mov-stco.c\
source/mov-stsc.c\
source/mov-stsd.c\
source/mov-stss.c\
source/mov-stsz.c\
source/mov-stts.c\
source/mov-tag.c\
source/mov-tfdt.c\
source/mov-tfhd.c\
source/mov-tfra.c\
source/mov-tkhd.c\
source/mov-track.c\
source/mov-trex.c\
source/mov-trun.c\
source/mov-tx3g.c\
source/mov-udta.c\
source/mov-vpcc.c\
source/mov-writer.c\
avc/mpeg4-annexbtomp4.c\
avc/mpeg4-avc.c\
avc/mpeg4-mp4toannexb.c\
mp4-io.c\
mp4-mutex.c\
mp4.c


OBJS_libmp4 = $(addprefix libmp4/, $(OBJS_mp4))

EXINC += -I$(ZRT_TOP)/src/function/libmp4/include\
		 -I$(ZRT_TOP)/src/function/libmp4/source\
		 -I$(ZRT_TOP)/src/function/libmp4/avc\
		 -I$(ZRT_TOP)/src/function/libmp4