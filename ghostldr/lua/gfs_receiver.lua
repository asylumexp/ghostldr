-- ghostfs installer, console side: receives save containers over TCP and
-- writes them into SWRR's own save data (no kernel exploit needed).
-- Started through the remote Lua loader (Luac0re's in SWRR, UltraC0re's in Jak X) by ghostldr/host/gfs_install.py,
-- which prepends GFS_PORT, PROBE_LOG_IP and PROBE_LOG_PORT, and GFS_PUMP_HEX
-- (ghostldr/data/gfs_pump.bin, the native pump PUT uses; optional).
--
-- Request (96 bytes, little endian): "GFSI", op u32, blocks u64, size u64,
-- dirName[32], fileName[32], 8 reserved.  Ops: 1 PUT (delete + create the
-- container with `blocks`, write `size` bytes that follow into fileName,
-- commit, verify), 2 QUERY (size of fileName, -1 if missing), 3 BYE,
-- 4 DELETE, 5 READ (up to `size` bytes of fileName from offset `blocks`; the
-- reply is followed by `value` bytes), 6 WRITE (the `size` bytes that follow
-- become fileName in an existing container, committed), 7 LIST (the title
-- catalogue GFSCATALOG/catalog.bin follows, or one built from the legacy
-- GFSINDEX/GHOSTIDX index), 8 FREE (do `size` bytes fit on the console's
-- storage?  1 GiB probe containers GFSFREEnn are created and deleted; value =
-- the bytes that fit), 9 DELSAVE (delete a title's progress container).
-- Reply (32 bytes): "GFSR", status s32, value u64, millis u64, 8 pad.
-- Containers named *SAVE hold a title's progress: PUT, DELETE and WRITE
-- refuse them, as they refuse the host's own save (SWRR's, or Jak X's with
-- UltraC0re in it); only DELSAVE (sent on the user's explicit request) deletes
-- them, never the host's own.
-- Names and formats: docs/ghostfs-catalogue.md.
--
-- Save containers: sceSaveDataMount from SWRR's import table (eboot+0x3893F0),
-- params userId@0, dirName*@0x10, blocks@0x20, mode@0x28 (RO 1, RW 2,
-- CREATE 4), at most 32768 blocks (1 GiB).  Under UltraC0re, Jak X's
-- sceSaveDataMount2 (eboot+0xcd1e40): dirName*@0x8, blocks@0x10, mode@0x18.
-- File calls must be libkernel's own exports, or the commit (unmount) stays
-- BUSY for good.

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
local JAKX = J_COMP ~= nil
local mount = func_wrap(read64(EBOOT_BASE + (JAKX and 0xcd1e40 or 0x3893F0)))
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
    if JAKX then
        write64(params + 0x08, dir)
        write64(params + 0x10, blocks)
        write32(params + 0x18, mode)
    else
        write64(params + 0x10, dir)
        write64(params + 0x20, blocks)
        write32(params + 0x28, mode)
    end
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
-- The pump (ghostldr/native/pump/pump.c): native code that receives a PUT's
-- bytes on this thread while a second thread writes them, so a container
-- goes in at the slower of the network and the save writes (about 25 MB/s)
-- instead of one after the other (about 14).  It runs from the IPU JIT
-- buffer, where ghostldr.lua puts the core at boot.  Luac0re only: under
-- UltraC0re, and whenever it can't be set up, PUT keeps the Lua loop.
local PUMP_BUFS = 4
local pump, pump_ctx
if GFS_PUMP_HEX and not JAKX and IPU_PROG_BASE and SHELLCODE_SCRATCH and jit_memcpy and EBOOT_OFFSETS then
    local blob = hex_to_binary(GFS_PUMP_HEX)
    local code = IPU_PROG_BASE + 0x4000
    write_buffer(SHELLCODE_SCRATCH, blob)
    jit_memcpy(code, SHELLCODE_SCRATCH, #blob)
    local bufs = {buf}
    for i = 2, PUMP_BUFS do bufs[i] = malloc(CHUNK) end
    local fns = {dlsym(LIBKERNEL_HANDLE, "recvfrom"), dlsym(LIBKERNEL_HANDLE, "sceKernelWrite"),
                 read64(EBOOT_OFFSETS.scePthreadCreate), read64(EBOOT_OFFSETS.scePthreadJoin),
                 dlsym(LIBKERNEL_HANDLE, "sceKernelUsleep")}
    local ready = read64(code) == string.unpack("<I8", blob, 1)
    for i = 1, PUMP_BUFS do ready = ready and bufs[i] ~= 0 end
    for i = 1, #fns do ready = ready and fns[i] ~= 0 end
    if ready then
        pump_ctx = malloc(0xb8)
        for i = 1, #fns do write64(pump_ctx + (i - 1) * 8, fns[i]) end
        write64(pump_ctx + 0x38, CHUNK)
        for i = 1, PUMP_BUFS do write64(pump_ctx + 0x40 + (i - 1) * 8, bufs[i]) end
        pump = func_wrap(code)
    end
    say(string.format("pump %s (%d bytes at %x)", ready and "ready" or "unavailable: Lua loop", #blob, code))
end

-- a PUT's bytes through the pump: bytes written, and whether all arrived and were written
local function pump_put(cfd, fd, size, dname)
    write32(pump_ctx + 0x28, cfd)
    write32(pump_ctx + 0x2c, fd)
    write64(pump_ctx + 0x30, size)
    local status = pump(pump_ctx)
    local got, written = read64(pump_ctx + 0x68), read64(pump_ctx + 0x70)
    if status == -1 then
        say(string.format("PUT %s: connection lost after %d MiB", dname, got >> 20))
    elseif status ~= 0 then
        say(string.format("PUT %s: write -> %s after %d MiB", dname, hex(status), written >> 20))
    end
    return written, status == 0
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
    if pump then written, ok = pump_put(cfd, fd, size, dname) end
    while ok and written < size do
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
            say(string.format("PUT %s: %d MiB, %.1f MB/s (Lua heap %d KiB)", dname, written >> 20,
                              written / 1e6 / sec, math.floor(collectgarbage("count"))))
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
    say(string.format("PUT %s/%s: %d MiB in %.1f s (%.1f MB/s%s), verify %d", dname, fname, written >> 20,
        millis / 1000, written / 1000 / math.max(millis, 1), pump and ", pump" or "", vsize))
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

-- up to n bytes of fname (from its start) into p: mount status, bytes read (-1: no file)
local function read_into(dname, fname, p, n)
    set_dir(dname)
    local rc, mp = do_mount(1, 0)
    if rc ~= 0 then return rc, -1 end
    write_string(path, mp .. "/" .. fname)
    local fd = kopen(path, 0, 0)
    local got = -1
    if fd >= 0 and fd < 0x10000 then
        got = kpread(fd, p, n, 0)
        kclose(fd)
    end
    do_umount(mp)
    return 0, got
end

local function copy(dst, src, n)
    for i = 0, n - 1 do write8(dst + i, read8(src + i)) end
end

-- LIST: the catalogue as stored; without one, built from the legacy index
-- (header flag 1 = built here, entry flag 2 = legacy index)
local CAT_MAGIC, INDEX_MAGIC = 0x43534647, 0x31534647     -- "GFSC", "GFS1"
local function list(cfd)
    local rc, n = read_into("GFSCATALOG", "catalog.bin", buf, CHUNK)
    if rc == 0 and n >= 64 and n <= CHUNK and read32(buf) == CAT_MAGIC then
        send_reply(cfd, 0, n, 0)
        send_all(cfd, buf, n)
        return
    end
    for i = 0, 255, 8 do write64(buf + i, 0) end
    local count, h = 0, buf + 0x1000
    for _, iname in ipairs({"GFSINDEX", "GHOSTIDX"}) do
        local irc, got = read_into(iname, "index.bin", h, 128)
        if irc == 0 and got >= 64 and read32(h) == INDEX_MAGIC then
            local e, version = buf + 64, read32(h + 4)
            if version >= 2 then
                copy(e, h + 64, 16)                              -- title
                write64(e + 0x90, read64(h + 80))                -- layout id
            end
            copy(e + 0x10, h + 40, 16)                           -- prefix
            write_string(e + 0x20, iname)
            write64(e + 0x98, read64(h + 24))                    -- total
            write32(e + 0xA0, read32(h + 8))                     -- containers
            write32(e + 0xA4, 2 | (version >= 2 and (read32(h + 60) & 1) or 0))
            count = 1
            break
        end
    end
    write32(buf, CAT_MAGIC)
    write32(buf + 4, 1)
    write32(buf + 8, 64)
    write32(buf + 12, 192)
    write32(buf + 16, count)
    write32(buf + 20, 1)
    send_reply(cfd, 0, 64 + 192 * count, 0)
    send_all(cfd, buf, 64 + 192 * count)
end

-- FREE: create 1 GiB probes until `want` bytes are covered or the storage is
-- full (NO_SPACE_FS 0x809f000a reports the blocks still missing at
-- result+0x10), then delete them all
local function free_probe(want)
    local have, made, status = 0, 0, 0
    while have < want and made < 100 do
        set_dir(string.format("GFSFREE%02d", made))
        do_delete()
        local rc, mp = do_mount(6, 32768)
        if rc ~= 0 then
            if (rc & 0xFFFFFFFF) == 0x809F000A then
                local missing = read64(result + 0x10)
                if missing < 32768 then have = have + (32768 - missing) * 32768 end
            else
                status = rc
            end
            break
        end
        do_umount(mp)
        made = made + 1
        have = have + (1 << 30)
    end
    for i = 0, made do
        set_dir(string.format("GFSFREE%02d", i))
        do_delete()
    end
    say(string.format("FREE: %d MiB wanted, %d MiB fit (%d probes, status %s)", want >> 20, have >> 20, made,
        hex(status)))
    return status, have
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
        local host_save = dname == "SLUS-20268" or dname == "SLES-50366" or dname == "SCUS-97429"
        local protected = host_save or dname:sub(-4) == "SAVE"
        if protected and (op == 1 or op == 4 or op == 6) then
            if op == 6 then recv_exact(cfd, buf, math.min(size, CHUNK)) end
            send_reply(cfd, -3, 0, 0)       -- never the host's own save or a title's progress
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
        elseif op == 7 then
            list(cfd)
        elseif op == 8 then
            local status, have = free_probe(size)
            send_reply(cfd, status, have, 0)
        elseif op == 9 then
            -- a title's progress, on the user's explicit request; never SWRR's own save
            if dname:sub(-4) == "SAVE" and not host_save then
                set_dir(dname)
                local rc = do_delete()
                say(string.format("DELSAVE %s -> %s", dname, hex(rc)))
                send_reply(cfd, rc, 0, 0)
            else
                send_reply(cfd, -3, 0, 0)
            end
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
