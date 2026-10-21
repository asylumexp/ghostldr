-- Can SWRR (no kernel exploit) create a save container big enough to hold a
-- game?  Creates PROBE_DIR (default GHOSTPROBE) with PROBE_BLOCKS 32 KiB
-- blocks, writes PROBE_WRITE_MIB into it, unmounts (= commit), remounts it
-- read-only to check the file, then deletes the container.  SWRR's own save
-- (SLUS-20268 / SLES-50366) is never touched.
--
-- sceSaveDataMount comes from SWRR's import table (eboot+0x3893F0, the
-- patched v01.02 build): homebrew reports the dlsym'd pointer refuses every
-- mount.  Parameter block (SceSaveDataMount): userId u32 @0, titleId* @8
-- (NULL = this title), dirName* @0x10, fingerprint* @0x18, blocks u64 @0x20,
-- mountMode u32 @0x28.  Result: mountPoint char[16] @0, requiredBlocks u64
-- @0x10, mountStatus u32 @0x18.

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
    s = "[save-probe] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, addr, 16)
end
local function hex(v) return string.format("%x", v & 0xFFFFFFFF) end

local DIR = PROBE_DIR or "GHOSTPROBE"
local BLOCKS = PROBE_BLOCKS or 131072          -- 4 GiB
local WRITE_MIB = PROBE_WRITE_MIB or 1536
local KEEP = PROBE_KEEP or false

local load_start = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
local usleep = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUsleep"))
local unlink = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUnlink"))
local name = malloc(128)
local function module(n)
    write_string(name, n)
    local h = load_start(name, 0, 0, 0, 0, 0)
    say(string.format("load %s -> %x", n, h))
    return h
end
local sd = module("libSceSaveData.sprx")
local us = module("libSceUserService.sprx")
local umount = func_wrap(dlsym(sd, "sceSaveDataUmount"))
local delete = func_wrap(dlsym(sd, "sceSaveDataDelete"))
local get_user = func_wrap(dlsym(us, "sceUserServiceGetInitialUser"))

local mount_ptr = read64(EBOOT_BASE + 0x3893F0)
say(string.format("eboot %x, sceSaveDataMount from imports %x (dlsym %x)", EBOOT_BASE, mount_ptr,
    dlsym(sd, "sceSaveDataMount")))
if mount_ptr < 0x10000 then
    say("import slot looks wrong for this SWRR build; stopping")
    say("done")
    return
end
local mount = func_wrap(mount_ptr)

local uid_buf = malloc(8)
write64(uid_buf, 0)
say("sceUserServiceGetInitialUser -> " .. hex(get_user(uid_buf)) .. string.format(", user %x", read32(uid_buf)))
local user = read32(uid_buf)

local dir = malloc(32)
for i = 0, 31 do write8(dir + i, 0) end
write_string(dir, DIR)
local params = malloc(128)
local result = malloc(64)
local function do_mount(mode, blocks)
    for i = 0, 120, 8 do write64(params + i, 0) end
    for i = 0, 56, 8 do write64(result + i, 0) end
    write32(params + 0x00, user)
    write64(params + 0x10, dir)
    write64(params + 0x20, blocks)
    write32(params + 0x28, mode)
    local rc = mount(params, result)
    local mp = read_buffer(result, 16):match("^[^%z]*")
    say(string.format("mount %s mode %d blocks %d -> %s, mountPoint '%s', requiredBlocks %d, status %d",
        DIR, mode, blocks, hex(rc), mp, read64(result + 0x10), read32(result + 0x18)))
    return rc, mp
end
local mp_buf = malloc(16)
local st = malloc(0x100)
local path = malloc(256)
local function do_umount(mp)
    for i = 0, 15 do write8(mp_buf + i, 0) end
    write_string(mp_buf, mp)
    local rc = umount(mp_buf)
    local waited = 0
    write_string(path, mp)
    while waited < 240 and sceKernelStat(path, st) == 0 do   -- unmount is the commit, and asynchronous
        usleep(250000)
        waited = waited + 1
    end
    say(string.format("umount %s -> %s, gone after %.1f s", mp, hex(rc), waited / 4))
end

-- 1. create, read/write (RW 2 | CREATE 4)
local rc, mp = do_mount(6, BLOCKS)
if rc ~= 0 then
    say("create failed; trying to open an existing one read/write")
    rc, mp = do_mount(2, 0)
end
if rc ~= 0 then
    say("done")
    return
end

-- 2. write
local CHUNK = 4 * 1024 * 1024
local buf = malloc(CHUNK)
for i = 0, CHUNK - 8, 8 * 4096 do write64(buf + i, i) end
write_string(path, mp .. "/probe.bin")
local fd = sceKernelOpen(path, 0x601, 0x1b6)
local written, started = 0, os.time()
if fd >= 0 and fd < 0x10000 then
    for i = 1, WRITE_MIB // 4 do
        local n = sceKernelWrite(fd, buf, CHUNK)
        if n ~= CHUNK then
            say(string.format("write %d -> %s after %d MiB", i, hex(n), written >> 20))
            break
        end
        written = written + n
        if i % 128 == 0 then say(string.format("%d MiB written (%d s)", written >> 20, os.time() - started)) end
    end
    sceKernelClose(fd)
else
    say("open " .. mp .. "/probe.bin -> " .. hex(fd))
end
say(string.format("wrote %d MiB in %d s", written >> 20, os.time() - started))

-- 3. commit, then check it survived
do_umount(mp)
rc, mp = do_mount(1, 0)
if rc == 0 then
    write_string(path, mp .. "/probe.bin")
    local src = sceKernelStat(path, st)
    say(string.format("after commit: stat %s, size %d MiB", hex(src), read64(st + 0x48) >> 20))
    local read = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelRead"))
    local rfd = sceKernelOpen(path, 0, 0)
    local got, t0 = 0, os.time()
    if rfd >= 0 and rfd < 0x10000 then
        while true do
            local n = read(rfd, buf, CHUNK)
            if n <= 0 or n > CHUNK then break end
            got = got + n
        end
        sceKernelClose(rfd)
    end
    say(string.format("read back %d MiB in %d s", got >> 20, os.time() - t0))
    do_umount(mp)
end

-- 4. remove the probe container
if not KEEP then
    local del = malloc(64)
    for i = 0, 56, 8 do write64(del + i, 0) end
    write32(del + 0x00, user)
    write64(del + 0x10, dir)
    say("sceSaveDataDelete -> " .. hex(delete(del)))
end
say("done")
