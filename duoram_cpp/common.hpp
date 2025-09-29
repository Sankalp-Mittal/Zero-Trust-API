#pragma once
#include <cstdint>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <random>

// ======================= ringArithmetic (mod 2^31) =======================
class ringArithmetic {
public:
    static constexpr uint32_t MOD  = (1u << 31);
    static constexpr uint32_t MASK = MOD - 1;

    uint32_t value;

    // --- ctors ---
    constexpr ringArithmetic() : value(0) {}
    constexpr explicit ringArithmetic(uint32_t v) : value(static_cast<uint32_t>(v & MASK)) {}
    constexpr explicit ringArithmetic(int32_t  v)
        : value(static_cast<uint32_t>(static_cast<uint64_t>(v) & MASK)) {}

    // --- implicit conversion back to raw word ---
    constexpr explicit operator uint32_t() const { return value; }

    // --- helpers ---
    static constexpr bool is_unit(uint32_t a) { return (a & 1u) == 1u; }

    // Modular inverse modulo 2^31 (only for odd a), via Newton iteration
    static uint32_t inv_pow2(uint32_t a) {
        if (!is_unit(a)) throw std::domain_error("No inverse modulo 2^31 for even element");
        uint32_t x = 1u;
        for (int i = 0; i < 5; ++i) { // 1→2→4→8→16→32 bits
            uint64_t ax      = (static_cast<uint64_t>(a) * x) & MASK;
            uint64_t two_min = (2ull + MASK - ax) & MASK;               // (2 - a*x) mod 2^31
            x = static_cast<uint32_t>((static_cast<uint64_t>(x) * two_min) & MASK);
        }
        return x;
    }

    // --- unary ---
    constexpr ringArithmetic operator+() const { return *this; }
    constexpr ringArithmetic operator-() const {
        return ringArithmetic{static_cast<uint32_t>((0u - value) & MASK)};
    }

    // --- arithmetic (binary) ---
    friend constexpr ringArithmetic operator+(ringArithmetic a, const ringArithmetic& b) { a += b; return a; }
    friend constexpr ringArithmetic operator-(ringArithmetic a, const ringArithmetic& b) { a -= b; return a; }
    friend constexpr ringArithmetic operator*(ringArithmetic a, const ringArithmetic& b) { a *= b; return a; }
    friend ringArithmetic operator/(ringArithmetic a, const ringArithmetic& b) { a /= b; return a; }

    // --- compound assigns ---
    constexpr ringArithmetic& operator+=(const ringArithmetic& other) {
        value = (value + other.value) & MASK; return *this;
    }
    constexpr ringArithmetic& operator-=(const ringArithmetic& other) {
        value = (value - other.value) & MASK; return *this;
    }
    constexpr ringArithmetic& operator*=(const ringArithmetic& other) {
        value = static_cast<uint32_t>((static_cast<uint64_t>(value) * other.value) & MASK);
        return *this;
    }
    ringArithmetic& operator/=(const ringArithmetic& other) {
        uint32_t inv = inv_pow2(other.value);
        value = static_cast<uint32_t>((static_cast<uint64_t>(value) * inv) & MASK);
        return *this;
    }

    // --- ++ / -- ---
    ringArithmetic& operator++()    { value = (value + 1u) & MASK; return *this; }
    ringArithmetic  operator++(int) { ringArithmetic t=*this; ++(*this); return t; }
    ringArithmetic& operator--()    { value = (value - 1u) & MASK; return *this; }
    ringArithmetic  operator--(int) { ringArithmetic t=*this; --(*this); return t; }

    // --- comparisons ---
    friend constexpr bool operator==(const ringArithmetic& a, const ringArithmetic& b) { return a.value == b.value; }
    friend constexpr bool operator!=(const ringArithmetic& a, const ringArithmetic& b) { return !(a == b); }
    friend constexpr bool operator< (const ringArithmetic& a, const ringArithmetic& b) { return a.value <  b.value; }
    friend constexpr bool operator<=(const ringArithmetic& a, const ringArithmetic& b) { return a.value <= b.value; }
    friend constexpr bool operator> (const ringArithmetic& a, const ringArithmetic& b) { return a.value >  b.value; }
    friend constexpr bool operator>=(const ringArithmetic& a, const ringArithmetic& b) { return a.value >= b.value; }

    // --- stream I/O ---
    friend std::ostream& operator<<(std::ostream& os, const ringArithmetic& x) { return os << x.value; }
    friend std::istream& operator>>(std::istream& is, ringArithmetic& x) {
        uint64_t tmp; if (is >> tmp) x.value = static_cast<uint32_t>(tmp & MASK); return is;
    }
};

// ======================= duoram (local share) =======================
class duoram{
    std::vector<ringArithmetic> data;
    size_t rows = 0;

public:
    void initialize(size_t num_rows){
        rows = num_rows;
        data.assign(num_rows, ringArithmetic(0));
    }
    ringArithmetic read(size_t row){
        if(row >= rows) throw std::out_of_range("Row index out of range");
        return data[row];
    }
    void write(size_t row, ringArithmetic value){
        if(row >= rows) throw std::out_of_range("Row index out of range");
        data[row] = value;
    }
    std::size_t get_rows() const { return rows; }

    // oblivious add of a vector share
    void obliviousWrite(const std::vector<ringArithmetic>& toWrite){
        if(toWrite.size()!=rows) throw std::runtime_error("obliviousWrite: size mismatch");
        for(std::size_t i = 0 ; i < rows; i++) data[i] += toWrite[i];
    }

    ringArithmetic& operator[](std::size_t idx) { return data[idx]; }
    const ringArithmetic& operator[](std::size_t idx) const { return data[idx]; }

    duoram& operator=(const duoram& other){
        if (this != &other) { rows = other.rows; data = other.data; }
        return *this;
    }
    duoram& operator=(duoram&& other) noexcept {
        if (this != &other) { rows = other.rows; data = std::move(other.data); other.rows = 0; }
        return *this;
    }
};

// ======================= Du-Atallah share structs =======================
// We (re)interpret DuAtAllahClient fields as:
// X = a_i (my share of vector a), Y = b_i (my share of vector b), Z = c_i (my share of scalar c)
// with a = a0 + a1, b = b0 + b1, and c0 + c1 = <a, b>.
struct DuAtAllahClient{
    std::vector<ringArithmetic> X, Y; // a_i, b_i
    ringArithmetic Z;                 // c_i
};

struct DuAtAllahServer{
    std::vector<ringArithmetic> a0, a1, b0, b1;
    size_t dim = 0;

    template <class URNG>
    static ringArithmetic rand_elem(URNG& rng) {
        static std::uniform_int_distribution<uint32_t> dist(0u, ringArithmetic::MASK);
        return ringArithmetic(dist(rng));
    }
    template <class URNG>
    static std::vector<ringArithmetic> rand_vec(std::size_t n, URNG& rng) {
        std::vector<ringArithmetic> v; v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) v.emplace_back(rand_elem(rng));
        return v;
    }

    template <class URNG>
    DuAtAllahServer(size_t dimension, URNG& rng) : dim(dimension) {
        a0 = rand_vec(dim, rng);
        a1 = rand_vec(dim, rng);
        b0 = rand_vec(dim, rng);
        b1 = rand_vec(dim, rng);
    }
    DuAtAllahServer(size_t dimension) : dim(dimension) {
        std::random_device rd; std::mt19937_64 rng(rd());
        a0 = rand_vec(dim, rng);
        a1 = rand_vec(dim, rng);
        b0 = rand_vec(dim, rng);
        b1 = rand_vec(dim, rng);
    }

    static ringArithmetic dot(const std::vector<ringArithmetic>& u,
                              const std::vector<ringArithmetic>& v){
        if(u.size()!=v.size()) throw std::runtime_error("dot: size mismatch");
        ringArithmetic acc(0);
        for(std::size_t i=0;i<u.size();++i) acc += u[i]*v[i];
        return acc;
    }

    std::pair<DuAtAllahClient, DuAtAllahClient> getShares() const {
        // a = a0 + a1, b = b0 + b1, c = <a,b>
        std::vector<ringArithmetic> a(dim), b(dim);
        for(std::size_t i=0;i<dim;++i){ a[i] = a0[i] + a1[i]; b[i] = b0[i] + b1[i]; }
        ringArithmetic c = dot(a, b);

        // random split of c into c0, c1
        std::random_device rd; std::mt19937_64 rng(rd());
        ringArithmetic c0 = rand_elem(rng);
        ringArithmetic c1 = c - c0;

        DuAtAllahClient p0, p1;
        p0.X = a0; p0.Y = b0; p0.Z = c0; // party A
        p1.X = a1; p1.Y = b1; p1.Z = c1; // party B
        return {p0, p1};
    }
};
