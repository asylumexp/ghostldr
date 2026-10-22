-- A fake Luac0re for ghostldr/lua/ghostldr.lua: just enough of its globals
-- (memory, func_wrap, dlsym, the JIT exploit's values) to run the bootstrap
-- as on a PS4 or a PS5.  Calling the core prints the boot context, one
-- "ctx <offset> <value>" line per u64, and the compiler-side syscall gadget
-- it was given.  test_ps4.py runs it:
--
--   lua boot_fake.lua <payload.lua> PS4|PS5 <firmware> [no-gadget]

local payload_path, platform, fw, no_gadget = arg[1], arg[2], arg[3], arg[4] == "no-gadget"

-- ---------------------------------------------------------------- memory
local mem, next_base = {}, 0x200000000
function malloc(n)
    local p = next_base
    next_base = next_base + ((n + 0xFFFF) // 0x10000 + 1) * 0x10000
    return p
end
function write8(p, v) mem[p] = v & 0xFF end
function read8(p) return mem[p] or 0 end
function write_buffer(p, s) for i = 1, #s do mem[p + i - 1] = s:byte(i) end end
function write_string(p, s) write_buffer(p, s .. "\0") end
function read_buffer(p, n)
    local t = {}
    for i = 0, n - 1 do t[i + 1] = string.char(mem[p + i] or 0) end
    return table.concat(t)
end
function write16(p, v) write_buffer(p, string.pack("<I2", v & 0xFFFF)) end
function write32(p, v) write_buffer(p, string.pack("<I4", v & 0xFFFFFFFF)) end
function write64(p, v) write_buffer(p, string.pack("<i8", v)) end
function read64(p) return (string.unpack("<i8", read_buffer(p, 8))) end
function hex_to_binary(hex) return (hex:gsub("..", function(cc) return string.char(tonumber(cc, 16)) end)) end

-- ---------------------------------------------------------------- Luac0re's state after jit_init()
PROT_READ, PROT_WRITE, AF_INET, SOCK_STREAM, SOCK_DGRAM, SOL_SOCKET = 1, 2, 2, 1, 2, 0xffff
PLATFORM, FW_VERSION = platform, fw
EBOOT_BASE, LIBKERNEL_HANDLE, SCE_KERNEL_DLSYM = 0x400000, 0x2001, 0x800001000
BRIDGE_BASE = platform == "PS4" and 0x914104000 or 0x900000000
IPU_PROG_BASE = BRIDGE_BASE + 0x9D4000
JIT_BASE, NEW_JIT_SOCK, NEW_MAIN_SOCK = 0x1000000, 41, 42
SHELLCODE_SCRATCH, OOB_SCRATCH_BASE, FAKE_RSP = BRIDGE_BASE + 0xC0000, BRIDGE_BASE + 0x32000, BRIDGE_BASE + 0x50000
JIT_ROP_CHAIN_START, JIT_ROP_CHAIN_END = BRIDGE_BASE + 0x51000, BRIDGE_BASE + 0x52000
LEAK_RBX, LEAK_RBP, LEAK_R12, LEAK_R13, LEAK_R14, LEAK_R15 = 1, 2, 3, 4, 5, 6
JIT_ROP = setmetatable({}, {__index = function(_, k) return JIT_BASE + #k end})
jit_memcpy_addr, jit_calloc_addr, jit_libc_error_addr = 0x80010000, 0x80020000, 0x80030000
jit_sceKernelJitCreateSharedMemory_addr, jit_sceKernelJitCreateAliasOfSharedMemory_addr = 0x80040000, 0x80041000
jit_sceKernelJitMapSharedMemory_addr, jit_sceKernelVirtualQuery_addr = 0x80042000, 0x80043000
SYSCALL_TABLE = {getpid = 0x14, sendmsg = 0x1c}
-- the compiler's libkernel: one "mov rax, N; mov r10, rcx; syscall; ..." wrapper per syscall (PS4)
local compiler = {}
jit_syscall = {}
if platform == "PS5" then
    jit_syscall.syscall_address = 0x800050007
else
    jit_syscall.syscall_wrapper = {[0x14] = 0x800060100, [0x1c] = 0x800060140}
    compiler[0x800060107] = no_gadget and 0x9090909090 or 0xC3050FCA8949  -- mov r10, rcx; syscall; ret
end
function jit_read64(addr) return compiler[addr] or 0 end
function jit_memcpy(dst, src, n) write_buffer(dst, read_buffer(src, n)) end

-- ---------------------------------------------------------------- calls
local calls = {}
function init_dlsym() end
function dlsym(handle, name) calls[#calls + 1] = name return 0x800100000 + #calls * 0x10 end
function create_socket() return 7 end
syscall = {setsockopt = function() return 0 end, connect = function() return 0 end, close = function() end}
function get_error_string() return "fake" end
function send_notification(s) print("notification: " .. s) end
function func_wrap(addr)
    return function(a1)
        if addr == IPU_PROG_BASE + 0x4000 then -- the core: show its context
            for off = 0, 0x1F8, 8 do print(string.format("ctx %x %x", off, read64(a1 + off))) end
            return 0
        end
        return 0   -- munmap, sceKernelMapFlexibleMemory (the address stays as written)
    end
end

dofile(payload_path)
