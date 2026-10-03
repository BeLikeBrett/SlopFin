/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "update/transaction.h"
int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    sf_update_transaction *t = calloc(1, sizeof(*t));
    if (!t || sf_update_prepare(t, argv[1], argv[2], argv[3]) < 0)
    {
        free(t);
        return 3;
    }
    int result = sf_update_apply(t, atoi(argv[4]));
    free(t);
    return result == 0 ? 0 : result == -1 ? 4 : 5;
}
