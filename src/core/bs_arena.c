#include "bs_arena.h"

#include <stdlib.h>
#include <string.h>

#define ARENA_MIN 8192u

static bs_arena_blk *blk_new(size_t cap)
{
    bs_arena_blk *b = (bs_arena_blk *)malloc(sizeof(bs_arena_blk) + cap);

    if (!b) return NULL;
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

void bs_arena_init(bs_arena *a)
{
    if (!a) return;
    a->first = a->cur = NULL;
}

void bs_arena_free(bs_arena *a)
{
    bs_arena_blk *b;

    if (!a) return;
    b = a->first;
    while (b) {
        bs_arena_blk *n = b->next;
        free(b);
        b = n;
    }
    a->first = a->cur = NULL;
}

void *bs_arena_raw(bs_arena *a, size_t n)
{
    void *p;

    if (!a || n == 0) return NULL;
    if (!a->cur || a->cur->cap - a->cur->used < n) {
        bs_arena_blk *b = blk_new(n > ARENA_MIN ? n : ARENA_MIN);
        if (!b) return NULL;
        if (a->cur) a->cur->next = b;
        else a->first = b;
        a->cur = b;
    }
    p = a->cur->data + a->cur->used;
    a->cur->used += n;
    return p;
}

const char *bs_arena_put(bs_arena *a, const char *s, size_t n)
{
    char *p;

    if (!s || n == 0) return "";
    p = (char *)bs_arena_raw(a, n + 1);
    if (!p) return "";
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

const char *bs_arena_str(bs_arena *a, const char *s)
{
    return s ? bs_arena_put(a, s, strlen(s)) : "";
}
