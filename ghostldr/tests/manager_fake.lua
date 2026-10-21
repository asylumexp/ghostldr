-- A fake console for ghostldr/lua/manager.lua: Luac0re's globals (memory,
-- func_wrap, dlsym, syscall, jit sockets), libkernel's file calls, SWRR's
-- save service and the TCP connections a browser would make, all in Lua.
-- test_manager.py writes a scenario and reads what this leaves in <outdir>.
--
--   lua manager_fake.lua <manager.lua> <scenario.lua> <outdir>
--
-- scenario: {title = "CUSA03474", scratch = "GLDRT", no_space = false,
--   containers = {{name =, blocks =, mtime =, files = {[path] = data}, dirs = {path, ...}}},
--   requests = {{data = raw request, stall = true (recv times out at the end),
--                close_after = n (the peer goes away after n response bytes), chunk = n}}}

local manager_path, scenario_path, outdir = arg[1], arg[2], arg[3]
MANAGER_TEST = true
local H = dofile(manager_path)       -- the helpers (json for the report)
MANAGER_TEST = nil
local SC = dofile(scenario_path)

-- ---------------------------------------------------------------- memory
local regions, next_base = {}, 0x100000000
local function alloc(size)
    local r = {base = next_base, size = size, bytes = {}}
    next_base = next_base + ((size + 0xFFFF) // 0x10000 + 1) * 0x10000
    regions[#regions + 1] = r
    return r
end
local function locate(addr, n)
    for i = #regions, 1, -1 do
        local r = regions[i]
        if addr >= r.base and addr < r.base + r.size then
            assert(addr - r.base + (n or 1) <= r.size, string.format("fake: access past the end of %x", r.base))
            return r, addr - r.base
        end
    end
    error(string.format("fake: bad address %x", addr))
end
function malloc(n) return alloc(math.max(n, 1)).base end
function read_buffer(p, n)
    local r, o = locate(p, n)
    local t = {}
    for i = 0, n - 1 do t[i + 1] = string.char(r.bytes[o + i] or 0) end
    return table.concat(t)
end
function write_buffer(p, s)
    local r, o = locate(p, #s)
    for i = 1, #s do r.bytes[o + i - 1] = s:byte(i) end
end
function write_string(p, s) write_buffer(p, s .. "\0") end
function memset(p, c, n)
    local r, o = locate(p, n)
    for i = 0, n - 1 do r.bytes[o + i] = c ~= 0 and c or nil end
    return p
end
function read64(p) return (string.unpack("<i8", read_buffer(p, 8))) end
function read32(p) return (string.unpack("<I4", read_buffer(p, 4))) end
function read16(p) return (string.unpack("<I2", read_buffer(p, 2))) end
function read8(p) return read_buffer(p, 1):byte() end
function write64(p, v) write_buffer(p, string.pack("<i8", v)) end
function write32(p, v) write_buffer(p, string.pack("<I4", v & 0xFFFFFFFF)) end
function write16(p, v) write_buffer(p, string.pack("<I2", v & 0xFFFF)) end
function write8(p, v) write_buffer(p, string.char(v & 0xFF)) end
local strings = {}
function addrof(s)
    if not strings[s] then
        local r = alloc(#s + 0x21)
        write_buffer(r.base + 0x20, s)
        strings[s] = r.base
    end
    return strings[s]
end
local function cstr(p, max)
    local r, o = locate(p)
    local s = read_buffer(p, math.min(max or 256, r.size - o))
    return s:match("^[^%z]*")
end

-- ---------------------------------------------------------------- report
local R = {calls = {}, notes = {}, logs = {}, violations = {}, outputs = {}}
local function call(op, name, extra) R.calls[#R.calls + 1] = {op = op, name = name, extra = extra} end
local function violation(msg) R.violations[#R.violations + 1] = msg end

-- ---------------------------------------------------------------- natives
local natives, by_name, code = {}, {}, 0x7000000000
local function native(name, fn)
    code = code + 16
    natives[code] = fn
    if name then by_name[name] = code end
    return code
end
function func_wrap(addr)
    local fn = natives[addr]
    assert(fn, string.format("fake: call to %x", addr))
    return function(...)
        local a = table.pack(...)
        for i = 1, a.n do if type(a[i]) == "string" then a[i] = addrof(a[i]) + 0x20 end end
        return fn(table.unpack(a, 1, a.n))
    end
end
LIBKERNEL_HANDLE = 0x2001
local modules = {["libSceSaveData.sprx"] = 0x51, ["libSceUserService.sprx"] = 0x52}
function init_dlsym() end
function dlsym(handle, name) return by_name[name] or 0 end
EBOOT_BASE = alloc(0x400000).base
AF_INET, SOCK_STREAM, SOCK_DGRAM, SOL_SOCKET, SO_REUSEADDR = 2, 1, 2, 0xffff, 4
TITLE_ID = SC.title or "CUSA03474"
NEW_JIT_SOCK, NEW_MAIN_SOCK = 7, 8
local OWN = H.SWRR[TITLE_ID]
local USER = 0x1234abcd
local clock = 1000000

-- ---------------------------------------------------------------- file systems
local E = {NOENT = 0x80020002, ACCES = 0x8002000D, EXIST = 0x80020011, NOSPC = 0x8002001C, ROFS = 0x8002001E,
           NOTDIR = 0x80020014, NOTEMPTY = 0x80020042, BADF = 0x80020009}
local SD = {PARAMETER = 0x809F0000, BUSY = 0x809F0003, NOT_MOUNTED = 0x809F0004, EXISTS = 0x809F0007,
            NOT_FOUND = 0x809F0008, NO_SPACE_FS = 0x809F000A, MOUNT_FULL = 0x809F000C}
local containers = {}
local function new_container(name, blocks, mtime)
    local c = {name = name, blocks = blocks, mtime = mtime or 1700000000,
               files = {["sce_sys/param.sfo"] = string.rep("P", 1024)}, dirs = {["sce_sys"] = true}}
    containers[name] = c
    return c
end
for _, c in ipairs(SC.containers or {}) do
    local x = new_container(c.name, c.blocks or 96, c.mtime)
    for p, data in pairs(c.files or {}) do
        x.files[p] = data
        local parent = p:match("^(.*)/[^/]+$")
        while parent do x.dirs[parent] = true; parent = parent:match("^(.*)/[^/]+$") end
    end
    for _, d in ipairs(c.dirs or {}) do x.dirs[d] = true end
end
local temp = {name = "/temp0", blocks = 1 << 20, files = {}, dirs = {}, temp = true}
local mounts = {}            -- "/savedataN" -> {c, rw}
if OWN and containers[OWN] then mounts["/savedata0"] = {c = containers[OWN], rw = true, swrr = true} end

local function used(c)
    local n = c.temp and 0 or 34              -- the file system's own blocks (measured)
    for _, d in pairs(c.files) do n = n + math.max(1, (#d + 32767) // 32768) end
    for _ in pairs(c.dirs) do n = n + 1 end
    return n
end

-- path -> filesystem, relative path, mount
local function resolve(p)
    local mp, rest = p:match("^(/savedata%d+)/?(.*)$")
    if mp then
        local m = mounts[mp]
        if not m then return nil end
        return m.c, rest, m
    end
    rest = p:match("^/temp0/?(.*)$")
    if rest then return temp, rest, {rw = true} end
    return nil
end

local fds, next_fd = {}, 20
local function newfd(t) next_fd = next_fd + 1; fds[next_fd] = t; return next_fd end

native("sceKernelOpen", function(pp, flags)
    local p = cstr(pp, 512)
    local fs, rel, m = resolve(p)
    if not fs then return E.NOENT end
    if flags & 0x20000 ~= 0 then
        if rel ~= "" and not fs.dirs[rel] then return E.NOENT end
        return newfd({dir = true, fs = fs, rel = rel, m = m})
    end
    local acc = flags & 3
    if fs.dirs[rel] then return E.ACCES end
    if acc ~= 0 then
        if not m.rw then return E.ROFS end
        if H.is_system(rel) and not fs.temp then return E.ACCES end
        local parent = rel:match("^(.*)/[^/]+$")
        if parent and not fs.dirs[parent] then return E.NOENT end
        if fs.files[rel] == nil then
            if flags & 0x200 == 0 then return E.NOENT end
            fs.files[rel] = ""
        elseif flags & 0x400 ~= 0 then
            fs.files[rel] = ""
        end
    elseif fs.files[rel] == nil then
        return E.NOENT
    end
    return newfd({fs = fs, rel = rel, m = m, pos = 0, rw = acc ~= 0})
end)
local function rw_read(fd, p, n, off)
    local f = fds[fd]
    if not f or f.dir then return E.BADF end
    local data = f.fs.files[f.rel]
    local start = off or f.pos
    local chunk = data:sub(start + 1, start + n)
    if #chunk > 0 then write_buffer(p, chunk) end
    if not off then f.pos = f.pos + #chunk end
    return #chunk
end
native("sceKernelRead", function(fd, p, n) return rw_read(fd, p, n) end)
native("sceKernelPread", function(fd, p, n, off) return rw_read(fd, p, n, off) end)
native("sceKernelWrite", function(fd, p, n)
    local f = fds[fd]
    if not f or not f.rw then return E.BADF end
    local data = f.fs.files[f.rel]
    local chunk = read_buffer(p, n)
    local new = data:sub(1, f.pos) .. string.rep("\0", math.max(0, f.pos - #data)) .. chunk .. data:sub(f.pos + n + 1)
    local old = f.fs.files[f.rel]
    f.fs.files[f.rel] = new
    if not f.fs.temp and used(f.fs) > f.fs.blocks then
        f.fs.files[f.rel] = old
        return E.NOSPC
    end
    f.pos = f.pos + n
    return n
end)
native("sceKernelClose", function(fd)
    if not fds[fd] then return E.BADF end
    fds[fd] = nil
    return 0
end)
native("sceKernelStat", function(pp, stp)
    local p = cstr(pp, 512)
    if mounts[p] then
        memset(stp, 0, 0x78)
        write16(stp + 8, 0x41ff)
        return 0
    end
    local fs, rel = resolve(p)
    if not fs then return E.NOENT end
    memset(stp, 0, 0x78)
    if rel == "" or fs.dirs[rel] then
        write16(stp + 8, 0x41ff)
    elseif fs.files[rel] then
        write16(stp + 8, 0x81ff)
        write64(stp + 0x48, #fs.files[rel])
    else
        return E.NOENT
    end
    write64(stp + 0x28, (fs.mtime or 1700000000) + 60)
    return 0
end)
native("sceKernelGetdents", function(fd, p, n)
    local f = fds[fd]
    if not f or not f.dir then return E.BADF end
    if f.done then return 0 end
    f.done = true
    local names = {".", ".."}
    local prefix = f.rel == "" and "" or f.rel .. "/"
    local kids = {}
    for path in pairs(f.fs.files) do
        local rest = path:sub(1, #prefix) == prefix and path:sub(#prefix + 1)
        if rest and rest ~= "" and not rest:find("/", 1, true) then kids[#kids + 1] = {rest, 8} end
    end
    for path in pairs(f.fs.dirs) do
        local rest = path:sub(1, #prefix) == prefix and path:sub(#prefix + 1)
        if rest and rest ~= "" and not rest:find("/", 1, true) then kids[#kids + 1] = {rest, 4} end
    end
    local out = {}
    for _, nm in ipairs(names) do out[#out + 1] = {nm, 4} end
    for _, k in ipairs(kids) do out[#out + 1] = k end
    local blob = {}
    for i, e in ipairs(out) do
        local reclen = (8 + #e[1] + 1 + 3) & ~3
        blob[#blob + 1] = string.pack("<I4I2BB", i, reclen, e[2], #e[1]) .. e[1] .. string.rep("\0", reclen - 8 - #e[1])
    end
    local s = table.concat(blob)
    assert(#s <= n, "fake: getdents buffer too small")
    write_buffer(p, s)
    return #s
end)
native("sceKernelMkdir", function(pp)
    local fs, rel, m = resolve(cstr(pp, 512))
    if not fs then return E.NOENT end
    if not m.rw then return E.ROFS end
    if fs.dirs[rel] or fs.files[rel] then return E.EXIST end
    fs.dirs[rel] = true
    return 0
end)
native("sceKernelUnlink", function(pp)
    local fs, rel, m = resolve(cstr(pp, 512))
    if not fs or fs.files[rel] == nil then return E.NOENT end
    if not m.rw then return E.ROFS end
    if H.is_system(rel) and not fs.temp then return E.ACCES end
    fs.files[rel] = nil
    return 0
end)
native("sceKernelRmdir", function(pp)
    local fs, rel, m = resolve(cstr(pp, 512))
    if not fs or not fs.dirs[rel] then return E.NOENT end
    if not m.rw then return E.ROFS end
    for path in pairs(fs.files) do if path:sub(1, #rel + 1) == rel .. "/" then return E.NOTEMPTY end end
    for path in pairs(fs.dirs) do if path:sub(1, #rel + 1) == rel .. "/" then return E.NOTEMPTY end end
    fs.dirs[rel] = nil
    return 0
end)
native("sceKernelUsleep", function() clock = clock + 1000 return 0 end)
native("sceKernelGetProcessTime", function() clock = clock + 137 return clock end)
native("sceKernelReadTsc", function() clock = clock + 41 return clock * 3 end)
native("sceKernelLoadStartModule", function(pp) return modules[cstr(pp)] or 0x80020002 end)
native("sceUserServiceGetInitialUser", function(p) write32(p, USER) return 0 end)

-- ---------------------------------------------------------------- save service
local function mounted_name(name)
    for mp, m in pairs(mounts) do if m.c.name == name then return mp end end
end
local mount_fn = native(nil, function(params, result)
    local name = cstr(read64(params + 0x10), 32)
    local blocks, mode = read64(params + 0x20), read32(params + 0x28)
    call("mount", name, mode)
    if read32(params) ~= USER then return 0x809F0011 end
    if H.PROTECTED[name] then violation("mount " .. name) end
    if mounted_name(name) then return SD.BUSY end
    local c = containers[name]
    if mode & 4 ~= 0 then
        if c then return SD.EXISTS end
        if blocks < 96 or blocks > 32768 then return SD.PARAMETER end
        if SC.no_space then
            write64(result + 0x10, blocks + 7)
            return SD.NO_SPACE_FS
        end
        c = new_container(name, blocks, 1800000000)
    elseif not c then
        return SD.NOT_FOUND
    end
    local n = 1
    while mounts["/savedata" .. n] do n = n + 1 end
    if n > 15 then return SD.MOUNT_FULL end
    mounts["/savedata" .. n] = {c = c, rw = mode & 2 ~= 0}
    memset(result, 0, 64)
    write_string(result, "/savedata" .. n)
    return 0
end)
native("sceSaveDataUmount", function(p)
    local mp = cstr(p, 16)
    call("umount", mounts[mp] and mounts[mp].c.name or mp)
    if mp == "/savedata0" then violation("umount /savedata0") return SD.BUSY end
    if not mounts[mp] then return SD.NOT_MOUNTED end
    for _, f in pairs(fds) do if f.m == mounts[mp] then return SD.BUSY end end
    mounts[mp] = nil
    return 0
end)
native("sceSaveDataDelete", function(p)
    local name = cstr(read64(p + 0x10), 32)
    call("delete", name)
    if H.PROTECTED[name] then violation("delete " .. name) end
    if not containers[name] then return SD.NOT_FOUND end
    if mounted_name(name) then return SD.BUSY end
    containers[name] = nil
    return 0
end)
local search_fn = native(nil, function(cond, res)
    if read32(cond) ~= USER then return 0x809F0011 end
    local names = {}
    for n in pairs(containers) do names[#names + 1] = n end
    table.sort(names)
    local cap = read32(res + 0x10)
    local dn, params, infos = read64(res + 0x08), read64(res + 0x18), read64(res + 0x20)
    write32(res, #names)
    local set = math.min(#names, cap)
    write32(res + 0x14, set)
    for i = 0, set - 1 do
        local c = containers[names[i + 1]]
        memset(dn + 32 * i, 0, 32)
        write_string(dn + 32 * i, c.name)
        write64(params + 0x530 * i + 0x508, c.mtime)
        write64(infos + 0x30 * i, c.blocks)
        write64(infos + 0x30 * i + 8, math.max(0, c.blocks - used(c)))
    end
    return 0
end)
write64(EBOOT_BASE + 0x3893F0, mount_fn)
write64(EBOOT_BASE + 0x3893C0, search_fn)

-- ---------------------------------------------------------------- sockets
local conns, queue, log_socket = {}, {}, nil
for i, r in ipairs(SC.requests or {}) do queue[#queue + 1] = {i = i, data = r.data, pos = 0, out = {}, sent = 0,
                                                              stall = r.stall, close_after = r.close_after,
                                                              chunk = r.chunk} end
local bound_port
syscall = {}
function syscall.setsockopt() return 0 end
function syscall.bind(fd, sa)
    local port = (read8(sa + 2) << 8) | read8(sa + 3)
    for _, busy in ipairs(SC.busy_ports or {}) do if busy == port then return -1 end end
    bound_port = port
    return 0
end
function syscall.listen() return 0 end
function syscall.accept()
    local c = table.remove(queue, 1)
    if not c then return -1 end
    if c.data:find("{{TOKEN}}", 1, true) or c.data:find("{{token}}", 1, true) then
        local tok = "NONE-NONE"
        for _, line in ipairs(R.logs) do tok = line:match(" token (%S+)$") or tok end
        c.data = c.data:gsub("{{TOKEN}}", tok):gsub("{{token}}", tok:lower())
    end
    next_fd = next_fd + 1
    conns[next_fd] = c
    return next_fd
end
function syscall.recvfrom(fd, p, n, flags)
    local c = conns[fd]
    if not c then return -1 end
    local left = #c.data - c.pos
    if left <= 0 then return c.stall and -1 or 0 end
    local take = math.min(n, left)
    if flags & 0x40 == 0 and c.chunk then take = math.min(take, c.chunk) end
    write_buffer(p, c.data:sub(c.pos + 1, c.pos + take))
    c.pos = c.pos + take
    return take
end
function syscall.close(fd)
    if conns[fd] then conns[fd].closed = true end
    return 0
end
native("sendto", function(fd, p, n, flags)
    if fd == log_socket then
        R.logs[#R.logs + 1] = read_buffer(p, n)
        return n
    end
    local c = conns[fd]
    if not c then return -1 end
    if flags & 0x20000 == 0 then violation("send without MSG_NOSIGNAL") end
    if c.close_after and c.sent >= c.close_after then return -1 end
    c.out[#c.out + 1] = read_buffer(p, n)
    c.sent = c.sent + n
    return n
end)
jit_syscall = {}
function jit_syscall.socket(_, kind) return kind == SOCK_DGRAM and 901 or 900 end
function jit_syscall.close() return 0 end
local handed = 5000
function jit_send_recv_fd(jfd)
    handed = handed + 1
    if jfd == 901 then log_socket = handed end
    return handed
end
function send_notification(s) R.notes[#R.notes + 1] = s end
function get_current_ip() return "192.0.2.10" end

-- ---------------------------------------------------------------- run
MANAGER_PORT = SC.port or 8090
MANAGER_LOG_IP, MANAGER_LOG_PORT = "192.0.2.1", 18196
MANAGER_SCRATCH = SC.scratch
MANAGER_TITLES = SC.titles
local ok, err = pcall(dofile, manager_path)
R.error = not ok and tostring(err) or nil
R.port = bound_port

-- ---------------------------------------------------------------- report
local function hex(s) return (s:gsub(".", function(ch) return string.format("%02x", ch:byte()) end)) end
local out = {}
for name, c in pairs(containers) do
    local files = H.obj{}
    for p, d in pairs(c.files) do files[p] = hex(d) end
    local dirs = {}
    for d in pairs(c.dirs) do dirs[#dirs + 1] = d end
    table.sort(dirs)
    out[name] = H.obj{blocks = c.blocks, files = files, dirs = dirs}
end
R.containers = H.obj(out)
local still = {}
for mp, m in pairs(mounts) do if not m.swrr then still[#still + 1] = mp .. " " .. m.c.name end end
R.still_mounted = still
local open = 0
for _ in pairs(fds) do open = open + 1 end
R.open_fds = open
local tf = H.obj{}
for p, d in pairs(temp.files) do tf[p] = #d end
R.temp_files = tf
for i, c in pairs(conns) do
    local f = assert(io.open(string.format("%s/resp_%03d.bin", outdir, c.i), "wb"))
    f:write(table.concat(c.out))
    f:close()
    c.closed = c.closed or false
end
local unclosed = 0
for _, c in pairs(conns) do if not c.closed then unclosed = unclosed + 1 end end
R.unclosed_connections = unclosed
for _, cl in ipairs(R.calls) do setmetatable(cl, getmetatable(H.obj{})) end
local f = assert(io.open(outdir .. "/report.json", "wb"))
f:write(H.json(H.obj(R)))
f:close()
