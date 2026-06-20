package.path = package.path .. ";.\\LuaSocket\\?.lua"
package.cpath = package.cpath .. ";.\\LuaSocket\\?.dll"

local lfs = require("lfs")
local socket = nil
local connect = nil
local lastSendTime = 0

-- ============ 调试日志 ============
local logFile = nil
local function dbgLog(msg)
    if logFile then
        logFile:write(os.date("%H:%M:%S") .. " " .. msg .. "\n")
        logFile:flush()
    end
end

-- ============ 类型常量 ============
local CAT_AIR = 1
local CAT_GROUND = 2
local CAT_NAVAL = 3
local SUB_PLANE = 1
local SUB_HELO = 2

-- 重编码 ID
local ID_PLANE = 1
local ID_HELICOPTER = 2
local ID_WARSHIP = 3
local ID_CARRIER = 4
local ID_GROUND = 5

-- 已知航母关键词（用于区分航母 vs 普通战斗舰艇）
local CARRIER_KEYWORDS = {
    ["nimitz"] = true, ["kuznetsov"] = true, ["cvn"] = true,
    ["forrestal"] = true, ["kitty hawk"] = true, ["enterprise"] = true,
    ["vinson"] = true, ["roosevelt"] = true, ["reagan"] = true,
    ["bush"] = true, ["stennis"] = true, ["truman"] = true,
    ["wasp"] = true, ["tarawa"] = true, ["america"] = true,
    ["carrier"] = true, ["lha"] = true, ["lhd"] = true,
}

local HEADER = 0x1ab2f407
local METERS_TO_FEET = 3.28084
local RAD_TO_DEG = 180 / math.pi

-- ============ 二进制打包函数 ============

function packI4(value)
    local v = value % 0x100000000
    if v < 0 then v = v + 0x100000000 end
    local b1 = v % 256
    v = (v - b1) / 256
    local b2 = v % 256
    v = (v - b2) / 256
    local b3 = v % 256
    v = (v - b3) / 256
    local b4 = v
    return string.char(b1, b2, b3, b4)
end

function packS8(str)
    local s = str or ""
    if #s > 8 then
        s = string.sub(s, 1, 8)
    end
    return s .. string.rep("\0", 8 - #s)
end

function packF8(value)
    local v = value or 0.0
    local sign = 0
    if v < 0 then
        sign = 0x80
        v = -v
    end
    if v == 0 then
        return string.char(0, 0, 0, 0, 0, 0, 0, 0)
    end
    -- 使用 math.frexp 分解
    local mantissa, exponent = math.frexp(v)
    mantissa = mantissa * 2 - 1
    exponent = exponent - 1
    if exponent < -1022 then
        -- 次正规数，简化处理
        exponent = -1023
        mantissa = v * 2^1022
    end
    exponent = exponent + 1023
    local mHigh = math.floor(mantissa * 2^20)
    local mLow = math.floor((mantissa * 2^20 - mHigh) * 2^32)
    local b1 = (sign + math.floor(exponent / 16)) % 256
    local b2 = ((exponent % 16) * 16 + math.floor(mHigh / 65536)) % 256
    local b3 = math.floor(mHigh / 256) % 256
    local b4 = mHigh % 256
    local b5 = math.floor(mLow / 16777216) % 256
    local b6 = math.floor(mLow / 65536) % 256
    local b7 = math.floor(mLow / 256) % 256
    local b8 = mLow % 256
    return string.char(b1, b2, b3, b4, b5, b6, b7, b8)
end

-- ============ 辅助函数 ============

function isCarrier(name)
    if not name then return false end
    local lower = string.lower(name)
    for keyword, _ in pairs(CARRIER_KEYWORDS) do
        if string.find(lower, keyword, 1, true) then
            return true
        end
    end
    return false
end

function getObjectTypeId(objType, objName)
    if not objType then
        return nil
    end
    -- DCS Type 是命名键表 {level1=1, level2=1, level3=6, level4=54}，不是数组
    if objType.level1 == CAT_AIR then
        if objType.level2 == SUB_PLANE then
            return ID_PLANE
        elseif objType.level2 == SUB_HELO then
            return ID_HELICOPTER
        end
    elseif objType.level1 == CAT_NAVAL then
        if isCarrier(objName) then
            return ID_CARRIER
        else
            return ID_WARSHIP
        end
    elseif objType.level1 == CAT_GROUND then
        return ID_GROUND
    end
    return nil
end

function addObjectToList(dataList, obj, tag)
    if not obj then
        dbgLog("  [" .. (tag or "?") .. "] SKIP: obj is nil")
        return false
    end
    if not obj.Name then
        dbgLog("  [" .. (tag or "?") .. "] SKIP: obj.Name is nil, obj keys=" .. table.concat(getKeys(obj), ","))
        return false
    end
    if not obj.Type then
        dbgLog("  [" .. (tag or "?") .. "] SKIP: obj.Type is nil, name=" .. tostring(obj.Name))
        return false
    end
    local typeId = getObjectTypeId(obj.Type, obj.Name)
    if not typeId then
        -- 打印 Type 表的所有键值对，搞清楚结构
        local typeKeys = {}
        local typeVals = {}
        for k, v in pairs(obj.Type) do
            table.insert(typeKeys, tostring(k))
            table.insert(typeVals, tostring(k) .. "=" .. tostring(v))
        end
        dbgLog("  [" .. (tag or "?") .. "] SKIP: typeId=nil, name=" .. tostring(obj.Name) .. " Type={" .. table.concat(typeVals, ",") .. "}")
        return false
    end
    local altFeet = math.floor((obj.LatLongAlt.Alt or 0) * METERS_TO_FEET)
    local headingDeg = math.floor(((obj.Heading or 0) * RAD_TO_DEG) % 360)
    local lat = obj.LatLongAlt.Lat or 0
    local lon = obj.LatLongAlt.Long or 0
    table.insert(dataList, {
        id = typeId,
        name = obj.Name or "",
        alt = altFeet,
        heading = headingDeg,
        lat = lat,
        lon = lon,
    })
    return true
end

-- 获取 table 的 key 列表（用于调试）
function getKeys(t)
    local keys = {}
    if t then
        for k, _ in pairs(t) do
            table.insert(keys, tostring(k))
        end
    end
    return keys
end

-- ============ DCS 导出接口 ============

function LuaExportStart()
    pcall(function()
        logFile = io.open(lfs.writedir() .. "Logs\\lnm_export.log", "w")
        dbgLog("LuaExportStart called")
        socket = require("socket")
        if socket then
            connect = socket.udp()
            connect:settimeout(0)
            connect:setpeername("127.0.0.1", 54382)
            dbgLog("UDP socket created, connected to 127.0.0.1:54382")
        else
            dbgLog("ERROR: socket module not loaded")
        end
    end)
end

local SEND_INTERVAL = 0.05
local logStatsInterval = 0

function LuaExportAfterNextFrame()
    if not connect then
        return
    end

    local now = LoGetModelTime()
    if now - lastSendTime < SEND_INTERVAL then
        return
    end
    lastSendTime = now

    pcall(function()
        local dataList = {}

        -- 玩家自身数据
        local selfData = LoGetSelfData()
        if selfData then
            addObjectToList(dataList, selfData, "self")
        end

        -- 世界物体（Export-Func.lua 官方用法）
        local wobsOk, wobs = pcall(LoGetWorldObjects)
        if wobsOk and wobs then
            for _, obj in pairs(wobs) do
                addObjectToList(dataList, obj, "wob")
            end
        end

        -- 每 2 秒统计
        logStatsInterval = logStatsInterval + 1
        if logStatsInterval >= 40 then
            logStatsInterval = 0
            local selfOk = selfData and "yes" or "no"
            local wobOk = wobsOk and "yes" or "no"
            local wobCount = (wobsOk and wobs) and #wobs or 0
            dbgLog("STATS: selfData=" .. selfOk .. " wobsOk=" .. wobOk .. " wobCount=" .. wobCount .. " dataList=" .. #dataList)
        end

        -- 构建二进制报文（第1条记录为玩家自身）
        -- 格式: HEADER(4B) + count(4B) + [每条记录]:
        --   id:4B  name:8B  alt:4B  heading:4B  lat:8B  lon:8B
        --   类型    名称     高度英尺  航向角度     纬度弧度  经度弧度
        local msg = packI4(HEADER) .. packI4(#dataList)
        for _, d in ipairs(dataList) do
            msg = msg .. packI4(d.id)
            msg = msg .. packS8(d.name)
            msg = msg .. packI4(d.alt)
            msg = msg .. packI4(d.heading)
            msg = msg .. packF8(d.lat)
            msg = msg .. packF8(d.lon)
        end

        connect:send(msg)
    end)
end

function LuaExportStop()
    dbgLog("LuaExportStop called")
    if logFile then
        logFile:close()
        logFile = nil
    end
    if connect then
        connect:close()
    end
end