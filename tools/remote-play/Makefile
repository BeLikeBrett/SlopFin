# SlopFin dev tooling - headless Remote Play pairing payload.
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

pair.elf: pair.c
	$(CC) $(CFLAGS) -o $@ $^ -lSceRegMgr -lSceUserService -lSceRemoteplay

clean:
	rm -f pair.elf
