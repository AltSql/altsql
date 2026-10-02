/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Port: flash emulated in a file (POSIX), for gateways, tools and tests
 *
 * The file is memory-mapped and follows NOR rules like real flash: a
 * write can only clear bits, an erase sets a sector to 0xFF. With
 * sector_size 0, the geometry of an existing file is read from it.
 * Needs POSIX: build with -D_POSIX_C_SOURCE=200809L (or gnu99).
 * ====================================================================== */
#ifdef ALTSQL_PORT_FILE
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct as_filefl {
    int      fd;
    uint8_t *mem;
    size_t   size;
    uint32_t ss;
} as_filefl;

static int as_file_bad(const as_filefl *c, uint32_t addr, uint32_t len) {
    return addr > c->size || len > c->size - addr;
}

static int as_file_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    as_filefl *c = (as_filefl *)ctx;
    if (as_file_bad(c, addr, len)) return -1;
    memcpy(buf, c->mem + addr, len);
    return 0;
}

static const uint8_t *as_file_map(void *ctx, uint32_t addr, uint32_t len) {
    as_filefl *c = (as_filefl *)ctx;
    return as_file_bad(c, addr, len) ? NULL : c->mem + addr;
}

static int as_file_write(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
    as_filefl *c = (as_filefl *)ctx;
    const uint8_t *s = (const uint8_t *)buf;
    uint32_t i;
    if (as_file_bad(c, addr, len)) return -1;
    for (i = 0; i < len; i++) c->mem[addr + i] &= s[i];
    return 0;
}

static int as_file_erase(void *ctx, uint32_t sector) {
    as_filefl *c = (as_filefl *)ctx;
    if ((size_t)sector * c->ss >= c->size) return -1;
    memset(c->mem + (size_t)sector * c->ss, 0xFF, c->ss);
    return 0;
}

/* Finds sector size, count and write alignment from any valid sector header. */
static int as_file_detect(const uint8_t *m, size_t size, uint32_t *ss, uint32_t *sc, uint32_t *align) {
    uint32_t cand;
    for (cand = 256; cand && cand <= (1u << 24); cand <<= 1) {
        size_t k;
        if (size % cand) continue;
        for (k = 0; k < size / cand; k++) {
            const uint8_t *h = m + k * cand;
            if (as_hdr_valid(h) &&
                as_get32(h + 12) == cand && (size_t)as_get32(h + 16) * cand == size) {
                *ss = cand;
                *sc = as_get32(h + 16);
                *align = h[5];
                return 0;
            }
        }
    }
    return -1;
}

int altsql_file_flash_open(altsql_flash *f, const char *path, uint32_t sector_size,
                           uint32_t sector_count, uint32_t write_align, int create) {
    struct stat st;
    as_filefl *c;
    size_t size;
    uint8_t *m;
    int fd, fresh = 0;
    if (!f || !path) return -1;
    memset(f, 0, sizeof *f);
    fd = open(path, O_RDWR | (create ? O_CREAT : 0), 0644);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    if (st.st_size == 0) {
        if (!create || !sector_size || !sector_count) { close(fd); return -1; }
        size = (size_t)sector_size * sector_count;
        if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
        fresh = 1;
    } else {
        size = (size_t)st.st_size;
    }
    m = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    if (fresh) {
        memset(m, 0xFF, size);                        /* a new chip is erased */
    } else {
        uint32_t ss = 0, sc = 0, al = 0;
        if (as_file_detect(m, size, &ss, &sc, &al) == 0) {
            if ((sector_size && sector_size != ss) || (sector_count && sector_count != sc)) {
                munmap(m, size);
                close(fd);
                return -1;                            /* asked for another geometry */
            }
            sector_size = ss;
            sector_count = sc;
            if (!write_align) write_align = al;
        } else {
            if (!sector_size) sector_size = 4096;
            if (!sector_count) sector_count = (uint32_t)(size / sector_size);
            if ((size_t)sector_size * sector_count != size) { munmap(m, size); close(fd); return -1; }
        }
    }
    c = (as_filefl *)malloc(sizeof *c);
    if (!c) { munmap(m, size); close(fd); return -1; }
    c->fd = fd;
    c->mem = m;
    c->size = size;
    c->ss = sector_size;
    f->ctx = c;
    f->sector_size = sector_size;
    f->sector_count = sector_count;
    f->write_align = write_align ? write_align : 4;
    f->read = as_file_read;
    f->write = as_file_write;
    f->erase = as_file_erase;
    f->map = as_file_map;
    return 0;
}

void altsql_file_flash_close(altsql_flash *f) {
    as_filefl *c;
    if (!f || !f->ctx) return;
    c = (as_filefl *)f->ctx;
    msync(c->mem, c->size, MS_SYNC);
    munmap(c->mem, c->size);
    close(c->fd);
    free(c);
    f->ctx = NULL;
}

#endif /* ALTSQL_PORT_FILE */
