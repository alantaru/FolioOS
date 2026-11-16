/* FOLIO v4 — libc mínima freestanding (sem dependências de SO). */
#pragma once
#include <stddef.h>
void *malloc(size_t n);
void  free(void *p);
void *realloc(void *p, size_t n);
static inline int abs(int x) { return x < 0 ? -x : x; }
