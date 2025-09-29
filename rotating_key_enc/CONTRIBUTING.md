# Rotating-Key Auth Demo (Python)

This README documents the two files and how they plug into the earlier DUORAM Python project.

* `client.py` — the client (interactive REPL)
* `server.py` — the server (auth + rotating-key channel)

These are intended to be integrated with my DUORAM code from before (`share_server.py`, `bank_servers.py`, `user_facing_api.py`). The goal is to give me a simple authenticated, encrypted **control plane** that I can run alongside the DUORAM **data plane**.

---

## What this does

1. **Enrollment:** the client fetches the server’s RSA public key and sends an RSA-OAEP-encrypted envelope containing a fresh client session key `Kc`, the username, and a SHA-256(password) hex.

2. **Authentication:** the server checks the username/password hash (I keep a tiny in-memory user DB) and, if valid, returns an **initial rotating key** `rk` and a **counter** encrypted under `Kc` using **AES-GCM**.

3. **Interactive loop:** client and server exchange JSON messages protected by AES-GCM. After **each** message, both sides **rotate** the key with an HMAC-SHA256 schedule tied to the shared counter.

This secures small control messages (e.g., “start a DUORAM session”, health checks, admin commands) and is designed to sit beside my DUORAM protocol for secret-sharing data.

---

## File overview

### `server.py`

* Generates/loads a 2048-bit RSA keypair under `./keys/{private.pem, public.pem}`.
* Listens on `127.0.0.1:5000`.
* Supports:

  * `{"op":"PUB"}` → responds with the base64-encoded public key.
  * `{"op":"ENROLL","payload_b64":...}` → RSA-decrypts `Kc|ulen|username|hlen|pass_hex`, authenticates, then returns `{rk,counter}` under AES-GCM(Kc).
  * `{"op":"RK_MSG",...}` → decrypts with current `rk`, verifies `counter`, prints plaintext, replies with an **ACK** (also under AES-GCM).
* Rotates `rk` with:

  ```
  rk_{t+1} = HMAC_SHA256(key=rk_t, msg = b"rotate" || counter_be64)
  ```
* Demo users:

  ```python
  USERS = {
    "alice": {"password_sha256_hex": sha256("correct horse battery staple")}
  }
  ```

### `client.py`

* Connects, fetches the server’s public key, and enrolls:

  * Generates random `Kc` (32 bytes).
  * Sends RSA-OAEP envelope `Kc | ulen | username | hlen | pass_hex`.
* On success, decrypts `{rk, counter}` with `Kc` and enters a REPL:

  * For each input line: sends `{"payload": line, "counter": counter}` with AES-GCM(rk), then rotates for the incoming reply, verifies `counter`, prints ACK, rotates again for the next send.

---

## Install

```bash
python3 -m pip install cryptography
```

---

## Run

Open **two terminals**.

### Server

```bash
python3 server.py
```

You’ll see:

```
[server] listening on 127.0.0.1:5000
[server] public key at .../keys/public.pem
```

### Client

```bash
python3 client.py
```

Then:

```
[client] interactive session ready. type messages; 'quit' to exit.
> hello
[server ACK] ACK:hello
> quit
[client] bye
```

The server logs the decrypted payloads with counters:

```
[server] ... authenticated as 'alice'
[server] from ('127.0.0.1', ... ) (ctr=0): hello
```

---

## Wire protocol (newline-delimited JSON)

**Client → Server**

* `{"op":"PUB"}`
* `{"op":"ENROLL","payload_b64": base64(RSA_OAEP(Kc|ulen|username|hlen|pass_hex))}`
* `{"op":"RK_MSG","nonce_b64":..., "ct_b64":...}`

**Server → Client**

* `{"op":"PUB","public_pem_b64": base64(PEM)}`
* `{"op":"AUTH","ok":false,"nonce_b64":...,"ct_b64":...}` with AES-GCM(Kc, "AUTH_FAIL")
* `{"op":"AUTH","ok":true,"nonce_b64":...,"ct_b64":...}` with AES-GCM(Kc, JSON({"rk":b64, "counter":0}))
* `{"op":"RK_MSG","nonce_b64":..., "ct_b64":...}` with AES-GCM(rk, JSON({"payload":"ACK:...", "counter": next_counter}))

**Errors**

* `{"error":"...human-readable..."}`

---

## Crypto details

* **RSA (enrollment):** OAEP with SHA-256 (`MGF1(SHA256)`), 2048-bit keys.
* **AES-GCM:** 256-bit key, 12-byte random nonce, no AAD (but supported in helpers).
* **Key rotation (both directions):**
  `rk_{t+1} = HMAC_SHA256(rk_t, b"rotate" || counter_be64)`
  I rotate after each inbound, then after each outbound; the `counter` monotonically increases and must match on both sides.

> Security note about rotation: if a party learns the **current** `rk_t`, they can compute **future** keys (`rk_{t+1}, rk_{t+2}, …`). Prior keys remain hidden. If I need future-key secrecy after compromise, I’ll add a DH/HKDF ratchet later. Which uses a fixed key as well to generate the next key.

---

## Operational notes

* **Key storage:** `./keys` is created by the server; delete it to regenerate keys.
* **Lengths:** username and password-hash fields use 1-byte length prefixes (≤255). The client sends `hex(sha256(password))` during enrollment.
* **Nonces:** fresh 12 bytes per AES-GCM message; tags are handled internally.
* **Counters:** strictly monotonic; a mismatch is a protocol error.

---

## How to integrate with DUORAM

These files augment the earlier DUORAM project:

* **Existing:**

  * `share_server.py` — correlated randomness helper for the two DUORAM servers.
  * `bank_servers.py` — the two DUORAM servers (roles A & B).
  * `user_facing_api.py` — client for DUORAM read/write.

* **Plan to combine them**

  1. **Control plane:** Keep `server.py` running as the **auth front door**. After successful auth over the rotating-key channel, you can mint a short-lived token or export a derived sub-key that `user_facing_api.py` must present to A and B.
  2. **(Alternative) Socket wrapper:** I could embed the rotating-key layer directly into `bank_servers.py` sockets so DUORAM control frames ride the AES-GCM channel negotiated by RSA enrollment.
  3. **Record size consistency (DUORAM):** in my DUORAM demo I fixed record/string size to **10 characters** (`STR_SIZE = 10`). The servers are **oblivious** to true message lengths; everything is padded/treated as fixed blocks. This rotating-key layer doesn’t change that; it only protects the control traffic.