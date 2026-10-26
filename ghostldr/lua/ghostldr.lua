-- ghostldr bootstrap for Luac0re 2.4b (SWRR US CUSA03474 or EU CUSA03492, after jit_init())
-- or UltraC0re (Jak X EU CUSA07992 1.04, after its jit_init() and jit_compat_init()).
--
-- Two ways in, by GHOSTLDR_MODE:
--   "connect"  run.py prepends the values below: the extension and the game
--              selector come from TCP servers on the computer, the log goes
--              to it over UDP.
--   "listen"   the release's ghostldr.lua (tools/mkbundle.py): the core, the
--              title profiles, defaults and the pins of the two bundles are in
--              it.  It opens TCP 9021 (9022 when that is taken), waits up to 5
--              minutes for ghostldr-ext.bin (boot a game) or
--              ghostldr-elfldr.bin (the ELF loader), checks it against the
--              pins, and the log goes back on that connection
--              (docs/architecture.md, "Two-file boot").  Nothing of SWRR's is
--              touched before a bundle is taken.  The prompt and every
--              outcome show on the TV, in Luac0re's message dialog, as well
--              as in a notification and on the bundle's connection.
--
-- The host tool prepends:
--   GHOSTLDR_CORE_HEX   hex of build/ghostldr/core.bin
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
--   GHOSTLDR_PROFILE_HEX  the title profiles (host/profiles.py boot_bundle); the
--                       core finds them through the last u64 of the context
--   GHOSTLDR_BUILD      (listen) the build's ID, 16 hex digits, also in each
--                       bundle's header
--   GHOSTLDR_BUNDLES    (listen) per bundle kind (1 full, 2 ELF loader): its
--                       file name and its parts' sizes, CRC-32s and SHA-256s

local LISTEN = GHOSTLDR_MODE == "listen"
GHOSTLDR_MODE = nil     -- Luac0re keeps one Lua state: the next script sets its own

-- listen mode: the bundle's connection.  Lines to it start "ghostldr: ";
-- from the core on, the core's log follows on it.
local conn = -1
local line_buf
local function say(msg)
    if conn < 0 then return end
    local s = "ghostldr: " .. msg
    s = (#s > 1000 and s:sub(1, 1000) or s) .. "\n"
    line_buf = line_buf or malloc(1024)
    write_buffer(line_buf, s)
    syscall.write(conn, line_buf, #s)
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

-- UltraC0re parks the compiler on its own loop (door1.lua) and keeps
-- Luac0re's jit_* names for the rest (jitcompat.lua)
local ULTRA = J_COMP ~= nil and J_SPIN ~= nil
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

-- ---------------------------------------------------------------- listen mode: the bundle
-- Its header (tools/mkbundle.py), 128 bytes, little-endian: "GLDRBNDL",
-- format 1, header size, kind (1 ghostldr-ext.bin: ext.bin and selector.bin;
-- 2 ghostldr-elfldr.bin: ext-elfldr.bin alone), flags, the extension's and
-- the selector's sizes, CRC-32s and SHA-256s, the build ID; then the
-- extension, then the selector.
local BUNDLE_MAGIC, BUNDLE_HEADER = "GLDRBNDL", 128
local BUNDLE_PORTS = {9021, 9022}   -- 9021: the scene's ELF loader port, free on a stock console
local BUNDLE_WAIT_S = 300
local KIND_FILE = {[1] = "ghostldr-ext.bin", [2] = "ghostldr-elfldr.bin"}
local CTX_FLAG_ELFLDR, CTX_FLAG_BUNDLE = 1 << 20, 1 << 21
local SO_REUSEADDR_, SO_NOSIGPIPE, SO_RCVBUF, SO_SNDTIMEO, SO_RCVTIMEO = 0x4, 0x800, 0x1002, 0x1005, 0x1006

local function s32(v) v = v & 0xFFFFFFFF return v >= 0x80000000 and v - 0x100000000 or v end
local function hex(s) return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end)) end
local function why() return (get_error_string():gsub("%s*\n%s*", ", ")) end
local function pages(n) return (n + 0x3FFF) & ~0x3FFF end

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

local scratch
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

-- the connection's bundle, or nil (refused, said and closed)
local function take(port, from)
    setopt(conn, SO_NOSIGPIPE, 1)      -- a sender that goes away must not take SWRR with it
    setopt(conn, SO_RCVTIMEO, 30, true)
    setopt(conn, SO_SNDTIMEO, 5, true) -- a sender that stops reading ends the log (log.c), not the game
    local ok, got, r = recv_into(scratch, BUNDLE_HEADER)
    local h = read_buffer(scratch, got)
    if h ~= BUNDLE_MAGIC:sub(1, #h) and h:sub(1, 8) ~= BUNDLE_MAGIC then
        local what = h:sub(1, 4) == "\x7fELF" and "an ELF" or h:sub(1, 4) == "\x4f\x15\x3d\x1d" and "a SELF"
            or h:find("^%s*%-%-") and "a Lua script" or "not a ghostldr bundle"
        local hint = (what == "an ELF" or what == "a SELF")
            and ": ELF payloads go to the ELF loader, which ghostldr-elfldr.bin starts (on the port it then names)"
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
    local magic, format, hsize, kind, _, ext_size, sel_size, ext_crc, sel_crc, ext_sha, sel_sha, build =
        string.unpack("<c8I4I4I4I4I8I8I4I4c32c32c8", h)
    local file = KIND_FILE[kind] or string.format("a bundle of kind %d", kind)
    if format ~= 1 or hsize ~= BUNDLE_HEADER then
        refuse(string.format("%s has bundle format %d; this ghostldr.lua (build %s) reads format 1: send "
            .. "ghostldr.lua and the bundle from the same release", file, format, GHOSTLDR_BUILD),
            string.format("%s has bundle format %d, expected 1.\nSend ghostldr.lua and the bundle from the same "
            .. "release", file, format))
        return nil
    end
    local pin = pin_of(kind)
    if not pin or hex(build) ~= GHOSTLDR_BUILD or ext_size ~= pin.ext_size or sel_size ~= pin.sel_size
            or ext_crc ~= pin.ext_crc or sel_crc ~= pin.sel_crc or hex(ext_sha) ~= pin.ext_sha256
            or hex(sel_sha) ~= pin.sel_sha256 then
        refuse(string.format("%s is from another build (%s) than this ghostldr.lua (%s): send ghostldr.lua "
            .. "and %s from the same release", file, hex(build), GHOSTLDR_BUILD, KIND_FILE[kind] or "the bundle"),
            string.format("%s is from build %s,\nthis ghostldr.lua expects build %s.\nSend both from the same "
            .. "release", file, hex(build), GHOSTLDR_BUILD))
        return nil
    end
    if kind == 2 and (PLATFORM == "PS4" or ULTRA) then
        refuse("the ELF loader runs on a PS5, in SWRR with Luac0re: send ghostldr-ext.bin to boot a game",
            "ghostldr-elfldr.bin: the ELF loader runs on a PS5 only.\nSend ghostldr-ext.bin to boot a game")
        return nil
    end
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
        refuse(string.format("%d of %s's %d bytes came (%s)", BUNDLE_HEADER + got, file,
            BUNDLE_HEADER + ext_size + sel_size, r == 0 and "the connection ended" or "nothing more for 30 seconds"))
        return nil
    end
    return {kind = kind, file = file, port = port, from = from, pin = pin,
            ext = ext, ext_size = ext_size, sel = sel or -1, sel_size = sel_size}
end

-- Listen on 9021, else 9022, until a bundle is taken or BUNDLE_WAIT_S pass
-- (nil: nothing taken, nothing left open)
local function receive_bundle()
    local lfd, port, taken = -1, nil, {}
    for _, p in ipairs(BUNDLE_PORTS) do
        local e
        lfd, e = listen_on(p)
        if lfd >= 0 then
            port = p
            break
        end
        taken[#taken + 1] = string.format("%d (%s)", p, e)
    end
    if not port then fail("no port for the bundle: TCP " .. table.concat(taken, ", ")) end
    scratch = scratch or malloc(0x10000)
    local ip = get_current_ip and get_current_ip() or nil
    local taken_note = port ~= BUNDLE_PORTS[1] and string.format(" (%d is taken)", BUNDLE_PORTS[1]) or ""
    waiting = string.format("Send ghostldr-ext.bin (games)\nor ghostldr-elfldr.bin (ELF loader)\nto %s:%d%s\n\n"
        .. "Waiting %d minutes", ip or "this console", port, taken_note, BUNDLE_WAIT_S // 60)
    send_notification(string.format("ghostldr %s: send ghostldr-ext.bin (games) or ghostldr-elfldr.bin (ELF "
        .. "loader) to %s:%d%s in the next %d minutes", GHOSTLDR_BUILD, ip or "this console", port, taken_note,
        BUNDLE_WAIT_S // 60))
    screen(waiting)
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
                local b = take(port, from)
                if b then
                    syscall.close(lfd)
                    b.ip = ip
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

local bundle, elf_port
if LISTEN then
    bundle = receive_bundle()
    if not bundle then return end
    cleanup[#cleanup + 1] = function() syscall.close(conn) conn = -1 end
    cleanup[#cleanup + 1] = function() unmap(bundle.ext, bundle.ext_size) end
    if bundle.sel_size > 0 then cleanup[#cleanup + 1] = function() unmap(bundle.sel, bundle.sel_size) end end
    local got_line = string.format("received %s (%d bytes, build %s) from %s on TCP %d", bundle.file,
        BUNDLE_HEADER + bundle.ext_size + bundle.sel_size, GHOSTLDR_BUILD, bundle.from, bundle.port)
    say(got_line)
    if bundle.kind == 2 then
        -- the ELF loader's port: the next free one after the bundle's
        for p = bundle.port + 1, bundle.port + 8 do
            local fd = listen_on(p)
            if fd >= 0 then
                syscall.close(fd)
                elf_port = p
                break
            end
        end
        if not elf_port then fail(string.format("no free port for the ELF loader after %d", bundle.port)) end
        event(string.format("starting the ELF loader on TCP %d: send payloads with nc %s %d < payload.elf; "
            .. "send \"quit\" there to give SWRR back to Luac0re's loader", elf_port, bundle.ip or "<PS5>", elf_port),
            string.format("Received ghostldr-elfldr.bin.\n\nELF loader: send payloads to %s:%d\n"
            .. "(nc %s %d < payload.elf)\nSend \"quit\" there to go back to Luac0re", bundle.ip or "this console",
            elf_port, bundle.ip or "PS5", elf_port))
    else
        event("booting: the games on a USB drive in the PS5, else the installed ones; the log follows here "
            .. "(closing this connection stops the log, not the game)",
            "Received ghostldr-ext.bin.\n\nBooting: a game on a USB drive in the PS5,\nelse an installed one")
    end
end

local core = hex_to_binary(GHOSTLDR_CORE_HEX)
local text_size = GHOSTLDR_TEXT_SIZE
local image_size = (GHOSTLDR_IMAGE_SIZE + 0x3FFF) & ~0x3FFF
local data_size = image_size - text_size

-- 1. the core lives at the start of the emulator's IPU JIT program buffer
-- (RX here, RW in the compiler at the same address): the compiler process
-- can create almost no new JIT memory, so reuse what the emulator owns.
local out = malloc(0x40)
local rx = IPU_PROG_BASE + 0x4000
local small_arena = rx + image_size

-- 2. code through the compiler (same address in both processes)
write_buffer(SHELLCODE_SCRATCH, core:sub(1, text_size))
jit_memcpy(rx, SHELLCODE_SCRATCH, text_size)

-- 3. the core's data pages: replace that part of the RX buffer with RW memory
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

-- 5. the log: the bundle's connection (listen; log.c), or a UDP socket to
-- the computer (Luac0re routes AF_INET creation through the compiler)
local log_fd, log_addr
if LISTEN then
    log_fd, log_addr = conn, 0
else
    log_fd = create_socket(AF_INET, SOCK_DGRAM, 0)
    log_addr = malloc(16)
    for i = 0, 15 do write8(log_addr + i, 0) end
    write8(log_addr + 1, AF_INET)
    write16(log_addr + 2, ((GHOSTLDR_LOG_PORT << 8) | (GHOSTLDR_LOG_PORT >> 8)) & 0xFFFF)
    write32(log_addr + 4, u32le_ip(GHOSTLDR_LOG_IP))
end

-- 6. extension and selector sockets (created through the compiler like the log socket)
local function blob_socket(port, size, what)
    if (size or 0) <= 0 then return -1 end
    local sa, tv = malloc(16), malloc(16)
    for i = 0, 15 do write8(sa + i, 0) end
    write8(sa + 1, AF_INET)
    write16(sa + 2, ((port << 8) | (port >> 8)) & 0xFFFF)
    write32(sa + 4, u32le_ip(GHOSTLDR_LOG_IP))
    write64(tv, 15)     -- SO_RCVTIMEO: a stalled transfer fails instead of hanging the core
    write64(tv + 8, 0)
    local fd = create_socket(AF_INET, SOCK_STREAM, 0)
    if fd >= 0 then
        syscall.setsockopt(fd, SOL_SOCKET, 0x1006, tv, 16)
        local rc = syscall.connect(fd, sa, 16)
        if rc ~= 0 then
            send_notification(string.format("ghostldr: %s server %s:%d: connect failed (%s)",
                what, GHOSTLDR_LOG_IP, port, get_error_string()))
            syscall.close(fd)
            fd = -1
        end
    end
    return fd
end
-- listen: the bundle's parts in memory (CTX_FLAG_BUNDLE), checked against its pins
local ext_fd, ext_size, ext_crc, ext_sha256, sel_fd, sel_size, sel_crc
if LISTEN then
    local pin = bundle.pin
    ext_fd, ext_size, ext_crc, ext_sha256 = bundle.ext, pin.ext_size, pin.ext_crc, pin.ext_sha256
    sel_fd, sel_size, sel_crc = bundle.sel, pin.sel_size, pin.sel_crc
else
    ext_fd = blob_socket(GHOSTLDR_EXT_PORT, GHOSTLDR_EXT_SIZE, "extension")
    sel_fd = blob_socket(GHOSTLDR_SEL_PORT, GHOSTLDR_SEL_SIZE, "selector")
    ext_size, ext_crc, ext_sha256 = GHOSTLDR_EXT_SIZE, GHOSTLDR_EXT_CRC, GHOSTLDR_EXT_SHA256
    sel_size, sel_crc = GHOSTLDR_SEL_SIZE, GHOSTLDR_SEL_CRC
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
local flags = GHOSTLDR_FLAGS | (PLATFORM == "PS4" and 8192 or 0) | (ULTRA and 32768 or 0)
local app_root_path = GHOSTLDR_APP_ROOT
if LISTEN then
    flags = flags | CTX_FLAG_BUNDLE
    if bundle.kind == 2 then  -- as run.py elfldr: no title, the ELF loader on elf_port
        flags = CTX_FLAG_ELFLDR | (elf_port << 32) | CTX_FLAG_BUNDLE
        app_root_path = "/app0"
    end
end
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
local fields = {
    0x3130305244484c47, 0, EBOOT_BASE, core_dlsym,
    EBOOT_BASE + (ULTRA and 0x19869 or 0x31aa9),    -- the eboot's "call rbx; ret"
    LIBKERNEL_HANDLE, compiler_stubs(), log_fd, log_addr,
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
    0, flags,
}) do fields[#fields + 1] = v end
if #fields ~= 45 then fail("the boot context has " .. #fields .. " fields, not 45") end
local LOG_FD = 7 * 8     -- fields[8]: log_fd
local ctx = malloc(0x200)
for i = 0, 0x1F8, 8 do write64(ctx + i, 0) end
for i, v in ipairs(fields) do write64(ctx + (i - 1) * 8, v) end
write64(ctx + 8, #fields * 8 + 8 * 8 + 7 * 8 + 3 * 8)    -- fields, reserved[8], ext_fd/size/crc, sel_fd/size/crc, fw, ext SHA-256 pin[24]
local app_root = malloc(#app_root_path + 1)
write_string(app_root, app_root_path)
write64(ctx + (#fields - 2) * 8, app_root)
local EXT = #fields * 8 + 0x40    -- ext_fd, then size, crc; sel_fd at +0x18
write64(ctx + #fields * 8 + 0x00, jit_sceKernelVirtualQuery_addr or 0)
write64(ctx + #fields * 8 + 0x08, BRIDGE_ADDR or 0)
write64(ctx + #fields * 8 + 0x10, IPU_PROG_BASE or 0)
write64(ctx + #fields * 8 + 0x18, small_arena)
write64(ctx + #fields * 8 + 0x20, GHOSTLDR_GPU_HEAP_MIB or 0)
write64(ctx + #fields * 8 + 0x28, GHOSTLDR_SLACK_MIB or 0)
write64(ctx + #fields * 8 + 0x30, GHOSTLDR_POOL1_MIB or 0)
write64(ctx + #fields * 8 + 0x38, GHOSTLDR_CPU_HEAP_MIB or 0)
write64(ctx + EXT, ext_fd)
write64(ctx + EXT + 0x08, ext_size or 0)
write64(ctx + EXT + 0x10, ext_crc or 0)
-- the selector's fields and fw end at +0x78, the ext SHA-256 pin fills
-- +0x78..+0x90: both must stay below the profile slot (0x1F8)
if #fields * 8 + 0x90 > 0x1F8 then fail("the boot context reaches the profile slot") end
write64(ctx + EXT + 0x18, sel_fd)
write64(ctx + EXT + 0x20, sel_size or 0)
write64(ctx + EXT + 0x28, sel_crc or 0)
write64(ctx + #fields * 8 + 0x70, fw_number())   -- "12.02" -> 0x1202
-- the extension's SHA-256 pin (first 24 bytes of the digest): ext.c checks
-- the served blob against it after the CRC-32 screen; absent or short
-- leaves zero bytes, which the core reads as "not pinned"
if ext_sha256 and #ext_sha256 >= 48 then
    write_buffer(ctx + #fields * 8 + 0x78, hex_to_binary(ext_sha256:sub(1, 48)))
end
-- title profiles: blobs ended by 8 zero bytes, pointed to by the context's last
-- u64 (profile.c PROFILE_SLOT); without them the core uses its generic defaults
if GHOSTLDR_PROFILE_HEX and #GHOSTLDR_PROFILE_HEX > 0 then
    local prof = hex_to_binary(GHOSTLDR_PROFILE_HEX)
    local prof_mem = malloc(#prof + 8)
    write_buffer(prof_mem, prof)
    write64(prof_mem + #prof, 0)
    write64(ctx + 0x1F8, prof_mem)
end

cleanup = {}    -- from here on the core owns the bundle's memory and its connection
send_notification(string.format("ghostldr: core at %x, calling", rx))
local ret = func_wrap(rx)(ctx)
send_notification(string.format("ghostldr: core returned %x", ret))

if LISTEN then
    -- what the core did not take goes back (a boot that stopped before it)
    if read64(ctx + EXT) == bundle.ext then unmap(bundle.ext, bundle.ext_size) end
    if bundle.sel_size > 0 and read64(ctx + EXT + 0x18) == bundle.sel then unmap(bundle.sel, bundle.sel_size) end
    -- the connection, unless the core gave it up (the sender went away: log.c)
    local open = read64(ctx + LOG_FD) == conn
    if bundle.kind == 2 or ret ~= 0 then
        -- nothing runs: the connection closes, so nc ends
        write64(ctx + LOG_FD, -1)
        local code = s32(ret)
        local why = bundle.kind == 2 and (code == 0 and "the ELF loader quit" or "the ELF loader stopped")
            or "the boot stopped"
        if not open then conn = -1 end
        event(string.format("%s: back to Luac0re's loader on TCP %d (the core returned %s; its log says why)",
            why, ULTRA and 8888 or 9026, code < 0 and tostring(code) or string.format("0x%x", code)),
            string.format("%s%s.\nBack to Luac0re's loader.\n\n%s", why:sub(1, 1):upper(), why:sub(2),
            loader_text()))
        if open then syscall.close(conn) end
        conn = -1
    elseif open then
        say("the game runs; its log keeps coming here")
    end
end
