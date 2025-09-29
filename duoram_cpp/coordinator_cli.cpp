// coordinator_cli.cpp  (async READs to avoid deadlocks)
#include "common.hpp"
#include <boost/asio.hpp>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using boost::asio::ip::tcp;

// ========= Random helpers =========
template <class URNG>
ringArithmetic random_ring_elem(URNG& rng) {
    static_assert(std::is_unsigned_v<typename URNG::result_type>);
    static std::uniform_int_distribution<uint32_t> dist(0u, ringArithmetic::MASK);
    return ringArithmetic(dist(rng));
}
template <class URNG>
std::vector<ringArithmetic> make_random_vector(std::size_t dim, URNG& rng) {
    std::vector<ringArithmetic> v; v.reserve(dim);
    for (std::size_t i = 0; i < dim; ++i) v.emplace_back(random_ring_elem(rng));
    return v;
}
inline std::vector<ringArithmetic> make_random_vector(std::size_t dim) {
    std::random_device rd; std::mt19937_64 rng(rd());
    return make_random_vector(dim, rng);
}
std::pair<std::vector<ringArithmetic>, std::vector<ringArithmetic>>
makeStandardBasis(std::size_t dim, std::size_t index, ringArithmetic value){
    std::vector<ringArithmetic> e(dim, ringArithmetic(0));
    if(index >= dim) throw std::out_of_range("Index out of range for standard basis vector");
    e[index] = value;
    std::vector<ringArithmetic> f = make_random_vector(dim);
    for(std::size_t i=0;i<dim;i++) e[i] -= f[i];
    return {e,f};
}

// ========= Endian & socket helpers =========
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
static void write_all(tcp::socket& s, const void* p, std::size_t n){ boost::asio::write(s, boost::asio::buffer(p, n)); }
static void read_all (tcp::socket& s, void* p, std::size_t n){ boost::asio::read (s, boost::asio::buffer(p, n)); }
static void write_u8  (tcp::socket& s, uint8_t v){ write_all(s, &v, 1); }
static uint8_t read_u8(tcp::socket& s){ uint8_t v=0; read_all(s,&v,1); return v; }
static void write_be32_u32(tcp::socket& s, uint32_t v){ v = to_be32(v); write_all(s, &v, 4); }
static uint32_t read_be32_u32(tcp::socket& s){ uint32_t v=0; read_all(s, &v, 4); return from_be32(v); }
static tcp::socket connect_to(boost::asio::io_context& io, const std::string& host, const std::string& port){
    tcp::resolver res(io); auto eps = res.resolve(host, port);
    tcp::socket sock(io); boost::asio::connect(sock, eps); return sock;
}

// ========= CLI parsing & protocol =========
struct HostPort { std::string host, port; };
static HostPort parse_hp(const std::string& s){
    auto p = s.find(':'); if(p==std::string::npos) throw std::invalid_argument("expected host:port");
    return {s.substr(0,p), s.substr(p+1)};
}
enum : uint8_t {
    OP_WRITE_VEC   = 0x40,
    OP_READ_SECURE = 0x41
};

// ========= Single-client helpers =========
static void send_vector_to_client(const HostPort& hp, uint8_t op,
                                  const std::vector<ringArithmetic>& vec)
{
    boost::asio::io_context io;
    auto sock = connect_to(io, hp.host, hp.port);
    const uint32_t dim = static_cast<uint32_t>(vec.size());

    write_u8(sock, op);
    write_be32_u32(sock, dim);
    for(uint32_t i=0;i<dim;++i) write_be32_u32(sock, static_cast<uint32_t>(vec[i]));

    if(op == OP_WRITE_VEC){
        char ok[2]; boost::system::error_code ec;
        sock.read_some(boost::asio::buffer(ok,2), ec); // best-effort
    }
}

static uint32_t send_vector_and_get_share(const HostPort& hp,
                                          const std::vector<ringArithmetic>& vec)
{
    boost::asio::io_context io;
    auto sock = connect_to(io, hp.host, hp.port);
    const uint32_t dim = static_cast<uint32_t>(vec.size());

    write_u8(sock, OP_READ_SECURE);
    write_be32_u32(sock, dim);
    for(uint32_t i=0;i<dim;++i) write_be32_u32(sock, static_cast<uint32_t>(vec[i]));
    uint32_t share = read_be32_u32(sock);
    return share;
}

// ========= Usage =========
static void usage(const char* prog){
    std::cerr <<
    "Usage:\n"
    "  " << prog << " --op read  --dim N --idx I --c0 H:P --c1 H:P\n"
    "  " << prog << " --op write --dim N --idx I --val V --c0 H:P --c1 H:P\n"
    "Notes:\n"
    "  - READ runs both requests concurrently to avoid deadlocks.\n"
    "  - WRITE sends share vectors to both clients.\n";
}

// ========= Main =========
int main(int argc, char** argv){
    std::string op;
    std::size_t dim = 0, idx = 0;
    uint64_t val = 0;
    std::string c0_s, c1_s;

    for(int i=1;i<argc;++i){
        std::string a = argv[i];
        auto need = [&](int k){ if(i+k>=argc) throw std::runtime_error("missing arg after "+a); };
        if(a=="--op"){ need(1); op = argv[++i]; }
        else if(a=="--dim"){ need(1); dim = std::stoull(argv[++i]); }
        else if(a=="--idx"){ need(1); idx = std::stoull(argv[++i]); }
        else if(a=="--val"){ need(1); val = std::stoull(argv[++i]); }
        else if(a=="--c0"){ need(1); c0_s = argv[++i]; }
        else if(a=="--c1"){ need(1); c1_s = argv[++i]; }
        else if(a=="--help"){ usage(argv[0]); return 0; }
        else { std::cerr << "Unknown arg: " << a << "\n"; usage(argv[0]); return 1; }
    }

    if(op.empty() || dim==0 || c0_s.empty() || c1_s.empty()){
        usage(argv[0]); return 1;
    }
    if(idx >= dim){
        std::cerr << "Index out of range (idx < dim required)\n"; return 1;
    }

    HostPort c0 = parse_hp(c0_s);
    HostPort c1 = parse_hp(c1_s);

    try{
        if(op == "read"){
            // Basis e_idx split into two additive shares
            auto [share0_vec, share1_vec] = makeStandardBasis(dim, idx, ringArithmetic(1));

            auto fut0 = std::async(std::launch::async, [&]{ return send_vector_and_get_share(c0, share0_vec); });
            auto fut1 = std::async(std::launch::async, [&]{ return send_vector_and_get_share(c1, share1_vec); });

            uint32_t s0 = fut0.get();
            uint32_t s1 = fut1.get();
            uint32_t sum = static_cast<uint32_t>((static_cast<uint64_t>(s0) + s1) & ringArithmetic::MASK);
            std::cout << "READ idx=" << idx << " -> reconstructed value = " << sum << "\n";
        }
        else if(op == "write"){
            uint32_t vv = static_cast<uint32_t>(val & ringArithmetic::MASK);
            auto [share0_vec, share1_vec] = makeStandardBasis(dim, idx, ringArithmetic(vv));

            auto f0 = std::async(std::launch::async, [&]{ send_vector_to_client(c0, OP_WRITE_VEC, share0_vec); });
            auto f1 = std::async(std::launch::async, [&]{ send_vector_to_client(c1, OP_WRITE_VEC, share1_vec); });
            f0.get(); f1.get();

            std::cout << "WRITE idx=" << idx << " value=" << vv << " (mod 2^31) sent as shares\n";
        }
        else {
            std::cerr << "Unknown --op (use 'read' or 'write')\n";
            return 1;
        }

    } catch(const std::exception& e){
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }

    return 0;
}
