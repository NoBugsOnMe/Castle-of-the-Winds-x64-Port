/*
 * mem.h - emulated Win16 memory: selectors, global heap, local heap.
 */
#ifndef MEM_H
#define MEM_H

#include "cpu.h"

/* Win16 GMEM_/LMEM_ flags */
#define G_FIXED        0x0000
#define G_MOVEABLE     0x0002
#define G_NOCOMPACT    0x0010
#define G_NODISCARD    0x0020
#define G_ZEROINIT     0x0040
#define G_MODIFY       0x0080
#define G_DISCARDABLE  0x0100
#define G_SHARE        0x2000
#define G_DDESHARE     0x2000
#define G_NOTIFY       0x4000

#define L_FIXED        0x0000
#define L_MOVEABLE     0x0002
#define L_NOCOMPACT    0x0010
#define L_NODISCARD    0x0020
#define L_ZEROINIT     0x0040
#define L_MODIFY       0x0080
#define L_DISCARDABLE  0x0F00

/* kinds of global blocks */
enum { GK_APP = 0, GK_SEGMENT, GK_RESOURCE, GK_SYSTEM };

void mem_init(void);

/* linear space */
u32  lin_alloc(u32 size);
void lin_free(u32 base, u32 size);

/* selectors */
u16  sel_alloc(int count);
void sel_free(u16 sel, int count);
void sel_set(u16 sel, u32 base, u32 limit, int code);

/* global heap */
u16  glob_alloc(u16 flags, u32 size, int kind, int code, u32 reserve);
u16  glob_free(u16 h);
u16  glob_realloc(u16 h, u32 size, u16 flags);
u32  glob_size(u16 h);
u16  glob_sel(u16 h);           /* handle -> selector, 0 if invalid */
u16  glob_handle(u16 sel);      /* selector -> handle */
u16  glob_lock(u16 h);          /* returns selector or 0 */
int  glob_unlock(u16 h);
u16  glob_flags(u16 h);
u32  glob_free_space(void);

/* convenience: allocate a fixed system block and return its selector */
u16  sys_alloc(u32 size, int code);

/* local heap (the application's DGROUP) */
void local_init(u16 sel, u16 start, u16 end);
u16  local_alloc(u16 flags, u16 size);
u16  local_free(u16 h);
u16  local_realloc(u16 h, u16 size, u16 flags);
u16  local_lock(u16 h);
int  local_unlock(u16 h);
u16  local_size(u16 h);
u16  local_heap_sel(void);

/* temporary storage on the 16-bit stack (callers restore SP) */
u32  stack_alloc(u16 size);     /* returns segptr SS:newSP */

/* strings */
size_t strlen16(u32 segptr);

#endif
