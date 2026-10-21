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
M.PROTECTED = {["SLUS-20268"] = true, ["SLES-50366"] = true}    -- SWRR's own save
M.SWRR = {CUSA03474 = "SLUS-20268", CUSA03492 = "SLES-50366"}
M.REGION = {CUSA03474 = "US", CUSA03492 = "EU"}
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
<button id="refresh">Refresh</button><button id="exit" class="danger">Exit</button></header>
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
-- refuses every mount (ghostfs.c).  SWRR 01.02, US and EU.
local mount_addr, search_addr = read64(EBOOT_BASE + 0x3893F0), read64(EBOOT_BASE + 0x3893C0)
if mount_addr < 0x10000 or search_addr < 0x10000 then error("manager: SWRR's save data imports not found") end
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
    write64(params + 0x10, dirbuf)
    write64(params + 0x20, blocks or 0)
    write32(params + 0x28, mode)
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
        if p == "/api/state" then
            local s, e = state()
            if not s then return reply_err(fd, 500, e) end
            return reply_json(fd, s)
        end
        if p == "/api/files" then return h_files(fd, q) end
        if p == "/api/download" then return h_download(fd, q) end
        return reply_err(fd, 404, "not found")
    elseif m == "POST" then
        if p == "/api/exit" then
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
        if p == "/api/auth" then return reply_json(fd, M.obj{ok = true}) end
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
pcall(cleanup)
if not ok then
    say("stopped: " .. tostring(err))
    send_notification("ghostldr manager stopped:\n" .. tostring(err):gsub("^%s+", ""))
else
    say("stopped")
    send_notification("ghostldr manager stopped")
end
if log_fd >= 0 then syscall.close(log_fd) end
