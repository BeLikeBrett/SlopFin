# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
$(error PS5_PAYLOAD_SDK is undefined)
endif
slopfin-pkg-update.elf: main.c $(wildcard ../../src/update/*.h)
	$(CC) $(CFLAGS) -Wall -Wextra -Werror -O2 -o $@ main.c -lSceSystemService -lSceUserService -lSceIpmi -lSceAppInstUtil -lpthread
clean:
	rm -f slopfin-pkg-update.elf
