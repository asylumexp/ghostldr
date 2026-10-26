-- A fake Luac0re for ghostldr/lua/ghostldr.lua: just enough of its globals
-- (memory, func_wrap, dlsym, the JIT exploit's values, sockets) to run the
-- bootstrap as on a PS4 or a PS5.  Calling the core prints the boot context,
-- one "ctx <offset> <value>" line per u64, and the compiler-side syscall
-- gadget it was given.  test_ps4.py and test_bundle.py run it:
--
--   lua boot_fake.lua <payload.lua> PS4|PS5 <firmware> [OPTION...]
--
-- It also prints the compiler's syscall stubs the context points to (PS4),
-- one "stub <number> <address>" line each.
--
-- Options: no-gadget, no-search (PS4 stubs); for the release's ghostldr.lua
-- (listen mode), a fake network whose clients connect one after another:
--   client=PATH     a client that sends PATH's bytes (repeat for more clients)
--   cut=N           the last client stops after N bytes and closes
--   stall           the last client stops after cut=N bytes and goes quiet (reads time out)
--   taken=PORT      another process listens on PORT (bind fails; repeatable)
--   core=RET        the core returns RET (default 0); it takes the bundle's
--                   memory as the real one does (ext_fd, sel_fd set to -1)
--   core-takes=none the core returns before taking anything
--   sender-gone     the core's log sends fail: it closes the connection (log_fd -1)
--   noflex          sceKernelMapFlexibleMemory fails for anything but the core's pages
-- and prints what the network saw: "bind <fd> <port> <rc>", "accept <fd>",
-- "sent <fd> <line>", "close <fd>", "map <addr> <size>", "unmap <addr> <size>",
-- "dialog: <text>" (what the TV shows, "|" for line breaks),
-- "jit-socket <fd>" and "jit-close <fd>" (sockets the compiler makes on a
-- PS5), "core-call", and at the end "leak"/"jit-leak" lines for what is
-- still held and "clock <seconds>".

local payload_path, platform, fw = arg[1], arg[2], arg[3]
local opts, clients, taken = {}, {}, {}
for i = 4, #arg do
    local k, v = arg[i]:match("^([%w-]+)=(.*)$")
    if k == "client" then
        local f = assert(io.open(v, "rb"))
        clients[#clients + 1] = {data = f:read("a"), pos = 1}
        f:close()
    elseif k == "taken" then
        taken[tonumber(v)] = true
    elseif k then
        opts[k] = v
    else
        opts[arg[i]] = true
    end
end
local no_gadget = opts["no-gadget"]
if opts.cut and #clients > 0 then
    local c = clients[#clients]
    c.data = c.data:sub(1, tonumber(opts.cut))
    c.stall = opts.stall
end

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
local function wrapper(w, num)   -- its bytes, as the compiler reads them
    compiler[w] = 0xC0C748 | (num << 24) | (0x49 << 56)                -- mov rax, num; (mov r10, ...)
    compiler[w + 7] = 0xC3050FCA8949                                     -- mov r10, rcx; syscall; ret
end
if platform == "PS5" then
    jit_syscall.syscall_address = 0x800050007
else
    -- Luac0re's table: wrappers in the first 128 KiB of libkernel (no clock_gettime here)
    LIBKERNEL_BASE, JIT_LIBKERNEL_BASE = 0x810000000, 0x800060000
    jit_syscall.syscall_wrapper = {}
    for i, num in ipairs({0x14, 0x1c, 0x4, 0x6, 0x49, 0x61, 0x5, 0x36, 0x24a}) do
        jit_syscall.syscall_wrapper[num] = JIT_LIBKERNEL_BASE + 0xC0 + i * 0x40
        wrapper(JIT_LIBKERNEL_BASE + 0xC0 + i * 0x40, num)
    end
    if no_gadget then compiler[0x800060107] = 0x9090909090 end
    -- clock_gettime's wrapper past them, in both processes' libkernel
    wrapper(JIT_LIBKERNEL_BASE + 0x24000, 0xe8)
    mem_init = {[LIBKERNEL_BASE + 0x24000] = string.pack("<I3I4I3", 0xC0C748, 0xe8, 0xCA8949) .. "\x0f\x05\xc3"}
    if not opts["no-search"] then
        function sceKernelGetModuleInfoFromAddr(addr, flags, info)
            write64(info + 0x160, LIBKERNEL_BASE)
            write64(info + 0x168, 0x500000000 | 0x30000)   -- 192 KiB of text, prot 5
            return 0
        end
    end
end
function jit_read64(addr) return compiler[addr] or 0 end
function jit_memcpy(dst, src, n) write_buffer(dst, read_buffer(src, n)) end

-- ---------------------------------------------------------------- the network
local next_fd, sockets, clock_us = 7, {}, 0
local function new_fd(kind)
    next_fd = next_fd + 1
    sockets[next_fd] = {kind = kind}
    return next_fd
end
function create_socket(domain, type) return new_fd(type == SOCK_STREAM and "tcp" or "udp") end
-- a PS5 at 8.00 or later: SWRR's own socket() fails, the JIT compiler makes
-- one and hands it over; its copy stays open until closed there
local jit_open, next_jfd = {}, 100
jit_syscall.socket = function(domain, type)
    next_jfd = next_jfd + 1
    jit_open[next_jfd] = true
    print(string.format("jit-socket %d", next_jfd))
    return next_jfd
end
jit_syscall.close = function(jfd)
    print(string.format("jit-close %d", jfd))
    jit_open[jfd] = nil
    return 0
end
function jit_send_recv_fd(jfd, a, b) return new_fd("tcp") end
function get_current_ip() return "192.0.2.10" end
local function port_of(sa) return (read8(sa + 2) << 8) | read8(sa + 3) end
local listening   -- the fd that listens, while one does
syscall = {
    socket = function(domain, type)
        if platform == "PS4" then return new_fd(type == SOCK_STREAM and "tcp" or "udp") end
        return -1                       -- ENOSYS
    end,
    setsockopt = function(fd, level, name, val, len)
        local s = sockets[fd]
        if s then s["opt" .. name] = len == 16 and read64(val) or read64(val) & 0xFFFFFFFF end
        return 0
    end,
    connect = function() return 0 end,
    bind = function(fd, sa)
        local port = port_of(sa)
        local rc = taken[port] and -1 or 0
        print(string.format("bind %d %d %d", fd, port, rc))
        if rc == 0 then sockets[fd].port = port end
        return rc
    end,
    listen = function(fd)
        listening = fd
        return 0
    end,
    accept = function(fd, sa, alen)
        local c = table.remove(clients, 1)
        if not c then return -1 end
        local cfd = new_fd("conn")
        sockets[cfd].client = c
        write8(sa + 4, 192) write8(sa + 5, 0) write8(sa + 6, 2) write8(sa + 7, 7)
        print(string.format("accept %d", cfd))
        return cfd
    end,
    read = function(fd, buf, n)
        local c = sockets[fd] and sockets[fd].client
        if not c then return -1 end
        if c.pos > #c.data then return c.stall and -1 or 0 end
        n = math.min(n, 5000, #c.data - c.pos + 1)     -- a few KiB per read, as a socket gives
        write_buffer(buf, c.data:sub(c.pos, c.pos + n - 1))
        c.pos = c.pos + n
        return n
    end,
    write = function(fd, buf, n)
        print(string.format("sent %d %s", fd, (read_buffer(buf, n):gsub("\n$", ""))))
        return n
    end,
    close = function(fd)
        print(string.format("close %d", fd))
        if sockets[fd] then sockets[fd].closed = true end
        if fd == listening then listening = nil end
    end,
}
function get_error_string() return "errno : 0x0000000000000030\nerror : Address already in use" end
function send_notification(s) print("notification: " .. s) end
-- Luac0re's dialog on the TV, one line ("|" for its line breaks)
version_string, TITLE_ID = "Luac0re 2.4b by Gezine", platform == "PS4" and "CUSA03474" or "CUSA03492"
function show_dialog(text) print("dialog: " .. text:gsub("\n", " | ")) end

-- ---------------------------------------------------------------- calls
local calls, names = {}, {}
function init_dlsym() end
function dlsym(handle, name)
    calls[#calls + 1] = name
    local addr = 0x800100000 + #calls * 0x10
    names[addr] = name
    return addr
end
local flex = {}
local K = {
    poll = function(pfd, n, ms)
        if listening and #clients > 0 then
            write16(pfd + 6, 1)
            return 1
        end
        clock_us = clock_us + ms * 1000
        return 0
    end,
    sceKernelGetProcessTime = function() return clock_us end,
    sceKernelMapFlexibleMemory = function(out, size, prot, fl)
        local want = read64(out)
        if want ~= 0 then return 0 end               -- the core's data pages, at their address
        if opts.noflex then return 0x8002000c end   -- an int: the upper half of rax is the caller's
        local p = malloc(size)
        flex[p] = size
        write64(out, p)
        print(string.format("map %x %x", p, size))
        return 0
    end,
    munmap = function(addr, size)
        if flex[addr] then
            print(string.format("unmap %x %x", addr, size))
            if flex[addr] ~= size then print("FAULT unmap size") end
            flex[addr] = nil
        end
        return 0
    end,
}
function func_wrap(addr)
    return function(a1, a2, a3, a4)
        if addr == IPU_PROG_BASE + 0x4000 then -- the core: show its context
            print("core-call")
            for off = 0, 0x1F8, 8 do print(string.format("ctx %x %x", off, read64(a1 + off))) end
            local t = read64(a1 + 0x30)   -- jit_sysw (test_ps4.py checks the offset)
            while t ~= 0 and read64(t) ~= 0 do
                print(string.format("stub %x %x", read64(t), read64(t + 8)))
                t = t + 16
            end
            local flags = read64(a1 + 0x160)
            if flags & (1 << 21) ~= 0 and opts["core-takes"] ~= "none" then
                -- the core takes the bundle's memory (ext.c, sel.c) and unmaps it itself
                local E = 0x168 + 0x40
                for _, off in ipairs({E, E + 0x18}) do
                    local p = read64(a1 + off)
                    if p ~= -1 then
                        flex[p] = nil
                        write64(a1 + off, -1)
                    end
                end
            end
            if opts["sender-gone"] then
                print(string.format("core-closes %d", read64(a1 + 0x38)))
                write64(a1 + 0x38, -1)
            end
            return tonumber(opts.core or "0")
        end
        local f = K[names[addr]]
        if f then return f(a1, a2, a3, a4) end
        return 0   -- the rest
    end
end

for p, bytes in pairs(mem_init or {}) do write_buffer(p, bytes) end
dofile(payload_path)
for p, size in pairs(flex) do print(string.format("leak %x %x", p, size)) end
for jfd in pairs(jit_open) do print(string.format("jit-leak %d", jfd)) end
print(string.format("clock %d", clock_us // 1000000))
