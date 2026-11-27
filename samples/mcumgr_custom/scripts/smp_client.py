#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Minimal MCUmgr SMP client over a serial port, for the mcumgr_custom sample.

Only needs pyserial (in the Zephyr venv). Implements the SMP serial
framing, a small CBOR codec, and the commands of the sample:

  led       get | on N | off N | blink N [-t MS]  custom group 64
  version                                         custom group 64
  info   [-f FORMAT] | bootloader                 OS group
  echo   TEXT                                     OS group
  reset                                           OS group
  image  list | upload FILE [--test] [--reset] | test HASH | confirm [HASH]

Examples:
  smp_client.py -p /dev/ttyUSB0 led get
  smp_client.py -p /dev/ttyUSB0 led blink 2 -t 100
  smp_client.py -p /dev/ttyUSB0 image upload zephyr.signed.bin --test --reset
"""

import argparse
import base64
import hashlib
import struct
import sys
import time

import serial

# --------------------------------------------------------------------------
# CBOR (RFC 8949) subset: int, bytes, str, list, dict, bool, None.
# Decodes definite and indefinite length lists / maps (zcbor uses both).

def cbor_encode(obj):
    def head(major, n):
        if n < 24:
            return bytes([major << 5 | n])
        for ai, fmt in ((24, ">B"), (25, ">H"), (26, ">I"), (27, ">Q")):
            if n < (1 << (8 * struct.calcsize(fmt))):
                return bytes([major << 5 | ai]) + struct.pack(fmt, n)
        raise ValueError("integer too large")

    if obj is False:
        return b"\xf4"
    if obj is True:
        return b"\xf5"
    if obj is None:
        return b"\xf6"
    if isinstance(obj, int):
        return head(0, obj) if obj >= 0 else head(1, -1 - obj)
    if isinstance(obj, (bytes, bytearray)):
        return head(2, len(obj)) + bytes(obj)
    if isinstance(obj, str):
        b = obj.encode()
        return head(3, len(b)) + b
    if isinstance(obj, (list, tuple)):
        return head(4, len(obj)) + b"".join(cbor_encode(x) for x in obj)
    if isinstance(obj, dict):
        return head(5, len(obj)) + b"".join(
            cbor_encode(k) + cbor_encode(v) for k, v in obj.items())
    raise TypeError(f"cannot CBOR-encode {type(obj)}")


_BREAK = object()


def cbor_decode(data):
    def item(i):
        ib = data[i]
        major, ai = ib >> 5, ib & 0x1F
        i += 1
        if ib == 0xFF:
            return _BREAK, i
        if ai < 24:
            n = ai
        elif ai in (24, 25, 26, 27):
            size = 1 << (ai - 24)
            n = int.from_bytes(data[i:i + size], "big")
            i += size
        elif ai == 31:
            n = None  # indefinite length
        else:
            raise ValueError(f"bad CBOR head 0x{ib:02x}")

        if major == 0:
            return n, i
        if major == 1:
            return -1 - n, i
        if major in (2, 3):
            if n is None:
                raise ValueError("indefinite strings not supported")
            raw = bytes(data[i:i + n])
            return (raw if major == 2 else raw.decode()), i + n
        if major == 4:
            out = []
            while n is None or len(out) < n:
                v, i = item(i)
                if v is _BREAK:
                    break
                out.append(v)
            return out, i
        if major == 5:
            out = {}
            while n is None or len(out) < n:
                k, i = item(i)
                if k is _BREAK:
                    break
                out[k], i = item(i)
            return out, i
        if major == 7:
            simple = {20: False, 21: True, 22: None}
            if ai in simple:
                return simple[ai], i
            if ai == 25:
                return struct.unpack(">e", data[i - 2:i])[0], i
            if ai == 26:
                return struct.unpack(">f", data[i - 4:i])[0], i
            if ai == 27:
                return struct.unpack(">d", data[i - 8:i])[0], i
        raise ValueError(f"unsupported CBOR item 0x{ib:02x}")

    value, _ = item(0)
    return value

# --------------------------------------------------------------------------
# SMP


OP_READ, OP_READ_RSP, OP_WRITE, OP_WRITE_RSP = 0, 1, 2, 3
GRP_OS, GRP_IMG = 0, 1
GRP_LED = 64  # include/app_mgmt.h
OS_ECHO, OS_RESET, OS_INFO, OS_BOOTLOADER_INFO = 0, 5, 7, 8
IMG_STATE, IMG_UPLOAD = 0, 1
LED_STATE, LED_VERSION = 0, 1

MCUBOOT_MODES = [  # enum mcuboot_mode, bootutil/boot_status.h
    "single slot", "swap using scratch", "upgrade only", "swap using move",
    "direct XIP", "direct XIP with revert", "RAM load", "firmware loader",
    "single slot RAM load", "swap using offset",
]

MGMT_ERR = {
    0: "EOK", 1: "EUNKNOWN", 2: "ENOMEM", 3: "EINVAL", 4: "ETIMEOUT",
    5: "ENOENT", 6: "EBADSTATE", 7: "EMSGSIZE", 8: "ENOTSUP", 9: "ECORRUPT",
    10: "EBUSY", 11: "EACCESSDENIED",
}
GROUP_ERR = {  # include/<group>_mgmt.h
    GRP_LED: {1: "UNKNOWN", 2: "INVALID_LED", 3: "INVALID_MODE",
              4: "INVALID_PERIOD", 5: "GPIO"},
}


class SmpError(Exception):
    pass


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


class SmpSerial:
    """SMP over the Zephyr serial transport (MCUMGR_TRANSPORT_UART)."""

    FRAME_RAW = 90  # raw bytes per line: 120 base64 chars + 3, < 127

    def __init__(self, port, baud=115200, timeout=3.0, verbose=False):
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.timeout = timeout
        self.verbose = verbose
        self.seq = 0

    def close(self):
        self.ser.close()

    def _send_packet(self, pkt):
        body = struct.pack(">H", len(pkt) + 2) + pkt + \
            struct.pack(">H", crc16_xmodem(pkt))
        # a leading newline ends any partial line left in the device line
        # buffer (noise when the port is opened), which would otherwise
        # corrupt the first frame
        self.ser.write(b"\n")
        # each line is base64 of its own (multiple of 3 bytes, except last)
        for off in range(0, len(body), self.FRAME_RAW):
            marker = b"\x06\x09" if off == 0 else b"\x04\x14"
            chunk = base64.b64encode(body[off:off + self.FRAME_RAW])
            self.ser.write(marker + chunk + b"\n")
        self.ser.flush()

    def _recv_packet(self, timeout):
        deadline = time.monotonic() + timeout
        buf = bytearray()
        want = None
        line = bytearray()
        while time.monotonic() < deadline:
            b = self.ser.read(1)
            if not b:
                continue
            if b != b"\n":
                line += b
                continue
            raw, line = bytes(line), bytearray()
            if raw.startswith(b"\x06\x09"):
                buf = bytearray(base64.b64decode(raw[2:]))
                want = struct.unpack(">H", buf[:2])[0]
                buf = buf[2:]
            elif raw.startswith(b"\x04\x14") and want is not None:
                buf += base64.b64decode(raw[2:])
            else:
                if self.verbose and raw.strip():
                    print(f"  (non-SMP) {raw.decode(errors='replace').strip()}",
                          file=sys.stderr)
                continue
            if len(buf) >= want:
                pkt = bytes(buf[:want])
                if crc16_xmodem(pkt) != 0:
                    raise SmpError("bad CRC in response")
                return pkt[:-2]
        raise SmpError("timeout waiting for a response")

    def _transact(self, op, group, cmd, data, timeout):
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFF
        # SMP v2: version 1 in bits 3-4 of the op byte
        hdr = struct.pack(">BBHHBB", (1 << 3) | op, 0, len(data), group, seq, cmd)
        self.ser.reset_input_buffer()
        self._send_packet(hdr + data)

        deadline = time.monotonic() + timeout
        while True:
            pkt = self._recv_packet(max(0.1, deadline - time.monotonic()))
            _, _, r_len, r_grp, r_seq, r_cmd = struct.unpack(">BBHHBB", pkt[:8])
            if r_seq == seq and r_grp == group and r_cmd == cmd:
                return cbor_decode(pkt[8:8 + r_len]) if r_len else {}

    def request(self, op, group, cmd, payload=None, timeout=None, retries=2):
        # Requests sent while MCUboot runs (about 1 s after a reset) are
        # lost: retry on timeout. All the commands of this client can be
        # sent twice (reads, LED state, upload at the same offset).
        data = cbor_encode(payload or {})
        for attempt in range(retries + 1):
            try:
                rsp = self._transact(op, group, cmd, data,
                                     timeout or self.timeout)
                break
            except SmpError as e:
                if attempt == retries or "timeout" not in str(e):
                    raise
                if self.verbose:
                    print("  timeout, retrying", file=sys.stderr)
        if self.verbose:
            print(f"  rsp {rsp}", file=sys.stderr)

        if rsp.get("rc", 0):
            rc = rsp["rc"]
            raise SmpError(f"rc={rc} ({MGMT_ERR.get(rc, '?')})")
        err = rsp.get("err")
        if err and err.get("rc", 0):
            g, rc = err.get("group"), err["rc"]
            name = GROUP_ERR.get(g, {}).get(rc, "?")
            raise SmpError(f"group {g} error {rc} ({name})")
        return rsp

# --------------------------------------------------------------------------
# commands


def show_images(rsp):
    for img in rsp.get("images", []):
        flags = [f for f in ("active", "confirmed", "pending", "permanent")
                 if img.get(f)]
        print(f"slot {img.get('slot')}: {img.get('version'):<10} "
              f"{img.get('hash', b'').hex()}  {' '.join(flags)}")


def show_led(led):
    extra = f" {led['period_ms']} ms" if led["mode"] == "blink" else ""
    name = f" ({led['name']})" if "name" in led else ""  # protocol >= 2
    print(f"led {led['led']:>2}{name:<12}: {led['mode']}{extra}")


def cmd_led(smp, a):
    if a.action == "get":
        for led in smp.request(OP_READ, GRP_LED, LED_STATE)["leds"]:
            show_led(led)
        return
    req = {"led": a.led, "mode": a.action}
    if a.action == "blink":
        req["period_ms"] = a.period
    show_led(smp.request(OP_WRITE, GRP_LED, LED_STATE, req))


def cmd_version(smp, _a):
    """Custom group: protocol / image / build versions of the application."""
    rsp = smp.request(OP_READ, GRP_LED, LED_VERSION)
    print(f"protocol: {rsp['protocol']}\nimage:    {rsp['image']}\n"
          f"build:    {rsp['build']}\nleds:     {rsp['leds']}")


def cmd_info(smp, a):
    """OS group info, uname-like. Format letters: s kernel name, n node,
    r kernel release, v kernel version, b build date, m machine,
    p processor, i hardware platform, o OS, a all."""
    print(smp.request(OP_READ, GRP_OS, OS_INFO, {"format": a.format})["output"])


def cmd_bootloader(smp, _a):
    name = smp.request(OP_READ, GRP_OS, OS_BOOTLOADER_INFO)["bootloader"]
    print(f"bootloader: {name}")
    if name == "MCUboot":
        rsp = smp.request(OP_READ, GRP_OS, OS_BOOTLOADER_INFO, {"query": "mode"})
        mode = rsp["mode"]
        label = MCUBOOT_MODES[mode] if 0 <= mode < len(MCUBOOT_MODES) else "?"
        print(f"mode:       {mode} ({label})")
        if "no-downgrade" in rsp:
            print(f"no-downgrade: {rsp['no-downgrade']}")


def cmd_echo(smp, a):
    print(smp.request(OP_WRITE, GRP_OS, OS_ECHO, {"d": a.text})["r"])


def cmd_reset(smp, _a, wait=120.0):
    """Reset, then wait until the application answers again.

    Zephyr resets about 250 ms after its answer; MCUboot then boots in about
    1 s, or swaps the slots first (swap using scratch on the STM32F407:
    tens of seconds).
    """
    smp.request(OP_WRITE, GRP_OS, OS_RESET)
    print("reset", file=sys.stderr)
    time.sleep(1.0)  # past the reset delay: do not talk to the old image
    t0 = time.monotonic()
    while time.monotonic() - t0 < wait:
        try:
            smp.request(OP_WRITE, GRP_OS, OS_ECHO, {"d": "up"}, timeout=1.0,
                        retries=0)
            print(f"up after {time.monotonic() - t0 + 1.0:.1f} s", file=sys.stderr)
            return
        except SmpError:
            pass
    raise SmpError(f"no answer {wait:.0f} s after the reset")


def image_upload(smp, path, chunk):
    data = open(path, "rb").read()
    sha = hashlib.sha256(data).digest()
    off = 0
    t0 = time.monotonic()
    while off < len(data):
        req = {"image": 0, "off": off, "data": data[off:off + chunk]}
        if off == 0:
            req.update(len=len(data), sha=sha)
        # the first chunk waits for the slot erase (128 KiB sectors: seconds)
        rsp = smp.request(OP_WRITE, GRP_IMG, IMG_UPLOAD, req,
                          timeout=60 if off == 0 else None)
        off = rsp["off"]
        print(f"\rupload {off}/{len(data)} ({100 * off // len(data)}%)",
              end="", file=sys.stderr, flush=True)
    print(f"\nuploaded {len(data)} bytes in {time.monotonic() - t0:.1f} s",
          file=sys.stderr)


def cmd_image(smp, a):
    if a.action == "list":
        show_images(smp.request(OP_READ, GRP_IMG, IMG_STATE))
    elif a.action == "upload":
        image_upload(smp, a.file, a.chunk)
        rsp = smp.request(OP_READ, GRP_IMG, IMG_STATE)
        show_images(rsp)
        if a.test:
            slot1 = [i for i in rsp["images"] if i.get("slot") == 1]
            if not slot1:
                raise SmpError("no image in slot 1 after upload")
            show_images(smp.request(OP_WRITE, GRP_IMG, IMG_STATE,
                                    {"hash": slot1[0]["hash"], "confirm": False}))
        if a.reset:
            cmd_reset(smp, a)
    elif a.action == "test":
        show_images(smp.request(OP_WRITE, GRP_IMG, IMG_STATE,
                                {"hash": bytes.fromhex(a.hash), "confirm": False}))
    elif a.action == "confirm":
        req = {"confirm": True}
        if a.hash:
            req["hash"] = bytes.fromhex(a.hash)
        show_images(smp.request(OP_WRITE, GRP_IMG, IMG_STATE, req))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-p", "--port", default="/dev/ttyUSB0")
    p.add_argument("-b", "--baud", type=int, default=115200)
    p.add_argument("--timeout", type=float, default=3.0)
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    led = sub.add_parser("led", help="custom LED group")
    lsub = led.add_subparsers(dest="action", required=True)
    lsub.add_parser("get")
    for mode in ("on", "off", "blink"):
        sp = lsub.add_parser(mode)
        sp.add_argument("led", type=int, nargs="?", default=0)
        if mode == "blink":
            sp.add_argument("-t", "--period", type=int, default=500,
                            help="toggle interval in ms (default 500)")

    sub.add_parser("version", help="custom group: application versions")
    sub.add_parser("info", help="OS group: uname-like info").add_argument(
        "-f", "--format", default="a", help="format letters (default a: all)")
    sub.add_parser("bootloader", help="OS group: bootloader name and mode")
    sub.add_parser("echo").add_argument("text")
    sub.add_parser("reset")

    img = sub.add_parser("image", help="image group")
    isub = img.add_subparsers(dest="action", required=True)
    isub.add_parser("list")
    up = isub.add_parser("upload")
    up.add_argument("file")
    up.add_argument("--chunk", type=int, default=128,
                    help="data bytes per request (fit CONFIG_MCUMGR_TRANSPORT_UART_MTU)")
    up.add_argument("--test", action="store_true", help="mark slot 1 for test")
    up.add_argument("--reset", action="store_true", help="reset after upload")
    isub.add_parser("test").add_argument("hash")
    isub.add_parser("confirm").add_argument("hash", nargs="?")

    a = p.parse_args()
    smp = SmpSerial(a.port, a.baud, a.timeout, a.verbose)
    try:
        commands = {
            "led": cmd_led,
            "version": cmd_version,
            "info": cmd_info,
            "bootloader": cmd_bootloader,
            "echo": cmd_echo,
            "reset": cmd_reset,
            "image": cmd_image,
        }
        commands[a.cmd](smp, a)
    except SmpError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        smp.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
