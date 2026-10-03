# SlopFin dev tooling - HDMI audio bitstream experiments.
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

LIBS := -lSceAudioOut -lSceRegMgr -lSceUserService

probe.elf: probe.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# make play-MODE-DEVICE.elf, e.g. play-0-255.elf; CHANNELS/GRAIN for 192 kHz types
play-%.elf: play.c
	$(CC) $(CFLAGS) -DMODE=$(word 1,$(subst -, ,$*)) -DDEVICE=$(word 2,$(subst -, ,$*)) \
	    -DCHANNELS=$(or $(CHANNELS),2) -DGRAIN=$(or $(GRAIN),256) -o $@ $^ $(LIBS)

clean:
	rm -f *.elf
