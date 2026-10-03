# SlopFin - the sandbox payload SlopFin sends to elfldr for the picture picker.
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

slopfin-sandbox.elf: main.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f slopfin-sandbox.elf
