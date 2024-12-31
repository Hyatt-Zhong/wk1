-- 定义协议名称和描述
local my_proto = Proto("my_proto", "Custom UDP Parsing Protocol")

-- 创建字段以显示提取的字节
local f_src_ip = Field.new("ip.src")
local f_dst_port = Field.new("udp.dstport")

-- 定义 dissector 函数
function my_proto.dissector(buffer, pinfo, tree)
    -- 获取源IP和目标端口
    local src_ip = tostring(f_src_ip())
    local dst_port = tonumber(tostring(f_dst_port()))

    -- 过滤条件: 源IP为192.168.0.102且目标端口为9902
    if src_ip == "192.168.0.102" and dst_port == 9902 then
        -- 确保数据包长度足够长
        if buffer:len() >= 0x12 then
            -- 提取第0x11和0x12字节
            local byte_11 = buffer(0x10, 1):uint()
            local byte_12 = buffer(0x11, 1):uint()

            -- 在Wireshark的协议树上显示提取的字节
            local subtree = tree:add(my_proto, buffer(), "Custom UDP Parsing")
            subtree:add(buffer(0x10, 1), string.format("Byte 0x11: 0x%02X", byte_11))
            subtree:add(buffer(0x11, 1), string.format("Byte 0x12: 0x%02X", byte_12))

            -- 在Wireshark的列中显示提取的字节
            pinfo.cols.info:set(string.format("Src: %s, Byte 0x11: 0x%02X, Byte 0x12: 0x%02X", src_ip, byte_11, byte_12))
        end
    end
end

-- 将 dissector 绑定到UDP协议的 table 上
local udp_table = DissectorTable.get("udp.port")
udp_table:add(9902, my_proto)