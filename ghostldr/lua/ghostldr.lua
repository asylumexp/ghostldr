-- ghostldr bootstrap for Luac0re 2.4b (SWRR US CUSA03474 or EU CUSA03492, after jit_init())
-- or UltraC0re (Jak X EU CUSA07992 1.04, after its jit_init() and jit_compat_init()).
--
-- Three ways in, by GHOSTLDR_MODE:
--   "connect"  run.py prepends the values below: the extension and the game
--              selector come from TCP servers on the computer, the log goes
--              to it over UDP.
--   "listen"   the release's ghostldr.lua (tools/mkbundle.py): the core, the
--              title profiles, defaults and the pins of the two bundles are in
--              it.  The bundle comes from the cache, a save container of
--              ghostldr's own (GLDRCACHE) holding the last bundle taken, when
--              it is this build's; else ghostldr.lua opens TCP 9021 (9022 when
--              that is taken) and waits up to 5 minutes for ghostldr-ext.bin
--              (boot a game) or ghostldr-elfldr.bin (the ELF loader), checks
--              it against the pins, caches it, and the log goes back on that
--              connection.  On a PS5, ghostldr-ext.bin (cached or sent) opens
--              the game selector at once with the ELF loader listening beside
--              it on that port; once a game is picked the ELF loader stops,
--              the extension's executable memory goes, and a second call
--              boots the pick.  A cached ghostldr-elfldr.bin starts the ELF
--              loader alone.  A bundle sent to the ELF loader (any build's,
--              checked against its own header) is cached for the next boot
--              (docs/architecture.md, "Two-file boot" and "The bundle
--              cache").  A PS4 or Jak X opens the selector without it.  Nothing of SWRR's is
--              touched before a bundle is in hand.  The prompt and every
--              outcome show on the TV, in Luac0re's message dialog, as well
--              as in a notification and on the bundle's connection.
--   "clear-cache"  run.py clear-cache: delete the cache, nothing else.
--              GHOSTLDR_CLEAR_CACHE = true before the release's ghostldr.lua
--              does the same, then carries on without one.
--
-- The host tool prepends:
--   GHOSTLDR_CORE_B64   base64 of build/ghostldr/core.bin
--   GHOSTLDR_TEXT_SIZE  bytes of code placed in JIT RX memory (16 KiB aligned)
--   GHOSTLDR_IMAGE_SIZE total image size including bss
--   GHOSTLDR_LOG_IP / GHOSTLDR_LOG_PORT   UDP log sink (connect)
--   GHOSTLDR_FLAGS      1 = probe only (ctx flags in native/include/ghost.h);
--                       this script adds 8192 on a PS4 (Luac0re's PLATFORM)
--                       and 32768 under UltraC0re
--   GHOSTLDR_APP_ROOT   where the title's app0 is: "ghostfs", a directory, or
--                       "usb:/path" on a USB drive (docs/usb.md)
--   GHOSTLDR_EXT_PORT / GHOSTLDR_EXT_SIZE / GHOSTLDR_EXT_CRC
--   GHOSTLDR_EXT_SHA256 (the blob's SHA-256 hex; the core pins its
--   first 24 bytes against the served blob after the CRC screen)
--                       the runtime extension (build/ghostldr/ext.bin, with the
--                       NID table): run.py serves it over TCP on the log IP;
--                       size 0 = none.  This script only connects the socket:
--                       the core reads the blob itself and checks size and CRC.
--   GHOSTLDR_SEL_PORT / GHOSTLDR_SEL_SIZE / GHOSTLDR_SEL_CRC
--                       the same for the game selector (build/ghostldr/selector.bin)
--   GHOSTLDR_PROFILE_B64  the title profiles (host/profiles.py boot_bundle); the
--                       core finds them through the last u64 of the context
--   GHOSTLDR_BUILD      (listen) the build's ID, 16 hex digits, also in each
--                       bundle's header
--   GHOSTLDR_BUNDLES    (listen) per bundle kind (1 full, 2 ELF loader): its
--                       file name and its parts' sizes, CRC-32s and SHA-256s

local MODE = GHOSTLDR_MODE
local LISTEN, CLEAR = MODE == "listen", MODE == "clear-cache" or GHOSTLDR_CLEAR_CACHE
-- Luac0re keeps one Lua state: the next script sets its own
GHOSTLDR_MODE, GHOSTLDR_CLEAR_CACHE = nil, nil
-- UltraC0re parks the compiler on its own loop (door1.lua) and keeps
-- Luac0re's jit_* names for the rest (jitcompat.lua)
local ULTRA = J_COMP ~= nil and J_SPIN ~= nil

-- listen mode: the bundle's connection.  Lines to it start "ghostldr: ";
-- from the core on, the core's log follows on it.  Lines said before there
-- is one wait for the next.
local conn, pending = -1, {}
local line_buf
local function say(msg)
    local s = "ghostldr: " .. msg
    s = (#s > 1000 and s:sub(1, 1000) or s) .. "\n"
    if conn < 0 then
        if LISTEN and #pending < 16 then pending[#pending + 1] = s end
        return
    end
    line_buf = line_buf or malloc(1024)
    write_buffer(line_buf, s)
    syscall.write(conn, line_buf, #s)
end
local function flush_pending()
    local p = pending
    pending = {}
    for _, s in ipairs(p) do say(s:sub(11, -2)) end
end

-- On the TV: the message dialog Luac0re's loader shows its own text in
-- (show_dialog, run before the core: ghost_boot closes it before a game
-- starts, so it is ghostldr's until then).  The text is kept referenced
-- while it shows.
local shown
local function screen(text)
    shown = "ghostldr " .. (GHOSTLDR_BUILD or "") .. "\n\n" .. text
    if show_dialog then pcall(show_dialog, shown) end
end
-- what Luac0re's loader shows, for when SWRR goes back to it
local function loader_text()
    local ip = get_current_ip and get_current_ip()
    return string.format("%s\nRemote Lua Loader\nPlatform: %s\nFW: %s\nTitle ID: %s\nListening: %s:%d",
        version_string or "Luac0re", tostring(PLATFORM), tostring(FW_VERSION), tostring(TITLE_ID), ip or "?",
        ULTRA and 8888 or 9026)
end
-- One outcome, said three ways: a line on the bundle's connection (the
-- sender's log), a notification, and the dialog on the TV (tv: its text)
local function event(line, tv)
    say(line)
    send_notification("ghostldr: " .. (#line > 300 and line:sub(1, 300) .. "..." or line))
    if tv then screen(tv) end
end

local cleanup = {}      -- listen mode: what a failure gives back (the bundle's memory, its connection)
local function fail(msg)
    say(msg)
    for i = #cleanup, 1, -1 do pcall(cleanup[i]) end
    cleanup = {}
    send_notification("ghostldr: " .. msg)
    if LISTEN then screen("Stopped: " .. msg .. "\n\n" .. loader_text()) end
    error("ghostldr: " .. msg)
end

local function u32le_ip(ip)
    local a, b, c, d = ip:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
    return (tonumber(d) << 24) | (tonumber(c) << 16) | (tonumber(b) << 8) | tonumber(a)
end

if ULTRA then
    if not J_STUB or not NEW_JIT_SOCK or not jit_memcpy_addr then
        fail("UltraC0re's compiler calls are not initialized (jit_compat_init)")
    end
elseif not NEW_JIT_SOCK or not JIT_ROP then
    fail("Luac0re JIT exploit is not initialized")
end
init_dlsym()
local BRIDGE_ADDR = BRIDGE_BASE or BRIDGE
-- The core calls dlsym through the eboot's "call rbx; ret" gadget, and the
-- kernel takes the caller from the callee's frame: it must build one, as
-- Luac0re's SCE_KERNEL_DLSYM (sceKernelDlsym itself) does.  UltraC0re's is
-- the bare dynlib_dlsym system call, which leaves the core's frame (JIT
-- memory) in place and is refused, so the core gets the export instead.
local core_dlsym = SCE_KERNEL_DLSYM
if ULTRA then
    core_dlsym = dlsym(LIBKERNEL_HANDLE, "sceKernelDlsym")
    if core_dlsym == 0 then fail("dlsym sceKernelDlsym") end
end

-- Bridge memory (shared with the compiler) for the core's staging buffer and
-- its private scratch (jitrpc.c PRIV: oob_scratch + 0x70000 on, ~0x21000).
-- Luac0re: its own OOB scratch, and 4 MiB at SHELLCODE_SCRATCH.  Jak X's
-- bridge holds the emulator's data at +0..+0x2f9000 and +0x90f000 on, and
-- UltraC0re's at +0x500000..+0x504400; +0x540000 (SHELLCODE_SCRATCH) to
-- +0x90f000 stays zero (SWRR-game-bridge research/jakx/bridge_probe.lua):
-- staging +0x540000, scratch up to about +0x761000.
local staging, staging_size, oob_scratch = SHELLCODE_SCRATCH, 0x400000, OOB_SCRATCH_BASE
if ULTRA then
    staging, staging_size = SHELLCODE_SCRATCH, 0x200000
    oob_scratch = SHELLCODE_SCRATCH + staging_size - 0x70000
end

local k = {}
local function kfn(name)
    if not k[name] then
        local addr = dlsym(LIBKERNEL_HANDLE, name)
        if addr == 0 then fail("dlsym " .. name) end
        k[name] = func_wrap(addr)
    end
    return k[name]
end

local function s32(v) v = v & 0xFFFFFFFF return v >= 0x80000000 and v - 0x100000000 or v end
local function hex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end
local function why() return (get_error_string():gsub("%s*\n%s*", ", ")) end
local function pages(n) return (n + 0x3FFF) & ~0x3FFF end
local function mib(n) return string.format("%.1f MiB", n / 1048576) end

-- ---------------------------------------------------------------- listen mode: the bundle
-- Its header (tools/mkbundle.py), 128 bytes, little-endian: "GLDRBNDL",
-- format 1, header size, kind (1 ghostldr-ext.bin: ext.bin and selector.bin;
-- 2 ghostldr-elfldr.bin: ext-elfldr.bin alone), flags, the extension's and
-- the selector's sizes, CRC-32s and SHA-256s, the build ID; then the
-- extension, then the selector.
local BUNDLE_MAGIC, BUNDLE_HEADER, HEADER_FMT = "GLDRBNDL", 128, "<c8I4I4I4I4I8I8I4I4c32c32c8"
local BUNDLE_PORTS = {9021, 9022}   -- 9021: the scene's ELF loader port, free on a stock console
local BUNDLE_WAIT_S = 300
local KIND_FILE = {[1] = "ghostldr-ext.bin", [2] = "ghostldr-elfldr.bin"}
local CTX_FLAG_ELFLDR, CTX_FLAG_BUNDLE, CTX_ELFLDR_CAN_BOOT = 1 << 20, 1 << 21, 1 << 56
local CTX_FLAG_SELECT = 2048    -- the game selector even with one title: a bundle never boots straight into a game
local EL_BOOT, EL_BUNDLE, EL_FORGET = 1, 2, 3   -- the ELF loader's results (ext/include/elfldr.h)
-- the ELF loader beside the game selector (ghost.h CTX_ELFLDR_SELECT, struct el_select): the core's result
-- once a game is picked, and the pick's fields
local CTX_ELFLDR_SELECT, EL_PICKED = 1 << 57, 4
local SEL_INDEX, SEL_USER, SEL_TITLE, SEL_BOOT = 0x18, 0x1c, 0x20, 0x30
local SO_REUSEADDR_, SO_NOSIGPIPE, SO_RCVBUF, SO_SNDTIMEO, SO_RCVTIMEO = 0x4, 0x800, 0x1002, 0x1005, 0x1006
-- the extension's ELF loader runs on a PS5 in SWRR (Luac0re); not on a PS4 or in Jak X
local HAS_ELFLDR = PLATFORM ~= "PS4" and not ULTRA

local opt
local function setopt(fd, name, value, timeval)   -- an int option, or a timeval of whole seconds
    opt = opt or malloc(16)
    write64(opt, value)
    write64(opt + 8, 0)
    return syscall.setsockopt(fd, SOL_SOCKET, name, opt, timeval and 16 or 4)
end

local function sockaddr(port)
    local sa = malloc(16)
    write64(sa, 0)
    write64(sa + 8, 0)
    write8(sa + 1, AF_INET)
    write16(sa + 2, ((port << 8) | (port >> 8)) & 0xFFFF)
    return sa
end

-- A TCP socket, made as Luac0re's create_socket makes it (SWRR's own
-- socket() on a PS4 before 14.00 or a PS5 before 8.00, else one the JIT
-- compiler process makes and hands over), but with the compiler's copy
-- closed, as the extension's jit_socket does: Luac0re leaves it open, and it
-- keeps the socket, and a port it listens on, after SWRR closes its own.
local function tcp_socket()
    local fw = tonumber(FW_VERSION) or 99
    if ULTRA or (PLATFORM == "PS4" and fw < 14.00) or (PLATFORM == "PS5" and fw < 8.00)
            or not (jit_syscall and jit_syscall.socket and jit_syscall.close and jit_send_recv_fd) then
        return create_socket(AF_INET, SOCK_STREAM, 0)
    end
    local jfd = jit_syscall.socket(AF_INET, SOCK_STREAM, 0)
    if jfd < 0 then return -1 end
    local fd = jit_send_recv_fd(jfd, NEW_JIT_SOCK, NEW_MAIN_SOCK)
    jit_syscall.close(jfd)
    return fd
end

-- a TCP socket listening on port (every address), or -1 and why not
local function listen_on(port)
    local fd = tcp_socket()
    if fd < 0 then return -1, "no socket" end
    setopt(fd, SO_REUSEADDR_, 1)
    setopt(fd, SO_RCVBUF, 2 << 20)   -- connections inherit it: the bundle in fewer reads
    if syscall.bind(fd, sockaddr(port), 16) < 0 or syscall.listen(fd, 4) < 0 then
        local e = why()
        syscall.close(fd)
        return -1, e
    end
    return fd
end

-- flexible memory, as ext.c's blob_alloc takes: the core unmaps what it takes
local function map_flex(size)
    local out = malloc(8)
    write64(out, 0)
    if s32(kfn("sceKernelMapFlexibleMemory")(out, pages(size), PROT_READ | PROT_WRITE, 0)) ~= 0 then return nil end
    return read64(out)
end
local function unmap(addr, size) kfn("munmap")(addr, pages(size)) end

-- n bytes from the connection: true, or false, how many came and the last read's result
local function recv_into(addr, n)
    local got = 0
    while got < n do
        local r = syscall.read(conn, addr + got, math.min(n - got, 0x100000))
        if r <= 0 then return false, got, r end
        got = got + r
    end
    return true, got
end

local crc_table
local function crc32(addr, n)
    if not crc_table then
        crc_table = {}
        for i = 0, 255 do
            local c = i
            for _ = 1, 8 do c = (c >> 1) ~ (0xEDB88320 & -(c & 1)) end
            crc_table[i] = c
        end
    end
    local c = 0xFFFFFFFF
    for off = 0, n - 1, 0x10000 do
        local s = read_buffer(addr + off, math.min(0x10000, n - off))
        for i = 1, #s do c = crc_table[(c ~ s:byte(i)) & 0xFF] ~ (c >> 8) end
    end
    return c ~ 0xFFFFFFFF
end

local scratch, ip, port, port_note
local waiting = ""      -- the standing prompt (receive_bundle), under each refusal on the TV
-- A bundle not taken: the reason to the sender, in a notification and on
-- the TV (tv: a shorter form for the dialog, with what was expected), and
-- the prompt again; what it still sends is read first, so it reads the
-- reason before the close
local function refuse(msg, tv)
    event("refused: " .. msg, "Refused: " .. (tv or msg) .. "\n\n" .. waiting)
    setopt(conn, SO_RCVTIMEO, 2, true)
    local total = 0
    while total < (64 << 20) do
        local r = syscall.read(conn, scratch, 0x10000)
        if r <= 0 then break end
        total = total + r
    end
    syscall.close(conn)
    conn = -1
end

local function pin_of(kind)
    for _, b in ipairs(GHOSTLDR_BUNDLES or {}) do
        if b.kind == kind then return b end
    end
end

-- A bundle's header against this ghostldr.lua's pins: kind, file, the parts'
-- sizes and pin; bad says what is wrong: "magic", "format" or "build" (another
-- release's), "pins" (this build's ID, other contents: damaged)
local function check_header(h)
    local magic, format, hsize, kind, _, ext_size, sel_size, ext_crc, sel_crc, ext_sha, sel_sha, build =
        string.unpack(HEADER_FMT, h)
    local r = {kind = kind, file = KIND_FILE[kind] or string.format("a bundle of kind %d", kind), format = format,
               build = hex(build), ext_size = ext_size, sel_size = sel_size, pin = pin_of(kind), header = h}
    local pin = r.pin
    if magic ~= BUNDLE_MAGIC then
        r.bad = "magic"
    elseif format ~= 1 or hsize ~= BUNDLE_HEADER then
        r.bad = "format"
    elseif r.build ~= GHOSTLDR_BUILD then
        r.bad = "build"
    elseif not pin or ext_size ~= pin.ext_size or sel_size ~= pin.sel_size or ext_crc ~= pin.ext_crc
            or sel_crc ~= pin.sel_crc or hex(ext_sha) ~= pin.ext_sha256 or hex(sel_sha) ~= pin.sel_sha256 then
        r.bad = "pins"
    end
    return r
end

-- what to say and show about a header check_header found wrong (what: whose
-- it is; advice: the TV's last line)
local function header_problem(r, what, advice)
    if r.bad == "format" then
        return string.format("%s has bundle format %d; this ghostldr.lua (build %s) reads format 1: send "
            .. "ghostldr.lua and the bundle from the same release", what, r.format, GHOSTLDR_BUILD),
            string.format("%s has bundle format %d, expected 1.\n%s", what, r.format, advice)
    elseif r.bad == "build" then
        return string.format("version mismatch: %s is from build %s, this ghostldr.lua is build %s: send "
            .. "ghostldr.lua and %s from the same release", what, r.build, GHOSTLDR_BUILD,
            KIND_FILE[r.kind] or "the bundle"),
            string.format("Version mismatch: %s is from build %s,\nthis ghostldr.lua expects build %s.\n%s", what,
            r.build, GHOSTLDR_BUILD, advice)
    end
    return string.format("%s is damaged: its header does not match this ghostldr.lua's pins (build %s)", what,
        GHOSTLDR_BUILD), string.format("%s is damaged:\nit does not match this ghostldr.lua (build %s).\n%s", what,
        GHOSTLDR_BUILD, advice)
end

-- the connection's bundle, or nil (refused, said and closed)
local function take(from)
    setopt(conn, SO_NOSIGPIPE, 1)      -- a sender that goes away must not take SWRR with it
    setopt(conn, SO_RCVTIMEO, 30, true)
    setopt(conn, SO_SNDTIMEO, 5, true) -- a sender that stops reading ends the log (log.c), not the game
    flush_pending()
    local ok, got, r = recv_into(scratch, BUNDLE_HEADER)
    local h = read_buffer(scratch, got)
    if h ~= BUNDLE_MAGIC:sub(1, #h) and h:sub(1, 8) ~= BUNDLE_MAGIC then
        local what = h:sub(1, 4) == "\x7fELF" and "an ELF" or h:sub(1, 4) == "\x4f\x15\x3d\x1d" and "a SELF"
            or h:find("^%s*%-%-") and "a Lua script" or "not a ghostldr bundle (no GLDRBNDL header: a .bin from something else?)"
        local hint = (what == "an ELF" or what == "a SELF")
            and ": ELF payloads go to the ELF loader, which ghostldr-elfldr.bin starts (on this port)"
            or what == "a Lua script" and ": Lua scripts go to Luac0re's loader on TCP 9026" or ""
        refuse(string.format("%s (it starts %s)%s; this port takes ghostldr-ext.bin or ghostldr-elfldr.bin",
            what, hex(h:sub(1, 8)), hint), string.format("%s from %s.\nExpected ghostldr-ext.bin or ghostldr-elfldr.bin%s",
            what, from, hint ~= "" and ".\n" .. hint:sub(3) or ""))
        return nil
    end
    if not ok then
        refuse((got == 0 and (r == 0 and "nothing came" or "nothing came for 30 seconds")
               or string.format("the connection ended after %d bytes, inside the bundle's header", got))
               .. " from " .. from)
        return nil
    end
    local b = check_header(h)
    if b.bad then
        refuse(header_problem(b, b.file, "Send both from the same release"))
        return nil
    end
    if b.kind == 2 and not HAS_ELFLDR then
        refuse("the ELF loader runs on a PS5, in SWRR with Luac0re: send ghostldr-ext.bin to boot a game",
            "ghostldr-elfldr.bin: the ELF loader runs on a PS5 only.\nSend ghostldr-ext.bin to boot a game")
        return nil
    end
    local ext_size, sel_size = b.ext_size, b.sel_size
    local ext = map_flex(ext_size)
    local sel = ext and sel_size > 0 and map_flex(sel_size) or nil
    if not ext or (sel_size > 0 and not sel) then
        if ext then unmap(ext, ext_size) end
        refuse(string.format("no memory for its %d bytes: restart SWRR and try again", ext_size + sel_size))
        return nil
    end
    local n
    ok, n, r = recv_into(ext, ext_size)
    got = n
    if ok and sel then
        ok, n, r = recv_into(sel, sel_size)
        got = ext_size + n
    end
    if not ok then
        unmap(ext, ext_size)
        if sel then unmap(sel, sel_size) end
        refuse(string.format("%d of %s's %d bytes came (%s)", BUNDLE_HEADER + got, b.file,
            BUNDLE_HEADER + ext_size + sel_size, r == 0 and "the connection ended" or "nothing more for 30 seconds"))
        return nil
    end
    b.from, b.ext, b.sel = from, ext, sel or -1
    return b
end

-- ---------------------------------------------------------------- listen mode: the cache
-- One save container of ghostldr's own in the host's saved data (SWRR's; Jak
-- X's under UltraC0re), GLDRCACHE, holding the last bundle taken as it came:
-- header, extension, selector, in bundle.bin.  It is checked against this
-- ghostldr.lua's pins before use (header, size, the selector's CRC-32 here;
-- the core checks the extension's CRC-32 and SHA-256), so a cache from
-- another release or a damaged one is never run.  Calls as the ghostfs
-- installer's (gfs_receiver.lua): sceSaveDataMount from the eboot's import
-- table, file calls through libkernel's own exports, or the commit (unmount)
-- stays BUSY for good.
local CACHE_DIR, CACHE_FILE = "GLDRCACHE", "bundle.bin"
local SAVE_NOT_FOUND, SAVE_NO_SPACE = 0x809F0008, 0x809F000A
local sd, cached_kind, cached_build
local function save_calls()
    if sd ~= nil then return sd end
    sd = false
    local name, uid = malloc(64), malloc(8)
    local function lib(n)
        write_string(name, n)
        return s32(kfn("sceKernelLoadStartModule")(name, 0, 0, 0, 0, 0))
    end
    local h, us = lib("libSceSaveData.sprx"), lib("libSceUserService.sprx")
    local mount = read64(EBOOT_BASE + (ULTRA and 0xcd1e40 or 0x3893F0))
    local umount = h > 0 and dlsym(h, "sceSaveDataUmount") or 0
    local delete = h > 0 and dlsym(h, "sceSaveDataDelete") or 0
    local user = us > 0 and dlsym(us, "sceUserServiceGetInitialUser") or 0
    write32(uid, 0)
    if mount == 0 or umount == 0 or delete == 0 or user == 0 or s32(func_wrap(user)(uid)) ~= 0 then
        say(string.format("cache: no saved-data calls (libSceSaveData %x, libSceUserService %x)", h, us))
        return false
    end
    sd = {mount = func_wrap(mount), umount = func_wrap(umount), delete = func_wrap(delete), user = read32(uid),
          dir = malloc(32), params = malloc(128), result = malloc(64), mp = malloc(16), st = malloc(0x100),
          path = malloc(64)}
    for i = 0, 31 do write8(sd.dir + i, 0) end
    write_string(sd.dir, CACHE_DIR)
    return sd
end

-- mode 1 read-only, 6 create and read-write (blocks of 32 KiB): status, mount point
local function cache_mount(mode, blocks)
    local p = sd.params
    for i = 0, 120, 8 do write64(p + i, 0) end
    for i = 0, 56, 8 do write64(sd.result + i, 0) end
    write32(p, sd.user)
    if ULTRA then   -- Jak X's sceSaveDataMount2
        write64(p + 0x08, sd.dir)
        write64(p + 0x10, blocks)
        write32(p + 0x18, mode)
    else
        write64(p + 0x10, sd.dir)
        write64(p + 0x20, blocks)
        write32(p + 0x28, mode)
    end
    local rc = s32(sd.mount(p, sd.result))
    local mp = {}
    for i = 0, 15 do
        local c = read8(sd.result + i)
        if c == 0 then break end
        mp[#mp + 1] = string.char(c)
    end
    return rc, table.concat(mp)
end

-- the commit: unmount, then wait until the mount point is gone
local function cache_umount(mp)
    for i = 0, 15 do write8(sd.mp + i, 0) end
    write_string(sd.mp, mp)
    local rc = s32(sd.umount(sd.mp))
    for _ = 1, 240 do
        if s32(kfn("sceKernelStat")(sd.mp, sd.st)) ~= 0 then break end
        kfn("sceKernelUsleep")(250000)
    end
    return rc
end

local function cache_delete()
    cached_kind, cached_build = nil, nil
    if not save_calls() then return -1 end
    local p = sd.params
    for i = 0, 56, 8 do write64(p + i, 0) end
    write32(p, sd.user)
    write64(p + 0x10, sd.dir)
    return s32(sd.delete(p))
end

-- Settings > Advanced (the game selector's Settings, selector/src/conf.c): GLDRCONF/settings.txt's flags_on
-- and flags_off go into the flags this boots with.  Safe mode: pending=1 is written before they are used and
-- the selector clears it; finding it means the last boot with them never reached the selector, so this one
-- goes without them, once (pending=safe), and says so.  mark: the first read of a boot (writes the marker).
local function conf_flags(flags, mark)
    if not save_calls() then return flags end
    for i = 0, 31 do write8(sd.dir + i, 0) end
    write_string(sd.dir, "GLDRCONF")
    local text, rc, mp = "", cache_mount(1, 0)
    if rc == 0 then
        write_string(sd.path, mp .. "/settings.txt")
        local fd = s32(kfn("sceKernelOpen")(sd.path, 0, 0))
        if fd >= 0 then
            local n = s32(kfn("sceKernelPread")(fd, scratch, 2048, 0))
            if n > 0 then text = read_buffer(scratch, n) end
            kfn("sceKernelClose")(fd)
        end
        cache_umount(mp)
    end
    local on = tonumber(text:match("flags_on=0x(%x+)") or "0", 16)
    local off = tonumber(text:match("flags_off=0x(%x+)") or "0", 16)
    if mark and on | off ~= 0 then
        local safe = text:match("pending=(%w+)") == "1"
        text = text:gsub("pending=%w*\n?", "") .. (safe and "pending=safe\n" or "pending=1\n")
        rc, mp = cache_mount(2, 0)
        if rc == 0 then
            write_string(sd.path, mp .. "/settings.txt")
            local fd = s32(kfn("sceKernelOpen")(sd.path, 0x601, 0x1b6))
            if fd >= 0 then
                write_buffer(scratch, text)
                kfn("sceKernelWrite")(fd, scratch, #text)
                kfn("sceKernelClose")(fd)
            end
            cache_umount(mp)
        end
        if safe then
            event("the last boot with Settings > Advanced never reached the game selector: this one goes without it",
                "Settings > Advanced is left out of this boot:\nthe last boot with it stopped early.")
            on, off = 0, 0
        end
    end
    for i = 0, 31 do write8(sd.dir + i, 0) end
    write_string(sd.dir, CACHE_DIR)
    if on | off ~= 0 then say(string.format("settings: flags %x -> %x", flags, (flags | on) & ~off)) end
    return (flags | on) & ~off
end

-- The cached bundle, with its extension (parts 1), and its selector (parts
-- 2) mapped as a received one's: the bundle, or nil (none here), or nil and
-- "mismatch" with its header (another release's), or nil and "damaged" with
-- why (deleted here)
local function cache_read(parts)
    if not save_calls() then return nil end
    local rc, mp = cache_mount(1, 0)
    if rc ~= 0 then
        if rc == s32(SAVE_NOT_FOUND) then return nil end
        cache_delete()
        return nil, "damaged", string.format("its container does not mount (%x)", rc & 0xFFFFFFFF)
    end
    local kopen, kpread = kfn("sceKernelOpen"), kfn("sceKernelPread")
    write_string(sd.path, mp .. "/" .. CACHE_FILE)
    local size = s32(kfn("sceKernelStat")(sd.path, sd.st)) == 0 and read64(sd.st + 0x48) or -1
    local fd = s32(kopen(sd.path, 0, 0))
    local b, bad
    if fd < 0 or kpread(fd, scratch, BUNDLE_HEADER, 0) ~= BUNDLE_HEADER then
        bad = "its header is missing"
    else
        b = check_header(read_buffer(scratch, BUNDLE_HEADER))
        local total = BUNDLE_HEADER + b.ext_size + b.sel_size
        if b.bad == "magic" or b.bad == "pins" then
            bad = "its header does not match this ghostldr.lua's pins"
        elseif not b.bad and size ~= total then
            bad = string.format("it holds %d of the bundle's %d bytes", size, total)
        elseif not b.bad and parts > 0 then
            local sel_size = parts > 1 and b.sel_size or 0
            local ext = map_flex(b.ext_size)
            local sel = ext and sel_size > 0 and map_flex(sel_size)
            if not ext or (sel_size > 0 and not sel) then
                if ext then unmap(ext, b.ext_size) end
                kfn("sceKernelClose")(fd)
                cache_umount(mp)
                fail(string.format("no memory for the cached %s (%d bytes): restart SWRR and try again", b.file,
                    total))
            end
            b.ext, b.sel = ext, sel or -1
            if kpread(fd, b.ext, b.ext_size, BUNDLE_HEADER) ~= b.ext_size or (sel_size > 0 and
                    (kpread(fd, b.sel, sel_size, BUNDLE_HEADER + b.ext_size) ~= sel_size
                     or crc32(b.sel, sel_size) ~= b.pin.sel_crc)) then
                bad = sel_size > 0 and "its selector's CRC-32 is wrong" or "it did not read"
                unmap(b.ext, b.ext_size)
                if sel_size > 0 then unmap(b.sel, sel_size) end
            end
            b.sel_size = sel_size
        end
    end
    if fd >= 0 then kfn("sceKernelClose")(fd) end
    cache_umount(mp)
    if bad then
        cache_delete()
        return nil, "damaged", bad
    end
    if b.bad then return nil, "mismatch", b end
    b.cached = true
    cached_kind, cached_build = b.kind, b.build
    return b
end

-- b into the cache, replacing what is there, its header as it came (another
-- build's too: one sent to the ELF loader is for the next boot): true, or
-- false and why
local function cache_store(b)
    if not save_calls() then return false, "the saved-data calls are missing" end
    local total = BUNDLE_HEADER + b.ext_size + b.sel_size
    local needed = (total + 32767) // 32768
    local blocks = math.max(512, needed + 48 + math.max(64, needed // 16))   -- as gfs_pack.data_blocks
    cache_delete()
    local rc, mp = cache_mount(6, blocks)
    if rc ~= 0 then
        if rc == s32(SAVE_NO_SPACE) then
            return false, string.format("the console's storage is full: the cache needs %s, %s more than is free",
                mib(blocks * 32768), mib(read64(sd.result + 0x10) * 32768))
        end
        return false, string.format("its save container was not made (%x)", rc & 0xFFFFFFFF)
    end
    local kwrite = kfn("sceKernelWrite")
    write_string(sd.path, mp .. "/" .. CACHE_FILE)
    local fd, ok = s32(kfn("sceKernelOpen")(sd.path, 0x601, 0x1b6)), false
    if fd >= 0 then
        write_buffer(scratch, b.header)
        ok = kwrite(fd, scratch, BUNDLE_HEADER) == BUNDLE_HEADER and kwrite(fd, b.ext, b.ext_size) == b.ext_size
            and (b.sel_size == 0 or kwrite(fd, b.sel, b.sel_size) == b.sel_size)
        kfn("sceKernelClose")(fd)
    end
    rc = cache_umount(mp)
    if not ok or rc ~= 0 then
        cache_delete()
        return false, ok and string.format("its commit failed (%x)", rc & 0xFFFFFFFF) or "writing it failed"
    end
    cached_kind, cached_build = b.kind, b.build
    return true, string.format("%s save container %s, %s", CACHE_DIR, mib(blocks * 32768), mib(total))
end

-- ---------------------------------------------------------------- listen mode: waiting for a bundle
-- a listener on the bundle port: 9021, else 9022, the first time; the same port after
local function bundle_listener()
    if port then
        local fd, e = listen_on(port)
        if fd < 0 then fail(string.format("TCP %d for the bundle: %s", port, e)) end
        return fd
    end
    local taken = {}
    for _, p in ipairs(BUNDLE_PORTS) do
        local fd, e = listen_on(p)
        if fd >= 0 then
            port, port_note = p, p ~= BUNDLE_PORTS[1] and string.format(" (%d is taken)", BUNDLE_PORTS[1]) or ""
            return fd
        end
        taken[#taken + 1] = string.format("%d (%s)", p, e)
    end
    fail("no port for the bundle: TCP " .. table.concat(taken, ", "))
end

-- Wait on the bundle port (lfd: its listener, else one is opened) for a
-- bundle, which is returned, or BUNDLE_WAIT_S (nil, said, nothing left
-- open).  note: shown above the prompt.
local function receive_bundle(lfd, note)
    lfd = lfd or bundle_listener()
    local where = string.format("%s:%d%s", ip or "this console", port, port_note)
    waiting = string.format("Send ghostldr-ext.bin (games)\nor ghostldr-elfldr.bin (ELF loader)\nto %s\n\n"
        .. "Waiting %d minutes", where, BUNDLE_WAIT_S // 60)
    send_notification(string.format("ghostldr %s: send ghostldr-ext.bin (games) or ghostldr-elfldr.bin (ELF "
        .. "loader) to %s in the next %d minutes", GHOSTLDR_BUILD, where, BUNDLE_WAIT_S // 60))
    screen((note and note .. "\n\n" or "") .. waiting)
    local poll, clock = kfn("poll"), kfn("sceKernelGetProcessTime")
    local pfd, sa, alen = malloc(8), malloc(16), malloc(8)
    local deadline = clock() + BUNDLE_WAIT_S * 1000000
    while clock() < deadline do
        write32(pfd, lfd)
        write32(pfd + 4, 1)             -- POLLIN
        if s32(poll(pfd, 1, 1000)) > 0 then
            write32(alen, 16)
            local c = syscall.accept(lfd, sa, alen)
            if c >= 0 then
                conn = c
                local from = string.format("%d.%d.%d.%d", read8(sa + 4), read8(sa + 5), read8(sa + 6), read8(sa + 7))
                local b = take(from)
                if b then
                    syscall.close(lfd)  -- SWRR's copy; the compiler's went at once (tcp_socket)
                    return b
                end
            end
        end
    end
    syscall.close(lfd)
    event(string.format("nothing came on TCP %d in %d minutes: Luac0re's loader takes scripts again", port,
        BUNDLE_WAIT_S // 60), string.format("Timed out: nothing came on port %d in %d minutes.\nBack to Luac0re's "
        .. "loader: send ghostldr.lua again to retry.\n\n%s", port, BUNDLE_WAIT_S // 60, loader_text()))
    return nil
end

-- ---------------------------------------------------------------- the core
local function unb64(s)
    local v, out, n = {}, {}, 0
    local A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    for i = 1, 64 do v[A:byte(i)] = i - 1 end
    for i = 1, #s, 4 do
        local a, b, c, d = s:byte(i, i + 3)
        local x = v[a] << 18 | v[b] << 12 | (v[c] or 0) << 6 | (v[d] or 0)
        n = n + 1
        out[n] = string.char(x >> 16, x >> 8 & 0xFF, x & 0xFF)
    end
    local pad = #s:match("=*$")
    local r = table.concat(out)
    return pad > 0 and r:sub(1, -pad - 1) or r
end

-- 1. the core lives at the start of the emulator's IPU JIT program buffer
-- (RX here, RW in the compiler at the same address): the compiler process
-- can create almost no new JIT memory, so reuse what the emulator owns.
-- It is placed afresh for each call (fresh data pages: a second call is as
-- a second ghostldr.lua).
local core
local text_size = GHOSTLDR_TEXT_SIZE
local image_size = ((GHOSTLDR_IMAGE_SIZE or 0) + 0x3FFF) & ~0x3FFF
local data_size = image_size - (text_size or 0)
local rx = IPU_PROG_BASE + 0x4000
local small_arena = rx + image_size
local function load_core()
    if not core then
        core = unb64(GHOSTLDR_CORE_B64)
        GHOSTLDR_CORE_B64 = nil         -- the Lua state keeps the decoded copy only
    end
end
local function place_core()
    load_core()
    -- 2. code through the compiler (same address in both processes)
    write_buffer(SHELLCODE_SCRATCH, core:sub(1, text_size))
    jit_memcpy(rx, SHELLCODE_SCRATCH, text_size)
    -- 3. the core's data pages: replace that part of the RX buffer with RW memory
    local out = malloc(0x40)
    kfn("munmap")(rx + text_size, data_size)
    write64(out, rx + text_size)
    local rc = kfn("sceKernelMapFlexibleMemory")(out, data_size, PROT_READ | PROT_WRITE, 0x10)
    if rc ~= 0 or read64(out) ~= rx + text_size then
        fail(string.format("data map rc=%x at %x", rc, read64(out)))
    end
    -- 4. data directly
    if #core > text_size then
        write_buffer(rx + text_size, core:sub(text_size + 1))
    end
    if read64(rx) ~= string.unpack("<I8", core, 1) then fail("core text verify") end
end

-- 7. boot context (field order must match struct boot_ctx in ghost.h)
-- A syscall instruction in the compiler (jitrpc.c moves descriptors and
-- frees JIT blocks with it).  PS5: Luac0re's own (getpid + 7), which takes
-- any number from rax.  PS4: getpid's wrapper + 7 stands for "a system
-- call"; the core calls each number's own wrapper from compiler_stubs().
local function compiler_syscall()
    if jit_syscall.syscall_address then return jit_syscall.syscall_address end
    local w = jit_syscall.syscall_wrapper and jit_syscall.syscall_wrapper[SYSCALL_TABLE.getpid]
    if w and (jit_read64(w + 7) & 0xFFFFFFFFFF) == 0x050FCA8949 then return w + 7 end
    return 0
end
-- PS4: a system call runs only from its own number's libkernel wrapper,
-- "mov rax, N; mov r10, rcx; syscall" (Luac0re: "PS4 requires valid syscall
-- wrapper"); getpid's with another number in rax stopped the compiler.  The
-- compiler's wrapper for each number the core and the extension call there
-- (getpid, write, close, sendmsg, munmap, socket, clock_gettime; open, ioctl
-- and dmem_container for the diagnostic's direct memory checks), as
-- (number, address) pairs ended by a zero number.  Luac0re's table covers
-- its first 128 KiB of libkernel; the rest of the text is searched here, in
-- this process's copy, at the same offset.  Each wrapper's bytes are
-- checked in the compiler.  0 on a PS5.
local COMPILER_SYSCALLS = {0x14, 0x4, 0x6, 0x1c, 0x49, 0x61, 0xe8, 0x5, 0x36, 0x24a}
local function stub_ok(w, num)
    return jit_read64(w) == (0xC0C748 | (num << 24) | (0x49 << 56))
        and (jit_read64(w + 7) & 0xFFFFFFFFFF) == 0x050FCA8949
end
local libkernel_text
local function search_stub(num)
    if not (LIBKERNEL_BASE and JIT_LIBKERNEL_BASE and sceKernelGetModuleInfoFromAddr) then return nil end
    if not libkernel_text then
        local info, size = malloc(0x300), 0
        if sceKernelGetModuleInfoFromAddr(LIBKERNEL_BASE, 1, info) == 0 and read64(info + 0x160) == LIBKERNEL_BASE then
            size = read64(info + 0x168) & 0xFFFFFFFF   -- segments[0]: {address, size, prot}
        end
        libkernel_text = size > 0 and size <= 0x400000 and read_buffer(LIBKERNEL_BASE, size) or ""
    end
    local at = libkernel_text:find(string.pack("<I3I4I3", 0xC0C748, num, 0xCA8949) .. "\x0f\x05", 1, true)
    return at and JIT_LIBKERNEL_BASE + at - 1
end
local function compiler_stubs()
    if PLATFORM ~= "PS4" then return 0 end
    local tbl, n = malloc(#COMPILER_SYSCALLS * 16 + 16), 0
    for _, num in ipairs(COMPILER_SYSCALLS) do
        local w = jit_syscall.syscall_wrapper and jit_syscall.syscall_wrapper[num]
        if not (w and stub_ok(w, num)) then w = search_stub(num) end
        if w and stub_ok(w, num) then
            write64(tbl + n * 16, num)
            write64(tbl + n * 16 + 8, w)
            n = n + 1
        end
    end
    write64(tbl + n * 16, 0)
    return tbl
end
local function fw_number()
    local major, minor = tostring(FW_VERSION or ""):match("(%d+)%.(%d+)")
    return major and tonumber(major, 16) * 0x100 + tonumber(minor, 16) or 0
end
local base_flags = (GHOSTLDR_FLAGS or 0) | (PLATFORM == "PS4" and 8192 or 0) | (ULTRA and 32768 or 0)
-- the compiler chain's parts: 19 slots (a union in struct boot_ctx)
local engine
if ULTRA then
    local c = J_COMP
    engine = {
        c + C_PRAX, c + C_PRDI_PRBP, c + C_PRSI_PRBP, c + C_PRCX, c + C_MOVRDXRSI, c + C_WRDI,
        c + C_POPRSPDEREF, J_SPIN, J_CALL, J_RES, J_CNT, 0, 0, 0, 0, 0, 0, 0, 0,
    }
else
    engine = {
        JIT_ROP.RET, JIT_ROP.MOV_RAX_RDI_RET, JIT_ROP.POP_RDI_RET, JIT_ROP.POP_RSI_RET,
        JIT_ROP.MOV_RDX_RDI_RET, JIT_ROP.POP_RCX_RET, JIT_ROP.POP_RBX_RET,
        JIT_ROP.MOV_DEREF_RBX_RAX_RET, JIT_ROP.POP_RSP_RET, JIT_ROP.POP_R12_R13_R14_R15_RBP_RET,
        JIT_ROP.MOV_R8_RBX_RET, JIT_ROP.MOV_R9_RBX_RET,
        LEAK_RBX, LEAK_RBP, LEAK_R12, LEAK_R13, LEAK_R14, LEAK_R15,
        FAKE_RSP,
    }
end
local LOG_FD = 7 * 8                -- fields[8]: log_fd
local EXT = 45 * 8 + 0x40           -- ext_fd, then size, crc; sel_fd at +0x18
local CTX_IP, CTX_HEADER, CTX_SELECT = 0x200, 0x208, 0x210   -- listen mode, after the profile slot (ghost.h CTX_*_SLOT)
local stubs, prof_mem, ip_mem, header_mem, select_mem
-- o: flags, app_root, log_fd, log_addr, the extension's ext/ext_size/ext_crc/ext_sha256 and the
-- selector's sel/sel_size/sel_crc (ext and sel: a socket, or an address with CTX_FLAG_BUNDLE);
-- picked: boot the game picked beside the ELF loader (select_mem as the first call left it)
local function boot_ctx(o)
    stubs = stubs or compiler_stubs()
    local fields = {
        0x3130305244484c47, 0, EBOOT_BASE, core_dlsym,
        EBOOT_BASE + (ULTRA and 0x19869 or 0x31aa9),    -- the eboot's "call rbx; ret"
        LIBKERNEL_HANDLE, stubs, o.log_fd, o.log_addr,
        JIT_BASE,
    }
    for _, v in ipairs(engine) do fields[#fields + 1] = v end
    for _, v in ipairs({
        oob_scratch, JIT_ROP_CHAIN_START or 0, JIT_ROP_CHAIN_END or 0,
        jit_memcpy_addr, jit_calloc_addr, compiler_syscall(), jit_libc_error_addr or 0,
        jit_sceKernelJitCreateSharedMemory_addr, jit_sceKernelJitCreateAliasOfSharedMemory_addr,
        jit_sceKernelJitMapSharedMemory_addr,
        NEW_JIT_SOCK, NEW_MAIN_SOCK,
        staging, staging_size,
        0, o.flags,
    }) do fields[#fields + 1] = v end
    if #fields ~= 45 then fail("the boot context has " .. #fields .. " fields, not 45") end
    local ctx = malloc(0x220)
    for i = 0, 0x218, 8 do write64(ctx + i, 0) end
    for i, v in ipairs(fields) do write64(ctx + (i - 1) * 8, v) end
    -- fields, reserved[8], ext_fd/size/crc, sel_fd/size/crc, fw, ext SHA-256 pin[24]; listen mode: and
    -- after the profile slot, the console's address and the ELF loader's bundle header
    write64(ctx + 8, LISTEN and CTX_SELECT + 8 or #fields * 8 + 8 * 8 + 7 * 8 + 3 * 8)
    local app_root = malloc(#o.app_root + 1)
    write_string(app_root, o.app_root)
    write64(ctx + (#fields - 2) * 8, app_root)
    write64(ctx + #fields * 8 + 0x00, jit_sceKernelVirtualQuery_addr or 0)
    write64(ctx + #fields * 8 + 0x08, BRIDGE_ADDR or 0)
    write64(ctx + #fields * 8 + 0x10, IPU_PROG_BASE or 0)
    write64(ctx + #fields * 8 + 0x18, small_arena)
    write64(ctx + #fields * 8 + 0x20, GHOSTLDR_GPU_HEAP_MIB or 0)
    write64(ctx + #fields * 8 + 0x28, GHOSTLDR_SLACK_MIB or 0)
    write64(ctx + #fields * 8 + 0x30, GHOSTLDR_POOL1_MIB or 0)
    write64(ctx + #fields * 8 + 0x38, GHOSTLDR_CPU_HEAP_MIB or 0)
    write64(ctx + EXT, o.ext)
    write64(ctx + EXT + 0x08, o.ext_size or 0)
    write64(ctx + EXT + 0x10, o.ext_crc or 0)
    -- the selector's fields and fw end at +0x78, the ext SHA-256 pin fills
    -- +0x78..+0x90: both must stay below the profile slot (0x1F8)
    if #fields * 8 + 0x90 > 0x1F8 then fail("the boot context reaches the profile slot") end
    write64(ctx + EXT + 0x18, o.sel)
    write64(ctx + EXT + 0x20, o.sel_size or 0)
    write64(ctx + EXT + 0x28, o.sel_crc or 0)
    write64(ctx + #fields * 8 + 0x70, fw_number())   -- "12.02" -> 0x1202
    -- the extension's SHA-256 pin (first 24 bytes of the digest): ext.c checks
    -- the served blob against it after the CRC-32 screen; absent or short
    -- leaves zero bytes, which the core reads as "not pinned"
    if o.ext_sha256 and #o.ext_sha256 >= 48 then
        write_buffer(ctx + #fields * 8 + 0x78, hex_to_binary(o.ext_sha256:sub(1, 48)))
    end
    -- title profiles: blobs ended by 8 zero bytes, pointed to by the context's last
    -- u64 (profile.c PROFILE_SLOT); without them the core uses its generic defaults
    if not prof_mem and GHOSTLDR_PROFILE_B64 and #GHOSTLDR_PROFILE_B64 > 0 then
        local prof = unb64(GHOSTLDR_PROFILE_B64)
        prof_mem = malloc(#prof + 8)
        write_buffer(prof_mem, prof)
        write64(prof_mem + #prof, 0)
    end
    write64(ctx + 0x1F8, prof_mem or 0)
    if LISTEN then
        if not ip_mem then
            ip_mem, header_mem, select_mem = malloc(64), malloc(BUNDLE_HEADER), malloc(64)
            write_string(ip_mem, (ip or "this PS5"):sub(1, 47))
            -- the host game's title ID (Luac0re's TITLE_ID) at +48, for Settings > About
            write_string(ip_mem + 48, tostring(TITLE_ID or ""):sub(1, 15))
        end
        write64(ctx + CTX_IP, ip_mem)
        write64(ctx + CTX_HEADER, header_mem)
        if o.picked then
            write64(select_mem + SEL_BOOT, 1)
        else
            for i = 0, 56, 8 do write64(select_mem + i, 0) end
        end
        write64(ctx + CTX_SELECT, select_mem)
    end
    return ctx
end

local function call_core(ctx)
    place_core()
    cleanup = {}    -- from here on the core owns what it was given
    send_notification(string.format("ghostldr: core at %x, calling", rx))
    local ret = func_wrap(rx)(ctx)
    send_notification(string.format("ghostldr: core returned %x", ret))
    return ret
end

-- ---------------------------------------------------------------- listen mode: the flow
-- The core with bundle b's parts (taken: the core unmaps them; what it did
-- not take is unmapped here): its result, and the context
local function run_bundle(b, flags, app_root, picked)
    local pin = b.pin
    local ctx = boot_ctx({flags = flags | CTX_FLAG_BUNDLE, app_root = app_root, log_fd = conn, log_addr = 0, picked = picked,
        ext = b.ext, ext_size = pin.ext_size, ext_crc = pin.ext_crc, ext_sha256 = pin.ext_sha256,
        sel = b.sel, sel_size = b.sel ~= -1 and pin.sel_size or 0, sel_crc = pin.sel_crc})
    local ret = s32(call_core(ctx))
    if read64(ctx + EXT) == b.ext then unmap(b.ext, b.ext_size) end
    if b.sel ~= -1 and read64(ctx + EXT + 0x18) == b.sel then unmap(b.sel, b.sel_size) end
    b.ext, b.sel = nil, -1
    -- the log's connection now: log.c drops one that went away, the ELF loader hands over another
    conn = read64(ctx + LOG_FD)
    if conn < 0 then conn = -1 end
    return ret, ctx
end

-- nothing runs any more: say so, close the connection (so nc ends), back to Luac0re's loader
local function back(what, code, ctx)
    if ctx then write64(ctx + LOG_FD, -1) end   -- the connection is ours again
    event(string.format("%s: back to Luac0re's loader on TCP %d (the core returned %s; its log says why)",
        what, ULTRA and 8888 or 9026, code < 0 and tostring(code) or string.format("0x%x", code)),
        string.format("%s%s.\nBack to Luac0re's loader.\n\n%s", what:sub(1, 1):upper(), what:sub(2), loader_text()))
    if conn >= 0 then syscall.close(conn) end
    conn = -1
end

-- b in hand: what a failure gives back before the core has it
local function hold(b)
    cleanup = {
        function() if conn >= 0 then syscall.close(conn) end conn = -1 end,
        function() if b.ext then unmap(b.ext, b.ext_size) end end,
        function() if b.sel and b.sel ~= -1 then unmap(b.sel, b.sel_size) end end,
    }
end

-- back to the prompt: the connection the core had (the log) is ours again, and closed
local function drop(ctx)
    write64(ctx + LOG_FD, -1)
    if conn >= 0 then syscall.close(conn) end
    conn = -1
end

-- a bundle from the network into the cache (an ELF loader bundle does not
-- replace a cached ghostldr-ext.bin, whose extension has the ELF loader too)
local function store(b)
    if b.kind == 2 and cached_kind == 1 then
        say("not cached: the cache's ghostldr-ext.bin has the ELF loader too")
        return
    end
    local ok, what = cache_store(b)
    b.cached = ok       -- what is in the cache now
    if ok then
        event(string.format("cached %s in this console's saved data (%s): from now on send ghostldr.lua alone",
            b.file, what))
    else
        event(string.format("%s not cached: %s; ghostldr goes on without it", b.file, what))
    end
end

-- what the TV and the log say when games boot from the cache
local function booting_cached()
    event(string.format("booting the cached ghostldr-ext.bin (build %s): a game on a USB drive, else an installed one",
        GHOSTLDR_BUILD), "Booting: a game on a USB drive in the console,\nelse an installed one")
end

-- A bundle the ELF loader took (nb, its parts in memory): cached for the
-- next ghostldr boot, whichever build it is (the ELF loader checked its own
-- header against its parts as it came), and its memory let go; the sender,
-- a notification and the TV are told.  Refused (said) when it has this
-- build's ID with other contents.  Whether it was cached.
local function keep_for_next_boot(nb)
    if nb.bad and nb.bad ~= "build" then
        local line, tv = header_problem(nb, nb.file, "Send it again")
        event("refused: " .. line, "Refused: " .. tv)
    else
        store(nb)
    end
    unmap(nb.ext, nb.ext_size)
    if nb.sel ~= -1 then unmap(nb.sel, nb.sel_size) end
    if not nb.cached then return false end
    local other = nb.build ~= GHOSTLDR_BUILD
    event(string.format("received %s (build %s) at the ELF loader: the new ghostldr ext file will be applied on "
        .. "the next ghostldr boot%s", nb.file, nb.build, other and string.format(" (send ghostldr.lua of build %s "
        .. "with it)", nb.build) or ""), string.format("Received %s (build %s).\n\nThe new ghostldr ext file will "
        .. "be applied\non the next ghostldr boot.%s", nb.file, nb.build, other
        and string.format("\n(It needs ghostldr.lua of build %s.)", nb.build) or ""))
    return true
end

-- the bundle the ELF loader took (its header copied, its parts in the context), or nil
local function taken(ctx)
    if read64(ctx + EXT) == -1 then return nil end
    local nb = check_header(read_buffer(header_mem, BUNDLE_HEADER))
    nb.ext, nb.sel = read64(ctx + EXT), read64(ctx + EXT + 0x18)
    return nb
end

-- the TV and the log when the game selector opens with the ELF loader beside it
local function selecting(what)
    event(string.format("%s (build %s): the game selector, with the ELF loader on TCP %d beside it until a game "
        .. "is picked", what, GHOSTLDR_BUILD, port), string.format("Opening the game selector.\nELF loader on %s:%d "
        .. "until a game is picked.", ip or "this console", port))
end

local function listen_main()
    scratch = malloc(0x10000)
    ip = get_current_ip and get_current_ip() or nil
    if CLEAR then
        local rc = cache_delete()
        event(rc == 0 and "the bundle cache is cleared" or rc == s32(SAVE_NOT_FOUND) and "no bundle cache to clear"
            or string.format("the bundle cache was not cleared (%x)", rc & 0xFFFFFFFF))
    end
    local note
    local b, problem, detail = cache_read(2)
    local flags0 = base_flags
    base_flags = conf_flags(flags0, true)      -- Settings > Advanced, for both calls
    if b and b.kind == 2 and not HAS_ELFLDR then   -- a PS5's: no ELF loader here
        unmap(b.ext, b.ext_size)
        b = nil
    end
    if problem == "mismatch" then
        local line, tv = header_problem(detail, "the cached " .. detail.file,
            "The bundle sent next replaces it.")
        event(line .. " (the bundle sent next replaces it)")
        note = tv
    elseif problem then
        event(string.format("the cached bundle is damaged (%s): it is deleted; send the bundle again", detail))
        note = "The cached bundle is damaged: deleted.\nSend the bundle again."
    end
    -- a cached ghostldr-ext.bin: the game selector at once, with the ELF loader beside it on a PS5
    local mode = not b and "prompt" or b.kind == 2 and "elfldr" or HAS_ELFLDR and "select" or "boot"
    local picked    -- the second call boots the game picked beside the ELF loader
    if mode ~= "prompt" then
        hold(b)
        if mode ~= "boot" then  -- the ELF loader's port: the bundle port, given back whole (both processes' copies)
            syscall.close(bundle_listener())
        end
        if mode == "boot" then
            booting_cached()
        elseif mode == "select" then
            selecting("the cached ghostldr-ext.bin")
        else
            event(string.format("the cached %s (build %s): the ELF loader on TCP %d; a ghostldr-ext.bin sent "
                .. "there is kept for the next boot", b.file, GHOSTLDR_BUILD, port))
        end
    end
    while true do
        if mode == "prompt" then
            local r = receive_bundle(nil, note)
            note = nil
            if r == nil then return end
            b = r
            hold(b)
            say(string.format("received %s (%d bytes, build %s) from %s on TCP %d", b.file,
                BUNDLE_HEADER + b.ext_size + b.sel_size, GHOSTLDR_BUILD, b.from, port))
            store(b)
            -- the second call boots from the cache: a bundle that could not be cached boots as it is
            mode = b.kind == 2 and "elfldr" or (HAS_ELFLDR and b.cached) and "select" or "boot"
            if mode == "select" then
                selecting("received ghostldr-ext.bin")
            elseif mode == "boot" then
                event("booting: the games on a USB drive in the console, else the installed ones; the log "
                    .. "follows here (closing this connection stops the log, not the game)",
                    "Received ghostldr-ext.bin.\n\nBooting: a game on a USB drive in the console,\nelse an "
                    .. "installed one")
            else
                event(string.format("starting the ELF loader on TCP %d: send payloads with nc %s %d < "
                    .. "payload.elf; send \"quit\" there to give SWRR back to Luac0re's loader", port,
                    ip or "<PS5>", port), string.format("Received ghostldr-elfldr.bin.\n\nELF loader: send "
                    .. "payloads to %s:%d\n(nc %s %d < payload.elf)\nSend \"quit\" there to go back to Luac0re",
                    ip or "this console", port, ip or "PS5", port))
            end
        elseif mode == "select" then
            -- the game selector with the ELF loader beside it on the bundle port (boot.c elfldr_select):
            -- once a game is picked the ELF loader stops, after any payload still running, and the
            -- extension goes with its executable memory; a second call boots the pick
            local flags = CTX_FLAG_ELFLDR | CTX_ELFLDR_SELECT | (port << 32) | CTX_FLAG_SELECT
                | (base_flags & 0xFFFFFFFF)
            local was_cached, sent = b.cached, b.from ~= nil
            local code, ctx = run_bundle(b, flags, GHOSTLDR_APP_ROOT)
            if code == EL_PICKED then
                local ob = cache_read(2)    -- what boots: the cache as it is before a bundle sent meanwhile
                local nb = taken(ctx)
                if nb then keep_for_next_boot(nb) end   -- its connection stays the log, into the game
                local title = read_buffer(select_mem + SEL_TITLE, 16):gsub("%z.*", "")
                picked = s32(read32(select_mem + SEL_INDEX)) >= 0 and title ~= ""
                if not ob then
                    return back("the game selector closed, but nothing of this build is cached to boot", code, ctx)
                end
                ob.from = b.from    -- sent this session, as before the cache: a failure then goes back
                b = ob
                hold(b)
                base_flags = conf_flags(flags0, false)   -- as the selector left Settings > Advanced
                mode = "boot"
                event(picked and string.format("%s picked: the ELF loader has stopped; booting it", title)
                    or "nothing picked in the game selector: booting as without it",
                    picked and string.format("Starting %s.", title) or nil)
            elseif code == -7 and was_cached and not sent then
                cache_delete()
                event("the cached bundle is damaged (the core's checksum of its extension failed): it is deleted")
                drop(ctx)
                note, mode = "The cached bundle is damaged: deleted.\nSend the bundle again.", "prompt"
            else
                if code == -7 and was_cached then cache_delete() end
                return back(code == -20 and "closed in Settings" or "the game selector stopped", code, ctx)
            end
        elseif mode == "elfldr" then
            -- as run.py elfldr: no title, the ELF loader on the bundle port; with this build's
            -- ghostldr-ext.bin cached (or here) "boot" opens the game selector
            local can_boot = b.kind == 1 or (cached_kind == 1 and cached_build == GHOSTLDR_BUILD)
            local flags = CTX_FLAG_ELFLDR | (port << 32) | (can_boot and CTX_ELFLDR_CAN_BOOT or 0)
            -- damaged (-7): a bundle from the cache goes, and the prompt asks for another; one just
            -- sent goes from the cache too, and SWRR back to Luac0re's loader as before
            local was_cached, sent = b.cached, b.from ~= nil
            local code, ctx = run_bundle(b, flags, "/app0")
            if code == EL_BOOT then
                b, problem, detail = cache_read(2)
                if b then
                    hold(b)
                    mode = "select"
                    selecting("the cached ghostldr-ext.bin")
                else
                    event(string.format("the cached bundle is damaged (%s): it is deleted", detail or "gone"))
                    drop(ctx)
                    note, mode = "The cached bundle is damaged: deleted.\nSend the bundle again.", "prompt"
                end
            elseif code == EL_BUNDLE then
                write64(ctx + LOG_FD, -1)   -- its connection is ours again
                -- the loader carries on with this build's bundle as cached now, read before the new one
                local ob = cache_read(1)
                local nb = taken(ctx)
                if keep_for_next_boot(nb) and not ob and nb.build == GHOSTLDR_BUILD then ob = cache_read(1) end
                if conn >= 0 then syscall.close(conn) end   -- the sender's nc ends; the loader carries on
                conn = -1
                b = ob
                if not b then
                    return back("the ELF loader stopped (nothing of this build cached to restart it with)", code)
                end
                hold(b)
            elseif code == EL_FORGET then
                local rc = cache_delete()
                event(rc == 0 and "the bundle cache is cleared: send a bundle to this port"
                    or string.format("the bundle cache was not cleared (%x)", rc & 0xFFFFFFFF))
                drop(ctx)
                note, mode = "The bundle cache is cleared.", "prompt"
            elseif code == -7 and was_cached and not sent then
                cache_delete()
                event("the cached bundle is damaged (the core's checksum of its extension failed): it is deleted")
                drop(ctx)
                note, mode = "The cached bundle is damaged: deleted.\nSend the bundle again.", "prompt"
            else
                if code == -7 and was_cached then cache_delete() end
                return back(code == 0 and "the ELF loader quit" or "the ELF loader stopped", code, ctx)
            end
        else    -- boot: the pick from beside the ELF loader, else the selector (even for one game)
            local was_cached, sent = b.cached, b.from ~= nil
            local code, ctx = run_bundle(b, base_flags | (picked and 0 or CTX_FLAG_SELECT), GHOSTLDR_APP_ROOT,
                picked)
            if code == 0 then
                if conn >= 0 then say("the game runs; its log keeps coming here") end
                return
            end
            if code == -7 and was_cached then cache_delete() end   -- the core found the extension damaged
            if code == -7 and was_cached and not sent then
                event("the cached bundle is damaged (the core's checksum of its extension failed): it is deleted")
                drop(ctx)
                note, mode, picked = "The cached bundle is damaged: deleted.\nSend the bundle again.", "prompt", nil
            else
                return back(code == -20 and "closed in Settings" or "the boot stopped", code, ctx)
            end
        end
    end
end

-- ---------------------------------------------------------------- run
if MODE == "clear-cache" then
    local rc = cache_delete()
    local msg = rc == 0 and "the bundle cache is cleared: the next ghostldr.lua asks for a bundle"
        or rc == s32(SAVE_NOT_FOUND) and "there is no bundle cache to clear"
        or string.format("the bundle cache was not cleared (%x)", rc & 0xFFFFFFFF)
    send_notification("ghostldr: " .. msg)
    if show_dialog then pcall(show_dialog, "ghostldr\n\n" .. msg:sub(1, 1):upper() .. msg:sub(2) .. ".\n\n"
        .. loader_text()) end
    return
end
if LISTEN then return listen_main() end

load_core()
-- connect (run.py): the log is a UDP socket to the computer (Luac0re routes
-- AF_INET creation through the compiler); the extension and the selector
-- come from its TCP servers (sockets created through the compiler too)
local log_fd = create_socket(AF_INET, SOCK_DGRAM, 0)
local log_addr = malloc(16)
for i = 0, 15 do write8(log_addr + i, 0) end
write8(log_addr + 1, AF_INET)
write16(log_addr + 2, ((GHOSTLDR_LOG_PORT << 8) | (GHOSTLDR_LOG_PORT >> 8)) & 0xFFFF)
write32(log_addr + 4, u32le_ip(GHOSTLDR_LOG_IP))
local function blob_socket(p, size, what)
    if (size or 0) <= 0 then return -1 end
    local sa, tv = malloc(16), malloc(16)
    for i = 0, 15 do write8(sa + i, 0) end
    write8(sa + 1, AF_INET)
    write16(sa + 2, ((p << 8) | (p >> 8)) & 0xFFFF)
    write32(sa + 4, u32le_ip(GHOSTLDR_LOG_IP))
    write64(tv, 15)     -- SO_RCVTIMEO: a stalled transfer fails instead of hanging the core
    write64(tv + 8, 0)
    local fd = create_socket(AF_INET, SOCK_STREAM, 0)
    if fd >= 0 then
        syscall.setsockopt(fd, SOL_SOCKET, 0x1006, tv, 16)
        local rc = syscall.connect(fd, sa, 16)
        if rc ~= 0 then
            send_notification(string.format("ghostldr: %s server %s:%d: connect failed (%s)",
                what, GHOSTLDR_LOG_IP, p, get_error_string()))
            syscall.close(fd)
            fd = -1
        end
    end
    return fd
end
local ctx = boot_ctx({flags = base_flags, app_root = GHOSTLDR_APP_ROOT, log_fd = log_fd, log_addr = log_addr,
    ext = blob_socket(GHOSTLDR_EXT_PORT, GHOSTLDR_EXT_SIZE, "extension"), ext_size = GHOSTLDR_EXT_SIZE,
    ext_crc = GHOSTLDR_EXT_CRC, ext_sha256 = GHOSTLDR_EXT_SHA256,
    sel = blob_socket(GHOSTLDR_SEL_PORT, GHOSTLDR_SEL_SIZE, "selector"), sel_size = GHOSTLDR_SEL_SIZE,
    sel_crc = GHOSTLDR_SEL_CRC})
call_core(ctx)
