#!/usr/bin/env python3
import base64, json, os, socket, hashlib, hmac, sys
from cryptography.hazmat.primitives import serialization, hashes
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

def rsa_encrypt(pub_pem: bytes, plaintext: bytes) -> str:
    pub = serialization.load_pem_public_key(pub_pem)
    ct = pub.encrypt(
        plaintext,
        padding.OAEP(mgf=padding.MGF1(algorithm=hashes.SHA256()),
                     algorithm=hashes.SHA256(), label=None)
    )
    return base64.b64encode(ct).decode()

def aesgcm_encrypt(key: bytes, plaintext: bytes, aad: bytes=b""):
    nonce = os.urandom(12)
    ct = AESGCM(key).encrypt(nonce, plaintext, aad)
    return nonce, ct

def aesgcm_decrypt(key: bytes, nonce: bytes, ciphertext: bytes, aad: bytes=b""):
    return AESGCM(key).decrypt(nonce, ciphertext, aad)

def derive_next_rk(cur_rk: bytes, counter: int) -> bytes:
    return hmac.new(cur_rk, b"rotate"+counter.to_bytes(8,"big"), hashlib.sha256).digest()

def send_json(conn, obj): conn.sendall((json.dumps(obj)+"\n").encode())
def recv_json(conn):
    buf = b""
    while True:
        ch = conn.recv(1)
        if not ch: raise ConnectionError("server closed")
        if ch == b"\n": break
        buf += ch
    return json.loads(buf.decode())

def main():
    host, port = "127.0.0.1", 5000
    username = "alice"
    password = "correct horse battery staple"

    # Single connection for the whole session
    with socket.create_connection((host, port)) as s:
        # 1) Get server pubkey
        send_json(s, {"op":"PUB"})
        resp = recv_json(s)
        if resp.get("op") != "PUB":
            raise RuntimeError(f"unexpected PUB reply: {resp}")
        public_pem = base64.b64decode(resp["public_pem_b64"])

        # 2) Enroll on the same connection
        Kc = os.urandom(32)
        pass_hex = hashlib.sha256(password.encode()).hexdigest().encode()
        uname = username.encode()
        if len(uname) > 255 or len(pass_hex) > 255:
            raise ValueError("username/password too long for demo framing")
        blob = Kc + bytes([len(uname)]) + uname + bytes([len(pass_hex)]) + pass_hex
        payload_b64 = rsa_encrypt(public_pem, blob)
        send_json(s, {"op":"ENROLL", "payload_b64": payload_b64})

        auth = recv_json(s)
        if auth.get("op") != "AUTH":
            raise RuntimeError(f"unexpected reply: {auth}")
        n = base64.b64decode(auth["nonce_b64"])
        c = base64.b64decode(auth["ct_b64"])
        pt = aesgcm_decrypt(Kc, n, c)
        if not auth.get("ok"):
            print("[client] auth failed"); return

        info = json.loads(pt.decode())
        rk = base64.b64decode(info["rk"])
        counter = int(info["counter"])
        print(f"[client] interactive session ready. type messages; 'quit' to exit.")

        # 3) Interactive loop: stdin -> send -> receive ACK
        while True:
            try:
                line = input("> ")
            except (EOFError, KeyboardInterrupt):
                print("\n[client] bye"); break
            if line.strip().lower() == "quit":
                print("[client] bye"); break

            msg_pt = json.dumps({"payload": line, "counter": counter}).encode()
            nonce, ct = aesgcm_encrypt(rk, msg_pt)
            send_json(s, {"op":"RK_MSG",
                          "nonce_b64": base64.b64encode(nonce).decode(),
                          "ct_b64": base64.b64encode(ct).decode()})

            # After sending, rotate for server's reply
            rk = derive_next_rk(rk, counter); counter += 1

            # Wait for ACK and show it
            resp = recv_json(s)
            rn = base64.b64decode(resp["nonce_b64"])
            rc = base64.b64decode(resp["ct_b64"])
            rpt = aesgcm_decrypt(rk, rn, rc)
            rinfo = json.loads(rpt.decode())
            if rinfo["counter"] != counter:
                raise RuntimeError("counter mismatch on reply")
            print("[server ACK]", rinfo["payload"])

            # Rotate for next outbound
            rk = derive_next_rk(rk, counter); counter += 1

if __name__ == "__main__":
    main()
