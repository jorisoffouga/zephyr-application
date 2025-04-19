#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_if.h>

#include "../inc/websocket.hpp"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

K_SEM_DEFINE(sem, 0, 1);	/* starts off "not available" */

static void iface_event_handler(struct net_mgmt_event_callback *cb,
    uint32_t mgmt_event,
    struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_IF_UP) {
        LOG_INF("Network interface is UP!");
        k_sem_give(&sem);
    } else if (mgmt_event == NET_EVENT_IF_DOWN) {
        LOG_INF("Network interface is DOWN!");
    }
}

int main (void)
{
    WebSocket ws("192.168.1.10", 8080, "/"); // Initialize WebSocket with appropriate parameters
    if (ws.init() != 1) {
        LOG_ERR("WebSocket initialization failed.");
        return -1;
    }
    k_sleep(K_SECONDS(5));

    LOG_INF("Finished init.");

    while (1) {

        std::string msg = ws.read_msg();
        if (!msg.empty()) {
            LOG_INF("Received message: %s", msg.c_str());
        }
        k_sleep(K_SECONDS(1));
    }
    return 0;
}

