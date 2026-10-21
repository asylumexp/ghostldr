-- What can SWRR (US CUSA03474 / EU CUSA03492) reach from inside its own sandbox?
-- Run through Luac0re's remote Lua loader on a console with NO payloads sent
-- (no unjail, no kstuff, no ShadowMountPlus), so the sandbox is stock.
-- The host tool prepends PROBE_LOG_IP / PROBE_LOG_PORT; every line goes there
-- over UDP, and a short summary also appears as a notification.

init_dlsym()
local O_DIRECTORY = 0x20000
local kernel = {}
for _, name in ipairs({"sceKernelRead", "sceKernelGetdents", "sendto"}) do
    local addr = dlsym(LIBKERNEL_HANDLE, name)
    kernel[name] = addr ~= 0 and func_wrap(addr) or nil
end

local log_fd = create_socket(AF_INET, SOCK_DGRAM, 0)
local addr = malloc(16)
for i = 0, 15 do write8(addr + i, 0) end
write8(addr + 1, AF_INET)
write16(addr + 2, ((PROBE_LOG_PORT << 8) | (PROBE_LOG_PORT >> 8)) & 0xFFFF)
local a, b, c, d = PROBE_LOG_IP:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
write32(addr + 4, (tonumber(d) << 24) | (tonumber(c) << 16) | (tonumber(b) << 8) | tonumber(a))
local line_buf = malloc(1024)
local function say(s)
    s = "[sandbox-probe] " .. s
    write_string(line_buf, s)
    if log_fd >= 0 and kernel.sendto then
        kernel.sendto(log_fd, line_buf, #s, 0, addr, 16)
    end
end

local path = malloc(512)
local st = malloc(0x100)
local dents = malloc(0x2000)
local data = malloc(64)

local function names(fd)
    local out = {}
    if not kernel.sceKernelGetdents then return out end
    local n = kernel.sceKernelGetdents(fd, dents, 0x2000)
    if n <= 0 or n > 0x2000 then return out end
    local off = 0
    while off < n and #out < 8 do
        local reclen = read32(dents + off + 4) & 0xFFFF
        local namlen = (read32(dents + off + 4) >> 24) & 0xFF
        if reclen == 0 then break end
        local name = read_buffer(dents + off + 8, namlen)
        if name ~= "." and name ~= ".." then out[#out + 1] = name end
        off = off + reclen
    end
    return out
end

local summary = {}
local function check(p)
    write_string(path, p)
    local rc = sceKernelStat(path, st)
    local line = string.format("%-14s stat %s", p, rc == 0 and "ok" or string.format("%x", rc & 0xFFFFFFFF))
    if rc == 0 then
        local fd = sceKernelOpen(path, O_RDONLY | O_DIRECTORY, 0)
        if fd >= 0 and fd < 0x10000 then
            local list = names(fd)
            sceKernelClose(fd)
            line = line .. string.format(", list %d: %s", #list, table.concat(list, " "))
        else
            line = line .. string.format(", open %x", fd & 0xFFFFFFFF)
        end
    end
    say(line)
    summary[#summary + 1] = p .. (rc == 0 and " yes" or " no")
end

say(string.format("uid %d, in sandbox %d", syscall.getuid(), syscall.is_in_sandbox()))
for _, p in ipairs({"/mnt/usb0", "/mnt/usb1", "/data", "/data/ghost", "/mnt/disc", "/mnt/ext0",
                    "/app0", "/savedata0", "/temp0", "/download0", "/user", "/system_data"}) do
    check(p)
end

-- reading a file on the stick, not just listing it: put canreadusb.txt (any
-- content, 4+ bytes) at the root of the USB drive
write_string(path, "/mnt/usb0/canreadusb.txt")
local fd = sceKernelOpen(path, O_RDONLY, 0)
if fd >= 0 and fd < 0x10000 and kernel.sceKernelRead then
    local n = kernel.sceKernelRead(fd, data, 4)
    say(string.format("read /mnt/usb0/canreadusb.txt: %d bytes, first bytes %08x", n, read32(data)))
    summary[#summary + 1] = "usb read " .. (n == 4 and "yes" or "no")
    sceKernelClose(fd)
else
    say(string.format("open /mnt/usb0/canreadusb.txt -> %x", fd & 0xFFFFFFFF))
    summary[#summary + 1] = "usb read no"
end
say("done")
send_notification("sandbox probe: " .. table.concat(summary, ", "))
