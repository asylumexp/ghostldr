/*
 * ghostldr's ELF loader (docs/elfldr.md): with no title running, ELF
 * payloads sent to a TCP port run inside SWRR, several at a time, with
 * SWRR's userland privileges and nothing more.
 *
 *   elfldr.c      the service: the listening socket, receiving a payload
 *                 (its length from its own headers, so `nc` needs no
 *                 half-close), text commands, status lines.  Plain C over
 *                 ftp.h's network environment and the el_* calls below.
 *   payload.c     what a payload is: the formats taken and refused, its
 *                 layout, relocation and TLS sites.  Plain C over
 *                 struct module and a resolver.
 *   elfldr_sys.c  the console: executable memory from views of the
 *                 emulator's JIT objects, direct memory for the rest,
 *                 threads, the imports payloads get instead of SWRR's
 *                 (sockets made by the compiler, _exit), their output on
 *                 the connection, faults, and freeing it all at the end.
 *
 * tests/test_elfldr.py builds elfldr.c and payload.c for the computer.
 * This header uses plain C types where elfldr.c needs it (the harness
 * includes it beside the system's headers).
 */
#pragma once

#define EL_PORT 9021                 /* TCP: the usual elfldr port; run.py elfldr --port */
#define EL_MAX_PAYLOAD (64u << 20)   /* bytes an ELF may have */
#define EL_SESSIONS 8                /* payloads at once (the JIT objects allow 4) */

/* ---- the loader: elfldr_sys.c on the console, a fake in the host tests ---- */
struct el_payload;
/* 0, or < 0 with why[] filled: what elfldr_serve can work with */
int el_init(char *why, unsigned size);
void el_fini(void);
/* Load the ELF (buf, len: the caller's, kept only during the call) as
 * payload id, its output to go to conn: 0 and *out, with a line about it
 * in info[]; or < 0 with why in info[] */
int el_load(const unsigned char *buf, unsigned long len, unsigned id, int conn, struct el_payload **out, char *info,
            unsigned size);
/* start it: 0, or < 0 with why[] filled (and the payload freed) */
int el_run(struct el_payload *p, char *why, unsigned size);
/* 1 when the payload has finished and is freed (its last line in line[]),
 * 0 while it runs */
int el_poll(struct el_payload *p, char *line, unsigned size);
void el_describe(struct el_payload *p, char *line, unsigned size); /* one line for "status" */

/* ---- the service (elfldr.c) ---- */
/* the length of the ELF or SELF whose first have bytes are b: > 0, 0 when
 * more bytes are needed to tell, < 0 when it is neither */
long el_file_size(const unsigned char *b, unsigned long have);
/* Serve on port until a client sends "quit" while nothing runs: 0, or < 0
 * (the port is taken, ...). */
int elfldr_serve(unsigned short port);

#ifdef BOOT_MAGIC /* beside ghost.h: the payload formats (payload.c) */
#define PL_ORBIS 1  /* imports from the system's libraries (OpenOrbis: an ELF, or create-fself's OELF/SELF) */
#define PL_FREE 2   /* no imports (freestanding): its entry gets struct el_args */
#define PL_TLS_MAX 0x8000 /* static TLS a payload may have (every thread's block has room for it) */
#define PL_TLS_ALIGN 64
#define PL_HEADER 0x8000 /* bytes at a JIT object's start never used for code */

struct pl_info {
    int abi;            /* PL_ORBIS or PL_FREE */
    bool sce;           /* Orbis dynamic tables (elf.c module_parse), else ELF's own */
    u64 exec_end;       /* [0, exec_end) is executable (a view of a JIT object); 16 KiB aligned */
    u64 image_size;     /* [exec_end, image_size) is direct memory; 16 KiB aligned */
    u64 relro_lo, relro_hi; /* read-only once relocated (whole pages inside it) */
    const u64 *relr;    /* ELF: DT_RELR's packed relative relocations */
    u64 nrelr;
};

/* What payload.c asks of its environment (elfldr_sys.c; the harness's in
 * the host tests): an Orbis module's tables (elf.c module_parse), and an
 * import's address (name as the symbol table has it: plain, or NID#L#M) */
int pl_parse_sce(struct module *m, u8 *file, u64 size);
typedef u64 (*pl_resolver)(void *ctx, struct module *m, const char *name, bool weak, int kind);

/* Parse and check file (kept until relocated) into m: 0, or -1 with why[] */
int pl_parse(struct module *m, u8 *file, u64 size, struct pl_info *info, char *why, u32 n);
/* The payload's image at m->base built in img (info->image_size bytes,
 * zeroed): segments, relocations, TLS sites.  0, or -1 with why[] */
int pl_build(struct module *m, const struct pl_info *info, u8 *img, pl_resolver resolve, void *ctx, char *why,
             u32 n);

/* A freestanding payload's entry gets this in rdi: ps5-payload-sdk's
 * payload_args_t, without kernel access (docs/elfldr.md) */
struct el_args {
    int (*sys_dynlib_dlsym)(int handle, const char *name, void *out); /* 0: *out is the address */
    int *rwpipe, *rwpair;     /* NULL: no kernel R/W */
    s64 kpipe_addr, kdata_base_addr; /* 0 */
    int *payloadout;          /* the exit code, if the payload sets it */
};
#endif
