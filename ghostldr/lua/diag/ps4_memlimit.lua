-- ghostldr: SWRR's direct memory limit on a PS4 (docs/ps4.md, the sixth report).
--
-- One file for Luac0re's Remote Lua Loader, no ghostldr needed: start SWRR,
-- go to OPTIONS -> HALL OF FAME until the loader listens on TCP 9026, then
--
--     python payload_sender.py <PS4 address> ps4_memlimit.lua
--
-- (payload_sender.py comes with Luac0re; any tool that sends the file to
-- port 9026 works).  When it's done, a notification shows the result and the
-- full report is at http://<PS4 address>:9031/ for five minutes.
--
-- What it does, all in SWRR's own process:
--   1. how much direct memory the kernel still hands out (taken in pieces,
--      then released);
--   2. the sysctl that sceKernelSetGameDirectMemoryLimit writes: its name,
--      found in that function's code, and its value;
--   3. one try to raise that sysctl to 4608 MiB; if the kernel takes it, what
--      it hands out then, and the old value written back at once;
--   4. sceKernelGetResourceLimit(0..3), whose arguments are unknown: every
--      buffer it may write ends at an unmapped page, and SIGSYS is ignored
--      meanwhile, so a wrong guess or a refusal fails cleanly.
-- Nothing is booted and SWRR keeps running.

local MiB = 0x100000
local REPORT_PORT = 9031
local lines = {}

local function log(fmt, ...)
    local s = string.format(fmt, ...)
    lines[#lines + 1] = s
    print(s)
end

local function errno() return read32(libc_error()) end
local function ok(rc) return (rc & 0xFFFFFFFF) == 0 end
local function hex32(rc) return string.format("%08x", rc & 0xFFFFFFFF) end

local function sym(name)
    local good, addr = pcall(dlsym, LIBKERNEL_HANDLE, name)
    return good and addr or 0
end

local function hexdump(tag, addr, n)
    local b = read_buffer(addr, n)
    for i = 1, n, 32 do
        local t = {}
        for j = i, math.min(i + 31, n) do t[#t + 1] = string.format("%02x", b:byte(j)) end
        log("%s+%03x: %s", tag, i - 1, table.concat(t, " "))
    end
end

-- 1. what the kernel still hands out: the largest piece it grants (halving
-- down to 2 MiB) again and again, then all of it released
local alloc = func_wrap(sym("sceKernelAllocateDirectMemory"))
local release = func_wrap(sym("sceKernelReleaseDirectMemory"))
local avail = func_wrap(sym("sceKernelAvailableDirectMemorySize"))
local dmem_size = func_wrap(sym("sceKernelGetDirectMemorySize"))()

local function allocatable(mtype)
    local got, sum = {}, 0
    local s, size, phys = malloc(8), malloc(8), malloc(8)
    for _ = 1, 96 do
        if not ok(avail(0, dmem_size, 0x200000, s, size)) then break end
        local len = read64(size) & ~0x1FFFFF
        while len >= 0x200000 and not ok(alloc(0, dmem_size, len, 0x200000, mtype, phys)) do
            len = (len // 2) & ~0x1FFFFF
        end
        if len < 0x200000 then break end
        got[#got + 1] = {read64(phys), len}
        sum = sum + len
    end
    for _, g in ipairs(got) do release(g[1], g[2]) end
    return sum
end

-- the sysctl name: sceKernelSetGameDirectMemoryLimit loads it with
-- "lea rdi, [rip + disp32]" (48 8d 3d) before calling sysctlbyname
local function limit_sysctl_name()
    local fn = sym("sceKernelSetGameDirectMemoryLimit")
    if fn == 0 then return nil, "sceKernelSetGameDirectMemoryLimit is not exported here" end
    local code = read_buffer(fn, 0x80)
    hexdump("code", fn, 0x80)
    local at = code:find("\x48\x8d\x3d", 1, true)
    if not at then return nil, "no lea rdi in its first 128 bytes" end
    local disp = string.unpack("<i4", code, at + 3)
    local addr = fn + (at - 1) + 7 + disp
    local name = read_null_terminated_string(addr)
    if #name == 0 or #name > 96 or name:find("[^%w%._%-]") then
        return nil, string.format("the string at %x does not look like a sysctl name", addr)
    end
    -- its neighbours may be the other memory sysctls
    local near = read_buffer(addr - 0x100, 0x300)
    for str in near:gmatch("[%w%._%-]+") do
        if #str >= 6 and str:find("%.") then log("  string near it: %s", str) end
    end
    return name
end

local function sysctl_read(name)
    local oldp, oldlen = malloc(8), malloc(8)
    write64(oldlen, 8)
    local good, res = pcall(sysctlbyname, name, oldp, oldlen, 0, 0)
    if not good then return nil, "name not found (" .. tostring(res):gsub("\n", " ") .. ")" end
    if not res then return nil, "read refused, errno " .. errno() end
    return read64(oldp), read64(oldlen)
end

local function sysctl_write(name, value)
    local newp = malloc(8)
    write64(newp, value)
    local good, res = pcall(sysctlbyname, name, 0, 0, newp, 8)
    if not good then return false, "name not found" end
    if not res then return false, "refused, errno " .. errno() end
    return true
end

-- 4. guarded buffers: 64 KiB, then an unmapped page
local function guarded()
    local p = syscall.mmap(0, 0x20000, 3, 0x1002, -1, 0) -- PROT_READ|WRITE, MAP_ANON|MAP_PRIVATE
    if p == -1 or p == 0 then return nil end
    syscall.munmap(p + 0x10000, 0x10000)
    return p
end

local function resource_limits()
    local fn = sym("sceKernelGetResourceLimit")
    if fn == 0 then log("sceKernelGetResourceLimit: not exported here") return end
    local g = guarded()
    if not g then log("sceKernelGetResourceLimit: no guarded buffer") return end
    local sa_new, sa_old = malloc(32), malloc(32)
    write64(sa_new, 1) -- SIG_IGN
    if syscall.sigaction(12, sa_new, sa_old) ~= 0 then -- SIGSYS
        log("sceKernelGetResourceLimit: SIGSYS cannot be ignored here: not called")
        return
    end
    local f = func_wrap(fn)
    for which = 0, 3 do
        local out = g + 0x1000
        write_buffer(g, string.rep("\0", 0x2000))
        write32(g, 64) -- a count, in case the third argument is one
        local rc = f(which, out, g, 0, 0, 0)
        log("sceKernelGetResourceLimit(%d, out, &count) -> %s (errno %d)", which, hex32(rc), ok(rc) and 0 or errno())
        if ok(rc) then hexdump("  out", out, 64) end
    end
    syscall.sigaction(12, sa_old, 0)
    syscall.munmap(g, 0x10000)
end

-- the report: a notification, and the whole text over HTTP
local function serve_report(summary)
    local text = table.concat(lines, "\n") .. "\n"
    local ip = get_current_ip() or "<PS4 address>"
    send_notification(string.format("%s\nReport: http://%s:%d/", summary, ip, REPORT_PORT))
    local fd = create_socket(AF_INET, SOCK_STREAM, 0)
    if fd < 0 then return end
    local one, sa = malloc(4), malloc(16)
    write32(one, 1)
    syscall.setsockopt(fd, 0xffff, 4, one, 4) -- SO_REUSEADDR
    write8(sa + 1, AF_INET)
    write16(sa + 2, ((REPORT_PORT << 8) | (REPORT_PORT >> 8)) & 0xFFFF)
    if syscall.bind(fd, sa, 16) ~= 0 or syscall.listen(fd, 2) ~= 0 then
        send_notification("ghostldr diag: port " .. REPORT_PORT .. " unavailable")
        syscall.close(fd)
        return
    end
    local fds, tv, req = malloc(128), malloc(16), malloc(2048)
    for _ = 1, 3 do -- three views at most, five minutes of waiting each
        write_buffer(fds, string.rep("\0", 128))
        write8(fds + (fd // 8), 1 << (fd % 8))
        write64(tv, 300)
        write64(tv + 8, 0)
        if syscall.select(fd + 1, fds, 0, 0, tv) <= 0 then break end
        local c = syscall.accept(fd, 0, 0)
        if c >= 0 then
            syscall.read(c, req, 2048)
            local body = "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n" .. text
            syscall.write(c, body, #body)
            syscall.close(c)
        end
    end
    syscall.close(fd)
end

-- the run
log("ghostldr PS4 memory-limit diagnostic: platform %s, firmware %s, title %s", tostring(PLATFORM),
    tostring(FW_VERSION), tostring(TITLE_ID))
log("direct memory size %d MiB", dmem_size // MiB)
local before = allocatable(0)
log("the kernel hands out %d MiB more direct memory", before // MiB)

local summary
local name, why = limit_sysctl_name()
if not name then
    log("limit sysctl: %s", why)
    summary = "ghostldr diag: limit sysctl not found"
else
    local value, len = sysctl_read(name)
    if not value then
        log("sysctl %s: %s", name, len)
        summary = string.format("ghostldr diag: %s: %s", name, len)
    else
        log("sysctl %s = 0x%x (%d MiB), %d bytes", name, value, value // MiB, len)
        summary = string.format("ghostldr diag: %s = %d MiB", name, value // MiB)
        if PLATFORM ~= "PS4" then
            log("not a PS4: no write tried")
        else
            local want = 4608 * MiB
            local wrote, err = sysctl_write(name, want)
            log("write %d MiB: %s", want // MiB, wrote and "TAKEN" or err)
            if wrote then
                local value2 = sysctl_read(name)
                local after = allocatable(0)
                log("now %s = %d MiB; the kernel hands out %d MiB more direct memory", name,
                    (value2 or 0) // MiB, after // MiB)
                local back, berr = sysctl_write(name, value)
                log("written back to %d MiB: %s", value // MiB, back and "ok" or berr)
                summary = summary .. string.format(", raise TAKEN: %d -> %d MiB free", before // MiB, after // MiB)
            else
                summary = summary .. ", raise refused"
            end
        end
    end
end
resource_limits()
log("done")
serve_report(summary)
