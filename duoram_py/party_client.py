#!/usr/bin/env python3
import argparse, socket, struct

# user <-> party ops
OP_WRITE_VEC   = 0x40
OP_READ_SECURE = 0x41

# pairing server ops
OP_REQUEST  = 0x31
OP_RESPONSE = 0x33

# ---------- BE helpers ----------
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
def read_u8(s):  return struct.unpack("!B", recv_exact(s,1))[0]
def read_u32(s): return struct.unpack("!I", recv_exact(s,4))[0]
def read_i64(s): return struct.unpack("!q", recv_exact(s,8))[0]

# ---------- net utils ----------
def connect(host, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host, port))
    return s

# ---------- algebra ----------
def dot(a,b): return sum(x*y for x,y in zip(a,b))

# ---------- residual exchange (send TWO vectors: u_part then v_part; both int64) ----------
def send_two_vecs(host, port, sid, tag, vec_u_part, vec_v_part):
    s = connect(host, port)
    s.sendall(pack_i64(sid))
    s.sendall(pack_u8(tag))
    s.sendall(pack_u32(len(vec_u_part)))
    for v in vec_u_part: s.sendall(pack_i64(v))
    s.sendall(pack_u32(len(vec_v_part)))
    for v in vec_v_part: s.sendall(pack_i64(v))
    s.close()

def recv_two_vecs(acceptor, expect_sid, expect_tag, expect_dim):
    s, _ = acceptor.accept()
    sid = read_i64(s)
    tag = read_u8(s)
    dim1 = read_u32(s)
    if sid!=expect_sid or tag!=expect_tag or dim1!=expect_dim:
        s.close()
        raise RuntimeError("peer residual header mismatch (u_part)")
    u_part = [read_i64(s) for _ in range(dim1)]
    dim2 = read_u32(s)
    if dim2!=expect_dim:
        s.close()
        raise RuntimeError("peer residual header mismatch (v_part)")
    v_part = [read_i64(s) for _ in range(dim2)]
    s.close()
    return u_part, v_part

# ---------- Du-Atallah cross-term (no mod) ----------
# Build public u = x - a, v = y - b via additive parts, then:
#   A: s =  u·b_A + a_A·v + c_A
#   B: s =  u·b_B + a_B·v + u·v + c_B
def dta_cross(role, io_acceptor, peer_host, peer_port, sid, tag,
              i_am_X_side, my_input, a_i, b_i, c_i):
    dim = len(my_input)

    if i_am_X_side:
        u_part_me = [ my_input[i] - a_i[i] for i in range(dim) ]  # x - a_i
        v_part_me = [ -b_i[i]               for i in range(dim) ]  # -b_i
        send_two_vecs(peer_host, peer_port, sid, tag, u_part_me, v_part_me)
        u_part_pe, v_part_pe = recv_two_vecs(io_acceptor, sid, tag, dim)
    else:
        u_part_me = [ -a_i[i]               for i in range(dim) ]  # -a_i
        v_part_me = [ my_input[i] - b_i[i] for i in range(dim) ]  # y - b_i
        u_part_pe, v_part_pe = recv_two_vecs(io_acceptor, sid, tag, dim)
        send_two_vecs(peer_host, peer_port, sid, tag, u_part_me, v_part_me)

    u = [ u_part_me[i] + u_part_pe[i] for i in range(dim) ]  # x - (a0+a1)
    v = [ v_part_me[i] + v_part_pe[i] for i in range(dim) ]  # y - (b0+b1)

    if role == "A":
        return dot(u, b_i) + dot(a_i, v) + c_i
    else:
        return dot(u, b_i) + dot(a_i, v) + dot(u, v) + c_i

# ---------- fetch correlated randomness ----------
def fetch_share(share_host, share_port, dim):
    s = connect(share_host, share_port)
    s.sendall(pack_u8(OP_REQUEST))
    s.sendall(pack_u32(dim))

    op  = read_u8(s)
    if op != OP_RESPONSE: raise RuntimeError("share server: bad op")
    rdim = read_u32(s)
    if rdim != dim: raise RuntimeError("share server: dim mismatch")
    sid = read_i64(s)
    a_i = [read_i64(s) for _ in range(dim)]
    b_i = [read_i64(s) for _ in range(dim)]
    c_i = read_i64(s)
    s.close()
    return sid, a_i, b_i, c_i

# ---------- party service ----------
def serve(role, rows, listen_host, listen_port,
          peer_listen_port, peer_host, peer_port,
          share_host, share_port):

    A_share = [0]*rows  # local RAM share

    # acceptor for residuals
    res_ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    res_ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    res_ls.bind((listen_host, peer_listen_port))
    res_ls.listen()

    # acceptor for user requests
    user_ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    user_ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    user_ls.bind((listen_host, listen_port))
    user_ls.listen()

    while True:
        conn, _ = user_ls.accept()
        try:
            op = read_u8(conn)

            if op == OP_WRITE_VEC:
                dim = read_u32(conn)
                if dim != rows: raise RuntimeError("WRITE dim != rows")
                vec = [read_i64(conn) for _ in range(dim)]
                for i in range(rows): A_share[i] += vec[i]
                print(f"[{role}] WRITE {vec} -> A_share now {A_share}")
                conn.sendall(b"OK")

            elif op == OP_READ_SECURE:
                dim = read_u32(conn)
                if dim != rows: raise RuntimeError("READ dim != rows")
                e_share = [read_i64(conn) for _ in range(dim)]

                sid, a_i, b_i, c_i = fetch_share(share_host, share_port, dim)

                z01 = dta_cross(role, res_ls, peer_host, peer_port, sid, 0x01,
                                i_am_X_side=(role=="A"),
                                my_input=(A_share if role=="A" else e_share),
                                a_i=a_i, b_i=b_i, c_i=c_i)
                z10 = dta_cross(role, res_ls, peer_host, peer_port, sid, 0x10,
                                i_am_X_side=(role=="B"),
                                my_input=(A_share if role=="B" else e_share),
                                a_i=a_i, b_i=b_i, c_i=c_i)

                self_term = dot(A_share, e_share)
                my_share  = self_term + z01 + z10
                conn.sendall(pack_i64(my_share))
        finally:
            conn.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--role", required=True, choices=["A","B"])
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--listen", default="0.0.0.0:9700")
    ap.add_argument("--peer-listen", type=int, default=9701)
    ap.add_argument("--peer", default="127.0.0.1:9801")
    ap.add_argument("--share", default="127.0.0.1:9300")
    args = ap.parse_args()

    lh, lp = args.listen.split(":")[0], int(args.listen.split(":")[1])
    ph, pp = args.peer.split(":")[0],   int(args.peer.split(":")[1])
    sh, sp = args.share.split(":")[0],  int(args.share.split(":")[1])

    serve(args.role, args.rows, lh, lp, args.peer_listen, ph, pp, sh, sp)

if __name__ == "__main__":
    main()
