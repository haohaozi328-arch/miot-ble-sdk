# -*- coding: utf-8 -*-
"""
ricott（米家口袋照片打印机Pro）打印通道 SPP 模拟服务端 —— 协议验证捷径

用途：不动 ESP32，先在 PC 侧验证「米家插件 ↔ Hannto 0x7E/DH/AES-ECB」协议是否
与 tuat-yate 逆向的 ricotg 海外版同版。server 侧实现（设备角色）：
  0x7E 分帧 → DH 握手出题(G/P/RA) → 校验 App 回执 → AES-128-ECB 会话
  → channel 3 JSON-RPC 应答 → channel 4 接收打印文件并落盘。

用法：
  python tools/ricott_spp_mock.py --loopback   # 本机回环自测（对照 tuat-yate 客户端）
  python tools/ricott_spp_mock.py --tcp 9527   # TCP 模式（给蓝牙主机隧道转发用）
  python tools/ricott_spp_mock.py --rfcomm     # Linux/BlueZ RFCOMM SPP server（需 pip bluetooth）

零硬依赖：pycryptodome 缺失时自动用内置纯 Python AES（仅自测用，性能足够）。
协议参考：refs/xiaomi-photo-printer（tuat-yate，实机验证）、refs/mizink。
"""

import argparse
import json
import os
import socket
import struct
import sys
import threading
import time

# ---------------------------------------------------------------- 路径与依赖 ---
REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                   "WorkBuddy", "2026-11-06-14-32-16", "refs",
                   "xiaomi-photo-printer")
REF = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    os.pardir, os.pardir))
# refs 目录在 WorkBuddy 工作区根下：
_CAND = [os.path.join(REF, "refs", "xiaomi-photo-printer"),
         os.path.join(REF, "xiaomi-photo-printer"),
         os.path.join(os.path.expanduser("~"),
                      "WorkBuddy", "2026-11-06-14-32-16", "refs",
                      "xiaomi-photo-printer")]
REF_DIR = next((p for p in _CAND if os.path.isdir(p)), None)


# ============================================================ 纯 Python AES ===
class PureAES:
    """AES-128-ECB 最小实现（自测回环用）。接口对齐 pycryptodome：
    AES.new(key, AES.MODE_ECB).encrypt(data) / .decrypt(data)"""

    MODE_ECB = 1

    @classmethod
    def new(cls, key, mode=None):
        """兼容 pycryptodome 调用形式：AES.new(key, AES.MODE_ECB)。"""
        return cls(key)

    _SBOX = [
        0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76,
        0xCA,0x82,0xC9,0x7D,0xFA,0x59,0x47,0xF0,0xAD,0xD4,0xA2,0xAF,0x9C,0xA4,0x72,0xC0,
        0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15,
        0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75,
        0x09,0x83,0x2C,0x1A,0x1B,0x6E,0x5A,0xA0,0x52,0x3B,0xD6,0xB3,0x29,0xE3,0x2F,0x84,
        0x53,0xD1,0x00,0xED,0x20,0xFC,0xB1,0x5B,0x6A,0xCB,0xBE,0x39,0x4A,0x4C,0x58,0xCF,
        0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8,
        0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2,
        0xCD,0x0C,0x13,0xEC,0x5F,0x97,0x44,0x17,0xC4,0xA7,0x7E,0x3D,0x64,0x5D,0x19,0x73,
        0x60,0x81,0x4F,0xDC,0x22,0x2A,0x90,0x88,0x46,0xEE,0xB8,0x14,0xDE,0x5E,0x0B,0xDB,
        0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79,
        0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08,
        0xBA,0x78,0x25,0x2E,0x1C,0xA6,0xB4,0xC6,0xE8,0xDD,0x74,0x1F,0x4B,0xBD,0x8B,0x8A,
        0x70,0x3E,0xB5,0x66,0x48,0x03,0xF6,0x0E,0x61,0x35,0x57,0xB9,0x86,0xC1,0x1D,0x9E,
        0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF,
        0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16,
    ]
    _RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]

    def __init__(self, key: bytes):
        if len(key) != 16:
            raise ValueError("AES-128 needs 16-byte key")
        self._rkey = self._expand(key)

    @classmethod
    def _xtime(cls, a):
        a <<= 1
        return (a ^ 0x1B) & 0xFF if a & 0x100 else a

    @classmethod
    def _mul(cls, a, b):
        p = 0
        for _ in range(8):
            if b & 1:
                p ^= a
            b >>= 1
            a = cls._xtime(a)
        return p

    @classmethod
    def _expand(cls, key):
        w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
        for i in range(4, 44):
            t = list(w[i - 1])
            if i % 4 == 0:
                t = t[1:] + t[:1]
                t = [cls._SBOX[b] for b in t]
                t[0] ^= cls._RCON[i // 4 - 1]
            w.append([a ^ b for a, b in zip(w[i - 4], t)])
        rks = []
        for r in range(11):
            rk = []
            for c in range(4):
                rk += w[r * 4 + c]
            rks.append(rk)
        return rks

    _INV_PERM = [0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3]

    def _crypt_block(self, block, enc=True):
        s = list(block)
        rks = self._rkey if enc else self._rkey  # 解密用正向轮密钥倒序取用
        def add_rk(rk):
            for i in range(16):
                s[i] ^= rk[i]
        def shift_rows():
            if enc:
                return [s[0],s[5],s[10],s[15],s[4],s[9],s[14],s[3],
                        s[8],s[13],s[2],s[7],s[12],s[1],s[6],s[11]]
            return [s[i] for i in self._INV_PERM]
        if enc:
            add_rk(rks[0])
            for rnd in range(1, 10):
                s = [self._SBOX[b] for b in s]
                s = shift_rows()
                t = s[:]
                for c in range(4):
                    a0,a1,a2,a3 = t[4*c:4*c+4]
                    s[4*c+0] = self._mul(a0,2)^self._mul(a1,3)^a2^a3
                    s[4*c+1] = a0^self._mul(a1,2)^self._mul(a2,3)^a3
                    s[4*c+2] = a0^a1^self._mul(a2,2)^self._mul(a3,3)
                    s[4*c+3] = self._mul(a0,3)^a1^a2^self._mul(a3,2)
                add_rk(rks[rnd])
            s = [self._SBOX[b] for b in s]
            s = shift_rows()
            add_rk(rks[10])
            return bytes(s)

        # ---- 解密：AddRoundKey(rk10) → [InvShift,InvSub,AddRoundKey,InvMix]×9 →
        #      InvShift,InvSub,AddRoundKey(rk0)
        add_rk(rks[10])
        for rnd in range(9, 0, -1):
            s = shift_rows()
            s = [self._SBOX.index(b) for b in s]
            add_rk(rks[rnd])
            t = s[:]
            for c in range(4):
                a0,a1,a2,a3 = t[4*c:4*c+4]
                s[4*c+0] = self._mul(a0,14)^self._mul(a1,11)^self._mul(a2,13)^self._mul(a3,9)
                s[4*c+1] = self._mul(a0,9)^self._mul(a1,14)^self._mul(a2,11)^self._mul(a3,13)
                s[4*c+2] = self._mul(a0,13)^self._mul(a1,9)^self._mul(a2,14)^self._mul(a3,11)
                s[4*c+3] = self._mul(a0,11)^self._mul(a1,13)^self._mul(a2,9)^self._mul(a3,14)
        s = shift_rows()
        s = [self._SBOX.index(b) for b in s]
        add_rk(rks[0])
        return bytes(s)

    def encrypt(self, data: bytes) -> bytes:
        data = bytes(data)
        if len(data) % 16:
            raise ValueError("ECB input must be 16-byte aligned")
        return b"".join(self._crypt_block(data[i:i+16], True)
                        for i in range(0, len(data), 16))

    def decrypt(self, data: bytes) -> bytes:
        data = bytes(data)
        if len(data) % 16:
            raise ValueError("ECB input must be 16-byte aligned")
        return b"".join(self._crypt_block(data[i:i+16], False)
                        for i in range(0, len(data), 16))


def _install_crypto_fallback():
    """给 tuat-yate 的 protocol.py 注入 Crypto.Cipher.AES 兼容模块。"""
    try:
        import Crypto.Cipher.AES  # noqa: F401
        return False  # 有真库，不注入
    except ImportError:
        pass
    import types
    crypto = types.ModuleType("Crypto")
    cipher = types.ModuleType("Crypto.Cipher")
    aes = types.ModuleType("Crypto.Cipher.AES")
    aes.new = lambda key, mode=None: PureAES(key)
    aes.MODE_ECB = PureAES.MODE_ECB
    crypto.Cipher = cipher
    cipher.AES = aes
    sys.modules.setdefault("Crypto", crypto)
    sys.modules.setdefault("Crypto.Cipher", cipher)
    sys.modules.setdefault("Crypto.Cipher.AES", aes)
    sys.modules.setdefault("Crypto.Util", types.ModuleType("Crypto.Util"))
    return True


# ======================================================== 模拟服务端（设备） ===
class RicottMockServer:
    """0x7E/DH/AES-ECB 协议的设备侧模拟。feed() 收字节，吐响应帧（列表）。"""

    # DH 参数：P 字段只有 16 字节 ASCII-hex → 模数上限 64-bit。
    # 取 2^64 - 59（已知最大 64-bit 素数 18446744073709551557），G=3。
    P_INT = (1 << 64) - 59
    G_INT = 3

    def __init__(self, verbose=True, out_dir=None):
        self.verbose = verbose
        self.out_dir = out_dir or "."
        self.buf = b""
        self.priv = None          # DH 私钥 a
        self.key = None           # 会话密钥
        self.hs_done = False
        self.sn = 0
        self.file_size = None
        self.file_chunks = {}     # pkg_num -> bytes
        self.job_id = 0
        self.finished = False

    # ---- 帧收发 ----
    def feed(self, data: bytes):
        self.buf += data
        out = []
        while True:
            f = self._try_parse()
            if f is None:
                break
            out += self._handle(f)
        return out

    def _try_parse(self):
        i = self.buf.find(b"\x7e\x64")
        if i < 0:
            self.buf = self.buf[-1:] if self.buf.endswith(b"\x7e") else b""
            return None
        self.buf = self.buf[i:]
        if len(self.buf) < 20:
            return None
        msg_attr = int.from_bytes(self.buf[18:20], "little")
        body_len = msg_attr % 1024 if msg_attr < 8192 else (msg_attr - 8192) % 1024
        if len(self.buf) < 20 + body_len + 2:
            return None
        frame = self.buf[:20 + body_len + 2]
        self.buf = self.buf[20 + body_len + 2:]
        return {
            "channel": frame[3], "interactive": frame[4], "encoding": frame[5],
            "msg_sn": int.from_bytes(frame[10:14], "little"),
            "pkg_total": int.from_bytes(frame[14:16], "little"),
            "pkg_num": int.from_bytes(frame[16:18], "little"),
            "msg_attr": msg_attr, "body": frame[20:20 + body_len],
        }

    def _next_sn(self):
        self.sn += 1
        return self.sn

    def _frame(self, channel, interactive, encoding, body, enc="000"):
        """用 tuat-yate 的 build_frame 组帧（body 若需加密先加密）。"""
        from xiaomi_photo_printer import protocol as P
        return P.build_frame(channel, interactive, encoding,
                             self._next_sn(), self._next_sn(), enc, body)

    def _log(self, *a):
        if self.verbose:
            print("[MOCK]", *a, flush=True)

    # ---- DH 握手 ----
    def _dh_pub(self, n):
        s = format(n, "X")
        return (s.rjust(16, "0")).encode("ascii")

    def _handle(self, f):
        import secrets
        from xiaomi_photo_printer import protocol as P

        ch, it = f["channel"], f["interactive"]

        if ch == 255 and it == 16:                       # App → hello，出题
            p_hex = format(self.P_INT, "X").upper().rjust(16, "0").encode()
            while True:
                self.priv = secrets.randbelow(self.P_INT - 4) + 2
                ra = pow(self.G_INT, self.priv, self.P_INT)
                if 2 < ra < self.P_INT - 1:
                    break
            body = b"0003" + p_hex + self._dh_pub(ra)
            self._log("DH hello ← 收到，出题 G=3 P=0x%X RA=%s" %
                      (self.P_INT, self._dh_pub(ra).decode()))
            return [self._frame(255, 17, P.ENCODING_JSON, body)]

        if ch == 255 and it == 18:                       # App → confirm，验证
            rb_field = f["body"][:16]
            enc_p = f["body"][16:32]
            rb = P.bytes_to_int(rb_field)
            k = pow(rb, self.priv, self.P_INT)
            k_bytes = P.int_to_bytes(k)
            if len(k_bytes) < 16:
                k_bytes = P.pack16_end(k_bytes)
            self.key = bytes(k_bytes[:16])
            calc = P.encrypt_ecb(self.key, self._dh_pub(self.P_INT))[:16]
            ok = calc == enc_p
            self._log("DH confirm ← RB=%s key=%s 回执校验=%s" %
                      (rb_field.decode(), self.key.hex(), "PASS" if ok else "FAIL"))
            self.hs_done = ok
            return [self._frame(255, 19, P.ENCODING_JSON,
                                b"ok" if ok else b"ng")]

        if not self.hs_done:
            self._log("!! 握手未完成就收到 ch=%d it=%d" % (ch, it))
            return []

        if ch == 3 and it == 6:                          # 加密 JSON-RPC 请求
            dec = P.decrypt_ecb(self.key, f["body"], strip_zeros=True)
            try:
                req = json.loads(dec.decode("utf-8"))
            except ValueError:
                self._log("!! JSON 解析失败: %r" % dec[:60])
                return []
            return self._rpc(req)

        if ch == 4 and it == 6:                          # 加密文件块
            dec = P.decrypt_ecb(self.key, f["body"], strip_zeros=False)
            job_id = struct.unpack("<I", dec[:4])[0]
            chunk = dec[4:].rstrip(b"\x00")
            self.file_chunks[f["pkg_num"]] = chunk
            got = sum(len(v) for v in self.file_chunks.values())
            self._log("文件块 %d/%d：%dB（累计 %d/%s）" %
                      (f["pkg_num"], f["pkg_total"], len(chunk),
                       got, self.file_size))
            if f["pkg_num"] == f["pkg_total"]:
                self._save_file(job_id)
            return []

        self._log("?? 未见过的帧 ch=%d it=%d body=%s" % (ch, it, f["body"][:32].hex()))
        return []

    def _save_file(self, job_id):
        data = b"".join(self.file_chunks[k]
                        for k in sorted(self.file_chunks))
        path = os.path.join(self.out_dir,
                            "ricott_mock_job%d.jpg" % job_id)
        with open(path, "wb") as fp:
            fp.write(data)
        self._log("*** 文件接收完成：%s（%dB，%d 块） ***" %
                  (path, len(data), len(self.file_chunks)))
        self.finished = True

    def _rpc(self, req):
        from xiaomi_photo_printer import protocol as P
        method, rid = req.get("method"), req.get("id")
        params = req.get("params", {})
        self._log("RPC → %s id=%s params=%s" % (method, rid, params))

        if method == "print_job":
            self.job_id = 1
            self.file_size = params.get("file_size")
            self.file_chunks.clear()
            self.finished = False
            result = {"job_id": self.job_id}
        elif method == "job_info":
            state = "finished" if self.finished else "downloading"
            result = [{"job_id": self.job_id, "job_state": state,
                       "job_type": 0, "prt_copies": 1}]
        elif method in ("get_prop", "device_info", "mixed_status"):
            result = {}
        else:
            result = {}
            self._log("?? 未实现的方法 %s（回空 result，观察插件反应）" % method)

        payload = json.dumps({"id": rid, "result": result},
                             separators=(",", ":")).encode("utf-8")
        enc = P.encrypt_ecb(self.key, payload)
        return [self._frame(3, 7, P.ENCODING_JSON, enc, enc="101")]


# ================================================================= 回环自测 ===
class _SockSerial:
    """把 socketpair 的一端适配成 tuat-yate 客户端要的 .read(n)/.write()/.close()
    接口（单 socket 全双工：sendall 与 recv 队列互不干扰）。"""

    def __init__(self, sock):
        self._s = sock

    def read(self, n):
        got = b""
        while len(got) < n:
            d = self._s.recv(n - len(got))
            if not d:
                raise ConnectionError("peer closed")
            got += d
        return got

    def write(self, data):
        self._s.sendall(bytes(data))

    def close(self):
        try:
            self._s.close()
        except OSError:
            pass


def loopback_test():
    """用 tuat-yate 的 PhotoPrinter 客户端打模拟服务端，全流程验证。"""
    if REF_DIR is None:
        print("!! 找不到 refs/xiaomi-photo-printer，自测中止")
        return 1
    sys.path.insert(0, REF_DIR)
    used_fallback = _install_crypto_fallback()
    from xiaomi_photo_printer.client import PhotoPrinter

    a, b = socket.socketpair()
    server = RicottMockServer(out_dir=os.path.join(os.path.dirname(__file__), "..", "logs"))
    os.makedirs(server.out_dir, exist_ok=True)

    stop = threading.Event()

    def serve():
        while not stop.is_set():
            try:
                data = a.recv(65536)   # a 是客户端 socket b 的对端
            except OSError:
                break
            if not data:
                break
            for resp in server.feed(data):
                a.sendall(resp)
        a.close()

    t = threading.Thread(target=serve, daemon=True)
    t.start()

    print("== 回环自测：tuat-yate 客户端 ↔ RicottMockServer ==")
    print("（AES %s）" % ("pycryptodome" if not used_fallback else "内置纯 Python"))
    cli = PhotoPrinter("loopback", debug=True)
    cli._ser = _SockSerial(b)      # 客户端用 b；服务端线程用 a（见 serve()）

    t0 = time.time()
    cli._handshake()                       # 1. DH 握手
    print(">>> 握手 OK（%.1fs）key=%s" % (time.time() - t0, cli.key.hex()))

    jpeg = bytes((i * 31 + 7) & 0xFF for i in range(3000))   # 伪 JPEG 载荷
    res = cli.command("print_job", {"file_size": len(jpeg), "copies": 1,
                                    "job_type": 0, "channel": 576})
    print(">>> print_job =", res)
    assert "job_id" in res.get("result", {}), "print_job 应返回 job_id"

    cli._send_file(jpeg, res["result"]["job_id"])            # 2. 文件上传

    info = cli.job_info(res["result"]["job_id"])             # 3. 进度轮询
    print(">>> job_info =", info)
    stop.set()
    try:
        b.close()
    except OSError:
        pass
    t.join(timeout=2)

    path = os.path.join(server.out_dir, "ricott_mock_job1.jpg")
    with open(path, "rb") as fp:
        saved = fp.read()
    ok = saved == jpeg
    print(">>> 文件比对：", "PASS" if ok else "FAIL",
          "(发送 %dB / 落盘 %dB)" % (len(jpeg), len(saved)))
    print("\n== 结果：%s ==" % ("全部通过 ✓" if ok and server.hs_done else "存在失败 ✗"))
    return 0 if ok else 1


# ================================================================ 实机模式 ===
def tcp_server(port):
    """TCP 隧道模式：server 逻辑跑在本机，字节流交给外部转发
    （例如树莓派把 RFCOMM fd 桥接过来）。"""
    if REF_DIR:
        sys.path.insert(0, REF_DIR)
        _install_crypto_fallback()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print("[MOCK] TCP 等待蓝牙主机隧道 127.0.0.1:%d ..." % port)
    conn, addr = srv.accept()
    print("[MOCK] 隧道已连：", addr)
    server = RicottMockServer(out_dir=os.path.join(os.path.dirname(__file__), "..", "logs"))
    os.makedirs(server.out_dir, exist_ok=True)

    def pump():
        while True:
            data = conn.recv(65536)
            if not data:
                break
            for resp in server.feed(data):
                conn.sendall(resp)

    threading.Thread(target=pump, daemon=True).start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


def rfcomm_server():
    """Linux/BlueZ 直起 RFCOMM SPP server（需 PyBluez；配合 bdaddr 改 MAC 可让
    手机插件直连）。Windows 上不可用。"""
    try:
        import bluetooth
    except ImportError:
        print("!! 需要 PyBluez：sudo apt install libbluetooth-dev && pip install pybluez")
        return 1
    if REF_DIR:
        sys.path.insert(0, REF_DIR)
        _install_crypto_fallback()
    sid = bluetooth.BluetoothSocket(bluetooth.RFCOMM)
    sid.bind(("", 1))
    bluetooth.advertise_service(sid, "RicottMock",
                                service_id="00001101-0000-1000-8000-00805f9b34fb",
                                service_classes=["00001101-0000-1000-8000-00805f9b34fb"])
    sid.listen(1)
    print("[MOCK] RFCOMM SPP 监听中（channel 1，UUID 00001101）……")
    conn, peer = sid.accept()
    print("[MOCK] 手机已连：", peer)
    server = RicottMockServer(out_dir=os.path.join(os.path.dirname(__file__), "..", "logs"))
    os.makedirs(server.out_dir, exist_ok=True)

    def pump():
        while True:
            data = conn.recv(65536)
            if not data:
                break
            for resp in server.feed(data):
                conn.sendall(resp)

    threading.Thread(target=pump, daemon=True).start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="ricott SPP 模拟服务端")
    ap.add_argument("--loopback", action="store_true", help="本机回环自测")
    ap.add_argument("--tcp", type=int, metavar="PORT", help="TCP 隧道模式")
    ap.add_argument("--rfcomm", action="store_true", help="Linux RFCOMM SPP server")
    args = ap.parse_args()
    if args.loopback:
        sys.exit(loopback_test())
    elif args.tcp:
        tcp_server(args.tcp)
    elif args.rfcomm:
        sys.exit(rfcomm_server())
    else:
        ap.print_help()
