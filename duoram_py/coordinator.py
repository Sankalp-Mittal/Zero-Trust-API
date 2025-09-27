#!/usr/bin/env python3
import argparse, socket, struct, random, threading

OP_WRITE_VEC   = 0x40
OP_READ_SECURE = 0x41

def pack_u8(x):  return struct.pack("!B", x)
def pack_u32(x): return struct.pack("!I", x & 0xFFFFFFFF)
def pack_i64(x): return struct.pack("!q", int(x))
def recv_exact(sock, n):
    b = b""
    while len(b) < n:
        chunk = sock.recv(n - len(b))
        if not chunk: raise ConnectionError("eof")
        b += chunk
    return b
def read_i64(s): return struct.unpack("!q", recv_exact(s,8))[0]

def connect(hostport):
    h,p = hostport.split(":")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((h, int(p)))
    return s

def rand_vec(n): return [random.randint(1,1024) for _ in range(n)]

def make_standard_basis_share(dim, idx, val):
    e = [0]*dim; e[idx] = int(val)
    f = rand_vec(dim)
    for i in range(dim): e[i] -= f[i]
    return e, f  # e = (val*e_idx) - f, f  -> e+f = val*e_idx

def write_vec(hp, vec):
    s = connect(hp)
    s.sendall(pack_u8(OP_WRITE_VEC))
    s.sendall(pack_u32(len(vec)))
    for v in vec: s.sendall(pack_i64(v))
    recv_exact(s, 2)  # "OK"
    s.close()

def read_share(hp, vec):
    s = connect(hp)
    s.sendall(pack_u8(OP_READ_SECURE))
    s.sendall(pack_u32(len(vec)))
    for v in vec: s.sendall(pack_i64(v))
    share = read_i64(s)
    s.close()
    return share

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--op", required=True, choices=["write","read"])
    ap.add_argument("--dim", type=int, required=True)
    ap.add_argument("--idx", type=int, required=True)
    ap.add_argument("--val", type=int, default=0)
    ap.add_argument("--c0", required=True)
    ap.add_argument("--c1", required=True)
    args = ap.parse_args()

    if args.idx >= args.dim: raise SystemExit("idx < dim required")

    if args.op == "write":
        e0, e1 = make_standard_basis_share(args.dim, args.idx, args.val)
        t0 = threading.Thread(target=write_vec, args=(args.c0, e0))
        t1 = threading.Thread(target=write_vec, args=(args.c1, e1))
        t0.start(); t1.start(); t0.join(); t1.join()
        print(f"WRITE idx={args.idx} value={args.val}")
    else:
        e0, e1 = make_standard_basis_share(args.dim, args.idx, 1)
        out = [None, None]
        def r0(): out[0] = read_share(args.c0, e0)
        def r1(): out[1] = read_share(args.c1, e1)
        t0 = threading.Thread(target=r0)
        t1 = threading.Thread(target=r1)
        t0.start(); t1.start(); t0.join(); t1.join()
        x = out[0] + out[1]
        print(f"READ idx={args.idx} -> {x}")

if __name__ == "__main__":
    main()
