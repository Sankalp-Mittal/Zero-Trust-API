#!/usr/bin/env python3
import argparse, socket, struct, random, threading

OP_WRITE_VEC   = 0x40
OP_READ_SECURE = 0x41
STR_SIZE = 10

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
    ap.add_argument("--val", type=str, default=0)
    ap.add_argument("--c0", required=True)
    ap.add_argument("--c1", required=True)
    args = ap.parse_args()

    # len of ram = 10 * dim

    if args.idx >= args.dim: raise SystemExit("idx < dim required")

    if args.op == "write":
        print(f"WRITE idx={args.idx} value={args.val}")

        stored_vals = [[0, 0] for _ in range(STR_SIZE)]
        threads = []
        sbv = []
        lock_c0 = threading.Lock()
        lock_c1 = threading.Lock()

        for i in range(STR_SIZE):
            ei0, ei1 = make_standard_basis_share(args.dim*STR_SIZE, STR_SIZE*args.idx + i, 1)
            sbv.append( (ei0, ei1) )
            def ri0(i=i):
                with lock_c0:
                    stored_vals[i][0] = read_share(args.c0, sbv[i][0])

            def ri1(i=i):
                with lock_c1:
                    stored_vals[i][1] = read_share(args.c1, sbv[i][1])

            ti0 = threading.Thread(target=ri0, daemon=True)
            ti1 = threading.Thread(target=ri1, daemon=True)
            ti0.start(); ti1.start()
            threads += [ti0, ti1]

        for t in threads: 
            t.join()
        
        print(f"READ idx={args.idx} -> shares {stored_vals}")

        vals_ascii = [0]*STR_SIZE
        for i in range(len(args.val)):
            if i >= STR_SIZE:
                print(f"WARNING: truncating input string to {STR_SIZE} chars")
                break
            vals_ascii[i] = ord(args.val[i])

        print(f"vals_ascii = {vals_ascii}")
        e0, e1 = [0]*args.dim * STR_SIZE, [0]*args.dim * STR_SIZE
        for i in range(STR_SIZE):
            ei0, ei1 = make_standard_basis_share(args.dim * STR_SIZE, STR_SIZE*args.idx + i, vals_ascii[i] - stored_vals[i][0] - stored_vals[i][1])
            e0 = [x+y for x,y in zip(e0, ei0)]
            e1 = [x+y for x,y in zip(e1, ei1)]
            
        t0 = threading.Thread(target=write_vec, args=(args.c0, e0))
        t1 = threading.Thread(target=write_vec, args=(args.c1, e1))
        t0.start(); t1.start(); t0.join(); t1.join()
        print(f"WRITE idx={args.idx} value={args.val}")

    else:
        stored_vals = [[0, 0] for _ in range(STR_SIZE)]
        threads = []
        sbv = []
        lock_c0 = threading.Lock()
        lock_c1 = threading.Lock()

        for i in range(STR_SIZE):
            ei0, ei1 = make_standard_basis_share(args.dim*STR_SIZE, STR_SIZE*args.idx + i, 1)
            sbv.append( (ei0, ei1) )
            def ri0(i=i):
                with lock_c0:
                    stored_vals[i][0] = read_share(args.c0, sbv[i][0])

            def ri1(i=i):
                with lock_c1:
                    stored_vals[i][1] = read_share(args.c1, sbv[i][1])

            ti0 = threading.Thread(target=ri0, daemon=True)
            ti1 = threading.Thread(target=ri1, daemon=True)
            ti0.start(); ti1.start()
            threads += [ti0, ti1]

        for t in threads: 
            t.join()
        
        # print(f"READ idx={args.idx} -> shares {stored_vals}")
        x = [a[0]+a[1] for a in stored_vals]
        s = "".join(map(chr, x))  
        print(f"READ idx={args.idx} -> {s}")

if __name__ == "__main__":
    main()
