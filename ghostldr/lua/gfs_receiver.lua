-- ghostfs installer, console side: receives save containers over TCP and
-- writes them into SWRR's own save data (no kernel exploit needed).
-- Started through Luac0re's remote loader by ghostldr/host/gfs_install.py,
-- which prepends GFS_PORT, PROBE_LOG_IP and PROBE_LOG_PORT.
--
-- Request (96 bytes, little endian): "GFSI", op u32, blocks u64, size u64,
-- dirName[32], fileName[32], 8 reserved.  Ops: 1 PUT (delete + create the
-- container with `blocks`, write `size` bytes that follow into fileName,
-- commit, verify), 2 QUERY (size of fileName, -1 if missing), 3 BYE,
-- 4 DELETE, 5 READ (up to `size` bytes of fileName from offset `blocks`; the
-- reply is followed by `value` bytes), 6 WRITE (the `size` bytes that follow
-- become fileName in an existing container, committed).
-- Reply (32 bytes): "GFSR", status s32, value u64, millis u64, 8 pad.
-- Containers named *SAVE hold a title's progress: PUT, DELETE and WRITE
-- refuse them, as they refuse SWRR's own save.
--
-- Save containers: sceSaveDataMount from SWRR's import table (eboot+0x3893F0),
-- params userId@0, dirName*@0x10, blocks@0x20, mode@0x28 (RO 1, RW 2,
-- CREATE 4), at most 32768 blocks (1 GiB).  File calls must be libkernel's
-- own exports, or the commit (unmount) stays BUSY for good.

init_dlsym()
local sendto = func_wrap(dlsym(LIBKERNEL_HANDLE, "sendto"))
local log_fd = create_socket(AF_INET, SOCK_DGRAM, 0)
local laddr = malloc(16)
for i = 0, 15 do write8(laddr + i, 0) end
write8(laddr + 1, AF_INET)
write16(laddr + 2, ((PROBE_LOG_PORT << 8) | (PROBE_LOG_PORT >> 8)) & 0xFFFF)
local a, b, c, d = PROBE_LOG_IP:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
write32(laddr + 4, (tonumber(d) << 24) | (tonumber(c) << 16) | (tonumber(b) << 8) | tonumber(a))
local line_buf = malloc(1024)
local function say(s)
    s = "[gfs-receiver] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, laddr, 16)
end
local function hex(v) return string.format("%x", v & 0xFFFFFFFF) end
-- Luac0re's buffer reader writes into a string.rep() result in place; short
-- strings are interned, so its results never compare equal to literals.
local function cstring(p, max)
    local out = {}
    for i = 0, max - 1 do
        local ch = read8(p + i)
        if ch == 0 then break end
        out[#out + 1] = string.char(ch)
    end
    return table.concat(out)
end

local kopen = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelOpen"))
local kwrite = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelWrite"))
local kpread = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelPread"))
local kclose = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelClose"))
local kstat = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelStat"))
local usleep = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUsleep"))
local gettime = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelGetProcessTime"))
local load_start = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
local name = malloc(128)
write_string(name, "libSceSaveData.sprx")
local sd = load_start(name, 0, 0, 0, 0, 0)
write_string(name, "libSceUserService.sprx")
local us = load_start(name, 0, 0, 0, 0, 0)
local umount = func_wrap(dlsym(sd, "sceSaveDataUmount"))
local delete = func_wrap(dlsym(sd, "sceSaveDataDelete"))
local mount = func_wrap(read64(EBOOT_BASE + 0x3893F0))
local uid_buf = malloc(8)
func_wrap(dlsym(us, "sceUserServiceGetInitialUser"))(uid_buf)
local user = read32(uid_buf)

local dir, params, result, st, path, mp_buf, del = malloc(32), malloc(128), malloc(64), malloc(0x100), malloc(256), malloc(16), malloc(64)
local function set_dir(n)
    for i = 0, 31 do write8(dir + i, 0) end
    write_string(dir, n)
end
local function do_mount(mode, blocks)
    for i = 0, 120, 8 do write64(params + i, 0) end
    for i = 0, 56, 8 do write64(result + i, 0) end
    write32(params + 0x00, user)
    write64(params + 0x10, dir)
    write64(params + 0x20, blocks)
    write32(params + 0x28, mode)
    local rc = mount(params, result)
    return rc, cstring(result, 16)
end
local function do_umount(mp)
    for i = 0, 15 do write8(mp_buf + i, 0) end
    write_string(mp_buf, mp)
    local rc = umount(mp_buf)
    local polls = 0
    while polls < 240 and kstat(mp_buf, st) == 0 do
        usleep(250000)
        polls = polls + 1
    end
    return rc
end
local function do_delete()
    for i = 0, 56, 8 do write64(del + i, 0) end
    write32(del + 0x00, user)
    write64(del + 0x10, dir)
    return delete(del)
end

-- TCP
local CHUNK = 4 * 1024 * 1024
local buf = malloc(CHUNK)
local hdr = malloc(96)
local reply = malloc(32)
local lfd = create_socket(AF_INET, SOCK_STREAM, 0)
local one = malloc(8)
write32(one, 1)
syscall.setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, one, 4)
local rcvbuf = malloc(8)
local SO_RCVBUF = 0x1002
for _, mib in ipairs({4, 2, 1}) do      -- a single stream is window-bound (4 MB/s at the default)
    write32(rcvbuf, mib * 1024 * 1024)
    local r = syscall.setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, rcvbuf, 4)
    say(string.format("SO_RCVBUF %d MiB -> %s", mib, hex(r)))
    if r == 0 then break end
end
local sa = malloc(16)
for i = 0, 15 do write8(sa + i, 0) end
write8(sa + 0, 16)
write8(sa + 1, AF_INET)
write16(sa + 2, ((GFS_PORT << 8) | (GFS_PORT >> 8)) & 0xFFFF)
local brc = syscall.bind(lfd, sa, 16)
local lrc = syscall.listen(lfd, 1)
say(string.format("listening on %d (socket %d, bind %s, listen %s), user %x", GFS_PORT, lfd, hex(brc), hex(lrc), user))

-- every Lua->native call is slow (a ROP round trip), so take whole 4 MiB
-- chunks per call: MSG_WAITALL with an 8 MiB receive buffer.  Plain read()
-- returns a few KiB at a time and ran at 4 MB/s.
local function recv_exact(fd, p, n)
    local got = 0
    while got < n do
        local r = syscall.recvfrom(fd, p + got, n - got, 0x40, 0, 0)   -- MSG_WAITALL
        if r <= 0 or r > n - got then
            say(string.format("read(%d) -> %s after %d of %d bytes", fd, tostring(r), got, n))
            return got
        end
        got = got + r
    end
    return got
end
local function send_reply(fd, status, value, millis)
    for i = 0, 24, 8 do write64(reply + i, 0) end
    write_string(reply, "GFSR")
    write32(reply + 4, status & 0xFFFFFFFF)
    write64(reply + 8, value)
    write64(reply + 16, millis)
    syscall.write(fd, reply, 32)
end

local function put(cfd, blocks, size, dname, fname)
    set_dir(dname)
    do_delete()
    local rc, mp = do_mount(6, blocks)
    if rc ~= 0 then
        say(string.format("PUT %s: create -> %s", dname, hex(rc)))
        return rc, 0, false
    end
    write_string(path, mp .. "/" .. fname)
    local fd = kopen(path, 0x601, 0x1b6)
    local t0, written, ok = gettime(), 0, true
    while written < size do
        local want = math.min(CHUNK, size - written)
        local got = recv_exact(cfd, buf, want)
        if got ~= want then
            say(string.format("PUT %s: connection lost after %d MiB", dname, (written + got) >> 20))
            ok = false
            break
        end
        local n = kwrite(fd, buf, want)
        if n ~= want then
            say(string.format("PUT %s: write -> %s after %d MiB", dname, hex(n), written >> 20))
            ok = false
            break
        end
        written = written + want
        if written % (256 << 20) == 0 then
            local sec = (gettime() - t0) / 1e6
            say(string.format("PUT %s: %d MiB, %.1f MB/s", dname, written >> 20, written / 1e6 / sec))
        end
    end
    local crc = kclose(fd)
    local urc = do_umount(mp)
    local millis = (gettime() - t0) // 1000
    if not ok then return -1, written, false end
    if crc ~= 0 or urc ~= 0 then
        say(string.format("PUT %s: close %s, commit %s", dname, hex(crc), hex(urc)))
        return urc ~= 0 and urc or crc, written, true
    end
    -- verify the committed size
    rc, mp = do_mount(1, 0)
    local vsize = -1
    if rc == 0 then
        write_string(path, mp .. "/" .. fname)
        if kstat(path, st) == 0 then vsize = read64(st + 0x48) end
        do_umount(mp)
    end
    say(string.format("PUT %s/%s: %d MiB in %.1f s (%.1f MB/s), verify %d", dname, fname, written >> 20,
        millis / 1000, written / 1000 / math.max(millis, 1), vsize))
    return vsize == size and 0 or -2, written, true, millis
end

local function send_all(fd, p, n)
    local sent = 0
    while sent < n do
        local w = syscall.write(fd, p + sent, n - sent)
        if w <= 0 then return false end
        sent = sent + w
    end
    return true
end

-- READ: reply, then the bytes (none on error)
local function read_file(cfd, dname, fname, offset, want)
    set_dir(dname)
    local rc, mp = do_mount(1, 0)
    if rc ~= 0 then send_reply(cfd, rc, 0, 0) return end
    write_string(path, mp .. "/" .. fname)
    local fd = kopen(path, 0, 0)
    local n = -1
    if fd >= 0 and fd < 0x10000 then
        n = kpread(fd, buf, math.min(want, CHUNK), offset)
        kclose(fd)
    end
    do_umount(mp)
    if n < 0 or n > CHUNK then send_reply(cfd, n < 0 and n or -1, 0, 0) return end
    send_reply(cfd, 0, n, 0)
    send_all(cfd, buf, n)
end

-- WRITE: the payload is always consumed, so the stream stays in step
local function write_file(cfd, dname, fname, size)
    if size > CHUNK then return -4, 0, false end
    if recv_exact(cfd, buf, size) ~= size then return -1, 0, false end
    set_dir(dname)
    local rc, mp = do_mount(2, 0)
    if rc ~= 0 then return rc, 0, true end
    write_string(path, mp .. "/" .. fname)
    local fd = kopen(path, 0x601, 0x1b6)
    local w = -1
    if fd >= 0 and fd < 0x10000 then
        w = kwrite(fd, buf, size)
        kclose(fd)
    end
    local urc = do_umount(mp)
    if w ~= size then return w < 0 and w or -2, 0, true end
    return urc, w, true
end

local function query(dname, fname)
    set_dir(dname)
    local rc, mp = do_mount(1, 0)
    if rc ~= 0 then return rc, -1 end
    write_string(path, mp .. "/" .. fname)
    local size = kstat(path, st) == 0 and read64(st + 0x48) or -1
    do_umount(mp)
    return 0, size
end

-- a broken receiver must not hold Luac0re's loader thread for good: after
-- three connections in a row that send no valid request, exit.  (fcntl is
-- not exported to SWRR, so accept() cannot be given a timeout.)
local GFS_MAGIC = 0x49534647   -- "GFSI"
local running, bad = true, 0
while running and bad < 3 do
    local cfd = syscall.accept(lfd, 0, 0)
    if cfd < 0 or cfd >= 0x10000 then
        say("accept -> " .. cfd)
        break
    end
    say("connection " .. cfd)
    while true do
        local got = recv_exact(cfd, hdr, 96)
        if got ~= 96 or read32(hdr) ~= GFS_MAGIC then
            say(string.format("connection closed (header %d bytes, starts %s)", got,
                got > 0 and string.format("%016x", read64(hdr)) or "-"))
            if got ~= 0 then bad = bad + 1 end
            break
        end
        bad = 0
        local op, blocks, size = read32(hdr + 4), read64(hdr + 8), read64(hdr + 16)
        local dname = cstring(hdr + 24, 32)
        local fname = cstring(hdr + 56, 32)
        local protected = dname == "SLUS-20268" or dname == "SLES-50366" or dname:sub(-4) == "SAVE"
        if protected and (op == 1 or op == 4 or op == 6) then
            if op == 6 then recv_exact(cfd, buf, math.min(size, CHUNK)) end
            send_reply(cfd, -3, 0, 0)       -- never SWRR's own save or a title's progress
            if op == 1 then break end       -- its payload was not read: drop the connection
        elseif op == 5 then
            read_file(cfd, dname, fname, blocks, size)
        elseif op == 6 then
            local status, written, synced = write_file(cfd, dname, fname, size)
            send_reply(cfd, status, written, 0)
            if not synced then break end
        elseif op == 1 then
            local status, written, synced, millis = put(cfd, blocks, size, dname, fname)
            send_reply(cfd, status, written, millis or 0)
            if not synced then break end    -- the stream is out of step: drop the connection
        elseif op == 2 then
            local status, fsize = query(dname, fname)
            send_reply(cfd, status, fsize & 0xFFFFFFFFFFFFFFFF, 0)
        elseif op == 4 then
            set_dir(dname)
            send_reply(cfd, do_delete(), 0, 0)
        else
            send_reply(cfd, 0, 0, 0)
            running = false
            break
        end
    end
    syscall.close(cfd)
end
syscall.close(lfd)
say("done")
