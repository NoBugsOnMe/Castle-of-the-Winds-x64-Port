/* Compare emulated ALU/shift flag results with the host x86-64 CPU. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/cpu.h"

u32 cpu_test_alu(int op, u32 a, u32 b, int sz, u32 *flags);
u32 cpu_test_shift(int op, u32 a, unsigned cnt, int sz, u32 *flags);
u32 cpu_test_incdec(int dec, u32 a, int sz, u32 *flags);

void native_dispatch(u16 id) { (void)id; }
void cpu_interrupt(u8 v) { (void)v; }
void cpu_periodic(void) { }
void cpu_fatal(const char *fmt, ...) { printf("fatal %s\n", fmt); exit(1); }

#define ARITH 0x8D5u

#define HOST_BIN(name, insn, T, suf)                                                     \
static uint64_t host_##name##suf(T a, T b, uint64_t fin, uint64_t *fout) {              \
    uint64_t fl; T r = a;                                                                \
    __asm__ volatile("pushq %[fin]\n\tpopfq\n\t" insn " %[b], %[r]\n\tpushfq\n\tpopq %[fl]" \
        : [r] "+r"(r), [fl] "=r"(fl) : [b] "r"(b), [fin] "r"(fin) : "cc");             \
    *fout = fl; return r; }

#define HOST_ALL(name, insn) \
    HOST_BIN(name, insn "b", uint8_t, 8) HOST_BIN(name, insn "w", uint16_t, 16) HOST_BIN(name, insn "l", uint32_t, 32)

HOST_ALL(add, "add") HOST_ALL(or, "or") HOST_ALL(adc, "adc") HOST_ALL(sbb, "sbb")
HOST_ALL(and, "and") HOST_ALL(sub, "sub") HOST_ALL(xor, "xor") HOST_ALL(cmp, "cmp")

#define HOST_SH(name, insn, T, suf)                                                      \
static uint64_t host_##name##suf(T a, uint8_t c, uint64_t fin, uint64_t *fout) {        \
    uint64_t fl; T r = a;                                                                \
    __asm__ volatile("pushq %[fin]\n\tpopfq\n\t" insn " %%cl, %[r]\n\tpushfq\n\tpopq %[fl]" \
        : [r] "+r"(r), [fl] "=r"(fl) : "c"(c), [fin] "r"(fin) : "cc");                  \
    *fout = fl; return r; }
#define HOST_SHALL(name, insn) \
    HOST_SH(name, insn "b", uint8_t, 8) HOST_SH(name, insn "w", uint16_t, 16) HOST_SH(name, insn "l", uint32_t, 32)
HOST_SHALL(rol, "rol") HOST_SHALL(ror, "ror") HOST_SHALL(rcl, "rcl") HOST_SHALL(rcr, "rcr")
HOST_SHALL(shl, "shl") HOST_SHALL(shr, "shr") HOST_SHALL(sar, "sar")

#define HOST_UN(name, insn, T, suf)                                                      \
static uint64_t host_##name##suf(T a, uint64_t fin, uint64_t *fout) {                   \
    uint64_t fl; T r = a;                                                                \
    __asm__ volatile("pushq %[fin]\n\tpopfq\n\t" insn " %[r]\n\tpushfq\n\tpopq %[fl]"   \
        : [r] "+r"(r), [fl] "=r"(fl) : [fin] "r"(fin) : "cc");                          \
    *fout = fl; return r; }
HOST_UN(inc, "incb", uint8_t, 8) HOST_UN(inc, "incw", uint16_t, 16) HOST_UN(inc, "incl", uint32_t, 32)
HOST_UN(dec, "decb", uint8_t, 8) HOST_UN(dec, "decw", uint16_t, 16) HOST_UN(dec, "decl", uint32_t, 32)

static uint32_t rnd32(void) {
    static uint64_t s = 88172645463325252ull;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    uint32_t v = (uint32_t)s;
    switch (s >> 60) { case 0: return 0; case 1: return 0xFFFFFFFF; case 2: return 0x80000000u; case 3: return 0x7FFFFFFF; case 4: return v & 0xFF; case 5: return 0x80; case 6: return 0x8000; }
    return v;
}

int main(void)
{
    int fails = 0, it, op, sz;
    cpu_init();
    for (it = 0; it < 200000; it++) {
        for (op = 0; op < 8; op++) {
            for (sz = 8; sz <= 32; sz *= 2) {
                uint32_t a = rnd32(), b = rnd32();
                uint64_t fin = 0x202 | (rnd32() & 1), fh;
                uint32_t fe = (uint32_t)fin, re, rh;
                uint32_t m = sz == 8 ? 0xFF : sz == 16 ? 0xFFFF : 0xFFFFFFFF;
                a &= m; b &= m;
                re = cpu_test_alu(op, a, b, sz, &fe);
                switch (op * 100 + sz) {
#define C(o, n) case o*100+8: rh = host_##n##8(a,b,fin,&fh); break; case o*100+16: rh = host_##n##16(a,b,fin,&fh); break; case o*100+32: rh = host_##n##32(a,b,fin,&fh); break;
                C(0, add) C(1, or) C(2, adc) C(3, sbb) C(4, and) C(5, sub) C(6, xor) C(7, cmp)
                default: rh = 0; fh = 0;
                }
                uint32_t mask = ARITH;
                if (op == 1 || op == 4 || op == 6) mask &= ~0x10u; /* AF undefined for logic ops */
                if ((re & m) != (rh & m) || (fe & mask) != (fh & mask)) {
                    if (fails++ < 20) printf("ALU op%d sz%d a=%x b=%x cf=%d: emu r=%x f=%x host r=%x f=%x\n", op, sz, a, b, (int)(fin & 1), re, fe & mask, rh, (unsigned)(fh & mask));
                }
            }
        }
        /* shifts */
        for (op = 0; op < 8; op++) {
            if (op == 6) continue;
            for (sz = 8; sz <= 32; sz *= 2) {
                uint32_t m = sz == 8 ? 0xFF : sz == 16 ? 0xFFFF : 0xFFFFFFFF;
                uint32_t a = rnd32() & m;
                unsigned cnt = rnd32() & 31;
                uint64_t fin = 0x202 | (rnd32() & ARITH), fh;
                uint32_t fe = (uint32_t)fin, re, rh;
                re = cpu_test_shift(op, a, cnt, sz, &fe);
                switch (op * 100 + sz) {
#define S(o, n) case o*100+8: rh = host_##n##8(a,cnt,fin,&fh); break; case o*100+16: rh = host_##n##16(a,cnt,fin,&fh); break; case o*100+32: rh = host_##n##32(a,cnt,fin,&fh); break;
                S(0, rol) S(1, ror) S(2, rcl) S(3, rcr) S(4, shl) S(5, shr) S(7, sar)
                default: rh = 0; fh = 0;
                }
                uint32_t mask;
                if (cnt == 0) mask = ARITH;
                else if (op < 4) mask = 0x001u | (cnt == 1 ? 0x800u : 0);   /* rotates: CF, OF(1) */
                else mask = (ARITH & ~0x10u & ~0x800u) | (cnt == 1 ? 0x800u : 0);
                if (op >= 4 && cnt >= (unsigned)sz && sz < 32) mask &= ~0x1u; /* CF undefined */
                if ((re & m) != (rh & m) || (fe & mask) != (fh & mask)) {
                    if (fails++ < 40) printf("SH op%d sz%d a=%x cnt=%u fin=%x: emu r=%x f=%x host r=%x f=%x\n", op, sz, a, cnt, (unsigned)fin, re, fe & mask, rh, (unsigned)(fh & mask));
                }
            }
        }
        for (sz = 8; sz <= 32; sz *= 2) {
            int dec;
            for (dec = 0; dec < 2; dec++) {
                uint32_t m = sz == 8 ? 0xFF : sz == 16 ? 0xFFFF : 0xFFFFFFFF;
                uint32_t a = rnd32() & m;
                uint64_t fin = 0x202 | (rnd32() & ARITH), fh;
                uint32_t fe = (uint32_t)fin, re, rh;
                re = cpu_test_incdec(dec, a, sz, &fe);
                if (!dec) rh = sz == 8 ? host_inc8(a, fin, &fh) : sz == 16 ? host_inc16(a, fin, &fh) : host_inc32(a, fin, &fh);
                else rh = sz == 8 ? host_dec8(a, fin, &fh) : sz == 16 ? host_dec16(a, fin, &fh) : host_dec32(a, fin, &fh);
                if ((re & m) != (rh & m) || (fe & ARITH) != (fh & ARITH)) {
                    if (fails++ < 60) printf("INCDEC dec=%d sz%d a=%x: emu %x/%x host %x/%x\n", dec, sz, a, re, fe & ARITH, rh, (unsigned)(fh & ARITH));
                }
            }
        }
    }
    printf("%s: %d failures\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
