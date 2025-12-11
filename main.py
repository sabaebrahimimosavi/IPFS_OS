from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
import socket
import struct
import json
import os  # for getenv

ENGINE_SOCK = "/tmp/cengine.sock"

# opcode that should be the same in c_engine
# gateway uses them to understand what operation should perform
OP_UPLOAD_START = 0x01
OP_UPLOAD_CHUNK = 0x02
OP_UPLOAD_FINISH = 0x03
OP_UPLOAD_DONE  = 0x81

OP_DOWNLOAD_START = 0x11
OP_DOWNLOAD_CHUNK = 0x91
OP_DOWNLOAD_DONE  = 0x92

# env-based chunk size
DEFAULT_CHUNK_SIZE = 256 * 1024  # 256 KB

def get_chunk_size_from_env():
    """Read CHUNK_SIZE from environment (bytes), fallback to DEFAULT_CHUNK_SIZE."""
    val = os.getenv("CHUNK_SIZE")
    if not val:
        return DEFAULT_CHUNK_SIZE
    try:
        n = int(val)
        if n <= 0:
            raise ValueError("CHUNK_SIZE must be > 0")
        # Optional: clamp to a reasonable maximum, e.g. 16 MB
        if n > 16 * 1024 * 1024:
            n = 16 * 1024 * 1024
        return n
    except ValueError:
        print(f"[GATEWAY] WARNING: invalid CHUNK_SIZE='{val}', using default {DEFAULT_CHUNK_SIZE}")
        return DEFAULT_CHUNK_SIZE

CHUNK = get_chunk_size_from_env()
print(f"[GATEWAY] Using CHUNK_SIZE={CHUNK} bytes")
# ----------------------------------

# [ opcode (1 byte) | payload_length (4 byte)| payload ]
# payload : input data , it is different based on what it supposes to do (page 7)
# frame function get payload and turn it to the format that should be sent to c_engine
def frame(op, payload: bytes):
    return struct.pack(">BI", op, len(payload)) + payload

# call the c_engine
# sent all messages
# read the answers if it sees "OP_UPLOAD_DONE" or "OP_DOWNLOAD_DONE"
def engine_call(messages):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(ENGINE_SOCK)
    try:
        for m in messages:
            s.sendall(m)
        def recv_exact(n):
            buf = bytearray()
            while len(buf) < n:
                chunk = s.recv(n - len(buf))
                if not chunk:
                    raise ConnectionError("engine closed")
                buf.extend(chunk)
            return bytes(buf)
        while True:
            header = recv_exact(5)
            op, length = struct.unpack(">BI", header)
            data = recv_exact(length) if length else b""
            yield op, data
            if op in (OP_UPLOAD_DONE, OP_DOWNLOAD_DONE):
                break
    finally:
        s.close()

# handle HTTP requests
# server_version is just for HTTP headers
class Handler(BaseHTTPRequestHandler):
    server_version = "OS-Gateway/0.1"

    # upload
    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/upload":
            self.send_error(404, "Not Found")
            return

        fname = self.headers.get("X-Filename")
        clen = self.headers.get("Content-Length")
        if not fname or not clen:
            self.send_error(400, "Missing X-Filename or Content-Length")
            return

        try:
            total = int(clen)
        except:
            self.send_error(400, "Invalid Content-Length")
            return

        # here it only read the files chunks and do not hash or save
        msgs = [frame(OP_UPLOAD_START, fname.encode("utf-8"))]

        remaining = total
        # CHANGED: use global CHUNK instead of hardcoded 256 * 1024
        global CHUNK
        while remaining > 0:
            n = min(remaining, CHUNK)
            data = self.rfile.read(n)
            if not data or len(data) != n:
                self.send_error(400, "Body shorter than Content-Length")
                return
            msgs.append(frame(OP_UPLOAD_CHUNK, data))
            remaining -= n

        msgs.append(frame(OP_UPLOAD_FINISH, b""))

        # START + CHUNK + FINISH : sends all messages to c_engine
        # if "OP_UPLOAD_DONE" the output = cid
        cid = None
        for op, payload in engine_call(msgs):
            if op == OP_UPLOAD_DONE:
                cid = payload.decode("utf-8")

        if cid is None:
            self.send_error(502, "Engine did not return CID")
            return

        body = json.dumps({"cid": cid}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # download
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path != "/download":
            self.send_error(404, "Not Found")
            return
        q = parse_qs(parsed.query)
        cid = q.get("cid", [None])[0]
        if not cid:
            self.send_error(400, "Missing cid parameter")
            return

        msgs = [frame(OP_DOWNLOAD_START, cid.encode("utf-8"))]

        # Check first message for errors before sending 200 !!!!!!
        try:
            gen = engine_call(msgs)
            first_op, first_payload = next(gen)

            if first_op == 0xFF:  # OP_ERROR
                error_msg = first_payload.decode("utf-8", errors="replace")
                self.send_error(400, f"Engine error:  {error_msg}")
                return

            # No error, send 200
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.end_headers()

            # Send first chunk
            if first_op == 0x91:  # OP_DOWNLOAD_CHUNK
                self.wfile.write(first_payload)
                self.wfile.flush()

            # Send remaining chunks
            for op, payload in gen:
                if op == 0x91:  # OP_DOWNLOAD_CHUNK
                    self.wfile. write(payload)
                    self.wfile.flush()
                elif op == 0x92:  # OP_DOWNLOAD_DONE
                    break

        except StopIteration:
            # Empty file
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", "0")
            self.end_headers()
        except Exception as e:
            print(f"[GATEWAY] Download error: {e}")
            try:
                self.send_error(502, "Download failed")
            except:
                pass
        #
        # for op, payload in engine_call(msgs):
        #     if op == OP_DOWNLOAD_CHUNK and payload:
        #         self.wfile.write(payload)
        #         self.wfile.flush()
        #     # OP_DOWNLOAD_DONE ends the stream

def run(host='127.0.0.1', port=9000):
    srv = ThreadingHTTPServer((host, port), Handler)
    print(f"HTTP gateway listening on http://{host}:{port}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()

if __name__ == "__main__":
    run()