#include <zephyr/net/socket.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/websocket.h>
#include <zephyr/logging/log.h>

#include "../inc/websocket.hpp"

LOG_MODULE_REGISTER(websocket, LOG_LEVEL_INF);

static uint8_t recv_buf[MAX_RECV_BUF_LEN];
static uint8_t temp_recv_buf[MAX_RECV_BUF_LEN + EXTRA_BUF_SPACE];

WebSocket::WebSocket(std::string const& address, int const port, std::string const& path)
    : m_socket(-1), m_websocket(-1), m_timeout(3 * MSEC_PER_SEC), m_path(path), m_port(port), m_address(address)
{
}

WebSocket::~WebSocket()
{
    if (m_websocket >= 0) {
        websocket_disconnect(m_websocket);
    }
    if (m_socket >= 0) {
        close(m_socket);
    }
}

int WebSocket::init(void)
{
    int ret = -1;
    struct websocket_request req;
    memset(&req, 0, sizeof(req));
    req.host = m_address.c_str();
    req.url = m_path.c_str();
    req.cb = connect_cb;
    req.tmp_buf = temp_recv_buf;
    req.tmp_buf_len = sizeof(temp_recv_buf);
    socklen_t addr_len = sizeof(m_addr);

    if (!IS_ENABLED(CONFIG_NET_IPV4)) {
        return 1;
    }

    if (m_address.empty() || m_path.empty()) {
        LOG_ERR("Invalid address or path");
        return -1;
    }
    if (m_port <= 0) {
        LOG_ERR("Invalid port");
        return -1;
    }

    if (m_websocket >= 0) {
        websocket_disconnect(m_websocket);
    }

    if (m_socket >= 0) {
        close(m_socket);
    }

    memset(&m_addr, 0, addr_len);

    net_sin(reinterpret_cast<struct sockaddr*>(&m_addr))->sin_family = AF_INET;
    net_sin(reinterpret_cast<struct sockaddr*>(&m_addr))->sin_port = htons(m_port);
    inet_pton(AF_INET, m_address.c_str(), &net_sin(reinterpret_cast<struct sockaddr*>(&m_addr))->sin_addr);

    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (m_socket < 0) {
        LOG_ERR("Failed to create IPv4 HTTP socket (%d)", -errno);
        return -1;
    }

    ret = connect(m_socket, reinterpret_cast<struct sockaddr*>(&m_addr), addr_len);
    if (ret < 0) {
        LOG_ERR("Cannot connect to IPv4 remote (%d)", -errno);
        ret = -errno;
    }
    LOG_INF("Connected to %s:%d", m_address.c_str(), m_port);

    m_websocket = websocket_connect(m_socket, &req, m_timeout, this);

    if (m_websocket < 0) {
        LOG_ERR("Cannot connect Websocket to %s:%d", m_address.c_str(), m_port);
        close(m_socket);
        return 0;
    }

    LOG_INF("Websocket %d", m_websocket);
    return 1;
}

int WebSocket::send_msg(std::string const& message)
{
    int ret = 0;
    if (!IS_ENABLED(CONFIG_NET_IPV4)) {
        return -1;
    }

    if (message.empty()) {
        LOG_WRN("Message empty!");
        return -1;
    }
    size_t len = message.length();
    if (!m_websocket) {
        LOG_ERR("Socket not configured!");
        return -1;
    }
    ret = websocket_send_msg(m_websocket, (uint8_t*)message.c_str(), len,
                             WEBSOCKET_OPCODE_DATA_TEXT,
                             true, true, 0);
    if ((size_t)ret != len) {
        LOG_WRN("Failed to send!");
        return -1;
    }
    return ret;
}

int WebSocket::send_msg(cJSON const*const message)
{
    int ret = 0;
    if (!IS_ENABLED(CONFIG_NET_IPV4)) {
        return -1;
    }

    if (!message) {
        LOG_WRN("Message empty!");
        return -1;
    }
    char * buf = cJSON_PrintUnformatted(message);
    if (buf) {
        if (!m_websocket) {
            LOG_ERR("Socket not configured!");
            free(buf);
            return -1;
        }
        ret = send_msg(std::string(buf));
        free(buf);
    }
    return ret;
}

void WebSocket::read_msg(cJSON* message)
{
    std::string received_data = read_msg();
    if (received_data.empty()) {
        LOG_ERR("Failed to receive data");
        return;
    }

    message = cJSON_Parse(received_data.c_str());

    if (!message) {
        LOG_ERR("Failed to parse JSON message");
        return;
    }
    if (cJSON_GetArraySize(message) == 0) {
        LOG_ERR("Empty JSON message");
        cJSON_Delete(message);
        return;
    }
}

std::string WebSocket::read_msg(void)
{
    uint64_t remaining = ULONG_MAX;
    int total_read;
    uint32_t message_type;
    int ret, read_pos;

    read_pos = 0;
    total_read = 0;
    size_t buf_len = sizeof(m_recv_buf);

    if (!IS_ENABLED(CONFIG_NET_IPV4)) {
        return std::string();
    }

    if (!m_websocket) {
        LOG_ERR("Socket not configured!");
        return std::string();
    }

    while (remaining > 0) {
        ret = websocket_recv_msg(m_websocket, m_recv_buf + read_pos,
            buf_len - read_pos,
            &message_type,
            &remaining,
            m_timeout);
        if (ret < 0) {
            if (ret == -EAGAIN) {
                k_sleep(K_MSEC(50));
                continue;
            }
            LOG_INF("IPv4 connection closed while waiting (%d/%d)", ret, errno);
            break;
        }

        read_pos += ret;
        total_read += ret;
    }

    if (remaining != 0) {
        LOG_ERR("IPv4 data recv failure %zd/%d bytes (remaining %" PRId64 ")",
                buf_len, total_read, remaining);
    } else {
        LOG_DBG("IPv4 recv %d bytes", total_read);
    }
    if (total_read > 0) {
        m_recv_buf[total_read] = '\0';
        return std::string((char*)m_recv_buf);
    }
    return std::string();
}

int WebSocket::connect_cb(int sock, struct http_request *req, void *user_data)
{
    if (!user_data) {
        return 0;
    }
    WebSocket *ws = reinterpret_cast<WebSocket *>(user_data);
    LOG_INF("Websocket %d for %s connected.", sock, ws->get_address().c_str());
    return 0;
}
