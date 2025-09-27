#!/usr/bin/env python3
import base64, json, socket, threading, os, hmac, hashlib, pathlib
from typing import Tuple
from cryptography.hazmat.primitives import serialization, hashes
from cryptography.hazmat.primitives.asymmetric import rsa, padding
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

KEYDIR = pathlib.Path("./keys")
PRIV_PATH = KEYDIR / "private.pem"
PUB_PATH  = KEYDIR / "public.pem"

USERS = {
    "alice": {
        "password_sha256_hex": hashlib.sha256(b"correct horse battery staple").hexdigest()
    }
}

def ensure_keypair():
    KEYDIR.mkdir(exist_ok=True)
    if PRIV_PATH.exists() and PUB_PATH.exists():
        with open(PRIV_PATH, "rb") as f:
            priv = serialization.load_pem_private_key(f.read(), password=None)
        with open(PUB_PATH, "rb") as f:
            pub_pem = f.read()
        return priv, pub_pem
    priv = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    priv_pem = priv.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    pub_pem = priv.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    with open(PRIV_PATH, "wb") as f: f.write(priv_pem)
    with open(PUB_PATH,  "wb") as f: f.write(pub_pem)
    return priv, pub_pem

def aesgcm_encrypt(key: bytes, plaintext: bytes, aad: bytes=b""):
    nonce = os.urandom(12)
    ct = AESGCM(key).encrypt(nonce, plaintext, aad)
    return nonce, ct

def aesgcm_decrypt(key: bytes, nonce: bytes, ciphertext: bytes, aad: bytes=b""):
    return AESGCM(key).decrypt(nonce, ciphertext, aad)

def derive_next_rk(cur_rk: bytes, counter: int) -> bytes:
    return hmac.new(cur_rk, b"rotate"+counter.to_bytes(8,"big"), hashlib.sha256).digest()

def send_json(conn, obj):
    conn.sendall((json.dumps(obj)+"\n").encode())

def recv_json(conn):
    buf = b""
    while True:
        ch = conn.recv(1)
        if not ch: raise ConnectionError("peer closed")
        if ch == b"\n": break
        buf += ch
    return json.loads(buf.decode())

def handle_client(conn, addr, priv, pub_pem):
    try:
        # First message can be PUB or ENROLL
        msg = recv_json(conn)
        if msg.get("op") == "PUB":
            send_json(conn, {"op":"PUB", "public_pem_b64": base64.b64encode(pub_pem).decode()})
            msg = recv_json(conn)

        if msg.get("op") != "ENROLL":
            send_json(conn, {"error": "expected ENROLL"}); return

        # Decrypt RSA envelope
        ct = base64.b64decode(msg["payload_b64"])
        blob = priv.decrypt(
            ct,
            padding.OAEP(mgf=padding.MGF1(algorithm=hashes.SHA256()),
                         algorithm=hashes.SHA256(), label=None)
        )

        # Parse: Kc(32) | ulen(1) | username | hlen(1) | passhex
        if len(blob) < 34: send_json(conn, {"error":"malformed blob"}); return
        Kc = blob[:32]
        i = 32
        ulen = blob[i]; i += 1
        if i+ulen+1 > len(blob): send_json(conn, {"error":"bad username"}); return
        username = blob[i:i+ulen].decode(); i += ulen
        hlen = blob[i]; i += 1
        if i+hlen > len(blob): send_json(conn, {"error":"bad pass hash"}); return
        pass_hex = blob[i:i+hlen].decode()

        ok = USERS.get(username, {}).get("password_sha256_hex") == pass_hex
        if not ok:
            n, c = aesgcm_encrypt(Kc, b"AUTH_FAIL")
            send_json(conn, {"op":"AUTH","ok":False,
                             "nonce_b64": base64.b64encode(n).decode(),
                             "ct_b64": base64.b64encode(c).decode()})
            return

        # Send first rotating key (rk0) under Kc
        rk = os.urandom(32)
        counter = 0
        payload = json.dumps({"rk": base64.b64encode(rk).decode(), "counter": counter}).encode()
        n, c = aesgcm_encrypt(Kc, payload)
        send_json(conn, {"op":"AUTH","ok":True,
                         "nonce_b64": base64.b64encode(n).decode(),
                         "ct_b64": base64.b64encode(c).decode()})
        print(f"[server] {addr} authenticated as '{username}'")

        # Interactive rotating-key loop
        while True:
            msg = recv_json(conn)
            if msg.get("op") != "RK_MSG":
                send_json(conn, {"error":"expected RK_MSG"}); continue
            n = base64.b64decode(msg["nonce_b64"])
            c = base64.b64decode(msg["ct_b64"])
            pt = aesgcm_decrypt(rk, n, c)
            data = json.loads(pt.decode())

            if data.get("counter") != counter:
                send_json(conn, {"error":"bad counter"}); return

            # >>> Print the plaintext received <<<
            print(f"[server] from {addr} (ctr={counter}): {data.get('payload')}")

            # Rotate after processing inbound
            rk = derive_next_rk(rk, counter); counter += 1

            # Respond (ACK) with current rk/counter
            resp_pt = json.dumps({"payload":"ACK:"+str(data.get("payload")), "counter": counter}).encode()
            rn, rc = aesgcm_encrypt(rk, resp_pt)
            send_json(conn, {"op":"RK_MSG",
                             "nonce_b64": base64.b64encode(rn).decode(),
                             "ct_b64": base64.b64encode(rc).decode()})

            # Rotate for next inbound
            rk = derive_next_rk(rk, counter); counter += 1

    except Exception as e:
        try: send_json(conn, {"error": f"server exception: {type(e).__name__}: {e}"})
        except Exception: pass
    finally:
        conn.close()

def main():
    priv, pub_pem = ensure_keypair()
    host, port = "127.0.0.1", 5000
    print(f"[server] listening on {host}:{port}")
    print(f"[server] public key at {PUB_PATH.resolve()}")
    with socket.create_server((host, port), reuse_port=True) as srv:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle_client, args=(conn, addr, priv, pub_pem), daemon=True).start()

if __name__ == "__main__":
    main()
