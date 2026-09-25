# NoMiddle

A **serverless / peer-to-peer encrypted messenger** built in C++20. There is no central
"middle" server: every participant runs their own node, and nodes exchange messages
directly over HTTPS. Each node is a full member of the network — web UI, HTTP API,
WebSocket notifications, message database, and cryptographic identity in one binary.

```
[ Node A: server + DB + UI ] --- HTTPS ---> [ Node B: server + DB + UI ]
```

---

## Table of Contents

- [NoMiddle](#nomiddle)
  - [Table of Contents](#table-of-contents)
  - [Overview](#overview)
  - [Features](#features)
  - [Architecture](#architecture)
  - [How messaging works](#how-messaging-works)
    - [Direct message (simplified)](#direct-message-simplified)
    - [Offline peers (retry worker)](#offline-peers-retry-worker)
    - [Editing a message (direct)](#editing-a-message-direct)
    - [Deleting a message (direct)](#deleting-a-message-direct)
    - [Group chats](#group-chats)
  - [Crypto \& security](#crypto--security)
  - [Requirements](#requirements)
  - [Building](#building)
  - [Running](#running)
    - [TLS certificates](#tls-certificates)
  - [Docker \& ngrok deployment](#docker--ngrok-deployment)
  - [Configuration](#configuration)
  - [Web UI guide](#web-ui-guide)
  - [API reference](#api-reference)
    - [Public (no auth)](#public-no-auth)
    - [Authenticated](#authenticated)
    - [Static / misc](#static--misc)
  - [WebSocket protocol](#websocket-protocol)
  - [Database schema](#database-schema)
  - [Project structure](#project-structure)
  - [Troubleshooting](#troubleshooting)
  - [Limitations \& roadmap](#limitations--roadmap)

---

## Overview

- **Language / tech:** C++20, [Crow](https://github.com/CrowCpp/Crow) (HTTP + WebSocket),
  SQLite3, [libsodium](https://doc.libsodium.org/), [cpr](https://github.com/libcpr/cpr)
  (outgoing HTTPS), Asio.
- **No account system.** Each node owns a permanent cryptographic identity (a libsodium
  Curve25519 X25519 keypair). A node's "ID" is simply its base64-encoded **public key**.
- **Contacts** are remote nodes: a contact = `public_key` + `name` + `server_address`
  (host-domain / IP, optionally with port). To add a contact you only need to know their address;
  the public key is fetched automatically.
- **Messages never pass through a third party.** The sender node encrypts a message
  specifically for the recipient's public key and delivers the ciphertext straight to the
  recipient's server via `POST /accept_message`.

---

## Features

- **End-to-end encrypted direct messages** (libsodium `crypto_box` with random nonces;
  only the recipient's private key can decrypt).
- **Message delivery statuses:** pending → delivered / failed / deleted.
- **Edit and delete messages** in both direct chats and group chats (re-encrypted and
  fanned out per recipient).
- **Group chats:** create groups, add / remove members, per-member encryption, group
  membership "snapshots" synchronized to every member.
- **Offline delivery:** a background *retry worker* re-delivers pending messages, edits,
  deletes and group updates every 30 seconds (gives up after 24 hours).
- **Real-time updates** over WebSocket — new / edited / deleted messages and incoming
  device-authorization requests arrive instantly.
- **Device management:** the first client is provisioned automatically; any additional
  device must be approved by an already-authenticated device (request/approve flow).
- **Last message preview** and **chat list sorted by last activity** (most recent first),
  the same format for direct chats and groups.
- **Message timestamps** with smart formatting (today `HH:MM`, `yesterday`, date for
  anything older).
- **Static web UI** served alongside the API over the same HTTPS port.
- **Runs anywhere:** bare metal (systemd, nohup, …) or Docker, optionally published via
  an [ngrok](https://ngrok.com) static domain.

---

## Architecture

A single `server` binary provides everything a node needs:

```
┌─────────────────────────────────────────────────────────────┐
│  server                                                      │
│  ├── Crow HTTP router (CROW_ENABLE_SSL)                      │
│  │   ├── Static files        web/ (index.html, script.js, …) │
│  │   ├── REST API            /api/*                          │
│  │   ├── P2P accept endpoints  /accept_*                     │
│  │   └── WebSocket           /ws/messages                    │
│  ├── db_manager              SQLite3 (NoMiddle.db)           │
│  ├── message_delivery        outgoing HTTPS to peer nodes    │
│  ├── retry_worker            background thread (30 s loop)   │
│  └── key_manager + crypto    libsodium box keys & sealing    │
└─────────────────────────────────────────────────────────────┘
```

**Node identity**

On first start the server checks for a private key file:

- Direct chat: `private_key<port>.bin` next to the database file
  (e.g. `private_key18080.bin` for port 18080).

If missing, a fresh `crypto_box` keypair is generated. The public key is printed to the
console on startup and is what other nodes use to identify you. **Back up this file** —
losing it means you can no longer decrypt your own node's key material.

**How other nodes reach you**

Every node-to-node request is `https://<server_address>/accept_...`. The web UI shows
your address (`host:port`) in the "My domain" box and offers a copy button, so you can
share it with friends.

---

## How messaging works

### Direct message (simplified)

```
Sender UI                    Sender node                    Recipient node
   │  POST /api/send_message     │                                  │
   │  {recipient_id, text}  ───▶ │                                  │
   │                             │ 1) encrypt(text, recipient_pk,   │
   │                             │         sender_sk)               │
   │                             │ 2) save row (accepted=0)        │
   │                             │ 3) POST https://peer/accept_message│
   │                             │    {message_id, sender_id,      │
   │                             │     recipient_id, ciphertext,   │
   │                             │     timestamp} ────────────────▶ │
   │                             │                                  │ 1) recipient_id == self?
   │                             │◀─────────────────── HTTP 200 ────│ 2) decrypt(ciphertext,
   │                             │    mark accepted=1               │         sender_pk, self_sk)
   │                             │                                  │ 3) store row (accepted=1)
   │                             │                                  │ 4) WS "new_message" → UI
   │  WS "new_message" ◀─────────┤                                  │
   │  (own echo → status ✓)      │                                  │
```

- The **sender** saves the message locally with `accepted = 0` and flips it to `1`
  only after the peer answers `200`.
- The ciphertext on the wire looks like this (base64 of):
  `nonce (24 bytes) || crypto_box_easy output (> plaintext + 16-byte MAC)`.
- The **plaintext is never transmitted** — it stays in the sender's local DB
  (`plaintext` column) and inside the recipient's DB after decryption.

### Offline peers (retry worker)

If the recipient node is offline, delivery fails and the message stays `accepted = 0`.
The **retry worker** thread wakes up every 30 seconds, collects:

- pending messages (`accepted = 0`),
- pending edits / deletes,
- pending group snapshots,

and retries each one. After **24 hours** without delivery a message is marked
`accepted = 2` (failed), an edit `mark_edit_failed`, a delete `mark_delete_failed`, and
they are no longer retried.

### Editing a message (direct)

`POST /api/edit_message {message_id, text}`:

1. Verifies the message exists and was sent by *you* (`sender_id == self`).
2. Re-encrypts the new text for the recipient.
3. Updates the local row (`plaintext`, `ciphertext`, `edited_at = now`).
4. Delivers to the peer via `POST /accept_edit`; on `200` both sides' `accepted`
   flags are updated; peers see `edited_at != 0` → UI shows **edited**.

### Deleting a message (direct)

`POST /api/delete_message {message_id}`:

1. Marks the message deleted locally (`accepted = 3`) and delete-pending.
2. Tells the peer via `POST /accept_delete`; the peer hides it too.
3. The WebSocket `delete_message` event removes it from every open chat instantly.

### Group chats

A group is a random UUID (`generate_uuid()`); there is **no group encryption key** —
every member row is encrypted *individually* for each member's public key.

**Mutual group membership ("snapshots").** Membership is not gossiped per user. When you
add or remove a member, your node re-serializes the entire member list into a *snapshot*
and pushes it to **every member's** server via `POST /accept_group_update`
(`group_updates` table keeps one row per member with an `accepted` flag). A member who
never had the group **auto-creates** it from the snapshot (name, creator, members). This
is what lets a newly-added member see the group and its history.

**Group message send:**

`POST /api/groups/<group_id>/messages {text}`:

- For each member (except yourself):
  - encrypt `text` with **that member's** public key,
  - store a `messages` row: same `message_id`, `group_id` set, `recipient_id = member`,
  - deliver `POST /accept_message` (with `group_id`) to that member.
- On the **sender** node there is therefore one row per member sharing one `message_id` —
  the UI de-duplicates them by `message_id`. On each **recipient** node there is exactly
  one row.
- Reply `200` from a member marks that member's row `accepted`; `delivered` is `false`
  if any member was unreachable.

**Group edit/delete** work the same way — per-member re-encryption, `accept_edit` /
`accept_delete` fan-out, per-recipient acceptance marks — plus a WebSocket event carrying
the `group_id` so all group chats refresh.

**Add / remove member:**

- `POST /api/groups/<id>/add_member {member_id, server_address}` — appends/updates the
  member, saves the new list, pushes the snapshot to everyone.
- `POST /api/groups/<id>/remove_member {member_id}` — removes the member, pushes the
  snapshot; if no members remain the group is deleted.

---

## Crypto & security

| Concern | Mechanism |
|---|---|
| Node identity | libsodium `crypto_box` X25519 keypair; pubkey base64 is the node ID |
| Message encryption | `crypto_box_easy` (Curve25519 + XSalsa20-Poly1305), random 24-byte nonce prepended to the payload |
| Header verification | MAC baked in by `crypto_box`; `crypto_box_open_easy` rejects tampering |
| Secrets on disk | client secrets are **never stored**; only `crypto_generichash(secret, salt)` |
| Client auth | HTTP headers `X-Client-Id` + `X-Client-Secret`; WebSocket JSON `auth` frame |
| Transport | HTTPS node-to-node; TLS certificate validation is **disabled** (`VerifySsl{false}`) because nodes use self-signed certs |
| Key handling | private key file chmod `0600`; key buffers wiped via `sodium_memzero` |

> **Trust model:** every node fully trusts the servers it talks to (they hold the
> recipients' private keys), so NoMiddle ensures *your* messages are confidential to
> third parties, but not "serverless secrets" in the zero-knowledge sense. TLS is not
> pin-verified between nodes.

---

## Requirements

- CMake ≥ 3.16
- C++20 compiler (GCC ≥ 10 / Clang ≥ 12)
- libcurl (with OpenSSL), OpenSSL, SQLite3, libsodium
- Internet access at **configure** time (CMake `FetchContent` pulls cpr, Asio, Crow)

Debian / Ubuntu:

```bash
sudo apt install build-essential cmake git pkg-config \
    libsqlite3-dev libcurl4-openssl-dev libssl-dev libsodium-dev zlib1g-dev
```

---

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The resulting binary is `build/server`.

---

## Running

```
./build/server <port> <db_path> [cert_path] [key_path]
```

| Argument    | Default       | Meaning                                       |
|-------------|---------------|-----------------------------------------------|
| `port`      | `18080`       | HTTP(S) listen port                           |
| `db_path`   | `NoMiddle.db` | SQLite database file                         |
| `cert_path` | `cert.pem`    | TLS certificate, or `none` for plain HTTP    |
| `key_path`  | `key.pem`     | TLS private key                              |

Examples:

```bash
# HTTPS (self-signed cert/key)
./build/server 18080 NoMiddle.db cert.pem key.pem

# Plain HTTP (LAN only; no encryption between browser and node)
./build/server 8080 NoMiddle.db none none
```

On startup you should see:

```
My public key: <base64…>
[KEYS] …
[SQL] Schema ready.
[RETRY_WORKER] Started
Server listening on https://0.0.0.0:18080 (TLS: cert.pem / key.pem)
```

Then open `https://<host>:<port>/` in a browser (browser will warn about the
self-signed certificate — accept it).

### TLS certificates

It is fine to use any self-signed cert for node-to-node traffic, since peers verify TLS
with `VerifySsl{false}`. Generate one with:

```bash
openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 365 \
    -keyout key.pem -out cert.pem \
    -subj "/C=US/O=NoMiddle/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
```

> If the node is exposed to the internet, prefer a real certificate (Let's Encrypt) for
> the browser-facing side.

---

## Docker & ngrok deployment

The repo ships a multi-stage `Dockerfile` (Debian bookworm) plus a
`docker-compose.yml` that runs two services:

1. `server` — the NoMiddle binary on port `${PORT}` (plain HTTP inside the container;
   data persisted in the `nomiddle-data` Docker volume at `/data/NoMiddle.db`).
2. `ngrok` — tunnels `${PORT}` to a public static domain `${NGROK_DOMAIN}`.

```bash
# 1) create .env (see Configuration)
cp .env.example .env

# 2) build & start
docker compose up -d --build

# 3) check logs
docker compose logs -f server ngrok
```

The public address is printed by ngrok / available in your ngrok dashboard.

> **Rebuilding after source changes:** the image copies `src/` and `web/` at build time,
> so after editing code always run `docker compose up -d --build` again.

---

## Configuration

`.env` is used by docker-compose (see `.env.example`):

```ini
# ngrok personal authtoken (dashboard.ngrok.com → Your Authtoken)
NGROK_AUTHTOKEN=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

# static domain assigned to your account (dashboard.ngrok.com → Static Domains)
NGROK_DOMAIN=my-chat.ngrok-free.dev

# port the NoMiddle server listens on inside the container / exposed to host
PORT=18081
```

Everything else is configured through **command-line arguments** (see
[Running](#running)); there are no other config files.

---

## Web UI guide

The single-page UI (`web/index.html` + `web/script.js` + `web/style.css`) supports:

- **Getting in:** open the node URL. If the node has no clients yet, the first
  "Request access" auto-provisions your first device (credentials stored in
  `localStorage`). Any later device must be approved from an already-connected device —
  the approval popup appears via WebSocket in the existing session.
- **My domain:** your node's address `host:port`, with a *Copy* button — share it with
  friends so they can add you.
- **Add contact:** `+` → *Add contact* → enter name + `ip or domain`. The public key is
  fetched automatically through the server (`GET /api/remote_public_key`), so you never
  type a key by hand.
- **Create group:** `+` → *Create group* → name + optional members from your contacts.
- **Chats column:** shows direct chats and groups together, sorted by last activity.
  Each row shows the last message in `Sender: text` format (You / contact name) with a
  timestamp.
- **Message list:** timestamps (`HH:MM` today, `yesterday`, date), sender names in
  groups, `edited` marker, delivery status (`✓` delivered, `✗` failed, `⏳` pending).
- **Edit / delete:** right-click a message you sent → *Copy / Edit / Delete*.
- **Chat settings:** the gear button in the header opens contact settings (renaming or
  changing the address) for the selected direct contact.
- **Devices:** the monitor button lists authenticated devices and lets you revoke a
  device; you cannot revoke the device you are currently using.

---

## API reference

All routes respond with JSON. Unless noted, endpoints require auth:

- Header auth: `X-Client-Id` and `X-Client-Secret`.
- Unauthorized → `401 {"error": "Unauthorized"}`, malformed input → `400`.

### Public (no auth)

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/public_key` | This node's identity public key: `{"public_key": "…"}`. Adds `Access-Control-Allow-Origin: *` (used by foreign UIs / discovery). |
| `POST` | `/api/auth/register` | Register the **first** client when the node has none. Returns `{"client_id", "secret"}`. `403` once a client exists (`"Node is locked"`). |
| `POST` | `/api/auth/request` | Device access request. Empty node → auto-provisions (`"auto": true`, same shape as register). Otherwise creates a pending `auth_requests` row, broadcasts `auth_request` over WS and returns `{auto:false, request_id, expires_at}` (10 min TTL). |
| `GET` | `/api/auth/request?request_id=…` | Poll request status: `pending` / `approved` (returns `client_id` + `secret`, then the request row is deleted) / `expired` / `unknown`. |
| `POST` | `/accept_message` | P2P: receive and decrypt a direct/group message. Validates `recipient_id == self`. |
| `POST` | `/accept_edit` | P2P: receive an edited message (re-encrypted for this node). |
| `POST` | `/accept_delete` | P2P: receive a delete for a message. |
| `POST` | `/accept_group_update` | P2P: receive a group membership snapshot; creates or updates the group locally. |

### Authenticated

| Method | Path | Body / Params | Description |
|---|---|---|---|
| `PUT` | `/api/upsert_contact` | `{contact_id, name, server_address}` | Create or update a contact. |
| `POST` | `/api/send_message` | `{recipient_id, text}` | Send a direct message. Returns `{message_id}`. |
| `POST` | `/api/edit_message` | `{message_id, text}` | Edit a message you sent. Returns `{message_id, plaintext, edited_at}`. |
| `POST` | `/api/delete_message` | `{message_id}` | Delete a message you sent. Returns `{message_id}`. |
| `GET` | `/api/contacts` | — | Contacts with `latest_message` snapshot (incl. `sender_id`, `timestamp`, `edited_at`). |
| `GET` | `/api/messages` | `?contact_id=<pubkey>` | Message history with a contact (**note:** base64 contains `/` and `=`, always URL-encode `contact_id`). |
| `POST` | `/api/groups/create` | `{name, server_address?}` | Create a group; creator becomes admin. Returns `{group_id}`. |
| `POST` | `/api/groups/<id>/add_member` | `{member_id, server_address, self_server_address?}` | Add/update a member and sync the snapshot. Returns `{delivered}`. |
| `POST` | `/api/groups/<id>/remove_member` | `{member_id}` | Remove a member and sync the snapshot. |
| `GET` | `/api/groups` | — | Groups with last-message preview (`last_message`, `last_message_sender_id`, `last_message_sender_name`, `last_message_timestamp`). |
| `GET` | `/api/groups/<id>/members` | — | `{"members": [{member_id, server_address, role}]}`. |
| `POST` | `/api/groups/<id>/messages` | `{text}` | Send a group message (fans out per member). Returns `{message_id, delivered}`. |
| `GET` | `/api/groups/<id>/messages` | — | Group message history with `sender_name`, `accepted`, `edited_at`. |
| `POST` | `/api/groups/<id>/edit_message` | `{message_id, text}` | Edit a group message (re-encrypts per member). |
| `POST` | `/api/groups/<id>/delete_message` | `{message_id}` | Delete a group message (fans out per member). |
| `GET` | `/api/remote_public_key` | `?addr=<host[:port]>` | **Server-side proxy** that fetches `https://<addr>/api/public_key` and returns the JSON. Used by the web UI to add contacts without browser CORS / ngrok interstitial issues. |
| `POST` | `/api/auth/approve` | `{request_id, allow}` | Approve or reject a pending device request (`allow` boolean). |
| `GET` | `/api/clients` | — | List authenticated devices. |
| `DELETE` | `/api/clients/<id>` | — | Revoke a device (not the current one). Returns `409` for unknown id. |

### Static / misc

| Method | Path | Description |
|---|---|---|
| `GET` | `/` | Health: `200 "ok"`. |
| `GET` | `/<file>` | Static files from `web/` (index.html, style.css, script.js, images…). |
| `WS` | `/ws/messages` | Real-time channel (below). |

> **P2P endpoints are unauth-by-design** — they are called by other nodes' servers, not
> browsers, and every request is cryptographically validated by attempting to decrypt
> the payload.

---

## WebSocket protocol

`/ws/messages`, text frames only.

**Client → server**

```json
{ "type": "auth", "client_id": "…", "secret": "…" }
```

The server replies `{"type":"auth_ok"}` or closes the socket.

**Server → client**

| Type | Payload fields | Meaning |
|---|---|---|
| `auth_ok` | — | Authentication accepted. |
| `auth_request` | `request_id`, `device_name`, `created_at` | A new device wants access; show the approve/reject popup. |
| `new_message` | `message_id`, `contact_id`, `group_id?` | New incoming message (direct or group). |
| `edit_message` | `message_id`, `contact_id`, `group_id?` | A message was edited. |
| `delete_message` | `message_id`, `contact_id`, `group_id?` | A message was deleted. |

The browser reconnects automatically every 3 seconds if the socket drops, and the UI
refreshes chat lists on each event.

---

## Database schema

Created automatically at startup (`sqlite_schema` in `db_manager.cpp`).

```sql
contacts(
  contact_id      TEXT PRIMARY KEY,   -- remote node public key
  name            TEXT NOT NULL,
  server_address  TEXT
);

messages(
  message_id      TEXT PRIMARY KEY,   -- UUID
  sender_id       TEXT NOT NULL,      -- public key
  recipient_id    TEXT NOT NULL,      -- public key
  group_id        TEXT,               -- group UUID; '' / NULL for direct messages
  plaintext       TEXT NOT NULL,      -- decrypted text (local node only)
  ciphertext      TEXT NOT NULL,      -- nonce || sealed message (base64)
  accepted        INTEGER NOT NULL DEFAULT 0,  -- 0 pending, 1 delivered, 2 failed, 3 deleted
  timestamp       INTEGER NOT NULL DEFAULT (unixepoch()),
  edited_at       INTEGER NOT NULL DEFAULT 0,
  edit_accepted   INTEGER NOT NULL DEFAULT 0,   -- per-recipient edit ack
  delete_accepted INTEGER NOT NULL DEFAULT 1,
  deleted_at      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_messages_group ON messages(group_id);

groups(
  group_id   TEXT PRIMARY KEY,        -- UUID
  name       TEXT NOT NULL,
  created_by TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

group_members(
  group_id        TEXT NOT NULL,
  member_id       TEXT NOT NULL,      -- public key
  server_address  TEXT NOT NULL,
  role            TEXT NOT NULL DEFAULT 'member',  -- 'admin' for creators
  added_at        INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (group_id, member_id)
);
CREATE INDEX idx_group_members_group ON group_members(group_id);

clients(
  client_id   TEXT PRIMARY KEY,       -- random b64url id
  device_name TEXT NOT NULL DEFAULT '',
  salt        TEXT NOT NULL,
  secret_hash TEXT NOT NULL,          -- generichash(secret, salt)
  created_at  INTEGER NOT NULL
);

auth_requests(
  request_id        TEXT PRIMARY KEY,
  device_name       TEXT NOT NULL,
  created_at        INTEGER NOT NULL,
  expires_at        INTEGER NOT NULL,
  approved_client_id TEXT,
  approved_secret   TEXT
);

group_updates(                       -- pending membership snapshots
  group_id       TEXT NOT NULL,
  member_id      TEXT NOT NULL,
  snapshot_json  TEXT NOT NULL,      -- serialized member list
  version        INTEGER NOT NULL DEFAULT 0,
  accepted       INTEGER NOT NULL DEFAULT 0,  -- 0 pending, 1 accepted
  created_at     INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (group_id, member_id)
);
CREATE INDEX idx_group_updates_pending ON group_updates(accepted);
```

---

## Project structure

```
.
├── CMakeLists.txt              # build; pulls cpr, Asio, Crow via FetchContent
├── Dockerfile                  # multi-stage Debian bookworm build+runtime
├── docker-compose.yml          # server + ngrok services
├── .env / .env.example         # compose configuration (ngrok token/domain, port)
├── cert.pem / key.pem          # local TLS certs (gitignored)
├── web/
│   ├── index.html              # single-page UI
│   ├── script.js               # all client logic (API + WS)
│   └── style.css
└── src/
    ├── server/
    │   ├── main.cpp            # routes, WS, auth flows, orchestration
    │   ├── auth.{h,cpp}        # b64url secrets, generichash, authenticate()
    │   ├── http_client.{h,cpp} # send_message() (POST) and fetch_remote() (GET)
    │   ├── message_delivery.{h,cpp}  # deliver/edit/delete, group fan-out
    │   ├── retry_worker.{h,cpp}      # 30 s offline-retry thread
    │   ├── crypto/
    │   │   ├── key_manager.{h,cpp}   # load/generate box keypair
    │   │   └── message_crypto.{h,cpp} # crypto_box_easy / open
    │   └── auth.{h,cpp}
    └── db/
        └── db_manager.{h,cpp}  # SQLite wrappers + schema
```

---

## Troubleshooting

**"Failed to add contact" / NetworkError when adding an ngrok node from the browser.**

ngrok shows a *browser-warning* interstitial page to requests that carry a browser
User-Agent (it answers real HTTP clients like `curl` normally). Because the web UI now
fetches public keys **server-side** through `GET /api/remote_public_key` (the server uses
a libcurl User-Agent), this should not happen. If you still see it on an old build:

- Rebuild the node (`cmake --build build -j`) so `fetch_remote()` + the proxy route are
  present, and rebuild the image (`docker compose up -d --build`) for the container.
- Quick manual unlock: open `https://<your-ngrok-domain>` in the browser once and click
  through the warning — ngrok stores an approval cookie for that domain.

**The node's web UI runs but messages never arrive.**

Check the recipient announced the right `server_address` (host + port, and the port is
reachable from outside). Node-to-node requests are HTTPS on port 18080 (or your
`PORT`); opening it in the browser proves it works, but be careful — the address must be
reachable **from the peer's network**, not only from yours.

**"Node is locked" on first login.**

The node already has at least one client. Use a different approach: request access from
the same browser (creates a pending request) and approve it from the existing device, or
start a fresh node with a new database if you intended a clean install.

**Self-signed certificate warnings.**

Expected on first visit; accept it. `web/` is served over the same HTTPS port, so the
browser must trust the cert before the UI loads.

**SQLite "database is locked".**

Only one `server` instance may open one `.db` file at a time. You cannot run two nodes
on the same database.

**After `docker compose` config changes, port/db updates don't apply.**

The image bakes `src/` and `web/`. Always `docker compose up -d --build` after code
changes, and keep `PORT`, `NGROK_DOMAIN`, `NGROK_AUTHTOKEN` correct in `.env`.

---

## Limitations & roadmap

- **No group encryption key** — group messages are per-member encrypted; the sender node
  necessarily knows all member keys.
- **Plaintext is stored** on the local node's SQLite DB (needed for search/history).
  Back up and protect your `.db` files.
- **TLS not pinned** between nodes; certificate validation is disabled for node-to-node
  calls.
- No message sync across devices sharing one node besides the DB; realtime updates rely
  on WebSocket (no push when the node has a different console session closed is fine —
  history is fetched on load).
- History is not broadcast to newly-added group members (they receive the membership
  snapshot, but old messages remain on the servers that had them).
- 24-hour retry window is currently hard-coded.