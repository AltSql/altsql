/*
 * Copyright 2026 AltSql.com
 * SPDX-License-Identifier: Apache-2.0
 */
/* Minimal libc declarations for measuring code size on a bare-metal target. */
#ifndef SHIM_STRING_H
#define SHIM_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
char *strchr(const char *, int);
char *strpbrk(const char *, const char *);
#endif
