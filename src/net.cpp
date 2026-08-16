#include "engine/net.hpp"

#include <cstring>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SockLen = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
#endif

namespace engine {

namespace {

// On POSIX the raw fd is reused; on Windows the SOCKET handle fits in an int
// for practical purposes (w64devkit sockets are small handles).
#if defined(_WIN32)
SOCKET toSocket(int fd) { return static_cast<SOCKET>(fd); }
int toFd(SOCKET s) { return static_cast<int>(s); }
#else
int toSocket(int fd) { return fd; }
int toFd(int s) { return s; }
#endif

std::string g_last_error = "ok";

} // namespace

bool netInit() {
#if defined(_WIN32)
    WSADATA data;
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        g_last_error = "WSAStartup failed";
        return false;
    }
#endif
    return true;
}

void netCleanup() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

int netListen(std::uint16_t port) {
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (toFd(s) < 0) {
        g_last_error = "socket() failed: ";
        g_last_error += netErrorString();
        return -1;
    }
    int one = 1;
#if defined(_WIN32)
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(toSocket(s), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        g_last_error = "bind() failed: ";
        g_last_error += netErrorString();
        netClose(toFd(toSocket(s)));
        return -1;
    }
    if (::listen(toSocket(s), 8) != 0) {
        g_last_error = "listen() failed: ";
        g_last_error += netErrorString();
        netClose(toFd(toSocket(s)));
        return -1;
    }
    return toFd(toSocket(s));
}

int netAccept(int listen_fd) {
    sockaddr_in addr{};
    SockLen len = sizeof addr;
    const int c = toFd(::accept(toSocket(listen_fd), reinterpret_cast<sockaddr*>(&addr), &len));
    if (c < 0) {
        g_last_error = "accept() failed: ";
        g_last_error += netErrorString();
        return -1;
    }
    return c;
}

int netConnect(const char* host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host, port_str.c_str(), &hints, &res) != 0) {
        g_last_error = "getaddrinfo() failed";
        return -1;
    }
    int fd = -1;
    for (addrinfo* it = res; it != nullptr; it = it->ai_next) {
        fd = toFd(::socket(it->ai_family, it->ai_socktype, it->ai_protocol));
        if (fd < 0) continue;
        if (::connect(toSocket(fd), it->ai_addr, static_cast<SockLen>(it->ai_addrlen)) == 0) {
            break;
        }
        netClose(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd == -1) {
        g_last_error = "connect() failed: ";
        g_last_error += netErrorString();
    }
    return fd;
}

int netWaitReadable(int fd, int timeout_ms) {
#if defined(_WIN32)
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(toSocket(fd), &readfds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int rc = ::select(0, &readfds, nullptr, nullptr, &tv);
#else
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int rc = ::select(fd + 1, &readfds, nullptr, nullptr, &tv);
#endif
    if (rc < 0) {
        g_last_error = "select() failed";
        return -1;
    }
    return rc == 0 ? 0 : 1;
}

void netSetNonBlocking(int fd) {
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(toSocket(fd), FIONBIO, &mode);
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

long netSend(int fd, const char* data, std::size_t len) {
#if defined(_WIN32)
    const int n = ::send(toSocket(fd), data, static_cast<int>(len), 0);
#else
    const long n = ::send(toSocket(fd), data, len, MSG_NOSIGNAL);
#endif
    if (n == SOCKET_ERROR) {
        g_last_error = "send() failed: ";
        g_last_error += netErrorString();
        return -1;
    }
    return static_cast<long>(n);
}

long netRecv(int fd, char* buf, std::size_t len) {
#if defined(_WIN32)
    const int n = ::recv(toSocket(fd), buf, static_cast<int>(len), 0);
#else
    const long n = ::recv(toSocket(fd), buf, len, 0);
#endif
    if (n == SOCKET_ERROR) {
        g_last_error = "recv() failed: ";
        g_last_error += netErrorString();
        return -1;
    }
    return static_cast<long>(n);
}

void netClose(int fd) {
#if defined(_WIN32)
    closesocket(toSocket(fd));
#else
    ::close(fd);
#endif
}

const char* netErrorString() {
    if (!g_last_error.empty() && g_last_error != "ok") return g_last_error.c_str();
#if defined(_WIN32)
    static char buf[128];
    std::snprintf(buf, sizeof buf, "Winsock error %d", WSAGetLastError());
    return buf;
#else
    static char buf[128];
    std::snprintf(buf, sizeof buf, "errno %d", errno);
    return buf;
#endif
}

} // namespace engine
