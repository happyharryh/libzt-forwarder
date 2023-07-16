/**
 * libzt C API example
 *
 * Simple socket-based client application
 */

#include "ZeroTierSockets.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstdio>
#include <thread>

class Connection {
public:
    Connection(int fd) : fd(fd), stopped(false)
        { }
    virtual ssize_t read(void* buf, size_t count) const
        { return stopped ? -1 : ::read(fd, buf, count); }
    virtual ssize_t write(const void* buf, size_t count) const
        { return stopped ? -1 : ::write(fd, buf, count); }
    virtual int shutdown_wr() const
        { return stopped ? -1 : shutdown(fd, SHUT_WR); }
    virtual int close() const
        { return ::close(fd); }
    void stop()
        { stopped = true; }
protected:
    int fd;
    bool stopped;
};

class ZTConnection : public Connection {
public:
    ZTConnection(int fd) : Connection(fd)
        { }
    virtual ssize_t read(void* buf, size_t count) const
        { return stopped ? -1 : zts_read(fd, buf, count); }
    virtual ssize_t write(const void* buf, size_t count) const
        { return stopped ? -1 : zts_write(fd, buf, count); }
    virtual int shutdown_wr() const
        { return stopped ? -1 : zts_shutdown_wr(fd); }
    virtual int close() const
        { return zts_close(fd); }
};

void transport(Connection *src, Connection *dst) {
    char buf[4096];
    int r_count;

    for (;;) {
        if ((r_count = src->read(buf, 4096)) <= 0) {
            src->stop();
            dst->shutdown_wr();
            return;
        }

        if (dst->write(buf, r_count) != r_count) {
            src->stop();
            return;
        }
    }
}

void handle(int local_fd, const char* remote_addr, int remote_port) {
    int remote_fd = zts_socket(AF_INET, SOCK_STREAM, 0);
    while (zts_connect(remote_fd, remote_addr, remote_port, 0) < 0) {
        printf("Re-attempting to connect...\n");
    }

    Connection local_conn(local_fd);
    ZTConnection remote_conn(remote_fd);

    std::thread local_to_remote(transport, &local_conn, &remote_conn);
    std::thread remote_to_local(transport, &remote_conn, &local_conn);

    local_to_remote.join();
    remote_to_local.join();

    local_conn.close();
    remote_conn.close();
}

void serve(const char* addr_pair) {
    char local_addr[16], remote_addr[16];
    int local_port, remote_port;

    sscanf(addr_pair, "%[0-9.]:%u-%[0-9.]:%u", local_addr, &local_port, remote_addr, &remote_port);

    // Forwarder
    sockaddr_in local_address;
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = inet_addr(local_addr);
    local_address.sin_port = htons(local_port);

    int local_socket;
    if((local_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        printf("socket\n");
        exit(1);
    }
    if (bind(local_socket, (sockaddr*)&local_address, sizeof(local_address)) == -1) {
        printf("bind\n");
        exit(1);
    }
    if (listen(local_socket, 40) == -1) {
        printf("listen\n");
        exit(1);
    }

    printf("Start Server: %s:%u -> %s:%u\n", local_addr, local_port, remote_addr, remote_port);

    for (;;) {
        int local_fd;
        if ((local_fd = accept(local_socket, NULL, NULL)) == -1) {
            printf("accept\n");
            exit(1);
        }

        std::thread(handle, local_fd, remote_addr, remote_port).detach();
    }
}

void login(const char* storage_path, long long int net_id) {
    int err = ZTS_ERR_OK;

    // Initialize node
    if ((err = zts_init_from_storage(storage_path)) != ZTS_ERR_OK) {
        printf("Unable to start service, error = %d. Exiting.\n", err);
        exit(1);
    }

    // Start node
    if ((err = zts_node_start()) != ZTS_ERR_OK) {
        printf("Unable to start service, error = %d. Exiting.\n", err);
        exit(1);
    }
    printf("Waiting for node to come online\n");
    while (! zts_node_is_online()) {
        zts_util_delay(50);
    }
    printf("Public identity (node ID) is %llx\n", (long long int)zts_node_get_id());

    // Join network
    printf("Joining network %llx\n", net_id);
    if (zts_net_join(net_id) != ZTS_ERR_OK) {
        printf("Unable to join network. Exiting.\n");
        exit(1);
    }
    printf("Don't forget to authorize this device in my.zerotier.com or the web API!\n");
    printf("Waiting for join to complete\n");
    while (! zts_net_transport_is_ready(net_id)) {
        zts_util_delay(50);
    }

    // Get assigned address (of the family type we care about)
    printf("Waiting for address assignment from network\n");
    while (! (err = zts_addr_is_assigned(net_id, ZTS_AF_INET))) {
        zts_util_delay(50);
    }
    char ipstr[ZTS_IP_MAX_STR_LEN] = { 0 };
    zts_addr_get_str(net_id, ZTS_AF_INET, ipstr, ZTS_IP_MAX_STR_LEN);
    printf("IP address on network %llx is %s\n", net_id, ipstr);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("zerotier forward\n");
        printf("zt-forward <id_storage_path> <net_id> <local_addr>:<local_port>-<remote_addr>:<remote_port>...\n");
        exit(0);
    }
    char* storage_path = argv[1];
    long long int net_id = strtoull(argv[2], NULL, 16);   // At least 64 bits

    login(storage_path, net_id);

    std::thread *p_thread;
    for (int i = 3; i < argc; ++i) {
        p_thread = new std::thread(serve, argv[i]);
    }
    p_thread->join();

    return 0;
}
