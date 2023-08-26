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
    Connection(int fd) : fd(fd), stopped(false) { }
    virtual ~Connection() { }

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
    using Connection::Connection;

    virtual ssize_t read(void* buf, size_t count) const
        { return stopped ? -1 : zts_read(fd, buf, count); }
    virtual ssize_t write(const void* buf, size_t count) const
        { return stopped ? -1 : zts_write(fd, buf, count); }
    virtual int shutdown_wr() const
        { return stopped ? -1 : zts_shutdown_wr(fd); }
    virtual int close() const
        { return zts_close(fd); }
};

class Listener {
public:
    Listener(int local_socket) : local_socket(local_socket) { }
    virtual ~Listener() { }

    virtual Connection *accept() const {
        int local_fd;
        if ((local_fd = ::accept(local_socket, NULL, NULL)) < 0) {
            printf("accept\n");
            exit(1);
        }
        return new Connection(local_fd);
    }

    static Listener *listen(const char* local_addr, unsigned short local_port) {
        sockaddr_in local_address;
        local_address.sin_family = AF_INET;
        local_address.sin_addr.s_addr = inet_addr(local_addr);
        local_address.sin_port = htons(local_port);

        int local_socket;
        if((local_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            printf("socket\n");
            exit(1);
        }
        if (bind(local_socket, (sockaddr*)&local_address, sizeof(local_address)) < 0) {
            printf("bind\n");
            exit(1);
        }
        if (::listen(local_socket, 40) < 0) {
            printf("listen\n");
            exit(1);
        }
        return new Listener(local_socket);
    }

protected:
    int local_socket;
};

class ZTListener : public Listener {
public:
    using Listener::Listener;

    virtual Connection *accept() const {
        char remote_addr[ZTS_IP_MAX_STR_LEN];
        unsigned short port;

        int local_fd;
        if ((local_fd = zts_accept(local_socket, remote_addr, ZTS_IP_MAX_STR_LEN, &port)) < 0) {
            printf("accept\n");
            exit(1);
        }
        return new ZTConnection(local_fd);
    }

    static ZTListener *listen(const char* local_addr, unsigned short local_port) {
        int local_socket;
        if((local_socket = zts_socket(ZTS_AF_INET, ZTS_SOCK_STREAM, 0)) < 0) {
            printf("socket\n");
            exit(1);
        }
        if (zts_bind(local_socket, local_addr, local_port) < 0) {
            printf("bind\n");
            exit(1);
        }
        if (zts_listen(local_socket, 100) < 0) {
            printf("listen\n");
            exit(1);
        }
        return new ZTListener(local_socket);
    }
};

class Dialer {
public:
    Dialer(const char* remote_addr, unsigned short remote_port) :
        remote_addr(remote_addr), remote_port(remote_port) { }
    virtual ~Dialer() { }

    virtual Connection *dial() const {
        sockaddr_in remote_address;
        remote_address.sin_family = AF_INET;
        remote_address.sin_addr.s_addr = inet_addr(remote_addr);
        remote_address.sin_port = htons(remote_port);

        int remote_fd;
        if ((remote_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            printf("socket\n");
            exit(1);
        }
        if (connect(remote_fd, (sockaddr*)&remote_address, sizeof(remote_address)) < 0) {
            printf("connect\n");
            exit(1);
        }
        return new Connection(remote_fd);
    }

protected:
    const char* remote_addr;
    unsigned short remote_port;
};

class ZTDialer : public Dialer {
    using Dialer::Dialer;

    virtual Connection *dial() const {
        int remote_fd;
        if ((remote_fd = zts_socket(ZTS_AF_INET, ZTS_SOCK_STREAM, 0)) < 0) {
            printf("socket\n");
            exit(1);
        }
        while (zts_connect(remote_fd, remote_addr, remote_port, 0) < 0) {
            printf("Re-attempting to connect...\n");
        }
        return new ZTConnection(remote_fd);
    }
};

void transport(Connection *src, Connection *dst) {
    char buf[4096];
    int r_count;

    while (true) {
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

void handle(Connection *local_conn, Dialer *dialer) {
    Connection *remote_conn = dialer->dial();

    std::thread remote2local(transport, remote_conn, local_conn);
    transport(local_conn, remote_conn);  // local2remote
    remote2local.join();

    local_conn->close();
    remote_conn->close();

    delete local_conn;
    delete remote_conn;
}

void serve(const char* addr_pair) {
    char local_addr[16], remote_addr[16];
    unsigned short local_port, remote_port;

    sscanf(addr_pair, "%[v0-9.]:%hu-%[v0-9.]:%hu", local_addr, &local_port, remote_addr, &remote_port);

    Listener *listener;
    if (strncmp(local_addr, "v", 1) == 0) {
        listener = ZTListener::listen(local_addr + 1, local_port);
    } else {
        listener = Listener::listen(local_addr, local_port);
    }

    Dialer *dialer;
    if (strncmp(remote_addr, "v", 1) == 0) {
        dialer = new ZTDialer(remote_addr + 1, remote_port);
    } else {
        dialer = new Dialer(remote_addr, remote_port);
    }

    printf("Start Server: %s:%hu -> %s:%hu\n", local_addr, local_port, remote_addr, remote_port);

    while (true) {
        std::thread(handle, listener->accept(), dialer).detach();
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

    for (int i = 3; i < argc; ++i) {
        std::thread(serve, argv[i]).detach();
    }

    while (true) {
        zts_util_delay(500);   // Idle indefinitely
    }

    return 0;
}
