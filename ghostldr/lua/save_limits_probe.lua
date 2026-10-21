-- How many save containers can SWRR mount at once, and how many can it own?
-- 1. Mount: create tiny GHOSTCxx containers and keep each mounted until the
--    service refuses (or PROBE_MAX_MOUNT); report the error; unmount + delete.
-- 2. Own: create PROBE_OWN containers GHOSTOxx of 1 GiB (unmounted right
--    away, nothing written), report failures, then delete them all.

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
    s = "[save-limits] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, addr, 16)
end
local function hex(v) return string.format("%x", v & 0xFFFFFFFF) end

local load_start = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
local usleep = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUsleep"))
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

local params, result, st, del = malloc(128), malloc(64), malloc(0x100), malloc(64)
local dirs, mps = {}, {}
local function dirbuf(n)
    local p = malloc(32)
    for i = 0, 31 do write8(p + i, 0) end
    write_string(p, n)
    return p
end
local function do_mount(dir, mode, blocks)
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
    local p = malloc(16)
    for i = 0, 15 do write8(p + i, 0) end
    write_string(p, mp)
    local rc = umount(p)
    local polls = 0
    while polls < 40 and sceKernelStat(p, st) == 0 do
        usleep(250000)
        polls = polls + 1
    end
    return rc
end
local function do_delete(dir)
    for i = 0, 56, 8 do write64(del + i, 0) end
    write32(del + 0x00, user)
    write64(del + 0x10, dir)
    return delete(del)
end

-- 1. simultaneous mounts
local mounted = {}
for i = 1, PROBE_MAX_MOUNT or 40 do
    local dir = dirbuf(string.format("GHOSTC%02d", i))
    local rc, mp = do_mount(dir, 6, 96)
    if rc ~= 0 then
        say(string.format("mount #%d refused: %s (holding %d mounts of ours)", i, hex(rc), #mounted))
        do_delete(dir)
        break
    end
    mounted[#mounted + 1] = {dir = dir, mp = mp}
end
local last = mounted[#mounted]
say(string.format("held %d mounts at once, last at %s", #mounted, last and last.mp or "-"))
local bad = 0
for _, m in ipairs(mounted) do
    if do_umount(m.mp) ~= 0 or do_delete(m.dir) ~= 0 then bad = bad + 1 end
end
say(string.format("unmounted and deleted them (%d failures)", bad))

-- 2. how many 1 GiB containers can exist
local made, t0 = {}, os.time()
for i = 1, PROBE_OWN or 64 do
    local dir = dirbuf(string.format("GHOSTO%02d", i))
    local rc, mp = do_mount(dir, 6, 32768)
    if rc ~= 0 then
        say(string.format("container #%d refused: %s", i, hex(rc)))
        break
    end
    local urc = do_umount(mp)
    if urc ~= 0 then
        say(string.format("container #%d unmount -> %s", i, hex(urc)))
        break
    end
    made[#made + 1] = dir
    if i % 16 == 0 then say(string.format("%d x 1 GiB containers exist (%d s)", i, os.time() - t0)) end
end
say(string.format("created %d x 1 GiB containers in %d s", #made, os.time() - t0))
bad = 0
for _, dir in ipairs(made) do
    if do_delete(dir) ~= 0 then bad = bad + 1 end
end
say(string.format("deleted them (%d failures)", bad))
say("done")
