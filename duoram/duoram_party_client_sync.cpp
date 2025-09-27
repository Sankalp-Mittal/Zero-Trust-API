#include "common.hpp"   // ringArithmetic, duoram
#include <boost/asio.hpp>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>

using boost::asio::ip::tcp;

// ===== Endian helpers =====
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
static inline void write_all(tcp::socket& s, const void* p, std::size_t n){ boost::asio::write(s, boost::asio::buffer(p, n)); }
static inline void read_all (tcp::socket& s, void* p, std::size_t n){ boost::asio::read (s, boost::asio::buffer(p, n)); }
static inline uint8_t  read_u8 (tcp::socket& s){ uint8_t v=0; read_all(s,&v,1); return v; }
static inline void     write_u8(tcp::socket& s, uint8_t v){ write_all(s,&v,1); }
static inline uint32_t read_be32_u32(tcp::socket& s){ uint32_t be=0; read_all(s,&be,4); return from_be32(be); }
static inline void     write_be32_u32(tcp::socket& s, uint32_t v){ v = to_be32(v); write_all(s,&v,4); }
static inline uint64_t read_be64_u64(tcp::socket& s){ uint64_t be=0; read_all(s,&be,8); return from_be64(be); }
static inline void     write_be64_u64(tcp::socket& s, uint64_t v){ v = to_be64(v); write_all(s,&v,8); }

static inline tcp::socket connect_to(boost::asio::io_context& io, const std::string& host, const std::string& port){
    tcp::resolver res(io);
    auto eps = res.resolve(host, port);
    tcp::socket sock(io);
    boost::asio::connect(sock, eps);
    return sock;
}

// ===== ring utils =====
static inline uint32_t raw31(const ringArithmetic& r){ return static_cast<uint32_t>(r); }
static inline ringArithmetic dot_ra(const std::vector<ringArithmetic>& a, const std::vector<ringArithmetic>& b){
    if(a.size()!=b.size()) throw std::runtime_error("dot: size mismatch");
    ringArithmetic acc(0);
    for(std::size_t i=0;i<a.size();++i) acc += a[i]*b[i];
    return acc;
}
static inline std::vector<uint32_t> to_raw(const std::vector<ringArithmetic>& v){
    std::vector<uint32_t> r; r.reserve(v.size());
    for(const auto& x: v) r.push_back(static_cast<uint32_t>(x));
    return r;
}
static inline std::vector<ringArithmetic> from_raw(const std::vector<uint32_t>& v){
    std::vector<ringArithmetic> r; r.reserve(v.size());
    for(auto x: v) r.emplace_back(ringArithmetic(x));
    return r;
}

// ===== Correlated randomness (Du-Atallah) from pairing server =====
struct DTAShare {
    uint32_t dim = 0;
    std::vector<ringArithmetic> a_i; // my a_i
    std::vector<ringArithmetic> b_i; // my b_i
    ringArithmetic c_i;              // my c_i
};

enum : uint8_t {
    OP_REQUEST  = 0x31, // client -> pairing server: [op][dim]
    OP_RESPONSE = 0x33  // server -> client: [op][dim][X(dim)][Y(dim)][Z]
};

static DTAShare fetch_dta_share(boost::asio::io_context& io,
                                const std::string& host, const std::string& port,
                                uint32_t dim)
{
    auto s = connect_to(io, host, port);
    write_u8(s, OP_REQUEST);
    write_be32_u32(s, dim);

    uint8_t op = read_u8(s);
    if(op != OP_RESPONSE) throw std::runtime_error("pairing server: bad op");
    uint32_t rdim = read_be32_u32(s);
    if(rdim != dim) throw std::runtime_error("pairing server: dim mismatch");

    DTAShare m; m.dim = dim;
    m.a_i.resize(dim); m.b_i.resize(dim);
    for(uint32_t i=0;i<dim;++i) m.a_i[i] = ringArithmetic(read_be32_u32(s));
    for(uint32_t i=0;i<dim;++i) m.b_i[i] = ringArithmetic(read_be32_u32(s));
    m.c_i = ringArithmetic(read_be32_u32(s));
    return m;
}

// ===== Peer residual exchange =====
static void send_vec(boost::asio::io_context& io,
                     const std::string& peer_host, const std::string& peer_port,
                     uint64_t sid, uint8_t tag,
                     const std::vector<ringArithmetic>& v)
{
    auto s = connect_to(io, peer_host, peer_port);
    write_be64_u64(s, sid);
    write_u8(s, tag);
    write_be32_u32(s, static_cast<uint32_t>(v.size()));
    for(const auto& w: v) write_be32_u32(s, static_cast<uint32_t>(w));
}

static std::vector<ringArithmetic> recv_vec(boost::asio::io_context& io,
                                            tcp::acceptor& peer_acc,
                                            uint64_t expect_sid, uint8_t expect_tag, uint32_t expect_dim)
{
    tcp::socket s(io);
    peer_acc.accept(s);
    uint64_t sid = read_be64_u64(s);
    uint8_t  tag = read_u8(s);
    uint32_t dim = read_be32_u32(s);
    if(sid!=expect_sid || tag!=expect_tag || dim!=expect_dim)
        throw std::runtime_error("peer residual header mismatch");
    std::vector<ringArithmetic> r(dim);
    for(uint32_t i=0;i<dim;++i) r[i] = ringArithmetic(read_be32_u32(s));
    return r;
}

// ===== Online phase for one inner-product <x, y> =====
// Party A = role "A" (index 0)     uses s_A =    - <u, b_A> - <a_A, v> + c_A
// Party B = role "B" (index 1)     uses s_B = <u, v> - <u, b_B> - <a_B, v> + c_B
// with u = x + a_i (sent by X-side), v = y + b_j (sent by Y-side).
static ringArithmetic dta_cross(boost::asio::io_context& io,
                                const std::string& my_role,             // "A" or "B"
                                const std::string& peer_host, const std::string& peer_port,
                                tcp::acceptor& peer_acc,
                                uint64_t sid, uint8_t tag,
                                bool i_am_X_side,                        // true: I send u; false: I send v
                                const std::vector<ringArithmetic>& my_input, // x if X-side, y if Y-side
                                const std::vector<ringArithmetic>& a_i,  // my a_i
                                const std::vector<ringArithmetic>& b_i,  // my b_i
                                const ringArithmetic& c_i)
{
    const uint32_t dim = static_cast<uint32_t>(my_input.size());

    std::vector<ringArithmetic> mine(dim);
    if(i_am_X_side){
        for(uint32_t i=0;i<dim;++i) mine[i] = my_input[i] + a_i[i]; // u
        send_vec(io, peer_host, peer_port, sid, tag, mine);
        auto peer_v = recv_vec(io, peer_acc, sid, tag, dim);        // v
        // s = role-dependent
        ringArithmetic s(0);
        if(my_role=="A"){
            s = ringArithmetic(0) - dot_ra(mine, b_i) - dot_ra(a_i, peer_v) + c_i;
        }else{
            s = dot_ra(mine, peer_v) - dot_ra(mine, b_i) - dot_ra(a_i, peer_v) + c_i;
        }
        return s;
    }else{
        for(uint32_t i=0;i<dim;++i) mine[i] = my_input[i] + b_i[i]; // v
        auto peer_u = recv_vec(io, peer_acc, sid, tag, dim);        // u
        send_vec(io, peer_host, peer_port, sid, tag, mine);
        ringArithmetic s(0);
        if(my_role=="A"){
            s = ringArithmetic(0) - dot_ra(peer_u, b_i) - dot_ra(a_i, mine) + c_i;
        }else{
            s = dot_ra(peer_u, mine) - dot_ra(peer_u, b_i) - dot_ra(a_i, mine) + c_i;
        }
        return s;
    }
}

// ===== User request ops =====
enum : uint8_t {
    OP_WRITE_VEC   = 0x40,
    OP_READ_SECURE = 0x41
};

int main(int argc, char** argv){
    // CLI
    std::string role = "A";                  // A or B
    std::string listen_host = "0.0.0.0";
    std::string listen_port = "9700";       // user requests
    std::string peer_listen_port = "9701";  // inbound residuals
    std::string peer_host = "127.0.0.1", peer_port = "9801"; // peer's residual listener
    std::string share_host = "127.0.0.1", share_port = "9300"; // pairing server
    std::size_t rows = 0;

    for(int i=1;i<argc;++i){
        std::string a = argv[i];
        auto need = [&](int k){ if(i+k>=argc) throw std::runtime_error("missing arg after "+a); };
        if(a=="--role"){ need(1); role = argv[++i]; }
        else if(a=="--rows"){ need(1); rows = static_cast<std::size_t>(std::stoull(argv[++i])); }
        else if(a=="--listen"){ need(1); std::string hp=argv[++i]; auto p=hp.find(':'); if(p==std::string::npos){ listen_port=hp; } else { listen_host=hp.substr(0,p); listen_port=hp.substr(p+1);} }
        else if(a=="--peer-listen"){ need(1); peer_listen_port = argv[++i]; }
        else if(a=="--peer"){ need(1); std::string hp=argv[++i]; auto p=hp.find(':'); if(p==std::string::npos){ peer_port=hp; } else { peer_host=hp.substr(0,p); peer_port=hp.substr(p+1);} }
        else if(a=="--share"){ need(1); std::string hp=argv[++i]; auto p=hp.find(':'); if(p==std::string::npos){ share_port=hp; } else { share_host=hp.substr(0,p); share_port=hp.substr(p+1);} }
        else if(a=="--help"){
            std::cout <<
              "Usage: " << argv[0] << " --role A|B --rows N [--listen H:P] [--peer-listen P]\n"
              "                         [--peer H:P] [--share H:P]\n";
            return 0;
        }
    }
    if(rows==0) { std::cerr<<"--rows required\n"; return 1; }
    if(!(role=="A" || role=="B")) { std::cerr<<"--role must be A or B\n"; return 1; }

    try{
        boost::asio::io_context io;

        // User acceptor
        tcp::resolver res(io);
        auto eps = res.resolve(listen_host, listen_port);
        tcp::endpoint ep = *eps.begin();
        tcp::acceptor acc(io);
        acc.open(ep.protocol());
        acc.set_option(boost::asio::socket_base::reuse_address(true));
        acc.bind(ep);
        acc.listen();

        // Peer residual acceptor
        auto peps = res.resolve(listen_host, peer_listen_port);
        tcp::endpoint pep = *peps.begin();
        tcp::acceptor peer_acc(io);
        peer_acc.open(pep.protocol());
        peer_acc.set_option(boost::asio::socket_base::reuse_address(true));
        peer_acc.bind(pep);
        peer_acc.listen();

        std::cout << "[party " << role << "] user @" << listen_host << ":" << listen_port
                  << " | residual-in @:" << peer_listen_port
                  << " | peer=" << peer_host << ":" << peer_port
                  << " | share=" << share_host << ":" << share_port
                  << " | rows=" << rows << "\n";

        duoram ram; ram.initialize(rows);

        for(;;){
            tcp::socket user(io);
            acc.accept(user);
            try{
                uint8_t op = read_u8(user);

                if(op==OP_WRITE_VEC){
                    uint32_t dim = read_be32_u32(user);
                    if(dim != ram.get_rows()) throw std::runtime_error("WRITE dim != rows");
                    std::vector<ringArithmetic> raw(dim);
                    for(uint32_t i=0;i<dim;++i) raw[i] = ringArithmetic(read_be32_u32(user));
                    ram.obliviousWrite(raw);
                    const char ok[2]={'O','K'}; write_all(user, ok, 2);
                    std::cout << "[party " << role << "] wrote vector of dim " << dim << "\n";
                }
                else if(op==OP_READ_SECURE){
                    uint32_t dim = read_be32_u32(user);
                    if(dim != ram.get_rows()) throw std::runtime_error("READ dim != rows");
                    std::vector<ringArithmetic> e_share(dim);
                    for(uint32_t i=0;i<dim;++i) e_share[i] = ringArithmetic(read_be32_u32(user));

                    std::cout<<"[party "<<role<<"] READ_SECURE dim "<<dim<<"\n";

                    // Fetch fresh DTA shares for this session (pairing server pairs both parties)
                    DTAShare dta = fetch_dta_share(io, share_host, share_port, dim);

                    // Local A_share vector
                    std::vector<ringArithmetic> A_share(dim);
                    for(uint32_t i=0;i<dim;++i) A_share[i] = ram.read(i);

                    // Unique session id
                    static std::atomic<uint64_t> g_ctr{0};
                    static uint64_t g_epoch = []{
                        uint64_t t = static_cast<uint64_t>(
                            std::chrono::steady_clock::now().time_since_epoch().count());
                        uint64_t r = (static_cast<uint64_t>(std::random_device{}()) << 32)
                                   ^ static_cast<uint64_t>(std::random_device{}());
                        return t ^ r;
                    }();
                    uint64_t sid = (g_epoch ^ (++g_ctr)) ^ static_cast<uint64_t>(dim);

                    // Cross-term 01: <A_i (me), e_j (peer)>
                    ringArithmetic z01 = dta_cross(io, role, peer_host, peer_port, peer_acc,
                                                   sid, 0x01,
                                                   /*i_am_X_side=*/(role=="A"),
                                                   (role=="A" ? A_share : e_share), // A sends x, B sends y
                                                   dta.a_i, dta.b_i, dta.c_i);

                    // Cross-term 10: <A_j (peer), e_i (me)>
                    ringArithmetic z10 = dta_cross(io, role, peer_host, peer_port, peer_acc,
                                                   sid, 0x10,
                                                   /*i_am_X_side=*/(role=="B"),
                                                   (role=="B" ? A_share : e_share), // B sends x, A sends y
                                                   dta.a_i, dta.b_i, dta.c_i);

                    // Self term: <A_i, e_i>
                    ringArithmetic self = dot_ra(A_share, e_share);

                    ringArithmetic my_share = self + z01 + z10;
                    write_be32_u32(user, static_cast<uint32_t>(my_share));
                }
                else{
                    throw std::runtime_error("unknown op");
                }

            } catch(const std::exception& e){
                std::cerr << "[party " << role << "] request error: " << e.what() << "\n";
                try{ user.close(); } catch(...) {}
            }
        }

    } catch(const std::exception& e){
        std::cerr << "[party fatal] " << e.what() << "\n";
        return 1;
    }
}
