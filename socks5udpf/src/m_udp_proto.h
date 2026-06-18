#pragma once
#include <cstdint>

/*********************************************  YB UDP proxy proprietary header format ****************************************************/
//  UDP上行包（发送）
//+-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
//| byte0  |  byte1 | byte2  |  byte3 | byte4  |  byte5 | bit0    ……    bit14 | bit15 |  bit0    |  bit1   |    bit2     |   bit3   |     bit4     |    bit5     | …    bit7  |    byte9     | byte10 | byte11 | byte12 | byte13 | byte14 | byte15 | byte16 | byte17 | byte18  | byte19   |  byte20  ……  byten  | byten+1  ……  byten+m |
//| header          |    checksum     |     pkg_len     | pkg_start_time(MS)    |  zero | pkg_type | ip_type | double_line | line_idx | first_pkg_sn | encode_flag | reserve_bit | reserve_byte |     pkg_sn      | double_line_idx |         useruser  password        | game server's port |    game server's ip   |     pack data          |
//+-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+

//  UDP下行包（接收）
//+-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
//| byte0  |  byte1 | byte2  |  byte3 | byte4  |  byte5 | bit0    ……    bit14 | bit15 |  bit0    |  bit1   |    bit2     |   bit3   |     bit4     |    bit5     | …    bit7  |    byte9     | byte10 | byte11 | byte12 | byte13 | byte14  | byte15   |  byte16  ……  byten  | byten+1  ……  byten+m |
//| header          |    checksum     |     pkg_len     | pkg_start_time(MS)    |  zero | pkg_type | ip_type | double_line | line_idx | first_pkg_sn | encode_flag | reserve_bit | reserve_byte |     pkg_sn      | double_line_idx | game server's port |    game server's ip   |     pack data          |
//+-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+

//协议标识

#define M_UDP_FLAG_HEADER                           (0xA55A)
#define M_UDP_FLAG_ECHO_HEADER                      (0x9559)
#define M_UDP_FLAG_PKG_TYPE_CONTROL                  0   //命令包
#define M_UDP_FLAG_PKG_TYPE_BUSINESS                 1   //业务包
#define M_UDP_FLAG_IP_TYPE_IPV4                      0   //IPV4
#define M_UDP_FLAG_IP_TYPE_IPV6                      1   //IPV6
#define M_UDP_FLAG_IP_DOUBLE_DISABLE                 0   //单链
#define M_UDP_FLAG_IP_DOUBLE_ENABLE                  1   //双链

#define M_UDP_FLAG_ENCRYPTE_DISABLE                  0   //不加密
#define M_UDP_FLAG_ENCRYPTE_ENABLE                   1   //加密

#define M_UDP_FLAG_RETURN_USER_WAN_ADDR_DISABLE      0   //不返回
#define M_UDP_FLAG_RETURN_USER_WAN_ADDR_ENABLE       1   //返回

enum yb_cmd
{
    k_udp_cmd_port_check    = 0x0100,      //端口探测命令
    k_udp_cmd_user_wan_addr = 0x0101       //用户广域网地址
};

#pragma pack(push, 1)

//公共头部信息
struct m_udp_base_header
{
    uint16_t header;                    //0xA55A (可删减)
    uint16_t checksum;                  //校验和 (可删减)
    uint16_t pkg_len;                   //总体长度，包括header和checkSum (可删减)
    uint16_t pkg_start_time/*  : 15*/;  //发包时间(毫秒) (可删减)
    //uint16_t zero            : 1;     //固定为0
    uint8_t  pkg_type        : 1;       //类型标志，                0=命令包，   1=业务包
    uint8_t  ip_type         : 1;       //地址类型，                0=IPv4，     1=IPv6
    uint8_t  double_line     : 1;       //是否是双链，              0=单链，     1=双链
    uint8_t  line_idx        : 1;       //线路索引，                0=第1路，    1=第2路
    uint8_t  first_pkg_sn    : 1;       //第1个包序号，             0=非第1个包，1=第1个包
    uint8_t  encode_flag     : 1;       //加密标志，从byte8~包结尾，0=未加密，   1=加密
    uint8_t  rt_user_wan_addr: 1;       //返回客户端的外网地址
    uint8_t  reserve_bit     : 1;       //保留
    uint8_t  reserve_byte;              //保留
    uint16_t pkg_sn;                    //包序号
    uint16_t double_line_idx;           //双链序号
};

struct m_udp_up_base_header : m_udp_base_header
{
    uint32_t user_password;             //鉴权密码
};

//命令包头
struct udp_cmd_up_header : m_udp_up_base_header
{
    uint16_t command;                   //命令号
};

struct udp_cmd_down_header : m_udp_base_header
{
    uint16_t command;                   //命令号
};

using udp_cmd_port_check = udp_cmd_up_header;

using udp_cmd_up_user_wan_addr = udp_cmd_up_header;

struct user_wan_addr_info
{
    uint16_t family;
    uint16_t port;
    union
    {
        in_addr  addr4;
        in6_addr addr6;
    };
};

#if 0
struct udp_cmd_down_user_wan_addr : udp_cmd_down_header
{
    user_wan_addr_info user_wan_addr;
};
#endif

//上行接口，ipv4 applicable
struct udp_up4_header : m_udp_up_base_header
{
    uint16_t game_svr_port;             //服务器端口
    uint32_t game_svr_ip;               //服务器IP
};

//下行接口，ipv4 applicable
struct udp_down4_header : m_udp_base_header
{
    uint16_t game_svr_port;             //服务器端口
    uint32_t game_svr_ip;               //服务器IP
};

//上行接口，ipv6 applicable
struct udp_up6_header : m_udp_up_base_header
{
    uint16_t game_svr_port;             //服务器端口
    uint32_t game_svr_ip[4];            //服务器IP
};

//下行接口，ipv6 applicable
struct udp_down6_header : m_udp_base_header
{
    uint16_t game_svr_port;             //服务器端口
    uint32_t game_svr_ip[4];            //服务器IP
};

#pragma pack(pop)
