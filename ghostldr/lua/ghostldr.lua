-- ghostldr bootstrap for Luac0re 2.4b (SWRR US CUSA03474 or EU CUSA03492, after jit_init())
-- or UltraC0re (Jak X EU CUSA07992 1.04, after its jit_init() and jit_compat_init()).
-- The host tool prepends:
--   GHOSTLDR_CORE_HEX   hex of build/ghostldr/core.bin
--   GHOSTLDR_TEXT_SIZE  bytes of code placed in JIT RX memory (16 KiB aligned)
--   GHOSTLDR_IMAGE_SIZE total image size including bss
--   GHOSTLDR_LOG_IP / GHOSTLDR_LOG_PORT   UDP log sink
--   GHOSTLDR_FLAGS      1 = probe only (ctx flags in native/include/ghost.h);
--                       this script adds 8192 on a PS4 (Luac0re's PLATFORM)
--                       and 32768 under UltraC0re
--   GHOSTLDR_APP_ROOT   where the title's app0 is: "ghostfs", a directory, or
--                       "usb:/path" on a USB drive (docs/usb.md)
--   GHOSTLDR_EXT_PORT / GHOSTLDR_EXT_SIZE / GHOSTLDR_EXT_CRC
--   GHOSTLDR_EXT_SHA256 (the blob's SHA-256 hex; the core pins its
--   first 24 bytes against the served blob after the CRC screen)
--                       the runtime extension (build/ghostldr/ext.bin, with the
--                       NID table): run.py serves it over TCP on the log IP;
--                       size 0 = none.  This script only connects the socket:
--                       the core reads the blob itself and checks size and CRC.
--   GHOSTLDR_SEL_PORT / GHOSTLDR_SEL_SIZE / GHOSTLDR_SEL_CRC
--                       the same for the game selector (build/ghostldr/selector.bin)
--   GHOSTLDR_PROFILE_HEX  the title profiles (host/profiles.py boot_bundle); the
--                       core finds them through the last u64 of the context

local function fail(msg)
    send_notification("ghostldr: " .. msg)
    error("ghostldr: " .. msg)
end

local function u32le_ip(ip)
    local a, b, c, d = ip:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
    return (tonumber(d) << 24) | (tonumber(c) << 16) | (tonumber(b) << 8) | tonumber(a)
end

-- UltraC0re parks the compiler on its own loop (door1.lua) and keeps
-- Luac0re's jit_* names for the rest (jitcompat.lua)
local ULTRA = J_COMP ~= nil and J_SPIN ~= nil
if ULTRA then
    if not J_STUB or not NEW_JIT_SOCK or not jit_memcpy_addr then
        fail("UltraC0re's compiler calls are not initialized (jit_compat_init)")
    end
elseif not NEW_JIT_SOCK or not JIT_ROP then
    fail("Luac0re JIT exploit is not initialized")
end
init_dlsym()
local BRIDGE_ADDR = BRIDGE_BASE or BRIDGE
-- The core calls dlsym through the eboot's "call rbx; ret" gadget, and the
-- kernel takes the caller from the callee's frame: it must build one, as
-- Luac0re's SCE_KERNEL_DLSYM (sceKernelDlsym itself) does.  UltraC0re's is
-- the bare dynlib_dlsym system call, which leaves the core's frame (JIT
-- memory) in place and is refused, so the core gets the export instead.
local core_dlsym = SCE_KERNEL_DLSYM
if ULTRA then
    core_dlsym = dlsym(LIBKERNEL_HANDLE, "sceKernelDlsym")
    if core_dlsym == 0 then fail("dlsym sceKernelDlsym") end
end

-- Bridge memory (shared with the compiler) for the core's staging buffer and
-- its private scratch (jitrpc.c PRIV: oob_scratch + 0x70000 on, ~0x21000).
-- Luac0re: its own OOB scratch, and 4 MiB at SHELLCODE_SCRATCH.  Jak X's
-- bridge holds the emulator's data at +0..+0x2f9000 and +0x90f000 on, and
-- UltraC0re's at +0x500000..+0x504400; +0x540000 (SHELLCODE_SCRATCH) to
-- +0x90f000 stays zero (SWRR-game-bridge research/jakx/bridge_probe.lua):
-- staging +0x540000, scratch up to about +0x761000.
local staging, staging_size, oob_scratch = SHELLCODE_SCRATCH, 0x400000, OOB_SCRATCH_BASE
if ULTRA then
    staging, staging_size = SHELLCODE_SCRATCH, 0x200000
    oob_scratch = SHELLCODE_SCRATCH + staging_size - 0x70000
end

local k = {}
local function kfn(name)
    if not k[name] then
        local addr = dlsym(LIBKERNEL_HANDLE, name)
        if addr == 0 then fail("dlsym " .. name) end
        k[name] = func_wrap(addr)
    end
    return k[name]
end

local core = hex_to_binary(GHOSTLDR_CORE_HEX)
local text_size = GHOSTLDR_TEXT_SIZE
local image_size = (GHOSTLDR_IMAGE_SIZE + 0x3FFF) & ~0x3FFF
local data_size = image_size - text_size

-- 1. the core lives at the start of the emulator's IPU JIT program buffer
-- (RX here, RW in the compiler at the same address): the compiler process
-- can create almost no new JIT memory, so reuse what the emulator owns.
local out = malloc(0x40)
local rx = IPU_PROG_BASE + 0x4000
local small_arena = rx + image_size

-- 2. code through the compiler (same address in both processes)
write_buffer(SHELLCODE_SCRATCH, core:sub(1, text_size))
jit_memcpy(rx, SHELLCODE_SCRATCH, text_size)

-- 3. the core's data pages: replace that part of the RX buffer with RW memory
kfn("munmap")(rx + text_size, data_size)
write64(out, rx + text_size)
local rc = kfn("sceKernelMapFlexibleMemory")(out, data_size, PROT_READ | PROT_WRITE, 0x10)
if rc ~= 0 or read64(out) ~= rx + text_size then
    fail(string.format("data map rc=%x at %x", rc, read64(out)))
end

-- 4. data directly
if #core > text_size then
    write_buffer(rx + text_size, core:sub(text_size + 1))
end
if read64(rx) ~= string.unpack("<I8", core, 1) then fail("core text verify") end

-- 5. log socket (Luac0re routes AF_INET creation through the compiler)
local log_fd = create_socket(AF_INET, SOCK_DGRAM, 0)
local log_addr = malloc(16)
for i = 0, 15 do write8(log_addr + i, 0) end
write8(log_addr + 1, AF_INET)
write16(log_addr + 2, ((GHOSTLDR_LOG_PORT << 8) | (GHOSTLDR_LOG_PORT >> 8)) & 0xFFFF)
write32(log_addr + 4, u32le_ip(GHOSTLDR_LOG_IP))

-- 6. extension and selector sockets (created through the compiler like the log socket)
local function blob_socket(port, size, what)
    if (size or 0) <= 0 then return -1 end
    local sa, tv = malloc(16), malloc(16)
    for i = 0, 15 do write8(sa + i, 0) end
    write8(sa + 1, AF_INET)
    write16(sa + 2, ((port << 8) | (port >> 8)) & 0xFFFF)
    write32(sa + 4, u32le_ip(GHOSTLDR_LOG_IP))
    write64(tv, 15)     -- SO_RCVTIMEO: a stalled transfer fails instead of hanging the core
    write64(tv + 8, 0)
    local fd = create_socket(AF_INET, SOCK_STREAM, 0)
    if fd >= 0 then
        syscall.setsockopt(fd, SOL_SOCKET, 0x1006, tv, 16)
        local rc = syscall.connect(fd, sa, 16)
        if rc ~= 0 then
            send_notification(string.format("ghostldr: %s server %s:%d: connect failed (%s)",
                what, GHOSTLDR_LOG_IP, port, get_error_string()))
            syscall.close(fd)
            fd = -1
        end
    end
    return fd
end
local ext_fd = blob_socket(GHOSTLDR_EXT_PORT, GHOSTLDR_EXT_SIZE, "extension")
local sel_fd = blob_socket(GHOSTLDR_SEL_PORT, GHOSTLDR_SEL_SIZE, "selector")

-- 7. boot context (field order must match struct boot_ctx in ghost.h)
-- A syscall instruction in the compiler (jitrpc.c moves descriptors and
-- frees JIT blocks with it).  PS5: Luac0re's own (getpid + 7), which takes
-- any number from rax.  PS4: getpid's wrapper + 7 stands for "a system
-- call"; the core calls each number's own wrapper from compiler_stubs().
local function compiler_syscall()
    if jit_syscall.syscall_address then return jit_syscall.syscall_address end
    local w = jit_syscall.syscall_wrapper and jit_syscall.syscall_wrapper[SYSCALL_TABLE.getpid]
    if w and (jit_read64(w + 7) & 0xFFFFFFFFFF) == 0x050FCA8949 then return w + 7 end
    return 0
end
-- PS4: a system call runs only from its own number's libkernel wrapper,
-- "mov rax, N; mov r10, rcx; syscall" (Luac0re: "PS4 requires valid syscall
-- wrapper"); getpid's with another number in rax stopped the compiler.  The
-- compiler's wrapper for each number the core and the extension call there
-- (getpid, write, close, sendmsg, munmap, socket, clock_gettime), as
-- (number, address) pairs ended by a zero number.  Luac0re's table covers
-- its first 128 KiB of libkernel; the rest of the text is searched here, in
-- this process's copy, at the same offset.  Each wrapper's bytes are
-- checked in the compiler.  0 on a PS5.
local COMPILER_SYSCALLS = {0x14, 0x4, 0x6, 0x1c, 0x49, 0x61, 0xe8}
local function stub_ok(w, num)
    return jit_read64(w) == (0xC0C748 | (num << 24) | (0x49 << 56))
        and (jit_read64(w + 7) & 0xFFFFFFFFFF) == 0x050FCA8949
end
local libkernel_text
local function search_stub(num)
    if not (LIBKERNEL_BASE and JIT_LIBKERNEL_BASE and sceKernelGetModuleInfoFromAddr) then return nil end
    if not libkernel_text then
        local info, size = malloc(0x300), 0
        if sceKernelGetModuleInfoFromAddr(LIBKERNEL_BASE, 1, info) == 0 and read64(info + 0x160) == LIBKERNEL_BASE then
            size = read64(info + 0x168) & 0xFFFFFFFF   -- segments[0]: {address, size, prot}
        end
        libkernel_text = size > 0 and size <= 0x400000 and read_buffer(LIBKERNEL_BASE, size) or ""
    end
    local at = libkernel_text:find(string.pack("<I3I4I3", 0xC0C748, num, 0xCA8949) .. "\x0f\x05", 1, true)
    return at and JIT_LIBKERNEL_BASE + at - 1
end
local function compiler_stubs()
    if PLATFORM ~= "PS4" then return 0 end
    local tbl, n = malloc(#COMPILER_SYSCALLS * 16 + 16), 0
    for _, num in ipairs(COMPILER_SYSCALLS) do
        local w = jit_syscall.syscall_wrapper and jit_syscall.syscall_wrapper[num]
        if not (w and stub_ok(w, num)) then w = search_stub(num) end
        if w and stub_ok(w, num) then
            write64(tbl + n * 16, num)
            write64(tbl + n * 16 + 8, w)
            n = n + 1
        end
    end
    write64(tbl + n * 16, 0)
    return tbl
end
local function fw_number()
    local major, minor = tostring(FW_VERSION or ""):match("(%d+)%.(%d+)")
    return major and tonumber(major, 16) * 0x100 + tonumber(minor, 16) or 0
end
local flags = GHOSTLDR_FLAGS | (PLATFORM == "PS4" and 8192 or 0) | (ULTRA and 32768 or 0)
-- the compiler chain's parts: 19 slots (a union in struct boot_ctx)
local engine
if ULTRA then
    local c = J_COMP
    engine = {
        c + C_PRAX, c + C_PRDI_PRBP, c + C_PRSI_PRBP, c + C_PRCX, c + C_MOVRDXRSI, c + C_WRDI,
        c + C_POPRSPDEREF, J_SPIN, J_CALL, J_RES, J_CNT, 0, 0, 0, 0, 0, 0, 0, 0,
    }
else
    engine = {
        JIT_ROP.RET, JIT_ROP.MOV_RAX_RDI_RET, JIT_ROP.POP_RDI_RET, JIT_ROP.POP_RSI_RET,
        JIT_ROP.MOV_RDX_RDI_RET, JIT_ROP.POP_RCX_RET, JIT_ROP.POP_RBX_RET,
        JIT_ROP.MOV_DEREF_RBX_RAX_RET, JIT_ROP.POP_RSP_RET, JIT_ROP.POP_R12_R13_R14_R15_RBP_RET,
        JIT_ROP.MOV_R8_RBX_RET, JIT_ROP.MOV_R9_RBX_RET,
        LEAK_RBX, LEAK_RBP, LEAK_R12, LEAK_R13, LEAK_R14, LEAK_R15,
        FAKE_RSP,
    }
end
local fields = {
    0x3130305244484c47, 0, EBOOT_BASE, core_dlsym,
    EBOOT_BASE + (ULTRA and 0x19869 or 0x31aa9),    -- the eboot's "call rbx; ret"
    LIBKERNEL_HANDLE, compiler_stubs(), log_fd, log_addr,
    JIT_BASE,
}
for _, v in ipairs(engine) do fields[#fields + 1] = v end
for _, v in ipairs({
    oob_scratch, JIT_ROP_CHAIN_START or 0, JIT_ROP_CHAIN_END or 0,
    jit_memcpy_addr, jit_calloc_addr, compiler_syscall(), jit_libc_error_addr or 0,
    jit_sceKernelJitCreateSharedMemory_addr, jit_sceKernelJitCreateAliasOfSharedMemory_addr,
    jit_sceKernelJitMapSharedMemory_addr,
    NEW_JIT_SOCK, NEW_MAIN_SOCK,
    staging, staging_size,
    0, flags,
}) do fields[#fields + 1] = v end
if #fields ~= 45 then fail("the boot context has " .. #fields .. " fields, not 45") end
local ctx = malloc(0x200)
for i = 0, 0x1F8, 8 do write64(ctx + i, 0) end
for i, v in ipairs(fields) do write64(ctx + (i - 1) * 8, v) end
write64(ctx + 8, #fields * 8 + 8 * 8 + 7 * 8 + 3 * 8)    -- fields, reserved[8], ext_fd/size/crc, sel_fd/size/crc, fw, ext SHA-256 pin[24]
local app_root = malloc(#GHOSTLDR_APP_ROOT + 1)
write_string(app_root, GHOSTLDR_APP_ROOT)
write64(ctx + (#fields - 2) * 8, app_root)
write64(ctx + #fields * 8 + 0x00, jit_sceKernelVirtualQuery_addr or 0)
write64(ctx + #fields * 8 + 0x08, BRIDGE_ADDR or 0)
write64(ctx + #fields * 8 + 0x10, IPU_PROG_BASE or 0)
write64(ctx + #fields * 8 + 0x18, small_arena)
write64(ctx + #fields * 8 + 0x20, GHOSTLDR_GPU_HEAP_MIB or 0)
write64(ctx + #fields * 8 + 0x28, GHOSTLDR_SLACK_MIB or 0)
write64(ctx + #fields * 8 + 0x30, GHOSTLDR_POOL1_MIB or 0)
write64(ctx + #fields * 8 + 0x38, GHOSTLDR_CPU_HEAP_MIB or 0)
write64(ctx + #fields * 8 + 0x40, ext_fd)
write64(ctx + #fields * 8 + 0x48, GHOSTLDR_EXT_SIZE or 0)
write64(ctx + #fields * 8 + 0x50, GHOSTLDR_EXT_CRC or 0)
-- the selector's fields and fw end at +0x78, the ext SHA-256 pin fills
-- +0x78..+0x90: both must stay below the profile slot (0x1F8)
if #fields * 8 + 0x90 > 0x1F8 then fail("the boot context reaches the profile slot") end
write64(ctx + #fields * 8 + 0x58, sel_fd)
write64(ctx + #fields * 8 + 0x60, GHOSTLDR_SEL_SIZE or 0)
write64(ctx + #fields * 8 + 0x68, GHOSTLDR_SEL_CRC or 0)
write64(ctx + #fields * 8 + 0x70, fw_number())   -- "12.02" -> 0x1202
-- the extension's SHA-256 pin (first 24 bytes of the digest): ext.c checks
-- the served blob against it after the CRC-32 screen; absent or short
-- leaves zero bytes, which the core reads as "not pinned"
if GHOSTLDR_EXT_SHA256 and #GHOSTLDR_EXT_SHA256 >= 48 then
    write_buffer(ctx + #fields * 8 + 0x78, hex_to_binary(GHOSTLDR_EXT_SHA256:sub(1, 48)))
end
-- title profiles: blobs ended by 8 zero bytes, pointed to by the context's last
-- u64 (profile.c PROFILE_SLOT); without them the core uses its generic defaults
if GHOSTLDR_PROFILE_HEX and #GHOSTLDR_PROFILE_HEX > 0 then
    local prof = hex_to_binary(GHOSTLDR_PROFILE_HEX)
    local prof_mem = malloc(#prof + 8)
    write_buffer(prof_mem, prof)
    write64(prof_mem + #prof, 0)
    write64(ctx + 0x1F8, prof_mem)
end

send_notification(string.format("ghostldr: core at %x, calling", rx))
local ret = func_wrap(rx)(ctx)
send_notification(string.format("ghostldr: core returned %x", ret))
