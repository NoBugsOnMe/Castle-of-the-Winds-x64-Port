/*
 * ne.h - NE (Windows 3.x) executable loader
 */
#ifndef NE_H
#define NE_H

#include "cpu.h"

typedef struct NERes {
    u16  type_id;        /* integer type, or 0 if named */
    char type_name[32];
    u16  name_id;        /* integer id, or 0 if named */
    char name[32];
    u32  offset;         /* file offset */
    u32  length;
    u16  flags;
    u16  loaded;         /* global handle when loaded */
    u16  refcount;
} NERes;

typedef struct NEEntry {
    u8  seg;             /* 0 = unused, 0xFE = constant */
    u16 off;
    u8  flags;
} NEEntry;

typedef struct NEModule {
    char name[16];
    char path[260];      /* path of the original executable (directory used for data files) */
    const u8 *img;
    u32 imglen;
    u16 ne;              /* offset of NE header */
    u16 flags;
    u16 autodata;        /* 1-based */
    u16 heap, stack;
    u16 cs, ip, ss, sp;
    int nseg;
    u16 segsel[256];
    u16 seglen[256];
    u16 segflags[256];
    int nent;
    NEEntry *ent;        /* index = ordinal */
    NERes *res;
    int nres;
    u16 hModule;         /* fake module database selector */
    u16 hInstance;       /* = DGROUP selector */
    u16 dgroup;          /* DGROUP selector */
    u16 expver;
} NEModule;

extern NEModule g_mod;

int  ne_load(const u8 *img, u32 len, const char *path, char *err, int errlen);
u32  ne_entry(int ordinal);      /* segptr of exported entry */
int  ne_find_resource(u32 type, u32 name);   /* index or -1; type/name are segptr or MAKEINTRESOURCE */
int  ne_find_resource_c(u16 type_id, const char *type_name, u16 name_id, const char *name);
const u8 *ne_res_data(int idx, u32 *len);
int  ne_is_module_handle(u16 h);

/* thunk management (native.c) */
u16  thunk_offset_for(const char *module, u16 ordinal, int *is_equate, u16 *equate_value);
void thunks_init(void);
u32  make_proc_instance(u32 proc, u16 ds);
void free_proc_instance(u32 thunk);

#endif
