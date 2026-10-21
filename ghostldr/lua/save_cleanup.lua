-- Unstick a probe container: delete PROBE_FILE inside PROBE_MOUNT (a container
-- filled to 100% cannot commit: unmount -> 0x809f0003), unmount it, then
-- delete the container PROBE_DIR.  Never point this at SLUS-20268/SLES-50366.

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
    s = "[save-cleanup] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, addr, 16)
end
local function hex(v) return string.format("%x", v & 0xFFFFFFFF) end

local DIR, MP, FILE = PROBE_DIR or "GHOSTCAP", PROBE_MOUNT or "/savedata1", PROBE_FILE or "probe.bin"
assert(DIR ~= "SLUS-20268" and DIR ~= "SLES-50366", "refusing to touch SWRR's own save")

local load_start = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
local usleep = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUsleep"))
local unlink = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUnlink"))
local name = malloc(128)
write_string(name, "libSceSaveData.sprx")
local sd = load_start(name, 0, 0, 0, 0, 0)
write_string(name, "libSceUserService.sprx")
local us = load_start(name, 0, 0, 0, 0, 0)
local umount = func_wrap(dlsym(sd, "sceSaveDataUmount"))
local delete = func_wrap(dlsym(sd, "sceSaveDataDelete"))
local uid_buf = malloc(8)
func_wrap(dlsym(us, "sceUserServiceGetInitialUser"))(uid_buf)
local user = read32(uid_buf)

local path, st, mp_buf = malloc(256), malloc(0x100), malloc(16)
write_string(path, MP .. "/" .. FILE)
say(string.format("stat %s -> %s, size %d MiB", MP .. "/" .. FILE, hex(sceKernelStat(path, st)), read64(st + 0x48) >> 20))
say("unlink -> " .. hex(unlink(path)))
for i = 0, 15 do write8(mp_buf + i, 0) end
write_string(mp_buf, MP)
for attempt = 1, 5 do
    local rc = umount(mp_buf)
    local waited = 0
    while waited < 120 and sceKernelStat(mp_buf, st) == 0 do
        usleep(250000)
        waited = waited + 1
    end
    say(string.format("umount %s try %d -> %s, mounted after: %s (%.1f s)", MP, attempt, hex(rc),
        sceKernelStat(mp_buf, st) == 0 and "yes" or "no", waited / 4))
    if sceKernelStat(mp_buf, st) ~= 0 then break end
end
local dir, del = malloc(32), malloc(64)
for i = 0, 31 do write8(dir + i, 0) end
write_string(dir, DIR)
for i = 0, 56, 8 do write64(del + i, 0) end
write32(del + 0x00, user)
write64(del + 0x10, dir)
say("delete " .. DIR .. " -> " .. hex(delete(del)))
say("done")
