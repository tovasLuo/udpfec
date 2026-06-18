#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>

// 网络字节序相关宏定义
//#if defined(_MSC_VER)
//#ifndef WIN32_LEAN_AND_MEAN
//#define WIN32_LEAN_AND_MEAN
//#endif
//#include <WinSock2.h>
//#else
//#include <arpa/inet.h>
//#include <netinet/in.h>
//#endif

// IPv4伪首部结构体 UDP TCP SCTP(其checksum采用的是adler32算法)
struct pseudo_hdr_ipv4
{
    uint32_t saddr;        // 源IP地址 in_addr
    uint32_t daddr;        // 目的IP地址
    uint8_t  mbz;          // 必须为0
    uint8_t  protocol;     // 协议字段
    uint16_t tot_len;      // 首部+净荷总长
};

// IPv6伪首部结构体UDP TCP ICMPv6 SCTP(其checksum采用的是adler32算法)
struct pseudo_hdr_ipv6
{
    uint8_t  saddr[16];    // IPv6源地址 in6_addr
    uint8_t  daddr[16];    // IPv6目的地址
    uint32_t tot_len;      // 首部+净荷总长
    uint8_t  mbz[3];       // 必须为0
    uint8_t  next_hdr;     // 下一个头部
};

class packet_checksum
{
public:
    packet_checksum() = delete;
    ~packet_checksum() = delete;

    /**
     * @brief 计算IP头部校验和
     *
     * @param ip_header IP头部数据
     * @param length IP头部长度
     * @return uint16_t 校验和结果
     */
    static uint16_t compute_ip_checksum(const uint8_t* ip_header, size_t length);

    /**
     * @brief 计算传输层校验和
     *
     * @param header 传输层头部
     * @param header_len 头部长度
     * @param payload 传输层负载
     * @param payload_len 负载长度
     * @param source_addr IPv4源地址
     * @param dest_addr IPv4目标地址
     * @param protocol 协议类型
     * @param is_ipv6 是否是IPv6
     * @return uint16_t 校验和结果
     */
    static uint16_t compute_transport_checksum(const uint8_t* header, size_t header_len, const uint8_t* payload, size_t payload_len,
                                               const uint8_t* source_addr, const uint8_t* dest_addr, uint8_t protocol, bool is_ipv6 = false);

    /**
     * @brief 计算SCTP校验和
     *
     * @param header SCTP头部数据
     * @param header_len SCTP头部长度
     * @param chunk SCTP Chunk数据
     * @param chunk_length 数据长度
     * @param source_addr IPv4或IPv6源地址
     * @param dest_addr IPv4或IPv6目标地址
     * @param is_ipv6 是否是IPv6
     * @return uint32_t 校验和结果
     */
    static uint32_t compute_sctp_checksum(const uint8_t* header, size_t header_len, const uint8_t* chunk, size_t chunk_len,
                                          const uint8_t* source_addr, const uint8_t* dest_addr, bool is_ipv6 = false);

    /**
     * @brief 计算ICMPv4校验和
     *
     * @param header ICMP头部
     * @param header_len 头部长度
     * @param payload ICMP负载
     * @param payload_len 负载长度
     * @return uint16_t 校验和结果
     */
    static uint16_t compute_icmp_v4_checksum(const uint8_t* header, size_t header_len, const uint8_t* payload, size_t payload_len);

    /**
     * @brief 计算ICMPv6校验和
     *
     * @param header ICMPv6头部
     * @param header_len 头部长度
     * @param payload ICMPv6负载
     * @param payload_len 负载长度
     * @param source_addr IPv6源地址
     * @param dest_addr IPv6目标地址
     * @return uint16_t 校验和结果
     */
    static uint16_t compute_icmp_v6_checksum(const uint8_t* header, size_t header_len, const uint8_t* payload, size_t payload_len,
                                             const uint8_t* source_addr, const uint8_t* dest_addr);

private:
    /**
     * @brief 计算校验和的核心部分（16位数据）
     *
     * @param data 数据指针
     * @param count 数据长度（以16位为单位）
     * @param pre_sum 初始校验和值
     * @return uint32_t 中间状态的校验和
     */
    static uint32_t checksum_core(const uint16_t* data, size_t count, uint32_t pre_sum = 0);

    /**
     * @brief 完成校验和计算（包含可能的奇数字节）
     *
     * @param data 数据指针
     * @param length 数据长度（字节）
     * @param pre_sum 初始校验和值
     * @return uint16_t 最终的校验和
     */
    static uint16_t finalize_checksum(const uint8_t* data, size_t length, uint32_t pre_sum = 0);

    /**
     * @brief 计算IPv4伪首部校验和（用于TCP/UDP）
     *
     * @param source_addr 源地址
     * @param dest_addr 目标地址
     * @param protocol 协议类型
     * @param total_length 总长度
     * @return uint32_t 中间校验和结果
     */
    static uint32_t compute_pseudoheader_v4(const uint8_t* source_addr, const uint8_t* dest_addr, uint8_t protocol, uint16_t total_length);

    /**
     * @brief 计算IPv6伪首部校验和（用于TCP/UDP/ICMPv6）
     *
     * @param source_addr 源地址
     * @param dest_addr 目标地址
     * @param next_header 下一个头部类型
     * @param payload_length 负载长度
     * @return uint32_t 中间校验和结果
     */
    static uint32_t compute_pseudoheader_v6(const uint8_t* source_addr, const uint8_t* dest_addr, uint8_t next_header, uint32_t payload_length);

    /**
     * @brief 计算Adler-32校验和
     *
     * @param data 数据指针
     * @param len 数据长度
     * @param pre_adler 初始Adler-32值
     * @return uint32_t Adler-32校验和结果
     */
    static uint32_t adler32(const uint8_t* data, size_t len, uint32_t pre_adler = 1);

    /**
     * @brief 计算IPv4伪首部校验和（仅用于SCTP）
     *
     * @param source_addr 源地址
     * @param dest_addr 目标地址
     * @param total_length 总长度
     * @return uint32_t 中间校验和结果
     */
    static uint32_t compute_pseudoheader_4_sctp_v4(const uint8_t* source_addr, const uint8_t* dest_addr, uint16_t total_length);

    /**
     * @brief 计算IPv6伪首部校验和（仅用于SCTP）
     *
     * @param source_addr 源地址
     * @param dest_addr 目标地址
     * @param payload_length 负载长度
     * @return uint32_t 中间校验和结果
     */
    static uint32_t compute_pseudoheader_4_sctp_v6(const uint8_t* source_addr, const uint8_t* dest_addr, uint32_t payload_length);
};

inline uint16_t packet_checksum::compute_ip_checksum(const uint8_t* ip_header, size_t length)
{
    assert(ip_header != nullptr);
    return finalize_checksum(ip_header, length);
}

inline uint16_t packet_checksum::finalize_checksum(const uint8_t* data, size_t length, uint32_t pre_sum)
{
    uint32_t sum = pre_sum;
    if (data != nullptr && length != 0)
    {
        // 处理16位对齐的数据
        sum = checksum_core(reinterpret_cast<const uint16_t*>(data), length / sizeof(uint16_t), sum);

        // 处理剩余的单个字节
        if (length % sizeof(uint16_t) != 0)
        {
            uint8_t last_uint16[2] = {data[length - 1], 0};
            sum += *reinterpret_cast<const uint16_t*>(&last_uint16);
        }
    }

    // 将所有高位进位相加
    while (sum >> 16)
    {
        sum = (sum & UINT16_MAX) + (sum >> 16);
    }

    // 取反得到最终校验和
    sum = ~sum;
    sum &= UINT16_MAX; // 由于采用了uint32_t存储值，我们只取低16位

    // 根据RFC要求，如果校验和为0，则设置为全1
    return (sum == 0) ? UINT16_MAX : static_cast<uint16_t>(sum);
}

inline uint32_t packet_checksum::checksum_core(const uint16_t* data, size_t count, uint32_t pre_sum)
{
    if (data == nullptr || count == 0)
    {
        return pre_sum;
    }

    uint32_t sum = pre_sum;
    for (size_t i = 0; i < count; ++i)
    {
        sum += data[i];
        if (sum & 0x80000000)
        {
            sum = (sum & UINT16_MAX) + (sum >> 16);
        }
    }

    return sum;
}

inline uint16_t packet_checksum::compute_transport_checksum(const uint8_t* header, size_t header_len, const uint8_t* payload, size_t payload_len,
                                                            const uint8_t* source_addr, const uint8_t* dest_addr, uint8_t protocol, bool is_ipv6/* = false*/)
{
    assert(header != nullptr && source_addr != nullptr && dest_addr != nullptr);

    // 计算伪首部校验和
    uint32_t sum = (!is_ipv6) ? compute_pseudoheader_v4(source_addr, dest_addr, protocol, static_cast<uint16_t>(header_len + payload_len))
                              : compute_pseudoheader_v6(source_addr, dest_addr, protocol, static_cast<uint32_t>(header_len + payload_len));

    // 添加传输层头部
    sum = checksum_core(reinterpret_cast<const uint16_t*>(header), header_len / 2, sum);

    // 添加传输层负载
    return finalize_checksum(payload, payload_len, sum);
}

inline uint32_t packet_checksum::compute_pseudoheader_v4(const uint8_t* source_addr, const uint8_t* dest_addr, uint8_t protocol, uint16_t total_length)
{
    assert(source_addr != nullptr && dest_addr != nullptr);

    // 添加源地址
    uint32_t sum = checksum_core(reinterpret_cast<const uint16_t*>(source_addr), 4 / 2, 0);

    // 添加目标地址
    sum = checksum_core(reinterpret_cast<const uint16_t*>(dest_addr), 4 / 2, sum);

    // 添加协议字段
    uint8_t protocol_be[2] = {0, protocol};
    sum = checksum_core(reinterpret_cast<const uint16_t*>(&protocol_be), 2 / 2, sum);

    // 长度
    uint16_t total_length_be = htons(total_length);
    sum = checksum_core(&total_length_be, 2 / 2, sum);

    return sum;
}

inline uint32_t packet_checksum::compute_pseudoheader_v6(const uint8_t* source_addr, const uint8_t* dest_addr, uint8_t next_header, uint32_t payload_length)
{
    assert(source_addr != nullptr && dest_addr != nullptr);

    // 添加源地址
    uint32_t sum = checksum_core(reinterpret_cast<const uint16_t*>(source_addr), 16 / 2, 0);

    // 添加目标地址
    sum = checksum_core(reinterpret_cast<const uint16_t*>(dest_addr), 16 / 2, sum);

    // 添加负载长度
    uint32_t payload_length_be = htonl(static_cast<uint32_t>(payload_length));
    sum = checksum_core(reinterpret_cast<const uint16_t*>(&payload_length_be), 4 / 2, sum);

    // 添加下一头部
    uint8_t next_header_be[4] = {0, 0, 0, next_header};
    sum = checksum_core(reinterpret_cast<const uint16_t*>(&next_header_be), 4 / 2, sum);

    return sum;
}

inline uint32_t packet_checksum::compute_sctp_checksum(const uint8_t* header, size_t header_len, const uint8_t* chunk, size_t chunk_len,
                                                       const uint8_t* source_addr, const uint8_t* dest_addr, bool is_ipv6 /*= false*/) {
    assert(header != nullptr && chunk != nullptr && source_addr != nullptr && dest_addr != nullptr);

    // 计算伪首部并更新Adler-32
    uint32_t checksum = (!is_ipv6) ? compute_pseudoheader_4_sctp_v4(source_addr, dest_addr, static_cast<uint16_t>(header_len + chunk_len))
                                   : compute_pseudoheader_4_sctp_v6(source_addr, dest_addr, static_cast<uint32_t>(header_len + chunk_len));

    // 计算SCTP头部并更新Adler-32
    checksum = adler32(header, header_len, checksum);

    // 计算Chunk Data并更新Adler-32
    checksum = adler32(chunk, chunk_len, checksum);

    // 如果存在奇数个字节，在末尾添加一个零字节
    if ((header_len + chunk_len) % 2 != 0)
    {
        uint8_t zero_byte = 0;
        checksum = adler32(&zero_byte, 1, checksum);
    }

    return checksum;
}

inline uint32_t packet_checksum::compute_pseudoheader_4_sctp_v4(const uint8_t* source_addr, const uint8_t* dest_addr, uint16_t total_length)
{
    assert(source_addr != nullptr && dest_addr != nullptr);

    // 添加源地址
    uint32_t checksum = adler32(source_addr, 4, 1);

    // 添加目标地址
    checksum = adler32(dest_addr, 4, checksum);

    // 填充到16位边界
    uint8_t zero_byte = 0;
    checksum = adler32(&zero_byte, 1, checksum);

    // 协议字段（固定为IPPROTO_SCTP）
    uint8_t protocol_be = 132/*IPPROTO_SCTP*/;
    checksum = adler32(&protocol_be, 1, checksum);

    // 添加总长度
    uint16_t total_length_be = htons(total_length);
    checksum = adler32(reinterpret_cast<const uint8_t*>(&total_length_be), 2, checksum);

    return checksum;
}

inline uint32_t packet_checksum::compute_pseudoheader_4_sctp_v6(const uint8_t* source_addr, const uint8_t* dest_addr, uint32_t payload_length)
{
    assert(source_addr != nullptr && dest_addr != nullptr);

    // 添加源地址
    uint32_t checksum = adler32(source_addr, 16, 1);

    // 添加目标地址
    checksum = adler32(dest_addr, 16, checksum);

    // 添加负载长度
    uint32_t payload_length_be = htonl(payload_length);
    checksum = adler32(reinterpret_cast<const uint8_t*>(&payload_length_be), 4, checksum);

    // 填充到32位边界
    uint8_t zero_bytes[3] = {0};
    checksum = adler32(zero_bytes, 3, checksum);

    // 添加下一头部（固定为IPPROTO_SCTP）
    uint8_t next_header_be = 132/*IPPROTO_SCTP*/;
    checksum = adler32(&next_header_be, 1, checksum);

    return checksum;
}

inline uint32_t packet_checksum::adler32(const uint8_t* data, size_t len, uint32_t pre_adler/* = 1*/)
{
    if (data == nullptr || len == 0)
    {
        return pre_adler;
    }

    static const uint32_t MOD_ADLER = 65521;
    uint32_t a = pre_adler & UINT16_MAX;
    uint32_t b = (pre_adler >> 16) & UINT16_MAX;

    for (size_t index = 0; index < len; ++index)
    {
        a = (a + data[index]) % MOD_ADLER;
        b = (b + a) % MOD_ADLER;
    }

    return (b << 16) | a;
}

// IGMP/GRE也可参照ICMPv4的计算方式
inline uint16_t packet_checksum::compute_icmp_v4_checksum(const uint8_t* header, size_t header_len, const uint8_t* payload, size_t payload_len)
{
    assert(header != nullptr);

    // ICMPv4校验和需要包括ICMP头部和数据

    // 添加ICMPv4头部
    uint32_t sum = checksum_core(reinterpret_cast<const uint16_t*>(header), header_len / 2, 0);

    // 添加ICMPv4负载
    return finalize_checksum(payload, payload_len, sum);
}

inline uint16_t packet_checksum::compute_icmp_v6_checksum(const uint8_t* header, size_t header_len, const uint8_t* payload, size_t payload_len,
                                                          const uint8_t* source_addr, const uint8_t* dest_addr)
{
    return compute_transport_checksum(header, header_len, payload, payload_len, source_addr, dest_addr, 58/*IPPROTO_ICMPV6*/, true);
}
