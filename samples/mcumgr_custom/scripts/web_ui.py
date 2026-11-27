#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Web UI for the mcumgr_custom sample: board LEDs, MCP23S08 LEDs, BarGraph,
Air Quality, firmware update and the Zephyr log.

  browser ◀─WebSocket─▶ web_ui.py ──SMP──▶ /dev/ttyUSB0 (shield FT232RL, USART2)
                            └──── log ◀──── /dev/ttyACM* (Discovery micro-USB, CDC ACM)

The page keeps one WebSocket (/ws): the server pushes the board state when
it changes (one poller for all pages, only while a page is open), the log
lines, the firmware update progress and the serial ports; the page sends its
commands on it. The same commands exist as HTTP (/api/...) for scripts.

Only needs pyserial (in the Zephyr venv), uses smp_client.py of this
directory for SMP.

  python web_ui.py                       # http://127.0.0.1:8000
  python web_ui.py -p /dev/ttyUSB0 --log-port /dev/ttyACM2 --http 0.0.0.0:8000

The log port can also be changed from the UI (Zephyr log panel).

The SMP port is held while the UI runs: press "Release SMP port" in the UI
(or stop the server) before running SWUpdate on the same port.
"""

import argparse
import base64
import collections
import hashlib
import json
import os
import queue
import re
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import serial
import serial.tools.list_ports

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import smp_client as smp  # noqa: E402

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
LOG_USB_VID = 0x2FE3  # CONFIG_CDC_ACM_SERIAL_VID (Zephyr default)
LOG_USB_PRODUCT = "mcumgr_custom log"  # CONFIG_CDC_ACM_SERIAL_PRODUCT_STRING
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
LEVEL = re.compile(r"<(err|wrn|inf|dbg)>")


def jsonable(obj):
    """CBOR answers to JSON: bytes (image hashes) as hex."""
    if isinstance(obj, (bytes, bytearray)):
        return obj.hex()
    if isinstance(obj, dict):
        return {k: jsonable(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [jsonable(v) for v in obj]
    return obj


# --------------------------------------------------------------------------
# SMP link (shield FT232RL)

class SmpLink:
    def __init__(self, port, baud):
        self.port = port
        self.baud = baud
        self.lock = threading.Lock()
        self.smp = None
        self.released = False
        self.error = None

    def _open(self):
        if self.smp is None and not self.released:
            try:
                self.smp = smp.SmpSerial(self.port, self.baud, timeout=1.5)
                self.error = None
            except serial.SerialException as e:
                self.error = f"cannot open {self.port}: {e}"
        return self.smp

    def request(self, op, group, cmd, payload=None, timeout=None, retries=1):
        with self.lock:
            link = self._open()
            if link is None:
                raise smp.SmpError(self.error or "SMP port released")
            try:
                rsp = link.request(op, group, cmd, payload, timeout=timeout,
                                   retries=retries)
                self.error = None
                return rsp
            except serial.SerialException as e:
                link.close()
                self.smp = None
                self.error = f"{self.port}: {e}"
                raise smp.SmpError(self.error)
            except smp.SmpError as e:
                if "timeout" in str(e):
                    self.error = "no answer from the MCU"
                raise

    def release(self):
        with self.lock:
            self.released = True
            if self.smp:
                self.smp.close()
                self.smp = None
            self.error = None

    def connect(self):
        with self.lock:
            self.released = False
            self._open()

    def status(self):
        return {"port": self.port, "released": self.released,
                "open": self.smp is not None, "error": self.error}


# --------------------------------------------------------------------------
# Firmware update over SMP: upload, test, reset, verify, confirm

IMAGE_MAGIC = 0x96F3B83D  # MCUboot image header


def parse_image(data):
    """MCUboot header of a signed image (zephyr.signed.bin)."""
    if len(data) < 32:
        raise ValueError("file too small for an MCUboot image")
    magic, _load, hdr_size, _ptlv, img_size, _flags = struct.unpack_from("<IIHHII", data, 0)
    if magic != IMAGE_MAGIC:
        raise ValueError("not an MCUboot signed image (use zephyr.signed.bin)")
    major, minor, rev, build = struct.unpack_from("<BBHI", data, 20)
    return {"version": f"{major}.{minor}.{rev}+{build}", "size": len(data),
            "header_size": hdr_size, "image_size": img_size}


class FirmwareJob(threading.Thread):
    """One update at a time; on_change(status) at each step and percent."""

    CHUNK = 512  # data bytes per upload request (fits the 1536 MTU)

    def __init__(self, link, data, name, confirm, on_change=None):
        super().__init__(daemon=True)
        self.link, self.data, self.name, self.confirm = link, data, name, confirm
        self.on_change = on_change or (lambda status: None)
        self.header = parse_image(data)
        self.lock = threading.Lock()
        self.st = {"active": True, "step": "upload", "progress": 0, "ok": None,
                   "file": name, "version": self.header["version"],
                   "size": len(data), "steps": []}

    def status(self):
        with self.lock:
            return dict(self.st, steps=list(self.st["steps"]))

    def _set(self, **kw):
        with self.lock:
            if all(self.st.get(k) == v for k, v in kw.items()):
                return
            self.st.update(kw)
        self.on_change(self.status())

    def _step(self, text):
        with self.lock:
            self.st["steps"].append({"t": time.time(), "text": text})
        Handler.app.log.system(f"[web ui] update: {text}")
        self.on_change(self.status())

    def _req(self, *args, **kw):
        return self.link.request(*args, **kw)

    def run(self):
        try:
            self._run()
            self._set(ok=True, step="done", active=False)
        except (smp.SmpError, ValueError, KeyError) as e:
            self._step(f"FAILED: {e}")
            self._set(ok=False, step="failed", active=False, error=str(e))

    def _images(self):
        return self._req(smp.OP_READ, smp.GRP_IMG, smp.IMG_STATE)["images"]

    def _run(self):
        data, t0 = self.data, time.time()
        self._step(f"{self.name}: {len(data)} bytes, version {self.header['version']}")

        before = {i.get("slot"): i for i in self._images()}
        running = before.get(0, {})
        self._step(f"running: {running.get('version', '?')}")
        # the signed image embeds its SHA-256 (TLV), the hash MCUmgr reports
        if running.get("hash") and data.find(running["hash"]) >= 0:
            raise ValueError("this image is already running")

        self._set(step="upload")
        off = 0
        while off < len(data):
            req = {"image": 0, "off": off, "data": data[off:off + self.CHUNK]}
            if off == 0:
                req.update(len=len(data), sha=hashlib.sha256(data).digest())
                self._step("erasing slot 1 (first chunk, a few seconds)")
            rsp = self._req(smp.OP_WRITE, smp.GRP_IMG, smp.IMG_UPLOAD, req,
                            timeout=60 if off == 0 else 3)
            off = rsp["off"]
            self._set(progress=round(100 * off / len(data)))
        self._step(f"uploaded in {time.time() - t0:.1f} s")

        slot1 = [i for i in self._images() if i.get("slot") == 1]
        if not slot1:
            raise ValueError("no image in slot 1 after the upload")
        new_hash = slot1[0]["hash"]
        self._step(f"slot 1: {slot1[0].get('version')} hash {new_hash.hex()[:16]}…")

        self._set(step="test")
        self._req(smp.OP_WRITE, smp.GRP_IMG, smp.IMG_STATE,
                  {"hash": new_hash, "confirm": False})
        self._step("slot 1 marked for test")

        self._set(step="reset")
        self._req(smp.OP_WRITE, smp.GRP_OS, smp.OS_RESET)
        self._step("reset: MCUboot swaps the slots (about 20 s)")
        time.sleep(1.0)
        t_reset = time.time()
        while True:
            try:
                self._req(smp.OP_WRITE, smp.GRP_OS, smp.OS_ECHO, {"d": "up"},
                          timeout=1.0, retries=0)
                break
            except smp.SmpError:
                if time.time() - t_reset > 120:
                    raise smp.SmpError("no answer 120 s after the reset")
        self._step(f"MCU answers after {time.time() - t_reset + 1:.0f} s")

        self._set(step="verify")
        slot0 = [i for i in self._images() if i.get("slot") == 0][0]
        if slot0.get("hash") != new_hash:
            raise ValueError(f"MCUboot did not boot the new image (running "
                             f"{slot0.get('version')}): signature or swap failed")
        self._step(f"running {slot0.get('version')}")

        self._set(step="confirm")
        if self.confirm == "host":
            self._req(smp.OP_WRITE, smp.GRP_IMG, smp.IMG_STATE,
                      {"hash": new_hash, "confirm": True})
            self._step("confirmed by the web UI")
        else:
            t_wait = time.time()
            while not slot0.get("confirmed"):
                if time.time() - t_wait > 15:
                    raise ValueError("the image did not confirm itself (self-test "
                                     "failed?): MCUboot reverts it at the next reset")
                time.sleep(1)
                slot0 = [i for i in self._images() if i.get("slot") == 0][0]
            self._step("confirmed by the image (self-test ok)")
        self._step(f"update done in {time.time() - t0:.0f} s")


# --------------------------------------------------------------------------
# Zephyr log (USB CDC ACM), reconnects after each MCU reset

def list_ports(smp_port=None):
    """Serial ports for the log port picker (USB / ACM only, no legacy ttyS)."""
    out = []
    for p in sorted(serial.tools.list_ports.comports(), key=lambda p: p.device):
        if p.vid is None and not p.device.startswith(("/dev/ttyACM", "/dev/ttyUSB")):
            continue
        out.append({"device": p.device,
                    "description": p.product or p.description or "",
                    "manufacturer": p.manufacturer or "",
                    "usb": f"{p.vid:04x}:{p.pid:04x}" if p.vid is not None else "",
                    "zephyr_log": p.vid == LOG_USB_VID or (p.product or "") == LOG_USB_PRODUCT,
                    "smp": p.device == smp_port})
    return out


class LogReader(threading.Thread):
    """Reads the Zephyr log port. The port is chosen from the UI (set_port):
    "auto" finds the USB CDC ACM of the firmware, or a fixed device."""

    def __init__(self, port=None, baud=115200, keep=3000):
        super().__init__(daemon=True)
        self.fixed_port = port
        self.baud = baud
        self.gen = 0          # bumped by set_port: the reader reopens
        self.port = None
        self.connected = False
        self.lines = collections.deque(maxlen=keep)
        self.seq = 0
        self.cond = threading.Condition()

    def _find_port(self):
        if self.fixed_port:
            return self.fixed_port
        for p in serial.tools.list_ports.comports():
            if p.vid == LOG_USB_VID or (p.product or "") == LOG_USB_PRODUCT:
                return p.device
        return None

    def _push(self, text, level=None, system=False):
        if level is None:
            m = LEVEL.search(text)
            level = m.group(1) if m else "out"
        with self.cond:
            self.seq += 1
            self.lines.append({"seq": self.seq, "t": time.time(), "level": level,
                               "text": text, "system": system})
            self.cond.notify_all()

    def system(self, text):
        self._push(text, "sys", True)

    def set_port(self, port, baud=None):
        """port: device path, or None / "auto" for the firmware CDC ACM."""
        self.fixed_port = None if port in (None, "", "auto") else port
        if baud:
            self.baud = int(baud)
        self.gen += 1
        self.system(f"--- log port set to {self.fixed_port or 'auto (Zephyr USB CDC ACM)'}"
                    f"{f' @ {self.baud}' if self.fixed_port else ''} ---")

    def settings(self):
        return {"mode": "fixed" if self.fixed_port else "auto",
                "fixed_port": self.fixed_port, "baud": self.baud,
                "port": self.port, "connected": self.connected}

    def _set_connected(self, connected, port):
        if connected != self.connected:
            self.connected, self.port = connected, port
            self._push(f"--- log port {port} {'connected' if connected else 'lost'} ---",
                       "sys", True)

    def run(self):
        buf = bytearray()
        self._push(f"--- waiting for the log port "
                   f"({self.fixed_port or 'USB CDC ACM, Discovery micro-USB'}) ---",
                   "sys", True)
        while True:
            gen = self.gen
            port = self._find_port()
            if not port:
                self._set_connected(False, self.port or "(USB CDC ACM)")
                time.sleep(1)
                continue
            try:
                # baud is ignored by USB CDC ACM, used by a real UART
                with serial.Serial(port, self.baud, timeout=0.2) as ser:
                    ser.dtr = True  # Zephyr CDC ACM: host present
                    self._set_connected(True, port)
                    while gen == self.gen:
                        data = ser.read(512)
                        if not data:
                            continue
                        buf += data
                        while b"\n" in buf:
                            raw, _, rest = bytes(buf).partition(b"\n")
                            buf = bytearray(rest)
                            text = ANSI.sub("", raw.decode(errors="replace")).rstrip("\r")
                            if text.strip():
                                self._push(text)
                buf.clear()
                self._set_connected(False, port)   # port changed from the UI
            except (serial.SerialException, OSError):
                buf.clear()
                self._set_connected(False, port)
                time.sleep(1)

    def since(self, seq):
        with self.cond:
            return [ln for ln in self.lines if ln["seq"] > seq]

    def backlog_then(self, seq, fn):
        """fn(lines after seq), with no line pushed in between: a WebSocket
        client gets its backlog, then the live lines, in order."""
        with self.cond:
            fn([ln for ln in self.lines if ln["seq"] > seq])

    def wait(self, seq, timeout):
        with self.cond:
            self.cond.wait_for(lambda: self.seq > seq, timeout)
            return [ln for ln in self.lines if ln["seq"] > seq]


# --------------------------------------------------------------------------
# WebSocket (RFC 6455, the part a browser page needs: text frames, ping,
# close; no extensions)

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_MAX_FRAME = 1 << 20


def ws_accept_key(key):
    return base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()


def ws_frame(opcode, payload):
    """Server to client: final frame, not masked."""
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", 0x80 | opcode, n)
    elif n < 1 << 16:
        head = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    return head + payload


def ws_read_frame(rfile):
    """(fin, opcode, payload) of one client frame; EOFError when closed."""
    def exact(n):
        b = rfile.read(n)
        if len(b) < n:
            raise EOFError
        return b

    b0, b1 = exact(2)
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack("!H", exact(2))[0]
    elif n == 127:
        n = struct.unpack("!Q", exact(8))[0]
    if n > WS_MAX_FRAME:
        raise EOFError
    mask = exact(4) if b1 & 0x80 else None
    data = exact(n)
    if mask and n:
        m = (mask * (n // 4 + 1))[:n]
        data = (int.from_bytes(data, "big") ^ int.from_bytes(m, "big")).to_bytes(n, "big")
    return bool(b0 & 0x80), b0 & 0x0F, data


class WsClient:
    """One browser page. Sends go through a queue and a writer thread: a
    slow page is dropped (queue full) instead of stalling the others."""

    def __init__(self, sock):
        self.sock = sock
        self.q = queue.Queue(maxsize=2000)
        self.alive = True
        threading.Thread(target=self._writer, daemon=True).start()

    def send_raw(self, frame):
        if not self.alive:
            return
        try:
            self.q.put_nowait(frame)
        except queue.Full:
            self.close()

    def send(self, obj):
        self.send_raw(ws_frame(0x1, json.dumps(jsonable(obj)).encode()))

    def _writer(self):
        while True:
            frame = self.q.get()
            if frame is None:
                return
            try:
                self.sock.sendall(frame)
            except OSError:
                self.close()
                return

    def close(self):
        if not self.alive:
            return
        self.alive = False
        try:
            self.q.put_nowait(None)
        except queue.Full:
            pass
        try:
            self.sock.shutdown(socket.SHUT_RDWR)   # unblocks the reader
        except OSError:
            pass


class Hub:
    def __init__(self):
        self.clients = set()
        self.lock = threading.Lock()

    def add(self, c):
        with self.lock:
            self.clients.add(c)

    def remove(self, c):
        with self.lock:
            self.clients.discard(c)

    def count(self):
        with self.lock:
            return len(self.clients)

    def broadcast(self, obj):
        frame = ws_frame(0x1, json.dumps(jsonable(obj)).encode())
        with self.lock:
            clients = list(self.clients)
        for c in clients:
            c.send_raw(frame)


# --------------------------------------------------------------------------
# Board state, shared by the WebSocket pages and the HTTP API

def smp_call(link, op, group, cmd, payload=None):
    try:
        return {"ok": True, "rsp": link.request(op, group, cmd, payload)}
    except smp.SmpError as e:
        return {"ok": False, "error": str(e)}


class App:
    def __init__(self, link, log, poll_s):
        self.link, self.log = link, log
        self.job = None
        self.hub = Hub()
        self.server_id = os.urandom(4).hex()   # the page resyncs the log after a restart
        self.poll_s = poll_s
        self.state = None
        self.info = None
        self.ports = None
        self.kick_ev = threading.Event()
        self.want_info = True
        self.absent = set()     # groups this firmware does not have

    # ---- reads

    def read_state(self):
        link, log = self.link, self.log
        out = {"smp": None, "log": {"connected": log.connected, "port": log.port}}
        if self.job and self.job.is_alive():
            # firmware update running: keep the SMP link for it
            out.update(smp=link.status(), busy="firmware update",
                       leds=None, leds_error=None)
            return out
        leds = smp_call(link, smp.OP_READ, smp.GRP_LED, smp.LED_STATE)
        out["leds"] = leds["rsp"]["leds"] if leds["ok"] else None
        out["leds_error"] = None if leds["ok"] else leds["error"]
        if leds["ok"]:
            # groups not in this firmware answer ENOTSUP: the page hides them
            out["absent"] = []
            for key, grp, cmd in (("bargraph", smp.GRP_BARGRAPH, smp.BARGRAPH_STATE),
                                  ("airq", smp.GRP_AIRQ, smp.AIRQ_MEASURE),
                                  ("alcohol", smp.GRP_ALCOHOL, smp.ALCOHOL_MEASURE),
                                  ("store", smp.GRP_EEPROM, smp.EEPROM_STORE),
                                  ("patterns", smp.GRP_PATTERN, smp.PATTERN_STATE)):
                if key in self.absent:
                    out["absent"].append(key)
                    continue
                r = smp_call(link, smp.OP_READ, grp, cmd)
                if "ENOTSUP" in (r.get("error") or ""):
                    self.absent.add(key)
                    out["absent"].append(key)
                    continue
                out[key] = r.get("rsp")
                out[key + "_error"] = r.get("error")
        out["smp"] = link.status()
        return out

    def read_info(self):
        out = {}
        for key, args in (("version", (smp.GRP_LED, smp.LED_VERSION, None)),
                          ("images", (smp.GRP_IMG, smp.IMG_STATE, None)),
                          ("os", (smp.GRP_OS, smp.OS_INFO, {"format": "rvbi"}))):
            r = smp_call(self.link, smp.OP_READ, *args)
            out[key] = r.get("rsp")
            if not r["ok"]:
                out[key + "_error"] = r["error"]
                if "timeout" in r["error"] or "port" in r["error"]:
                    break
        return out

    def read_ports(self):
        return {"ports": list_ports(self.link.port), "log": self.log.settings()}

    def job_status(self):
        return self.job.status() if self.job else {"active": False}

    # ---- live updates

    def kick(self, info=False):
        """Read the board now (after a command) instead of at the next poll."""
        if info:
            self.want_info = True
        self.kick_ev.set()

    def poller(self):
        was_up, ports_t = False, 0.0
        while True:
            self.kick_ev.wait(self.poll_s)
            self.kick_ev.clear()
            if not self.hub.count():
                was_up = False          # next page: fresh info
                continue
            st = self.read_state()
            up = bool(st.get("leds"))
            if up and (self.want_info or not was_up):
                self.want_info = False
                self.absent.clear()     # new image or reset: ask the groups again
                self.info = self.read_info()
                self.hub.broadcast({"type": "info", "info": self.info})
            was_up = up
            if st != self.state:
                self.state = st
                self.hub.broadcast({"type": "state", "state": st})
            if time.time() - ports_t > 2:   # e.g. the log ttyACM appears
                ports_t = time.time()
                self.push_ports()

    def push_ports(self, force=False):
        ports = self.read_ports()
        if force or ports != self.ports:
            self.ports = ports
            self.hub.broadcast({"type": "ports", **ports})
        return ports

    def log_pump(self):
        seq = self.log.seq
        while True:
            lines = self.log.wait(seq, 15)
            if lines:
                seq = lines[-1]["seq"]
                self.hub.broadcast(self.log_msg(lines))

    def log_msg(self, lines):
        return {"type": "log", "lines": lines, "connected": self.log.connected,
                "port": self.log.port}

    def on_job(self, status):
        self.hub.broadcast({"type": "job", "job": status})
        if not status.get("active"):
            self.kick(info=True)        # new version and slots

    def start(self):
        for fn in (self.poller, self.log_pump):
            threading.Thread(target=fn, daemon=True).start()

    # ---- commands (WebSocket {"type": name, ...} and POST /api/<name>)

    def command(self, name, req):
        """-> (reply dict, HTTP status)"""
        def write(grp, cmd, keys):
            r = smp_call(self.link, smp.OP_WRITE, grp, cmd,
                         {k: req[k] for k in keys if k in req})
            self.kick()
            return r, 200

        if name == "led":
            return write(smp.GRP_LED, smp.LED_STATE, ("led", "mode", "period_ms"))
        if name == "bargraph":
            return write(smp.GRP_BARGRAPH, smp.BARGRAPH_STATE, ("level", "mask", "brightness"))
        if name == "pattern":
            return write(smp.GRP_PATTERN, smp.PATTERN_STATE,
                         ("bank", "pattern", "step_ms", "width"))
        if name == "alcohol":
            return write(smp.GRP_ALCOHOL, smp.ALCOHOL_MEASURE, ("calibrate", "baseline_mv"))
        if name.startswith("eeprom_"):
            return self.eeprom(name[len("eeprom_"):], req)
        if name == "reset":
            r = smp_call(self.link, smp.OP_WRITE, smp.GRP_OS, smp.OS_RESET)
            self.kick(info=True)
            return r, 200
        if name == "smp":
            if req.get("connect"):
                self.link.connect()
            else:
                self.link.release()
            self.kick(info=True)
            return {"ok": True, "smp": self.link.status()}, 200
        if name == "logport":
            port = req.get("port") or "auto"
            if port != "auto" and port == self.link.port and not self.link.released:
                return {"ok": False, "error": f"{port} is the SMP port"}, 409
            self.log.set_port(port, req.get("baud"))
            self.push_ports(force=True)
            return {"ok": True, "log": self.log.settings()}, 200
        if name == "airq_diag":
            return smp_call(self.link, smp.OP_READ, smp.GRP_AIRQ, smp.AIRQ_DIAG), 200
        if name == "ports":
            return {"ok": True, **self.push_ports(force=True)}, 200
        if name == "info":
            self.info = self.read_info()
            return {"ok": True, "info": self.info}, 200
        if name == "state":
            return {"ok": True, "state": self.read_state()}, 200
        return {"ok": False, "error": f"unknown command {name!r}"}, 404

    def eeprom(self, what, req):
        """EEPROM click: info / read / write / erase / store (group 69)."""
        try:
            if what == "info":
                return {"ok": True, "info": self.link.request(smp.OP_READ, smp.GRP_EEPROM,
                                                              smp.EEPROM_INFO)}, 200
            if what == "read":
                off, length = int(req.get("off", 0)), int(req.get("len", 1024))
                data = smp.eeprom_read(self.link, off, length)
                return {"ok": True, "off": off, "hex": data.hex()}, 200
            if what == "write":
                data = bytes.fromhex(req.get("hex", ""))
                if not data:
                    return {"ok": False, "error": "no data"}, 400
                smp.eeprom_write(self.link, int(req.get("off", 0)), data)
                return {"ok": True, "len": len(data)}, 200
            if what == "erase":
                r = self.link.request(smp.OP_WRITE, smp.GRP_EEPROM, smp.EEPROM_ERASE,
                                      {k: int(req[k]) for k in ("off", "len") if k in req},
                                      timeout=30)
                return {"ok": True, **r}, 200
            if what == "store":
                body = {k: req[k] for k in ("action", "autosave") if k in req}
                r = (self.link.request(smp.OP_WRITE, smp.GRP_EEPROM, smp.EEPROM_STORE, body)
                     if body else self.link.request(smp.OP_READ, smp.GRP_EEPROM, smp.EEPROM_STORE))
                self.kick()
                return {"ok": True, "store": r}, 200
        except (smp.SmpError, ValueError) as e:
            return {"ok": False, "error": str(e)}, 200
        return {"ok": False, "error": f"unknown command eeprom_{what}"}, 404

    def start_job(self, data, name, confirm):
        if self.job and self.job.is_alive():
            return {"ok": False, "error": "an update is already running"}, 409
        if self.link.released:
            return {"ok": False, "error": "SMP port released"}, 409
        try:
            job = FirmwareJob(self.link, data, name, confirm, self.on_job)
        except ValueError as e:
            return {"ok": False, "error": str(e)}, 400
        self.job = job
        job.start()
        self.on_job(job.status())
        self.kick()                     # pages: cards show "update running"
        return {"ok": True, "job": job.status()}, 200


# --------------------------------------------------------------------------
# HTTP

class Handler(BaseHTTPRequestHandler):
    app: App = None
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    def _json(self, obj, status=200):
        body = json.dumps(jsonable(obj)).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _file(self, name, ctype):
        path = os.path.join(WEB_DIR, name)
        try:
            body = open(path, "rb").read()
        except OSError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    # ---- GET

    def do_GET(self):
        app = self.app
        url = urlparse(self.path)
        q = parse_qs(url.query)
        if url.path in ("/", "/index.html"):
            return self._file("index.html", "text/html; charset=utf-8")
        if url.path == "/ws":
            return self.websocket(q)
        if url.path == "/api/state":
            return self._json(app.read_state())
        if url.path == "/api/info":
            return self._json(app.read_info())
        if url.path == "/api/firmware":
            return self._json(app.job_status())
        if url.path == "/api/ports":
            return self._json(app.read_ports())
        if url.path == "/api/airq/diag":
            return self._json(app.command("airq_diag", {})[0])
        if url.path == "/api/log":
            return self.log_stream(int(q.get("since", ["0"])[0]))
        self.send_error(404)

    def websocket(self, q):
        app = self.app
        key = self.headers.get("Sec-WebSocket-Key")
        if "websocket" not in (self.headers.get("Upgrade") or "").lower() or not key:
            self.send_error(400, "WebSocket upgrade expected")
            return
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", ws_accept_key(key.strip()))
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True

        client = WsClient(self.connection)
        # the page passes the log position it has, valid for this server only
        since = int(q.get("since", ["0"])[0]) if q.get("server", [""])[0] == app.server_id else 0
        client.send({"type": "hello", "server_id": app.server_id, "poll_s": app.poll_s})
        if app.state is not None:
            client.send({"type": "state", "state": app.state})
        if app.info is not None:
            client.send({"type": "info", "info": app.info})
        client.send({"type": "job", "job": app.job_status()})
        client.send({"type": "ports", **(app.ports or app.read_ports())})

        def join(lines):
            client.send(app.log_msg(lines))
            app.hub.add(client)
        app.log.backlog_then(since, join)
        app.kick(info=app.info is None)

        parts = []
        try:
            while client.alive:
                fin, op, data = ws_read_frame(self.rfile)
                if op == 0x8:                       # close
                    client.send_raw(ws_frame(0x8, data[:2]))
                    break
                if op == 0x9:                       # ping
                    client.send_raw(ws_frame(0xA, data))
                    continue
                if op in (0x1, 0x2, 0x0):
                    parts.append(data)
                    if not fin:
                        continue
                    msg, parts = b"".join(parts), []
                    self.ws_message(client, msg)
        except (EOFError, OSError):
            pass
        finally:
            app.hub.remove(client)
            client.close()

    def ws_message(self, client, raw):
        try:
            m = json.loads(raw)
            name = m.pop("type")
        except (ValueError, KeyError, AttributeError):
            client.send({"type": "reply", "id": None, "ok": False, "error": "bad message"})
            return
        mid = m.pop("id", None)
        # commands run in their own thread: a slow SMP answer does not hold
        # the reading of the next frames (ping, close)
        def run():
            rsp, _status = self.app.command(name, m)
            client.send({"type": "reply", "id": mid, **rsp})
        threading.Thread(target=run, daemon=True).start()

    def log_stream(self, since):
        log = self.app.log
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        seq = since
        try:
            backlog = log.since(seq)
            while True:
                lines = backlog or log.wait(seq, 15)
                backlog = None
                if lines:
                    seq = lines[-1]["seq"]
                    msg = json.dumps({"lines": lines, "connected": log.connected,
                                      "port": log.port})
                    self.wfile.write(f"id: {seq}\ndata: {msg}\n\n".encode())
                else:
                    self.wfile.write(b": ping\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    # ---- POST

    def do_POST(self):
        url = urlparse(self.path)
        if url.path == "/api/firmware":
            return self.firmware_upload(url)
        try:
            req = self._body()
        except ValueError:
            return self._json({"ok": False, "error": "bad JSON"}, 400)
        if not url.path.startswith("/api/"):
            return self.send_error(404)
        rsp, status = self.app.command(url.path[len("/api/"):], req)
        return self._json(rsp, status)

    def firmware_upload(self, url):
        q = parse_qs(url.query)
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > 1024 * 1024:
            return self._json({"ok": False, "error": "send the image as the request body"}, 400)
        data = self.rfile.read(n)
        rsp, status = self.app.start_job(data, q.get("name", ["image.bin"])[0],
                                         q.get("confirm", ["mcu"])[0])
        return self._json(rsp, status)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-p", "--port", default="/dev/ttyUSB0", help="SMP serial port")
    p.add_argument("-b", "--baud", type=int, default=115200)
    p.add_argument("--log-port", help="Zephyr log port (default: find the "
                   f"USB CDC ACM {LOG_USB_VID:04x}:* / '{LOG_USB_PRODUCT}'); "
                   "can be changed from the UI")
    p.add_argument("--log-baud", type=int, default=115200,
                   help="log port baud rate, for a UART (ignored by USB CDC ACM)")
    p.add_argument("--http", default="127.0.0.1:8000", help="listen address")
    p.add_argument("--poll", type=float, default=1.0,
                   help="board state poll period in s while a page is open "
                   "(commands are shown at once)")
    a = p.parse_args()

    host, _, port = a.http.rpartition(":")
    log = LogReader(a.log_port, a.log_baud)
    log.start()
    Handler.app = App(SmpLink(a.port, a.baud), log, max(0.2, a.poll))
    Handler.app.start()

    srv = ThreadingHTTPServer((host or "127.0.0.1", int(port)), Handler)
    srv.daemon_threads = True
    print(f"mcumgr_custom web UI: http://{host or '127.0.0.1'}:{port}  "
          f"(SMP {a.port}, log {a.log_port or 'auto'})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
