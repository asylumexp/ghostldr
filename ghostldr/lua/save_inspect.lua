-- Look at a probe container after a session ended badly: mount PROBE_DIR
-- read-only, list it and report its file sizes, unmount, then (unless
-- PROBE_KEEP) delete it.  Never point this at SLUS-20268/SLES-50366.

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
    s = "[save-inspect] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, addr, 16)
end
local function hex(v) return string.format("%x", v & 0xFFFFFFFF) end

local DIR = PROBE_DIR or "GHOSTCAP"
assert(DIR ~= "SLUS-20268" and DIR ~= "SLES-50366", "refusing to touch SWRR's own save")
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

local dir, params, result, st, path = malloc(32), malloc(128), malloc(64), malloc(0x100), malloc(256)
for i = 0, 31 do write8(dir + i, 0) end
write_string(dir, DIR)
for i = 0, 120, 8 do write64(params + i, 0) end
for i = 0, 56, 8 do write64(result + i, 0) end
write32(params + 0x00, user)
write64(params + 0x10, dir)
write32(params + 0x28, 1)   -- read-only
local rc = mount(params, result)
local mp = read_buffer(result, 16):match("^[^%z]*")
say(string.format("mount %s read-only -> %s at '%s'", DIR, hex(rc), mp))
if rc == 0 then
    write_string(path, mp .. "/probe.bin")
    say(string.format("probe.bin: stat %s, size %d MiB", hex(sceKernelStat(path, st)), read64(st + 0x48) >> 20))
    local mp_buf = malloc(16)
    for i = 0, 15 do write8(mp_buf + i, 0) end
    write_string(mp_buf, mp)
    local urc = umount(mp_buf)
    local waited = 0
    while waited < 120 and sceKernelStat(mp_buf, st) == 0 do
        usleep(250000)
        waited = waited + 1
    end
    say(string.format("umount -> %s (%.1f s)", hex(urc), waited / 4))
end
if not PROBE_KEEP then
    local del = malloc(64)
    for i = 0, 56, 8 do write64(del + i, 0) end
    write32(del + 0x00, user)
    write64(del + 0x10, dir)
    say("delete " .. DIR .. " -> " .. hex(delete(del)))
end
say("done")
