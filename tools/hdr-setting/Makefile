# SlopFin - VIDEOOUT registry payloads (read-only dump; SET_HDR variants write the HDR setting).
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

LDADD := -lSceRegMgr

all: videoout-read.elf avsetting.elf

avsetting.elf: avsetting.c
	$(CC) $(CFLAGS) -o $@ $^

videoout-read.elf: main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDADD)

videoout-set-hdr-%.elf: main.c
	$(CC) $(CFLAGS) -DSET_HDR=$* -o $@ $^ $(LDADD)

clean:
	rm -f *.elf
