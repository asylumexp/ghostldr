-- Build a copy of SWRR's Luac0re save for another PSN account, console side.
-- Started through Luac0re's remote loader by ghostldr/host/resign_save.py,
-- which prepends MODE, TEMP_DIR, ACCOUNT_ID_BYTES (8 bytes, little endian as
-- param.sfo stores them), PATCH_DIR (false keeps TEMP_DIR as the directory
-- name: a control), PROBE_LOG_IP and PROBE_LOG_PORT.
--
-- The image of a save can only be re-encrypted by the console, so the copy is
-- made by the system itself: a new container TEMP_DIR (same size as the
-- original) is created, the files of the mounted Luac0re save (/savedata0,
-- read only) are copied in, and its sce_sys/param.sfo gets ACCOUNT_ID and
-- SAVEDATA_DIRECTORY "SLUS-20268" before the unmount commits it.  The
-- original save is only read, never mounted or written.
--   MODE "copy"   create TEMP_DIR from /savedata0 and patch it
--   MODE "patch"  re-patch an existing TEMP_DIR (another account)
--   MODE "show"   print TEMP_DIR's param.sfo fields and files
--   MODE "delete" delete TEMP_DIR

init_dlsym()
local sendto = func_wrap(dlsym(LIBKERNEL_HANDLE, "sendto"))
local log_fd = create_socket(AF_INET, SOCK_DGRAM, 0)
local laddr = malloc(16)
for i = 0, 15 do write8(laddr + i, 0) end
write8(laddr + 1, AF_INET)
write16(laddr + 2, ((PROBE_LOG_PORT << 8) | (PROBE_LOG_PORT >> 8)) & 0xFFFF)
local a, b, c, d = PROBE_LOG_IP:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
write32(laddr + 4, (tonumber(d) << 24) | (tonumber(c) << 16) | (tonumber(b) << 8) | tonumber(a))
local line_buf = malloc(1024)
local function say(s)
    s = "[save-resign] " .. s
    write_string(line_buf, s)
    sendto(log_fd, line_buf, #s, 0, laddr, 16)
end
local function hex(v) return string.format("%x", v & 0xFFFFFFFF) end
local function failed(v) return v < 0 or (v & 0xFFFFFFFF) >= 0x80000000 end
-- Luac0re's buffer reader writes into an interned string: read bytes instead
local function cstring(p, max)
    local out = {}
    for i = 0, max - 1 do
        local ch = read8(p + i)
        if ch == 0 then break end
        out[#out + 1] = string.char(ch)
    end
    return table.concat(out)
end

local kopen = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelOpen"))
local kread = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelRead"))
local kpread = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelPread"))
local kwrite = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelWrite"))
local kpwrite = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelPwrite"))
local kclose = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelClose"))
local kstat = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelStat"))
local kmkdir = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelMkdir"))
local kgetdents = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelGetdents"))
local usleep = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelUsleep"))
local load_start = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
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

local dir, params, result, st, path, path2, mp_buf, del = malloc(32), malloc(128), malloc(64), malloc(0x100),
    malloc(512), malloc(512), malloc(16), malloc(64)
local CH = 1024 * 1024
local buf = malloc(CH)
local dents = malloc(0x4000)
local SOURCE = "/savedata0"
local ORIGINAL = "SLUS-20268"

local function set_dir(n)
    for i = 0, 31 do write8(dir + i, 0) end
    write_string(dir, n)
end
local function do_mount(mode, blocks)
    for i = 0, 120, 8 do write64(params + i, 0) end
    for i = 0, 56, 8 do write64(result + i, 0) end
    write32(params + 0x00, user)
    write64(params + 0x10, dir)
    write64(params + 0x20, blocks)
    write32(params + 0x28, mode)
    local rc = mount(params, result)
    return rc, cstring(result, 16)
end
local function do_umount(mp)
    for i = 0, 15 do write8(mp_buf + i, 0) end
    write_string(mp_buf, mp)
    local rc = umount(mp_buf)
    local polls = 0
    while polls < 240 and kstat(mp_buf, st) == 0 do
        usleep(250000)
        polls = polls + 1
    end
    return rc
end
local function do_delete()
    for i = 0, 56, 8 do write64(del + i, 0) end
    write32(del + 0x00, user)
    write64(del + 0x10, dir)
    return delete(del)
end
local function size_of(p)
    write_string(path, p)
    if kstat(path, st) ~= 0 then return -1 end
    return read64(st + 0x48)
end

-- immediate children of a directory: {name, is_dir}
local function list(p)
    write_string(path, p)
    local fd = kopen(path, 0x20000, 0)          -- O_DIRECTORY
    local out = {}
    if failed(fd) then return out end
    while true do
        local n = kgetdents(fd, dents, 0x4000)
        if failed(n) or n == 0 then break end
        local off = 0
        while off < n do
            local reclen = read16(dents + off + 4)
            local typ = read8(dents + off + 6)
            local nm = cstring(dents + off + 8, read8(dents + off + 7))
            if nm ~= "." and nm ~= ".." and reclen > 0 then out[#out + 1] = {nm, typ == 4} end
            if reclen == 0 then break end
            off = off + reclen
        end
    end
    kclose(fd)
    return out
end

local function copy_file(src, dst)
    write_string(path, src)
    local ifd = kopen(path, 0, 0)
    if failed(ifd) then return "open " .. src .. " -> " .. hex(ifd) end
    write_string(path2, dst)
    local ofd = kopen(path2, 0x601, 0x1b6)      -- O_WRONLY | O_CREAT | O_TRUNC
    if failed(ofd) then kclose(ifd) return "create " .. dst .. " -> " .. hex(ofd) end
    local total, err = 0, nil
    while true do
        local n = kread(ifd, buf, CH)
        if failed(n) then err = "read " .. src .. " -> " .. hex(n) break end
        if n == 0 then break end
        local w = kwrite(ofd, buf, n)
        if w ~= n then err = "write " .. dst .. " -> " .. hex(w) break end
        total = total + n
    end
    kclose(ifd)
    local crc = kclose(ofd)
    if err then return err end
    if crc ~= 0 then return "close " .. dst .. " -> " .. hex(crc) end
    return nil, total
end

-- copies every file below src except sce_sys; returns an error or nil
local function copy_tree(src, dst, rel)
    for _, e in ipairs(list(src .. rel)) do
        local r = rel .. "/" .. e[1]
        if e[2] then
            if r ~= "/sce_sys" then
                write_string(path, dst .. r)
                local rc = kmkdir(path, 0x1ff)
                if failed(rc) then return "mkdir " .. r .. " -> " .. hex(rc) end
                local err = copy_tree(src, dst, r)
                if err then return err end
            end
        else
            local err, n = copy_file(src .. r, dst .. r)
            if err then return err end
            say(string.format("copied %s (%d bytes)", r, n))
        end
    end
    return nil
end

-- param.sfo: header "\0PSF", version, key table, data table, count; entries
-- {key u16, fmt u16, len u32, max u32, data u32}
local function sfo_find(p, key)
    local kt, dt, n = read32(p + 8), read32(p + 12), read32(p + 16)
    for i = 0, n - 1 do
        local e = p + 20 + 16 * i
        if cstring(p + kt + read16(e), 64) == key then
            return e, p + dt + read32(e + 12), read32(e + 8)
        end
    end
    return nil
end
local function sfo_show(p, label)
    local e, v = sfo_find(p, "ACCOUNT_ID")
    local acct = {}
    if e then for i = 0, 7 do acct[#acct + 1] = string.format("%02x", read8(v + i)) end end
    local _, dv = sfo_find(p, "SAVEDATA_DIRECTORY")
    local _, tv = sfo_find(p, "TITLE_ID")
    local _, pv = sfo_find(p, "PARAMS")
    say(string.format("%s: ACCOUNT_ID %s, SAVEDATA_DIRECTORY %s, TITLE_ID %s, PARAMS user %s", label,
        table.concat(acct), dv and cstring(dv, 32) or "?", tv and cstring(tv, 12) or "?",
        pv and hex(read32(pv + 4)) or "?"))
end
-- reads the param.sfo at mount point mp into buf; returns its size or nil
local function sfo_read(mp)
    write_string(path, mp .. "/sce_sys/param.sfo")
    local fd = kopen(path, 0, 0)
    if failed(fd) then say("open param.sfo -> " .. hex(fd)) return nil end
    local n = kpread(fd, buf, CH, 0)
    kclose(fd)
    if failed(n) or read32(buf) ~= 0x46535000 then say("param.sfo read -> " .. hex(n)) return nil end
    return n
end
local function sfo_patch(mp)
    local n = sfo_read(mp)
    if not n then return false end
    sfo_show(buf, "before")
    local e, v, max = sfo_find(buf, "ACCOUNT_ID")
    if not e then say("no ACCOUNT_ID field") return false end
    for i = 1, 8 do write8(v + i - 1, ACCOUNT_ID_BYTES[i]) end
    local de, dv, dmax = sfo_find(buf, "SAVEDATA_DIRECTORY")
    if not de then say("no SAVEDATA_DIRECTORY field") return false end
    if PATCH_DIR then
        for i = 0, dmax - 1 do write8(dv + i, 0) end
        write_string(dv, ORIGINAL)
        write32(de + 4, #ORIGINAL + 1)
    end
    sfo_show(buf, "after")
    write_string(path, mp .. "/sce_sys/param.sfo")
    local fd = kopen(path, 2, 0)                 -- O_RDWR
    if failed(fd) then
        -- sce_sys is the system's (EACCES for the app): with the jailbreak
        -- the host patches it over FTP while the container stays mounted
        say(string.format("param.sfo is not writable from the app (%s): hold %s", hex(fd), mp))
        for _ = 1, 300 do
            usleep(2000000)
            if sfo_read(mp) then
                local _, v2 = sfo_find(buf, "ACCOUNT_ID")
                local _, d2 = sfo_find(buf, "SAVEDATA_DIRECTORY")
                local ok = cstring(d2, 32) == (PATCH_DIR and ORIGINAL or TEMP_DIR)
                for i = 1, 8 do ok = ok and read8(v2 + i - 1) == ACCOUNT_ID_BYTES[i] end
                if ok then
                    sfo_show(buf, "patched by the host")
                    return true
                end
            end
        end
        say("the host did not patch param.sfo in 10 minutes")
        return false
    end
    local w = kpwrite(fd, buf, n, 0)
    local crc = kclose(fd)
    if w ~= n or crc ~= 0 then say(string.format("param.sfo write %s, close %s", hex(w), hex(crc))) return false end
    return true
end

local function show()
    set_dir(TEMP_DIR)
    local rc, mp = do_mount(1, 0)
    if rc ~= 0 then say("mount " .. TEMP_DIR .. " read-only -> " .. hex(rc)) return end
    if sfo_read(mp) then sfo_show(buf, TEMP_DIR) end
    local function walk(rel)
        for _, e in ipairs(list(mp .. rel)) do
            local r = rel .. "/" .. e[1]
            if e[2] then walk(r) else say(string.format("  %s %d", r, size_of(mp .. r))) end
        end
    end
    walk("")
    say("unmount -> " .. hex(do_umount(mp)))
end

say(string.format("mode %s, temp %s, user %x", MODE, TEMP_DIR, user))
if TEMP_DIR == ORIGINAL or #TEMP_DIR ~= #ORIGINAL then
    say("refusing: TEMP_DIR must be another 10-character name")
elseif MODE == "copy" then
    if size_of(SOURCE .. "/lua/init.lua") <= 0 or size_of(SOURCE .. "/VMC0.card") <= 0 then
        say(SOURCE .. " is not the Luac0re save")
    elseif sfo_read(SOURCE) then
        sfo_show(buf, "original")
        local _, bv = sfo_find(buf, "SAVEDATA_BLOCKS")
        local blocks = bv and read64(bv) or 0
        set_dir(TEMP_DIR)
        do_delete()
        local rc, mp = do_mount(6, blocks)       -- CREATE | RDWR
        say(string.format("create %s (%d blocks) -> %s at %s", TEMP_DIR, blocks, hex(rc), mp))
        if rc == 0 then
            local err = copy_tree(SOURCE, mp, "")
            if err then say("copy failed: " .. err) end
            local patched = not err and sfo_patch(mp)
            say("commit -> " .. hex(do_umount(mp)))
            if patched then show() else say("deleting " .. TEMP_DIR .. " -> " .. hex(do_delete())) end
        end
    end
elseif MODE == "patch" then
    set_dir(TEMP_DIR)
    local rc, mp = do_mount(2, 0)
    say("mount " .. TEMP_DIR .. " -> " .. hex(rc) .. " " .. mp)
    if rc == 0 then
        sfo_patch(mp)
        say("commit -> " .. hex(do_umount(mp)))
        show()
    end
elseif MODE == "show" then
    show()
elseif MODE == "delete" then
    set_dir(TEMP_DIR)
    say("delete -> " .. hex(do_delete()))
end
say("done")
