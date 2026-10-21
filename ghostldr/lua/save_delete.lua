-- Delete leftover probe containers (names in PROBE_DELETE).  SWRR's own save
-- (SLUS-20268 / SLES-50366) is refused.

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
    s = "[save-delete] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, addr, 16)
end
local load_start = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
local name = malloc(128)
write_string(name, "libSceSaveData.sprx")
local sd = load_start(name, 0, 0, 0, 0, 0)
write_string(name, "libSceUserService.sprx")
local us = load_start(name, 0, 0, 0, 0, 0)
local delete = func_wrap(dlsym(sd, "sceSaveDataDelete"))
local uid_buf = malloc(8)
func_wrap(dlsym(us, "sceUserServiceGetInitialUser"))(uid_buf)
local user = read32(uid_buf)
local dir, del = malloc(32), malloc(64)
for _, n in ipairs(PROBE_DELETE or {"GHOSTSTEP", "GHOSTMB", "GHOSTMC", "GHOSTMD", "GHOSTMF", "GHOSTMG"}) do
    if n ~= "SLUS-20268" and n ~= "SLES-50366" then
        for i = 0, 31 do write8(dir + i, 0) end
        write_string(dir, n)
        for i = 0, 56, 8 do write64(del + i, 0) end
        write32(del + 0x00, user)
        write64(del + 0x10, dir)
        say(string.format("delete %s -> %x", n, delete(del) & 0xFFFFFFFF))
    end
end
say("done")
