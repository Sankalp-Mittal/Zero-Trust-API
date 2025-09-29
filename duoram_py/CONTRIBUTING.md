# DUORAM (Python)

A lightweight two-server ORAM (DUORAM-style) toy implementation in Python, with a third helper service to stream correlated randomness. This README explains what the repo contains, how to run it, what the security/obliviousness guarantees are (including the fixed string size detail), and how communication/compute costs are organized—plus how to push work into preprocessing.

---

## What’s in this repo

* `share_server.py` — third-party helper that provides correlated randomness to the two servers.
* `bank_servers.py` — the two non-colluding ORAM servers (roles `A` and `B`). They maintain *shares* of the logical database and talk to each other over a peer channel.
* `user_facing_api.py` — a thin client/driver that issues **read** and **write** operations against the two servers.

> **Security model (informal):** The client secret-shares each request. Servers `A` and `B` receive different shares; neither server alone learns the access index or plaintext. The **third party** only supplies randomness; it does not see queries or data. If *both* servers collude, privacy is lost (standard 2-server assumption).

---

## Important implementation detail: fixed string length

This demo fixes the **record/string size to 10 characters** (i.e., `STR_SIZE = 10`) in the protocol. The **servers do not know** per-request payload lengths and do not need to—everything is padded to 10 characters and treated as opaque ring elements/bytes. In other words, the servers are **oblivious** to the true application string lengths; they only ever see fixed-size blocks. This avoids variable-length timing/size side channels in the toy setup.

If you change `STR_SIZE` in code, make sure the client and both servers use the **same** value; otherwise you’ll get decoding errors or silent corruption.

---

## Quickstart

### 1) Start the correlated randomness helper (optional for writes if your code supports that, but start it anyway for consistency)

```bash
python3 share_server.py --listen 0.0.0.0:9300
```

### 2) Start the two ORAM servers (A and B)

```bash
python3 bank_servers.py --role A --rows 100   --listen 0.0.0.0:9700 --peer-listen 9701   --peer 127.0.0.1:9801 --share 127.0.0.1:9300
```

```bash
python3 bank_servers.py --role B --rows 100   --listen 0.0.0.0:9800 --peer-listen 9801   --peer 127.0.0.1:9701 --share 127.0.0.1:9300
```

**Flags (servers):**

* `--role {A|B}`: which server you’re launching.
* `--rows N`: logical database size (number of fixed-size records).
* `--listen HOST:PORT`: public listen address for the server’s client API.
* `--peer-listen PORT`: local port used for A↔B link.
* `--peer HOST:PORT`: where this server dials the *other* server.
* `--share HOST:PORT`: the correlated randomness source (the helper).

> Start A and B in separate terminals. A and B must be able to reach each other on the given peer ports.

### 3) Perform a **read**

```bash
python3 user_facing_api.py --op read --dim 10 --idx 5   --c0 0.0.0.0:9700 --c1 0.0.0.0:9800
```

### 4) Perform a **write**

```bash
python3 user_facing_api.py --op write --dim 10 --idx 5 --val 1   --c0 0.0.0.0:9700 --c1 0.0.0.0:9800
```

**Flags (client):**

* `--op {read|write}`: operation type.
* `--dim D`: logical dimension/record length parameter used by the client logic (kept consistent with the fixed 10-char blocks).
* `--idx I`: logical index to access.
* `--val V`: value for writes (client packs it into the fixed-size block).
* `--c0 HOST:PORT`, `--c1 HOST:PORT`: endpoints for servers A and B.

> We have used 0 based indexing

---

## How the implementation works (high-level)

* **Secret sharing of state:** The logical database is represented implicitly by *shares* across A and B. Neither A nor B alone holds plaintext records; each holds an additive share. A simple mental model is additive sharing over a ring: `x = x_A + x_B` (or over `Z/2^kZ`), where `x_A` lives on A and `x_B` on B.

* **Oblivious access:** The client transforms a `(op, idx, [val])` into two message shares. Each server receives only its share; taken alone, each message is indistinguishable from random with respect to `idx` and the data.
  * For **reads**, A and B locally compute response shares from their stored state and the request share, optionally engage in a tiny back-and-forth with each other using pre-agreed randomness, and send response shares back to the client. The client recombines shares to recover the plaintext block (10 chars).
  * For **writes**, the client similarly sends shares of the update; the servers update their local shares so that recombination reflects the new value.
  * **Important Note:** The code implements sending shares as standard basis vectors the benifit being it can be implemented using **Distributed Point Functions (DPF's)** which reduces the communication cost from $O(N)$ to $O(log N)$

* **Correlated randomness (preprocessing):** `share_server.py` streams randomness (e.g., masks, seeds, or precomputed “triples”/one-time pads) to A and B. This lets the **online** phase do minimal computation and communication: expensive cryptographic sampling or vector masks are shifted **offline** into a preprocessing pool. (Currently in this implementation we do this online to allow for unbounded queries)

* **Fixed-size records:** All data is handled as fixed 10-byte (or 10-char) blocks. If your application strings are shorter, they’re padded; if longer, they must be chunked or truncated by your application logic before calling the client API. The servers never see the unpadded length.

---