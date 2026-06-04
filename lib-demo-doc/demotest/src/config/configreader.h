#include "cos.h"

#include <fstream>
#include <string>
#include <vector>

using namespace std;

#ifdef __cplusplus
extern "C" {
#endif

class ConfigReader {
 public:
    ConfigReader();
    ~ConfigReader();

    void ReadConfigFile(const string &flName);

    const string GetCliUserName(void) const;

    const string GetCliListenIp(void) const;

    const u32 GetCliListenPort(void) const;

    const u32 GetCurLogLevel(void) const;

    const vector<string> GetUdpServerIp(void) const;

    const u32 GetUdpServerStartPort(void) const;

    const u32 GetUdpServerEndPort(void) const;

    const string& GetSendIp(void) const;

 private:
    string cli_user_name_;
    string cli_listen_ip_;
    u32    cli_listen_port_;

    u32    current_log_level_;

    vector<string> udp_server_ip_;
    u32    udp_server_start_port_;
    u32    udp_server_end_port_;

    string send_ip_;

    ifstream file_handle_;
};

#ifdef __cplusplus
}
#endif

