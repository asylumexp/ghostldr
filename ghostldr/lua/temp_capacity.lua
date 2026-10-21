-- How much can SWRR write to /temp0?  Writes /temp0/capacity_probe.bin in
-- 4 MiB chunks up to PROBE_CAP_GIB (default 8), stops at the first error,
-- reports how far it got, then deletes the file.  The appcontent "available
-- space" calls are not exported to SWRR on this firmware and a raw statfs
-- gets the app killed, so measure by writing.

init_dlsym()
local sendto = func_wrap(dlsym(LIBKERNEL_HANDLE, "sendto"))
local unlink = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUnlink"))

local log_fd = create_socket(AF_INET, SOCK_DGRAM, 0)
local addr = malloc(16)
for i = 0, 15 do write8(addr + i, 0) end
write8(addr + 1, AF_INET)
write16(addr + 2, ((PROBE_LOG_PORT << 8) | (PROBE_LOG_PORT >> 8)) & 0xFFFF)
local a, b, c, d = PROBE_LOG_IP:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
write32(addr + 4, (tonumber(d) << 24) | (tonumber(c) << 16) | (tonumber(b) << 8) | tonumber(a))
local line_buf = malloc(1024)
local function say(s)
    s = "[temp-capacity] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, addr, 16)
end

local CHUNK = 4 * 1024 * 1024
local cap = (PROBE_CAP_GIB or 8) * 1024 // 4   -- chunks
local path = malloc(64)
write_string(path, "/temp0/capacity_probe.bin")
local buf = malloc(CHUNK)
local fd = sceKernelOpen(path, 0x601, 0x1b6)   -- O_WRONLY | O_CREAT | O_TRUNC
say(string.format("open -> %x, writing up to %d GiB", fd & 0xFFFFFFFF, cap * 4 // 1024))
if fd < 0 or fd >= 0x10000 then
    say("done")
    return
end
local written = 0
local started = os.time()
for i = 1, cap do
    local n = sceKernelWrite(fd, buf, CHUNK)
    if n ~= CHUNK then
        say(string.format("write %d returned %x after %d MiB", i, n & 0xFFFFFFFF, written // (1024 * 1024)))
        break
    end
    written = written + n
    if i % 256 == 0 then
        say(string.format("%d MiB written (%d s)", written // (1024 * 1024), os.time() - started))
    end
end
sceKernelClose(fd)
say(string.format("total %d MiB in %d s; unlink -> %x", written // (1024 * 1024), os.time() - started,
    unlink(path) & 0xFFFFFFFF))
say("done")
