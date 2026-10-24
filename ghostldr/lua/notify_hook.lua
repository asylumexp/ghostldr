-- Mirror the loader's notifications and dialogs to run.py's log (UDP), so a
-- payload that fails to load or run says why in build/ghostldr/run.log, not
-- only in a notification on the TV.  run.py sends it ahead of each payload to
-- UltraC0re's loader (Jak X), prepending GHOSTLDR_LOG_IP / GHOSTLDR_LOG_PORT.
-- Lua globals outlive a payload: the first run wraps the functions, later
-- runs only point them at the current address.
NOTIFY_HOOK_ADDR = NOTIFY_HOOK_ADDR or malloc(16)
if not NOTIFY_HOOK_FD then NOTIFY_HOOK_FD = create_socket(AF_INET, SOCK_DGRAM, 0) end
local a = NOTIFY_HOOK_ADDR
for i = 0, 15 do write8(a + i, 0) end
write8(a + 1, AF_INET)
write16(a + 2, ((GHOSTLDR_LOG_PORT << 8) | (GHOSTLDR_LOG_PORT >> 8)) & 0xFFFF)
local o = {}
for x in GHOSTLDR_LOG_IP:gmatch("%d+") do o[#o + 1] = tonumber(x) end
write32(a + 4, (o[4] << 24) | (o[3] << 16) | (o[2] << 8) | o[1])

local function say(prefix, text)
    for line in (tostring(text) .. "\n"):gmatch("([^\n]*)\n") do
        if #line > 0 then
            local t = "lua: " .. prefix .. line
            local b = malloc(#t + 1)
            write_buffer(b, t)
            syscall.sendto(NOTIFY_HOOK_FD, b, #t, 0, NOTIFY_HOOK_ADDR, 16)
        end
    end
end

if not NOTIFY_HOOK_ORIG then
    NOTIFY_HOOK_ORIG = {send_notification = send_notification, show_dialog = show_dialog}
    function send_notification(text)
        say("", text)
        return NOTIFY_HOOK_ORIG.send_notification(text)
    end
    function show_dialog(text)
        say("dialog: ", text)
        return NOTIFY_HOOK_ORIG.show_dialog(text)
    end
end
