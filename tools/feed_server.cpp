// engine_feed -- Phase 6 market-data server + streaming client.
//
// The full pipeline on one machine:
//
//   generator thread -> ConcurrentBook (matcher) -> RingMarketDataSink
//   -> SPSCRing -> publisher thread -> TCP clients
//
// The matcher is the single source of truth for market data (see
// market_data.hpp); the sink forwards its fills and best bid/ask into a
// lock-free ring, and the publisher broadcasts them as text lines:
//
//   B <best_bid> <best_ask>    top-of-book update (bid/ask 0 = empty side)
//   T <price> <qty> <bid> <ask>  trade print
//
// Usage:
//   engine_feed [--ops N] [--seed S] [--port P] [--rate R] [--replay FILE]
//               [--client [--host H]]
//
// Server mode (default): streams the feed until the op stream is exhausted.
//   --rate R paces submissions to R ops/sec (0 = as fast as possible) so the
//   book is watchable.
// Client mode: connects and prints the live feed, rendering the top of book
// and trade prints. Exits when the server closes the connection.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "engine/concurrent_book.hpp"
#include "engine/market_data.hpp"
#include "engine/net.hpp"
#include "engine/order_stream.hpp"

using namespace engine;

namespace {

struct Options {
    std::size_t ops = 100000;
    std::uint64_t seed = 1;
    std::uint16_t port = 9000;
    std::size_t rate = 0; // ops/sec, 0 = unlimited
    std::string replay;
    bool client = false;
    std::string host = "127.0.0.1";
};

void printUsage() {
    std::cout <<
        "usage: engine_feed [--ops N] [--seed S] [--port P] [--rate R]\n"
        "                   [--replay FILE] [--client [--host H]]\n"
        "\n"
        "  server (default): stream trades + top-of-book over TCP\n"
        "  --client          connect as a viewer instead of serving\n"
        "  --rate R          pace submissions to R ops/sec (0 = fastest)\n";
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            printUsage();
            return false;
        }
        if (a == "--ops") opt.ops = std::stoull(argv[++i]);
        else if (a == "--seed") opt.seed = std::stoull(argv[++i]);
        else if (a == "--port") opt.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        else if (a == "--rate") opt.rate = std::stoull(argv[++i]);
        else if (a == "--replay") opt.replay = argv[++i];
        else if (a == "--host") opt.host = argv[++i];
        else if (a == "--client") opt.client = true;
        else {
            std::cerr << "engine_feed: unknown option '" << a << "'\n";
            printUsage();
            return false;
        }
    }
    return true;
}

std::string formatEvent(const MarketDataEvent& e) {
    std::ostringstream os;
    if (e.kind == MarketDataEvent::Kind::Trade) {
        os << "T " << e.fill.price << ' ' << e.fill.quantity << ' ' << e.best_bid << ' '
           << e.best_ask << '\n';
    } else {
        os << "B " << e.best_bid << ' ' << e.best_ask << '\n';
    }
    return os.str();
}

// --- Server state -----------------------------------------------------------
std::atomic<bool> g_running{true};
std::mutex g_clients_mutex;
std::vector<int> g_clients;

void broadcast(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    for (auto it = g_clients.begin(); it != g_clients.end();) {
        if (netSend(*it, line.data(), line.size()) < 0) {
            netClose(*it); // slow or gone: drop, don't block the publisher
            it = g_clients.erase(it);
        } else {
            ++it;
        }
    }
}

void runServer(const Options& opt, const std::vector<OrderOp>& ops) {
    RingMarketDataSink sink(1u << 16);
    ConcurrentBook book(1, 1u << 16, -1, 0, 100000, 1u << 20, &sink);

    // Accept connections until shutdown.
    std::thread acceptor([&] {
        const int listen_fd = netListen(opt.port);
        if (listen_fd < 0) {
            std::cerr << "engine_feed: " << netErrorString() << "\n";
            g_running.store(false);
            return;
        }
        std::cout << "engine_feed: listening on port " << opt.port << "\n";
        while (g_running.load()) {
            if (netWaitReadable(listen_fd, 100) == 1) {
                const int c = netAccept(listen_fd);
                if (c >= 0) {
                    netSetNonBlocking(c);
                    std::lock_guard<std::mutex> lock(g_clients_mutex);
                    g_clients.push_back(c);
                    std::cout << "engine_feed: client connected (" << g_clients.size()
                              << " total)\n";
                }
            }
        }
        netClose(listen_fd);
    });

    // Publisher: pop market-data events and broadcast to every client.
    std::thread publisher([&] {
        MarketDataEvent e;
        for (;;) {
            bool drained = false;
            while (sink.tryPop(e)) {
                broadcast(formatEvent(e));
                drained = true;
            }
            if (!g_running.load()) break;
            if (!drained) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });

    // Producer: submit the op stream (optionally paced).
    std::thread producer([&] {
        const auto period = opt.rate > 0
                                ? std::chrono::microseconds(1'000'000 / opt.rate)
                                : std::chrono::microseconds(0);
        for (const auto& op : ops) {
            book.submitOp(op);
            if (period.count() > 0) {
                std::this_thread::sleep_for(period);
            }
        }
    });

    producer.join();
    // Let the publisher flush what's left, then shut everything down.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_running.store(false);
    publisher.join();
    acceptor.join();
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        for (int fd : g_clients) netClose(fd);
        g_clients.clear();
    }
    book.stop();
}

void runClient(const Options& opt) {
    const int fd = netConnect(opt.host.c_str(), opt.port);
    if (fd < 0) {
        std::cerr << "engine_feed: " << netErrorString() << "\n";
        return;
    }
    std::cout << "engine_feed: connected to " << opt.host << ':' << opt.port
              << " (ctrl-c to quit)\n";

    char buf[4096];
    std::string pending;
    Price bid = 0, ask = 0;
    for (;;) {
        const long n = netRecv(fd, buf, sizeof buf);
        if (n <= 0) break; // orderly shutdown or error
        pending.append(buf, static_cast<std::size_t>(n));
        std::size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (line.empty()) continue;
            std::istringstream ls(line.substr(1));
            if (line[0] == 'B') {
                ls >> bid >> ask;
                std::cout << "book:  bid " << bid << "   ask " << ask << "\n";
            } else if (line[0] == 'T') {
                Price px = 0, b = 0, a = 0;
                Quantity q = 0;
                ls >> px >> q >> b >> a;
                std::cout << "TRADE: " << px << " x " << q << "   (bid " << b << " ask " << a
                          << ")\n";
            } else {
                std::cout << line << "\n";
            }
        }
    }
    netClose(fd);
    std::cout << "engine_feed: disconnected\n";
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        return argc > 1 ? 1 : 0;
    }
    if (!netInit()) {
        std::cerr << "engine_feed: network init failed\n";
        return 1;
    }

    if (opt.client) {
        runClient(opt);
        netCleanup();
        return 0;
    }

    std::vector<OrderOp> ops;
    if (!opt.replay.empty()) {
        std::ifstream in(opt.replay);
        if (!in) {
            std::cerr << "engine_feed: cannot open '" << opt.replay << "'\n";
            netCleanup();
            return 1;
        }
        ops = loadOps(in);
        std::cout << "engine_feed: replaying " << ops.size() << " ops from " << opt.replay << "\n";
    } else {
        OpStreamGenerator gen(OpStreamGenerator::Config{.seed = opt.seed, .count = opt.ops});
        for (auto op = gen.next(); op; op = gen.next()) {
            ops.push_back(*op);
        }
        std::cout << "engine_feed: " << ops.size() << " ops (seed " << opt.seed << ")\n";
    }

    runServer(opt, ops);
    netCleanup();
    return 0;
}
