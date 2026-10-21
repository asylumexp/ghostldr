-- Fill a 1 GiB save container in committed steps: PROBE_PARTS times, mount
-- PROBE_DIR read/write, write a PROBE_PART_MIB file, unmount (= commit) and
-- wait for it.  Then mount read-only, check every part and time a full read,
-- and delete the container (unless PROBE_KEEP).  One 1 GiB write followed by
-- a single unmount wedged the save service (0x809f0003 BUSY, container BROKEN).

init_dlsym()
local sendto = func_wrap(dlsym(LIBKERNEL_HANDLE, "sendto"))
local log_fd = create_socket(AF_INET, SOCK_DGRAM, 0)
local addr = malloc(16)
for i = 0, 15 do write8(addr + i, 0) end
write8(addr + 1, AF_INET)
write16(addr + 2, ((PROBE_LOG_PORT << 8) | (PROBE_LOG_PORT >> 8)) & 0xFFFF)
local a, b, c, d = PROBE_LOG_IP:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
write32(addr + 4, (tonumber(d) << 24) | (tonumber(c) << 16) | (tonumber(b) << 8) | tonumber(a))
local line_buf = malloc(1024)
local function say(s)
    s = "[save-steps] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, addr, 16)
end
local function hex(v) return string.format("%x", v & 0xFFFFFFFF) end

local DIR = PROBE_DIR or "GHOSTSTEP"
assert(DIR ~= "SLUS-20268" and DIR ~= "SLES-50366", "refusing to touch SWRR's own save")
local PARTS, PART_MIB = PROBE_PARTS or 7, PROBE_PART_MIB or 128

local load_start = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
local usleep = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUsleep"))
local read = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelRead"))
local kopen = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelOpen"))
local kwrite = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelWrite"))
local kclose = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelClose"))
local gettime = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelGetProcessTime"))   -- microseconds
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

local dir, params, result, st, path, mp_buf = malloc(32), malloc(128), malloc(64), malloc(0x100), malloc(256), malloc(16)
for i = 0, 31 do write8(dir + i, 0) end
write_string(dir, DIR)
local function do_mount(mode, blocks)
    for i = 0, 120, 8 do write64(params + i, 0) end
    for i = 0, 56, 8 do write64(result + i, 0) end
    write32(params + 0x00, user)
    write64(params + 0x10, dir)
    write64(params + 0x20, blocks)
    write32(params + 0x28, mode)
    local rc = mount(params, result)
    return rc, read_buffer(result, 16):match("^[^%z]*")
end
local function do_umount(mp)
    for i = 0, 15 do write8(mp_buf + i, 0) end
    write_string(mp_buf, mp)
    local t0 = gettime()
    local rc = umount(mp_buf)
    local polls = 0
    while polls < 480 and sceKernelStat(mp_buf, st) == 0 do
        usleep(250000)
        polls = polls + 1
    end
    return rc, (gettime() - t0) / 1e6, sceKernelStat(mp_buf, st) ~= 0
end

local CHUNK = 4 * 1024 * 1024
local buf = malloc(CHUNK)
local rc, mp = do_mount(6, 32768)
say(string.format("create %s (1 GiB) -> %s at '%s'", DIR, hex(rc), mp))
if rc ~= 0 then say("done") return end
local ok = true
for part = 1, PARTS do
    if part > 1 then
        rc, mp = do_mount(2, 0)
        if rc ~= 0 then
            say(string.format("part %d: mount read/write -> %s", part, hex(rc)))
            ok = false
            break
        end
    end
    for i = 0, CHUNK - 8, 8 * 4096 do write64(buf + i, part * 0x100000000 + i) end
    write_string(path, string.format("%s/part%02d.bin", mp, part))
    local fd = kopen(path, 0x601, 0x1b6)
    local t0, written = gettime(), 0
    for i = 1, PART_MIB // 4 do
        local n = kwrite(fd, buf, CHUNK)
        if n ~= CHUNK then
            say(string.format("part %d: write -> %s after %d MiB", part, hex(n), written >> 20))
            break
        end
        written = written + n
    end
    local crc = kclose(fd)
    if crc ~= 0 then say(string.format("part %d: close -> %s", part, hex(crc))) end
    local wsec = (gettime() - t0) / 1e6
    local urc, usec, gone = do_umount(mp)
    say(string.format("part %d: %d MiB in %.1f s (%.0f MB/s), commit -> %s in %.1f s%s", part, written >> 20, wsec,
        written / 1e6 / wsec, hex(urc), usec, gone and "" or " (still mounted!)"))
    if urc ~= 0 or not gone then ok = false break end
end

if ok then
    rc, mp = do_mount(1, 0)
    say(string.format("remount read-only -> %s at '%s'", hex(rc), mp))
    if rc == 0 then
        local total, t0 = 0, gettime()
        for part = 1, PARTS do
            write_string(path, string.format("%s/part%02d.bin", mp, part))
            local fd = kopen(path, 0, 0)
            local got = 0
            while fd >= 0 and fd < 0x10000 do
                local n = read(fd, buf, CHUNK)
                if n <= 0 or n > CHUNK then break end
                got = got + n
            end
            if fd >= 0 and fd < 0x10000 then kclose(fd) end
            local tag = read64(buf) >> 32
            if got ~= PART_MIB << 20 or tag ~= part then
                say(string.format("part %d read back %d MiB, tag %d", part, got >> 20, tag))
            end
            total = total + got
        end
        local sec = (gettime() - t0) / 1e6
        say(string.format("read back %d MiB in %.1f s (%.0f MB/s)", total >> 20, sec, total / 1e6 / sec))
        local urc, usec = do_umount(mp)
        say(string.format("read-only unmount -> %s in %.1f s", hex(urc), usec))
    end
end
if not PROBE_KEEP then
    local del = malloc(64)
    for i = 0, 56, 8 do write64(del + i, 0) end
    write32(del + 0x00, user)
    write64(del + 0x10, dir)
    say("delete " .. DIR .. " -> " .. hex(delete(del)))
end
say("done")
