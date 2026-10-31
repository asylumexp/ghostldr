-- ghostldr's autoloader for Luac0re 2.4b (SWRR US CUSA03474 or EU CUSA03492; docs/autoload.md).
--
-- Luac0re's main.lua runs /savedata0/lua/auto.lua, when its save has one, in
-- place of its loader on TCP 9026.  This one runs the release's ghostldr.lua
-- from the save's ghostldr folder, with GHOSTLDR_BUNDLE_FILE naming the
-- ghostldr-ext.bin beside it: with nothing of that build cached yet,
-- ghostldr.lua takes the bundle from there and caches it, so OPTIONS > HALL
-- OF FAME opens the game selector with nothing sent from a computer.  A
-- bundle sent to the ELF loader, or a cached one of this build, wins as
-- without the autoloader.
--
-- Hold L1+R1 as the Hall of Fame opens to skip it: Luac0re's loader on 9026,
-- as with no auto.lua.  However ghostldr ends (Close ghostldr in Settings, a
-- failure, a missing file), that loader takes scripts after it.
--
-- tools/make_save.py puts it in a save, as lua/auto.lua, with the two files
-- in ghostldr/.

local DIR = "/savedata0/ghostldr/"
local LUA, BUNDLE = DIR .. "ghostldr.lua", DIR .. "ghostldr-ext.bin"
local SKIP = 0x400 | 0x800      -- L1 + R1 in ScePadData's buttons
local PORT = 9026

local function s32(v) v = v & 0xFFFFFFFF return v >= 0x80000000 and v - 0x100000000 or v end

-- the buttons held on the first user's controller now, or 0 (no pad library, no handle)
local function buttons()
    local function sym(module, name)
        local load = func_wrap(dlsym(LIBKERNEL_HANDLE, "sceKernelLoadStartModule"))
        local p = malloc(64)
        write_string(p, module)
        local h = s32(load(p, 0, 0, 0, 0, 0))
        if h <= 0 then return nil end
        local addr = dlsym(h, name)
        return addr ~= 0 and func_wrap(addr) or nil
    end
    local get_user, get_handle = sym("libSceUserService.sprx", "sceUserServiceGetInitialUser"),
        sym("libScePad.sprx", "scePadGetHandle")
    local read_state = sym("libScePad.sprx", "scePadReadState")
    if not (get_user and get_handle and read_state) then return 0 end
    local uid, data = malloc(8), malloc(0x100)
    write32(uid, 0)
    if s32(get_user(uid)) ~= 0 then return 0 end
    local h = s32(get_handle(read32(uid), 0, 0))
    for i = 0, 0xF8, 8 do write64(data + i, 0) end
    if h < 0 or s32(read_state(h, data)) ~= 0 then return 0 end
    return read32(data)
end

local function loader(why)
    send_notification("ghostldr autoloader: " .. why .. ": Luac0re's loader on TCP " .. PORT)
    return remote_lua_loader(PORT)
end

local ok, held = pcall(buttons)
if ok and held & SKIP == SKIP then return loader("L1+R1 held") end
if not file_exists(LUA) then return loader(LUA .. " is not in the save") end

local main, err = loadfile(LUA)
if not main then return loader("ghostldr.lua does not load (" .. tostring(err) .. ")") end
GHOSTLDR_BUNDLE_FILE = BUNDLE
local ran, e = xpcall(main, debug.traceback)
GHOSTLDR_BUNDLE_FILE = nil
-- ghostldr says its own failures on the TV ("ghostldr: ..."); anything else is shown here
if not ran and not tostring(e):find("ghostldr: ", 1, true) then show_dialog("ghostldr autoloader:\n" .. tostring(e)) end
return loader(ran and "ghostldr returned" or "ghostldr stopped")
