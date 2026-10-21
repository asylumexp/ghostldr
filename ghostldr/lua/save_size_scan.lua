-- Which save container sizes will SWRR's save service create?  For each
-- block count in PROBE_BLOCK_LIST: create GHOSTSCAN (RW|CREATE), unmount,
-- delete.  Same mount call and layout as save_container_probe.lua.

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
    s = "[save-scan] " .. s
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

local dir = malloc(32)
for i = 0, 31 do write8(dir + i, 0) end
write_string(dir, PROBE_DIR or "GHOSTSCAN")
local params, result, mp_buf, st = malloc(128), malloc(64), malloc(16), malloc(0x100)
local del = malloc(64)

for _, blocks in ipairs(PROBE_BLOCK_LIST or {96, 32768, 65536, 131072}) do
    for i = 0, 120, 8 do write64(params + i, 0) end
    for i = 0, 56, 8 do write64(result + i, 0) end
    write32(params + 0x00, user)
    write64(params + 0x10, dir)
    write64(params + 0x20, blocks)
    write32(params + 0x28, PROBE_MODE or 6)
    local t0 = os.time()
    local rc = mount(params, result)
    local mp = read_buffer(result, 16):match("^[^%z]*")
    say(string.format("blocks %d (%d MiB): create -> %s in %d s, mountPoint '%s', requiredBlocks %d",
        blocks, blocks // 32, hex(rc), os.time() - t0, mp, read64(result + 0x10)))
    if rc == 0 then
        for i = 0, 15 do write8(mp_buf + i, 0) end
        write_string(mp_buf, mp)
        local urc = umount(mp_buf)
        local waited = 0
        while waited < 240 and sceKernelStat(mp_buf, st) == 0 do
            usleep(250000)
            waited = waited + 1
        end
        for i = 0, 56, 8 do write64(del + i, 0) end
        write32(del + 0x00, user)
        write64(del + 0x10, dir)
        say(string.format("  umount -> %s (%.1f s), delete -> %s", hex(urc), waited / 4, hex(delete(del))))
    end
end
say("done")
