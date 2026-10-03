# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
$(error PS5_PAYLOAD_SDK is undefined)
endif
slopfin-update.elf: main.c ../../src/update/transaction.h ../../src/update/files.h ../../src/update/sha256.h
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -o $@ main.c -lSceSystemService -lSceUserService
clean:
	rm -f slopfin-update.elf
