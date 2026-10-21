# Copyright (C) 2026 Brett
# SPDX-License-Identifier: GPL-3.0-or-later
ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
$(error PS5_PAYLOAD_SDK is undefined)
endif
PPR ?= ../../.deps/fpkg/ppr-patch
slopfin-a53-selector.elf: selector.c
	$(CC) $(CFLAGS) -Wall -Wextra -Werror -O2 -std=c17 -I$(PPR) -o $@ selector.c \
		$(PPR)/a53_transport.c $(PPR)/notify.c $(PPR)/ppr_patch.c
clean:
	rm -f slopfin-a53-selector.elf
