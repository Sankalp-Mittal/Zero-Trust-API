// duatallah_pairing_server.cpp
#include "common.hpp"  // ringArithmetic, DuAtAllahClient, DuAtAllahServer
#include <boost/asio.hpp>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using boost::asio::ip::tcp;

// ---- 64-bit endian helpers (self-contained, no dependency on write_all) ----
static inline uint64_t to_be64(uint64_t x){
#if defined(_WIN32)
    return _byteswap_uint64(x);
#elif defined(__APPLE__)
    return OSSwapHostToBigInt64(x);
#else
    return htobe64(x);
#endif
}
static inline uint64_t from_be64(uint64_t x){
#if defined(_WIN32)
    return _byteswap_uint64(x);
#elif defined(__APPLE__)
    return OSSwapBigToHostInt64(x);
#else
    return be64toh(x);
#endif
}
static inline void write_be64_u64(tcp::socket& s, uint64_t v){
    v = to_be64(v);
    boost::asio::write(s, boost::asio::buffer(&v, 8));
}
static inline uint64_t read_be64_u64(tcp::socket& s){
    uint64_t be = 0;
    boost::asio::read(s, boost::asio::buffer(&be, 8));
    return from_be64(be);
}



// -------- Endian helpers (big-endian u32) --------
static inline uint32_t to_be32(uint32_t x){
#if defined(_WIN32)
    return _byteswap_ulong(x);
#elif defined(__APPLE__)
    return OSSwapHostToBigInt32(x);
#else
    return htobe32(x);
#endif
}
static inline uint32_t from_be32(uint32_t x){
#if defined(_WIN32)
    return _byteswap_ulong(x);
#elif defined(__APPLE__)
    return OSSwapBigToHostInt32(x);
#else
    return be32toh(x);
#endif
}

// -------- I/O helpers --------
static void write_all(tcp::socket& s, const void* p, std::size_t n){
    boost::asio::write(s, boost::asio::buffer(p, n));
}
static void read_all(tcp::socket& s, void* p, std::size_t n){
    boost::asio::read(s, boost::asio::buffer(p, n));
}
static uint8_t read_u8(tcp::socket& s){ uint8_t v=0; read_all(s, &v, 1); return v; }
static void write_u8(tcp::socket& s, uint8_t v){ write_all(s, &v, 1); }
static uint32_t read_be32_u32(tcp::socket& s){ uint32_t be=0; read_all(s, &be, 4); return from_be32(be); }
static void write_be32_u32(tcp::socket& s, uint32_t v){ v = to_be32(v); write_all(s, &v, 4); }

// -------- Protocol ops --------
enum : uint8_t {
    OP_REQUEST  = 0x31, // client -> server: [op][dim]
    OP_RESPONSE = 0x33  // server -> client: [op][dim][X(dim)][Y(dim)][Z]
};

// -------- Waiting room (pair by dimension) --------
class PairingRoom {
public:
    // Returns (peer_socket, dim) if a match is ready; else (nullptr, 0) and queues this socket.
    std::pair<std::shared_ptr<tcp::socket>, uint32_t>
    add_and_try_pair(std::shared_ptr<tcp::socket> s, uint32_t dim) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& dq = waiting_[dim];
        if (!dq.empty()) {
            auto peer = dq.front();
            dq.pop_front();
            if (dq.empty()) waiting_.erase(dim);
            return {peer, dim};
        } else {
            dq.push_back(std::move(s));
            return {nullptr, 0};
        }
    }

private:
    std::mutex mu_;
    std::map<uint32_t, std::deque<std::shared_ptr<tcp::socket>>> waiting_;
};

// -------- Serialization of DuAtAllahClient (X=a_i, Y=b_i, Z=c_i) --------
// server -> client: [OP_RESPONSE][dim:be32][sid:be64][X...][Y...][Z]
static void send_client_share(tcp::socket& s, uint32_t dim, uint64_t sid, const DuAtAllahClient& c){
    write_u8(s, OP_RESPONSE);
    write_be32_u32(s, dim);
    write_be64_u64(s, sid); // <= here
    for(uint32_t i=0;i<dim;++i) write_be32_u32(s, static_cast<uint32_t>(c.X[i]));
    for(uint32_t i=0;i<dim;++i) write_be32_u32(s, static_cast<uint32_t>(c.Y[i]));
    write_be32_u32(s, static_cast<uint32_t>(c.Z));
}



// -------- Per-connection handler --------
static void handle_one(PairingRoom& room, std::shared_ptr<tcp::socket> sock) {
    try {
        const uint8_t op  = read_u8(*sock);
        if (op != OP_REQUEST) throw std::runtime_error("bad op (expected OP_REQUEST)");
        const uint32_t dim = read_be32_u32(*sock);
        if (dim == 0) throw std::runtime_error("dim must be > 0");

        std::cout << "[server] client requesting dim " << dim << "\n";

        // Try to pair this socket. If no peer yet, just park it and return — DO NOT READ.
        auto [peer, pdim] = room.add_and_try_pair(sock, dim);
        if (!peer) {
            std::cout << "[server] queued; waiting for a peer in another thread\n";
            return; // keep socket alive via the shared_ptr held in room
        }

        (void)pdim;
        std::cout << "[server] paired; generating shares...\n";

        // Generate shares and send to both sockets.
        DuAtAllahServer gen(dim);
        auto [p0, p1] = gen.getShares();

        // single sid for both parties
        uint64_t sid = (static_cast<uint64_t>(std::random_device{}()) << 32)
                    ^ static_cast<uint64_t>(std::random_device{}());

        // first arrival gets p0, second gets p1
        send_client_share(*peer, dim, sid, p0);
        send_client_share(*sock , dim, sid, p1);

        std::cout << "[server] shares sent.\n";

    } catch (const std::exception& e) {
        try { std::cerr << "[server] connection error: " << e.what() << "\n"; sock->close(); }
        catch (...) {}
    }
}

// -------- Main: listen and accept forever --------
int main(int argc, char** argv) {
    std::string listen_host = "0.0.0.0";
    std::string listen_port = "9300";

    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        if (a == "--listen" && i+1 < argc) {
            std::string hp = argv[++i];
            auto pos = hp.find(':');
            if (pos == std::string::npos) listen_port = hp;
            else { listen_host = hp.substr(0, pos); listen_port = hp.substr(pos+1); }
        } else if (a == "--help") {
            std::cout << "Usage: " << argv[0] << " --listen HOST:PORT\n";
            return 0;
        }
    }

    try {
        boost::asio::io_context io;
        tcp::resolver res(io);
        tcp::acceptor acc(io);

        auto eps = res.resolve(listen_host, listen_port);
        tcp::endpoint ep = *eps.begin();
        acc.open(ep.protocol());
        acc.set_option(boost::asio::socket_base::reuse_address(true));
        acc.bind(ep);
        acc.listen();

        std::cout << "[server] listening on " << listen_host << ":" << listen_port << "\n";

        PairingRoom room;

        for (;;) {
            auto sock = std::make_shared<tcp::socket>(io);
            acc.accept(*sock);
            std::thread([&room, sock](){ handle_one(room, sock); }).detach();
        }

    } catch (const std::exception& e) {
        std::cerr << "[server] fatal error: " << e.what() << "\n";
        return 1;
    }
}
