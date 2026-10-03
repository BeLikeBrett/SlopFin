# SlopFin - build the launch payload sent over elfldr.
# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
PS5_HOST ?=
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := slopfin-launch.elf
LDADD := -lSceSystemService -lSceUserService

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDADD)

clean:
	rm -f $(ELF)
