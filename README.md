# IPFS_OS

IPFS_OS is a small end‑to‑end content‑addressed storage system inspired by IPFS, implemented primarily in C with a thin Python HTTP gateway on top.

At a high level:

- A Python HTTP server (`main.py`) exposes simple `/upload` and `/download` endpoints.
- The HTTP server talks over a Unix domain socket to a C “engine” (`c_engine`) that does all heavy work.
- Files are split into fixed‑size chunks, each chunk is hashed with **BLAKE3‑256**, and stored under `blocks/` by its hash.
- A **manifest** (JSON) describing all chunks of a file is written under `manifests/` and identified by a **CID**.
- The HTTP API returns that CID to the client; downloads reconstruct the file by reading and verifying chunks using that manifest.

The engine is multithreaded (thread pool in C) and uses BLAKE3 both for chunk hashing and for manifest CIDs. CIDs are base32‑encoded multicodec+multihash values (similar in spirit to IPFS, but with project‑specific codes).

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Repository Structure](#repository-structure)
3. [How to Access the Project](#how-to-access-the-project)
    - [Clone from Git](#clone-from-git)
    - [Download as ZIP](#download-as-zip)
4. [Building the C Engine](#building-the-c-engine)
    - [Prerequisites](#prerequisites)
    - [Build with CMake](#build-with-cmake)
    - [Direct `gcc` build](#direct-gcc-build)
5. [Running the System](#running-the-system)
    - [Start the C Engine](#start-the-c-engine)
    - [Start the HTTP Gateway](#start-the-http-gateway)
    - [Directory Layout at Runtime](#directory-layout-at-runtime)
6. [HTTP API](#http-api)
    - [`POST /upload`](#post-upload)
    - [`GET /download`](#get-download)
7. [Content Addressing, CIDs, and Hashing](#content-addressing-cids-and-hashing)
8. [Configuration (Environment Variables)](#configuration-environment-variables)
9. [Concurrency Model](#concurrency-model)
10. [Debugging Large Downloads](#debugging-large-downloads)
11. [BLAKE3 Implementation Notes](#blake3-implementation-notes)
12. [Development Notes & Extensibility](#development-notes--extensibility)
13. [License](#license)

---

## Architecture Overview

The project consists of two major pieces:

1. **C engine (`c_engine`)**
    - Listens on a Unix domain socket (e.g. `/tmp/cengine.sock`).
    - Implements a small **binary frame protocol** with opcodes for upload and download:
        - Upload: `OP_UPLOAD_START`, `OP_UPLOAD_CHUNK`, `OP_UPLOAD_FINISH`, `OP_UPLOAD_DONE`
        - Download: `OP_DOWNLOAD_START`, `OP_DOWNLOAD_CHUNK`, `OP_DOWNLOAD_DONE`
    - Handles:
        - Chunking of incoming data
        - BLAKE3 hashing and block storage under `blocks/`
        - Manifest generation under `manifests/`
        - CID generation + base32 encoding
        - Chunk verification and ordering during download
    - Uses a **thread pool** for parallel chunk I/O and hashing.

2. **Python HTTP gateway (`main.py`)**
    - Exposes simple HTTP routes for clients:
        - `POST /upload` → stores file, returns JSON with `"cid"`.
        - `GET /download?cid=...` → streams raw file bytes back.
    - Translates HTTP requests into engine frames, forwards them over the Unix socket, and streams engine responses back to the HTTP client.

Data flow for **upload**:

1. HTTP client `POST /upload` with file body.
2. Gateway frames stream:
    - `OP_UPLOAD_START` (filename)
    - Many `OP_UPLOAD_CHUNK` frames (file data)
    - `OP_UPLOAD_FINISH`
3. Engine:
    - For each chunk:
        - Compute BLAKE3 hash
        - Store chunk under `blocks/<xx>/<yy>/<hash>.chunk`
        - Maintain per‑chunk reference count `*.ref`
    - Writes manifest JSON containing chunk metadata.
    - Uses BLAKE3‑256(multicodec+multihash) of manifest as CID, stores manifest under `manifests/<CID>.json`.
    - Replies `OP_UPLOAD_DONE` with the CID (base32 string).

Data flow for **download**:

1. HTTP client `GET /download?cid=<CID>`.
2. Gateway sends `OP_DOWNLOAD_START` with CID over socket.
3. Engine:
    - Validates CID (base32, allowed chars A‑Z and 2‑7).
    - Loads manifest `manifests/<CID>.json`.
    - Schedules per‑chunk tasks in a thread pool:
        - Open chunk file, read, hash, verify hash vs manifest.
    - A **sender thread** waits for chunks in index order and sends `OP_DOWNLOAD_CHUNK` frames, then `OP_DOWNLOAD_DONE`.
4. Gateway streams the chunk payloads as HTTP response body.

---

## Repository Structure

```text
IPFS_OS/
├─ CMakeLists.txt          # CMake build config for c_engine
├─ c_engine.c              # Main C engine (socket server, framing, dispatch)
├─ common.c / common.h     # Shared utilities (I/O, hashing, CIDs, thread pool)
├─ upload.c / upload.h     # Upload pipeline: chunking, hashing, manifest write
├─ download.c / download.h # Download pipeline: read+verify chunks, aggregate, send
├─ blake3.c                # BLAKE3 hasher core (chunking, tree, finalize)
├─ blake3.h                # Public BLAKE3 API
├─ blake3_impl.h           # BLAKE3 internals, platform detection, SIMD hooks
├─ blake3_portable.c       # Portable (non‑SIMD) BLAKE3 compression
├─ blake3_dispatch.c       # CPU feature detection and dispatch to SIMD/portable
├─ main.py                 # HTTP gateway (upload/download HTTP API)
└─ (runtime dirs, created on demand)
   ├─ blocks/              # Content‑addressed chunk storage
   └─ manifests/           # Manifest JSONs keyed by CID
```

Languages:

- ~92.8% **C** – engine, hashing, threading, manifest handling
- ~6.1% **Python** – HTTP gateway and debug tool
- ~1.1% **CMake** – build configuration

---

## How to Access the Project

### Clone from Git

To get the full history and contribute, clone the repository:

```bash
git clone https://github.com/sabaebrahimimosavi/IPFS_OS.git
cd IPFS_OS
```

If you prefer SSH:

```bash
git clone git@github.com:sabaebrahimimosavi/IPFS_OS.git
cd IPFS_OS
```

Update your local copy later with:

```bash
git pull
```

### Download as ZIP

If you only want to try the project without Git:

1. Open the repo page in your browser:  
   [IPFS_OS](https://github.com/sabaebrahimimosavi/IPFS_OS)
2. Click the green **Code** button.
3. Select **Download ZIP**.
4. Extract the ZIP and open a terminal inside the extracted `IPFS_OS/` directory.

---

## Building the C Engine

### Prerequisites

You need:

- A C compiler with C11 support (`gcc`, `clang`, or MSVC on Windows with extra work).
- **POSIX** environment for the engine as written (Unix domain sockets, `pthread`, `sys/un.h`):
    - Linux and macOS are natural targets.
    - Windows would require porting (e.g. Winsock + Windows threads).
- **CMake ≥ 3.10**
- Python 3 (for `main.py` HTTP gateway)
- `pthread` (POSIX threads) – used by the engine directly.

Example packages on Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake python3
```

On macOS with Homebrew:

```bash
brew install cmake python
```

### Build with CMake

From the project root:

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

This will produce an executable named:

```text
build/c_engine
```

CMake config notes (see `CMakeLists.txt`):

- Uses `-O2 -pthread -Wall -Wextra`.
- Disables BLAKE3 SIMD paths for portability:

  ```cmake
  add_compile_definitions(
      BLAKE3_NO_SSE2
      BLAKE3_NO_SSE41
      BLAKE3_NO_AVX2
      BLAKE3_NO_AVX512
  )
  ```

If you enable platform‑specific SIMD, you can remove or change these defines, but make sure your CPU actually supports the chosen features.

### Direct `gcc` build

The top of `c_engine.c` documents a simple build line:

```bash
gcc -O2 -pthread \
  -o c_engine \
  c_engine.c upload.c download.c common.c \
  blake3.c blake3_dispatch.c blake3_portable.c
```

You must run this from the project root (where these .c files are).

---

## Running the System

### Start the C Engine

From the project root (or from `build/` depending on how you built):

```bash
# If built via CMake:
cd build
./c_engine /tmp/cengine.sock
```

The engine expects a single argument: **the Unix domain socket path**. Example output:

```text
[ENGINE] Detected 8 CPU cores, creating 16 worker threads
[ENGINE] listening on /tmp/cengine.sock
```

The engine will:

- Create and listen on the socket path you specify.
- Initialize:
    - A thread pool sized based on `sysconf(_SC_NPROCESSORS_ONLN)` (2× cores, clamped between 2 and 64 threads).
    - A global `pthread_rwlock_t` for manifest operations.
    - Global mutex for reference count updates.

You should keep this process running in a terminal.

### Start the HTTP Gateway

In a separate terminal, from the project root:

```bash
export ENGINE_SOCK=/tmp/cengine.sock  # optional; default matches main.py
python3 main.py
```

`main.py` uses a hardcoded `ENGINE_SOCK = "/tmp/cengine.sock"`, so if you change the engine socket path, update `ENGINE_SOCK` in `main.py` or adapt it.

The gateway starts a threaded HTTP server (by default on `127.0.0.1:9000`):

```text
[GATEWAY] Using CHUNK_SIZE=262144 bytes
HTTP gateway listening on http://127.0.0.1:9000
```

### Directory Layout at Runtime

The engine will create and use these directories under the project directory:

- `blocks/` – content‑addressed chunk store:
    - `blocks/aa/bb/<64-hex-digest>.chunk`
    - `blocks/aa/bb/<64-hex-digest>.ref` (reference count text file)
- `manifests/` – per‑file manifests:
    - `manifests/<CID>.json` – final manifest
    - `manifests/<something>.tmp` – temporary manifest used during upload

You do **not** need to pre‑create them; the code calls `ensure_dir()` and creates directories with mode `0777` when needed.

---

## HTTP API

### `POST /upload`

Upload a file; returns a CID.

**Request:**

- Method: `POST`
- URL: `http://127.0.0.1:9000/upload`
- Headers:
    - `X-Filename: <original filename>`
    - `Content-Length: <integer>`
- Body: raw file bytes.

The gateway:

- Validates `X-Filename` and `Content-Length`.
- Streams the body to the engine in frames:
    - `OP_UPLOAD_START` with filename as UTF‑8.
    - `OP_UPLOAD_CHUNK` for each body segment.
    - `OP_UPLOAD_FINISH` when done.

The engine:

- For each chunk:
    - Computes BLAKE3 hash via `chunk_hash_hex()` (`common.c` using `blake3_hasher_*`).
    - Stores it under `blocks/` with hierarchical subdirs based on first 4 hex characters.
    - Maintains reference count (`.ref`) via `block_ref_increment()` for deduplication.
- After all chunks stored:
    - Writes a manifest JSON:

      ```json
      {
        "version": 1,
        "hash_algo": "blake3",
        "chunk_size": "uint32",
        "total_size": "uint64",
        "filename": "<original>",
        "chunks": [
          {
            "index": "uint32",
            "size": "uint32",
            "hash": "<64-hex blake3 digest>"
          }
        ]
      }
      ```

    - Ensures manifest is flushed and `fsync`ed before rename.
    - Computes CID (see [Content Addressing](#content-addressing-cids-and-hashing)).
    - Renames temp manifest to `manifests/<CID>.json`.
    - Returns `OP_UPLOAD_DONE` with CID string.

**Response example:**

```text

HTTP/1.1 200 OK
Content-Type: application/json

{"cid": "BAGCAYDAAAA..."}  // base32 CID
```

If the engine returns an error frame (`OP_ERROR` / 0xFF), the gateway will respond with HTTP 400/502 and the error message.

### `GET /download`

Download a file by its CID.

**Request:**

- Method: `GET`
- URL: `http://127.0.0.1:9000/download?cid=<CID>`
- Query:
    - `cid`: base32 CID returned from upload.

The gateway:

- Sends `OP_DOWNLOAD_START` with the CID to the engine.
- Reads the first frame:
    - If `OP_ERROR` → respond with HTTP 400 and message.
    - If `OP_DOWNLOAD_CHUNK` or `OP_DOWNLOAD_DONE` → start HTTP 200 response and stream chunks as they arrive.

Response headers:

```http
HTTP/1.1 200 OK
Content-Type: application/octet-stream
```

Body is just the raw bytes of the original file, reconstructed in order.

The engine:

- Validates base32 CID characters (A–Z, 2–7).
- Acquires read lock on manifest RW lock and reads manifest `manifests/<CID>.json`.
- Parses manifest with `parse_manifest_chunks()` into array of `chunk` structures.
- Creates a `download_aggregator_t`:
    - Holds connection fd, manifest chunks, a dynamic list of `ready_chunk_t` (verified chunks), and synchronization primitives.
- Spawns one **sender thread** (`download_sender_loop`).
- Dispatches a worker task per chunk to the global thread pool:
    - Opens corresponding `.chunk` file.
    - Reads and hashes it using `chunk_hash_hex()`.
    - Verifies hash equals manifest hash.
    - Pushes verified data into aggregator (`aggregator_add_chunk()`).

The sender thread:

- Waits for chunk `index = 0`, then `1`, etc. (in order).
- Sends `OP_DOWNLOAD_CHUNK` frames for each, then `OP_DOWNLOAD_DONE`.

Error behavior:

- If reading any chunk fails or hash mismatch occurs:
    - `has_error` is set on aggregator, sender aborts and engine sends `OP_ERROR` (`E_HASH_MISMATCH`).

---

## Content Addressing, CIDs, and Hashing

### Chunk Hashes

Chunks are hashed with **BLAKE3‑256**:

- Implementation from `blake3.c`, `blake3_impl.h`, `blake3_portable.c`, `blake3_dispatch.c`.
- Chunk hash helper in `common.c`:

  ```c
  void chunk_hash_hex(const uint8_t *data, size_t len,
                      char out_hex[HASH_HEX_LEN + 1]);
  ```

  Produces 32 bytes of BLAKE3 output, encoded as 64 hex characters into `out_hex`.

### Manifest CIDs

CIDs are constructed as:

1. Compute BLAKE3‑256 of the **manifest file contents** (raw bytes) using `blake3_raw_hash()`.
2. Build a **multihash**:

   ```text
   [ 1 byte: MH_BLAKE3_256 | 1 byte: length (32) | 32 bytes: digest ]
   ```

   `MH_BLAKE3_256` is a project‑defined code (`0x1f`) in `common.h`.

3. Build **CID bytes**:

   ```text
   [ 1 byte: CID_CODEC_MANIFEST | multihash bytes... ]
   ```

   `CID_CODEC_MANIFEST` is project‑defined (`0x71`).

4. Base32‑encode the CID bytes (no padding) using `base32_encode()` in `common.c`.

This gives the final CID string returned to clients and used in filenames under `manifests/`.

---

## Configuration (Environment Variables)

### Chunk Size (`CHUNK_SIZE`)

Controls the size of data chunks during upload:

- Read in **Python gateway** (`main.py` → `get_chunk_size_from_env()`).
- Also read in **C engine** during `handle_upload_start()` for consistency.

Rules:

- Default: `256 * 1024` bytes (256 KiB) if `CHUNK_SIZE` unset or invalid.
- Must be > 0; values > 16 MiB are clamped to 16 MiB in Python and C.
- In C engine, values < 1 KiB are clamped up to 1 KiB.

Example:

```bash
export CHUNK_SIZE=1048576  # 1 MiB
python3 main.py
# and run c_engine as usual
```

### Hash Algorithm (`HASH_ALGO`)

Currently only **`blake3`** is implemented. The engine will reject any other value:

- Checked in `handle_upload_start()`:

  ```c
  const char *env_hash = getenv("HASH_ALGO");
  ...
  else if (strcmp(env_hash, "blake3") == 0) { ... }
  else {
      send_error(cfd, "E_HASH_ALGO",
                 "Unsupported HASH_ALGO (only 'blake3' is supported)");
  }
  ```

If you want to experiment with another hash, you’d extend:

- `common.c` / `common.h` to support new codes and hashing functions.
- `upload.c` and `download.c` to branch on `sess->hash_algo`.

---

## Concurrency Model

The engine combines three layers of concurrency:

1. **Process‑level concurrency**:
    - `c_engine` accepts multiple Unix socket connections.
    - Each connection is handled in its own `pthread` created by `main()` (`connection_thread()`).

2. **Thread pool for chunk work** (`common.c`):
    - `thread_pool_t` with:
        - `thread_pool_init()`, `thread_pool_add_task()`, `thread_pool_destroy()`.
    - Size: `2 × num_cpu_cores`, clamped [2, 64].
    - Used for:
        - Upload chunk tasks: storing and hashing chunk data (`process_upload_chunk_task`).
        - Download chunk tasks: reading and verifying blocks (`process_chunk_task`).

3. **Per‑operation synchronization**:
    - **Upload sessions** (`upload_S`):
        - Mutex and condition variable to track `tasks_in_progress`.
        - `handle_upload_finish()` waits until all tasks finished before writing manifest and CID.
    - **Downloads** (`download_aggregator_t`):
        - Mutex + condition variable for aggregator buffers.
        - Workers add verified chunks; sender waits for next index and sends in order.
        - `has_error` flag short‑circuits on failures.

Shared structures:

- `g_manifest_rwlock`: global RW lock for `manifests/` dir to avoid races:
    - Write lock during manifest creation/rename.
    - Read lock during manifest load.
- `g_refcount_mutex`: global mutex around `.ref` files to keep block reference counts consistent.


---

## BLAKE3 Implementation Notes

The repository includes a full C implementation of BLAKE3:

- Public API:
    - `blake3_version()`
    - `blake3_hasher_init()`
    - `blake3_hasher_init_keyed()`
    - `blake3_hasher_init_derive_key[_raw]()`
    - `blake3_hasher_update()`
    - `blake3_hasher_finalize()` / `_finalize_seek()`
    - `blake3_hasher_reset()`

- Internals:
    - Chunk state, tree hashing, lazy CV merging for parallelism.
    - Portable compression (no SIMD) in `blake3_portable.c`.
    - SIMD dispatch (SSE2/SSE4.1/AVX2/AVX512/NEON) in `blake3_dispatch.c` using runtime CPU feature detection:
        - CPUID/xgetbv on x86.
        - NEON detection via `BLAKE3_USE_NEON` macros.

By default (per `CMakeLists.txt`), SIMD is **disabled** via:

```c
BLAKE3_NO_SSE2
BLAKE3_NO_SSE41
BLAKE3_NO_AVX2
BLAKE3_NO_AVX512
```

so the engine always uses `*_portable` functions. If you want maximum speed and your platform supports it, you can remove these defines and rebuild.

---

## Development Notes & Extensibility

Ideas and directions you can extend:

- **Alternate hash algorithms or multihash codes**:
    - Introduce additional `MH_*` constants in `common.h`.
    - Branch in `chunk_hash_hex`, `make_multihash_*`, and upload/download code.
- **Garbage collection / reference counting**:
    - `.ref` files currently only increment; you could track deletions and implement GC for unreferenced blocks.
- **Authentication / authorization**:
    - `main.py` is currently open; you could wrap in authentication, tokens, or TLS termination.
- **Windows support**:
    - Replace Unix domain sockets and POSIX APIs with platform‑specific equivalents (e.g. `AF_UNIX` on newer Windows or TCP + named pipes).
- **Streaming / range requests**:
    - Expose range downloads using manifest chunk indices.
