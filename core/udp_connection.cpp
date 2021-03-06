#include "udp_connection.h"
#include "global_include.h"
#include "log.h"

#ifndef WINDOWS
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h> // for close()
#else
#include <winsock2.h>
#include <Ws2tcpip.h> // For InetPton
#undef SOCKET_ERROR // conflicts with ConnectionResult::SOCKET_ERROR
#pragma comment(lib, "Ws2_32.lib") // Without this, Ws2_32.lib is not included in static library.
#endif

#include <cassert>

#ifndef WINDOWS
#define GET_ERROR(_x) strerror(_x)
#else
#define GET_ERROR(_x) WSAGetLastError()
#endif

namespace dronecore {

UdpConnection::UdpConnection(DroneCoreImpl *parent,
                             int local_port_number) :
    Connection(parent),
    _local_port_number(local_port_number)
{
    if (_local_port_number == 0) {
        _local_port_number = DEFAULT_UDP_LOCAL_PORT;
    }
}

UdpConnection::~UdpConnection()
{
    // If no one explicitly called stop before, we should at least do it.
    stop();
}

bool UdpConnection::is_ok() const
{
    return true;
}

DroneCore::ConnectionResult UdpConnection::start()
{
    if (!start_mavlink_receiver()) {
        return DroneCore::ConnectionResult::CONNECTIONS_EXHAUSTED;
    }

    DroneCore::ConnectionResult ret = setup_port();
    if (ret != DroneCore::ConnectionResult::SUCCESS) {
        return ret;
    }

    start_recv_thread();

    return DroneCore::ConnectionResult::SUCCESS;
}

DroneCore::ConnectionResult UdpConnection::setup_port()
{

#ifdef WINDOWS
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LogErr() << "Error: Winsock failed, error: %d", WSAGetLastError();
        return DroneCore::ConnectionResult::SOCKET_ERROR;
    }
#endif

    _socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (_socket_fd < 0) {
        LogErr() << "socket error" << GET_ERROR(errno);
        return DroneCore::ConnectionResult::SOCKET_ERROR;
    }

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(_local_port_number);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(_socket_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        LogErr() << "bind error: " << GET_ERROR(errno);
        return DroneCore::ConnectionResult::BIND_ERROR;
    }

    return DroneCore::ConnectionResult::SUCCESS;
}

void UdpConnection::start_recv_thread()
{
    _recv_thread = new std::thread(receive, this);
}

DroneCore::ConnectionResult UdpConnection::stop()
{
    _should_exit = true;

#ifndef WINDOWS
    // This should interrupt a recv/recvfrom call.
    shutdown(_socket_fd, SHUT_RDWR);

    // But on Mac, closing is also needed to stop blocking recv/recvfrom.
    close(_socket_fd);
#else
    shutdown(_socket_fd, SD_BOTH);

    closesocket(_socket_fd);

    WSACleanup();
#endif

    if (_recv_thread) {
        _recv_thread->join();
        delete _recv_thread;
        _recv_thread = nullptr;
    }

    // We need to stop this after stopping the receive thread, otherwise
    // it can happen that we interfere with the parsing of a message.
    stop_mavlink_receiver();

    return DroneCore::ConnectionResult::SUCCESS;
}

bool UdpConnection::send_message(const mavlink_message_t &message)
{
    struct sockaddr_in dest_addr {};

    {
        std::lock_guard<std::mutex> lock(_remote_mutex);

        if (_remote_ip.empty()) {
            LogErr() << "Remote IP unknown";
            return false;
        }

        if (_remote_port_number == 0) {
            LogErr() << "Remote port unknown";
            return false;
        }

        dest_addr.sin_family = AF_INET;

        inet_pton(AF_INET, _remote_ip.c_str(), &dest_addr.sin_addr.s_addr);

        dest_addr.sin_port = htons(_remote_port_number);
    }

    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    uint16_t buffer_len = mavlink_msg_to_send_buffer(buffer, &message);

    // TODO: remove this assert again
    assert(buffer_len <= MAVLINK_MAX_PACKET_LEN);

    int send_len = sendto(_socket_fd, reinterpret_cast<char *>(buffer), buffer_len, 0,
                          reinterpret_cast<const sockaddr *>(&dest_addr), sizeof(dest_addr));

    if (send_len != buffer_len) {
        LogErr() << "sendto failure: " << GET_ERROR(errno);
        return false;
    }

    return true;
}

void UdpConnection::receive(UdpConnection *parent)
{
    // Enough for MTU 1500 bytes.
    char buffer[2048];

    while (!parent->_should_exit) {

        struct sockaddr_in src_addr = {};
        socklen_t src_addr_len = sizeof(src_addr);
        int recv_len = recvfrom(parent->_socket_fd, buffer, sizeof(buffer), 0,
                                reinterpret_cast<struct sockaddr *>(&src_addr), &src_addr_len);

        if (recv_len == 0) {
            // This can happen when shutdown is called on the socket,
            // therefore we check _should_exit again.
            continue;
        }

        if (recv_len < 0) {
            // This happens on desctruction when close(_socket_fd) is called,
            // therefore be quiet.
            //LogErr() << "recvfrom error: " << GET_ERROR(errno);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(parent->_remote_mutex);

            int new_remote_port_number = ntohs(src_addr.sin_port);
            std::string new_remote_ip(inet_ntoa(src_addr.sin_addr));

            if (parent->_remote_ip.empty() ||
                parent->_remote_port_number == 0) {

                // Set IP if we don't know it yet.
                parent->_remote_ip = new_remote_ip;
                parent->_remote_port_number = new_remote_port_number;

                LogInfo() << "New device on: " << parent->_remote_ip
                          << ":" << parent->_remote_port_number;

            } else if (parent->_remote_ip.compare(new_remote_ip) != 0 ||
                       parent->_remote_port_number != new_remote_port_number) {

                // It is possible that wifi disconnects and a device might get a new
                // IP and/or UDP port.
                parent->_remote_ip = new_remote_ip;
                parent->_remote_port_number = new_remote_port_number;

                LogInfo() << "Device changed to: " << new_remote_ip << ":" << new_remote_port_number;
            }
        }

        parent->_mavlink_receiver->set_new_datagram(buffer, recv_len);

        // Parse all mavlink messages in one datagram. Once exhausted, we'll exit while.
        while (parent->_mavlink_receiver->parse_message()) {
            parent->receive_message(parent->_mavlink_receiver->get_last_message());
        }
    }
}


} // namespace dronecore
