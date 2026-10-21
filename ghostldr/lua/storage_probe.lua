-- How much space do SWRR's writable folders have?  Asked through the system
-- libraries a game is allowed to use (a raw statfs syscall gets SWRR killed:
-- CE-108255-1), reported over UDP like sandbox_probe.lua.  Run with
-- ghostldr/host/sandbox_probe.py.

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
    s = "[storage-probe] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, addr, 16)
end
say("start")

-- the module is already loaded in SWRR; loading it again returns its handle
local name = malloc(128)
local load_start = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
local handle = 0
for _, n in ipairs({"libSceAppContent.sprx", "libSceAppContentUtil.sprx",
                    "/system/common/lib/libSceAppContent.sprx", "/system/common/lib/libSceAppContentUtil.sprx"}) do
    write_string(name, n)
    local h = load_start(name, 0, 0, 0, 0, 0)
    say(string.format("load %s -> %x", n, h))
    if h > 0 and h < 0x80000000 then
        handle = h
        break
    end
end
if handle == 0 then
    say("done")
    return
end

local function api(fn)
    local addr = dlsym(handle, fn)
    say(string.format("%s at %x", fn, addr))
    return addr ~= 0 and func_wrap(addr) or nil
end

local mount_point = malloc(16)
for i = 0, 15 do write8(mount_point + i, 0) end
write_string(mount_point, "/temp0")
local kb = malloc(8)
write64(kb, 0)
local temp_space = api("sceAppContentTemporaryDataGetAvailableSpaceKb")
if temp_space then
    local rc = temp_space(mount_point, kb)
    say(string.format("/temp0 available: rc %x, %d KiB (%d MiB)", rc & 0xFFFFFFFF, read64(kb), read64(kb) // 1024))
end
local dl_space = api("sceAppContentDownloadDataGetAvailableSpaceKb")
if dl_space then
    write_string(mount_point, "/download0")
    write64(kb, 0)
    local rc = dl_space(mount_point, kb)
    say(string.format("/download0 available: rc %x, %d KiB", rc & 0xFFFFFFFF, read64(kb)))
end
say("done")
