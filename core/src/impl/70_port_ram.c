/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* ======================================================================
 * Port: flash emulated in RAM, with power-cut simulation (tests)
 *
 * Behaves like NOR flash: a write can only clear bits, an erase sets a
 * whole sector to 0xFF. budget counts the bytes the "chip" may still
 * program; an erase costs AS_RAM_ERASE_COST. When the budget runs out the
 * power is cut: the byte being written is left partly programmed (some of
 * its bits cleared, some not), or the sector being erased is left half
 * erased, and every later call fails until the test "reboots" by calling
 * altsql_ram_flash_init() again over the same memory. dead tells how the
 * power was lost: 1 while writing, 2 while erasing.
 * ====================================================================== */
#ifdef ALTSQL_PORT_RAM

#define AS_RAM_ERASE_COST 64

static uint32_t as_ram_rand(altsql_ram_flash *r) {
    uint32_t x = r->rng ? r->rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->rng = x;
    return x;
}

static int as_ram_bad(const altsql_ram_flash *r, uint32_t addr, uint32_t len) {
    return r->dead || addr > r->size || len > r->size - addr;
}

static int as_ram_read(void *ctx, uint32_t addr, void *buf, uint32_t len) {
    altsql_ram_flash *r = (altsql_ram_flash *)ctx;
    if (as_ram_bad(r, addr, len)) return -1;
    memcpy(buf, r->mem + addr, len);
    return 0;
}

static const uint8_t *as_ram_map(void *ctx, uint32_t addr, uint32_t len) {
    altsql_ram_flash *r = (altsql_ram_flash *)ctx;
    return as_ram_bad(r, addr, len) ? NULL : r->mem + addr;
}

static int as_ram_write(void *ctx, uint32_t addr, const void *buf, uint32_t len) {
    altsql_ram_flash *r = (altsql_ram_flash *)ctx;
    const uint8_t *s = (const uint8_t *)buf;
    uint32_t i;
    if (as_ram_bad(r, addr, len)) return -1;
    for (i = 0; i < len; i++) {
        if (r->budget == 0) {
            r->mem[addr + i] &= (uint8_t)(s[i] | as_ram_rand(r));   /* partly programmed */
            r->dead = 1;
            return -1;
        }
        if (r->budget > 0) r->budget--;
        r->mem[addr + i] &= s[i];
        r->bytes_written++;
    }
    return 0;
}

static int as_ram_erase(void *ctx, uint32_t sector) {
    altsql_ram_flash *r = (altsql_ram_flash *)ctx;
    uint8_t *p;
    uint32_t i;
    if (r->dead || sector >= r->size / r->sector_size) return -1;
    p = r->mem + (size_t)sector * r->sector_size;
    if (r->budget >= 0 && r->budget < AS_RAM_ERASE_COST) {
        for (i = 0; i < r->sector_size; i++) {             /* half erased */
            uint32_t x = as_ram_rand(r);
            if ((x & 3u) == 0) p[i] = 0xFF;
            else if ((x & 3u) == 1) p[i] |= (uint8_t)(x >> 8);
        }
        r->budget = 0;
        r->dead = 2;                                        /* 2: cut while erasing */
        return -1;
    }
    if (r->budget > 0) r->budget -= AS_RAM_ERASE_COST;
    memset(p, 0xFF, r->sector_size);
    r->erases++;
    return 0;
}

/* Does not touch mem, so calling it again "reboots" over the same flash.
 * Resets budget (-1), the counters and the random seed (set rng after). */
void altsql_ram_flash_init(altsql_flash *f, altsql_ram_flash *r, uint8_t *mem,
                           uint32_t sector_size, uint32_t sector_count, uint32_t write_align) {
    memset(r, 0, sizeof *r);
    r->mem = mem;
    r->size = sector_size * sector_count;
    r->sector_size = sector_size;
    r->budget = -1;
    r->rng = 0x2545F491u;
    memset(f, 0, sizeof *f);
    f->ctx = r;
    f->sector_size = sector_size;
    f->sector_count = sector_count;
    f->write_align = write_align;
    f->read = as_ram_read;
    f->write = as_ram_write;
    f->erase = as_ram_erase;
    f->map = as_ram_map;
}

#endif /* ALTSQL_PORT_RAM */
