#!/usr/bin/env python3
import argparse, socket, struct, threading, random, collections

OP_REQUEST  = 0x31  # client -> server:  [op][dim:u32]
OP_RESPONSE = 0x33  # server -> client:  [op][dim:u32][sid:i64][a_i:dim*i64][b_i:dim*i64][c_i:i64]

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

def rand_elem(): return random.randint(1, 1024)
def rand_vec(n): return [rand_elem() for _ in range(n)]
def dot(a,b):    return sum(x*y for x,y in zip(a,b))

waiting = collections.defaultdict(collections.deque)
waiting_mu = threading.Lock()

def send_share(s, dim, sid, a_i, b_i, c_i):
    s.sendall(pack_u8(OP_RESPONSE))
    s.sendall(pack_u32(dim))
    s.sendall(pack_i64(sid))
    for x in a_i: s.sendall(pack_i64(x))
    for x in b_i: s.sendall(pack_i64(x))
    s.sendall(pack_i64(c_i))

def handle_one(conn):
    try:
        op  = read_u8(conn)
        if op != OP_REQUEST: conn.close(); return
        dim = read_u32(conn)
        if dim == 0: conn.close(); return

        with waiting_mu:
            dq = waiting[dim]
            if dq:
                peer = dq.popleft()
                if not dq: waiting.pop(dim, None)
            else:
                dq.append(conn)
                return

        # generate correlated randomness
        a0, a1 = rand_vec(dim), rand_vec(dim)
        b0, b1 = rand_vec(dim), rand_vec(dim)
        a = [x+y for x,y in zip(a0,a1)]
        b = [x+y for x,y in zip(b0,b1)]
        c  = dot(a,b)
        c0 = rand_elem()
        c1 = c - c0
        sid = random.getrandbits(63)  # signed i64 domain; keep positive

        try:
            send_share(peer, dim, sid, a0, b0, c0)
        finally:
            try: peer.shutdown(socket.SHUT_RDWR)
            except: pass
            peer.close()

        try:
            send_share(conn, dim, sid, a1, b1, c1)
        finally:
            try: conn.shutdown(socket.SHUT_RDWR)
            except: pass
            conn.close()

    except Exception:
        try: conn.close()
        except: pass

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="0.0.0.0:9300")
    args = ap.parse_args()
    host, port = args.listen.split(":")[0], int(args.listen.split(":")[1])

    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((host, port))
    ls.listen()

    while True:
        c, _ = ls.accept()
        threading.Thread(target=handle_one, args=(c,), daemon=True).start()

if __name__ == "__main__":
    main()
