/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Smallest working memory AltSql DB opens with, for three page sizes, and the size of a cursor. */
#include "dbtest.h"
int main(void) {
    uint32_t pss[] = { 512, 4096, 65536 }; int i;
    printf("cursor %zu bytes, handle %zu bytes\n", sizeof(altsql_db_cursor), sizeof(altsql_db));
    for (i = 0; i < 3; i++) {
        size_t lo = 1024, hi = 8u << 20;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2; ramfile rf; altsql_db *db; void *mem = malloc(mid); int rc;
            ram_new(&rf, 1 << 20, 1 << 20);
            rc = db_open_ram(&db, &rf, mem, mid, pss[i]);
            ram_free(&rf); free(mem);
            if (rc == ALTSQL_OK) hi = mid; else lo = mid + 1;
        }
        printf("page %u: smallest working memory that opens: %zu bytes\n", pss[i], lo);
    }
    return 0;
}
