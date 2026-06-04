#include "configreader.h"
#include "macrodefine.h"
#include "cos.h"
#include "json.h"

#include <string>
#include <fstream>
#include <iostream>
using namespace std;
using namespace Json;

#ifdef __cplusplus
extern "C" {
#endif

ConfigReader::ConfigReader():
    cli_user_name_("root"),
    cli_listen_ip_("127.0.0.1"),
    cli_listen_port_(20004),
    udp_server_start_port_(30001),
    udp_server_end_port_(30010),
    current_log_level_(cos_log_level_warning),
    send_ip_("0.0.0.0") {
    vector<string> tmp_udp_server_ip_vec;
    swap(tmp_udp_server_ip_vec, udp_server_ip_);
    udp_server_ip_.reserve(2);
    udp_server_ip_.emplace_back("0.0.0.0");
    return;
}

ConfigReader::~ConfigReader() {
    if (file_handle_.is_open()) {
        file_handle_.close();
    }

    return;
}

void ConfigReader::ReadConfigFile(const string &flName) {
    file_handle_.open(flName);
    if (!file_handle_.is_open()) {
        COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_error,
                "open the %s file failed.\r\n", flName.c_str());
        return;
    }

    Value  json_root_object;
    CharReaderBuilder builder;
    JSONCPP_STRING errs;

    builder["collectComments"] = true;
    if (!parseFromStream(builder, file_handle_, &json_root_object, &errs)) {
        file_handle_.close();
        COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_error,
                "parse the %s json file failed.\r\n", flName.c_str());
        return;
    }

    if (json_root_object.isMember("cli")) {
        if (json_root_object["cli"].isMember("userName")) {
            cli_user_name_ = json_root_object["cli"]["userName"].asString();
        } else {
            COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                    "the cli's user name isn't exist in %s file, now using default name(root).\r\n",
                    flName.c_str());
        }

        if (json_root_object["cli"].isMember("listenIp")) {
            cli_listen_ip_ = json_root_object["cli"]["listenIp"].asString();
        } else {
            COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                    "the cli's listen ip isn't exist in %s file, now using default ip(127.0.0.1).\r\n",
                    flName.c_str());
        }

        if (json_root_object["cli"].isMember("listenPort")) {
            cli_listen_port_ = (u32)(json_root_object["cli"]["listenPort"].asInt());
        } else {
            COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                    "the cli's listen port isn't exist in %s file, now using default port(1234).\r\n",
                    flName.c_str());
        }
    } else {
        COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                "there isn't cli configure in %s file, now using default(root/127.0.0.1/1234).\r\n", flName.c_str());
    }

    if (json_root_object.isMember("testSpeed")) {
        if ((json_root_object["testSpeed"].isMember("listenIp"))
         && (0 != json_root_object["testSpeed"]["listenIp"].size())) {
            vector<string> tmp_vec;
            swap(tmp_vec, udp_server_ip_);

            udp_server_ip_.reserve(json_root_object["testSpeed"]["listenIp"].size() << 1);

            Value json_listen_ip_vec = json_root_object["testSpeed"]["listenIp"];

            for (u32 i = 0; json_listen_ip_vec.size() > i; ++i) {
                udp_server_ip_.emplace_back(json_listen_ip_vec[i]["IpAddress"].asString());
            }
        } else {
            vector<string> tmp_udp_server_ip_vec;
            swap(tmp_udp_server_ip_vec, udp_server_ip_);
            udp_server_ip_.reserve(2);
            udp_server_ip_.emplace_back("0.0.0.0");

            COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                    "the udpserver's ip isn't exist in %s file, now using default ip(0.0.0.0).\r\n",
                    flName.c_str());
        }

        if (json_root_object["testSpeed"].isMember("listenStartPort")) {
            udp_server_start_port_ = (u32)(json_root_object["testSpeed"]["listenStartPort"].asInt());
        } else {
            udp_server_start_port_ = 30001;
            COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                    "the udpserver's start port isn't exist in %s file, now using default port(30001).\r\n",
                    flName.c_str());
        }

        if (json_root_object["testSpeed"].isMember("listenEndPort")) {
            udp_server_end_port_ = (u32)(json_root_object["testSpeed"]["listenEndPort"].asInt());
        } else {
            udp_server_end_port_ = 30010;
            COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                    "the udpserver's end port isn't exist in %s file, now using default port(30010).\r\n",
                    flName.c_str());
        }
    } else {
        vector<string> tmp_udp_server_ip_vec;
        swap(tmp_udp_server_ip_vec, udp_server_ip_);
        udp_server_ip_.reserve(2);
        udp_server_ip_.emplace_back("0.0.0.0");

        udp_server_start_port_ = 30001;
        udp_server_end_port_   = 30010;
        COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                "there isn't udp server configure in %s file, now using default(0.0.0.0/30001~30010).\r\n",
                flName.c_str());
    }

    if (json_root_object.isMember("logLevel")) {
        current_log_level_ = (u32)(json_root_object["logLevel"].asInt());
    } else {
        current_log_level_ = 4;
        COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                "there isn't log level configure in %s file, now using default warning level(4).\r\n", flName.c_str());
    }

    if (json_root_object.isMember("sendIpAddress")) {
        send_ip_ = json_root_object["sendIpAddress"].asString();
    } else {
        send_ip_ = "0.0.0.0";
        COS_LOG(moduleIdEnum::kModuleOamId, cos_log_level_warning,
                "there isn't send ip configure in %s file, now using default(0.0.0.0).\r\n",
                flName.c_str());
    }

    file_handle_.close();

    return;
}

const string ConfigReader::GetCliUserName(void) const {
    return cli_user_name_;
}

const string ConfigReader::GetCliListenIp(void) const {
    return cli_listen_ip_;
}

const u32 ConfigReader::GetCliListenPort(void) const {
    return cli_listen_port_;
}

const u32 ConfigReader::GetCurLogLevel(void) const {
    return current_log_level_;
}

const vector<string> ConfigReader::GetUdpServerIp(void) const {
    return udp_server_ip_;
}

const u32 ConfigReader::GetUdpServerStartPort(void) const {
    return udp_server_start_port_;
}

const u32 ConfigReader::GetUdpServerEndPort(void) const {
    return udp_server_end_port_;
}

const string& ConfigReader::GetSendIp(void) const {
    return send_ip_;
}


#ifdef __cplusplus
}
#endif

