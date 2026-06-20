package.path = package.path .. ";.\\LuaSocket\\?.lua"
package.cpath = package.cpath .. ";.\\LuaSocket\\?.dll"

local socket = nil
local connect = nil

-- ============ 类型常量 ============
local CAT_AIR = 1
local CAT_NAVAL = 3
local SUB_PLANE = 1
local SUB_HELO = 2

-- 重编码 ID
local ID_PLANE = 1
local ID_HELICOPTER = 2
local ID_WARSHIP = 3
local ID_CARRIER = 4

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

-- 打包 4 字节小端整数
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

-- 打包 8 字节 ASCII 字符串（超长截断，不足 \0 填充）
function packS8(str)
    local s = str or ""
    if #s > 8 then
        s = string.sub(s, 1, 8)
    end
    return s .. string.rep("\0", 8 - #s)
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

-- 根据 DCS Type 表获取重编码 ID
function getObjectTypeId(objType, objName)
    if objType[1] == CAT_AIR then
        if objType[2] == SUB_PLANE then
            return ID_PLANE
        elseif objType[2] == SUB_HELO then
            return ID_HELICOPTER
        end
    elseif objType[1] == CAT_NAVAL then
        if isCarrier(objName) then
            return ID_CARRIER
        else
            return ID_WARSHIP
        end
    end
    return nil
end

-- ============ DCS 导出接口 ============

function LuaExportStart()
    pcall(function()
        socket = require("socket")
        if socket then
            connect = socket.udp()
            connect:settimeout(0)
            connect:setpeername("127.0.0.1", 54382)
        end
    end)
end

function LuaExportAfterNextFrame()
    if not connect then
        return
    end

    pcall(function()
        local objects = LoGetWorldObjects("units")
        if not objects then return end

        -- 收集目标物体数据
        local dataList = {}
        for _, obj in pairs(objects) do
            local typeId = getObjectTypeId(obj.Type, obj.Name)
            if typeId then
                local altFeet = math.floor((obj.LatLongAlt.Alt or 0) * METERS_TO_FEET)
                local headingDeg = math.floor(((obj.Heading or 0) * RAD_TO_DEG) % 360)
                table.insert(dataList, {
                    id = typeId,
                    name = obj.Name or "",
                    alt = altFeet,
                    heading = headingDeg,
                })
            end
        end

        -- 构建二进制报文：头(4) + 数量(4) + n * [iID(4) + s型号(8) + i高度(4) + i航向(4)]
        local msg = packI4(HEADER) .. packI4(#dataList)
        for _, d in ipairs(dataList) do
            msg = msg .. packI4(d.id)
            msg = msg .. packS8(d.name)
            msg = msg .. packI4(d.alt)
            msg = msg .. packI4(d.heading)
        end

        connect:send(msg)
    end)
end

function LuaExportStop()
    if connect then
        connect:close()
    end
end