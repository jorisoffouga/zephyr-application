#pragma once

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_ip.h>
#include <string>
#include <stdint.h>
#include <cJSON/cJSON.h>

#define MAX_RECV_BUF_LEN    (2048)
/* We need to allocate bigger buffer for the websocket data we receive so that
 * the websocket header fits into it.
 */
#define EXTRA_BUF_SPACE     (30)

class WebSocket {
public:
    WebSocket(std::string const& address, int const port, std::string const& path);
    ~WebSocket();
    int init(void);
    int send_msg(cJSON const*const message);
    int send_msg(std::string const& message);
    void read_msg(cJSON* message);
    std::string read_msg(void);
    std::string get_address(void) const { return m_address; }
    std::string get_path(void) const { return m_path; }
    int get_port(void) const { return m_port; }

private:
    int m_socket;
    int m_websocket;
    int32_t m_timeout;
    std::string m_path;
    int m_port;
    std::string m_address;
    uint8_t m_recv_buf[MAX_RECV_BUF_LEN];
    uint8_t m_temp_recv_buf[MAX_RECV_BUF_LEN + EXTRA_BUF_SPACE];
    struct sockaddr_in m_addr;

    static int connect_cb(int sock, struct http_request *req, void *user_data);
};