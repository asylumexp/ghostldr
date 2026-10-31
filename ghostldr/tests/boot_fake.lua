-- A fake Luac0re for ghostldr/lua/ghostldr.lua: just enough of its globals
-- (memory, func_wrap, dlsym, the JIT exploit's values, sockets, saved data,
-- the TV's dialog) to run the bootstrap as on a PS4 or a PS5.  Calling the
-- core prints the boot context, one "ctx <offset> <value>" line per u64, and
-- the compiler-side syscall gadget it was given.  test_ps4.py and
-- test_bundle.py run it:
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
--   jit-close-fails the compiler's close() of its socket copies does nothing
--                   (Luac0re's create_socket: the port stays held)
-- the core, which takes the bundle's memory as the real one does (ext_fd,
-- sel_fd set to -1) and checks the extension's CRC-32 first (ext.c: -7):
--   core=RET[,RET]  what each boot call returns (default 0; the last repeats)
--   elfldr=ACT[,ACT] what each ELF loader call does (default quit): quit,
--                   boot, forget, bundle:PATH (a bundle came to its port),
--                   pick:INDEX:TITLE[:USER] (beside the game selector: a game
--                   was picked; the core returns 4), or a number; "log+ACT":
--                   a client took the log first, "bundle:PATH+pick:...": a
--                   bundle came while the selector showed
--   core-takes=none the core returns before taking anything
--   sender-gone     the core's log sends fail: it closes the connection (log_fd -1)
--   noflex          sceKernelMapFlexibleMemory fails for anything but the core's pages
-- saved data and the TV:
--   saves=DIR       the save containers, one directory each (default: a new
--                   empty one, removed at the end)
--   savedata0=DIR   Luac0re's own save, mounted read-only at /savedata0 (the
--                   autoloader's files: Lua's io and loadfile see it too)
--   pad=HEX         the buttons held on the controller (scePadReadState);
--                   without it libScePad does not load
--   pad-intercepted=N  the first N reads have only the system's bit (0x80000000:
--                   a dialog or overlay has the controller)
--   disk=BYTES      free space on the console's storage (default: plenty)
--   no-savedata     libSceSaveData does not load
-- It prints what it saw: "bind <fd> <port> <rc>", "accept <fd>",
-- "sent <fd> <line>", "close <fd>", "map <addr> <size>", "unmap <addr> <size>",
-- "dialog: <text>" (what the TV shows, "|" for line breaks),
-- "jit-socket <fd>" and "jit-close <fd>" (sockets the compiler makes on a
-- PS5), "core-call", "core-ext ok|crc", "elfldr port <p> can-boot <0|1>[ selecting]",
-- "elfldr listening <port>" or "elfldr-bind <port> held by
-- <who>", "save-mount <name> <mode> <blocks> <rc>", "save-umount <name>",
-- "save-delete <name> <rc>", "luac0re-loader <port>" (Luac0re's
-- remote_lua_loader started), and at the end "leak"/"jit-leak"/"listen-leak"/
-- "save-leak"/"file-leak" lines for what is still held and "clock <seconds>".

local payload_path, platform, fw = arg[1], arg[2], arg[3]
local opts, clients, taken = {}, {}, {}
for i = 4, #arg do
    local k, v = arg[i]:match("^([%w-]+)=(.*)$")
    if k == "client" then
        local f = assert(io.open(v, "rb"))
        clients[#clients + 1] = {data = f:read("a"), pos = 1}
        f:close()
    elseif k == "taken" then
        taken[#taken + 1] = tonumber(v)
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
local function list(s)
    local t = {}
    for v in (s or ""):gmatch("[^,]+") do t[#t + 1] = v end
    return t
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
function write_buffer(p, s)
    for off = 1, #s, 4096 do
        local t = {s:byte(off, math.min(off + 4095, #s))}
        for i = 1, #t do mem[p + off + i - 2] = t[i] end
    end
end
function write_string(p, s) write_buffer(p, s .. "\0") end
function read_buffer(p, n)
    local parts = {}
    for off = 0, n - 1, 4096 do
        local t, m = {}, math.min(4096, n - off)
        for i = 1, m do t[i] = mem[p + off + i - 1] or 0 end
        parts[#parts + 1] = string.char(table.unpack(t, 1, m))
    end
    return table.concat(parts)
end
function write16(p, v) write_buffer(p, string.pack("<I2", v & 0xFFFF)) end
function write32(p, v) write_buffer(p, string.pack("<I4", v & 0xFFFFFFFF)) end
function write64(p, v) write_buffer(p, string.pack("<i8", v)) end
function read32(p) return (string.unpack("<I4", read_buffer(p, 4))) end
function read64(p) return (string.unpack("<i8", read_buffer(p, 8))) end
function hex_to_binary(hex) return (hex:gsub("..", function(cc) return string.char(tonumber(cc, 16)) end)) end
local function cstring(p)
    local t = {}
    while (mem[p] or 0) ~= 0 and #t < 256 do
        t[#t + 1] = string.char(mem[p])
        p = p + 1
    end
    return table.concat(t)
end
local crc_table = {}
for i = 0, 255 do
    local c = i
    for _ = 1, 8 do c = (c >> 1) ~ (0xEDB88320 & -(c & 1)) end
    crc_table[i] = c
end
local function crc32_mem(p, n)
    local c = 0xFFFFFFFF
    for i = p, p + n - 1 do c = crc_table[(c ~ (mem[i] or 0)) & 0xFF] ~ (c >> 8) end
    return c ~ 0xFFFFFFFF
end

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
-- Sockets are objects that descriptors in SWRR and in the JIT compiler
-- refer to: one lives (holds its port) while any descriptor to it is open.
local sockets, jit_open = {}, {}        -- SWRR's descriptors, the compiler's
local next_fd, next_jfd, clock_us = 7, 100, 0
local all = {}
local function new_obj(kind)
    local o = {kind = kind, refs = 0}
    all[#all + 1] = o
    return o
end
local function new_fd(o)
    next_fd = next_fd + 1
    sockets[next_fd] = o
    o.refs = o.refs + 1
    return next_fd
end
local function release(o)
    o.refs = o.refs - 1
    if o.refs == 0 then o.dead = true end
end
-- who holds port: nil when no live socket listens on it
local function held(port)
    for _, o in ipairs(all) do
        if not o.dead and o.listening and o.port == port then
            local who = {}
            for fd, s in pairs(sockets) do if s == o and not s.closed_fds[fd] then who[#who + 1] = "swrr fd " .. fd end end
            for jfd, s in pairs(jit_open) do if s == o then who[#who + 1] = "compiler fd " .. jfd end end
            if o.other then who[#who + 1] = "another process" end
            table.sort(who)
            return table.concat(who, ", ")
        end
    end
end
local function new_socket(kind)
    local o = new_obj(kind)
    o.closed_fds = {}
    return o
end
for _, p in ipairs(taken) do
    local o = new_socket("tcp")
    o.port, o.listening, o.other, o.refs = p, true, true, 1
end
function create_socket(domain, type) return new_fd(new_socket(type == SOCK_STREAM and "tcp" or "udp")) end
-- a PS5 at 8.00 or later: SWRR's own socket() fails, the JIT compiler makes
-- one and hands it over; its copy stays open until closed there
jit_syscall.socket = function(domain, type)
    next_jfd = next_jfd + 1
    local o = new_socket("tcp")
    o.refs = 1
    jit_open[next_jfd] = o
    print(string.format("jit-socket %d", next_jfd))
    return next_jfd
end
jit_syscall.close = function(jfd)
    print(string.format("jit-close %d", jfd))
    if opts["jit-close-fails"] then return 0 end
    if jit_open[jfd] then release(jit_open[jfd]) end
    jit_open[jfd] = nil
    return 0
end
function jit_send_recv_fd(jfd, a, b) return new_fd(jit_open[jfd]) end
function get_current_ip() return "192.0.2.10" end
local function port_of(sa) return (read8(sa + 2) << 8) | read8(sa + 3) end
local listening   -- the fd that listens, while one does
syscall = {
    socket = function(domain, type)
        if platform == "PS4" then return new_fd(new_socket(type == SOCK_STREAM and "tcp" or "udp")) end
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
        local rc = held(port) and -1 or 0
        print(string.format("bind %d %d %d", fd, port, rc))
        if rc == 0 then sockets[fd].port = port end
        return rc
    end,
    listen = function(fd)
        sockets[fd].listening = true
        listening = fd
        return 0
    end,
    accept = function(fd, sa, alen)
        local c = table.remove(clients, 1)
        if not c then return -1 end
        local o = new_socket("conn")
        o.client, o.port = c, sockets[fd].port
        local cfd = new_fd(o)
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
        local o = sockets[fd]
        if o and not o.closed_fds[fd] then
            o.closed_fds[fd] = true
            o.closed = true
            release(o)
        elseif o then
            print("FAULT close of a closed fd " .. fd)
        end
        if fd == listening then listening = nil end
    end,
}
function get_error_string() return "errno : 0x0000000000000030\nerror : Address already in use" end
function send_notification(s) print("notification: " .. s) end
-- Luac0re's dialog on the TV, one line ("|" for its line breaks)
version_string, TITLE_ID = "Luac0re 2.4b by Gezine", platform == "PS4" and "CUSA03474" or "CUSA03492"
function show_dialog(text)
    print("dialog: " .. text:gsub("\n", " | "))
end

-- ---------------------------------------------------------------- saved data
-- One directory per container under `saves`; a mount point stands for it
-- until it is unmounted (sceSaveDataUmount commits it).
local own_saves = not opts.saves
local saves = opts.saves or (os.tmpname() .. ".saves")
os.execute("mkdir -p '" .. saves .. "'")
local disk = tonumber(opts.disk or "1e15") // 1
local mounts, nmount, files, next_file = {}, 0, {}, 300
local function exists(path)
    local f = io.open(path, "rb")
    if f then f:close() return true end
    return false
end
local function container(name) return saves .. "/" .. name end
local function real_path(path)
    local own = opts.savedata0 and path:match("^/savedata0/(.*)$")
    if own and not own:find("%.%.") then return opts.savedata0 .. "/" .. own, {rw = false} end
    local mp, rest = path:match("^(/savedata%d+)/(.*)$")
    if mp and mounts[mp] and not rest:find("%.%.") then return container(mounts[mp].name) .. "/" .. rest, mounts[mp] end
end
local function file_size(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local n = f:seek("end")
    f:close()
    return n
end
local SAVE = {
    sceSaveDataMount = function(params, result)
        local name, blocks, mode = cstring(read64(params + 0x10)), read64(params + 0x20), read32(params + 0x28)
        local dir, rc = container(name), 0
        local there = exists(dir .. "/.blocks")
        if not there and mode & 4 == 0 then
            rc = 0x809F0008                                     -- NOT_FOUND
        elseif not there then
            if blocks * 32768 > disk then
                rc = 0x809F000A                                 -- NO_SPACE_FS: the blocks still missing
                write64(result + 0x10, blocks - disk // 32768)
            else
                os.execute("mkdir -p '" .. dir .. "'")
                local f = io.open(dir .. "/.blocks", "w")
                f:write(tostring(blocks))
                f:close()
            end
        end
        print(string.format("save-mount %s %d %d %x", name, mode, blocks, rc))
        if rc ~= 0 then return rc end
        nmount = nmount + 1
        local mp = "/savedata" .. nmount
        local f = io.open(dir .. "/.blocks")
        mounts[mp] = {name = name, rw = mode & 2 ~= 0, blocks = tonumber(f:read("a"))}
        f:close()
        for i = 0, 15 do write8(result + i, 0) end
        write_string(result, mp)
        return 0
    end,
    sceSaveDataUmount = function(mp_ptr)
        local mp = cstring(mp_ptr)
        local m = mounts[mp]
        if not m then return 0x809F0008 end
        print("save-umount " .. m.name)
        mounts[mp] = nil
        return 0
    end,
    sceSaveDataDelete = function(params)
        local name = cstring(read64(params + 0x10))
        local dir, rc = container(name), 0x809F0008
        if exists(dir .. "/.blocks") then
            for mp, m in pairs(mounts) do
                if m.name == name then print("FAULT delete of a mounted container " .. name) end
            end
            local p = io.popen("ls -A '" .. dir .. "'")
            for f in p:lines() do os.remove(dir .. "/" .. f) end
            p:close()
            os.remove(dir)
            rc = 0
        end
        print(string.format("save-delete %s %x", name, rc))
        return rc
    end,
    sceUserServiceGetInitialUser = function(uid)
        write32(uid, 0x10000001)
        return 0
    end,
    sceKernelLoadStartModule = function(name_ptr)
        local name = cstring(name_ptr)
        if name == "libSceSaveData.sprx" then return opts["no-savedata"] and 0x80020002 or 0x50 end
        if name == "libSceUserService.sprx" then return 0x51 end
        if name == "libSceMsgDialog.sprx" then return 0x52 end
        if name == "libScePad.sprx" then return opts.pad and 0x53 or 0x80020002 end
        return 0x80020002
    end,
    sceKernelOpen = function(path_ptr, flags)
        local path, m = real_path(cstring(path_ptr))
        if not path or (flags & 3 ~= 0 and not m.rw) then return 0x80020002 end
        local f = io.open(path, flags & 3 ~= 0 and "wb" or "rb")
        if not f then return 0x80020002 end
        next_file = next_file + 1
        files[next_file] = {f = f, m = m, written = 0}
        return next_file
    end,
    sceKernelWrite = function(fd, buf, n)
        local h = files[fd]
        if not h then return -9 end
        if h.written + n > (h.m.blocks - 48) * 32768 then return 0x8002001C end   -- ENOSPC: past the container
        h.f:write(read_buffer(buf, n))
        h.written = h.written + n
        return n
    end,
    sceKernelPread = function(fd, buf, n, off)
        local h = files[fd]
        if not h then return -9 end
        h.f:seek("set", off)
        local data = h.f:read(n) or ""
        write_buffer(buf, data)
        return #data
    end,
    sceKernelClose = function(fd)
        local h = files[fd]
        if not h then return 0x80020009 end
        h.f:close()
        files[fd] = nil
        return 0
    end,
    sceKernelStat = function(path_ptr, st)
        local path = cstring(path_ptr)
        if mounts[path] then return 0 end
        local real = real_path(path)
        local size = real and file_size(real)
        if not size then return 0x80020002 end
        write64(st + 0x48, size)
        return 0
    end,
    scePadGetHandle = function(user, type, index) return user == 0x10000001 and 3 or 0x80920003 end,
    scePadReadState = function(h, data)
        pad_reads = (pad_reads or 0) + 1
        local intercepted = pad_reads <= tonumber(opts["pad-intercepted"] or "0")
        write32(data, intercepted and 0x80000000 or tonumber(opts.pad or "0", 16))
        return h == 3 and 0 or 0x80920003
    end,
    sceKernelUsleep = function(us)
        clock_us = clock_us + us
        return 0
    end,
}

-- ---------------------------------------------------------------- calls
local calls, names = {}, {}
-- Luac0re's dlsym answers only after init_dlsym() (func.lua)
local dlsym_ready = false
function init_dlsym() dlsym_ready = true end
function sceKernelUsleep(us) clock_us = clock_us + us return 0 end
function dlsym(handle, name)
    if not dlsym_ready then error("dlsym not inited") end
    calls[#calls + 1] = name
    local addr = 0x800100000 + #calls * 0x10
    names[addr] = name
    return addr
end
local SAVE_MOUNT = 0x800900000             -- SWRR's import of sceSaveDataMount (eboot+0x3893F0)
names[SAVE_MOUNT] = "sceSaveDataMount"
local flex = {}
local K = {
    poll = function(pfd, n, ms)
        local fd = read32(pfd)
        if fd == listening and #clients > 0 then
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
for name, f in pairs(SAVE) do K[name] = f end

-- ---------------------------------------------------------------- the core
local CORE_RESULTS, ELFLDR_ACTS = list(opts.core or "0"), list(opts.elfldr or "quit")
local ncore, nelf = 0, 0
local LOG_FD, FLAGS, E = 0x38, 0x160, 0x168 + 0x40
local function take_mem(p)            -- the core unmaps what it takes
    if flex[p] then flex[p] = nil end
end
-- the ELF loader hands the log to conn (el_log_to): the previous one, if another, is closed
local function el_log_to(ctx, conn)
    local old = read64(ctx + LOG_FD)
    write64(ctx + LOG_FD, conn)
    if old >= 0 and old ~= conn then syscall.close(old) end
end
-- the extension's ELF loader (ext/src/elfldr.c) as ghostldr.lua sees it:
-- it listens on its port (a socket the compiler made and handed over, with
-- SO_REUSEADDR and SO_REUSEPORT) and returns what the client did
local function fake_elfldr(ctx, flags)
    local port, can_boot, selecting = flags >> 32 & 0xFFFF, flags >> 56 & 1, flags >> 57 & 1
    print(string.format("elfldr port %d can-boot %d%s", port, can_boot, selecting == 1 and " selecting" or ""))
    local who = held(port)
    if who then
        print(string.format("elfldr-bind %d held by %s", port, who))
        return -48                                              -- EADDRINUSE
    end
    local l = new_socket("tcp")
    l.port, l.listening, l.refs = port, true, 1
    print(string.format("elfldr listening %d", port))
    nelf = nelf + 1
    local rc = 0
    for step in ELFLDR_ACTS[math.min(nelf, #ELFLDR_ACTS)]:gmatch("[^+]+") do
        if step == "log" then
            local o = new_socket("conn")
            o.port = port
            local c = new_fd(o)
            print(string.format("elfldr-log %d", c))
            el_log_to(ctx, c)
        elseif step == "quit" then
            rc = 0
        elseif step == "boot" then
            rc = 1
        elseif step == "forget" then
            rc = 3
        elseif step == "close" then   -- the selector's Settings: Close ghostldr (SEL_EXIT_LOADER)
            rc = -20
        elseif step:match("^pick:") then
            local index, title, user = step:match("^pick:(-?%d+):([^:]*):?(%x*)$")
            local blk = read64(ctx + 0x210)
            write32(blk + 0x18, tonumber(index) & 0xFFFFFFFF)
            write32(blk + 0x1c, user ~= "" and tonumber(user, 16) or 0)
            write_buffer(blk + 0x20, title .. string.rep("\0", 16 - #title))
            print(string.format("elfldr-pick %s %s", index, title))
            rc = 4
        elseif step:match("^bundle:") then
            local f = assert(io.open(step:sub(8), "rb"))
            local data = f:read("a")
            f:close()
            local ext_size, sel_size = string.unpack("<I8I8", data, 25)
            local ext_crc, sel_crc = string.unpack("<I4I4", data, 41)
            local function part(at, size)
                if size == 0 then return -1 end
                local p = malloc(size)
                flex[p] = (size + 0x3FFF) & ~0x3FFF
                print(string.format("map %x %x", p, flex[p]))
                write_buffer(p, data:sub(at, at + size - 1))
                return p
            end
            local ext, sel = part(129, ext_size), part(129 + ext_size, sel_size)
            if read64(ctx + 8) >= 0x210 then write_buffer(read64(ctx + 0x208), data:sub(1, 128)) end
            write64(ctx + E, ext)
            write64(ctx + E + 0x08, ext_size)
            write64(ctx + E + 0x10, ext_crc)
            write_buffer(ctx + 0x1E0, data:sub(49, 72))
            write64(ctx + E + 0x18, sel)
            write64(ctx + E + 0x20, sel_size)
            write64(ctx + E + 0x28, sel_crc)
            local o = new_socket("conn")
            o.port = port
            local c = new_fd(o)
            print(string.format("elfldr-bundle %d", c))
            el_log_to(ctx, c)
            rc = 2
        else
            rc = tonumber(step)
        end
    end
    release(l)
    return rc
end

local function fake_core(ctx)
    print("core-call")
    for off = 0, 0x210, 8 do print(string.format("ctx %x %x", off, read64(ctx + off))) end
    local blk = read64(ctx + 8) >= 0x218 and read64(ctx + 0x210) or 0
    if blk ~= 0 then   -- the game selector's block (ghost.h struct el_select): index, user, title, boot
        print(string.format("select %d %x %s %d", string.unpack("<i4", read_buffer(blk + 0x18, 4)), read32(blk + 0x1c),
            (read_buffer(blk + 0x20, 16):gsub("%z.*", "")), read64(blk + 0x30)))
    end
    local t = read64(ctx + 0x30)   -- jit_sysw (test_ps4.py checks the offset)
    while t ~= 0 and read64(t) ~= 0 do
        print(string.format("stub %x %x", read64(t), read64(t + 8)))
        t = t + 16
    end
    local flags = read64(ctx + FLAGS)
    local elf = flags & (1 << 20) ~= 0
    if flags & (1 << 21) ~= 0 and opts["core-takes"] ~= "none" then
        -- the bundle's memory (ext.c, then sel.c in a boot): checked, then the core's to unmap
        local p = read64(ctx + E)
        if p ~= -1 then
            local ok = crc32_mem(p, read64(ctx + E + 8)) == read64(ctx + E + 0x10) & 0xFFFFFFFF
            take_mem(p)
            write64(ctx + E, -1)
            print("core-ext " .. (ok and "ok" or "crc"))
            if not ok then return -7 end
        end
        local s = read64(ctx + E + 0x18)
        if (not elf or flags & (1 << 57) ~= 0) and s ~= -1 then     -- sel.c takes it, beside the ELF loader too
            take_mem(s)
            write64(ctx + E + 0x18, -1)
        end
    end
    if opts["sender-gone"] then
        print(string.format("core-closes %d", read64(ctx + LOG_FD)))
        write64(ctx + LOG_FD, -1)
    end
    if elf then return fake_elfldr(ctx, flags) end
    ncore = ncore + 1
    return tonumber(CORE_RESULTS[math.min(ncore, #CORE_RESULTS)])
end

function func_wrap(addr)
    return function(a1, a2, a3, a4)
        if addr == IPU_PROG_BASE + 0x4000 then return fake_core(a1) end
        local f = K[names[addr]]
        if f then return f(a1, a2, a3, a4) end
        return 0   -- the rest
    end
end

-- Luac0re's own save at /savedata0, for Lua's io too
if opts.savedata0 then
    local io_open, lua_loadfile = io.open, loadfile
    local function own(path) return path:gsub("^/savedata0/", opts.savedata0 .. "/") end
    io.open = function(path, mode) return io_open(own(path), mode) end
    loadfile = function(path, ...) return lua_loadfile(own(path), ...) end
end
function file_exists(path)
    local f = io.open(path, "r")
    if f then f:close() return true end
    return false
end
function remote_lua_loader(port) print(string.format("luac0re-loader %d", port)) end

for p, bytes in pairs(mem_init or {}) do write_buffer(p, bytes) end
write64(EBOOT_BASE + 0x3893F0, SAVE_MOUNT)
dofile(payload_path)
for p, size in pairs(flex) do print(string.format("leak %x %x", p, size)) end
for jfd in pairs(jit_open) do print(string.format("jit-leak %d", jfd)) end
for _, o in ipairs(all) do
    if not o.dead and o.listening and not o.other then print(string.format("listen-leak %d", o.port)) end
end
for mp, m in pairs(mounts) do print("save-leak " .. m.name) end
for fd in pairs(files) do print("file-leak " .. fd) end
if own_saves then os.execute("rm -rf '" .. saves .. "'") end
print(string.format("clock %d", clock_us // 1000000))
