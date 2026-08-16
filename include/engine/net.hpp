#pragma once

#include <cstddef>
#include <cstdint>

namespace engine {

// ---------------------------------------------------------------------------
// Minimal cross-platform TCP socket wrapper (Phase 6). Winsock on Windows,
// POSIX sockets elsewhere. Enough for the feed server/client: listen,
// accept, non-blocking send, blocking receive, close. Errors are reported
// via netErrorString(); functions return -1 on failure (never throw).
//
// The feed's client policy is deliberately simple: non-blocking sends and
// "drop slow clients" -- a demo feed would rather disconnect a lagging
// client than stall the publisher.
// ---------------------------------------------------------------------------

// Call once at startup (no-op on POSIX; WSAStartup on Windows). Returns
// false on failure.
bool netInit();
// Call at exit (no-op on POSIX).
void netCleanup();

// Listening socket on the given port (0 = OS-assigned), returns fd or -1.
int netListen(std::uint16_t port);
// Accept one pending connection; returns the client fd or -1.
int netAccept(int listen_fd);
// Client: blocking connect to host:port; returns fd or -1.
int netConnect(const char* host, std::uint16_t port);

// Waits up to timeout_ms for `fd` to become readable. Returns 1 if readable,
// 0 on timeout, -1 on error.
int netWaitReadable(int fd, int timeout_ms);

void netSetNonBlocking(int fd);
// Best-effort send; returns bytes sent or -1 on error (including would-block
// and disconnect -- callers drop the client).
long netSend(int fd, const char* data, std::size_t len);
// Blocking receive into buf (max len); returns bytes read, 0 on orderly
// shutdown, -1 on error.
long netRecv(int fd, char* buf, std::size_t len);
void netClose(int fd);

const char* netErrorString();

} // namespace engine
