-- ghostldr bootstrap for Luac0re 2.4b (SWRR US CUSA03474 or EU CUSA03492, after jit_init()).
-- The host tool prepends:
--   GHOSTLDR_CORE_HEX   hex of build/ghostldr/core.bin
--   GHOSTLDR_TEXT_SIZE  bytes of code placed in JIT RX memory (16 KiB aligned)
--   GHOSTLDR_IMAGE_SIZE total image size including bss
--   GHOSTLDR_LOG_IP / GHOSTLDR_LOG_PORT   UDP log sink
--   GHOSTLDR_FLAGS      1 = probe only
--   GHOSTLDR_APP_ROOT   path of the title's app0 directory
--   GHOSTLDR_EXT_PORT / GHOSTLDR_EXT_SIZE / GHOSTLDR_EXT_CRC
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

if not NEW_JIT_SOCK or not JIT_ROP then
    fail("Luac0re JIT exploit is not initialized")
end
init_dlsym()

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
local fields = {
    0x3130305244484c47, 0, EBOOT_BASE, SCE_KERNEL_DLSYM, EBOOT_BASE + 0x31aa9,
    LIBKERNEL_HANDLE, 0, log_fd, log_addr,
    JIT_BASE,
    JIT_ROP.RET, JIT_ROP.MOV_RAX_RDI_RET, JIT_ROP.POP_RDI_RET, JIT_ROP.POP_RSI_RET,
    JIT_ROP.MOV_RDX_RDI_RET, JIT_ROP.POP_RCX_RET, JIT_ROP.POP_RBX_RET,
    JIT_ROP.MOV_DEREF_RBX_RAX_RET, JIT_ROP.POP_RSP_RET, JIT_ROP.POP_R12_R13_R14_R15_RBP_RET,
    JIT_ROP.MOV_R8_RBX_RET, JIT_ROP.MOV_R9_RBX_RET,
    LEAK_RBX, LEAK_RBP, LEAK_R12, LEAK_R13, LEAK_R14, LEAK_R15,
    FAKE_RSP, OOB_SCRATCH_BASE, JIT_ROP_CHAIN_START, JIT_ROP_CHAIN_END,
    jit_memcpy_addr, jit_calloc_addr, jit_syscall.syscall_address or 0, jit_libc_error_addr,
    jit_sceKernelJitCreateSharedMemory_addr, jit_sceKernelJitCreateAliasOfSharedMemory_addr,
    jit_sceKernelJitMapSharedMemory_addr,
    NEW_JIT_SOCK, NEW_MAIN_SOCK,
    SHELLCODE_SCRATCH, 0x400000,
    0, GHOSTLDR_FLAGS,
}
local ctx = malloc(0x200)
for i = 0, 0x1F8, 8 do write64(ctx + i, 0) end
for i, v in ipairs(fields) do write64(ctx + (i - 1) * 8, v) end
write64(ctx + 8, #fields * 8 + 8 * 8 + 6 * 8)    -- fields, reserved[8], ext_fd/size/crc, sel_fd/size/crc
local app_root = malloc(#GHOSTLDR_APP_ROOT + 1)
write_string(app_root, GHOSTLDR_APP_ROOT)
write64(ctx + (#fields - 2) * 8, app_root)
write64(ctx + #fields * 8 + 0x00, jit_sceKernelVirtualQuery_addr or 0)
write64(ctx + #fields * 8 + 0x08, BRIDGE_BASE or 0)
write64(ctx + #fields * 8 + 0x10, IPU_PROG_BASE or 0)
write64(ctx + #fields * 8 + 0x18, small_arena)
write64(ctx + #fields * 8 + 0x20, GHOSTLDR_GPU_HEAP_MIB or 0)
write64(ctx + #fields * 8 + 0x28, GHOSTLDR_SLACK_MIB or 0)
write64(ctx + #fields * 8 + 0x30, GHOSTLDR_POOL1_MIB or 0)
write64(ctx + #fields * 8 + 0x38, GHOSTLDR_CPU_HEAP_MIB or 0)
write64(ctx + #fields * 8 + 0x40, ext_fd)
write64(ctx + #fields * 8 + 0x48, GHOSTLDR_EXT_SIZE or 0)
write64(ctx + #fields * 8 + 0x50, GHOSTLDR_EXT_CRC or 0)
-- the selector's fields end at +0x70: they must stay below the profile slot (0x1F8)
if #fields * 8 + 0x70 > 0x1F8 then fail("the boot context reaches the profile slot") end
write64(ctx + #fields * 8 + 0x58, sel_fd)
write64(ctx + #fields * 8 + 0x60, GHOSTLDR_SEL_SIZE or 0)
write64(ctx + #fields * 8 + 0x68, GHOSTLDR_SEL_CRC or 0)
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
