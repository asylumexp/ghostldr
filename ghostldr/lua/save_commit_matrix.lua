-- Why does unmounting a freshly written container return 0x809f0003 (BUSY)?
-- Each variant uses its own tiny container GHOSTMxx (3 MiB), writes 1 MiB,
-- then tries to commit a different way.  Containers that commit are deleted.
--   A: no write, unmount (control)
--   B: write, close, unmount
--   C: write, fsync, close, unmount
--   D: write, close, sceSaveDataSetParam (title), unmount
--   E: write, close, sceSaveDataUmount2(mode 0), and if that fails Umount2(1)

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
    s = "[commit-matrix] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, addr, 16)
end
local function hex(v) return string.format("%x", v & 0xFFFFFFFF) end

local load_start = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
local usleep = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUsleep"))
local fsync = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelFsync"))
local name = malloc(128)
write_string(name, "libSceSaveData.sprx")
local sd = load_start(name, 0, 0, 0, 0, 0)
write_string(name, "libSceUserService.sprx")
local us = load_start(name, 0, 0, 0, 0, 0)
local umount = func_wrap(dlsym(sd, "sceSaveDataUmount"))
local umount2_addr = dlsym(sd, "sceSaveDataUmount2")
local setparam_addr = dlsym(sd, "sceSaveDataSetParam")
say(string.format("sceSaveDataUmount2 %x, sceSaveDataSetParam %x", umount2_addr, setparam_addr))
local delete = func_wrap(dlsym(sd, "sceSaveDataDelete"))
local mount = func_wrap(read64(EBOOT_BASE + 0x3893F0))
local uid_buf = malloc(8)
func_wrap(dlsym(us, "sceUserServiceGetInitialUser"))(uid_buf)
local user = read32(uid_buf)

local dir, params, result, st, path, mp_buf, del = malloc(32), malloc(128), malloc(64), malloc(0x100), malloc(256), malloc(16), malloc(64)
local data = malloc(1024 * 1024)
local param = malloc(0x600)   -- SceSaveDataParam: title[128] subTitle[128] detail[1024] userParam u32 ...

local function try(tag, write, before_umount, umount_fn)
    local dname = "GHOSTM" .. tag
    for i = 0, 31 do write8(dir + i, 0) end
    write_string(dir, dname)
    for i = 0, 120, 8 do write64(params + i, 0) end
    for i = 0, 56, 8 do write64(result + i, 0) end
    write32(params + 0x00, user)
    write64(params + 0x10, dir)
    write64(params + 0x20, 96)
    write32(params + 0x28, 6)
    local rc = mount(params, result)
    local mp = read_buffer(result, 16):match("^[^%z]*")
    if rc ~= 0 then
        say(string.format("%s: create -> %s", tag, hex(rc)))
        return
    end
    if write then
        write_string(path, mp .. "/data.bin")
        local fd = sceKernelOpen(path, 0x601, 0x1b6)
        local n = sceKernelWrite(fd, data, 1024 * 1024)
        local extra = before_umount and before_umount(fd, mp) or ""
        sceKernelClose(fd)
        say(string.format("%s: wrote %s %s", tag, hex(n), extra))
    end
    for i = 0, 15 do write8(mp_buf + i, 0) end
    write_string(mp_buf, mp)
    local urc = (umount_fn or umount)(mp_buf)
    local polls = 0
    while polls < 40 and sceKernelStat(mp_buf, st) == 0 do
        usleep(250000)
        polls = polls + 1
    end
    local gone = sceKernelStat(mp_buf, st) ~= 0
    say(string.format("%s: unmount -> %s, %s after %.1f s", tag, hex(urc), gone and "unmounted" or "STILL MOUNTED", polls / 4))
    if gone then
        for i = 0, 56, 8 do write64(del + i, 0) end
        write32(del + 0x00, user)
        write64(del + 0x10, dir)
        say(string.format("%s: delete -> %s", tag, hex(delete(del))))
    end
end

local function try_existing(tag)
    local dname = "GHOSTM" .. tag
    for i = 0, 31 do write8(dir + i, 0) end
    write_string(dir, dname)
    local function m(mode, blocks)
        for i = 0, 120, 8 do write64(params + i, 0) end
        for i = 0, 56, 8 do write64(result + i, 0) end
        write32(params + 0x00, user)
        write64(params + 0x10, dir)
        write64(params + 0x20, blocks)
        write32(params + 0x28, mode)
        return mount(params, result), read_buffer(result, 16):match("^[^%z]*")
    end
    local function u(mp)
        for i = 0, 15 do write8(mp_buf + i, 0) end
        write_string(mp_buf, mp)
        local urc = umount(mp_buf)
        local polls = 0
        while polls < 40 and sceKernelStat(mp_buf, st) == 0 do
            usleep(250000)
            polls = polls + 1
        end
        return urc, sceKernelStat(mp_buf, st) ~= 0
    end
    local rc, mp = m(6, 96)
    local urc, gone = u(mp)
    say(string.format("%s: create -> %s, empty unmount -> %s (%s)", tag, hex(rc), hex(urc), gone and "gone" or "stuck"))
    rc, mp = m(2, 0)
    say(string.format("%s: mount existing read/write -> %s at '%s'", tag, hex(rc), mp))
    if rc ~= 0 then return end
    write_string(path, mp .. "/data.bin")
    local fd = sceKernelOpen(path, 0x601, 0x1b6)
    local n = sceKernelWrite(fd, data, 1024 * 1024)
    sceKernelClose(fd)
    urc, gone = u(mp)
    say(string.format("%s: wrote %s, unmount -> %s (%s)", tag, hex(n), hex(urc), gone and "gone" or "STILL MOUNTED"))
    if gone then
        rc, mp = m(1, 0)
        write_string(path, mp .. "/data.bin")
        say(string.format("%s: read-only remount -> %s, data.bin stat %s size %d", tag, hex(rc),
            hex(sceKernelStat(path, st)), read64(st + 0x48)))
        u(mp)
        for i = 0, 56, 8 do write64(del + i, 0) end
        write32(del + 0x00, user)
        write64(del + 0x10, dir)
        say(string.format("%s: delete -> %s", tag, hex(delete(del))))
    end
end

-- G: like F but every file call's result is logged, the file is opened
-- O_RDWR|O_CREAT through libkernel's own exports instead of SWRR's imports
local function try_logged(tag)
    local kopen = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelOpen"))
    local kwrite = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelWrite"))
    local kclose = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelClose"))
    local dname = "GHOSTM" .. tag
    for i = 0, 31 do write8(dir + i, 0) end
    write_string(dir, dname)
    for i = 0, 120, 8 do write64(params + i, 0) end
    for i = 0, 56, 8 do write64(result + i, 0) end
    write32(params + 0x00, user)
    write64(params + 0x10, dir)
    write64(params + 0x20, 96)
    write32(params + 0x28, 6)
    local rc = mount(params, result)
    local mp = read_buffer(result, 16):match("^[^%z]*")
    say(string.format("%s: create -> %s at '%s'", tag, hex(rc), mp))
    if rc ~= 0 then return end
    write_string(path, mp .. "/data.bin")
    local fd = kopen(path, 0x202, 0x1b6)
    local n = kwrite(fd, data, 65536)
    local c = kclose(fd)
    local c2 = kclose(fd)
    say(string.format("%s: open %s, write %s, close %s, second close %s", tag, hex(fd), hex(n), hex(c), hex(c2)))
    for i = 0, 15 do write8(mp_buf + i, 0) end
    write_string(mp_buf, mp)
    for attempt = 1, 3 do
        local urc = umount(mp_buf)
        say(string.format("%s: unmount try %d -> %s", tag, attempt, hex(urc)))
        if urc == 0 then break end
        usleep(3000000)
    end
end

if PROBE_ONLY_G then
    try_logged("G")
    say("done")
    return
end
if PROBE_ONLY_F then
    try_existing("F")
    say("done")
    return
end
try("A", false)
try("B", true)
try("C", true, function(fd) return "fsync " .. hex(fsync(fd)) end)
if setparam_addr ~= 0 then
    local setparam = func_wrap(setparam_addr)
    try("D", true, function(fd, mp)
        for i = 0, 0x5f8, 8 do write64(param + i, 0) end
        write_string(param, "Ghost data")
        for i = 0, 15 do write8(mp_buf + i, 0) end
        write_string(mp_buf, mp)
        return "setparam " .. hex(setparam(mp_buf, 0, param, 0x5b0))   -- SCE_SAVE_DATA_PARAM_TYPE_ALL
    end)
end
if umount2_addr ~= 0 then
    local umount2 = func_wrap(umount2_addr)
    try("E", true, nil, function(mp)
        local r = umount2(0, mp)
        if r ~= 0 then r = umount2(1, mp) end
        return r
    end)
end
say("done")
