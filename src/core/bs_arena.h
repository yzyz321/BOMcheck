#ifndef BS_ARENA_H
#define BS_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 分块 arena：只增不删，整块释放。
   块与块之间不连续，所以已经发出去的指针不会因后续分配而失效——
   bs_sheet 的行表、bs_pair 的结果行都在里面存裸指针，靠的就是这一条。
   注意：单次分配大于块大小时单独开一块，绝不 realloc。 */

typedef struct bs_arena_blk {
    struct bs_arena_blk *next;
    size_t used, cap;
    char data[1];
} bs_arena_blk;

typedef struct {
    bs_arena_blk *first, *cur;
} bs_arena;

void        bs_arena_init(bs_arena *a);
void        bs_arena_free(bs_arena *a);                 /* 可重复调用 */
void       *bs_arena_raw(bs_arena *a, size_t n);        /* NULL = 内存不足 */
const char *bs_arena_put(bs_arena *a, const char *s, size_t n);  /* 失败返回 "" */
const char *bs_arena_str(bs_arena *a, const char *s);   /* NULL / "" → "" */

#ifdef __cplusplus
}
#endif

#endif /* BS_ARENA_H */
