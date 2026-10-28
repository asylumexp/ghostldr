-- ghostldr manager: a small web server inside SWRR, started through
-- Luac0re's Remote Lua Loader (like gfs_receiver.lua).  A browser on the LAN
-- lists the installed titles and their save containers, downloads saves as
-- tar archives, restores them, copies and deletes them, and stops the server.
--
-- ghostldr/host/manager.py prepends the settings (all optional):
--   MANAGER_PORT       TCP port (default 8090; the next free one up to +9)
--   MANAGER_LOG_IP     UDP log sink: the URL and the token go there too
--   MANAGER_LOG_PORT
--   MANAGER_SCRATCH    test prefix: containers named <prefix><A-Z 0-9> are
--                      treated as saves (the offline tests and console tests
--                      use it; never a real title's prefix)
-- MANAGER_TEST = true makes the script return its pure helpers (desktop Lua,
-- ghostldr/tests/test_manager.py) before touching anything.
--
-- Safety: writes and deletes need the session token, shown as a PS5
-- notification at start.  SWRR's own save (SLUS-20268 / SLES-50366) is only
-- ever read, from /savedata0, never mounted, written or deleted.  Names are
-- checked against strict patterns.  Mounts and files are tracked and released
-- after every request, also on errors.

local M = {}
M.VERSION = "1"
M.BLOCK = 32768                    -- save data block
M.MIN_BLOCKS, M.MAX_BLOCKS = 96, 32768
M.MAX_FILES = 1024                 -- entries in a save (walk and archives)
M.MAX_DEPTH = 8
M.MAX_UPLOAD = (1 << 30) + (64 << 20)
M.MANIFEST = "ghostldr-save.json"
-- the host's own save: SWRR's (with Luac0re), or Jak X's (with UltraC0re)
M.PROTECTED = {["SLUS-20268"] = true, ["SLES-50366"] = true, ["SCUS-97429"] = true}
M.SWRR = {CUSA03474 = "SLUS-20268", CUSA03492 = "SLES-50366", CUSA07992 = "SCUS-97429"}
M.REGION = {CUSA03474 = "US", CUSA03492 = "EU", CUSA07992 = "EU"}
M.KNOWN = {CUSA11456 = {name = "Ghost of Tsushima"}}   -- for installs whose dump has no param.sfo
M.TOKEN_ALPHABET = "ACDEFGHJKLMNPQRTUVWXY34679"
M.ERRORS = {
    [0x809F0000] = "parameter", [0x809F0003] = "busy", [0x809F0004] = "not mounted",
    [0x809F0007] = "exists", [0x809F0008] = "not found",
    [0x809F000A] = "no space left on the console's storage", [0x809F000B] = "internal",
    [0x809F000C] = "too many mounted", [0x809F000F] = "broken",
    [0x80020002] = "no such file", [0x8002000D] = "permission denied", [0x80020010] = "busy",
    [0x80020011] = "exists", [0x8002001C] = "the container is full", [0x8002001E] = "read-only",
}

function M.errtext(rc)
    local c = (rc or 0) & 0xFFFFFFFF
    local why = M.ERRORS[c]
    return string.format("0x%08x", c) .. (why and (" (" .. why .. ")") or "")
end

-- ---------------------------------------------------------------- JSON
local OBJ = {}
function M.obj(t) return setmetatable(t or {}, OBJ) end
local ESC = {['"'] = '\\"', ["\\"] = "\\\\", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t"}
local function jstr(s)
    return '"' .. (s:gsub('[%c"\\]', function(ch) return ESC[ch] or string.format("\\u%04x", ch:byte()) end)) .. '"'
end
function M.json(v)
    local t = type(v)
    if v == nil then return "null"
    elseif t == "boolean" then return v and "true" or "false"
    elseif t == "number" then
        if math.type(v) == "integer" then return string.format("%d", v) end
        if v ~= v or v == math.huge or v == -math.huge then return "null" end
        return string.format("%.14g", v)
    elseif t == "string" then return jstr(v)
    elseif t == "table" then
        local out = {}
        if getmetatable(v) ~= OBJ and (v[1] ~= nil or next(v) == nil) then
            for i = 1, #v do out[i] = M.json(v[i]) end
            return "[" .. table.concat(out, ",") .. "]"
        end
        local keys = {}
        for key in pairs(v) do keys[#keys + 1] = key end
        table.sort(keys)
        for _, key in ipairs(keys) do out[#out + 1] = jstr(key) .. ":" .. M.json(v[key]) end
        return "{" .. table.concat(out, ",") .. "}"
    end
    error("json: cannot encode " .. t)
end

-- ---------------------------------------------------------------- HTTP
function M.parse_head(s)
    local line, rest = s:match("^([^\r\n]*)\r?\n(.*)$")
    if not line then return nil, "bad request" end
    local method, target = line:match("^(%u+) (%S+) HTTP/1%.[01]$")
    if not method then return nil, "bad request line" end
    local headers = {}
    for key, value in rest:gmatch("([%w%-]+):[ \t]*([^\r\n]*)\r?\n") do
        headers[key:lower()] = (value:gsub("[ \t]+$", ""))
    end
    return {method = method, target = target, headers = headers}
end

function M.url_decode(s)
    s = s:gsub("%+", " ")
    return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

function M.parse_target(target)
    local path, q = target:match("^([^?#]*)%??([^#]*)")
    local query = {}
    for pair in (q or ""):gmatch("[^&]+") do
        local key, value = pair:match("^([^=]*)=?(.*)$")
        query[M.url_decode(key)] = M.url_decode(value)
    end
    return path or "/", query
end

M.STATUS = {[200] = "OK", [204] = "No Content", [400] = "Bad Request", [403] = "Forbidden",
            [404] = "Not Found", [405] = "Method Not Allowed", [409] = "Conflict", [411] = "Length Required",
            [413] = "Payload Too Large", [429] = "Too Many Requests", [431] = "Request Header Fields Too Large",
            [500] = "Internal Server Error", [503] = "Service Unavailable"}

function M.response_head(code, ctype, length, extra)
    local h = {string.format("HTTP/1.1 %d %s", code, M.STATUS[code] or "Status"),
               "Content-Type: " .. ctype, string.format("Content-Length: %d", length), "Connection: close",
               "Cache-Control: no-store", "X-Content-Type-Options: nosniff", "Referrer-Policy: no-referrer"}
    for _, line in ipairs(extra or {}) do h[#h + 1] = line end
    return table.concat(h, "\r\n") .. "\r\n\r\n"
end

-- ---------------------------------------------------------------- names
-- Any name handed to the save service: SWRR's dirName rule, upper case only.
function M.valid_container(name)
    return type(name) == "string" and #name >= 1 and #name <= 31 and name:match("^[A-Z0-9][A-Z0-9_-]*$") ~= nil
end

-- A title's save containers: <prefix>SAVE (its progress, save memory),
-- <prefix>D<nnn>SAVE (one save directory), <prefix>M<nn>SAVE (a save memory
-- slot).  Returns kind, prefix, slot.
function M.save_kind(name)
    if not M.valid_container(name) or M.PROTECTED[name] then return nil end
    local p, n = name:match("^([A-Z0-9]+)D(%d%d%d)SAVE$")
    if p and #p <= 12 then return "dir", p, n end
    p, n = name:match("^([A-Z0-9]+)M(%d%d)SAVE$")
    if p and #p <= 12 then return "memory", p, n end
    p = name:match("^([A-Z0-9]+)SAVE$")
    if p and #p <= 12 then return "progress", p end
    return nil
end

-- A title's data containers: <prefix>000 .. <prefix>095
function M.data_kind(name)
    if not M.valid_container(name) then return nil end
    local p, k = name:match("^([A-Z0-9]+)(%d%d%d)$")
    if p and #p <= 12 then return p, tonumber(k) end
    return nil
end

-- the test prefix: 3-8 of A-Z 0-9, nothing a title, index or SWRR uses
function M.scratch_ok(prefix)
    if type(prefix) ~= "string" or #prefix < 3 or #prefix > 8 or not prefix:match("^[A-Z][A-Z0-9]*$") then
        return false
    end
    for _, bad in ipairs({"GHOST", "GFS", "SL", "G0", "G1", "G2", "G3", "G4", "G5", "G6", "G7", "G8", "G9"}) do
        if prefix:sub(1, #bad) == bad or bad:sub(1, #prefix) == prefix then return false end
    end
    return true
end

function M.scratch_member(prefix, name)
    if not prefix or not M.valid_container(name) or #name <= #prefix or name:sub(1, #prefix) ~= prefix then
        return false
    end
    local rest = name:sub(#prefix + 1)
    return rest:match("^[A-Z0-9]+$") ~= nil and name:sub(-4) ~= "SAVE" and name:sub(-3) ~= "IDX"
end

-- Can `name` be restored into, copied into or deleted?  Returns its group
-- ("save" with the title prefix, or "scratch") or nil and why not.
function M.write_target(name, scratch)
    if not M.valid_container(name) then return nil, "not a valid container name" end
    if M.PROTECTED[name] then return nil, name .. " is SWRR's own save: read-only" end
    if scratch and M.scratch_member(scratch, name) then return "scratch", scratch end
    local kind, prefix = M.save_kind(name)
    if kind then return "save", prefix end
    return nil, name .. " is not a save container (<prefix>SAVE, <prefix>D<nnn>SAVE or <prefix>M<nn>SAVE)"
end

-- ---------------------------------------------------------------- paths
-- A relative path inside a save, as restore accepts it.  Returns the
-- normalised path and whether it named a directory, or nil and why.
function M.clean_path(p)
    if type(p) ~= "string" or #p == 0 or #p > 240 then return nil, "bad path length" end
    p = p:gsub("^%./+", "")
    local isdir = p:sub(-1) == "/"
    p = p:gsub("/+$", "")
    if p == "" then return nil, "empty path" end
    if p:sub(1, 1) == "/" then return nil, "absolute path" end
    local parts = {}
    for comp in (p .. "/"):gmatch("([^/]*)/") do
        if comp == "" or comp == "." or comp == ".." then return nil, "bad path component in " .. p end
        if #comp > 128 or not comp:match("^[%w%._%-]+$") then return nil, "unsupported characters in " .. p end
        parts[#parts + 1] = comp
    end
    if #parts > M.MAX_DEPTH then return nil, "path too deep: " .. p end
    return table.concat(parts, "/"), isdir
end

function M.is_system(path) return path == "sce_sys" or path:sub(1, 8) == "sce_sys/" end

function M.file_blocks(size) return math.max(1, (size + M.BLOCK - 1) // M.BLOCK) end

-- ---------------------------------------------------------------- tar (ustar)
local function pad512(n) return (n + 511) // 512 * 512 end
M.pad512 = pad512

local function octal(n, width)
    local s = string.format("%0" .. (width - 1) .. "o", n)
    if #s ~= width - 1 then return nil end
    return s .. "\0"
end

local function field(s, n) return s .. string.rep("\0", n - #s) end

-- 512-byte ustar header; `name` gets a trailing "/" for directories
function M.tar_header(name, size, mtime, isdir)
    if isdir and name:sub(-1) ~= "/" then name = name .. "/" end
    local prefix = ""
    if #name > 100 then
        local cut
        for i = math.min(#name - 1, 156), 2, -1 do
            if name:sub(i, i) == "/" and i - 1 <= 155 and #name - i <= 100 then cut = i break end
        end
        if not cut then return nil, "name too long for tar: " .. name end
        prefix, name = name:sub(1, cut - 1), name:sub(cut + 1)
    end
    local sz, mt = octal(isdir and 0 or size, 12), octal(math.max(0, math.min(mtime or 0, 0x1FFFFFFFF)), 12)
    if not sz or not mt then return nil, "file too large for tar: " .. name end
    local h = field(name, 100) .. octal(isdir and 493 or 420, 8) .. octal(0, 8) .. octal(0, 8) .. sz .. mt
        .. "        " .. (isdir and "5" or "0") .. string.rep("\0", 100) .. "ustar\0" .. "00"
        .. field("ghostldr", 32) .. field("ghostldr", 32) .. octal(0, 8) .. octal(0, 8) .. field(prefix, 155)
        .. string.rep("\0", 12)
    local sum = 0
    for i = 1, 512 do sum = sum + h:byte(i) end
    return h:sub(1, 148) .. string.format("%06o", sum) .. "\0 " .. h:sub(157)
end

-- archive length: the manifest, then each item (header + data), then two
-- zero blocks
function M.tar_length(manifest_len, items)
    local n = 512 + pad512(manifest_len)
    for _, it in ipairs(items) do n = n + 512 + (it.dir and 0 or pad512(it.size)) end
    return n + 1024
end

local ZERO512 = string.rep("\0", 512)
function M.tar_parse(b)
    if #b ~= 512 then return nil, "short header" end
    if b == ZERO512 then return "end" end
    local sum = 0
    for i = 1, 512 do sum = sum + ((i >= 149 and i <= 156) and 32 or b:byte(i)) end
    local stored = tonumber(b:sub(149, 156):match("^[ %z]*([0-7]+)") or "x", 8)
    if stored ~= sum then return nil, "not a tar archive (bad header checksum)" end
    local function str(a, z) return (b:sub(a, z):match("^[^%z]*")) end
    local name = str(1, 100)
    if b:sub(258, 263) == "ustar\0" then
        local pre = str(346, 500)
        if pre ~= "" then name = pre .. "/" .. name end
    end
    if b:byte(125) >= 0x80 then return nil, "file too large: " .. name end
    local size = tonumber(b:sub(125, 136):match("^[ %z]*([0-7]*)"), 8) or 0
    local mtime = tonumber(b:sub(137, 148):match("^[ %z]*([0-7]*)"), 8) or 0
    local typ = b:sub(157, 157)
    if typ == "\0" then typ = "0" end
    return {name = name, size = size, mtime = mtime, typ = typ}
end

function M.pax_parse(s)
    local out, pos = {}, 1
    while pos <= #s do
        local len = s:match("^(%d+) ", pos)
        local n = tonumber(len or "")
        if not n or n < 5 or pos + n - 1 > #s then break end
        local key, value = s:sub(pos, pos + n - 1):match("^%d+ ([^=]+)=(.*)\n$")
        if key then out[key] = value end
        pos = pos + n
    end
    return out
end

-- Every entry of an archive of `total` bytes, read through read_at(offset,
-- length).  pax and GNU long names are applied to the entry they describe.
function M.tar_scan(read_at, total)
    local entries, off, pending = {}, 0, nil
    while off < total do
        if off + 512 > total then return nil, "truncated archive" end
        local h, err = M.tar_parse(read_at(off, 512))
        if not h then return nil, err end
        if h == "end" then break end
        local data = off + 512
        if h.typ == "x" or h.typ == "L" or h.typ == "g" then
            if h.size > 65536 then return nil, "oversized extended header" end
            if data + h.size > total then return nil, "truncated archive" end
            if h.typ ~= "g" then
                local body = read_at(data, h.size)
                pending = pending or {}
                if h.typ == "x" then
                    local px = M.pax_parse(body)
                    if px.path then pending.path = px.path end
                    if px.size then pending.size = tonumber(px.size) end
                else
                    pending.path = body:match("^[^%z]*")
                end
            end
        else
            if pending then
                h.name = pending.path or h.name
                h.size = pending.size or h.size
                pending = nil
            end
            h.off = data
            if h.typ ~= "0" and h.typ ~= "7" then h.size_in_archive = h.size end
            if data + (h.typ == "5" and 0 or h.size) > total then return nil, "truncated archive (" .. h.name .. ")" end
            entries[#entries + 1] = h
            if #entries > 4 * M.MAX_FILES then return nil, "too many entries" end
        end
        off = data + pad512((h.typ == "5") and 0 or h.size)
    end
    return entries
end

-- The folder to strip when every entry sits below one (the manager's own
-- archives: <CONTAINER>/... next to the manifest).  "" for none.
function M.auto_strip(entries)
    local first, manifest = nil, false
    for _, e in ipairs(entries) do
        local n = e.name:gsub("^%./+", "")
        if n == M.MANIFEST then
            manifest = true
        elseif n ~= "" then
            local c = n:match("^([^/]+)")
            if first == nil then first = c elseif first ~= c then return "" end
            if not n:find("/", 1, true) and e.typ ~= "5" then return "" end
        end
    end
    return (manifest and first) or ""
end

-- What a restore writes: files (path, size, off), directories (parents
-- first), skipped entries, and the manifest entry.
function M.restore_plan(entries, strip)
    local files, dirs, skipped, seen, manifest = {}, {}, {}, {}, nil
    strip = strip or ""
    for _, e in ipairs(entries) do
        local raw = e.name:gsub("^%./+", "")
        if raw == M.MANIFEST then
            manifest = e
        else
            local rel = raw
            if strip ~= "" then
                if rel == strip or rel == strip .. "/" then
                    rel = nil
                elseif rel:sub(1, #strip + 1) == strip .. "/" then
                    rel = rel:sub(#strip + 2)
                else
                    return nil, "entry outside " .. strip .. "/: " .. raw
                end
            end
            if rel and rel ~= "" then
                local path, isdir = M.clean_path(rel)
                if not path then return nil, isdir end
                local base = path:match("[^/]+$")
                isdir = isdir or e.typ == "5"
                if M.is_system(path) then
                    skipped[#skipped + 1] = path
                elseif base:sub(1, 2) == "._" or base == ".DS_Store" then
                    skipped[#skipped + 1] = path
                elseif isdir then
                    if seen[path] == "file" then return nil, path .. " is both a file and a folder" end
                    if not seen[path] then seen[path] = "dir"; dirs[#dirs + 1] = path end
                elseif e.typ == "0" or e.typ == "7" then
                    if seen[path] then return nil, "duplicate entry " .. path end
                    seen[path] = "file"
                    files[#files + 1] = {path = path, size = e.size, off = e.off}
                else
                    return nil, "unsupported entry type '" .. e.typ .. "' (links and devices): " .. path
                end
            end
        end
    end
    -- parents of every file and folder
    local all = {}
    for _, f in ipairs(files) do all[#all + 1] = f.path end
    for _, d in ipairs(dirs) do all[#all + 1] = d end
    for _, p in ipairs(all) do
        local parent = p:match("^(.*)/[^/]+$")
        while parent do
            if seen[parent] == "file" then return nil, parent .. " is both a file and a folder" end
            if not seen[parent] then seen[parent] = "dir"; dirs[#dirs + 1] = parent end
            parent = parent:match("^(.*)/[^/]+$")
        end
    end
    local function depth(p) return select(2, p:gsub("/", "")) end
    table.sort(dirs, function(a, b)
        if depth(a) ~= depth(b) then return depth(a) < depth(b) end
        return a < b
    end)
    if #files == 0 then return nil, "the archive holds no save files" end
    if #files + #dirs > M.MAX_FILES then return nil, "too many files" end
    local bytes = 0
    for _, f in ipairs(files) do bytes = bytes + f.size end
    return {files = files, dirs = dirs, skipped = skipped, manifest = manifest, bytes = bytes}
end

-- blocks the files and folders take (each file at least one block, one per
-- folder), plus a small margin
function M.blocks_needed(plan)
    local n = #plan.dirs + 4
    for _, f in ipairs(plan.files) do n = n + M.file_blocks(f.size) end
    return n
end

-- A new container holds the file system's own blocks and the system's
-- sce_sys files besides the save: measured 44 of 96 blocks on a fresh one
-- (34 for the file system, 10 for sce_sys), and about the same on SWRR's own
-- 586-block save.
M.OVERHEAD = 48

-- size of a new container for `needed` blocks of files, with some slack, at
-- least `want` (the original size from a manifest)
function M.create_blocks(needed, want)
    local blocks = math.max(M.MIN_BLOCKS, needed + M.OVERHEAD + math.max(8, needed // 32), want or 0)
    if blocks > M.MAX_BLOCKS then
        if needed + M.OVERHEAD <= M.MAX_BLOCKS then return M.MAX_BLOCKS end
        return nil
    end
    return blocks
end

function M.manifest_blocks(text)
    local b = tonumber((text or ""):match('"blocks"%s*:%s*(%d+)') or "")
    if b and b >= M.MIN_BLOCKS and b <= M.MAX_BLOCKS then return b end
    return nil
end

-- ---------------------------------------------------------------- titles
function M.sfo_parse(s)
    if type(s) ~= "string" or #s < 20 or s:sub(1, 4) ~= "\0PSF" then return nil end
    local kt, dt, n = string.unpack("<I4I4I4", s, 9)
    local out = {}
    for i = 0, math.min(n, 128) - 1 do
        local e = 21 + 16 * i
        if e + 15 > #s then break end
        local ko, fmt, len, _, doff = string.unpack("<I2I2I4I4I4", s, e)
        local key = s:match("^[^%z]*", kt + ko + 1)
        local p = dt + doff + 1
        if key and key ~= "" and p + len - 1 <= #s then
            if fmt == 0x0404 and len >= 4 then
                out[key] = string.unpack("<I4", s, p)
            else
                out[key] = s:sub(p, p + len - 1):match("^[^%z]*")
            end
        end
    end
    return out
end

-- gfs_pack.py's index header (version 1: 64 bytes, version 2: 128)
function M.index_header(s)
    if type(s) ~= "string" or #s < 64 or s:sub(1, 4) ~= "GFS1" then return nil end
    local version, count, entries, cap, total, names_off, names_size = string.unpack("<I4I4I4I8I8I4I4", s, 5)
    local h = {version = version, containers = count, entries = entries, cap = cap, total = total,
               names_off = names_off, names_size = names_size, prefix = s:sub(41, 56):match("^[^%z]*"),
               header_size = 64, flags = 0, title = "", layout = ""}
    if version >= 2 and #s >= 88 then
        h.header_size, h.flags = string.unpack("<I4I4", s, 57)
        h.title = s:sub(65, 80):match("^[^%z]*")
        h.layout = string.format("%016x", string.unpack("<I8", s, 81))
    end
    if not h.prefix:match("^[A-Z0-9]+$") or #h.prefix > 12 or count > 96 or cap == 0 then return nil end
    if not h.title:match("^[A-Z0-9_-]*$") then h.title = "" end
    h.incomplete = (h.flags & 1) ~= 0
    return h
end

-- binary search of the index for `want`: offset, size, flags
function M.index_find(read_at, h, want)
    local lo, hi = 0, h.entries - 1
    for _ = 1, 40 do
        if lo > hi then return nil end
        local mid = (lo + hi) // 2
        local off, size, name_off, flags = string.unpack("<I8I8I4I4", read_at(h.header_size + 24 * mid, 64))
        local nm = read_at(h.names_off + name_off, 160):match("^[^%z]*")
        if nm == want then return off, size, flags end
        if nm < want then lo = mid + 1 else hi = mid - 1 end
    end
    return nil
end

-- ---------------------------------------------------------------- catalogue
-- GFSCATALOG/catalog.bin (docs/ghostfs-catalogue.md on hle/title-agnostic):
-- a 64-byte header "GFSC", version, header_size, entry_size, count, flags,
-- generation u64; entries of entry_size (192 in version 1): title[16]
-- prefix[16] index[32] name[64] version[16] layout u64, total u64,
-- containers u32, flags u32 (1 INCOMPLETE, 2 LEGACY_INDEX), updated u64.
M.CATALOG = "GFSCATALOG"
M.CAT_INCOMPLETE, M.CAT_LEGACY = 1, 2

function M.catalogue_parse(s)
    if type(s) ~= "string" or #s < 64 or s:sub(1, 4) ~= "GFSC" then return nil end
    local version, hsize, esize, count, flags, gen = string.unpack("<I4I4I4I4I4i8", s, 5)
    if hsize < 64 or esize < 192 or count > 4096 or #s < hsize + count * esize then return nil end
    local cat = {version = version, header_size = hsize, entry_size = esize, flags = flags, generation = gen,
                 entries = {}, raw = s}
    for i = 0, count - 1 do
        local off = hsize + i * esize
        local e = s:sub(off + 1, off + esize)
        local function str(a, n) return (e:sub(a + 1, a + n):match("^[^%z]*")) end
        local layout, total, conts, ef, upd = string.unpack("<I8I8I4I4i8", e, 0x91)
        cat.entries[#cat.entries + 1] = {title = str(0, 16), prefix = str(0x10, 16), index = str(0x20, 32),
            name = str(0x40, 64), version = str(0x80, 16), layout = string.format("%016x", layout), total = total,
            containers = conts, flags = ef, updated = upd, raw = e}
    end
    return cat
end

-- catalog.bin with `prefix`'s entry marked INCOMPLETE ("mark", at time `now`)
-- or dropped ("drop"), generation + 1; other bytes stay as they are
function M.catalogue_update(cat, prefix, how, now)
    local parts, found = {}, false
    for _, e in ipairs(cat.entries) do
        if e.prefix == prefix then
            found = true
            if how == "mark" then
                local f = string.unpack("<I4", e.raw, 0xA5) | M.CAT_INCOMPLETE
                parts[#parts + 1] = e.raw:sub(1, 0xA4) .. string.pack("<I4i8", f, now or 0) .. e.raw:sub(0xB1)
            end
        else
            parts[#parts + 1] = e.raw
        end
    end
    if not found then return nil end
    local head = cat.raw:sub(1, cat.header_size)
    head = head:sub(1, 0x10) .. string.pack("<I4", #parts) .. head:sub(0x15, 0x18)
        .. string.pack("<i8", cat.generation + 1) .. head:sub(0x21)
    return head .. table.concat(parts)
end

-- index containers: <prefix>INDEX, and the legacy GFSINDEX / GHOSTIDX
function M.is_index_name(name)
    if name == "GHOSTIDX" then return true end
    if not M.valid_container(name) then return false end
    local p = name:match("^([A-Z0-9]+)INDEX$")
    return p ~= nil and #p <= 12
end

-- the installer's free-space probes, left over when it was interrupted
function M.probe_name(name) return type(name) == "string" and name:match("^GFSFREE%d%d$") ~= nil end

local function cont(c)
    return M.obj{name = c.name, blocks = c.blocks, free = c.free, mtime = c.mtime}
end

local function save_entry(c, kind, slot)
    local e = cont(c)
    e.kind, e.slot = kind, slot
    return e
end

local INDEX_RANK = {GFSINDEX = 2, GHOSTIDX = 3}

-- The state the page shows, from the container list, the index headers
-- (headers[name] = parsed header, or false when unreadable) and the
-- catalogue (parsed, or nil when there is none).
-- opts: own (SWRR's save name), scratch (prefix), catalogue, names
-- ({prefix = {name, version}} from param.sfo), titles ({title ID = {name,
-- version}} from the computer's profiles).
function M.classify(listing, headers, opts)
    opts = opts or {}
    local cat = opts.catalogue
    local st = {titles = {}, orphans = {}, other = {}, swrr_save = nil, scratch = nil, catalogue = nil}
    local titles, order = {}, {}
    local function title_for(prefix)
        if not titles[prefix] then
            titles[prefix] = {prefix = prefix, data = {}, indexes = {}}
            order[#order + 1] = prefix
        end
        return titles[prefix]
    end
    local rest, saves, present_names = {}, {}, {}
    if opts.scratch then st.scratch = M.obj{prefix = opts.scratch, saves = {}} end
    for _, e in ipairs(cat and cat.entries or {}) do title_for(e.prefix).entry = e end
    for _, c in ipairs(listing) do
        local n = c.name
        present_names[n] = true
        if M.PROTECTED[n] then
            local o = cont(c)
            o.readonly = true
            if n == opts.own then st.swrr_save = o else st.other[#st.other + 1] = o end
        elseif opts.scratch and M.scratch_member(opts.scratch, n) then
            local s = st.scratch.saves
            s[#s + 1] = save_entry(c, "scratch")
        elseif n == M.CATALOG then
            st.catalogue = cont(c)
            st.catalogue.titles = cat and #cat.entries or nil
            st.catalogue.generation = cat and cat.generation or nil
            st.catalogue.readable = cat ~= nil
        elseif headers[n] ~= nil then
            local h = headers[n]
            if h then
                local t = title_for(h.prefix)
                t.indexes[#t.indexes + 1] = {name = n, header = h}
            else
                local o = cont(c)
                o.note = "unreadable index"
                st.other[#st.other + 1] = o
            end
        elseif M.probe_name(n) then
            local o = cont(c)
            o.note, o.deletable = "free-space probe left over", true
            st.other[#st.other + 1] = o
        else
            rest[#rest + 1] = c
        end
    end
    for _, c in ipairs(rest) do
        local p, k = M.data_kind(c.name)
        if p and p:sub(1, 3) ~= "GFS" then
            local t = title_for(p)
            t.data[#t.data + 1] = {name = c.name, k = k, blocks = c.blocks}
        else
            local kind, sp, slot = M.save_kind(c.name)
            if kind then
                saves[sp] = saves[sp] or {}
                local s = saves[sp]
                s[#s + 1] = save_entry(c, kind, slot)
            else
                st.other[#st.other + 1] = cont(c)
            end
        end
    end
    for _, prefix in ipairs(order) do
        local t, e = titles[prefix], titles[prefix].entry
        -- the index: the catalogue's choice, else <P>INDEX, GFSINDEX, GHOSTIDX
        table.sort(t.indexes, function(a, b)
            local ra = (e and a.name == e.index) and 0 or INDEX_RANK[a.name] or 1
            local rb = (e and b.name == e.index) and 0 or INDEX_RANK[b.name] or 1
            if ra ~= rb then return ra < rb end
            return a.name < b.name
        end)
        local idx = t.indexes[1]
        local h = idx and idx.header
        local out = M.obj{prefix = prefix, saves = saves[prefix] or {}, stale_indexes = {}, catalogued = e ~= nil}
        saves[prefix] = nil
        for i = 2, #t.indexes do out.stale_indexes[#out.stale_indexes + 1] = t.indexes[i].name end
        out.index = idx and idx.name or (e and e.index ~= "" and e.index) or nil
        local have, blocks = {}, 0
        for _, d in ipairs(t.data) do have[d.k] = true; blocks = blocks + d.blocks end
        out.blocks = blocks
        local id = (e and e.title ~= "" and e.title) or (h and h.title ~= "" and h.title) or nil
        out.id = id
        local expected = (h and h.containers) or (e and e.containers) or 0
        local present, missing, extra = 0, {}, {}
        for k = 0, expected - 1 do
            if have[k] then present = present + 1 else missing[#missing + 1] = string.format("%s%03d", prefix, k) end
        end
        for _, d in ipairs(t.data) do if d.k >= expected then extra[#extra + 1] = d.name end end
        out.containers, out.present, out.missing, out.extra = expected, present, missing, extra
        out.bytes = (h and h.total) or (e and e.total) or nil
        out.layout = (h and h.layout ~= "" and h.layout) or (e and e.layout) or nil
        if not h and not e then
            out.status = "leftover data without an index"
        elseif e and (e.flags & M.CAT_INCOMPLETE) ~= 0 then
            out.status = "install interrupted (or removal)"
        elseif not h then
            out.status = "index missing: " .. tostring(out.index)
        elseif h.incomplete then
            out.status = "install interrupted"
        elseif present < expected then
            out.status = string.format("partial: %d of %d data containers", present, expected)
        else
            out.status = "complete"
        end
        out.complete = out.status == "complete"
        local nm = (e and e.name ~= "" and e) or (opts.names and opts.names[prefix])
            or (id and opts.titles and opts.titles[id]) or (id and M.KNOWN[id])
        if nm then
            out.name = nm.name
            out.version = (nm.version ~= "" and nm.version) or nil
        end
        if e and not out.version and e.version ~= "" then out.version = e.version end
        st.titles[#st.titles + 1] = out
    end
    local prefixes = {}
    for p in pairs(saves) do prefixes[#prefixes + 1] = p end
    table.sort(prefixes)
    for _, p in ipairs(prefixes) do st.orphans[#st.orphans + 1] = M.obj{prefix = p, saves = saves[p]} end
    return st
end

-- ---------------------------------------------------------------- token
-- A 64-bit mixer: entropy goes in with add(), next() gives output.
function M.mixer(seed)
    local h = (seed or 0) ~ 0x2545F4914F6CDD1D
    local m = {}
    function m.add(v)
        h = (h ~ (v or 0)) * 0x9E3779B97F4A7C15
        h = h ~ (h >> 29)
    end
    function m.next()
        m.add(0x632BE59BD9B4E019)
        local z = (h ~ (h >> 30)) * 0xBF58476D1CE4E5B9
        z = (z ~ (z >> 27)) * 0x94D049BB133111EB
        return z ~ (z >> 31)
    end
    return m
end

function M.token(next64)
    local a, out = M.TOKEN_ALPHABET, {}
    for i = 1, 8 do
        local v = (next64() >> 11) % #a
        out[i] = a:sub(v + 1, v + 1)
    end
    return table.concat(out, "", 1, 4) .. "-" .. table.concat(out, "", 5, 8)
end

function M.token_norm(s) return ((s or ""):upper():gsub("[^A-Z0-9]", "")) end

-- ---------------------------------------------------------------- the page
M.PAGE = [==[<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ghostldr manager</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--fg:#1d2329;--mute:#66707a;--line:#dde2e7;--acc:#2f6fdb;--bad:#c2372c;--ok:#23845a;--warn:#a86b00}
@media (prefers-color-scheme:dark){:root{--bg:#121518;--card:#1b1f24;--fg:#e4e8ec;--mute:#98a2ac;--line:#2c333a;--acc:#6ea0ff;--bad:#ff6b5e;--ok:#4cc38a;--warn:#e0a640}}
*{box-sizing:border-box}[hidden]{display:none!important}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;flex-wrap:wrap;gap:8px 16px;align-items:center;padding:12px 16px;border-bottom:1px solid var(--line);background:var(--card);position:sticky;top:0;z-index:2}
h1{font-size:18px;margin:0}h2{font-size:16px;margin:0 0 4px}h3{font-size:14px;margin:12px 0 4px;color:var(--mute);font-weight:600}
.sp{flex:1}.mute{color:var(--mute)}.small{font-size:13px}main{max-width:1000px;margin:0 auto;padding:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin:0 0 14px}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
button,.btn{font:inherit;font-size:14px;padding:5px 11px;border-radius:7px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer;text-decoration:none;display:inline-block}
button:hover,.btn:hover{border-color:var(--acc)}button:disabled{opacity:.45;cursor:default}
button.primary{background:var(--acc);border-color:var(--acc);color:#fff}button.danger{color:var(--bad)}button.danger.solid{background:var(--bad);border-color:var(--bad);color:#fff}
input[type=text]{font:inherit;padding:5px 8px;border-radius:7px;border:1px solid var(--line);background:var(--bg);color:var(--fg);min-width:0}
table{width:100%;border-collapse:collapse;font-size:14px}td,th{text-align:left;padding:6px 6px;border-top:1px solid var(--line);vertical-align:middle}th{font-weight:600;color:var(--mute);font-size:13px;border-top:0}
td.acts{text-align:right;white-space:nowrap}td.acts>*{margin-left:4px}.name{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px;cursor:pointer}
.badge{display:inline-block;font-size:12px;padding:1px 8px;border-radius:99px;border:1px solid currentColor}.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}
#log{font:12px/1.5 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;max-height:180px;overflow:auto}
dialog{border:1px solid var(--line);border-radius:12px;background:var(--card);color:var(--fg);max-width:min(560px,94vw);width:100%;padding:18px}
dialog::backdrop{background:#0008}.files{max-height:240px;overflow:auto;font:12px/1.5 ui-monospace,Menlo,Consolas,monospace;border:1px solid var(--line);border-radius:7px;padding:6px 8px;margin:8px 0}
.progress{height:6px;background:var(--line);border-radius:3px;overflow:hidden;margin:8px 0}.progress>div{height:100%;width:0;background:var(--acc)}
@media (max-width:640px){td.hide,th.hide{display:none}td.acts{white-space:normal}}
</style></head><body>
<header><h1>ghostldr manager</h1><span id="where" class="mute small"></span><span class="sp"></span>
<span id="lock" class="small"></span><input id="tok" type="text" placeholder="token" size="10" autocomplete="off" spellcheck="false">
<button id="unlock">Unlock</button><button id="shownote" title="Show the token on the PS5 again">Show token on PS5</button>
<a class="btn" href="/install">Install a game</a><button id="refresh">Refresh</button><button id="exit" class="danger">Exit</button></header>
<main><div id="main"><div class="card mute">Loading...</div></div>
<div class="card"><h2>Activity</h2><div id="log" class="mute"></div></div></main>
<dialog id="dlg"><form method="dialog" id="dlgform"><h2 id="dlgtitle"></h2><div id="dlgbody"></div>
<div class="progress" id="prog" hidden><div></div></div>
<div class="row" style="justify-content:flex-end;margin-top:12px"><button type="button" id="dlgcancel">Cancel</button><button value="ok" id="dlgok" class="primary">OK</button></div></form></dialog>
<input type="file" id="picker" accept=".tar,application/x-tar" hidden>
<script>
"use strict";
const $=s=>document.querySelector(s);let S=null,token=sessionStorage.getItem("gldr-token")||"",busy=false;
const BLOCK=32768,MANIFEST="ghostldr-save.json";
function esc(s){return String(s==null?"":s).replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]))}
function size(n){if(n==null)return"";const u=["B","KiB","MiB","GiB"];let i=0;while(n>=1024&&i<3){n/=1024;i++}return(i?n.toFixed(n<10?1:0):n)+" "+u[i]}
function when(t){return t?new Date(t*1000).toLocaleString():""}
function log(m,cls){const d=document.createElement("div");if(cls)d.className=cls;d.textContent=new Date().toLocaleTimeString()+"  "+m;$("#log").prepend(d)}
function setBusy(b){busy=b;document.querySelectorAll("main button,header button").forEach(x=>{if(x.id!=="exit")x.disabled=b||(x.dataset.w&&!token)})}
async function api(method,path,body){const h={};if(method==="POST"&&token)h["X-Ghostldr-Token"]=token;
 const r=await fetch(path,{method,headers:h,body});let j=null;try{j=await r.json()}catch(e){}
 if(!r.ok||(j&&j.ok===false))throw new Error((j&&j.error)||(r.status+" "+r.statusText));return j}
function q(o){return Object.entries(o).map(([k,v])=>encodeURIComponent(k)+"="+encodeURIComponent(v)).join("&")}
function lockState(){$("#lock").innerHTML=token?'<a href="#" id="relock" class="ok" title="Enter another token">unlocked</a>':'<span class="warn">read-only: enter the token</span>';$("#tok").hidden=$("#unlock").hidden=!!token;
 const r=$("#relock");if(r)r.onclick=e=>{e.preventDefault();token="";sessionStorage.removeItem("gldr-token");lockState();setBusy(busy)}}
async function load(){setBusy(true);try{S=await api("GET","/api/state");render()}catch(e){log("refresh failed: "+e.message,"bad")}setBusy(false)}
function used(c){return c.blocks!=null&&c.free!=null?(c.blocks-c.free)*BLOCK:null}
function saveRows(list,opts){if(!list.length)return'<p class="mute small">No save containers.</p>';
 let h='<table><tr><th>Container</th><th>Kind</th><th class="hide">Used / size</th><th class="hide">Modified</th><th></th></tr>';
 for(const s of list){const k=s.kind==="dir"?"save dir "+s.slot:s.kind==="memory"?"memory slot "+s.slot:s.kind==="progress"?"progress":s.kind||"";
  h+=`<tr><td class="name" data-files="${esc(s.name)}">${esc(s.name)}</td><td>${esc(k)}</td><td class="hide">${size(used(s))} / ${size(s.blocks*BLOCK)}</td><td class="hide">${esc(when(s.mtime))}</td><td class="acts">
  ${s.deletable?"":`<a class="btn" href="/api/download?${q({c:s.name})}" download>Download</a>`}`;
  if(!opts.readonly)h+=`<button data-w="1" data-restore="${esc(s.name)}">Restore</button><button data-w="1" data-copy="${esc(s.name)}" data-group="${esc(opts.group)}">Copy</button><button data-w="1" class="danger" data-del="${esc(s.name)}">Delete</button>`;
  else if(s.deletable)h+=`<button data-w="1" class="danger" data-del="${esc(s.name)}">Delete</button>`;
  h+="</td></tr>"}return h+"</table>"}
function render(){const m=$("#main");let h="";
 $("#where").textContent=(S.swrr&&S.swrr.title?S.swrr.title+" ("+(S.swrr.region||"?")+")":"")+" user "+S.user+" · "+S.containers+" containers";
 for(const t of S.titles){const st=t.complete?'<span class="badge ok">complete</span>':`<span class="badge warn">${esc(t.status)}</span>`;
  h+=`<div class="card"><div class="row"><h2>${esc(t.name||t.id||"Unknown title")}</h2>${st}<span class="sp"></span>
  <button data-w="1" data-newsave="${esc(t.prefix)}">Restore into new container</button><button data-w="1" class="danger" data-deltitle="${esc(t.prefix)}">Delete game data</button></div>
  <div class="mute small">${esc(t.id||"no title ID")}${t.version?" · version "+esc(t.version):""} · prefix ${esc(t.prefix)} · ${t.bytes?size(t.bytes)+" in ":""}${t.present} of ${t.containers||t.present} data containers${t.index?" · index "+esc(t.index):""}${S.catalogue?(t.catalogued?" · in the catalogue":" · not in the catalogue"):""}</div>
  ${t.missing&&t.missing.length?'<div class="small bad">missing: '+esc(t.missing.join(", "))+"</div>":""}
  <h3>Saves</h3>${saveRows(t.saves,{group:t.prefix})}</div>`}
 if(!S.titles.length)h+='<div class="card mute">No installed titles.</div>';
 for(const o of S.orphans)h+=`<div class="card"><div class="row"><h2>Saves of prefix ${esc(o.prefix)}</h2><span class="badge warn">game data not installed</span><span class="sp"></span><button data-w="1" data-newsave="${esc(o.prefix)}">Restore into new container</button></div>${saveRows(o.saves,{group:o.prefix})}</div>`;
 if(S.scratch)h+=`<div class="card"><div class="row"><h2>Test containers ${esc(S.scratch.prefix)}*</h2><span class="sp"></span><button data-w="1" data-newsave="${esc(S.scratch.prefix)}" data-scratch="1">Restore into new container</button></div>${saveRows(S.scratch.saves,{group:"scratch"})}</div>`;
 if(S.swrr_save)h+=`<div class="card"><div class="row"><h2>SWRR's own save</h2><span class="badge">read-only</span></div><div class="mute small">The PS2 memory cards and Luac0re. It can be downloaded; the manager never writes or deletes it.</div>${saveRows([Object.assign({kind:"SWRR + Luac0re"},S.swrr_save)],{readonly:true})}</div>`;
 if(S.other.length)h+=`<div class="card"><h2>Other containers</h2>${saveRows(S.other.map(o=>Object.assign({kind:o.note||""},o)),{readonly:true})}</div>`;
 const cat=S.catalogue?(S.catalogue.readable?`catalogue GFSCATALOG: ${S.catalogue.titles} title(s), generation ${S.catalogue.generation}`:"GFSCATALOG unreadable"):"no catalogue (a single-title install)";
 h+=`<div class="card small mute">${esc(cat)}. Free space on the console: ${S.free==null?esc(S.free_note):size(S.free)}. Lua heap ${S.lua_kib} KiB. Manager v${esc(S.version)}.</div>`;
 m.innerHTML=h;setBusy(false)}
function dialog(title,body,okText,danger){return new Promise(res=>{const d=$("#dlg");$("#dlgtitle").textContent=title;$("#dlgbody").innerHTML=body;
 $("#dlgok").textContent=okText||"OK";$("#dlgok").className=danger?"danger solid":"primary";$("#dlgok").hidden=!okText;$("#prog").hidden=true;
 const conf=$("#dlgbody [data-confirm]");if(conf){$("#dlgok").disabled=true;conf.oninput=()=>{$("#dlgok").disabled=conf.value.trim()!==conf.dataset.confirm}}else $("#dlgok").disabled=false;
 d.onclose=()=>res(d.returnValue==="ok");d.returnValue="";d.showModal();if(conf)conf.focus()})}
function confirmField(name){return `<p>Type <b class="name">${esc(name)}</b> to confirm.</p><input type="text" data-confirm="${esc(name)}" style="width:100%" autocomplete="off" spellcheck="false">`}
function findSave(n){for(const t of S.titles)for(const s of t.saves)if(s.name===n)return s;for(const o of S.orphans)for(const s of o.saves)if(s.name===n)return s;
 if(S.scratch)for(const s of S.scratch.saves)if(s.name===n)return s;return null}
function saveName(n,group){if(group==="scratch")return S.scratch&&n.startsWith(S.scratch.prefix)&&/^[A-Z0-9]+$/.test(n)&&n.length<=31&&!/SAVE$|IDX$/.test(n)&&n.length>S.scratch.prefix.length;
 const m=/^([A-Z0-9]+?)(D\d{3}|M\d{2})?SAVE$/.exec(n);return!!m&&m[1]===group}
async function scanTar(file){const E=[];let off=0,pend=null;const td=new TextDecoder();
 while(off+512<=file.size){const h=new Uint8Array(await file.slice(off,off+512).arrayBuffer());if(h.every(b=>!b))break;
  let sum=0;for(let i=0;i<512;i++)sum+=(i>=148&&i<156)?32:h[i];const str=(a,b)=>{let e=a;while(e<b&&h[e])e++;return td.decode(h.subarray(a,e))};
  if(parseInt(str(148,156).trim(),8)!==sum)throw new Error("not a tar archive (bad header at byte "+off+")");
  let name=str(0,100);if(str(257,263)==="ustar"&&h[262]===0){const p=str(345,500);if(p)name=p+"/"+name}
  const sz=parseInt(str(124,136).trim()||"0",8);let typ=h[156]?String.fromCharCode(h[156]):"0";const data=off+512;
  if(typ==="x"||typ==="L"||typ==="g"){if(typ!=="g"){const t=td.decode(await file.slice(data,data+sz).arrayBuffer());pend=pend||{};
    if(typ==="L")pend.path=t.split("\0")[0];else for(const r of t.split("\n")){const m=/^\d+ ([^=]+)=(.*)$/.exec(r);if(m&&m[1]==="path")pend.path=m[2];if(m&&m[1]==="size")pend.size=+m[2]}}
   off=data+Math.ceil(sz/512)*512;continue}
  const e={name:pend&&pend.path||name,size:pend&&pend.size||sz,typ};pend=null;E.push(e);off=data+(typ==="5"?0:Math.ceil(e.size/512)*512)}
 return E}
function planTar(E){let first=null,man=false,flat=false;for(const e of E){const n=e.name.replace(/^\.\/+/,"");if(n===MANIFEST){man=true;continue}if(!n)continue;
  const c=n.split("/")[0];if(first===null)first=c;else if(first!==c)flat=true;if(!n.includes("/")&&e.typ!=="5")flat=true}
 const strip=man&&!flat&&first?first:"";const files=[],skipped=[],dirs=new Set();
 for(const e of E){let n=e.name.replace(/^\.\/+/,"");if(n===MANIFEST)continue;if(strip){if(n===strip||n===strip+"/")continue;n=n.slice(strip.length+1)}
  n=n.replace(/\/+$/,"");if(!n)continue;const base=n.split("/").pop();
  if(n==="sce_sys"||n.startsWith("sce_sys/")||base.startsWith("._")||base===".DS_Store"){skipped.push(n);continue}
  const parts=n.split("/");for(let i=1;i<parts.length;i++)dirs.add(parts.slice(0,i).join("/"));
  if(e.typ==="5")dirs.add(n);else files.push({path:n,size:e.size})}
 let need=dirs.size+4,bytes=0;for(const f of files){need+=Math.max(1,Math.ceil(f.size/BLOCK));bytes+=f.size}
 return{strip,files,skipped,need,bytes,manifest:man}}
async function restore(target,isNew,group){const pick=$("#picker");pick.value="";
 const file=await new Promise(r=>{pick.onchange=()=>r(pick.files[0]);pick.click()});if(!file)return;
 let p;try{p=planTar(await scanTar(file))}catch(e){return log(file.name+": "+e.message,"bad")}
 if(!p.files.length)return log(file.name+": no save files in it","bad");
 if(isNew){target=await askName(group,file);if(!target)return}
 let chk;setBusy(true);try{chk=await api("POST","/api/check-restore?"+q({c:target,need:p.need,bytes:p.bytes}))}catch(e){setBusy(false);return log("check "+target+": "+e.message,"bad")}setBusy(false);
 const list=p.files.map(f=>esc(f.path)+"  "+size(f.size)).join("<br>");
 const body=`<p>${p.files.length} file(s), ${size(p.bytes)} from <b>${esc(file.name)}</b>${p.strip?" (folder "+esc(p.strip)+"/)":""} into <b class="name">${esc(target)}</b>.</p>
  <div class="files">${list}${p.skipped.length?'<div class="mute">skipped (system or macOS files): '+esc(p.skipped.join(", "))+"</div>":""}</div>
  <p class="${chk.fits?"":"bad"}">${esc(chk.message)}</p>${chk.exists?'<p class="warn">Its current files are replaced (system files in sce_sys stay).</p>':""}${chk.fits?confirmField(target):""}`;
 if(!await dialog("Restore a save",body,chk.fits?"Restore":null,chk.exists))return;
 const d=$("#dlg");$("#dlgbody").innerHTML="<p>Uploading "+esc(file.name)+"...</p>";$("#prog").hidden=false;$("#dlgok").hidden=true;$("#dlgcancel").hidden=true;d.showModal();setBusy(true);
 try{const r=await new Promise((res,rej)=>{const x=new XMLHttpRequest();x.open("POST","/api/restore?"+q({c:target,confirm:target,strip:p.strip}));x.setRequestHeader("X-Ghostldr-Token",token);
   x.upload.onprogress=e=>{if(e.lengthComputable){$("#prog>div").style.width=(100*e.loaded/e.total)+"%";if(e.loaded===e.total)$("#dlgbody").innerHTML="<p>Writing to the console...</p>"}};
   x.onload=()=>{let j=null;try{j=JSON.parse(x.responseText)}catch(e){}(x.status===200&&j&&j.ok)?res(j):rej(new Error(j&&j.error||("HTTP "+x.status)))};x.onerror=()=>rej(new Error("network error"));x.send(file)});
  log(`restored ${r.files} file(s), ${size(r.bytes)} into ${target}${r.created?" (created, "+r.blocks+" blocks)":""}${r.skipped&&r.skipped.length?"; skipped "+r.skipped.join(", "):""}`,"ok")}
 catch(e){log("restore "+target+": "+e.message,"bad")}$("#dlgcancel").hidden=false;d.close();await load()}
async function askName(group,file){const scratch=group==="scratch";const pre=scratch?S.scratch.prefix:group;
 let sug=pre+"SAVE";if(scratch)sug=pre+"T1";else if(findSave(sug)){for(let i=1;i<1000;i++){const n=pre+"D"+String(i).padStart(3,"0")+"SAVE";if(!findSave(n)){sug=n;break}}}
 const body=`<p>New container for ${esc(file?file.name:"the copy")}:</p><input type="text" id="newname" value="${esc(sug)}" style="width:100%" autocomplete="off" spellcheck="false">
  <p class="small mute">${scratch?"Test containers: "+esc(pre)+" followed by A-Z/0-9.":esc(pre)+"SAVE holds the title's progress; "+esc(pre)+"D001SAVE.. are save folders, "+esc(pre)+"M01SAVE.. save memory slots."}</p><p id="nameerr" class="bad small"></p>`;
 const p=dialog("Choose the container",body,"Next");const inp=$("#newname");const chk=()=>{const v=inp.value.trim().toUpperCase();const ok=saveName(v,group)&&!findSave(v);$("#nameerr").textContent=ok?"":(findSave(v)?"exists already: use its Restore button":"not a valid name here");$("#dlgok").disabled=!ok};inp.oninput=chk;chk();
 if(!await p)return null;return inp.value.trim().toUpperCase()}
async function copy(from,group){const body=`<p>Copy the files of <b class="name">${esc(from)}</b> to:</p><input type="text" id="cpto" style="width:100%" autocomplete="off" spellcheck="false" list="cplist"><datalist id="cplist"></datalist><p id="cperr" class="bad small"></p><div id="cpconf"></div>`;
 const p=dialog("Copy a save",body,"Copy",true);const inp=$("#cpto");const dl=$("#cplist");
 const peers=group==="scratch"?S.scratch.saves:((S.titles.find(t=>t.prefix===group)||S.orphans.find(o=>o.prefix===group)||{saves:[]}).saves);
 dl.innerHTML=peers.filter(x=>x.name!==from).map(x=>`<option value="${esc(x.name)}">`).join("");
 const chk=()=>{const v=inp.value.trim().toUpperCase();const ok=v!==from&&saveName(v,group);$("#cperr").textContent=ok?(findSave(v)?"Its current files are replaced.":"A new container is created."):"Choose another container of the same title.";$("#cperr").className=ok?"small mute":"bad small";
  $("#cpconf").innerHTML=ok?confirmField(v):"";const c=$("#cpconf [data-confirm]");$("#dlgok").disabled=true;if(c)c.oninput=()=>{$("#dlgok").disabled=c.value.trim()!==v}};inp.oninput=chk;chk();
 if(!await p)return;const to=inp.value.trim().toUpperCase();setBusy(true);
 try{const r=await api("POST","/api/copy?"+q({from,to,confirm:to}));log(`copied ${r.files} file(s), ${size(r.bytes)} from ${from} to ${to}${r.created?" (created)":""}`,"ok")}catch(e){log("copy: "+e.message,"bad")}await load()}
async function delSave(n){if(!await dialog("Delete a save",`<p>Delete the save container <b class="name">${esc(n)}</b> and everything in it? Download it first if you may want it back.</p>`+confirmField(n),"Delete",true))return;
 setBusy(true);try{await api("POST","/api/delete?"+q({c:n,confirm:n}));log("deleted "+n,"ok")}catch(e){log("delete "+n+": "+e.message,"bad")}await load()}
async function delTitle(prefix){const t=S.titles.find(x=>x.prefix===prefix);const id=t.id||prefix;
 const body=`<p>Delete the installed game data of <b>${esc(t.name||id)}</b>: ${t.index?"its index "+esc(t.index)+" and ":""}${t.present+t.extra.length} data container(s)${t.bytes?", "+size(t.bytes):""}. Reinstalling takes a full install again.</p>
  <label><input type="checkbox" id="alsosaves"> Also delete its ${t.saves.length} save container(s) (its progress)</label>`+confirmField(id);
 if(!await dialog("Delete game data",body,"Delete",true))return;const saves=$("#alsosaves").checked;setBusy(true);
 try{const r=await api("POST","/api/delete-title?"+q({prefix,confirm:id,saves:saves?1:0}));log(`deleted ${r.deleted.length} container(s) of ${id}${r.failed.length?"; failed: "+r.failed.join(", "):""}`,r.failed.length?"bad":"ok")}catch(e){log("delete "+id+": "+e.message,"bad")}await load()}
async function files(n){setBusy(true);try{const r=await api("GET","/api/files?"+q({c:n}));setBusy(false);
  await dialog(n,`<div class="files">${r.files.map(f=>esc(f.path)+(f.dir?"/":"  "+size(f.size))).join("<br>")||"(empty)"}</div>`,null)}catch(e){setBusy(false);log(n+": "+e.message,"bad")}}
document.addEventListener("click",e=>{const b=e.target.closest("[data-restore],[data-copy],[data-del],[data-deltitle],[data-newsave],[data-files]");if(!b||busy)return;
 if(b.dataset.restore)restore(b.dataset.restore,false);
 else if(b.dataset.copy)copy(b.dataset.copy,b.dataset.group);else if(b.dataset.del)delSave(b.dataset.del);else if(b.dataset.deltitle)delTitle(b.dataset.deltitle);
 else if(b.dataset.newsave)restore(null,true,b.dataset.scratch?"scratch":b.dataset.newsave);else if(b.dataset.files)files(b.dataset.files)});
$("#unlock").onclick=async()=>{const t=$("#tok").value.trim();if(!t)return;const old=token;token=t;try{await api("POST","/api/auth");sessionStorage.setItem("gldr-token",t);log("unlocked","ok")}catch(e){token=old;log("unlock: "+e.message,"bad")}lockState();setBusy(false)};
$("#tok").onkeydown=e=>{if(e.key==="Enter")$("#unlock").click()};
$("#shownote").onclick=async()=>{try{await api("POST","/api/notify");log("token shown on the PS5")}catch(e){log(e.message,"bad")}};
$("#refresh").onclick=load;$("#dlgcancel").onclick=()=>$("#dlg").close("cancel");
$("#exit").onclick=async()=>{if(!await dialog("Stop the manager",'<p>Stop the web server and hand Luac0re\'s loader back (it can take other scripts again)? Start it again with <code>manager.py</code>.</p>',"Stop",true))return;
 try{await api("POST","/api/exit")}catch(e){}document.body.innerHTML='<main><div class="card"><h2>Manager stopped</h2><p>Luac0re\'s loader on port 9026 is free again.</p></div></main>'};
lockState();(async()=>{if(token){try{await api("POST","/api/auth")}catch(e){token="";sessionStorage.removeItem("gldr-token");log("the saved token is not valid for this session: enter the new one","warn")}lockState()}await load()})();
</script></body></html>]==]

-- ---------------------------------------------------------------- network install
-- The page (/install) packs a game in the browser and stores it through
-- /api/install/* (docs/manager.md "Install a game").  Which containers each
-- request may touch: never the host's own save or a title's progress
-- (*SAVE), never ghostldr's other containers (GLDRCACHE, GLDRCONF...).
--   data:   <prefix>000..: a title's data containers
--   index:  <prefix>INDEX, GFSINDEX, GHOSTIDX
--   meta:   <prefix>META
--   catalog GFSCATALOG
--   free:   GFSFREE00..99 (the free-space probes)
function M.install_kind(name)
    if not M.valid_container(name) or M.PROTECTED[name] or name:sub(-4) == "SAVE" then return nil end
    if name == "GFSCATALOG" then return "catalog" end
    if name == "GFSINDEX" or name == "GHOSTIDX" then return "index" end
    if M.probe_name(name) then return "free" end
    local p = name:match("^([A-Z0-9]+)INDEX$")
    if p and #p <= 12 then return "index" end
    p = name:match("^([A-Z0-9]+)META$")
    if p and #p <= 12 then return "meta" end
    p = name:match("^([A-Z0-9]+)%d%d%d$")
    if p and #p <= 12 then return "data" end
    return nil
end
M.INSTALL_OPS = {           -- the kinds each request may write
    put = {index = true, meta = true, catalog = true},
    write = {index = true, meta = true, catalog = true, data = true},
    delete = {index = true, meta = true, data = true, free = true},
    data = {data = true},
}
function M.install_file_ok(f) return type(f) == "string" and #f <= 31 and f:match("^[A-Za-z0-9][A-Za-z0-9._-]*$") ~= nil end

-- ghostldr/data/netinstall.html, copied in by ghostldr/tools/netinstall_page.py (the tests check it is current)
M.INSTALL_PAGE = [====[
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ghostldr: install a game</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1b1d22;--mute:#666b76;--line:#dfe2e8;--acc:#2f6fe4;--ok:#16834a;--warn:#a86400;--bad:#c3302b;--bar:#e7eaf0}
@media (prefers-color-scheme:dark){:root{--bg:#121417;--card:#1b1e23;--fg:#e8eaee;--mute:#9aa1ad;--line:#2c3038;--acc:#6b9cff;--ok:#4cc583;--warn:#e8a33a;--bad:#f0645e;--bar:#2a2e36}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 16px;border-bottom:1px solid var(--line);background:var(--card)}
header h1{font-size:17px;margin:0}
header .who{color:var(--mute);font-size:13px;text-align:right}
main{max-width:760px;margin:0 auto;padding:16px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin:0 0 14px}
h2{font-size:15px;margin:0 0 10px}
p{margin:6px 0}
.mute{color:var(--mute)}.small{font-size:13px}.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
button,.btn{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:8px;padding:8px 14px;cursor:pointer}
button.primary{background:var(--acc);border-color:var(--acc);color:#fff}
button:disabled{opacity:.45;cursor:default}
input[type=text]{font:inherit;padding:7px 9px;border:1px solid var(--line);border-radius:7px;background:var(--bg);color:var(--fg);min-width:0}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.drop{border:2px dashed var(--line);border-radius:10px;padding:16px;text-align:center;color:var(--mute);margin-top:10px}
.drop.on{border-color:var(--acc);color:var(--acc)}
table{border-collapse:collapse;width:100%;font-size:14px}
td,th{padding:5px 6px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{color:var(--mute);font-weight:500}
dl{display:grid;grid-template-columns:max-content 1fr;gap:4px 12px;margin:8px 0}
dt{color:var(--mute)}dd{margin:0;word-break:break-word}
.bar{height:12px;background:var(--bar);border-radius:6px;overflow:hidden;margin:8px 0}
.bar>i{display:block;height:100%;width:0;background:var(--acc);transition:width .3s}
#log{font:12px/1.4 ui-monospace,Menlo,Consolas,monospace;max-height:220px;overflow:auto;white-space:pre-wrap;color:var(--mute);margin-top:8px}
.hide{display:none!important}
</style>
</head>
<body>
<header><h1>ghostldr · Install a game</h1><div class="who" id="who">connecting…</div></header>
<main>
<section id="s-installed">
  <h2>Installed on the console</h2>
  <div id="installed" class="small mute">Reading…</div>
</section>

<section id="s-pick">
  <h2>1. Choose the game</h2>
  <p class="small mute">Choose the game's <b>app0</b> folder (the one with <code>eboot.bin</code> and <code>sce_sys</code>), or a
  <b>.zip</b> of it. On a phone, use a .zip.</p>
  <div class="row">
    <label class="btn" id="pick-dir-l">Choose folder…<input id="pick-dir" type="file" webkitdirectory multiple class="hide"></label>
    <label class="btn">Choose .zip…<input id="pick-zip" type="file" accept=".zip,application/zip" class="hide"></label>
  </div>
  <div class="drop" id="drop">or drop the folder or the .zip here</div>
  <div id="pick-msg" class="small"></div>
</section>

<section id="s-what" class="hide">
  <h2>2. Check</h2>
  <dl id="what"></dl>
  <div class="row" id="title-row" style="margin:6px 0"><label for="title-in">Title ID</label>
    <input id="title-in" type="text" maxlength="15" style="width:12em" autocomplete="off" spellcheck="false"></div>
  <div id="what-msg" class="small"></div>
</section>

<section id="s-go" class="hide">
  <h2>3. Install</h2>
  <div class="row" id="tok-row"><label for="tok">Token shown on the TV</label>
    <input id="tok" type="text" style="width:10em" autocomplete="off" spellcheck="false" placeholder="XXXX-XXXX"></div>
  <div class="row" style="margin-top:10px"><button class="primary" id="go">Install</button><button id="stop" disabled>Pause</button>
    <label class="small"><input type="checkbox" id="force"> resend everything</label></div>
  <div class="bar"><i id="bar"></i></div>
  <div id="status" class="small"></div>
  <div id="log"></div>
  <p class="small mute">Keep this page open, and the phone or computer awake, until it says the game is installed. If the
  connection drops it carries on where it stopped; a closed page can be resumed later by choosing the same game again.</p>
</section>
</main>
<script>
"use strict";
/*GFS-BEGIN: packing and installing, no browser APIs (tests run this part in node) */
function makeGFS(opts) {
opts = opts || {};
const CAP = opts.cap || 992 * 1024 * 1024;   // bytes of data per container (gfs_pack.py CAP; the tests make it tiny)
const DATA_BLOCKS = 32768, MIN_BLOCKS = 512, HEADER_SIZE = 128, FLAG_DIR = 1, FLAG_INCOMPLETE = 1;
const SMALL = 64 << 10, SMALL_BUDGET = 256 << 20;
const MAX_CONTAINERS = 96, MAX_TITLES = 64, CATALOG = "GFSCATALOG", CATALOG_FILE = "catalog.bin";
const CATALOG_BLOCKS = 512, INDEX_FILE = "index.bin", TAG = "gfs.tag";
const LEGACY_INDEXES = ["GFSINDEX", "GHOSTIDX"];
const INCOMPLETE = 1, LEGACY_INDEX = 2, SYNTHESIZED = 1;
const NOT_FOUND = 0x809F0008, GIB = 1 << 30;
const SAVE_ERRORS = {0x809F0003: "busy", 0x809F0008: "not found", 0x809F000A: "no space left on the console's storage",
                     0x809F000C: "too many mounted", 0x809F000F: "broken"};
const TITLE_RE = /^[A-Z0-9_-]{1,15}$/;
const enc = new TextEncoder(), dec = new TextDecoder();

// ---- SHA-256 (no crypto.subtle: a plain http:// page is not a secure context)
const K256 = new Uint32Array([
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,
  0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
  0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
  0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,
  0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
function sha256(data) {
  const n = data.length, total = ((n + 9 + 63) >> 6) << 6;
  const m = new Uint8Array(total);
  m.set(data); m[n] = 0x80;
  const dv = new DataView(m.buffer);
  dv.setUint32(total - 8, Math.floor(n / 0x20000000)); dv.setUint32(total - 4, (n << 3) >>> 0);
  const h = new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]);
  const w = new Uint32Array(64);
  const rotr = (x, k) => (x >>> k) | (x << (32 - k));
  for (let o = 0; o < total; o += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getUint32(o + i * 4);
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >>> 3);
      const s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >>> 10);
      w[i] = (w[i-16] + s0 + w[i-7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = h;
    for (let i = 0; i < 64; i++) {
      const t1 = (hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i]) >>> 0;
      const t2 = ((rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  const out = new Uint8Array(32), ov = new DataView(out.buffer);
  for (let i = 0; i < 8; i++) ov.setUint32(i * 4, h[i]);
  return out;
}

// ---- Python's json.dumps (default separators, ensure_ascii): the layout id hashes it
function pyJson(v) {
  if (typeof v === "number" || typeof v === "bigint") return String(v);
  if (typeof v === "string") {
    let s = '"';
    for (let i = 0; i < v.length; i++) {
      const c = v.charCodeAt(i);
      if (c === 0x22) s += '\\"'; else if (c === 0x5c) s += "\\\\";
      else if (c === 0x0a) s += "\\n"; else if (c === 0x0d) s += "\\r"; else if (c === 0x09) s += "\\t";
      else if (c === 0x08) s += "\\b"; else if (c === 0x0c) s += "\\f";
      else if (c < 0x20 || c > 0x7e) s += "\\u" + c.toString(16).padStart(4, "0");
      else s += v[i];
    }
    return s + '"';
  }
  if (Array.isArray(v)) return "[" + v.map(pyJson).join(", ") + "]";
  throw new Error("pyJson: " + typeof v);
}

// ---- comparisons as Python makes them (strings by code point; tuples item by item)
function cmpStr(a, b) {
  if (a === b) return 0;
  const x = Array.from(a), y = Array.from(b);
  for (let i = 0; i < Math.min(x.length, y.length); i++) {
    const p = x[i].codePointAt(0), q = y[i].codePointAt(0);
    if (p !== q) return p < q ? -1 : 1;
  }
  return x.length - y.length < 0 ? -1 : x.length === y.length ? 0 : 1;
}
function cmpKey(a, b) {
  for (let i = 0; i < Math.min(a.length, b.length); i++) {
    const p = a[i], q = b[i];
    const c = typeof p === "string" ? cmpStr(p, q) : p < q ? -1 : p > q ? 1 : 0;
    if (c) return c;
  }
  return a.length - b.length;
}
function cmpBytes(a, b) {
  for (let i = 0; i < Math.min(a.length, b.length); i++) if (a[i] !== b[i]) return a[i] - b[i];
  return a.length - b.length;
}
const sortedBy = (items, key) => items.map(x => [key(x), x]).sort((p, q) => cmpKey(p[0], q[0])).map(p => p[1]);
function rpartition(s, sep) { const i = s.lastIndexOf(sep); return i < 0 ? ["", s] : [s.slice(0, i), s.slice(i + 1)]; }
function partition(s, sep) { const i = s.indexOf(sep); return i < 0 ? [s, ""] : [s.slice(0, i), s.slice(i + 1)]; }

// ---- titles and prefixes (profiles.py, gfs_catalog.py)
function normalizeTitle(t) { t = String(t || "").trim().toUpperCase(); return TITLE_RE.test(t) ? t : null; }
function defaultPrefix(title) {
  let p = ("G" + title.toUpperCase().replace(/[^A-Z0-9]/g, "")).slice(0, 12);
  if (p.startsWith("GFS")) p = "T" + p.slice(1);
  if (/D\d{3}$|M\d{2}$/.test(p)) p = p.slice(0, -1) + "X";
  return p;
}
function legacyPrefix(title) {
  if (/^[A-Z]{4}\d{5}$/.test(title)) return "G" + title.slice(4);
  return ("G" + title.replace(/[^A-Z0-9]/g, "")).slice(0, 12);
}
function prefixProblem(p) {
  if (!/^[A-Z0-9]{1,12}$/.test(p || "")) return "a prefix is 1-12 of A-Z 0-9";
  if (p.startsWith("GFS")) return "prefixes starting with GFS are ghostldr's own";
  if (/D\d{3}$|M\d{2}$/.test(p)) return "a prefix must not end in D + 3 digits or M + 2 digits";
  return null;
}
const dataName = (p, k) => p + String(k).padStart(3, "0");
const indexName = p => p + "INDEX", saveName = p => p + "SAVE", metaName = p => p + "META";

// ---- param.sfo
function parseSfo(d) {
  const out = {};
  try {
    if (!d || d.length < 20 || d[0] !== 0 || d[1] !== 0x50 || d[2] !== 0x53 || d[3] !== 0x46) return out;
    const v = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const kt = v.getUint32(8, true), dt = v.getUint32(12, true), n = v.getUint32(16, true);
    for (let i = 0; i < Math.min(n, 256); i++) {
      const e = 20 + 16 * i, ko = v.getUint16(e, true), fmt = v.getUint16(e + 2, true), ln = v.getUint32(e + 4, true);
      const doff = v.getUint32(e + 12, true);
      let end = kt + ko; while (end < d.length && d[end]) end++;
      const key = dec.decode(d.subarray(kt + ko, end));
      const raw = d.subarray(dt + doff, dt + doff + ln);
      if (fmt === 0x0404 && ln >= 4) out[key] = new DataView(raw.buffer, raw.byteOffset, 4).getInt32(0, true);
      else { const z = raw.indexOf(0); out[key] = dec.decode(z < 0 ? raw : raw.subarray(0, z)); }
    }
  } catch (e) { /* an odd param.sfo: what was read so far */ }
  return out;
}

// ---- the layout (gfs_pack.py)
function dataBlocks(size) {
  const needed = Math.floor((size + 32767) / 32768);
  return Math.min(DATA_BLOCKS, Math.max(MIN_BLOCKS, needed + 48 + Math.max(64, Math.floor(needed / 16))));
}
function smallLimit(files) {
  let limit = SMALL;
  const sum = l => { let s = 0; for (const n of files.values()) if (n <= l) s += n; return s; };
  while (limit > 4096 && sum(limit) > SMALL_BUDGET) limit = Math.floor(limit / 2);
  return limit;
}
function heuristicKey(files) {
  const limit = smallLimit(files);
  return rel => {
    const [top, rest] = partition(rel, "/");
    const [d, name] = rpartition(rel, "/");
    if (rel === "eboot.bin") return [0, 0, "", rel];
    if (top === "sce_module" && rest) return [0, 1, d, name];
    if (top === "sce_sys" && rest) return [0, 2, d, name];
    if (files.get(rel) <= limit) return [0, 3, d, name];
    return [1, 0, d, name];
  };
}
function rulesKey(groups) {
  const rules = groups.map(([rx, rank]) => [new RegExp(rx), rank | 0]);
  const last = Math.max(0, ...rules.map(r => r[1])) + 1;
  return rel => { const r = rules.find(([rx]) => rx.test(rel)); return [r ? r[1] : last, rel]; };
}
function fileOrder(files, pk) {
  const key = pk.groups ? rulesKey(pk.groups) : heuristicKey(files);
  const seen = new Set(), traced = [];
  for (const rel of pk.order || []) if (files.has(rel) && !seen.has(rel)) { seen.add(rel); traced.push(rel); }
  const late = pk.late || [], isLate = rel => late.some(p => rel.startsWith(p));
  const all = [...files.keys()];
  const boot = sortedBy(all.filter(p => key(p)[0] === 0 && !seen.has(p)), key);
  const placed = new Set([...boot, ...traced]);
  const rest = sortedBy(all.filter(p => !placed.has(p)), key);
  return {order: [...boot, ...traced.filter(p => !isLate(p)), ...traced.filter(isLate), ...rest], traced: traced.length};
}
function indexHeader(count, entries, total, namesOff, namesSize, prefix, title, layoutId, flags) {
  const h = new Uint8Array(HEADER_SIZE), v = new DataView(h.buffer);
  h.set(enc.encode("GFS1"));
  v.setUint32(4, 2, true); v.setUint32(8, count, true); v.setUint32(12, entries, true);
  v.setBigUint64(16, BigInt(CAP), true); v.setBigUint64(24, BigInt(total), true);
  v.setUint32(32, namesOff, true); v.setUint32(36, namesSize, true);
  h.set(enc.encode(prefix).subarray(0, 16), 40);
  v.setUint32(56, HEADER_SIZE, true); v.setUint32(60, flags, true);
  h.set(enc.encode(title).subarray(0, 16), 64);
  v.setBigUint64(80, BigInt.asUintN(64, BigInt(layoutId)), true);
  return h;
}
// files: Map rel -> size; dirs: Set of rel dirs ("" is added); pk: the profile's "pack"
function pack(files, dirs, title, prefix, pk) {
  pk = pk || {};
  if (pk.exclude) {
    const ex = new RegExp("^(?:" + pk.exclude + ")");
    files = new Map([...files].filter(([rel]) => !ex.test(rel)));
  }
  if (!files.has("eboot.bin")) throw new Error("no eboot.bin: choose the game's app0 folder");
  const allDirs = new Set([""]);
  for (const d of dirs || []) allDirs.add(d);
  for (const rel of files.keys()) { let d = rel; while (d.includes("/")) { d = rpartition(d, "/")[0]; allDirs.add(d); } }
  const {order, traced} = fileOrder(files, pk);
  let offset = 0;
  const layout = order.map(rel => { const e = [rel, offset, files.get(rel)]; offset += files.get(rel); return e; });
  const total = offset, count = Math.ceil(total / CAP);
  if (count > MAX_CONTAINERS) throw new Error(`too big: ${(total / GIB).toFixed(1)} GiB needs ${count} containers (at most ${MAX_CONTAINERS})`);
  const containers = [];
  for (let i = 0; i < count; i++) {
    const size = Math.min(CAP, total - i * CAP);
    containers.push({dir: dataName(prefix, i), blocks: dataBlocks(size), size, pieces: []});
  }
  for (const [rel, off, size] of layout) {
    let pos = off;
    const end = off + size;
    while (pos < end) {
      const k = Math.floor(pos / CAP), take = Math.min(end, (k + 1) * CAP) - pos;
      containers[k].pieces.push([rel, pos - off, take]);
      pos += take;
    }
  }
  const digest = sha256(enc.encode(pyJson([title, prefix, CAP, containers.map(c => c.pieces)])));
  const layoutId = new DataView(digest.buffer).getBigUint64(0, true);
  const entries = layout.map(([rel, off, size]) => [enc.encode(rel), off, size, 0]);
  for (const d of allDirs) entries.push([enc.encode(d), 0, 0, FLAG_DIR]);
  entries.sort((a, b) => cmpBytes(a[0], b[0]));
  let namesSize = 0;
  for (const e of entries) namesSize += e[0].length + 1;
  const namesOff = HEADER_SIZE + 24 * entries.length;
  const index = new Uint8Array(namesOff + namesSize), iv = new DataView(index.buffer);
  index.set(indexHeader(count, entries.length, total, namesOff, namesSize, prefix, title, layoutId, 0));
  let np = 0;
  entries.forEach(([name, off, size, flags], i) => {
    const o = HEADER_SIZE + 24 * i;
    iv.setBigUint64(o, BigInt(off), true); iv.setBigUint64(o + 8, BigInt(size), true);
    iv.setUint32(o + 16, np, true); iv.setUint32(o + 20, flags, true);
    index.set(name, namesOff + np); np += name.length + 1;
  });
  const indexBlocks = Math.max(512, Math.floor((4 * index.length + 32767) / 32768));
  return {title, prefix, layoutId, total, entries: entries.length, indexBlocks, containers, index, traced,
          files: files.size, dirs: allDirs.size};
}

// ---- index headers and the catalogue (gfs_install.py parse_header, gfs_catalog.py)
const str0 = b => { const z = b.indexOf(0); return dec.decode(z < 0 ? b : b.subarray(0, z)); };
function parseHeader(name, d) {
  if (!d || d.length < 64 || str0(d.subarray(0, 4)) !== "GFS1") return null;
  const v = new DataView(d.buffer, d.byteOffset, d.byteLength);
  const version = v.getUint32(4, true), containers = v.getUint32(8, true), entries = v.getUint32(12, true);
  const total = Number(v.getBigUint64(24, true)), prefix = str0(d.subarray(40, 56));
  if (version >= 2 && d.length >= 88)
    return {name, version, title: str0(d.subarray(64, 80)), layoutId: v.getBigUint64(80, true), prefix, containers,
            entries, total, incomplete: !!(v.getUint32(60, true) & FLAG_INCOMPLETE)};
  return {name, version, title: "", layoutId: 0n, prefix, containers, entries, total, incomplete: false};
}
function sameData(have, nw) {
  if (have.version >= 2) return have.layoutId === nw.layoutId;
  return have.prefix === nw.prefix && have.containers === nw.containers && have.entries === nw.entries && have.total === nw.total;
}
function parseCatalogue(d) {
  if (!d || d.length < 64 || str0(d.subarray(0, 4)) !== "GFSC") return null;
  const v = new DataView(d.buffer, d.byteOffset, d.byteLength);
  const hsize = v.getUint32(8, true), esize = v.getUint32(12, true), count = v.getUint32(16, true);
  const flags = v.getUint32(20, true), generation = v.getBigUint64(24, true);
  if (hsize < 64 || esize < 192 || count > 4096 || d.length < hsize + count * esize) return null;
  const entries = [];
  for (let i = 0; i < count; i++) {
    const o = hsize + i * esize, s = (a, n) => str0(d.subarray(o + a, o + a + n));
    entries.push({title: s(0, 16), prefix: s(16, 16), index: s(32, 32), name: s(64, 64), version: s(128, 16),
                  layoutId: v.getBigUint64(o + 0x90, true), total: Number(v.getBigUint64(o + 0x98, true)),
                  containers: v.getUint32(o + 0xA0, true), flags: v.getUint32(o + 0xA4, true),
                  updated: Number(v.getBigUint64(o + 0xA8, true)), extra: d.slice(o + 192, o + esize)});
  }
  return {entries, generation, exists: !(flags & SYNTHESIZED)};
}
function cutUtf8(text, size) {
  let raw = enc.encode(text || "").subarray(0, size - 1);
  while (raw.length && (raw[raw.length - 1] & 0xC0) === 0x80) raw = raw.subarray(0, raw.length - 1);
  if (raw.length && raw[raw.length - 1] >= 0xC0) raw = raw.subarray(0, raw.length - 1);
  // keep a whole last sequence: drop a lead byte whose sequence was cut
  return raw;
}
function buildCatalogue(cat) {
  if (cat.entries.length > MAX_TITLES) throw new Error(`at most ${MAX_TITLES} titles`);
  const entries = [...cat.entries].sort((a, b) => cmpStr(a.title, b.title));
  const esize = 192 + Math.max(0, ...entries.map(e => (e.extra || []).length));
  const out = new Uint8Array(64 + esize * entries.length), v = new DataView(out.buffer);
  out.set(enc.encode("GFSC"));
  v.setUint32(4, 1, true); v.setUint32(8, 64, true); v.setUint32(12, esize, true);
  v.setUint32(16, entries.length, true); v.setUint32(20, 0, true); v.setBigUint64(24, BigInt(cat.generation), true);
  entries.forEach((e, i) => {
    const o = 64 + i * esize;
    out.set(cutUtf8(e.title, 16), o); out.set(cutUtf8(e.prefix, 16), o + 16); out.set(cutUtf8(e.index, 32), o + 32);
    out.set(cutUtf8(e.name, 64), o + 64); out.set(cutUtf8(e.version, 16), o + 128);
    v.setBigUint64(o + 0x90, BigInt.asUintN(64, BigInt(e.layoutId || 0)), true);
    v.setBigUint64(o + 0x98, BigInt(e.total || 0), true);
    v.setUint32(o + 0xA0, e.containers || 0, true); v.setUint32(o + 0xA4, e.flags || 0, true);
    v.setBigUint64(o + 0xA8, BigInt(e.updated || 0), true);
    if (e.extra && e.extra.length) out.set(e.extra, o + 192);
  });
  return out;
}
const catFind = (cat, t) => cat.entries.find(e => e.title === t) || null;
const catByPrefix = (cat, p) => cat.entries.find(e => e.prefix === p) || null;
function catPut(cat, e) { cat.entries = cat.entries.filter(x => x.title !== e.title).concat([e]); }
function catDrop(cat, t) { cat.entries = cat.entries.filter(x => x.title !== t); }
function describeEntry(e) {
  return `${e.title || "(untitled)"}${e.name ? " " + e.name : ""}${e.version ? " " + e.version : ""}: ${e.containers} containers ` +
         `${e.prefix}000.. (${(e.total / GIB).toFixed(1)} GiB), index ${e.index}${e.flags & INCOMPLETE ? ", INCOMPLETE" : ""}`;
}
const now = () => Math.floor(Date.now() / 1000);
function tagBytes(layoutId, k, size) {
  const t = new Uint8Array(16), v = new DataView(t.buffer);
  v.setBigUint64(0, BigInt.asUintN(64, BigInt(layoutId)), true); v.setUint32(8, k, true); v.setUint32(12, size, true);
  return t.subarray(0, 16);
}
const bytesEq = (a, b) => !!a && !!b && a.length === b.length && a.every((x, i) => x === b[i]);

// ---- the install (gfs_install.py): survey, plan, execute, over rx (the console's API)
class InstallError extends Error {}

async function readCatalogue(rx, knownPrefixes, log) {
  const cat = parseCatalogue(await rx.readFile(CATALOG, CATALOG_FILE, 1 << 20)) || {entries: [], generation: 0n, exists: false};
  const legacy = [];
  for (const n of LEGACY_INDEXES) { const h = parseHeader(n, await rx.readFile(n, INDEX_FILE, 128)); if (h) legacy.push(h); }
  const adopted = [];
  for (const h of legacy) {
    if (cat.entries.some(e => e.index === h.name)) continue;
    const title = h.title || (knownPrefixes || {})[h.prefix] || "";
    if (!title || catFind(cat, title) || catByPrefix(cat, h.prefix)) {
      log(`${h.name}: an install that is not in the catalogue and cannot be added (title unknown or taken)`);
      continue;
    }
    catPut(cat, entryFromHeader(h.name, h, title));
    adopted.push(title);
  }
  return {cat, legacy, adopted};
}
function entryFromHeader(index, h, title) {
  return {title: title || h.title, prefix: h.prefix, index, name: "", version: "", layoutId: h.layoutId, total: h.total,
          containers: h.containers, flags: LEGACY_INDEX | (h.incomplete ? INCOMPLETE : 0), updated: 0, extra: new Uint8Array(0)};
}
// A title installed with the old default prefix (G + digits) keeps it: its progress is there
function relabel(p, prefix) {
  p.prefix = prefix;
  p.containers.forEach((c, k) => { c.dir = dataName(prefix, k); });
  p.index = p.index.slice();
  p.index.fill(0, 40, 56); p.index.set(enc.encode(prefix), 40);
}
function newInstall(p) {
  return {name: indexName(p.prefix), version: 2, title: p.title, layoutId: p.layoutId, prefix: p.prefix,
          containers: p.containers.length, entries: p.entries, total: p.total, incomplete: false};
}
async function survey(rx, p, info, force, knownPrefixes, log) {
  const {cat, legacy, adopted} = await readCatalogue(rx, knownPrefixes, log);
  const old = catFind(cat, p.title);
  if (old && old.prefix !== p.prefix && p.prefix === defaultPrefix(p.title) && old.prefix === legacyPrefix(p.title)) {
    log(`${p.title} is installed with the prefix ${old.prefix} (the default before 2026-10-28): it keeps it, and its progress ${saveName(old.prefix)}`);
    relabel(p, old.prefix);
  }
  const nw = newInstall(p);
  const st = {cat, legacy, adopted, entry: null, current: null, present: new Set(), tagged: new Set(), nw};
  const other = catByPrefix(cat, p.prefix);
  if (other && other.title !== p.title)
    throw new InstallError(`the prefix ${p.prefix} belongs to ${other.title} ${other.name}: this game can't use it`);
  st.entry = catFind(cat, p.title);
  if (st.entry && st.entry.prefix !== p.prefix)
    throw new InstallError(`${p.title} is installed with the prefix ${st.entry.prefix}, this install uses ${p.prefix}: remove it first`);
  if (st.entry) st.current = parseHeader(st.entry.index, await rx.readFile(st.entry.index, INDEX_FILE, 128));
  const cur = st.current;
  if (cur && sameData(cur, nw) && !cur.incomplete && !force) return st;
  const jobs = p.containers;
  if (!jobs.length) return st;
  const first = (await rx.probe([jobs[0].dir]))[0];
  if (st.entry || first.exists) {
    const rest = jobs.length > 1 ? await rx.probe(jobs.slice(1).map(j => j.dir)) : [];
    [first, ...rest].forEach((r, k) => {
      if (r.exists) st.present.add(k);
      if (bytesEq(r.tag, tagBytes(nw.layoutId, k, jobs[k].size))) st.tagged.add(k);
    });
  }
  return st;
}
function entryFor(p, info, incomplete) {
  return {title: p.title, prefix: p.prefix, index: indexName(p.prefix), name: info.name || "", version: info.version || "",
          layoutId: p.layoutId, total: p.total, containers: p.containers.length, flags: incomplete ? INCOMPLETE : 0,
          updated: now(), extra: new Uint8Array(0)};
}
function sameEntry(a, b) {
  if (!a) return false;
  for (const k of ["title", "prefix", "index", "name", "version", "layoutId", "total", "containers", "flags"])
    if (a[k] !== b[k]) return false;
  return true;
}
function plan(st, p, info, force) {
  const steps = [], cur = st.current, entry = st.entry, nw = st.nw;
  if (cur && sameData(cur, nw) && !cur.incomplete && !force) {
    if (entry.flags & LEGACY_INDEX && entry.index === LEGACY_INDEXES[1]) {
      steps.push(["index"], ["catalog", entryFor(p, info, false)], ["delete", LEGACY_INDEXES[1]]);
    } else {
      const done = Object.assign(entryFor(p, info, false), {index: entry.index, flags: entry.flags & LEGACY_INDEX});
      if (!sameEntry(entry, done) || st.adopted.length) steps.push(["catalog", done]);
    }
    return steps;
  }
  const resume = !!(cur && sameData(cur, nw) && cur.incomplete && !force);
  steps.push(["catalog", entryFor(p, info, true)], ["stub"]);
  for (const h of st.legacy) if (h.prefix === p.prefix && (h.title === p.title || h.title === "")) steps.push(["delete", h.name]);
  for (let k = 0; k < p.containers.length; k++) if (force || !(resume && st.tagged.has(k))) steps.push(["put", k]);
  steps.push(["sweep"], ["index"], ["catalog", entryFor(p, info, false)]);
  return steps;
}
function spaceNeeded(st, steps, p) {
  let need = 0;
  for (const s of steps) if (s[0] === "put" && !st.present.has(s[1])) need += GIB;
  const writesIndex = steps.some(s => s[0] === "stub" || s[0] === "index");
  if (writesIndex && !(st.entry && st.entry.index === indexName(p.prefix))) need += CATALOG_BLOCKS * 32768;
  if (steps.some(s => s[0] === "catalog") && !st.cat.exists) need += CATALOG_BLOCKS * 32768;
  return need;
}
async function writeCatalogue(rx, cat) {
  cat.generation = BigInt(cat.generation) + 1n;
  const data = buildCatalogue(cat);
  let rc = await rx.write(CATALOG, CATALOG_FILE, data);
  if (rc === NOT_FOUND) rc = await rx.put(CATALOG, CATALOG_FILE, CATALOG_BLOCKS, data);
  if (rc) throw new InstallError(`${CATALOG}: ${errText(rc)}`);
  cat.exists = true;
}
async function deleteOwned(rx, prefix, ks, layoutIds, untaggedBelow) {
  let gone = 0, missing = 0;
  const left = [];
  for (const k of ks) {
    const name = dataName(prefix, k);
    const r = (await rx.probe([name]))[0];
    if (!r.exists) { if (++missing >= 4) break; continue; }
    missing = 0;
    let mine = false;
    if (r.tag && r.tag.length >= 12) {
      const v = new DataView(r.tag.buffer, r.tag.byteOffset, r.tag.byteLength);
      mine = layoutIds.has(v.getBigUint64(0, true)) && v.getUint32(8, true) === k;
    } else if (k < (untaggedBelow || 0)) mine = true;
    if (mine) {
      const rc = await rx.del(name);
      if (rc && rc !== NOT_FOUND) throw new InstallError(`delete ${name}: ${errText(rc)}`);
      if (!rc) gone++;
    } else left.push(name);
  }
  return {gone, left};
}
function errText(rc) {
  if (typeof rc === "string") return rc;
  const u = rc >>> 0;
  return "0x" + u.toString(16) + (SAVE_ERRORS[u] ? ` (${SAVE_ERRORS[u]})` : "");
}
// ui: {log(msg), step(text), progress(done, total), container(k, n, job)}; src: {chunks(job, from) -> async iterator}
async function execute(rx, steps, st, p, src, ui, signal) {
  const cat = st.cat, idx = indexName(p.prefix), puts = steps.filter(s => s[0] === "put").map(s => s[1]);
  const total = puts.reduce((a, k) => a + p.containers[k].size, 0);
  let sent = 0;
  for (const s of steps) {
    if (signal && signal.stopped) throw new InstallError("paused");
    if (s[0] === "catalog") {
      catPut(cat, s[1]); await writeCatalogue(rx, cat);
      ui.log(`${CATALOG}: ${describeEntry(s[1])}`);
    } else if (s[0] === "delete") {
      ui.log(`deleted ${s[1]}: ${errText(await rx.del(s[1]))}`);
    } else if (s[0] === "stub") {
      const stub = indexHeader(p.containers.length, p.entries, p.total, 0, 0, p.prefix, p.title, p.layoutId, FLAG_INCOMPLETE);
      const rc = await rx.put(idx, INDEX_FILE, p.indexBlocks, stub);
      if (rc) throw new InstallError(`${idx} (marking the install incomplete): ${errText(rc)}`);
    } else if (s[0] === "put") {
      const k = s[1], job = p.containers[k];
      ui.step(`Sending ${job.dir} (${puts.indexOf(k) + 1} of ${puts.length})`);
      const base = sent;
      const r = await rx.data(job, src, done => ui.progress(base + done, total), signal);
      if (r.status) throw new InstallError(`${job.dir}: ${errText(r.status)}`);
      const rc = await rx.write(job.dir, TAG, tagBytes(p.layoutId, k, job.size));
      if (rc) throw new InstallError(`${job.dir}: writing its tag: ${errText(rc)}`);
      sent += job.size;
      ui.progress(sent, total);
      ui.log(`${job.dir}: ${(job.size / 1048576).toFixed(0)} MiB stored`);
    } else if (s[0] === "sweep") {
      const ids = new Set([p.layoutId]);
      if (st.current) ids.add(st.current.layoutId);
      if (st.entry) ids.add(st.entry.layoutId);
      const ks = []; for (let k = p.containers.length; k < MAX_CONTAINERS; k++) ks.push(k);
      const {gone, left} = await deleteOwned(rx, p.prefix, ks, ids, 0);
      if (gone) ui.log(`deleted ${gone} leftover ${p.prefix} containers`);
      if (left.length) ui.log(`left ${left.join(", ")}: not this game's`);
    } else if (s[0] === "index") {
      const rc = await rx.put(idx, INDEX_FILE, p.indexBlocks, p.index);
      if (rc) throw new InstallError(`${idx}: ${errText(rc)}`);
      ui.log(`${idx} written`);
    }
  }
}
function planRemove(entry, legacy) {
  const steps = [["catalog", Object.assign({}, entry, {flags: entry.flags | INCOMPLETE, updated: now()})], ["delete", entry.index]];
  for (const h of legacy) if (h.prefix === entry.prefix && h.name !== entry.index) steps.push(["delete", h.name]);
  steps.push(["delete_data", entry], ["drop", entry.title]);
  return steps;
}
async function remove(rx, title, knownPrefixes, log) {
  const {cat, legacy} = await readCatalogue(rx, knownPrefixes, log);
  const e = catFind(cat, title);
  if (!e) throw new InstallError(`${title} is not installed`);
  for (const s of planRemove(e, legacy)) {
    if (s[0] === "catalog") { catPut(cat, s[1]); await writeCatalogue(rx, cat); }
    else if (s[0] === "delete") log(`deleted ${s[1]}: ${errText(await rx.del(s[1]))}`);
    else if (s[0] === "delete_data") {
      const ks = []; for (let k = 0; k < MAX_CONTAINERS; k++) ks.push(k);
      const {gone, left} = await deleteOwned(rx, e.prefix, ks, new Set([e.layoutId]), e.containers);
      log(`deleted ${gone} data containers ${e.prefix}000..` + (left.length ? `; left ${left.join(", ")} (not this game's)` : ""));
    } else if (s[0] === "drop") { catDrop(cat, s[1]); await writeCatalogue(rx, cat); log(`${title} removed (its saves stay)`); }
  }
}

// ---- reading a .zip (central directory, zip64; stored and deflated entries)
async function zipEntries(file) {
  const size = file.size, tail = Math.min(size, 65536 + 22);
  const t = new Uint8Array(await file.slice(size - tail, size).arrayBuffer()), tv = new DataView(t.buffer);
  let e = -1;
  for (let i = t.length - 22; i >= 0; i--) if (tv.getUint32(i, true) === 0x06054b50) { e = i; break; }
  if (e < 0) throw new Error("not a zip file (no end of central directory)");
  let count = tv.getUint16(e + 10, true), cdSize = tv.getUint32(e + 12, true), cdOff = tv.getUint32(e + 16, true);
  if ((count === 0xFFFF || cdOff === 0xFFFFFFFF || cdSize === 0xFFFFFFFF) && e >= 20 && tv.getUint32(e - 20, true) === 0x07064b50) {
    const z64 = Number(tv.getBigUint64(e - 20 + 8, true));
    const r = new DataView(await file.slice(z64, z64 + 56).arrayBuffer());
    if (r.getUint32(0, true) !== 0x06064b50) throw new Error("damaged zip64 record");
    count = Number(r.getBigUint64(32, true)); cdSize = Number(r.getBigUint64(40, true)); cdOff = Number(r.getBigUint64(48, true));
  }
  const cd = new Uint8Array(await file.slice(cdOff, cdOff + cdSize).arrayBuffer()), v = new DataView(cd.buffer);
  const out = [];
  let o = 0;
  for (let i = 0; i < count && o + 46 <= cd.length; i++) {
    if (v.getUint32(o, true) !== 0x02014b50) throw new Error("damaged zip central directory");
    const flags = v.getUint16(o + 8, true), method = v.getUint16(o + 10, true);
    let csize = v.getUint32(o + 20, true), usize = v.getUint32(o + 24, true), lho = v.getUint32(o + 42, true);
    const nlen = v.getUint16(o + 28, true), xlen = v.getUint16(o + 30, true), clen = v.getUint16(o + 32, true);
    const name = dec.decode(cd.subarray(o + 46, o + 46 + nlen)).replace(/\\/g, "/");
    let x = o + 46 + nlen;
    const xend = x + xlen;
    while (x + 4 <= xend) {
      const id = v.getUint16(x, true), len = v.getUint16(x + 2, true);
      if (id === 1) {
        let q = x + 4;
        if (usize === 0xFFFFFFFF) { usize = Number(v.getBigUint64(q, true)); q += 8; }
        if (csize === 0xFFFFFFFF) { csize = Number(v.getBigUint64(q, true)); q += 8; }
        if (lho === 0xFFFFFFFF) { lho = Number(v.getBigUint64(q, true)); q += 8; }
      }
      x += 4 + len;
    }
    out.push({name, flags, method, csize, usize, lho, dir: name.endsWith("/")});
    o = xend + clen;
  }
  return out;
}

const JUNK = /(^|\/)(__MACOSX|\.DS_Store|\._[^/]*|Thumbs\.db|desktop\.ini)(\/|$)/i;
// ---- the game's files: a folder or a .zip
class Inflater {
  constructor(file, start, csize) { this.file = file; this.start = start; this.csize = csize; this.pos = 0; this.reader = null; this.pending = null; }
  async read(a, b) {
    if (!this.reader || a < this.pos) {
      if (this.reader) this.reader.cancel().catch(() => {});
      this.reader = this.file.slice(this.start, this.start + this.csize).stream().pipeThrough(new DecompressionStream("deflate-raw")).getReader();
      this.pos = 0; this.pending = null;
    }
    const out = new Uint8Array(b - a);
    while (this.pos < b) {
      let chunk = this.pending;
      this.pending = null;
      if (!chunk) { const r = await this.reader.read(); if (r.done) throw new Error("a file in the zip ended early"); chunk = r.value; }
      const cs = this.pos, ce = cs + chunk.length, s = Math.max(a, cs), e = Math.min(b, ce);
      if (s < e) out.set(chunk.subarray(s - cs, e - cs), s - a);
      if (ce > b) { this.pending = chunk.subarray(b - cs); this.pos = b; break; }
      this.pos = ce;
    }
    return out;
  }
}
function makeSource(kind, name, items, zipFile) {
  // items: [{path, size, file?, zip?, dir?}]
  const ebs = items.filter(x => !x.dir && /(^|\/)eboot\.bin$/.test(x.path) && !JUNK.test(x.path));
  if (!ebs.length) throw new Error("no eboot.bin in it: choose the game's app0 folder (the one with eboot.bin and sce_sys), or a .zip of it");
  ebs.sort((a, b) => a.path.split("/").length - b.path.split("/").length);
  const root = ebs[0].path.slice(0, ebs[0].path.length - "eboot.bin".length);
  const files = new Map(), dirs = new Set(), map = new Map();
  let skipped = 0, junk = 0;
  for (const x of items) {
    if (JUNK.test(x.path)) { if (!x.dir) junk++; continue; }
    if (!x.path.startsWith(root)) { if (!x.dir) skipped++; continue; }
    let rel = x.path.slice(root.length);
    if (x.dir) { rel = rel.replace(/\/+$/, ""); if (rel) dirs.add(rel); continue; }
    if (!files.has(rel)) { files.set(rel, x.size); map.set(rel, x); }
  }
  const inflaters = new Map(), starts = new Map();
  async function dataStart(z) {
    if (starts.has(z)) return starts.get(z);
    const h = new DataView(await zipFile.slice(z.lho, z.lho + 30).arrayBuffer());
    if (h.getUint32(0, true) !== 0x04034b50) throw new Error("damaged zip: " + z.name);
    const s = z.lho + 30 + h.getUint16(26, true) + h.getUint16(28, true);
    starts.set(z, s);
    return s;
  }
  async function fileRange(rel, a, b) {
    const x = map.get(rel);
    if (x.file) return x.file.slice(a, b);
    const z = x.zip, s = await dataStart(z);
    if (z.method === 0) return zipFile.slice(s + a, s + b);
    if (!inflaters.has(rel)) inflaters.set(rel, new Inflater(zipFile, s, z.csize));
    return new Blob([await inflaters.get(rel).read(a, b)]);
  }
  const pieceRanges = (job, off, len, fn) => {
    let pos = 0;
    for (const [rel, foff, take] of job.pieces) {
      const ps = pos, pe = pos + take;
      pos = pe;
      if (pe <= off || ps >= off + len) continue;
      fn(rel, Math.max(off, ps) - ps + foff, Math.min(off + len, pe) - ps + foff);
    }
  };
  return {
    kind, name, files, dirs, skipped, junk, root,
    async bytes(rel) { return files.has(rel) ? new Uint8Array(await (await fileRange(rel, 0, files.get(rel))).arrayBuffer()) : null; },
    async blob(rel) { return files.has(rel) ? fileRange(rel, 0, files.get(rel)) : null; },
    streamed(job, off) { let s = false; pieceRanges(job, off, job.size - off, rel => { const x = map.get(rel); if (x.zip && x.zip.method !== 0) s = true; }); return s; },
    async range(job, off, len) {
      const parts = [], todo = [];
      pieceRanges(job, off, len, (rel, a, b) => todo.push([rel, a, b]));
      for (const [rel, a, b] of todo) parts.push(await fileRange(rel, a, b));
      return new Blob(parts);
    },
  };
}
async function fromZip(file) {
  const entries = await zipEntries(file);
  for (const z of entries) {
    if (z.dir) continue;
    if (z.flags & 1) throw new Error(z.name + ": encrypted zips can't be read");
    if (z.method !== 0 && z.method !== 8) throw new Error(z.name + `: compression method ${z.method} isn't supported: zip it with normal (deflate) or no compression`);
    if (z.method === 8 && typeof DecompressionStream === "undefined") throw new Error("this browser can't unpack zips: use a zip made without compression (store), or another browser");
  }
  return makeSource("zip", file.name, entries.map(z => ({path: z.name, size: z.usize, zip: z, dir: z.dir})), file);
}

// ---- the console's API (docs/manager.md "Install a game"): env gives
// fetch(url, opts), post(url, blob, progress(sent), signal) -> {status, json},
// token(), sleep(ms), log(msg, cls), status(msg, cls), fmtSize(n)
class HttpError extends Error { constructor(m, code) { super(m); this.code = code; } }
const hexOf = b => Array.from(b, x => x.toString(16).padStart(2, "0")).join("");
const unhex = s => s ? new Uint8Array(s.match(/../g).map(x => parseInt(x, 16))) : null;
const ZCHUNK = opts.zchunk || 32 * 1024 * 1024;     // bytes per request when the browser unpacks them
function makeRx(env) {
  async function api(method, path, params, body, raw) {
    const q = params ? "?" + Object.entries(params).map(([k, v]) => encodeURIComponent(k) + "=" + encodeURIComponent(v)).join("&") : "";
    for (let attempt = 0; ; attempt++) {
      let r;
      try {
        const tok = env.token();
        r = await env.fetch("/api/install/" + path + q, {method, body, headers: tok ? {"X-Ghostldr-Token": tok} : {}, cache: "no-store"});
      } catch (e) {
        if (attempt >= 6) throw new HttpError("the console does not answer (" + e.message + ")", 0);
        await env.sleep(1000 * Math.min(8, 1 << attempt));
        continue;
      }
      if (raw && r.ok) return {status: +(r.headers.get("X-Gfs-Status") || 0), bytes: new Uint8Array(await r.arrayBuffer())};
      let j = {};
      try { j = await r.json(); } catch (e) { /* not JSON */ }
      if (r.status === 403 || r.status === 429) throw new HttpError(j.error || "the token is needed", r.status);
      if (!r.ok && j.rc === undefined) throw new HttpError(j.error || ("HTTP " + r.status), r.status);
      return j;
    }
  }
  return {
    api,
    async readFile(c, f, len, off) {
      const r = await api("GET", "read", {c, f, off: off || 0, len}, null, true);
      return r.bytes && r.status === 0 ? r.bytes : null;
    },
    async probe(names) {
      const out = [];
      for (let i = 0; i < names.length; i += 16) {
        const j = await api("POST", "probe", {c: names.slice(i, i + 16).join(",")});
        if (!j.ok) throw new InstallError(j.error || "probe failed");
        for (const x of j.containers) out.push({exists: !!x.exists, tag: unhex(x.tag)});
      }
      return out;
    },
    async write(c, f, bytes) { return (await api("POST", "write", {c, f}, bytes)).rc >>> 0; },
    async put(c, f, blocks, bytes) { return (await api("POST", "put", {c, f, blocks}, bytes)).rc >>> 0; },
    async del(c) { return (await api("POST", "delete", {c})).rc >>> 0; },
    async free(n) { const j = await api("POST", "free", {bytes: n}); return {status: j.rc >>> 0, have: j.have || 0}; },
    async data(job, src, progress, signal) {
      const lid = hexOf(tagBytes(job.lid, 0, 0).subarray(0, 8));
      let off = 0, stuck = 0;
      const se = (await api("GET", "info")).session;
      if (se && se.c === job.dir && se.size === job.size && se.lid === lid && se.written > 0 && se.written < job.size) {
        off = se.written;
        env.log(`${job.dir}: carrying on from ${env.fmtSize(off)}`);
      }
      while (true) {
        if (signal.stopped) throw new InstallError("paused");
        const len = src.streamed(job, off) ? Math.min(ZCHUNK, job.size - off) : job.size - off;
        const blob = await src.range(job, off, len);
        const r = await env.post(`/api/install/data?c=${job.dir}&blocks=${job.blocks}&size=${job.size}&off=${off}&lid=${lid}`,
                                 blob, n => progress(off + n), signal);
        if (r.json && r.json.ok && !r.json.rc) {
          off = r.json.written; stuck = 0;
          progress(off);
          if (r.json.done) return {status: 0};
          continue;
        }
        if (r.json && r.status === 409 && typeof r.json.written === "number") { off = r.json.written; continue; }
        if (r.json && r.json.rc) return {status: r.json.rc};
        if (r.status === 403) throw new InstallError((r.json && r.json.error) || "the token is needed");
        if (r.status >= 400) throw new InstallError(`${job.dir}: ` + ((r.json && r.json.error) || "HTTP " + r.status));
        if (signal.stopped) throw new InstallError("paused");
        // the connection dropped: ask the console how far it got
        if (++stuck > 12) throw new InstallError("the connection keeps dropping: check the network, then Install again to carry on");
        env.status(`Connection lost: carrying on (try ${stuck})…`, "warn");
        await env.sleep(Math.min(16000, 1000 << Math.min(stuck, 4)));
        let s2 = null;
        try { s2 = (await api("GET", "info")).session; } catch (e) { continue; }
        off = s2 && s2.c === job.dir && s2.size === job.size && s2.lid === lid ? s2.written : 0;
        env.log(`${job.dir}: connection lost, carrying on from ${env.fmtSize(off)}`, "warn");
      }
    },
  };
}

return {CAP, GIB, NOT_FOUND, MAX_CONTAINERS, CATALOG, INDEX_FILE, TAG, INCOMPLETE, LEGACY_INDEX, InstallError,
        sha256, pyJson, normalizeTitle, defaultPrefix, legacyPrefix, prefixProblem, dataName, indexName, saveName,
        metaName, parseSfo, dataBlocks, fileOrder, pack, indexHeader, parseHeader, parseCatalogue, buildCatalogue,
        describeEntry, tagBytes, survey, plan, spaceNeeded, execute, remove, errText, zipEntries, cmpStr,
        makeSource, fromZip, makeRx, HttpError, readCatalogue};
}
const GFS = makeGFS();
/*GFS-END*/
/*PROFILES-BEGIN: generated from ghostldr/data/titles/*.json by ghostldr/tools/netinstall_page.py */
const PROFILES = {"APOL00004":{"name":"Apollo Save Tool"},"CUSA00900":{"name":"Bloodborne","version":"01.09","pack":{"prefix":"G00900"}},"CUSA11456":{"name":"Ghost of Tsushima","version":"02.18","pack":{"prefix":"GHOST","exclude":"^(eboot\\.bin\\.xref|cache/psarc/lang_(french|latino|brazilian|japanese)_audio\\.psarc)$","groups":[["^(sce_sys|sce_module|ui)/|^[^/]+$",0],["^cache/(?:[iI][zZ][uU][^/]*|[^/]*[_.#&-][iI][zZ][uU](?:[_.#&-][^/]*)?)$",12],["^cache/(?:[tT][oO][yY][^/]*|[^/]*[_.#&-][tT][oO][yY](?:[_.#&-][^/]*)?)$",13],["^cache/(?:[kK][aA][mM][^/]*|[^/]*[_.#&-][kK][aA][mM](?:[_.#&-][^/]*)?)$",14],["^cache/(?:[iI][kK][iI][^/]*|[^/]*[_.#&-][iI][kK][iI](?:[_.#&-][^/]*)?)$",15],["^cache/[^/]+$",11],["^cache/psarc/",20],["^cache/meshes/",30],["^cache/bitmaps/",40],["^cache/",50],["^movies/",60],["",70]],"late":["cache/psarc/"],"order":["cache/t11456.dat","cache/playgo.pgm","ui/vshell/content_ps4/save_icon.png","cache/all_shaders.xpps","cache/all_shaders.texmeshman","cache/lang_english_text.xpps","cache/lang_english_audio.xpps","cache/pulse.sprig.xpps","cache/pulse.sprig.texmeshman","cache/bitmaps/fxt_or_023_snowflakes.psd.0.sps","cache/bitmaps/fxt_or_029_bloodstring.psd.0.sps","cache/psarc/music_a.psarc","cache/psarc/music_b.psarc","cache/psarc/music_c.psarc","cache/psarc/music_d.psarc","cache/psarc/music_e.psarc","cache/psarc/lang_english_audio.psarc","cache/init_sound.sprig.xpps","cache/common_sound.sprig.xpps","cache/single_player_sound.sprig.xpps","movies/cutscene/splash_america.bsf","cache/load_menu_catalog.log","cache/game.sprig.packman","cache/game.sprig.texmeshman","cache/game.sprig.xpps","cache/downloaded.sprig.xpps","cache/core_common.sprig.xpps","cache/bitmaps/mask_wipe_radial_01.msac.d.0.sps","cache/bitmaps/meter_radial_04_fill_white.msac.d.0.sps","cache/bitmaps/&paper_pj_shoji_001_msac_n&g&h.1c8ea51aaf5b12eb34bf3ff6138e46c9.sps","cache/bitmaps/compass_icon_follow.msac.d.0.sps","cache/bitmaps/custom_ap_loot_fire_sword_oils_001.msac.d.0.sps","cache/bitmaps/custom_cv_clothes_kimono_folded_a_001.msac.d.0.sps","cache/bitmaps/custom_firework_single_a_ap.msac.d.0.sps","cache/bitmaps/custom_geom_bandit_katana_rank2.msac.d.0.sps","cache/bitmaps/custom_geom_mongol_sabre_rank2.msac.d.0.sps","cache/bitmaps/custom_gm_fish_drying_001.msac.d.0.sps","cache/bitmaps/custom_ja_gift_furoshiki_001a.msac.d.0.sps","cache/bitmaps/custom_ja_loot_furoshiki_001a.msac.d.0.sps","cache/bitmaps/custom_ja_loot_furoshiki_001b.msac.d.0.sps","cache/bitmaps/custom_jjl_tealeaves_001.msac.d.0.sps","cache/bitmaps/custom_ll_windchimes_iron_002.msac.d.0.sps","cache/bitmaps/custom_loot_leather_medium_001.msac.d.0.sps","cache/bitmaps/custom_ma_mongol_meatrack_001.msac.d.0.sps","cache/bitmaps/custom_pj_scroll_gilded_001.msac.d.0.sps","cache/bitmaps/custom_zc_tea_001.msac.d.0.sps","cache/bitmaps/dt_decal_blood_wipe.msac.d.0.sps","cache/bitmaps/fabric_banner_remaster_horsearmor_01.msac.d.0.sps","cache/bitmaps/fabric_dh_rope_002b.msac.d.0.sps","cache/bitmaps/foliage_ap_bamboobark_001.msac.d.0.sps","cache/bitmaps/foliage_ll_ricebunch_001.msac.d.0.sps","cache/bitmaps/fxt_cl_001_cumulus.psd.0.sps","cache/bitmaps/fxt_cl_002_cumulus_fuzzy.psd.0.sps","cache/bitmaps/fxt_cl_004_wispy.psd.0.sps","cache/bitmaps/fxt_cl_005_small.psd.0.sps","cache/bitmaps/fxt_cl_006_small_fuzzy.psd.0.sps","cache/bitmaps/fxt_de_003_ashbits.dds.0.sps","cache/bitmaps/fxt_de_005_smallbits.dds.0.sps","cache/bitmaps/fxt_de_011_woodsplinters.psd.0.sps","cache/bitmaps/fxt_di_001_shockwave_n.psd.0.sps","cache/bitmaps/fxt_di_008_heatshimmer_n.psd.0.sps","cache/bitmaps/fxt_di_010_waterripple_n.psd.0.sps","cache/bitmaps/fxt_di_015_pinch_n.psd.0.sps","cache/bitmaps/fxt_di_016_waterripple_n.psd.0.sps","cache/bitmaps/fxt_di_017_shimmer_n.psd.0.sps","cache/bitmaps/fxt_di_018_charmove_n.psd.0.sps","cache/bitmaps/fxt_di_019_bulge_n.psd.0.sps","cache/bitmaps/fxt_di_021_grasswind_n.psd.0.sps","cache/bitmaps/fxt_di_022_wind_n.psd.0.sps","cache/bitmaps/fxt_di_023_wake_n.psd.0.sps","cache/bitmaps/fxt_di_024_wake_n.psd.0.sps","cache/bitmaps/fxt_di_025_shock_n.psd.0.sps","cache/bitmaps/fxt_en_003_shockwave.psd.0.sps","cache/bitmaps/fxt_en_009_glowflare.psd.0.sps","cache/bitmaps/fxt_en_015_wavetile.psd.0.sps","cache/bitmaps/fxt_fi_011_firetosmoke.psd.0.sps","cache/bitmaps/fxt_fi_011_firetosmoke_n.psd.0.sps","cache/bitmaps/fxt_fi_011_white_firetosmoke.psd.0.sps","cache/bitmaps/fxt_fi_013_licks.psd.0.sps","cache/bitmaps/fxt_fi_013_licks_n.psd.0.sps","cache/bitmaps/fxt_fi_016_firetosmoke.psd.0.sps","cache/bitmaps/fxt_fi_016_firetosmoke_n.psd.0.sps","cache/bitmaps/fxt_fi_019_small_loop_04.psd.0.sps","cache/bitmaps/fxt_fi_019_small_loop_04_n.psd.0.sps","cache/bitmaps/fxt_fi_019_white_small_loop_04.psd.0.sps","cache/bitmaps/fxt_fi_020_small_loop_n.psd.0.sps","cache/bitmaps/fxt_fi_020_white_small_loop.psd.0.sps","cache/bitmaps/fxt_fi_021_med_loop_n.psd.0.sps","cache/bitmaps/fxt_fi_022_lrg_loop_n.psd.0.sps","cache/bitmaps/fxt_fi_023_candle_lrg_loop.psd.0.sps","cache/bitmaps/fxt_fi_023_candle_lrg_loop_n.psd.0.sps","cache/bitmaps/fxt_fi_024_candle_loop.psd.0.sps","cache/bitmaps/fxt_fi_025_white_small_loop_lgd.psd.0.sps","cache/bitmaps/fxt_fi_lgd_white_flame_licks.psd.0.sps","cache/bitmaps/fxt_fm_001_swirl_n.psd.0.sps","cache/bitmaps/fxt_gn_001_swordswipe.psd.0.sps","cache/bitmaps/fxt_gn_002_ray.psd.0.sps","cache/bitmaps/fxt_gn_004_shock.psd.0.sps","cache/bitmaps/fxt_gn_006_elements.psd.0.sps","cache/bitmaps/fxt_gn_008_flare.psd.0.sps","cache/bitmaps/fxt_gn_009_arrow_splinter.msac.d.0.sps","cache/bitmaps/fxt_gn_009_arrow_splinter.msac.n.0.sps","cache/bitmaps/fxt_gn_010_bamboo.msac.d.0.sps","cache/bitmaps/fxt_fi_020_small_loop.psd.0.sps","cache/bitmaps/fxt_fi_021_med_loop.psd.0.sps","cache/bitmaps/fxt_gn_011_bamboo_leaves.msac.d.0.sps","cache/bitmaps/fxt_gn_012_rock.msac.d.0.sps","cache/bitmaps/fxt_gn_012_rock.msac.n.0.sps","cache/bitmaps/fxt_gn_015_wood.msac.d.0.sps","cache/bitmaps/fxt_gn_015_wood.msac.n.0.sps","cache/bitmaps/fxt_gn_017_flare.psd.0.sps","cache/bitmaps/fxt_gn_020_flare_red.psd.0.sps","cache/bitmaps/fxt_gn_021_flare_blue.psd.0.sps","cache/bitmaps/fxt_fi_021_white_med_loop.psd.0.sps","cache/bitmaps/fxt_gn_022_flare_round.psd.0.sps","cache/bitmaps/fxt_gn_024_paper.psd.0.sps","cache/bitmaps/fxt_gn_025_kanji.psd.0.sps","cache/bitmaps/fxt_gn_026_smokebomb.msac.d.0.sps","cache/bitmaps/fxt_gn_026_smokebomb.msac.n.0.sps","cache/bitmaps/fxt_gn_027_sailpieces.psd.0.sps","cache/bitmaps/fxt_gn_029_brush_anim.psd.0.sps","cache/bitmaps/fxt_gn_030_spirit_arrow.psd.0.sps","cache/bitmaps/fxt_gn_032_spike.psd.0.sps","cache/bitmaps/fxt_jn_black_flame_licks.psd.0.sps","cache/bitmaps/fxt_mv_lgd_ring.psd.0.sps","cache/bitmaps/fxt_gn_031_paper.psd.0.sps","cache/bitmaps/fxt_gn_033_ring.psd.0.sps","cache/bitmaps/fxt_jn_blue_flame_loop_01.psd.0.sps","cache/bitmaps/fxt_mv_lgd_swipe03.psd.0.sps","cache/bitmaps/fxt_mv_ui_cloudtest.psd.0.sps","cache/bitmaps/fxt_or_003_blackfly.psd.0.sps","cache/bitmaps/fxt_or_008_grasses.psd.0.sps","cache/bitmaps/fxt_or_014_maplebigleafleaves.psd.0.sps","cache/bitmaps/fxt_or_016_coniferneedles.psd.0.sps","cache/bitmaps/fxt_or_01_mud.psd.0.sps","cache/bitmaps/fxt_or_020_feathers.psd.0.sps","cache/bitmaps/fxt_or_033_liquid.psd.0.sps","cache/bitmaps/fxt_or_034_pampas_fluff.psd.0.sps","cache/bitmaps/fxt_or_035_straw.psd.0.sps","cache/bitmaps/fxt_or_037_blood.psd.0.sps","cache/bitmaps/fxt_fi_022_lrg_loop.psd.0.sps","cache/bitmaps/fxt_or_039_beech_leaves.msac.d.0.sps","cache/bitmaps/fxt_or_039_beech_leaves.msac.n.0.sps","cache/bitmaps/fxt_or_040_ginkgo_leaves.msac.d.0.sps","cache/bitmaps/fxt_or_040_ginkgo_leaves.msac.n.0.sps","cache/bitmaps/fxt_or_044_fireflies.psd.0.sps","cache/bitmaps/fxt_or_046_caddis.psd.0.sps","cache/bitmaps/fxt_or_047_bamboo_leaves_dead.msac.d.0.sps","cache/bitmaps/fxt_or_047_bamboo_leaves_dead.msac.n.0.sps","cache/bitmaps/fxt_or_048_starlings.psd.0.sps","cache/bitmaps/fxt_or_049_grass.psd.0.sps","cache/bitmaps/fxt_or_050_redmaple_leaves.msac.d.0.sps","cache/bitmaps/fxt_or_050_redmaple_leaves.msac.n.0.sps","cache/bitmaps/fxt_or_051_parasol_leaves.msac.d.0.sps","cache/bitmaps/fxt_or_051_parasol_leaves.msac.n.0.sps","cache/bitmaps/fxt_or_052_zelkova_leaves.msac.d.0.sps","cache/bitmaps/fxt_or_052_zelkova_leaves.msac.n.0.sps","cache/bitmaps/fxt_or_053_ginkgo_leaves_white.msac.d.0.sps","cache/bitmaps/fxt_or_053_ginkgo_leaves_white.msac.n.0.sps","cache/bitmaps/fxt_or_054_flower_petals.msac.d.0.sps","cache/bitmaps/fxt_or_054_flower_petals.msac.n.0.sps","cache/bitmaps/fxt_or_055_plum.msac.d.0.sps","cache/bitmaps/fxt_or_055_plum.msac.n.0.sps","cache/bitmaps/fxt_sm_002_tilingwispy.psd.0.sps","cache/bitmaps/fxt_sm_011_greywisps.dds.0.sps","cache/bitmaps/fxt_sm_020_generic.psd.0.sps","cache/bitmaps/fxt_sm_025_large_flat_loop.psd.0.sps","cache/bitmaps/fxt_sm_025_large_loop.psd.0.sps","cache/bitmaps/fxt_sm_025_large_loop_n.psd.0.sps","cache/bitmaps/fxt_sm_026_medium_loop.psd.0.sps","cache/bitmaps/fxt_sm_026_medium_loop_n.psd.0.sps","cache/bitmaps/fxt_sm_027_small_loop.psd.0.sps","cache/bitmaps/fxt_sm_028_dust_tendril.psd.0.sps","cache/bitmaps/fxt_sm_028_dust_tendril_n.psd.0.sps","cache/bitmaps/fxt_sm_029_dust_burst.psd.0.sps","cache/bitmaps/fxt_sm_029_dust_burst_n.psd.0.sps","cache/bitmaps/fxt_sm_030_small_loop.psd.0.sps","cache/bitmaps/fxt_sm_030_small_loop_n.psd.0.sps","cache/bitmaps/fxt_sm_031_loop.psd.0.sps","cache/bitmaps/fxt_sm_031_loop_n.psd.0.sps","cache/bitmaps/fxt_sm_032_medium_blur_loop.psd.0.sps","cache/bitmaps/fxt_sm_033_loop.psd.0.sps","cache/bitmaps/fxt_sm_034_rise.psd.0.sps","cache/bitmaps/fxt_sm_034_rise_n.psd.0.sps","cache/bitmaps/fxt_sm_035_fuzzy_loop.psd.0.sps","cache/bitmaps/fxt_sm_036_medium_loop_lgd.psd.0.sps","cache/bitmaps/fxt_sm_037_strip_lgd.psd.0.sps","cache/bitmaps/fxt_splash_small_flip.psd.0.sps","cache/bitmaps/fxt_splash_step.psd.0.sps","cache/bitmaps/fxt_st_007_fogbank.psd.0.sps","cache/bitmaps/fxt_st_007_fogbank_n.psd.0.sps","cache/bitmaps/fxt_st_010_fog_large.psd.0.sps","cache/bitmaps/fxt_st_010_fog_large_n.psd.0.sps","cache/bitmaps/fxt_st_011_fog_medium.psd.0.sps","cache/bitmaps/fxt_st_012_lowfog.psd.0.sps","cache/bitmaps/fxt_st_015_base_loop.psd.0.sps","cache/bitmaps/fxt_st_015_base_loop_n.psd.0.sps","cache/bitmaps/fxt_st_016_loop.psd.0.sps","cache/bitmaps/fxt_st_016_loop_n.psd.0.sps","cache/bitmaps/fxt_st_017_loop.psd.0.sps","cache/bitmaps/fxt_sy_009_videoblocks.psd.0.sps","cache/bitmaps/fxt_sy_010_videogrunge.psd.0.sps","cache/bitmaps/fxt_wa_004_splashsmall.psd.0.sps","cache/bitmaps/fxt_wa_012_splash.psd.0.sps","cache/bitmaps/fxt_wa_013_large_splash.psd.0.sps","cache/bitmaps/fxt_wa_015_rain.psd.0.sps","cache/bitmaps/fxt_wa_016_rainfall_far.psd.0.sps","cache/bitmaps/fxt_wa_018_stream.psd.0.sps","cache/bitmaps/fxt_wa_018_stream_n.psd.0.sps","cache/bitmaps/fxt_wa_021_flow_long.psd.0.sps","cache/bitmaps/fxt_wa_022_splash.psd.0.sps","cache/bitmaps/fxt_wa_022_wave_splash.psd.0.sps","cache/bitmaps/gore_pj_blood_orb_001.msac.e.0.sps","cache/bitmaps/hero_samurai_orange_rope.msac.d.0.sps","cache/bitmaps/hero_thighshield_chainmail.msac.d.0.sps","cache/bitmaps/horse_armor_brush_mtl.msac.d.0.sps","cache/bitmaps/horse_hero_saddle_tassles_mtl.msac.d.0.sps","cache/bitmaps/horse_teeth.msac.d.0.sps","cache/bitmaps/icon_camp_rumored_worldmap.msac.d.0.sps","cache/bitmaps/icon_camp_waypoint.msac.d.0.sps","cache/bitmaps/icon_camp_waypoint_complete.msac.d.0.sps","cache/bitmaps/icon_camp_worldmap.msac.d.0.sps","cache/bitmaps/icon_camp_worldmap_complete.msac.d.0.sps","cache/bitmaps/icon_crier_waypoint.msac.d.0.sps","cache/bitmaps/icon_farmstead_rumored_worldmap.msac.d.0.sps","cache/bitmaps/icon_farmstead_waypoint.msac.d.0.sps","cache/bitmaps/icon_farmstead_waypoint_complete.msac.d.0.sps","cache/bitmaps/icon_farmstead_worldmap.msac.d.0.sps","cache/bitmaps/icon_farmstead_worldmap_complete.msac.d.0.sps","cache/bitmaps/icon_inventory_currency_essence.msac.d.0.sps","cache/bitmaps/icon_inventory_resource_blessing.msac.d.0.sps","cache/bitmaps/icon_inventory_resource_crystals.msac.d.0.sps","cache/bitmaps/icon_inventory_tool_heal.msac.d.0.sps","cache/bitmaps/icon_mission_waypoint_yuna.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_goldpath.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_goldpath_ishikawa.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_goldpath_kenji.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_goldpath_masako.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_goldpath_norio.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_goldpath_resume.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_goldpath_yuna.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_goldpath_yuriko.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_legendary.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_legendary_resume.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_silverpath.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_silverpath_ishikawa.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_silverpath_kenji.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_silverpath_masako.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_silverpath_norio.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_silverpath_resume.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_silverpath_tenzo.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_silverpath_yuna.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_silverpath_yuriko.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_tenzo_resume.msac.d.0.sps","cache/bitmaps/icon_mission_worldmap_yuna.msac.d.0.sps","cache/bitmaps/icon_navigation_castle_worldmap.msac.d.0.sps","cache/bitmaps/icon_navigation_castle_worldmap_occupied.msac.d.0.sps","cache/bitmaps/icon_navigation_duel_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_hub_worldmap.msac.d.0.sps","cache/bitmaps/icon_navigation_hub_worldmap_occupied.msac.d.0.sps","cache/bitmaps/icon_navigation_landmark_worldmap.msac.d.0.sps","cache/bitmaps/icon_navigation_landmark_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_lighthouse_waypoint.msac.d.0.sps","cache/bitmaps/icon_navigation_lighthouse_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_lighthouse_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_memory_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_archery_waypoint.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_archery_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_archery_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_bamboo_waypoint.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_bamboo_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_bamboo_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_bokken_waypoint.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_bokken_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_bokken_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_charmcats_waypoint.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_charmcats_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_charmcats_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_charmdeer_waypoint.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_charmdeer_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_charmdeer_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_charmmonkeys_waypoint.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_charmmonkeys_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_charmmonkeys_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_foxshrine_waypoint.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_foxshrine_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_foxshrine_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_haiku_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_haiku_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_hutbuilder_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_hutbuilder_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_onsen_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_onsen_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_shrine_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_minigame_shrine_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_swordkitshrine_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_swordkitshrine_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_navigation_windshrine_worldmap_incomplete.msac.d.0.sps","cache/bitmaps/icon_navigation_windshrine_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_refugeecamp_worldmap.msac.d.0.sps","cache/bitmaps/icon_refugeecamp_worldmap_rumored.msac.d.0.sps","cache/bitmaps/icon_roadblock_rumored_worldmap.msac.d.0.sps","cache/bitmaps/icon_roadblock_waypoint.msac.d.0.sps","cache/bitmaps/icon_roadblock_waypoint_complete.msac.d.0.sps","cache/bitmaps/icon_roadblock_worldmap.msac.d.0.sps","cache/bitmaps/icon_roadblock_worldmap_complete.msac.d.0.sps","cache/bitmaps/icon_storyteller_waypoint.msac.d.0.sps","cache/bitmaps/icon_storyteller_worldmap.msac.d.0.sps","cache/bitmaps/icon_symbol_skill_point_01.msac.d.0.sps","cache/bitmaps/icon_town_occupied_worldmap.msac.d.0.sps","cache/bitmaps/icon_town_worldmap.msac.d.0.sps","cache/bitmaps/icon_unknown_worldmap.msac.d.0.sps","cache/bitmaps/iki_fog_of_war_texture.msac.d.0.sps","cache/bitmaps/iki_world_map_texture_exile.msac.d.0.sps","cache/bitmaps/iyo_phase_01_crown_mtl.msac.d.0.sps","cache/bitmaps/iyo_phase_01_rope_mtl.msac.d.0.sps","cache/bitmaps/iyo_phase_01_rope_tassle_mtl.msac.d.0.sps","cache/bitmaps/map_water_01.msac.d.0.sps","cache/bitmaps/metal_ap_steel_crystal_ore_001.msac.d.0.sps","cache/bitmaps/mongol_horse_saddle.msac.d.0.sps","cache/bitmaps/mongol_melee_tribe_05_costume_boot_mtl.msac.d.0.sps","cache/bitmaps/mongol_melee_tribe_05_costume_helmet_mtl.msac.d.0.sps","cache/bitmaps/mongol_melee_tribe_05_costume_tight_covers_mtl.msac.d.0.sps","cache/bitmaps/mongols_ranged_tribe_03_grenadier_helmet_mtl.msac.d.0.sps","cache/bitmaps/paper_pj_shoji_001.msac.t.0.sps","cache/bitmaps/particle_char_ayu_mtl.msac.d.0.sps","cache/bitmaps/particle_char_ayu_mtl.msac.n.0.sps","cache/bitmaps/particle_char_bluetail_mtl.msac.d.0.sps","cache/bitmaps/particle_char_crow_glow_mtl.msac.d.0.sps","cache/bitmaps/particle_char_crow_glow_mtl.msac.n.0.sps","cache/bitmaps/particle_char_crow_mtl.msac.d.0.sps","cache/bitmaps/particle_char_crow_mtl.msac.n.0.sps","cache/bitmaps/particle_char_eagle_mtl.msac.d.0.sps","cache/bitmaps/particle_char_eagle_mtl.msac.n.0.sps","cache/bitmaps/particle_char_firefly_mtl.msac.d.0.sps","cache/bitmaps/particle_char_hornet_mtl.msac.d.0.sps","cache/bitmaps/particle_char_hornet_mtl.msac.n.0.sps","cache/bitmaps/particle_char_koi_mtl.msac.d.0.sps","cache/bitmaps/particle_char_koi_mtl.msac.n.0.sps","cache/bitmaps/particle_char_mongol_skirmisher_low_mtl.msac.d.0.sps","cache/bitmaps/particle_char_mongol_skirmisher_low_mtl.msac.n.0.sps","cache/bitmaps/particle_char_robin_mtl.msac.d.0.sps","cache/bitmaps/particle_char_robin_mtl.msac.n.0.sps","cache/bitmaps/particle_char_seagull_mtl.msac.d.0.sps","cache/bitmaps/particle_char_seagull_mtl.msac.n.0.sps","cache/bitmaps/particle_char_swallow_mtl.msac.d.0.sps","cache/bitmaps/particle_char_swallow_mtl.msac.n.0.sps","cache/bitmaps/particle_char_wagtail_mtl.msac.d.0.sps","cache/bitmaps/particle_char_warbler_mtl.msac.d.0.sps","cache/bitmaps/particle_lgd_char_samurai_burned.msac.d.0.sps","cache/bitmaps/particle_lgd_char_samurai_burned.msac.n.0.sps","cache/bitmaps/sprobe_default.spbmp.0.sps","cache/bitmaps/straw_back_basket.msac.d.0.sps","cache/bitmaps/tsu_world_map_noise.msac.h.0.sps","cache/bitmaps/ui_fx_streak_02.msac.d.0.sps","cache/bitmaps/tsu_world_map_texture_exile.msac.d.0.sps","cache/bitmaps/strawhat_amigasa_001.msac.d.0.sps","cache/bitmaps/taka_head_decapitated_mtl.msac.d.0.sps","cache/bitmaps/tenzo_costume_acc_mtl.msac.d.0.sps","cache/bitmaps/tsu_world_map_texture_iki_intro.msac.d.0.sps","cache/bitmaps/wicker_ll_palm_001.msac.d.0.sps","cache/bitmaps/iki_world_map_texture.msac.d.0.sps","cache/bitmaps/tsu_fog_of_war_texture.msac.d.0.sps","cache/bitmaps/tsu_world_map_texture.msac.d.0.sps","cache/core_tsu.sprig.xpps","cache/bitmaps/fxt_or_045_alder_leaves.psd.0.sps","cache/bitmaps/particle_char_crane_mtl.msac.d.0.sps","cache/bitmaps/particle_char_crane_mtl.msac.n.0.sps","cache/bitmaps/particle_char_frog_mtl.msac.d.0.sps","cache/bitmaps/particle_char_frog_mtl.msac.n.0.sps","cache/bitmaps/particle_char_ghostcrab_mtl.msac.d.0.sps","cache/bitmaps/particle_char_ghostcrab_mtl.msac.n.0.sps","cache/bitmaps/particle_char_goose_mtl.msac.d.0.sps","cache/bitmaps/particle_char_goose_mtl.msac.n.0.sps","cache/bitmaps/particle_char_heron_mtl.msac.d.0.sps","cache/tsu.xpps","cache/bitmaps/custom_ll_lm_kanji_001.msac.d.0.sps","cache/bitmaps/fxt_gn_016_paperlantern.psd.0.sps","cache/bitmaps/fxt_or_043_lilypad.msac.d.0.sps","cache/bitmaps/fxt_or_043_lilypad.msac.n.0.sps","cache/hero.xpps","cache/bitmaps/background_texture_foil_01.msac.d.0.sps","cache/bitmaps/mask_wipe_noise_02.msac.d.0.sps","cache/bitmaps/mask_wipe_radial_06.msac.d.0.sps","cache/bitmaps/ui_fx_shine_02.msac.d.0.sps","cache/game_missions_tsu.sprig.xpps","cache/bitmaps/icon_symbol_wind_01.msac.d.0.sps","cache/bitmaps/ui_fx_streak_04.msac.d.0.sps","cache/bitmaps/background_diamond_8.msac.d.0.sps","cache/bitmaps/background_diamond_8_highlight.msac.d.0.sps","cache/game_missions_all.sprig.xpps","cache/bitmaps/select_bow_select.msac.d.0.sps","cache/bitmaps/mask_wipe_linear_01.msac.d.0.sps","cache/bitmaps/background_bar_textured_47.msac.d.0.sps","cache/bitmaps/ui_fx_streak_05.msac.d.0.sps","cache/bitmaps/background_full_screen_03.msac.d.0.sps","cache/bitmaps/background_gradient_vertical_04.msac.d.0.sps","cache/bitmaps/icon_arrow_02.msac.d.0.sps","cache/bitmaps/mask_wipe_linear_05.msac.d.0.sps","cache/ghost_title.xpps","cache/ghost_title_0_0_0.xpps","cache/bitmaps/divider_horizontal_line_textured_06.msac.d.0.sps","cache/bitmaps/mask_wipe_linear_mirror_01.msac.d.0.sps","cache/bitmaps/reticle_bow_spline.msac.d.0.sps","cache/bitmaps/mask_wipe_linear_02.msac.d.0.sps","cache/bitmaps/meter_hero_health_mask.msac.d.0.sps","cache/bitmaps/reticle_bow_spline_shadow.msac.d.0.sps"]}}};
/*PROFILES-END*/
/*UI-BEGIN*/
const $ = s => document.querySelector(s);
const MIB = 1048576;
const fmtSize = n => n >= GFS.GIB ? (n / GFS.GIB).toFixed(2) + " GiB" : n >= MIB ? (n / MIB).toFixed(1) + " MiB" : n >= 1024 ? (n / 1024).toFixed(0) + " KiB" : n + " B";
const esc = s => String(s == null ? "" : s).replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
const sleep = ms => new Promise(r => setTimeout(r, ms));
let INFO = null, SRC = null, TOKEN = "", RUNNING = null;

function log(m, cls) {
  const d = document.createElement("div");
  if (cls) d.className = cls;
  d.textContent = new Date().toLocaleTimeString() + "  " + m;
  $("#log").prepend(d);
}
function status(html, cls) { $("#status").className = "small " + (cls || ""); $("#status").innerHTML = html; }

// ---- the console's API (makeRx in GFS)
function xhrPost(url, blob, progress, signal) {
  return new Promise(resolve => {
    const x = new XMLHttpRequest();
    signal.abort = () => x.abort();
    x.open("POST", url);
    if (TOKEN) x.setRequestHeader("X-Ghostldr-Token", TOKEN);
    x.upload.onprogress = e => progress(e.loaded);
    x.onload = () => { let j = null; try { j = JSON.parse(x.responseText); } catch (e) { /* none */ } resolve({status: x.status, json: j}); };
    x.onerror = x.onabort = x.ontimeout = () => resolve({status: 0, json: null});
    x.send(blob);
  });
}
const rx = GFS.makeRx({fetch: (u, o) => fetch(u, o), post: xhrPost, token: () => TOKEN, sleep,
                       log: (m, c) => log(m, c), status: (m, c) => status(esc(m), c), fmtSize});
const api = rx.api;

function fromFiles(list) {
  const items = Array.from(list, f => ({path: f.webkitRelativePath || f.name, size: f.size, file: f}));
  const top = items.length ? items[0].path.split("/")[0] : "";
  return GFS.makeSource("folder", top, items);
}
async function fromDrop(dt) {
  const items = [], dirs = [];
  const entries = Array.from(dt.items || [], i => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
  if (entries.length === 1 && entries[0].isFile && /\.zip$/i.test(entries[0].name)) return GFS.fromZip(dt.files[0]);
  async function walk(e) {
    if (e.isFile) {
      const f = await new Promise((res, rej) => e.file(res, rej));
      items.push({path: e.fullPath.replace(/^\/+/, ""), size: f.size, file: f});
    } else if (e.isDirectory) {
      dirs.push({path: e.fullPath.replace(/^\/+/, "") + "/", dir: true, size: 0});
      const rd = e.createReader();
      while (true) {
        const batch = await new Promise((res, rej) => rd.readEntries(res, rej));
        if (!batch.length) break;
        for (const c of batch) await walk(c);
      }
    }
  }
  for (const e of entries) await walk(e);
  return GFS.makeSource("folder", entries[0] ? entries[0].name : "", items.concat(dirs));
}

// ---- what the game is
let WHAT = null;
async function choose(getSrc) {
  $("#pick-msg").className = "small"; $("#pick-msg").textContent = "Reading…";
  try {
    SRC = await getSrc();
  } catch (e) {
    SRC = null; $("#s-what").classList.add("hide"); $("#s-go").classList.add("hide");
    $("#pick-msg").className = "small bad"; $("#pick-msg").textContent = e.message;
    return;
  }
  const sfo = GFS.parseSfo(await SRC.bytes("sce_sys/param.sfo"));
  let title = GFS.normalizeTitle(sfo.TITLE_ID) || "";
  const from = title ? "param.sfo" : "";
  if (!title) { const m = (SRC.name + " " + SRC.root).toUpperCase().match(/[A-Z]{4}\d{5}/); if (m) title = m[0]; }
  WHAT = {sfo, from};
  $("#title-in").value = title;
  $("#pick-msg").textContent = "";
  let total = 0; for (const n of SRC.files.values()) total += n;
  WHAT.total = total;
  describe();
  $("#s-what").classList.remove("hide");
  $("#s-go").classList.remove("hide");
}
function prof(title) { return PROFILES[title] || {}; }
function describe() {
  const title = GFS.normalizeTitle($("#title-in").value);
  const sfo = WHAT.sfo, pr = title ? prof(title) : {};
  const pk = pr.pack || {};
  const name = pr.name || sfo.TITLE || "", version = pr.version || sfo.APP_VER || "";
  const prefix = title ? (pk.prefix || GFS.defaultPrefix(title)) : "";
  const msgs = [];
  let ok = !!title;
  if (!title) msgs.push(['bad', "Type the game's title ID (for example CUSA12345): it has no param.sfo that says it."]);
  else if (!WHAT.from) msgs.push(['warn', "The title ID was not read from the game's param.sfo: check it."]);
  const why = prefix && GFS.prefixProblem(prefix);
  if (why) { ok = false; msgs.push(['bad', why]); }
  const containers = Math.ceil(WHAT.total / GFS.CAP);
  if (containers > GFS.MAX_CONTAINERS) { ok = false; msgs.push(['bad', "Too big for saved data (at most 93 GiB)."]); }
  const dump = /^(CUSA|PLAS|PCAS|PCJS|PLJS|PLJM|PCKS|PLKS|PPSA)\d{5}$/i.test(title || "") && !/^IV\d{4}-/.test(sfo.CONTENT_ID || "");
  if (INFO && INFO.platform === "ps4" && dump)
    msgs.push(['warn', "A PS4 gives SWRR about 2.5 GiB of memory: game dumps largely won't run there. Homebrew and small titles do."]);
  if (SRC.skipped) msgs.push(['mute', `${SRC.skipped} files outside the game's folder are left out.`]);
  if (SRC.junk) msgs.push(['mute', `${SRC.junk} system files (.DS_Store, ._*, __MACOSX) are left out.`]);
  if (SRC.kind === "zip" && [...SRC.files.keys()].length && SRC.streamed && WHAT.total > 4 * GFS.GIB)
    msgs.push(['mute', "Large compressed zips are unpacked in this browser as they are sent: a stored (uncompressed) zip is lighter on a phone."]);
  const dl = [["Game", esc(name || "(no name)")], ["Title ID", esc(title || "?") + (WHAT.from ? ' <span class="mute small">(param.sfo)</span>' : "")],
              ["Version", esc(version || "-")], ["Size", `${fmtSize(WHAT.total)} in ${SRC.files.size} files`],
              ["Stored as", prefix ? `${containers} save container${containers === 1 ? "" : "s"} ${esc(prefix)}000… + ${esc(prefix)}INDEX` : "-"],
              ["From", esc(SRC.kind === "zip" ? SRC.name : (SRC.name || "folder")) +
                (SRC.root && SRC.root.replace(/\/$/, "") !== SRC.name ? " / " + esc(SRC.root.replace(/\/$/, "")) : "")]];
  $("#what").innerHTML = dl.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join("");
  $("#what-msg").innerHTML = msgs.map(([c, m]) => `<p class="${c}">${esc(m)}</p>`).join("");
  Object.assign(WHAT, {title, name, version, prefix, pk, ok});
  $("#go").disabled = !ok || !!RUNNING;
}

// ---- installed titles
async function refresh() {
  let cat = null;
  try {
    cat = (await GFS.readCatalogue(rx, knownPrefixes(), () => {})).cat;
  } catch (e) { $("#installed").innerHTML = `<span class="bad">${esc(e.message)}</span>`; return; }
  if (!cat || !cat.entries.length) { $("#installed").textContent = "No games installed in saved data yet."; return; }
  $("#installed").innerHTML = "<table><tr><th>Game</th><th>Size</th><th>State</th><th></th></tr>" + cat.entries.map(e =>
    `<tr><td>${esc(e.name || e.title)}<div class="mute small">${esc(e.title)} ${esc(e.version)}</div></td><td>${fmtSize(e.total)}</td>` +
    `<td>${e.flags & GFS.INCOMPLETE ? '<span class="warn">incomplete</span>' : '<span class="ok">ready</span>'}</td>` +
    `<td><button data-rm="${esc(e.title)}" class="small">Remove</button></td></tr>`).join("") + "</table>" +
    '<p class="small mute">Remove deletes the game\'s data; its saves stay.</p>';
  document.querySelectorAll("[data-rm]").forEach(b => b.onclick = () => removeTitle(b.dataset.rm));
}
const knownPrefixes = () => Object.fromEntries(Object.entries(PROFILES).filter(([, p]) => p.pack && p.pack.prefix).map(([t, p]) => [p.pack.prefix, t]));
async function unlock() {
  TOKEN = ($("#tok").value || "").toUpperCase().replace(/[^A-Z0-9]/g, "");
  if (!INFO || !INFO.token) return true;
  try { await api("POST", "auth"); try { sessionStorage.setItem("gldr-token", TOKEN); } catch (e) { /* storage off */ } return true; }
  catch (e) { status(esc(e.message) + ": type the token the TV shows.", "bad"); $("#tok").focus(); return false; }
}
async function removeTitle(title) {
  if (RUNNING) return;
  if (!confirm(`Remove ${title}'s game data from the console? Its saves stay.`)) return;
  if (!(await unlock())) return;
  try { await GFS.remove(rx, title, knownPrefixes(), m => log(m)); status(`${esc(title)} removed.`, "ok"); }
  catch (e) { status(esc(e.message), "bad"); }
  refresh();
}

// ---- the install
async function install() {
  if (RUNNING || !WHAT || !WHAT.ok) return;
  if (!(await unlock())) return;
  const signal = {stopped: false, abort: null};
  RUNNING = signal;
  $("#go").disabled = true; $("#stop").disabled = false;
  const t0 = Date.now();
  let speedAt = Date.now(), speedDone = 0, rate = 0, lastTotal = 1;
  const ui = {
    log: m => log(m),
    step: m => status(esc(m)),
    progress(done, total) {
      lastTotal = total || 1;
      $("#bar").style.width = (100 * done / lastTotal).toFixed(1) + "%";
      const t = Date.now();
      if (t - speedAt > 2000) { rate = rate ? 0.7 * rate + 0.3 * (done - speedDone) / ((t - speedAt) / 1000) : (done - speedDone) / ((t - speedAt) / 1000); speedAt = t; speedDone = done; }
      const eta = rate > 0 ? Math.round((total - done) / rate) : 0;
      status(`${fmtSize(done)} of ${fmtSize(total)}` + (rate > 0 ? ` · ${(rate / 1e6).toFixed(1)} MB/s · about ${eta >= 120 ? Math.round(eta / 60) + " min" : eta + " s"} left` : ""));
    },
  };
  try {
    const p = GFS.pack(SRC.files, SRC.dirs, WHAT.title, WHAT.prefix, WHAT.pk);
    for (const c of p.containers) c.lid = p.layoutId;
    const info = {name: WHAT.name, version: WHAT.version};
    await api("POST", "begin", {title: p.title, name: WHAT.name.slice(0, 63), total: p.total});
    log(`${p.title}: ${p.files} files, ${fmtSize(p.total)} in ${p.containers.length} containers, layout ${p.layoutId.toString(16)}`);
    status("Looking at what the console has…");
    const st = await GFS.survey(rx, p, info, $("#force").checked, knownPrefixes(), m => log(m));
    for (const c of p.containers) c.lid = p.layoutId;
    const steps = GFS.plan(st, p, info, $("#force").checked);
    const puts = steps.filter(s => s[0] === "put").length;
    if (!steps.length) { status(`${esc(p.title)} is already installed: nothing to send.`, "ok"); await api("POST", "end", {title: p.title, ok: 1}); return; }
    if (st.tagged.size && puts < p.containers.length) log(`carrying on: ${p.containers.length - puts} of ${p.containers.length} containers are already there`);
    const need = GFS.spaceNeeded(st, steps, p);
    if (need) {
      status(`Checking the console has ${fmtSize(need)} free…`);
      const f = await rx.free(need);
      if (f.status) throw new GFS.InstallError("the free-space check failed: " + GFS.errText(f.status));
      if (f.have < need) throw new GFS.InstallError(`not enough space on the console: this needs ${fmtSize(need)} more and ${fmtSize(f.have)} is free. Nothing was changed. Free some space (or remove a game above), then Install again.`);
    }
    await GFS.execute(rx, steps, st, p, SRC, ui, signal);
    await api("POST", "end", {title: p.title, ok: 1});
    $("#bar").style.width = "100%";
    status(`<b>${esc(WHAT.name || p.title)} is installed</b> (${fmtSize(p.total)} in ${Math.round((Date.now() - t0) / 1000)} s). Pick it in ghostldr's game selector.`, "ok");
    log("install complete", "ok");
  } catch (e) {
    const paused = e.message === "paused";
    status(paused ? "Paused. Install again carries on where it stopped." : esc(e.message), paused ? "warn" : "bad");
    log(paused ? "paused" : "stopped: " + e.message, paused ? "warn" : "bad");
    try { await api("POST", "end", {title: WHAT.title, ok: 0}); } catch (e2) { /* the console is gone */ }
  } finally {
    RUNNING = null;
    $("#stop").disabled = true; $("#go").disabled = !WHAT.ok;
    refresh();
  }
}

// ---- start
$("#pick-dir").onchange = e => e.target.files.length && choose(() => fromFiles(e.target.files));
$("#pick-zip").onchange = e => e.target.files.length && choose(() => GFS.fromZip(e.target.files[0]));
$("#title-in").oninput = () => WHAT && describe();
$("#go").onclick = install;
$("#stop").onclick = () => { if (RUNNING) { RUNNING.stopped = true; if (RUNNING.abort) RUNNING.abort(); } };
const drop = $("#drop");
drop.ondragover = e => { e.preventDefault(); drop.classList.add("on"); };
drop.ondragleave = () => drop.classList.remove("on");
drop.ondrop = e => { e.preventDefault(); drop.classList.remove("on"); const dt = e.dataTransfer; choose(() => fromDrop(dt)); };
window.addEventListener("beforeunload", e => { if (RUNNING) { e.preventDefault(); e.returnValue = ""; } });
(async () => {
  const m = location.hash.match(/token=([A-Za-z0-9-]+)/);
  let saved = "";
  try { saved = sessionStorage.getItem("gldr-token") || ""; } catch (e) { /* storage off */ }
  $("#tok").value = m ? m[1] : saved;
  try {
    INFO = await api("GET", "info");
    $("#who").textContent = `${INFO.server === "ghostldr" ? "ghostldr" : "manager"} · SWRR ${INFO.swrr || "?"} · ${(INFO.platform || "").toUpperCase()}`;
    $("#tok-row").classList.toggle("hide", !INFO.token);
  } catch (e) { $("#who").textContent = "the console does not answer"; }
  refresh();
})();
/*UI-END*/
</script>
</body>
</html>
]====]

if MANAGER_TEST then return M end

-- ===================================================================== console
-- Everything below runs inside SWRR with Luac0re's globals.

init_dlsym()
local function kfn(name)
    local addr = dlsym(LIBKERNEL_HANDLE, name)
    if addr == 0 then error("dlsym " .. name) end
    return func_wrap(addr)
end
local kopen, kread, kpread, kwrite = kfn("sceKernelOpen"), kfn("sceKernelRead"), kfn("sceKernelPread"), kfn("sceKernelWrite")
local kclose, kstat, kgetdents = kfn("sceKernelClose"), kfn("sceKernelStat"), kfn("sceKernelGetdents")
local kmkdir, kunlink, krmdir = kfn("sceKernelMkdir"), kfn("sceKernelUnlink"), kfn("sceKernelRmdir")
local usleep, gettime, tsc = kfn("sceKernelUsleep"), kfn("sceKernelGetProcessTime"), kfn("sceKernelReadTsc")
local sendto, load_start = kfn("sendto"), kfn("sceKernelLoadStartModule")

local PORT = math.tointeger(tonumber(MANAGER_PORT or 8090)) or 8090
local SCRATCH = MANAGER_SCRATCH
if SCRATCH ~= nil and not M.scratch_ok(SCRATCH) then SCRATCH = nil end
local OWN = M.SWRR[TITLE_ID or ""]
local TITLES = type(MANAGER_TITLES) == "table" and MANAGER_TITLES or {}   -- {[title ID] = {name =, version =}}
local MSG_NOSIGNAL, MSG_WAITALL = 0x20000, 0x40
local SO_NOSIGPIPE, SO_SNDTIMEO, SO_RCVTIMEO, SO_SNDBUF, SO_RCVBUF = 0x0800, 0x1005, 0x1006, 0x1001, 0x1002

-- buffers (one each, for the whole session: the Lua GC is off)
local CHUNK = 4 << 20
local buf = malloc(CHUNK)
local small, zeros, hbuf = malloc(0x10000), malloc(1024), malloc(16384)
local dirbuf, params, result, st, mp_buf, del = malloc(32), malloc(128), malloc(64), malloc(0x100), malloc(16), malloc(64)
local path, strbuf, dents, one, tv = malloc(512), malloc(256), malloc(0x4000), malloc(8), malloc(16)
local SEARCH_MAX = 512
local s_cond, s_res = malloc(0x40), malloc(0x40)
local s_names, s_params, s_infos = malloc(SEARCH_MAX * 32), malloc(SEARCH_MAX * 0x530), malloc(SEARCH_MAX * 0x30)
if buf == 0 or s_params == 0 then error("manager: out of memory") end
memset(zeros, 0, 1024)
write32(one, 1)

local function u32(v) return v & 0xFFFFFFFF end
local function failed(v) return v < 0 or u32(v) >= 0x80000000 end
local function fd_ok(fd) return fd >= 0 and fd < 0x10000 end
local function cstr(p, n) return (read_buffer(p, math.max(n, 64)):match("^[^%z]*")) end

-- log over UDP to the computer that started us
local log_fd, log_addr, log_buf = -1, nil, nil

-- sockets are made in the JIT compiler process and handed over; its copy is
-- closed so the port is free again once we close ours
local function new_socket(kind)
    if jit_syscall and jit_send_recv_fd and NEW_JIT_SOCK then
        local jfd = jit_syscall.socket(AF_INET, kind, 0)
        if jfd < 0 then return -1 end
        local fd = jit_send_recv_fd(jfd, NEW_JIT_SOCK, NEW_MAIN_SOCK)
        jit_syscall.close(jfd)
        return fd
    end
    return create_socket(AF_INET, kind, 0)
end

local function sockaddr(p, ip, port)
    memset(p, 0, 16)
    write8(p, 16)
    write8(p + 1, AF_INET)
    write16(p + 2, ((port << 8) | (port >> 8)) & 0xFFFF)
    if ip then
        local a, b, c, d = ip:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
        write32(p + 4, (tonumber(d) << 24) | (tonumber(c) << 16) | (tonumber(b) << 8) | tonumber(a))
    end
end

if MANAGER_LOG_IP and MANAGER_LOG_PORT then
    log_fd = new_socket(SOCK_DGRAM)
    log_addr, log_buf = malloc(16), malloc(1100)
    sockaddr(log_addr, MANAGER_LOG_IP, MANAGER_LOG_PORT)
end
local function say(s)
    if log_fd < 0 then return end
    s = "[manager] " .. s
    if #s > 1000 then s = s:sub(1, 1000) end
    write_string(log_buf, s)
    sendto(log_fd, log_buf, #s, 0, log_addr, 16)
end

-- ---------------------------------------------------------------- save data
local function module(n)
    write_string(strbuf, n)
    return load_start(strbuf, 0, 0, 0, 0, 0)
end
local sd, us = module("libSceSaveData.sprx"), module("libSceUserService.sprx")
local sd_umount = func_wrap(dlsym(sd, "sceSaveDataUmount"))
local sd_delete = func_wrap(dlsym(sd, "sceSaveDataDelete"))
-- Mount (v1) and DirNameSearch from SWRR's import table: a dlsym'd Mount
-- refuses every mount (ghostfs.c).  SWRR 01.02, US and EU.  Jak X 1.04
-- (UltraC0re) imports Mount2 instead: dirName@0x8, blocks@0x10, mode@0x18.
local MOUNT2 = J_COMP ~= nil
local mount_addr = read64(EBOOT_BASE + (MOUNT2 and 0xcd1e40 or 0x3893F0))
local search_addr = read64(EBOOT_BASE + (MOUNT2 and 0xcd1e60 or 0x3893C0))
if mount_addr < 0x10000 or search_addr < 0x10000 then error("manager: the host's save data imports not found") end
local sd_mount, sd_search = func_wrap(mount_addr), func_wrap(search_addr)
local uid = malloc(8)
func_wrap(dlsym(us, "sceUserServiceGetInitialUser"))(uid)
local USER = read32(uid)

local mounted, open_fds, staging = {}, {}, false
local STAGE = "/temp0/ghostldr-manager.tar"

local function open(p, flags, mode)
    write_string(path, p)
    local fd = kopen(path, flags, mode or 0)
    if fd_ok(fd) then open_fds[fd] = true return fd end
    return nil, fd
end
local function close(fd)
    open_fds[fd] = nil
    return kclose(fd)
end
local function stat(p)
    write_string(path, p)
    if kstat(path, st) ~= 0 then return nil end
    local s = read_buffer(st, 0x60)
    return string.unpack("<I2", s, 9), string.unpack("<i8", s, 0x29), string.unpack("<i8", s, 0x49)
end

local function set_dir(name)
    if not M.valid_container(name) then error("bad container name") end
    memset(dirbuf, 0, 32)
    write_string(dirbuf, name)
end

-- mode 1 read-only, 2 read-write, 6 create + read-write
local function mount(name, mode, blocks)
    if M.PROTECTED[name] then error("refusing to mount " .. name) end
    set_dir(name)
    memset(params, 0, 128)
    memset(result, 0, 64)
    write32(params, USER)
    if MOUNT2 then
        write64(params + 0x08, dirbuf)
        write64(params + 0x10, blocks or 0)
        write32(params + 0x18, mode)
    else
        write64(params + 0x10, dirbuf)
        write64(params + 0x20, blocks or 0)
        write32(params + 0x28, mode)
    end
    local rc = sd_mount(params, result)
    if rc ~= 0 then return nil, rc, read64(result + 0x10) end
    local mp = cstr(result, 16)
    if not mp:match("^/savedata%d+$") or mp == "/savedata0" then error("unexpected mount point " .. mp) end
    mounted[mp] = name
    return mp
end

-- the unmount is the commit (asynchronous): wait until the point is gone
local function umount(mp)
    if not mounted[mp] then return 0 end
    memset(mp_buf, 0, 16)
    write_string(mp_buf, mp)
    local rc = sd_umount(mp_buf)
    local polls = 0
    while polls < 240 and kstat(mp_buf, st) == 0 do
        usleep(250000)
        polls = polls + 1
    end
    if polls < 240 then mounted[mp] = nil end
    return rc
end

local function delete(name)
    if M.PROTECTED[name] then error("refusing to delete " .. name) end
    for mp, n in pairs(mounted) do if n == name then umount(mp) end end
    set_dir(name)
    memset(del, 0, 64)
    write32(del, USER)
    write64(del + 0x10, dirbuf)
    return sd_delete(del)
end

-- every save container of SWRR's title for this user
local function list_containers()
    memset(s_cond, 0, 0x40)
    memset(s_res, 0, 0x40)
    write32(s_cond, USER)
    write64(s_res + 0x08, s_names)
    write32(s_res + 0x10, SEARCH_MAX)
    write64(s_res + 0x18, s_params)
    write64(s_res + 0x20, s_infos)
    local rc = sd_search(s_cond, s_res)
    if rc ~= 0 then return nil, rc end
    local hits, set = read32(s_res), read32(s_res + 0x14)
    local n = math.min(hits, set, SEARCH_MAX)
    local names = read_buffer(s_names, math.max(64, 32 * n))
    local infos = read_buffer(s_infos, math.max(64, 0x30 * n))
    local out = {}
    for i = 0, n - 1 do
        local blocks, free = string.unpack("<I8I8", infos, i * 0x30 + 1)
        out[#out + 1] = {name = names:sub(i * 32 + 1, i * 32 + 32):match("^[^%z]*"), blocks = blocks, free = free,
                         mtime = read64(s_params + i * 0x530 + 0x508)}
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out, hits
end
local function find(listing, name)
    for _, c in ipairs(listing) do if c.name == name then return c end end
end

-- the files and folders below root, parents first
local function walk(root, only)
    local out = {}
    local function visit(rel, depth)
        local fd, rc = open(rel == "" and root or root .. "/" .. rel, 0x20000)     -- O_DIRECTORY
        if not fd then error("cannot list " .. (rel == "" and root or rel) .. ": " .. M.errtext(rc)) end
        local names = {}
        while true do
            local n = kgetdents(fd, dents, 0x4000)
            if failed(n) or n == 0 then break end
            local blob = read_buffer(dents, math.max(n, 64))
            local off = 0
            while off < n do
                local reclen, _, nlen = string.unpack("<I2BB", blob, off + 5)
                if reclen == 0 then break end
                local nm = blob:sub(off + 9, off + 8 + nlen)
                if nm ~= "." and nm ~= ".." then names[#names + 1] = nm end
                off = off + reclen
            end
        end
        close(fd)
        table.sort(names)
        for _, nm in ipairs(names) do
            local r = rel == "" and nm or rel .. "/" .. nm
            if not only or r == only or r:sub(1, #only + 1) == only .. "/" then
                local mode, mtime, size = stat(root .. "/" .. r)
                if mode then
                    local isdir = (mode & 0xF000) == 0x4000
                    out[#out + 1] = {path = r, dir = isdir, size = isdir and 0 or size, mtime = mtime}
                    if #out > M.MAX_FILES then error("more than " .. M.MAX_FILES .. " files") end
                    if isdir then
                        if depth >= M.MAX_DEPTH then error("folders nested too deep") end
                        visit(r, depth + 1)
                    end
                end
            end
        end
    end
    visit("", 1)
    return out
end

-- blocks the save's own files take in a mounted container (everything but
-- sce_sys): a restore frees them first
local function reclaimable(mp)
    local n = 0
    for _, it in ipairs(walk(mp)) do
        if not M.is_system(it.path) then n = n + (it.dir and 1 or M.file_blocks(it.size)) end
    end
    return n
end

-- remove every file and folder except sce_sys
local function clear(mp)
    local items = walk(mp)
    for i = #items, 1, -1 do
        local it = items[i]
        if not M.is_system(it.path) then
            write_string(path, mp .. "/" .. it.path)
            local rc = it.dir and krmdir(path) or kunlink(path)
            if rc ~= 0 then return nil, "remove " .. it.path .. ": " .. M.errtext(rc) end
        end
    end
    return true
end

-- release whatever a request left open or mounted (commits partial writes)
local function cleanup()
    for fd in pairs(open_fds) do kclose(fd) end
    open_fds = {}
    for mp in pairs(mounted) do umount(mp) end
    if staging then
        write_string(path, STAGE)
        kunlink(path)
        staging = false
    end
end

-- ---------------------------------------------------------------- titles
local header_cache, name_cache = {}, {}

-- the title's name and version from its param.sfo, through the index
local function lookup_name(fd, h)
    local function read_at(off, n)
        local got = kpread(fd, small, n, off)
        if failed(got) or got <= 0 then return string.rep("\0", n) end
        return read_buffer(small, math.max(n, 64)):sub(1, n)
    end
    local off, size = M.index_find(read_at, h, "sce_sys/param.sfo")
    if not off or size == 0 or size > 0xF000 then return nil end
    local k, within = off // h.cap, off % h.cap
    if within + size > h.cap then return nil end
    local mp = mount(string.format("%s%03d", h.prefix, k), 1, 0)
    if not mp then return nil end
    local sfo
    local dfd = open(mp .. "/data.bin", 0)
    if dfd then
        local got = kpread(dfd, small, size, within)
        if got == size then sfo = M.sfo_parse(read_buffer(small, math.max(size, 64)):sub(1, size)) end
        close(dfd)
    end
    umount(mp)
    if not sfo then return nil end
    return {name = sfo.TITLE, version = sfo.APP_VER or sfo.VERSION}
end

local function read_index(c)
    local key = c.name .. ":" .. tostring(c.mtime)
    if header_cache[key] ~= nil then return header_cache[key] end
    local mp = mount(c.name, 1, 0)
    if not mp then return false end
    local h = false
    local fd = open(mp .. "/index.bin", 0)
    if fd then
        local n = kpread(fd, small, 128, 0)
        if not failed(n) and n >= 64 then h = M.index_header(read_buffer(small, 128):sub(1, n)) or false end
        local nkey = h and (h.layout ~= "" and h.layout or h.prefix .. ":" .. h.total)
        if h and name_cache[nkey] == nil and not (TITLES[h.title] or M.KNOWN[h.title]) then
            local ok, nm = pcall(lookup_name, fd, h)
            name_cache[nkey] = ok and nm or false
        end
        if h then name_cache[h.prefix] = name_cache[nkey] or nil end
        close(fd)
    end
    umount(mp)
    header_cache[key] = h
    return h
end

-- GFSCATALOG/catalog.bin, parsed (nil without a readable one)
local cat_cache = {}
local function read_catalogue(c)
    local key = tostring(c.mtime)
    if cat_cache[key] ~= nil then return cat_cache[key] or nil end
    local mp = mount(M.CATALOG, 1, 0)
    local cat = false
    if mp then
        local _, _, size = stat(mp .. "/catalog.bin")
        local fd = size and size >= 64 and size <= 0x10000 and open(mp .. "/catalog.bin", 0)
        if fd then
            local n = kpread(fd, small, size, 0)
            if n == size then cat = M.catalogue_parse(read_buffer(small, math.max(size, 64)):sub(1, size)) or false end
            close(fd)
        end
        umount(mp)
    end
    cat_cache[key] = cat
    return cat or nil
end

-- replace catalog.bin (GFSCATALOG must exist: the manager never creates it)
local function write_catalogue(raw)
    if #raw > 0x10000 then return nil, "catalogue too large" end
    local mp, rc = mount(M.CATALOG, 2, 0)
    if not mp then return nil, "mount " .. M.CATALOG .. ": " .. M.errtext(rc) end
    write_buffer(buf, raw)
    local fd, orc = open(mp .. "/catalog.bin", 0x601, 0x1b6)
    local w = fd and kwrite(fd, buf, #raw) or orc
    local crc = fd and close(fd) or 0
    local urc = umount(mp)
    if not fd or w ~= #raw then return nil, "write catalog.bin: " .. M.errtext(w) end
    cat_cache = {}
    if crc ~= 0 or urc ~= 0 then return nil, "commit catalog.bin: " .. M.errtext(crc ~= 0 and crc or urc) end
    return true
end

local auth = {fails = 0, wait_until = 0}
local function state()
    local listing, hits = list_containers()
    if not listing then return nil, "listing the save containers failed: " .. M.errtext(hits) end
    local headers, cat = {}, nil
    for _, c in ipairs(listing) do
        if M.is_index_name(c.name) then headers[c.name] = read_index(c) end
        if c.name == M.CATALOG then cat = read_catalogue(c) end
    end
    local names = {}
    for k, v in pairs(name_cache) do if v and not k:find(":", 1, true) then names[k] = v end end
    local s = M.classify(listing, headers, {own = OWN, scratch = SCRATCH, names = names, titles = TITLES,
                                            catalogue = cat})
    s.ok, s.version, s.port = true, M.VERSION, PORT
    s.swrr = M.obj{title = TITLE_ID, region = M.REGION[TITLE_ID or ""], save = OWN}
    s.user = string.format("%08x", USER)
    s.containers, s.truncated = hits, hits > SEARCH_MAX
    s.lua_kib = math.floor(collectgarbage("count"))
    s.free_note = "not available from SWRR's save data API"
    s.writes_locked = auth.fails >= 30
    return M.obj(s), listing, cat
end

-- ---------------------------------------------------------------- HTTP I/O
local responded = false
local function send_ptr(fd, p, n)
    local sent = 0
    while sent < n do
        local w = sendto(fd, p + sent, n - sent, MSG_NOSIGNAL, 0, 0)
        if w <= 0 or w > n - sent then return false end
        sent = sent + w
    end
    return true
end
local function send_str(fd, s)
    if #s == 0 then return true end
    return send_ptr(fd, addrof(s) + 0x20, #s)
end
local function reply(fd, code, ctype, body, extra)
    responded = true
    send_str(fd, M.response_head(code, ctype, #body, extra))
    send_str(fd, body)
end
local function reply_json(fd, t) reply(fd, 200, "application/json", M.json(t)) end
local function reply_err(fd, code, msg)
    say(string.format("%d %s", code, msg))
    reply(fd, code, "application/json", M.json(M.obj{ok = false, error = msg}))
end

local function recv_exact(fd, p, n)
    local got = 0
    while got < n do
        local r = syscall.recvfrom(fd, p + got, n - got, MSG_WAITALL, 0, 0)
        if r <= 0 or r > n - got then return got end
        got = got + r
    end
    return got
end

-- the request head; the body bytes that came with it stay in hbuf
local function read_head(fd)
    local got = 0
    while got < 16384 do
        local r = syscall.recvfrom(fd, hbuf + got, 16384 - got, 0, 0, 0)
        if r <= 0 or r > 16384 - got then return nil end
        got = got + r
        local s = read_buffer(hbuf, math.max(got, 64)):sub(1, got)
        local e = s:find("\r\n\r\n", 1, true)
        if e then return s:sub(1, e + 3), hbuf + e + 3, got - e - 3 end
    end
    return nil, 431
end

local function set_timeout(fd, opt, sec)
    write64(tv, sec)
    write64(tv + 8, 0)
    syscall.setsockopt(fd, SOL_SOCKET, opt, tv, 16)
end

-- ---------------------------------------------------------------- handlers
local function container_root(name, listing)
    if not M.valid_container(name) then return nil, 400, "not a valid container name" end
    if not find(listing, name) then return nil, 404, "no container " .. name end
    if M.PROTECTED[name] then
        if name ~= OWN then return nil, 403, name .. " belongs to the other SWRR region" end
        return "/savedata0"                -- SWRR's own save: mounted by SWRR, read in place
    end
    local mp, rc = mount(name, 1, 0)
    if not mp then return nil, 409, "mount " .. name .. ": " .. M.errtext(rc) end
    return mp
end

local function h_files(fd, q)
    local listing, err = list_containers()
    if not listing then return reply_err(fd, 500, M.errtext(err)) end
    local root, code, msg = container_root(q.c, listing)
    if not root then return reply_err(fd, code, msg) end
    local items = walk(root)
    local out = {}
    for _, it in ipairs(items) do out[#out + 1] = M.obj{path = it.path, dir = it.dir, size = it.size} end
    reply_json(fd, M.obj{ok = true, container = q.c, files = out})
end

local io_time = {read = 0, send = 0}
local function stream_file(fd, full, size)
    local f = open(full, 0)
    if not f then return false end
    local left = size
    while left > 0 do
        local want = math.min(CHUNK, left)
        local t0 = gettime()
        local n = kread(f, buf, want)
        local t1 = gettime()
        if n <= 0 or n > want then close(f) return false end
        if not send_ptr(fd, buf, n) then close(f) return false end
        io_time.read, io_time.send = io_time.read + t1 - t0, io_time.send + gettime() - t1
        left = left - n
    end
    close(f)
    local pad = M.pad512(size) - size
    return pad == 0 or send_ptr(fd, zeros, pad)
end

local function h_download(fd, q)
    local listing, err = list_containers()
    if not listing then return reply_err(fd, 500, M.errtext(err)) end
    local name = q.c
    local root, code, msg = container_root(name, listing)
    if not root then return reply_err(fd, code, msg) end
    local info = find(listing, name)
    local items, unreadable, list = {}, {}, {}
    for _, it in ipairs(walk(root)) do
        local ok = it.dir
        if not ok then
            local f = open(root .. "/" .. it.path, 0)
            if f then close(f); ok = true end
        end
        if ok and M.tar_header(name .. "/" .. it.path, it.size, it.mtime, it.dir) then
            items[#items + 1] = it
            if not it.dir then list[#list + 1] = M.obj{path = it.path, size = it.size} end
        else
            unreadable[#unreadable + 1] = it.path
        end
    end
    local kind, prefix = M.save_kind(name)
    local man = M.json(M.obj{format = "ghostldr-save", version = 1, container = name, kind = kind, prefix = prefix,
                             blocks = info.blocks, free_blocks = info.free, mtime = info.mtime,
                             exported = os.time(), swrr = TITLE_ID, files = list, unreadable = unreadable}) .. "\n"
    local stamp = os.date("%Y%m%d-%H%M%S")
    local total = M.tar_length(#man, items)
    responded = true
    if not send_str(fd, M.response_head(200, "application/x-tar", total,
            {string.format('Content-Disposition: attachment; filename="%s-%s.tar"', name, stamp)})) then return end
    local t0 = gettime()
    io_time.read, io_time.send = 0, 0
    if not send_str(fd, M.tar_header(M.MANIFEST, #man, os.time(), false)) or not send_str(fd, man)
            or not send_ptr(fd, zeros, M.pad512(#man) - #man) then return end
    for _, it in ipairs(items) do
        if not send_str(fd, (M.tar_header(name .. "/" .. it.path, it.size, it.mtime, it.dir))) then return end
        if not it.dir and not stream_file(fd, root .. "/" .. it.path, it.size) then
            say("download " .. name .. ": stopped in " .. it.path)
            return
        end
    end
    send_ptr(fd, zeros, 1024)
    say(string.format("download %s: %d bytes, %d files in %.1f s (reading %.1f s, sending %.1f s)", name, total,
        #list, (gettime() - t0) / 1e6, io_time.read / 1e6, io_time.send / 1e6))
end

-- auth: the session token, with a growing wait after wrong ones
local TOKEN
local function check_token(req)
    if auth.fails >= 30 then return false, 403, "writes are locked: too many wrong tokens (restart the manager)" end
    if os.time() < auth.wait_until then return false, 429, "too many wrong tokens: wait a little" end
    local given = req.headers["x-ghostldr-token"]
    if given and M.token_norm(given) == M.token_norm(TOKEN) then return true end
    auth.fails = auth.fails + 1
    if auth.fails >= 5 then auth.wait_until = os.time() + math.min(300, 1 << (auth.fails - 4)) end
    return false, 403, given and "wrong token" or "the token is missing: unlock the page first"
end

-- room check for writing `need` blocks of files into `name`
local function space_check(listing, name, need, want)
    local c = find(listing, name)
    if not c then
        local blocks = M.create_blocks(need, want)
        if not blocks then
            return {fits = false, exists = false, message = string.format("Needs %d blocks (%.0f MiB): more than "
                .. "a container can hold (%d).", need, need / 32, M.MAX_BLOCKS)}
        end
        return {fits = true, exists = false, blocks = blocks, message = string.format(
            "A new container of %d blocks (%.1f MiB) is created; the files take about %d.", blocks, blocks / 32, need)}
    end
    local mp, rc = mount(name, 1, 0)
    if not mp then return {fits = false, exists = true, message = "mount " .. name .. ": " .. M.errtext(rc)} end
    local reclaim = reclaimable(mp)
    umount(mp)
    local avail = c.free + reclaim
    return {fits = need <= avail, exists = true, blocks = c.blocks, available = avail, message = string.format(
        "%s has %d blocks (%.1f MiB): %d free, %d more once its current files go; the files need about %d.%s",
        name, c.blocks, c.blocks / 32, c.free, reclaim, need,
        need <= avail and "" or " Too small: delete it first to restore into a new, larger one.")}
end

local function h_check(fd, q)
    local group, why = M.write_target(q.c, SCRATCH)
    if not group then return reply_err(fd, 403, why) end
    local listing, err = list_containers()
    if not listing then return reply_err(fd, 500, M.errtext(err)) end
    local r = space_check(listing, q.c, math.tointeger(tonumber(q.need or "")) or 0, 0)
    r.ok = true
    reply_json(fd, M.obj(r))
end

-- write plan.files from the staged archive (sfd) into mp
local function write_files(mp, plan, sfd)
    for _, d in ipairs(plan.dirs) do
        write_string(path, mp .. "/" .. d)
        local rc = kmkdir(path, 0x1ff)
        if rc ~= 0 and u32(rc) ~= 0x80020011 then return nil, "mkdir " .. d .. ": " .. M.errtext(rc) end
    end
    for _, f in ipairs(plan.files) do
        local out, rc = open(mp .. "/" .. f.path, 0x601, 0x1b6)      -- O_WRONLY | O_CREAT | O_TRUNC
        if not out then return nil, "create " .. f.path .. ": " .. M.errtext(rc) end
        local done = 0
        while done < f.size do
            local want = math.min(CHUNK, f.size - done)
            local n = kpread(sfd, buf, want, f.off + done)
            if n ~= want then close(out) return nil, "read the upload: " .. M.errtext(n) end
            local w = kwrite(out, buf, want)
            if w ~= want then close(out) return nil, "write " .. f.path .. ": " .. M.errtext(w) end
            done = done + want
        end
        local crc = close(out)
        if crc ~= 0 then return nil, "close " .. f.path .. ": " .. M.errtext(crc) end
    end
    return true
end

local function verify(name, files)
    local mp, rc = mount(name, 1, 0)
    if not mp then return "mount for verify: " .. M.errtext(rc) end
    local bad = {}
    for _, f in ipairs(files) do
        local _, _, size = stat(mp .. "/" .. f.path)
        if size ~= f.size then bad[#bad + 1] = f.path end
    end
    umount(mp)
    if #bad > 0 then return "size mismatch after the commit: " .. table.concat(bad, ", ") end
    return nil
end

-- Restore: stage the upload in /temp0, check it, then replace the container's
-- files (create it if missing).  Nothing is touched before the whole archive
-- has arrived and parsed.
local function h_restore(fd, q, req, pre_ptr, pre_len)
    local name = q.c
    local group, why = M.write_target(name, SCRATCH)
    if not group then return reply_err(fd, 403, why) end
    if q.confirm ~= name then return reply_err(fd, 400, "confirm=" .. name .. " is required") end
    local len = math.tointeger(tonumber(req.headers["content-length"] or ""))
    if not len then return reply_err(fd, 411, "Content-Length is required") end
    if len < 1024 or len > M.MAX_UPLOAD then return reply_err(fd, 413, "the archive must be 1 KiB to 1 GiB") end
    local sfd, rc = open(STAGE, 0x602, 0x1b6)                         -- O_RDWR | O_CREAT | O_TRUNC
    if not sfd then return reply_err(fd, 503, "cannot stage the upload in /temp0: " .. M.errtext(rc)) end
    staging = true
    -- 1. receive
    set_timeout(fd, SO_RCVTIMEO, 30)
    local done = math.min(pre_len, len)
    if done > 0 and kwrite(sfd, pre_ptr, done) ~= done then return reply_err(fd, 503, "staging write failed") end
    local t0 = gettime()
    while done < len do
        local want = math.min(CHUNK, len - done)
        local got = recv_exact(fd, buf, want)
        if got > 0 then
            local w = kwrite(sfd, buf, got)
            if w ~= got then return reply_err(fd, 503, "staging write failed (is /temp0 full?): " .. M.errtext(w)) end
        end
        done = done + got
        if got ~= want then
            say(string.format("restore %s: upload stopped after %d of %d bytes", name, done, len))
            return reply_err(fd, 400, "the upload stopped early: nothing was changed")
        end
    end
    say(string.format("restore %s: %d bytes received in %.1f s", name, len, (gettime() - t0) / 1e6))
    -- 2. parse and check
    local function read_at(off, n)
        local got = kpread(sfd, small, n, off)
        if got ~= n then error("reading the staged upload failed") end
        return read_buffer(small, math.max(n, 64)):sub(1, n)
    end
    local entries, err = M.tar_scan(read_at, len)
    if not entries then return reply_err(fd, 400, err .. ": nothing was changed") end
    local strip = q.strip
    if strip == nil or strip == "auto" then strip = M.auto_strip(entries) end
    if strip ~= "" and not M.clean_path(strip) then return reply_err(fd, 400, "bad strip folder") end
    local plan
    plan, err = M.restore_plan(entries, strip)
    if not plan then return reply_err(fd, 400, err .. ": nothing was changed") end
    local want = math.tointeger(tonumber(q.blocks or "")) or 0
    if plan.manifest and plan.manifest.size <= 0x10000 then
        want = math.max(want, M.manifest_blocks(read_at(plan.manifest.off, plan.manifest.size)) or 0)
    end
    local need = M.blocks_needed(plan)
    local listing
    listing, err = list_containers()
    if not listing then return reply_err(fd, 500, M.errtext(err)) end
    local chk = space_check(listing, name, need, want)
    if not chk.fits then return reply_err(fd, 409, chk.message) end
    -- 3. write
    local mp, mrc, required
    if chk.exists then
        mp, mrc = mount(name, 2, 0)
        if not mp then return reply_err(fd, 409, "mount " .. name .. ": " .. M.errtext(mrc)) end
        local ok, cerr = clear(mp)
        if not ok then return reply_err(fd, 500, cerr .. " (the save may be partly cleared)") end
    else
        mp, mrc, required = mount(name, 6, chk.blocks)
        if not mp then
            local extra = u32(mrc) == 0x809F000A and string.format(" (%d more blocks needed)", required) or ""
            return reply_err(fd, 409, "create " .. name .. ": " .. M.errtext(mrc) .. extra)
        end
    end
    local ok, werr = write_files(mp, plan, sfd)
    local urc = umount(mp)
    if not ok then return reply_err(fd, 500, werr .. ": " .. name .. " is now incomplete, restore again") end
    if urc ~= 0 then return reply_err(fd, 500, "commit " .. name .. ": " .. M.errtext(urc)) end
    local verr = verify(name, plan.files)
    if verr then return reply_err(fd, 500, verr) end
    say(string.format("restore %s: %d files, %d bytes%s", name, #plan.files, plan.bytes,
        chk.exists and "" or string.format(" (created, %d blocks)", chk.blocks)))
    reply_json(fd, M.obj{ok = true, container = name, files = #plan.files, bytes = plan.bytes,
                         created = not chk.exists, blocks = chk.blocks, skipped = plan.skipped})
end

local function h_copy(fd, q)
    local from, to = q.from, q.to
    local gfrom, pfrom = M.write_target(from, SCRATCH)
    local gto, pto = M.write_target(to, SCRATCH)
    if not gfrom then return reply_err(fd, 403, pfrom) end
    if not gto then return reply_err(fd, 403, pto) end
    if gfrom ~= gto or pfrom ~= pto then return reply_err(fd, 403, "copies stay within one title's containers") end
    if from == to then return reply_err(fd, 400, "source and target are the same") end
    if q.confirm ~= to then return reply_err(fd, 400, "confirm=" .. to .. " is required") end
    local listing, err = list_containers()
    if not listing then return reply_err(fd, 500, M.errtext(err)) end
    local src = find(listing, from)
    if not src then return reply_err(fd, 404, "no container " .. from) end
    local smp, rc = mount(from, 1, 0)
    if not smp then return reply_err(fd, 409, "mount " .. from .. ": " .. M.errtext(rc)) end
    local plan = {files = {}, dirs = {}}
    for _, it in ipairs(walk(smp)) do
        if not M.is_system(it.path) then
            if it.dir then plan.dirs[#plan.dirs + 1] = it.path
            else plan.files[#plan.files + 1] = {path = it.path, size = it.size} end
        end
    end
    if #plan.files == 0 then return reply_err(fd, 400, from .. " holds no files") end
    local chk = space_check(listing, to, M.blocks_needed(plan), src.blocks)
    if not chk.fits then return reply_err(fd, 409, chk.message) end
    local dmp, drc
    if chk.exists then
        dmp, drc = mount(to, 2, 0)
        if dmp then
            local ok, cerr = clear(dmp)
            if not ok then return reply_err(fd, 500, cerr) end
        end
    else
        dmp, drc = mount(to, 6, chk.blocks)
    end
    if not dmp then return reply_err(fd, 409, "mount " .. to .. ": " .. M.errtext(drc)) end
    local bytes, werr = 0, nil
    for _, d in ipairs(plan.dirs) do
        write_string(path, dmp .. "/" .. d)
        local mrc = kmkdir(path, 0x1ff)
        if mrc ~= 0 and u32(mrc) ~= 0x80020011 then werr = "mkdir " .. d .. ": " .. M.errtext(mrc) break end
    end
    for _, f in ipairs(plan.files) do
        if werr then break end
        local i = open(smp .. "/" .. f.path, 0)
        local o, orc = open(dmp .. "/" .. f.path, 0x601, 0x1b6)
        if not i or not o then werr = "open " .. f.path .. ": " .. M.errtext(orc or 0) break end
        local left = f.size
        while left > 0 do
            local want = math.min(CHUNK, left)
            local n = kread(i, buf, want)
            if n ~= want then werr = "read " .. f.path break end
            local w = kwrite(o, buf, want)
            if w ~= want then werr = "write " .. f.path .. ": " .. M.errtext(w) break end
            left = left - want
        end
        close(i)
        local crc = close(o)
        if not werr and crc ~= 0 then werr = "close " .. f.path .. ": " .. M.errtext(crc) end
        bytes = bytes + f.size - left
    end
    local urc = umount(dmp)
    umount(smp)
    if werr then return reply_err(fd, 500, werr .. ": " .. to .. " is now incomplete") end
    if urc ~= 0 then return reply_err(fd, 500, "commit " .. to .. ": " .. M.errtext(urc)) end
    local verr = verify(to, plan.files)
    if verr then return reply_err(fd, 500, verr) end
    say(string.format("copy %s -> %s: %d files, %d bytes", from, to, #plan.files, bytes))
    reply_json(fd, M.obj{ok = true, from = from, to = to, files = #plan.files, bytes = bytes, created = not chk.exists})
end

local function h_delete(fd, q)
    local name = q.c
    local group, why = M.write_target(name, SCRATCH)
    if not group and not M.probe_name(name) then return reply_err(fd, 403, why) end
    if q.confirm ~= name then return reply_err(fd, 400, "confirm=" .. name .. " is required") end
    local listing, err = list_containers()
    if not listing then return reply_err(fd, 500, M.errtext(err)) end
    if not find(listing, name) then return reply_err(fd, 404, "no container " .. name) end
    local rc = delete(name)
    if rc ~= 0 then return reply_err(fd, 500, "delete " .. name .. ": " .. M.errtext(rc)) end
    say("deleted " .. name)
    reply_json(fd, M.obj{ok = true, deleted = name})
end

-- A title's installed data, in the catalogue's order: its entry marked
-- INCOMPLETE, its index (an install without one never boots, and the
-- installer starts over), the data containers, then the entry dropped.  Its
-- saves only with saves=1.  Without a catalogue (a v0.1 install) the same
-- minus the catalogue steps; the manager never creates GFSCATALOG.
local function h_delete_title(fd, q)
    local s, listing, cat = state()
    if not s then return reply_err(fd, 500, listing) end
    local t
    for _, x in ipairs(s.titles) do if x.prefix == q.prefix then t = x end end
    if not t then return reply_err(fd, 404, "no installed title with prefix " .. tostring(q.prefix)) end
    if q.confirm ~= (t.id or t.prefix) then return reply_err(fd, 400, "confirm=" .. (t.id or t.prefix) .. " is required") end
    local entry = t.catalogued and cat and M.catalogue_update(cat, t.prefix, "mark", os.time())
    if entry then
        local ok, err = write_catalogue(entry)
        if not ok then return reply_err(fd, 500, "marking the title in " .. M.CATALOG .. ": " .. err .. "; nothing deleted") end
    end
    local names = {}
    local function add(n) if n and find(listing, n) and M.valid_container(n) then names[#names + 1] = n end end
    add(t.index)
    for _, n in ipairs(t.stale_indexes or {}) do add(n) end
    for _, c in ipairs(listing) do
        local p = M.data_kind(c.name)
        if p == t.prefix and not (SCRATCH and M.scratch_member(SCRATCH, c.name)) then add(c.name) end
    end
    if q.saves == "1" then
        for _, sv in ipairs(t.saves) do add(sv.name) end
    end
    local deleted, failed_list = {}, {}
    for _, n in ipairs(names) do
        local rc = (M.PROTECTED[n] or n == M.CATALOG) and -1 or delete(n)
        if rc == 0 or u32(rc) == 0x809F0008 then deleted[#deleted + 1] = n
        else failed_list[#failed_list + 1] = n .. " " .. M.errtext(rc) end
    end
    if entry and #failed_list == 0 then
        local listing2 = list_containers() or {}
        local c = find(listing2, M.CATALOG)
        local cat2 = c and read_catalogue(c)
        local dropped = cat2 and M.catalogue_update(cat2, t.prefix, "drop")
        local ok, err = true, nil
        if dropped then ok, err = write_catalogue(dropped) end
        if not ok then failed_list[#failed_list + 1] = "catalogue entry (" .. err .. ")" end
    end
    say(string.format("delete title %s: %d deleted, %d failed", t.id or t.prefix, #deleted, #failed_list))
    reply_json(fd, M.obj{ok = #failed_list == 0, deleted = deleted, failed = failed_list,
                         error = #failed_list > 0 and ("could not delete " .. table.concat(failed_list, ", ")) or nil})
end

-- ---------------------------------------------------------------- network install
-- What /install's page asks for (docs/manager.md "Install a game"): the
-- browser lays the game out; these store bytes in containers.  A data
-- container (<prefix>000..) arrives in one or more requests: the first
-- (off=0) creates it and opens data.bin, which stays open and mounted between
-- requests; the last commits and checks its size.  A dropped connection
-- leaves it open, so the page carries on from where the console got to
-- (/api/install/info's session).
local PUMP_HEX = "554889e54157415641554154534883e4f04883ec204889fb488b47304c8b67384e8d3c2049ffcf0f57c00f11476048c7477000000000c787a8000000000000000f1147784c89f84c09e048c1e820740a4c89f831d249f7f4eb084489f831d241f7f448890424488dbbb0000000488d154c0100004c8d052f02000031f64889d9ff531085c07408489848894360eb154d39fc761f488bbbb000000031f6ff5318488b4360488d65d85b415c415d415e415f5dc34531f6662e0f1f8400000000008b83a800000085c075ca4589f74183e7034489f88b44837885c0741f0f1f40008b83a800000085c07511bfc8000000ff5320428b44bb7885c075e58b83a800000085c0758f4c897424104c8b7330488b43384c2b73684939c64c0f43f04c897c24084e8b64fb4031c90f1f80000000004989cd4d89f74929cf76258b7b284b8d342c4c89fab9400000004531c04531c9ff134885c07e094a8d0c284c39f876d04c016b684d39ee752c488b4424084c89b4c388000000c7448378010000004c8b74241049ffc64c3b34240f8230ffffffe9fffeffff8b83a800000085c0750848c74360ffffffffc783a800000001000000e9defeffffcccccccccccccccccccc554889e5415741564154534883e4f04889fb488b4730488b7738488d0c3048ffc94889c84809f048c1e82074154889c831d248f7f64989c64839ce7617e99900000089c831d2f7f64189c64839ce0f87870000004531ff4589fc4183e4034489e08b44837885c075220f1f80000000008b83a800000085c07561bfc8000000ff5320428b44a37885c074e58b7b2c4a8b74e3404a8b94e388000000ff53084a3b84e38800000075174801437042c744a3780000000049ffc74d39f7729aeb1c4885c048c7c1feffffff480f48c848894b60c783a80000000100000031c0488d65e05b415c415e415f5dc36766732d70756d7000"
local inst = nil                 -- the data container being written
local pump, pump_ctx
local function pump_setup()
    if pump ~= nil or PUMP_HEX == "" or MOUNT2 or not (IPU_PROG_BASE and SHELLCODE_SCRATCH and jit_memcpy and EBOOT_OFFSETS) then
        pump = pump or false
        return
    end
    pump = false
    local blob = hex_to_binary(PUMP_HEX)
    local code = IPU_PROG_BASE + 0x4000
    write_buffer(SHELLCODE_SCRATCH, blob)
    jit_memcpy(code, SHELLCODE_SCRATCH, #blob)
    local bufs = {buf}
    for i = 2, 4 do bufs[i] = malloc(CHUNK) end
    local fns = {dlsym(LIBKERNEL_HANDLE, "recvfrom"), dlsym(LIBKERNEL_HANDLE, "sceKernelWrite"),
                 read64(EBOOT_OFFSETS.scePthreadCreate), read64(EBOOT_OFFSETS.scePthreadJoin),
                 dlsym(LIBKERNEL_HANDLE, "sceKernelUsleep")}
    local ready = read64(code) == string.unpack("<I8", blob, 1)
    for i = 1, 4 do ready = ready and bufs[i] ~= 0 end
    for i = 1, #fns do ready = ready and fns[i] ~= 0 end
    if ready then
        pump_ctx = malloc(0xb8)
        for i = 1, #fns do write64(pump_ctx + (i - 1) * 8, fns[i]) end
        write64(pump_ctx + 0x38, CHUNK)
        for i = 1, 4 do write64(pump_ctx + 0x40 + (i - 1) * 8, bufs[i]) end
        pump = func_wrap(code)
    end
    say(string.format("install: pump %s", ready and "ready" or "unavailable (the Lua loop)"))
end

local function inst_umount(mp)
    mounted[mp] = inst and inst.name or "?"
    return umount(mp)
end
-- close and commit the open data container (what it holds stays, untagged)
local function inst_close()
    if not inst then return 0 end
    local i = inst
    inst = nil
    if i.fd then kclose(i.fd) end
    return inst_umount(i.mp)
end

local function qint(v)
    local n = math.tointeger(tonumber(v or ""))
    return n
end
local function reply_rc(fd, rc, extra)
    local t = extra or {}
    t.ok, t.rc = true, u32(rc or 0)
    if t.rc ~= 0 then t.error = M.errtext(t.rc) end
    reply_json(fd, M.obj(t))
end
local function inst_check(fd, op, name)
    local kind = M.install_kind(name)
    if not kind or (op and not M.INSTALL_OPS[op][kind]) then
        reply_err(fd, 403, tostring(name) .. ": not a container the installer may " .. (op or "read"))
        return nil
    end
    return kind
end

-- up to n bytes of f from off into buf: the save call's status (0, a mount
-- error, or -1: no such file) and how many.  The bytes stay in buf: the Lua
-- GC is off, so a string of each read would pile up (4 MiB reads of a data
-- container ran SWRR out of memory on a PS4).
local function inst_read(name, f, off, n)
    local mp, rc = mount(name, 1, 0)
    if not mp then return rc, 0 end
    local h = open(mp .. "/" .. f, 0)
    local got = -1
    if h then
        got = kpread(h, buf, math.min(n, CHUNK), off)
        close(h)
    end
    umount(mp)
    if got < 0 or got > CHUNK then return -1, 0 end
    return 0, got
end
local function buf_string(n) return n > 0 and read_buffer(buf, math.max(n, 64)):sub(1, n) or "" end

local function h_inst_info(fd)
    local se = inst and M.obj{c = inst.name, size = inst.size, written = inst.written, lid = inst.lid} or nil
    reply_json(fd, M.obj{ok = true, server = "manager", version = 1, swrr = TITLE_ID,
                         platform = PLATFORM and tostring(PLATFORM):lower() or nil, token = true, session = se,
                         pump = pump and true or false, max_write = CHUNK})
end

local function h_inst_read(fd, q)
    if not inst_check(fd, nil, q.c) then return end
    if not M.install_file_ok(q.f) then return reply_err(fd, 400, "bad file name") end
    local off, n = qint(q.off) or 0, qint(q.len) or 0
    if off < 0 or n < 0 then return reply_err(fd, 400, "bad range") end
    local rc, got = inst_read(q.c, q.f, off, n)
    responded = true
    if send_str(fd, M.response_head(200, "application/octet-stream", got, {"X-Gfs-Status: " .. tostring(u32(rc))}))
            and got > 0 then
        send_ptr(fd, buf, got)
    end
end

local function h_inst_probe(fd, q)
    local out = {}
    for name in (q.c or ""):gmatch("[^,]+") do
        if #out >= 16 or not inst_check(fd, nil, name) then
            if #out >= 16 then reply_err(fd, 400, "at most 16 containers") end
            return
        end
        local rc, got = inst_read(name, "gfs.tag", 0, 16)
        local hex = rc == 0 and (buf_string(got):gsub(".", function(ch) return string.format("%02x", ch:byte()) end))
            or nil
        out[#out + 1] = M.obj{name = name, exists = u32(rc) ~= 0x809F0008, tag = hex}
    end
    reply_json(fd, M.obj{ok = true, containers = out})
end

-- the request's body, whole (at most CHUNK bytes) into buf: its length, or nil
local function body_into_buf(fd, req, pre_ptr, pre_len)
    local len = qint(req.headers["content-length"])
    if not len or len < 0 or len > CHUNK then return nil end
    local pre = math.min(pre_len, len)
    for i = 0, pre - 1 do write8(buf + i, read8(pre_ptr + i)) end
    if pre < len and recv_exact(fd, buf + pre, len - pre) ~= len - pre then return nil end
    return len
end

-- write n bytes of buf as f into the mounted mp: 0, or an error
local function put_file(mp, f, n)
    local h, rc = open(mp .. "/" .. f, 0x601, 0x1b6)
    if not h then return rc end
    local w = n > 0 and kwrite(h, buf, n) or 0
    local crc = close(h)
    if w ~= n then return w < 0 and w or -2 end
    return crc
end

local function h_inst_write(fd, q, req, pre_ptr, pre_len, create)
    if not inst_check(fd, create and "put" or "write", q.c) then return end
    if not M.install_file_ok(q.f) then return reply_err(fd, 400, "bad file name") end
    local blocks = qint(q.blocks) or 0
    if create and (blocks < M.MIN_BLOCKS or blocks > M.MAX_BLOCKS) then return reply_err(fd, 400, "bad blocks") end
    local n = body_into_buf(fd, req, pre_ptr, pre_len)
    if not n then return reply_err(fd, 400, "the body must be 0 to 4 MiB, with Content-Length") end
    if inst and inst.name == q.c then inst_close() end
    local mp, rc
    if create then
        delete(q.c)
        mp, rc = mount(q.c, 6, blocks)
    else
        mp, rc = mount(q.c, 2, 0)
    end
    if not mp then return reply_rc(fd, rc) end
    local wrc = put_file(mp, q.f, n)
    local urc = umount(mp)
    if wrc ~= 0 then return reply_rc(fd, wrc) end
    if urc ~= 0 then return reply_rc(fd, urc) end
    if create then          -- check what was committed
        local vrc, got = inst_read(q.c, q.f, 0, n)
        if vrc ~= 0 or got ~= n then return reply_rc(fd, vrc ~= 0 and vrc or -2) end
    end
    say(string.format("install: %s %s/%s, %d bytes", create and "put" or "write", q.c, q.f, n))
    reply_rc(fd, 0)
end

local function h_inst_delete(fd, q)
    if not inst_check(fd, "delete", q.c) then return end
    if inst and inst.name == q.c then inst_close() end
    local rc = delete(q.c)
    say(string.format("install: delete %s -> %s", q.c, M.errtext(rc)))
    reply_rc(fd, rc)
end

-- whether `want` bytes fit: 1 GiB probes GFSFREE00.. until they do or the
-- storage is full (NO_SPACE_FS reports the blocks missing), then deleted
local function h_inst_free(fd, q)
    local want = qint(q.bytes) or 0
    local have, made, status = 0, 0, 0
    while have < want and made < 100 do
        local name = string.format("GFSFREE%02d", made)
        delete(name)
        local mp, rc, missing = mount(name, 6, M.MAX_BLOCKS)
        if not mp then
            if u32(rc) == 0x809F000A then
                if missing and missing < M.MAX_BLOCKS then have = have + (M.MAX_BLOCKS - missing) * M.BLOCK end
            else
                status = rc
            end
            break
        end
        umount(mp)
        made = made + 1
        have = have + (1 << 30)
    end
    for i = 0, made do delete(string.format("GFSFREE%02d", i)) end
    say(string.format("install: free %d MiB wanted, %d MiB fit (%d probes, %s)", want >> 20, have >> 20, made,
        M.errtext(status)))
    reply_rc(fd, status, {have = have})
end

local function verify_size(name, f, size)
    local mp, rc = mount(name, 1, 0)
    if not mp then return rc end
    local _, _, got = stat(mp .. "/" .. f)
    umount(mp)
    return got == size and 0 or -2
end

local function h_inst_data(fd, q, req, pre_ptr, pre_len)
    local name = q.c
    if not inst_check(fd, "data", name) then return end
    local size, blocks, off = qint(q.size), qint(q.blocks), qint(q.off)
    local len = qint(req.headers["content-length"])
    local lid = tostring(q.lid or "")
    if not (size and blocks and off and len) or size < 1 or blocks < M.MIN_BLOCKS or blocks > M.MAX_BLOCKS
            or size > blocks * M.BLOCK or off < 0 or len < 0 or off + len > size or #lid > 32 then
        return reply_err(fd, 400, "bad data request")
    end
    if off == 0 then
        inst_close()
        delete(name)
        local mp, rc, required = mount(name, 6, blocks)
        if not mp then
            local extra = u32(rc) == 0x809F000A and string.format(" (%d more blocks needed)", required or 0) or ""
            say("install: create " .. name .. ": " .. M.errtext(rc) .. extra)
            return reply_rc(fd, rc)
        end
        mounted[mp] = nil
        write_string(path, mp .. "/data.bin")
        local h = kopen(path, 0x601, 0x1b6)
        if not fd_ok(h) then
            mounted[mp] = name
            umount(mp)
            return reply_rc(fd, h)
        end
        inst = {name = name, size = size, blocks = blocks, lid = lid, written = 0, fd = h, mp = mp, t0 = gettime()}
    elseif not inst or inst.name ~= name or inst.size ~= size or inst.lid ~= lid or inst.written ~= off then
        local same = inst and inst.name == name and inst.size == size and inst.lid == lid
        return reply(fd, 409, "application/json", M.json(M.obj{ok = false, written = same and inst.written or 0,
            error = "the console has " .. (same and inst.written or 0) .. " bytes of " .. name}))
    end
    set_timeout(fd, SO_RCVTIMEO, 30)
    local ok, werr = true, 0
    local pre = math.min(pre_len, len)
    if pre > 0 then
        local w = kwrite(inst.fd, pre_ptr, pre)
        if w ~= pre then ok, werr = false, w end
        if ok then inst.written = inst.written + pre end
    end
    local rest, lost = len - pre, false
    if ok and rest > 0 then
        pump_setup()
        if pump then
            write32(pump_ctx + 0x28, fd)
            write32(pump_ctx + 0x2c, inst.fd)
            write64(pump_ctx + 0x30, rest)
            local st = pump(pump_ctx)
            inst.written = inst.written + read64(pump_ctx + 0x70)
            if st == -1 then lost = true elseif st ~= 0 then ok, werr = false, st end
        else
            local done = 0
            while done < rest do
                local want = math.min(CHUNK, rest - done)
                local got = recv_exact(fd, buf, want)
                if got > 0 then
                    local w = kwrite(inst.fd, buf, got)
                    if w ~= got then ok, werr = false, w break end
                    inst.written = inst.written + got
                    done = done + got
                end
                if got ~= want then lost = true break end
            end
        end
    end
    if not ok then
        say(string.format("install: %s: write -> %s after %d MiB", name, M.errtext(werr), inst.written >> 20))
        inst_close()
        return reply_rc(fd, werr ~= 0 and werr or -2)
    end
    if lost then
        say(string.format("install: %s: connection lost at %d MiB (kept open: the page carries on)", name,
            inst.written >> 20))
        responded = true
        return
    end
    if inst.written < size then return reply_json(fd, M.obj{ok = true, rc = 0, written = inst.written}) end
    local t0 = inst.t0
    local h = inst.fd
    inst.fd = nil
    local crc = kclose(h)
    local urc = inst_close()
    local millis = (gettime() - t0) // 1000
    if crc ~= 0 or urc ~= 0 then return reply_rc(fd, urc ~= 0 and urc or crc) end
    local vrc = verify_size(name, "data.bin", size)
    say(string.format("install: %s: %d MiB in %.1f s (%.1f MB/s%s), verify %s", name, size >> 20, millis / 1000,
        size / 1000 / math.max(millis, 1), pump and ", pump" or "", M.errtext(vrc)))
    if vrc ~= 0 then return reply_rc(fd, vrc) end
    reply_json(fd, M.obj{ok = true, rc = 0, written = size, done = true, millis = millis})
end

local function h_inst_note(fd, q, done)
    local what = (q.name and q.name ~= "" and q.name or q.title or "a game"):sub(1, 64)
    if done == nil then
        say(string.format("install: begin %s %s, %s bytes", tostring(q.title), what, tostring(q.total)))
        send_notification(string.format("ghostldr: installing %s", what))
    else
        say(string.format("install: end %s: %s", tostring(q.title), q.ok == "1" and "installed" or "stopped"))
        send_notification(q.ok == "1" and string.format("ghostldr: %s installed", what)
            or string.format("ghostldr: install of %s stopped", what))
    end
    reply_json(fd, M.obj{ok = true})
end

local PAGE_HEADERS = {"Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; "
    .. "style-src 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'", "X-Frame-Options: DENY"}
local running, last_note = true, 0
local IP

local function notify_token()
    send_notification(string.format("ghostldr manager\nhttp://%s:%d\ntoken %s", IP, PORT, TOKEN))
end

local function handle(fd)
    local head, pre_ptr, pre_len = read_head(fd)
    if not head then
        if pre_ptr == 431 then reply_err(fd, 431, "request head too large") end
        return
    end
    local req, err = M.parse_head(head)
    if not req then return reply_err(fd, 400, err) end
    local p, q = M.parse_target(req.target)
    local m = req.method
    if m == "GET" then
        if p == "/" or p == "/index.html" then return reply(fd, 200, "text/html; charset=utf-8", M.PAGE, PAGE_HEADERS) end
        if p == "/favicon.ico" then return reply(fd, 204, "text/plain", "") end
        if p == "/install" then return reply(fd, 200, "text/html; charset=utf-8", M.INSTALL_PAGE, PAGE_HEADERS) end
        if p == "/api/install/info" then return h_inst_info(fd) end
        if p == "/api/install/read" then return h_inst_read(fd, q) end
        if p == "/api/state" then
            local s, e = state()
            if not s then return reply_err(fd, 500, e) end
            return reply_json(fd, s)
        end
        if p == "/api/files" then return h_files(fd, q) end
        if p == "/api/download" then return h_download(fd, q) end
        return reply_err(fd, 404, "not found")
    elseif m == "POST" then
        if p == "/api/exit" or p == "/api/install/exit" then
            running = false
            return reply_json(fd, M.obj{ok = true})
        end
        if p == "/api/notify" then
            if os.time() - last_note < 10 then return reply_err(fd, 429, "shown a moment ago") end
            last_note = os.time()
            notify_token()
            return reply_json(fd, M.obj{ok = true})
        end
        local ok, code, msg = check_token(req)
        if not ok then return reply_err(fd, code, msg) end
        if p == "/api/auth" or p == "/api/install/auth" then return reply_json(fd, M.obj{ok = true}) end
        if p == "/api/install/probe" then return h_inst_probe(fd, q) end
        if p == "/api/install/data" then return h_inst_data(fd, q, req, pre_ptr, pre_len) end
        if p == "/api/install/put" then return h_inst_write(fd, q, req, pre_ptr, pre_len, true) end
        if p == "/api/install/write" then return h_inst_write(fd, q, req, pre_ptr, pre_len, false) end
        if p == "/api/install/delete" then return h_inst_delete(fd, q) end
        if p == "/api/install/free" then return h_inst_free(fd, q) end
        if p == "/api/install/begin" then return h_inst_note(fd, q, nil) end
        if p == "/api/install/end" then return h_inst_note(fd, q, true) end
        if p == "/api/check-restore" then return h_check(fd, q) end
        if p == "/api/restore" then return h_restore(fd, q, req, pre_ptr, pre_len) end
        if p == "/api/copy" then return h_copy(fd, q) end
        if p == "/api/delete" then return h_delete(fd, q) end
        if p == "/api/delete-title" then return h_delete_title(fd, q) end
        return reply_err(fd, 404, "not found")
    end
    return reply_err(fd, 405, "method not allowed")
end

-- ---------------------------------------------------------------- start
local function make_token()
    local mix = M.mixer(tsc())
    mix.add(gettime())
    mix.add(os.time())
    mix.add(buf)
    for i = 1, 64 do               -- timing jitter of native round trips
        local t0 = tsc()
        usleep(i % 4)
        mix.add(tsc() - t0)
    end
    return M.token(mix.next)
end

local function serve()
    local lfd = new_socket(SOCK_STREAM)
    if lfd < 0 then error("cannot create the server socket") end
    syscall.setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, one, 4)
    syscall.setsockopt(lfd, SOL_SOCKET, SO_NOSIGPIPE, one, 4)
    local bufs = {}
    for _, opt in ipairs({SO_RCVBUF, SO_SNDBUF}) do     -- one TCP stream is window-bound
        for _, mib in ipairs({4, 2, 1, 0}) do
            write32(strbuf, mib << 20)
            if mib == 0 or syscall.setsockopt(lfd, SOL_SOCKET, opt, strbuf, 4) == 0 then
                bufs[#bufs + 1] = mib
                break
            end
        end
    end
    set_timeout(lfd, SO_RCVTIMEO, 3)       -- accepted sockets inherit these
    set_timeout(lfd, SO_SNDTIMEO, 30)
    local sa = malloc(16)
    local port
    for try = PORT, PORT + 9 do
        sockaddr(sa, nil, try)
        if syscall.bind(lfd, sa, 16) == 0 then port = try break end
    end
    if not port or syscall.listen(lfd, 8) ~= 0 then
        syscall.close(lfd)
        error(string.format("no free port in %d-%d", PORT, PORT + 9))
    end
    PORT = port
    IP = get_current_ip() or "this PS5"
    TOKEN = make_token()
    say(string.format("url http://%s:%d/", IP, PORT))
    say("token " .. TOKEN)
    say(string.format("SWRR %s, scratch %s, socket buffers %d/%d MiB", tostring(TITLE_ID), tostring(SCRATCH),
        bufs[1], bufs[2]))
    notify_token()
    local fails = 0
    while running do
        local cfd = syscall.accept(lfd, 0, 0)
        if not fd_ok(cfd) then
            fails = fails + 1
            say("accept -> " .. tostring(cfd))
            if fails >= 10 then break end
            usleep(200000)
        else
            fails = 0
            syscall.setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, one, 4)
            set_timeout(cfd, SO_SNDTIMEO, 30)
            responded = false
            local ok, err = pcall(handle, cfd)
            if not ok then
                say("request failed: " .. tostring(err))
                if not responded then pcall(reply_err, cfd, 500, tostring(err):gsub("^%s+", "")) end
            end
            local cok, cerr = pcall(cleanup)
            if not cok then say("cleanup: " .. tostring(cerr)) end
            syscall.close(cfd)
        end
    end
    syscall.close(lfd)
end

local ok, err = pcall(serve)
pcall(inst_close)
pcall(cleanup)
if not ok then
    say("stopped: " .. tostring(err))
    send_notification("ghostldr manager stopped:\n" .. tostring(err):gsub("^%s+", ""))
else
    say("stopped")
    send_notification("ghostldr manager stopped")
end
if log_fd >= 0 then syscall.close(log_fd) end
