/*
 * cpu.h - 16-bit (286/386-class) x86 protected-mode interpreter used to run
 *         the original Win16 code of Castle of the Winds.
 *
 * Segments are always interpreted through an emulated LDT (selectors), the
 * way Windows 3.x standard/enhanced mode runs applications.  Linear memory is
 * a flat byte array owned by mem.c.
 */
#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
/* msvcrt.dll only has _snprintf (no guaranteed termination); use wrappers */
#include <stdio.h>
#include <stdarg.h>
int cotw_snprintf(char *buf, size_t n, const char *fmt, ...);
int cotw_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
#undef snprintf
#undef vsnprintf
#define snprintf cotw_snprintf
#define vsnprintf cotw_vsnprintf
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* ---- linear memory ---------------------------------------------------- */
#define MEM_SIZE   (64u * 1024u * 1024u)       /* must be a power of two */
#define MEM_MASK   (MEM_SIZE - 1u)
extern u8 *g_mem;

static inline u8  rd8 (u32 a) { return g_mem[a & MEM_MASK]; }
static inline u16 rd16(u32 a) { u16 v; __builtin_memcpy(&v, g_mem + (a & MEM_MASK), 2); return v; }
static inline u32 rd32(u32 a) { u32 v; __builtin_memcpy(&v, g_mem + (a & MEM_MASK), 4); return v; }
static inline void wr8 (u32 a, u8  v) { g_mem[a & MEM_MASK] = v; }
static inline void wr16(u32 a, u16 v) { __builtin_memcpy(g_mem + (a & MEM_MASK), &v, 2); }
static inline void wr32(u32 a, u32 v) { __builtin_memcpy(g_mem + (a & MEM_MASK), &v, 4); }

/* ---- descriptor table -------------------------------------------------- */
#define LDT_ENTRIES 8192

#define DESC_PRESENT  0x01
#define DESC_CODE     0x02
#define DESC_DATA     0x04
#define DESC_HUGE     0x08     /* part of a tiled (huge) allocation */
#define DESC_SYSTEM   0x10     /* reserved by the emulator (thunks, etc.) */

typedef struct Descriptor {
    u32 base;       /* linear base */
    u32 limit;      /* byte limit (size-1) */
    u8  flags;
    u8  tiles;      /* for the first selector of a huge block: number of tiles */
    u16 owner;      /* handle of owning global block (first selector) */
} Descriptor;

extern Descriptor g_ldt[LDT_ENTRIES];

#define SEL_INDEX(s)   ((u16)(s) >> 3)
#define INDEX_SEL(i)   ((u16)(((i) << 3) | 7))

static inline int sel_valid(u16 sel)
{
    u16 i = SEL_INDEX(sel);
    return i != 0 && (g_ldt[i].flags & DESC_PRESENT);
}
static inline u32 sel_base(u16 sel) { return g_ldt[SEL_INDEX(sel)].base; }
static inline u32 sel_limit(u16 sel) { return g_ldt[SEL_INDEX(sel)].limit; }

/* Linear address of sel:off (no checks) */
static inline u32 lin(u16 sel, u16 off) { return sel_base(sel) + off; }
/* Host pointer to sel:off */
static inline void *ptr16(u16 sel, u16 off) { return g_mem + ((sel_base(sel) + off) & MEM_MASK); }
static inline void *ptrseg(u32 segptr) { return segptr ? ptr16((u16)(segptr >> 16), (u16)segptr) : (void *)0; }

/* ---- CPU state --------------------------------------------------------- */
enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS, S_NONE = 7 };

#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_TF 0x0100u
#define F_IF 0x0200u
#define F_DF 0x0400u
#define F_OF 0x0800u
#define F_NT 0x4000u
#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

typedef struct CPU {
    u32 r[8];
    u32 ip;
    u32 flags;
    u16 seg[6];
    u32 base[6];
    u32 limit[6];

    /* decoding state for the current instruction */
    u32 start_ip;
    int seg_ovr;      /* S_NONE or override segment */
    int opsz32;
    int adsz32;
    int rep;          /* 0, 0xF2, 0xF3 */

    /* run loop control */
    int stop;         /* set when a callback returns to native code */
    int depth;        /* nesting depth of cpu_run */
    u64 icount;       /* instructions executed */
} CPU;

extern CPU g_cpu;

/* register accessors */
#define AX (*(u16 *)&g_cpu.r[R_AX])
#define CX (*(u16 *)&g_cpu.r[R_CX])
#define DX (*(u16 *)&g_cpu.r[R_DX])
#define BX (*(u16 *)&g_cpu.r[R_BX])
#define SP (*(u16 *)&g_cpu.r[R_SP])
#define BP (*(u16 *)&g_cpu.r[R_BP])
#define SI (*(u16 *)&g_cpu.r[R_SI])
#define DI (*(u16 *)&g_cpu.r[R_DI])
#define AL (((u8 *)&g_cpu.r[R_AX])[0])
#define AH (((u8 *)&g_cpu.r[R_AX])[1])
#define CL (((u8 *)&g_cpu.r[R_CX])[0])
#define CH (((u8 *)&g_cpu.r[R_CX])[1])
#define DL (((u8 *)&g_cpu.r[R_DX])[0])
#define DH (((u8 *)&g_cpu.r[R_DX])[1])
#define BL (((u8 *)&g_cpu.r[R_BX])[0])
#define BH (((u8 *)&g_cpu.r[R_BX])[1])
#define EAX (g_cpu.r[R_AX])
#define ECX (g_cpu.r[R_CX])
#define EDX (g_cpu.r[R_DX])
#define EBX (g_cpu.r[R_BX])
#define ESP (g_cpu.r[R_SP])
#define EBP (g_cpu.r[R_BP])
#define ESI (g_cpu.r[R_SI])
#define EDI (g_cpu.r[R_DI])
#define CS (g_cpu.seg[S_CS])
#define DS (g_cpu.seg[S_DS])
#define ES (g_cpu.seg[S_ES])
#define SS (g_cpu.seg[S_SS])

/* hooks implemented elsewhere */
typedef void (*native_fn)(void);
void native_dispatch(u16 id);           /* opcode F1 xx xx */
void cpu_interrupt(u8 vec);             /* INT n */
void cpu_periodic(void);                /* every 256K instructions */
void cpu_fatal(const char *fmt, ...);   /* unrecoverable emulation error */

/* API */
void cpu_init(void);
int  cpu_load_seg(int sreg, u16 sel);   /* returns 0 on fault */
void cpu_run(void);                     /* runs until g_cpu.stop */
void push16(u16 v);
u16  pop16(void);
void push32(u32 v);
u32  pop32(void);

/* stack helpers for native code */
static inline u32 stack_lin(u16 off) { return g_cpu.base[S_SS] + off; }
static inline u16 stk16(int byteoff) { return rd16(g_cpu.base[S_SS] + (u16)(SP + byteoff)); }
static inline u32 stk32(int byteoff) { return rd32(g_cpu.base[S_SS] + (u16)(SP + byteoff)); }

/* trace ring buffer for diagnostics */
#define TRACE_LEN 256
extern u32 g_trace[TRACE_LEN];
extern unsigned g_trace_pos;
void cpu_dump_state(char *buf, size_t len);

/* the selector used for native thunks */
extern u16 g_thunk_sel;

#endif
