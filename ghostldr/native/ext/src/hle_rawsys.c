/*
 * Raw system calls in a title's own code.
 *
 * Sony's titles reach the kernel only through libkernel.  Homebrew made for
 * jailbroken consoles often doesn't: its jailbreak code (libjbc's kexec, a
 * mknod of /dev nodes) and some libraries issue `syscall` themselves.  On a
 * PS5 a system call from outside libkernel kills the process (a raw statfs
 * ended SWRR), so such a title would end SWRR at its first one.
 *
 * Before a module's text is written to executable memory (ext_exports.
 * patch_text), each `mov r10, rcx; syscall` that follows a `mov eax/rax, N`
 * becomes a call to rawsys_tramp, which hands the call to the libkernel
 * function of that number (as the title's own import of it would go: the
 * extension's, the core's, then SWRR's) and returns its result the way the
 * kernel does: the value in rax, or the errno with the carry flag set.  A
 * number with no such function answers ENOSYS, as a kernel without that call
 * does, logged once.  Nothing here gives the title more than SWRR has: GoldHEN's
 * kexec (11), for instance, is ENOSYS on a console whose kernel isn't exploited,
 * but for libjbc's kernel payloads, which hle_jbc.c runs against a virtual
 * kernel of its own (that kernel also follows the title's raw opens; mknod
 * and the rest reach hle_jbc.c's virtual devices through libkernel's names).
 */
#include "kmod.h"

/* hle_jbc.c; not in the ELF loader's extension */
void jbc_scan_text(struct module *m, const u8 *text, u64 size) HIDDEN __attribute__((weak));
bool jbc_kexec(u64 fn, u64 args, u64 sp, u64 *ret) HIDDEN __attribute__((weak));
s64 jbc_raw_open(const char *path, s64 fd) HIDDEN __attribute__((weak));

#define ENOSYS_ 78

/* number -> libkernel function (FreeBSD/Orbis numbering) */
static const struct {
    u16 nr;
    const char *name;
} k_sys[] = {
    {1, "_exit"},     {3, "read"},       {4, "write"},     {5, "open"},       {6, "close"},     {10, "unlink"},
    {12, "chdir"},    {14, "mknod"},     {15, "chmod"},    {20, "getpid"},    {23, "setuid"},   {24, "getuid"},
    {25, "geteuid"},  {41, "dup"},       {43, "getegid"},  {47, "getgid"},    {54, "ioctl"},    {74, "mprotect"},
    {90, "dup2"},     {92, "fcntl"},     {97, "socket"},   {98, "connect"},   {104, "bind"},    {105, "setsockopt"},
    {106, "listen"},  {118, "getsockopt"}, {128, "rename"}, {133, "sendto"},  {136, "mkdir"},   {137, "rmdir"},
    {188, "stat"},    {189, "fstat"},    {190, "lstat"},   {202, "sysctl"},   {240, "nanosleep"}, {272, "getdents"},
    {477, "mmap"},    {478, "lseek"},
};

typedef u64 (*fn6)(u64, u64, u64, u64, u64, u64);
static fn6 g_fn[ARRAY_SIZE(k_sys)];
static u8 g_said[1024 / 8];

/* the function a title's import of name would bind to */
static fn6 sys_fn(u32 i)
{
    if (!g_fn[i]) {
        char nid[12];
        compute_nid(k_sys[i].name, nid);
        u64 f = ext_find("libkernel", nid);
        if (!f && hle_lookup)
            f = hle_lookup("libkernel", k_sys[i].name);
        if (!f)
            f = (u64)host_real(k_sys[i].name);
        g_fn[i] = (fn6)f;
    }
    return g_fn[i];
}

static void say_once(u64 nr, u64 ra, const char *what)
{
    if (nr < 1024 && !(g_said[nr / 8] & (1u << (nr % 8)))) {
        g_said[nr / 8] |= (u8)(1u << (nr % 8));
        struct module *m = mod_by_addr(ra);
        logf("raw syscall %lu from %s+%lx: %s", nr, m ? m->name : "?", m ? ra - m->base : ra, what);
    }
}

/* r: {nr, a1..a6} as the trampoline saved them, then the return address at
 * r[8].  r[0] becomes 1 when the call failed (rax is then the errno). */
u64 rawsys_dispatch(u64 *r) HIDDEN __attribute__((used));
u64 rawsys_dispatch(u64 *r)
{
    u64 nr = r[0], a[6] = {r[1], r[2], r[3], r[4], r[5], r[6]}, ra = r[8];
    if (nr == 0 || nr == 198) { /* syscall(2), __syscall(2): the number comes first */
        nr = a[0];
        for (int k = 0; k < 5; k++)
            a[k] = a[k + 1];
        a[5] = 0;
    }
    r[0] = 1;
    if (nr == 11) {
        u64 v;
        if (jbc_kexec && jbc_kexec(a[0], a[1], (u64)r, &v)) { /* logged there */
            r[0] = 0;
            return v;
        }
        say_once(nr, ra, "kexec on a console running GoldHEN/HEN (run code in the kernel); this kernel isn't "
                         "exploited, so ENOSYS");
        return ENOSYS_;
    }
    for (u32 i = 0; i < ARRAY_SIZE(k_sys); i++)
        if (k_sys[i].nr == nr) {
            fn6 f = sys_fn(i);
            if (!f)
                break;
            say_once(nr, ra, k_sys[i].name);
            int *e = K.errno_fn ? K.errno_fn() : NULL;
            if (e)
                *e = 0;
            u64 v = f(a[0], a[1], a[2], a[3], a[4], a[5]);
            u64 err = (s64)v == -1 && e && *e ? (u64)*e : 0;
            if (nr == 5 && jbc_raw_open) { /* the virtual kernel follows the title's raw opens */
                s64 fd = jbc_raw_open((const char *)a[0], err ? -(s64)err : (s64)v);
                err = fd < 0 ? (u64)-fd : 0;
                v = fd < 0 ? v : (u64)fd;
            }
            if (err) {
                if (nr == 14 || nr == 23)
                    say_once(1000 + nr % 16, ra, "refused by the kernel, as for any app");
                return err;
            }
            r[0] = 0;
            return v;
        }
    say_once(nr, ra, "no libkernel function for it here: ENOSYS");
    return ENOSYS_;
}

/* The call that replaces `mov r10, rcx; syscall`: rax the number, the
 * arguments in rdi, rsi, rdx, rcx (the 4th: r10 was never set), r8, r9.
 * Returns as the kernel does: rax, CF on error, rdx 0, rcx/r11 clobbered,
 * the other registers kept. */
void rawsys_tramp(void) HIDDEN;
#if defined(__APPLE__) /* the tests' Mach-O build (test_rawsys.py) */
#define SYM(x) "_" #x
#define HIDE(x) ".private_extern _" #x "\n"
#else
#define SYM(x) #x
#define HIDE(x) ".hidden " #x "\n"
#endif
__asm__(".text\n"
        ".globl " SYM(rawsys_tramp) "\n" HIDE(rawsys_tramp)
        SYM(rawsys_tramp) ":\n"
        "    push %rbp\n"
        "    mov %rsp, %rbp\n"
        "    push %r9\n"
        "    push %r8\n"
        "    push %rcx\n"
        "    push %rdx\n"
        "    push %rsi\n"
        "    push %rdi\n"
        "    push %rax\n"
        "    push %r10\n"
        "    push %r11\n"
        "    lea -56(%rbp), %rdi\n"
        "    and $-16, %rsp\n"
        "    call " SYM(rawsys_dispatch) "\n"
        "    lea -72(%rbp), %rsp\n"
        "    pop %r11\n"
        "    pop %r10\n"
        "    mov -48(%rbp), %rdi\n"
        "    mov -40(%rbp), %rsi\n"
        "    mov -16(%rbp), %r8\n"
        "    mov -8(%rbp), %r9\n"
        "    mov $0, %edx\n"
        "    btq $0, -56(%rbp)\n"
        "    mov %rbp, %rsp\n"
        "    pop %rbp\n"
        "    ret\n");

/* the stub sites call: movabs r11, rawsys_tramp; jmp r11 (near every module) */
static u64 g_stub;

static u64 stub(void)
{
    if (!g_stub && stub_alloc_code) {
        u8 code[13] = {0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0xFF, 0xE3};
        u64 t = (u64)rawsys_tramp;
        memcpy(code + 2, &t, 8);
        g_stub = stub_alloc_code(code, sizeof(code));
    }
    return g_stub;
}

/* `mov eax, N` (b8) or `mov rax, N` (48 c7 c0) right before, N a system call number */
static bool number_before(const u8 *t, u64 at, u32 *nr)
{
    if (at >= 5 && t[at - 5] == 0xB8 && *(const u32 *)(t + at - 4) < 1024) {
        *nr = *(const u32 *)(t + at - 4);
        return true;
    }
    if (at >= 7 && t[at - 7] == 0x48 && t[at - 6] == 0xC7 && t[at - 5] == 0xC0 && *(const u32 *)(t + at - 4) < 1024) {
        *nr = *(const u32 *)(t + at - 4);
        return true;
    }
    return false;
}

void rawsys_patch_text(struct module *m, u8 *text, u64 size) HIDDEN;
void rawsys_patch_text(struct module *m, u8 *text, u64 size)
{
    static const u8 k_site[5] = {0x49, 0x89, 0xCA, 0x0F, 0x05}; /* mov r10, rcx; syscall */
    if (jbc_scan_text)
        jbc_scan_text(m, text, size);
    u32 n = 0, far = 0, nums[8], nn = 0;
    for (u64 at = 0; at + 5 <= size; at++) {
        if (text[at] != 0x49 || memcmp(text + at, k_site, 5) != 0)
            continue;
        u32 nr;
        if (!number_before(text, at, &nr))
            continue;
        u64 s = stub();
        s64 rel = s ? (s64)s - (s64)(m->base + at + 5) : 0;
        if (!s || rel != (s64)(s32)rel) {
            far++;
            continue;
        }
        text[at] = 0xE8; /* call rel32: the return lands where the syscall's next instruction is */
        s32 r32 = (s32)rel;
        memcpy(text + at + 1, &r32, 4);
        if (nn < ARRAY_SIZE(nums))
            nums[nn++] = nr;
        n++;
        at += 4;
    }
    if (n || far) {
        char list[64] = "";
        for (u32 i = 0; i < nn; i++)
            fmt(list + strlen(list), sizeof(list) - strlen(list), "%s%u", i ? " " : "", nums[i]);
        logf("%s: %u raw system call sites go through libkernel now (numbers %s%s)%s", m->name, n, list,
             n > nn ? " ..." : "", far ? ", some too far from the stub area to patch" : "");
    }
}
