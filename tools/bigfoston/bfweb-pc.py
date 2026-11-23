#!/usr/bin/env python3
"""BigFoston Web — modo completo: o Chromium do PC navega, o FOSTON mostra e toca.

O FOSTON só tem internet através do PC (rede USB). Este programa roda no PC, abre um
Chrome/Chromium sem janela (perfil próprio, som mudo) do tamanho exato da página do
FOSTON e se liga ao app Internet do aparelho (192.168.7.2:7071). Assim qualquer site que
funciona no Chrome funciona no FOSTON: JavaScript, CSS moderno, logins, YouTube (imagem).

    tools/bigfoston/bfweb-pc.py                 # fica esperando o FOSTON, reconecta sozinho
    tools/bigfoston/bfweb-pc.py --host 127.0.0.1   # simulador do shell no próprio PC
    tools/bigfoston/bfweb-pc.py --browser /usr/bin/chromium

Imagem: o Chrome manda quadros (screencast PNG) só quando a página muda; aqui eles viram
RGB565 e só os blocos de 32x32 que mudaram vão ao FOSTON (zlib). Toque, rolagem e teclado
voltam pelo mesmo protocolo de comandos do motor local (src/web/frontend/protocol.md).

Segurança: a conexão sai do PC para o FOSTON (nenhuma porta aberta no PC); na primeira
vez o FOSTON pergunta na tela se este PC pode mostrar páginas (chave aleatória guardada
em ~/.config/bigfoston-web/key). O Chrome usa um perfil separado do seu navegador
(~/.local/share/bigfoston-web/chrome). Só http/https/about são abertos por comando.
Downloads vão para o cartão do FOSTON (o aparelho baixa pelo bfdl), não para o PC.

Requer: Python 3, Pillow e numpy; Google Chrome, Chromium ou Brave.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import fcntl
import base64
import io
import json
import re
import os
import secrets
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
import zlib

try:
    import numpy as np
    from PIL import Image
except ImportError as e:               # pragma: no cover
    sys.exit(f"bfweb-pc: falta {e.name} (pacotes python-pillow e python-numpy)")

PORT = 7071
TILE = 32
MIN_FRAME_S = 0.04          # no máximo ~25 quadros por segundo (o FOSTON confirma cada um: Q)
# som: o Chrome toca numa saída virtual só dele (não sai no PC) e o que ela recebe vai pelo
# cabo; S16_LE estéreo 32 kHz (src/ui/core/webaudio.h), blocos de 40 ms, só enquanto há som
SINK = "bigfoston_web"
AUDIO_PORT = 7072           # conexão só do som (o FOSTON a lê numa thread própria)
AUDIO_RATE, AUDIO_CH = 32000, 2
AUDIO_CHUNK = AUDIO_RATE * AUDIO_CH * 2 * 40 // 1000
AUDIO_HANG_S = 1.0          # depois do último som, ainda manda 1 s (pausas curtas não cortam)
VERBOSE = False
CONF = os.path.expanduser("~/.config/bigfoston-web")
DATA = os.path.expanduser("~/.local/share/bigfoston-web")
BROWSERS = ["chromium", "chromium-browser", "google-chrome-stable", "google-chrome", "brave", "brave-browser"]
UA = None  # definido depois de saber a versão do navegador

# script em toda página: cursor de texto (teclado do FOSTON) e janelas novas na mesma aba
PAGE_SCRIPT = r"""
(() => {
  if (window.__bfweb) return; window.__bfweb = 1;
  const editable = e => e && (e.isContentEditable || e.tagName === 'TEXTAREA' ||
      (e.tagName === 'INPUT' && !/^(button|submit|reset|checkbox|radio|file|image|range|color|hidden)$/i.test(e.type)));
  const report = () => {
    const e = document.activeElement;
    if (!editable(e)) { try { bfCaret('0'); } catch (x) {} return; }
    const r = e.getBoundingClientRect();
    try { bfCaret(JSON.stringify([Math.round(r.left), Math.round(r.top), Math.round(r.height)])); } catch (x) {}
  };
  document.addEventListener('focusin', report, true);
  document.addEventListener('focusout', () => setTimeout(report, 0), true);
  window.open = (u) => { if (u) location.href = new URL(u, location.href).href; return null; };
  document.addEventListener('click', ev => {
    const a = ev.target.closest && ev.target.closest('a[target]');
    if (a && a.target !== '_self' && a.target !== '_top' && a.target !== '_parent') a.target = '_self';
  }, true);
  // vídeo tocando (para o modo performance do FOSTON): playing/pause/ended dos <video>
  const vid = Math.random().toString(36).slice(2, 8);
  let vlast = null;
  const vcheck = () => {
    const on = [...document.querySelectorAll('video')].some(v => !v.paused && !v.ended);
    if (on !== vlast) { vlast = on; try { bfVideo(vid + (on ? ' 1' : ' 0')); } catch (x) {} }
  };
  for (const ev of ['playing', 'pause', 'ended', 'emptied'])
    document.addEventListener(ev, () => setTimeout(vcheck, 0), true);
  window.addEventListener('pagehide', () => { try { bfVideo(vid + ' 0'); } catch (x) {} });
  // tela cheia: o Chrome headless recusa requestFullscreen ("not granted"). Emulada só na
  // página principal: o elemento ocupa a área toda, o site vê fullscreenElement e os
  // eventos normais, e o FOSTON esconde as barras (bfFull) e passa a página para 800x480.
  if (window.top !== window) return;
  const fsStyle = document.createElement('style');
  fsStyle.textContent = 'html{overflow:hidden!important}' +
    '.__bffs{position:fixed!important;inset:0!important;width:100vw!important;' +
    'height:100vh!important;max-width:none!important;max-height:none!important;margin:0!important;' +
    'transform:none!important;z-index:2147483647!important;background:#000!important}';
  let fsEl = null;
  const fsFire = el => [el, document].forEach(t => {
    t.dispatchEvent(new Event('fullscreenchange', { bubbles: true }));
    t.dispatchEvent(new Event('webkitfullscreenchange', { bubbles: true }));
  });
  for (const k of ['fullscreenElement', 'webkitFullscreenElement', 'webkitCurrentFullScreenElement'])
    Object.defineProperty(Document.prototype, k, { get: () => fsEl, configurable: true });
  for (const k of ['fullscreenEnabled', 'webkitFullscreenEnabled'])
    Object.defineProperty(Document.prototype, k, { get: () => true, configurable: true });
  Object.defineProperty(Document.prototype, 'webkitIsFullScreen', { get: () => !!fsEl, configurable: true });
  const fsEnter = function () {
    if (fsEl !== this) {
      if (fsEl) fsEl.classList.remove('__bffs');
      fsEl = this;
      (document.head || document.documentElement).appendChild(fsStyle);
      this.classList.add('__bffs');
      try { bfFull('1'); } catch (x) {}
      fsFire(this);
    }
    return Promise.resolve();
  };
  const fsLeave = function () {
    const el = fsEl;
    if (el) {
      fsEl = null;
      el.classList.remove('__bffs');
      try { bfFull('0'); } catch (x) {}
      fsFire(el);
    }
    return Promise.resolve();
  };
  Element.prototype.requestFullscreen = Element.prototype.webkitRequestFullscreen =
    Element.prototype.webkitRequestFullScreen = fsEnter;
  Document.prototype.exitFullscreen = Document.prototype.webkitExitFullscreen =
    Document.prototype.webkitCancelFullScreen = fsLeave;
  window.__bfExitFull = () => document.exitFullscreen();
  // título da página: o Chrome não avisa trocas de document.title pelo Target.targetInfoChanged
  let last = null;
  const title = () => {
    const v = document.title;
    if (v !== last) { last = v; try { bfTitle(v); } catch (x) {} }
  };
  const watch = () => {
    title();
    if (document.head) new MutationObserver(title).observe(document.head,
        { subtree: true, childList: true, characterData: true });
  };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', watch);
  else watch();
  window.addEventListener('load', title);
})();
"""

KEYS = {  # nome do protocolo -> (key, code, keyCode, texto)
    "enter": ("Enter", "Enter", 13, "\r"), "backspace": ("Backspace", "Backspace", 8, ""),
    "delete": ("Delete", "Delete", 46, ""), "left": ("ArrowLeft", "ArrowLeft", 37, ""),
    "right": ("ArrowRight", "ArrowRight", 39, ""), "up": ("ArrowUp", "ArrowUp", 38, ""),
    "down": ("ArrowDown", "ArrowDown", 40, ""), "tab": ("Tab", "Tab", 9, ""),
    "escape": ("Escape", "Escape", 27, ""), "home": ("Home", "Home", 36, ""), "end": ("End", "End", 35, ""),
    "pageup": ("PageUp", "PageUp", 33, ""), "pagedown": ("PageDown", "PageDown", 34, ""),
}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, file=sys.stderr, flush=True)


# ------------------------------------------------------------------ Chrome (CDP por pipe)

def load_lz4():
    """compressão LZ4 de bloco pela liblz4 do sistema (ctypes; nada para instalar). O FOSTON
    descomprime LZ4 ~4x mais rápido que o inflate (225 -> 57 ms num quadro de vídeo)."""
    import ctypes
    import ctypes.util
    name = ctypes.util.find_library("lz4") or "liblz4.so.1"
    try:
        lib = ctypes.CDLL(name)
        lib.LZ4_compressBound.argtypes = [ctypes.c_int]
        lib.LZ4_compress_fast.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    except (OSError, AttributeError):
        return None

    def compress(data):
        cap = lib.LZ4_compressBound(len(data))
        out = ctypes.create_string_buffer(cap)
        n = lib.LZ4_compress_fast(data, out, len(data), cap, 1)
        return out.raw[:n] if n > 0 else None
    return compress


LZ4 = load_lz4()


def die_with_parent():
    """no filho: morre junto com este script, mesmo num kill -9 (PR_SET_PDEATHSIG)"""
    try:
        import ctypes
        ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGTERM)
    except (OSError, AttributeError):
        pass


class AudioSink:
    """Saída virtual (PulseAudio/PipeWire) só para o Chrome do FOSTON. Sem pactl/parec,
    ok = False e o Chrome fica mudo, como antes."""

    def __init__(self):
        self.module = None
        self.ok = False
        if not (shutil.which("pactl") and shutil.which("parec")):
            return
        try:
            sinks = subprocess.run(["pactl", "list", "short", "sinks"], capture_output=True,
                                   text=True, timeout=5).stdout
            if any(line.split("\t")[1:2] == [SINK] for line in sinks.splitlines()):
                # sobrou de uma execução anterior (kill -9): reaproveita e remove ao sair
                mods = subprocess.run(["pactl", "list", "short", "modules"], capture_output=True,
                                      text=True, timeout=5).stdout
                for line in mods.splitlines():
                    f = line.split("\t")
                    if len(f) > 2 and f[1] == "module-null-sink" and f"sink_name={SINK}" in f[2].split():
                        self.module = f[0]
                self.ok = True
                return
            r = subprocess.run(["pactl", "load-module", "module-null-sink", f"sink_name={SINK}",
                                "sink_properties=device.description=BigFoston-Web"],
                               capture_output=True, text=True, timeout=5)
            if r.returncode == 0 and r.stdout.strip().isdigit():
                self.module = r.stdout.strip()
                self.ok = True
        except (OSError, subprocess.SubprocessError):
            pass

    def close(self):
        if self.module:
            try:
                subprocess.run(["pactl", "unload-module", self.module], timeout=5,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except (OSError, subprocess.SubprocessError):
                pass
            self.module = None


def start_xvfb():
    """Tela X virtual só do Chrome do FOSTON (invisível; número livre escolhido pelo próprio
    Xvfb). Com ela o Chrome roda normal, com janela: o YouTube recusa tocar em Chrome
    headless ou marcado como automatizado ("Algo deu errado" ~40 s depois de começar).
    Devolve (processo, ":N") ou (None, None) sem Xvfb."""
    exe = shutil.which("Xvfb")
    if not exe:
        return None, None
    r, w = os.pipe()
    try:
        p = subprocess.Popen([exe, "-displayfd", str(w), "-screen", "0", "1024x768x24", "-nolisten", "tcp",
                              "-noreset"], pass_fds=(w,), preexec_fn=die_with_parent,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        os.close(r)
        os.close(w)
        return None, None
    os.close(w)
    num = b""
    end = time.time() + 10
    while time.time() < end and not num.endswith(b"\n"):
        ready, _, _ = select.select([r], [], [], 0.5)
        if ready:
            d = os.read(r, 16)
            if not d:
                break
            num += d
    os.close(r)
    if not num.strip().isdigit():
        p.kill()
        return None, None
    return p, ":" + num.strip().decode()


class Chrome:
    def __init__(self, exe, w, h, audio=False):
        self.audio = audio
        os.makedirs(os.path.join(DATA, "chrome"), exist_ok=True)
        self.xvfb, display = start_xvfb()
        r_in, w_in = os.pipe()
        r_out, w_out = os.pipe()

        def child():
            a, b = os.dup(r_in), os.dup(w_out)
            os.dup2(a, 3)
            os.dup2(b, 4)
            os.setsid()
            die_with_parent()                # sem isso ficava órfão: setsid o tira do Ctrl+C
        args = [exe, "--remote-debugging-pipe", f"--user-data-dir={DATA}/chrome",
                "--no-first-run", "--no-default-browser-check", "--hide-scrollbars",
                "--lang=pt-BR", "--accept-lang=pt-BR,pt,en", f"--window-size={w},{h}",
                "--force-device-scale-factor=1", "--disable-features=Translate",
                "--password-store=basic", "about:blank"]
        env = dict(os.environ)
        if display:
            # Chrome normal na tela virtual; sem a marca de "controlado por automação" (quem
            # navega é a pessoa no FOSTON, não um robô)
            env["DISPLAY"] = display
            env.pop("WAYLAND_DISPLAY", None)
            args[1:1] = ["--ozone-platform=x11", "--disable-blink-features=AutomationControlled"]
            log(f"Chrome com janela na tela virtual {display}")
        else:
            args.insert(1, "--headless=new")
            log("sem Xvfb: Chrome headless (o YouTube para de tocar depois de ~40 s; "
                "instale o pacote xorg-server-xvfb)")
        if audio:
            env["PULSE_SINK"] = SINK         # toca na saída virtual, não nos alto-falantes do PC
            # endereço digitado no FOSTON não conta como gesto na página: sem isto o vídeo
            # começaria mudo (perfil novo, sem histórico de mídia)
            args.insert(-1, "--autoplay-policy=no-user-gesture-required")
        else:
            args.insert(-1, "--mute-audio")
        self.p = subprocess.Popen(args, preexec_fn=child, close_fds=False, env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(r_in)
        os.close(w_out)
        self.wfd, self.rfd = w_in, r_out
        self.buf = b""
        self.next_id = 1
        self.pending = {}        # id -> callback
        self.events = []
        self.session = None      # sessão CDP da ligação atual com o FOSTON

    def send(self, method, params=None, session=None, cb=None):
        i = self.next_id
        self.next_id += 1
        m = {"id": i, "method": method, "params": params or {}}
        if session:
            m["sessionId"] = session
        if cb:
            self.pending[i] = cb
        data = json.dumps(m).encode() + b"\0"
        while data:
            n = os.write(self.wfd, data)
            data = data[n:]
        return i

    def call(self, method, params=None, session=None, timeout=15):
        """chamada síncrona (só na inicialização)"""
        i = self.send(method, params, session)
        end = time.time() + timeout
        while time.time() < end:
            for m in self.read(0.2):
                if m.get("id") == i:
                    if "error" in m:
                        raise RuntimeError(f"{method}: {m['error']}")
                    return m.get("result", {})
                self.events.append(m)
        raise TimeoutError(method)

    def read(self, timeout):
        r, _, _ = select.select([self.rfd], [], [], timeout)
        if r:
            chunk = os.read(self.rfd, 1 << 20)
            if not chunk:
                raise EOFError("o navegador fechou")
            self.buf += chunk
        out = []
        while b"\0" in self.buf:
            raw, self.buf = self.buf.split(b"\0", 1)
            out.append(json.loads(raw))
        return out

    def close(self):
        try:
            os.killpg(self.p.pid, signal.SIGTERM)
            self.p.wait(5)
        except Exception:
            try:
                os.killpg(self.p.pid, signal.SIGKILL)
            except Exception:
                pass
        if self.xvfb:
            self.xvfb.terminate()
            try:
                self.xvfb.wait(5)
            except subprocess.TimeoutExpired:
                self.xvfb.kill()
            self.xvfb = None


REAL = ["/usr/lib/chromium/chromium", "/opt/google/chrome/chrome", "/opt/brave.com/brave/brave",
        "/usr/lib/chromium-browser/chromium-browser"]


def works(exe):
    """o navegador existe e responde (scripts de atalho podem apontar para nada)"""
    try:
        r = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=15)
        return r.returncode == 0 and any(w in r.stdout for w in ("Chrom", "Brave"))
    except (OSError, subprocess.TimeoutExpired):
        return False


def find_browser(choice):
    if choice:
        return choice
    cands = [p for p in REAL if os.access(p, os.X_OK)] + [shutil.which(b) for b in BROWSERS]
    for p in cands:
        if p and works(p):
            return p
    sys.exit("bfweb-pc: nenhum Chrome/Chromium/Brave encontrado (use --browser)")


# ------------------------------------------------------------------ ligação com o FOSTON

def frame(t, payload):
    return struct.pack("<cI", t, len(payload)) + payload


class Foston:
    def __init__(self, host, key, name):
        self.s = socket.create_connection((host, PORT), timeout=5)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(None)
        self.buf = b""
        self.s.sendall(frame(b"H", f"{key}\t{name}".encode()))

    def send(self, t, payload):
        self.s.sendall(frame(t, payload))

    def event(self, line):
        self.send(b"E", line.replace("\n", " ").encode())

    def read(self):
        chunk = self.s.recv(65536)
        if not chunk:
            raise EOFError("o FOSTON fechou a ligação")
        self.buf += chunk
        out = []
        while len(self.buf) >= 5:
            t, n = struct.unpack("<cI", self.buf[:5])
            if n > 1 << 20:
                raise ValueError("mensagem grande demais")
            if len(self.buf) < 5 + n:
                break
            out.append((t, self.buf[5:5 + n]))
            self.buf = self.buf[5 + n:]
        return out


# ------------------------------------------------------------------ sessão

class Session:
    def __init__(self, chrome, foston, w, h, start_url, caps=()):
        self.c, self.f = chrome, foston
        self.w, self.h = w, h
        self.prev = None                 # último quadro RGB565 (h, w) uint16 enviado
        self.inflight = 0                # quadros mandados e ainda não aplicados no FOSTON (Q)
        self.caps = set(caps)            # "lz4", "half": o que o FOSTON entende (mensagem C)
        self.scale = 1                   # 2: vídeo em tela cheia em meia resolução
        self.video = {}                  # quadro da página -> <video> tocando
        self.st_t, self.st_n, self.st_b, self.st_ms = time.time(), 0, 0, 0.0
        self.pending_img = None          # quadro mais novo esperando a vez
        self.last_sent = 0.0
        self.dls = {}                    # guid -> {"id", "name", "url", "file", "sent"}
        self.next_dl = 1
        self.url, self.title = "", ""
        self.loading = False
        self.zoom = 100
        self.last_scroll_q = 0
        self.dl_dir = os.path.join(DATA, "downloads-tmp")
        self.arec, self.abuf, self.a_loud, self.asock = None, b"", 0.0, None
        self.full = False
        targets = self.c.call("Target.getTargets")["targetInfos"]
        page = next((t for t in targets if t["type"] == "page"), None)
        if not page:
            page = {"targetId": self.c.call("Target.createTarget", {"url": "about:blank"})["targetId"]}
        self.tid = page["targetId"]
        # a ligação anterior (o FOSTON reiniciou ou o app foi reaberto) deixou uma sessão
        # anexada à mesma aba: sem desanexar, cada evento e cada quadro viriam em dobro
        if self.c.session:
            for m, p in [("Page.stopScreencast", {}), ("Target.detachFromTarget", {"sessionId": self.c.session})]:
                try:
                    self.c.call(m, p, session=self.c.session if m.startswith("Page.") else None, timeout=3)
                except (RuntimeError, TimeoutError):
                    pass
            self.c.session = None
        self.c.events = [m for m in self.c.events if "sessionId" not in m]
        self.sid = self.c.call("Target.attachToTarget", {"targetId": self.tid, "flatten": True})["sessionId"]
        self.c.session = self.sid
        s = self.sid
        version = self.c.call("Browser.getVersion")
        major = version["product"].split("/")[1].split(".")[0]
        ua = (f"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
              f"Chrome/{major}.0.0.0 Safari/537.36")
        for m, p in [("Page.enable", {}), ("Runtime.enable", {}), ("Security.enable", {}),
                     ("Target.setDiscoverTargets", {"discover": True}),
                     ("Network.setUserAgentOverride", {"userAgent": ua, "acceptLanguage": "pt-BR,pt;q=0.9,en;q=0.5"}),
                     ("Runtime.addBinding", {"name": "bfCaret"}),
                     ("Runtime.addBinding", {"name": "bfTitle"}),
                     ("Runtime.addBinding", {"name": "bfFull"}),
                     ("Runtime.addBinding", {"name": "bfVideo"}),
                     ("Page.addScriptToEvaluateOnNewDocument", {"source": PAGE_SCRIPT}),
                     ("Page.setLifecycleEventsEnabled", {"enabled": True}),
                     # o FOSTON desenha o próprio indicador de rolagem; --hide-scrollbars só
                     # vale no headless
                     ("Emulation.setScrollbarsHidden", {"hidden": True})]:
            self.c.call(m, p, session=None if m.startswith("Target.") else s)
        os.makedirs(self.dl_dir, exist_ok=True)
        self.c.call("Browser.setDownloadBehavior", {"behavior": "allowAndName", "downloadPath": self.dl_dir,
                                                     "eventsEnabled": True})
        self.metrics()
        self.c.call("Page.startScreencast", {"format": "png", "everyNthFrame": 1, "maxWidth": w, "maxHeight": h},
                    session=s)
        self.audio_start()
        self.f.event(f"READY {w} {h}")
        if start_url:
            self.go(start_url)

    # ---- comandos do FOSTON
    def metrics(self):
        z = self.zoom / 100
        self.c.send("Emulation.setDeviceMetricsOverride", {
            "width": round(self.w / z), "height": round(self.h / z), "deviceScaleFactor": z, "mobile": False,
            "screenWidth": round(self.w / z), "screenHeight": round(self.h / z)}, session=self.sid)

    def go(self, url):
        if not url.lower().startswith(("http://", "https://", "about:")):
            self.f.event("WARN url recusada")
            return
        self.c.send("Page.navigate", {"url": url}, session=self.sid)

    def mouse(self, kind, x, y, **kw):
        z = self.zoom / 100
        p = {"type": kind, "x": x / z, "y": y / z}
        p.update(kw)
        self.c.send("Input.dispatchMouseEvent", p, session=self.sid)

    def key(self, name):
        k = KEYS.get(name)
        if not k:
            return
        key, code, vk, text = k
        base = {"key": key, "code": code, "windowsVirtualKeyCode": vk, "nativeVirtualKeyCode": vk}
        self.c.send("Input.dispatchKeyEvent", dict(base, type="rawKeyDown"), session=self.sid)
        if text:
            self.c.send("Input.dispatchKeyEvent", dict(base, type="char", text=text), session=self.sid)
        self.c.send("Input.dispatchKeyEvent", dict(base, type="keyUp"), session=self.sid)

    def history(self, delta):
        def got(m):
            r = m.get("result", {})
            i = r.get("currentIndex", 0) + delta
            ent = r.get("entries", [])
            if 0 <= i < len(ent):
                self.c.send("Page.navigateToHistoryEntry", {"entryId": ent[i]["id"]}, session=self.sid)
        self.c.send("Page.getNavigationHistory", {}, session=self.sid, cb=got)

    def feat(self, x, y):
        z = self.zoom / 100
        js = (f"(()=>{{const e=document.elementFromPoint({x / z},{y / z});if(!e)return['',''];"
              "const a=e.closest('a[href]');const i=e.closest('img')||(e.tagName==='IMG'?e:null);"
              "return [a?a.href:'', i?(i.currentSrc||i.src):'']})()")

        def got(m):
            v = m.get("result", {}).get("result", {}).get("value") or ["", ""]
            self.f.event(f"FEAT {x} {y}\t{v[0]}\t{v[1]}")
        self.c.send("Runtime.evaluate", {"expression": js, "returnByValue": True}, session=self.sid, cb=got)

    def command(self, line):
        cmd, _, arg = line.partition(" ")
        nums = [int(v) for v in arg.split()[:4] if v.lstrip("-").isdigit()]
        if cmd == "GO":
            self.go(arg.strip())
        elif cmd == "BACK":
            self.history(-1)
        elif cmd == "FWD":
            self.history(1)
        elif cmd == "RELOAD":
            self.c.send("Page.reload", {}, session=self.sid)
        elif cmd == "STOP":
            self.c.send("Page.stopLoading", {}, session=self.sid)
        elif cmd == "CLICK" and len(nums) == 2:
            x, y = nums
            self.mouse("mouseMoved", x, y)
            self.mouse("mousePressed", x, y, button="left", clickCount=1)
            self.mouse("mouseReleased", x, y, button="left", clickCount=1)
        elif cmd == "LONG" and len(nums) == 2:
            self.feat(*nums)
        elif cmd == "SCROLL" and len(nums) == 2:
            self.mouse("mouseWheel", self.w // 2, self.h // 2, deltaX=nums[0], deltaY=nums[1])
        elif cmd == "KEY":
            self.key(arg.strip())
        elif cmd == "TEXT":
            self.c.send("Input.insertText", {"text": arg}, session=self.sid)
        elif cmd == "ZOOM" and nums:
            self.zoom = max(30, min(400, nums[0]))
            self.metrics()
            self.f.event(f"ZOOM {self.zoom}")
        elif cmd == "DOWNLOAD":
            u = arg.strip()
            if u.lower().startswith(("http://", "https://")):
                js = ("(()=>{const a=document.createElement('a');a.href=" + json.dumps(u) +
                      ";a.download='';document.body.appendChild(a);a.click();a.remove()})()")
                self.c.send("Runtime.evaluate", {"expression": js}, session=self.sid)
        elif cmd == "DLCANCEL" and nums:
            for guid, d in list(self.dls.items()):
                if d["id"] == nums[0]:
                    self.c.send("Browser.cancelDownload", {"guid": guid})
                    self.dl_finish(guid, notify=False)
        elif cmd == "REDRAW":
            self.prev = None
        elif cmd == "SIZE" and len(nums) == 2 and 160 <= nums[0] <= 1920 and 120 <= nums[1] <= 1080:
            # tela cheia no FOSTON (800x480) e de volta (800x380)
            self.w, self.h = nums
            self.scale = 1
            self.prev = None
            self.metrics()
            self.c.send("Page.stopScreencast", {}, session=self.sid)
            self.c.send("Page.startScreencast", {"format": "png", "everyNthFrame": 1,
                                                 "maxWidth": self.w, "maxHeight": self.h}, session=self.sid)
        elif cmd == "SCALE" and nums and nums[0] in (1, 2):
            # vídeo em tela cheia: o Chrome já entrega a imagem reduzida (menos trabalho para todos)
            self.scale = nums[0] if "half" in self.caps and LZ4 else 1
            self.prev = None
            self.c.send("Page.stopScreencast", {}, session=self.sid)
            self.c.send("Page.startScreencast", {"format": "png", "everyNthFrame": 1,
                                                 "maxWidth": self.w // self.scale,
                                                 "maxHeight": self.h // self.scale}, session=self.sid)
        elif cmd == "EXITFULL":                  # o FOSTON saiu da tela cheia pelo botão dele
            self.c.send("Runtime.evaluate", {"expression": "window.__bfExitFull && __bfExitFull()"},
                        session=self.sid)
        elif cmd == "QUIT":
            raise EOFError("o FOSTON pediu para sair")

    # ---- eventos do Chrome
    def nav_state(self):
        def got(m):
            r = m.get("result", {})
            i, n = r.get("currentIndex", 0), len(r.get("entries", []))
            self.f.event(f"NAV {int(i > 0)} {int(i < n - 1)}")
        self.c.send("Page.getNavigationHistory", {}, session=self.sid, cb=got)

    def scroll_state(self):
        def got(m):
            v = m.get("result", {}).get("result", {}).get("value")
            if v:
                z = self.zoom / 100
                self.f.event("SCROLL %d %d %d %d" % tuple(round(a * z) for a in v))
        js = "[scrollX,scrollY,document.documentElement.scrollWidth,document.documentElement.scrollHeight]"
        self.c.send("Runtime.evaluate", {"expression": js, "returnByValue": True}, session=self.sid, cb=got)

    def on_frame(self, p):
        self.c.send("Page.screencastFrameAck", {"sessionId": p["sessionId"]}, session=self.sid)
        self.pending_img = p["data"]      # só o mais novo importa
        self.pump()

    def pump(self):
        """manda o quadro pendente se o FOSTON já aplicou o anterior e respeitando o ritmo"""
        # dois quadros em trânsito: o PC prepara e o cabo leva o próximo enquanto o FOSTON
        # ainda aplica o anterior
        if self.pending_img is None or self.inflight >= 2 or time.time() - self.last_sent < MIN_FRAME_S:
            return
        data, self.pending_img = self.pending_img, None
        if self.send_frame(data):
            self.inflight += 1
            self.last_sent = time.time()

    def send_frame(self, data):
        t0 = time.time()
        k = self.scale if "half" in self.caps and LZ4 else 1
        W, H = self.w // k, self.h // k
        img = Image.open(io.BytesIO(base64.b64decode(data))).convert("RGB")
        if img.size != (W, H):
            if k > 1 and img.width > W:          # o Chrome ainda manda no tamanho inteiro
                img = img.resize((img.width // k, img.height // k), Image.BILINEAR)
            canvas = Image.new("RGB", (W, H), (255, 255, 255))
            canvas.paste(img.crop((0, 0, min(img.width, W), min(img.height, H))), (0, 0))
            img = canvas
        a = np.asarray(img, dtype=np.uint16)
        cur = ((a[:, :, 0] >> 3) << 11) | ((a[:, :, 1] >> 2) << 5) | (a[:, :, 2] >> 3)
        cur = cur.astype("<u2")
        if self.prev is None or self.prev.shape != cur.shape:
            changed = np.ones(((H + TILE - 1) // TILE, (W + TILE - 1) // TILE), bool)
        else:
            diff = cur != self.prev
            ph, pw = -H % TILE, -W % TILE
            d = np.pad(diff, ((0, ph), (0, pw)))
            changed = d.reshape(d.shape[0] // TILE, TILE, d.shape[1] // TILE, TILE).any(axis=(1, 3))
        self.prev = cur
        rows, cols = np.nonzero(changed)
        if not len(rows):
            return False
        # trechos contínuos de cada fileira de blocos; trechos iguais em fileiras seguidas viram
        # um retângulo só (vídeo: poucos retângulos grandes, menos cabeçalhos, LZ4 melhor)
        rects = []                               # [x0, x1, y0, y1] em blocos
        open_by_span = {}
        for ty in np.unique(rows):
            xs = cols[rows == ty]
            runs, start = [], xs[0]
            for i in range(1, len(xs) + 1):
                if i == len(xs) or xs[i] != xs[i - 1] + 1:
                    runs.append((int(start), int(xs[i - 1]) + 1))
                    if i < len(xs):
                        start = xs[i]
            nxt = {}
            for span in runs:
                r = open_by_span.get(span)
                if r is not None and r[3] == ty:
                    r[3] = ty + 1
                else:
                    r = [span[0], span[1], int(ty), int(ty) + 1]
                    rects.append(r)
                nxt[span] = r
            open_by_span = nxt
        kind = b"H" if k > 1 else b"L" if LZ4 and "lz4" in self.caps else b"T"
        nbytes = 0
        for bx0, bx1, by0, by1 in rects:
            x0, y0 = bx0 * TILE, by0 * TILE
            x1, y1 = min(W, bx1 * TILE), min(H, by1 * TILE)
            # até ~240 KB crus por mensagem (o FOSTON aceita até 1 MB)
            step = max(1, 122880 // max(1, x1 - x0))
            for ys in range(y0, y1, step):
                ye = min(y1, ys + step)
                block = np.ascontiguousarray(cur[ys:ye, x0:x1]).tobytes()
                z = LZ4(block) if kind != b"T" else zlib.compress(block, 1)
                msg = struct.pack("<HHHH", x0, ys, x1 - x0, ye - ys) + z
                self.f.send(kind, msg)
                nbytes += len(msg)
        self.f.send(b"P", b"")
        self.st_n += 1
        self.st_b += nbytes
        self.st_ms += (time.time() - t0) * 1000
        if VERBOSE and time.time() - self.st_t >= 5:
            dt = time.time() - self.st_t
            log(f"imagem: {self.st_n / dt:.1f} quadros/s, {self.st_b / max(1, self.st_n) / 1024:.0f} KB/quadro, "
                f"{self.st_ms / max(1, self.st_n):.0f} ms no PC, {kind.decode()} {W}x{H}")
            self.st_t, self.st_n, self.st_b, self.st_ms = time.time(), 0, 0, 0.0
        if time.time() - self.last_scroll_q > 0.3:
            self.last_scroll_q = time.time()
            self.scroll_state()
        return True

    def audio_start(self):
        if not self.c.audio:
            return
        try:
            self.arec = subprocess.Popen(
                ["parec", f"--device={SINK}.monitor", "--format=s16le", f"--rate={AUDIO_RATE}",
                 f"--channels={AUDIO_CH}", "--raw", "--latency-msec=20", "--client-name=BigFoston Web"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, preexec_fn=die_with_parent)
            os.set_blocking(self.arec.stdout.fileno(), False)
        except OSError:
            self.arec = None

    def audio_read(self):
        try:
            d = os.read(self.arec.stdout.fileno(), 1 << 16)
        except BlockingIOError:
            return
        if not d:                            # parec saiu (servidor de som reiniciado)
            self.audio_stop()
            return
        self.abuf += d
        while len(self.abuf) >= AUDIO_CHUNK:
            chunk, self.abuf = self.abuf[:AUDIO_CHUNK], self.abuf[AUDIO_CHUNK:]
            now = time.time()
            if np.abs(np.frombuffer(chunk, "<i2").astype(np.int32)).max() > 16:
                if VERBOSE and now - self.a_loud >= AUDIO_HANG_S:
                    log("som: tocando")
                self.a_loud = now
            if now - self.a_loud < AUDIO_HANG_S and self.asock:   # silêncio longo não ocupa o cabo
                try:
                    self.asock.sendall(chunk)
                except OSError as e:
                    log(f"som: conexão perdida ({e})")
                    self.asock.close()
                    self.asock = None

    def audio_connect(self, token):
        """o FOSTON mandou o código de uso único da conexão de som"""
        if not self.arec or not re.fullmatch(rb"[0-9a-f]{32}", token):
            return
        try:
            s = socket.create_connection((self.f.s.getpeername()[0], AUDIO_PORT), timeout=3)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(2)                  # FOSTON travado: perde o som, não a imagem
            s.sendall(token + b"\n")
        except OSError as e:
            log(f"som: sem conexão ({e})")
            return
        if self.asock:
            self.asock.close()
        self.asock = s

    def audio_stop(self):
        if self.asock:
            self.asock.close()
            self.asock = None
        if self.arec:
            try:
                self.arec.terminate()
                self.arec.wait(2)
            except (OSError, subprocess.TimeoutExpired):
                self.arec.kill()
            self.arec = None

    def close(self):
        self.audio_stop()

    def set_title(self, title, url):
        title = " ".join(title.split())[:300]
        if not title or title == self.title:
            return
        # sem <title> o Chrome usa o endereço (com ou sem "https://" e "www."): não é título
        bare = url.split("://", 1)[-1]
        tt = title.rstrip("/").rstrip(".")
        if title.startswith(("http://", "https://")) or bare.startswith(tt) \
                or bare.startswith("www.") and bare[4:].startswith(tt):
            return
        self.title = title
        self.f.event(f"TITLE {title}")

    def on_event(self, m):
        if "id" in m:
            cb = self.c.pending.pop(m["id"], None)
            if cb:
                cb(m)
            return
        if m.get("sessionId", self.sid) != self.sid:
            return                       # resto de uma sessão antiga
        meth, p = m.get("method", ""), m.get("params", {})
        if meth == "Page.screencastFrame":
            self.on_frame(p)
        elif meth == "Page.frameStartedLoading" and p.get("frameId") == self.tid:
            self.loading = True
            self.f.event("LOAD 1")
        elif meth == "Page.frameStoppedLoading" and p.get("frameId") == self.tid:
            self.loading = False
            self.f.event("LOAD 0")
            self.nav_state()
            self.scroll_state()
        elif meth == "Page.frameNavigated" and not p.get("frame", {}).get("parentId"):
            url = p["frame"].get("url", "")
            if url.startswith("chrome-error://"):
                self.f.event("ERRPAGE fetcherror")
            else:
                self.url = url
                self.title = ""                  # documento novo: o título vem de novo
                if any(self.video.values()):     # a página com o vídeo saiu
                    self.f.event("VIDEO 0")
                self.video.clear()
                if self.full:                    # a página nova não está em tela cheia
                    self.full = False
                    self.f.event("FULL 0")
                self.f.event(f"URL {url}")
            self.f.event("CARET 0")
            self.nav_state()
        elif meth == "Page.navigatedWithinDocument":
            self.url = p.get("url", self.url)
            self.f.event(f"URL {self.url}")
            self.nav_state()
        elif meth == "Target.targetInfoChanged":
            t = p.get("targetInfo", {})
            if t.get("targetId") == self.tid:
                self.set_title(t.get("title", ""), t.get("url", self.url))
        elif meth == "Target.targetCreated":
            t = p.get("targetInfo", {})
            if t.get("type") == "page" and t.get("targetId") != self.tid:
                # janela nova (popup): abre na mesma aba
                if t.get("url") and t["url"] != "about:blank":
                    self.go(t["url"])
                self.c.send("Target.closeTarget", {"targetId": t["targetId"]})
        elif meth == "Runtime.bindingCalled" and p.get("name") == "bfVideo":
            fid, _, on = p.get("payload", "").partition(" ")
            was = any(self.video.values())
            self.video[fid[:16]] = on == "1"
            if any(self.video.values()) != was:
                self.f.event("VIDEO 1" if not was else "VIDEO 0")
        elif meth == "Runtime.bindingCalled" and p.get("name") == "bfFull":
            self.full = p.get("payload") == "1"
            self.f.event("FULL 1" if self.full else "FULL 0")
        elif meth == "Runtime.bindingCalled" and p.get("name") == "bfTitle":
            self.set_title(p.get("payload", ""), self.url)
        elif meth == "Runtime.bindingCalled" and p.get("name") == "bfCaret":
            v = p.get("payload", "0")
            if v == "0":
                self.f.event("CARET 0")
            else:
                x, y, h = json.loads(v)
                z = self.zoom / 100
                self.f.event(f"CARET 1 {round(x * z)} {round(y * z)} {round(h * z)} new")
        elif meth == "Security.visibleSecurityStateChanged":
            st = p.get("visibleSecurityState", {}).get("securityState", "")
            if not self.url.startswith(("http://", "https://")):
                self.f.event("SEC 0")          # about:, data: — nem cadeado nem aviso
            else:
                self.f.event("SEC " + {"secure": "6", "insecure": "3", "neutral": "3"}.get(st, "5"))
        elif meth == "Browser.downloadWillBegin":
            # o Chrome baixa (com a sessão/cookies dele) e o arquivo vai pelo cabo para o cartão
            guid = p.get("guid")
            d = {"id": self.next_dl, "name": p.get("suggestedFilename") or "download", "url": p.get("url", ""),
                 "file": None, "sent": 0}
            self.next_dl += 1
            self.dls[guid] = d
            name = d["name"].replace("\t", " ").replace("\n", " ")
            self.f.send(b"F", f"{d['id']}\t{name}\t0\t{d['url']}".encode())
        elif meth == "Browser.downloadProgress":
            guid, st = p.get("guid"), p.get("state")
            if guid in self.dls and st == "completed" and self.dls[guid]["file"] is None:
                try:
                    self.dls[guid]["file"] = open(os.path.join(self.dl_dir, guid), "rb")
                    self.dl_chunk(guid)
                except OSError:
                    self.dl_finish(guid)
            elif guid in self.dls and st == "canceled":
                self.dl_finish(guid)

    DL_CHUNK = 128 * 1024

    def dl_chunk(self, guid):
        """manda o próximo bloco (o FOSTON confirma com q antes do seguinte)"""
        d = self.dls.get(guid)
        if not d or not d["file"]:
            return
        data = d["file"].read(self.DL_CHUNK)
        if data:
            d["sent"] += len(data)
            self.f.send(b"G", struct.pack("<I", d["id"]) + data)
        else:
            self.dl_finish(guid)

    def dl_finish(self, guid, notify=True):
        d = self.dls.pop(guid, None)
        if not d:
            return
        if d["file"]:
            d["file"].close()
        if notify:
            size = d["sent"] if d["file"] else -1
            self.f.send(b"Z", f"{d['id']}\t{size}".encode())
        try:
            os.unlink(os.path.join(self.dl_dir, guid))
        except OSError:
            pass

    def run(self):
        sock = self.f.s
        while True:
            fds = [self.c.rfd, sock] + ([self.arec.stdout] if self.arec else [])
            r, _, _ = select.select(fds, [], [], MIN_FRAME_S if self.pending_img else 1.0)
            if self.arec and self.arec.stdout in r:
                self.audio_read()
            if sock in r:
                for t, payload in self.f.read():
                    if t == b"K":
                        line = payload.decode(errors="replace")
                        if VERBOSE:
                            log("cmd", line[:120])
                        self.command(line)
                    elif t == b"U":                  # código da conexão de som
                        self.audio_connect(payload)
                    elif t == b"Q":                  # o FOSTON aplicou o quadro
                        self.inflight = max(0, self.inflight - 1)
                    elif t == b"C":                  # capacidades (FOSTON novo)
                        self.caps = set(payload.decode(errors="replace").split())
                        log("imagem: " + ("LZ4" if LZ4 and "lz4" in self.caps else "zlib"))
                    elif t == b"q" and len(payload) == 4:   # bloco de download gravado
                        pid = struct.unpack("<I", payload)[0]
                        for guid, d in list(self.dls.items()):
                            if d["id"] == pid:
                                self.dl_chunk(guid)
            if self.c.rfd in r:
                msgs = self.c.read(0)
                # muitos quadros parados na fila: só o último importa
                frames = [m for m in msgs if m.get("method") == "Page.screencastFrame"
                          and m.get("sessionId", self.sid) == self.sid]
                for m in frames[:-1]:
                    self.c.send("Page.screencastFrameAck", {"sessionId": m["params"]["sessionId"]}, session=self.sid)
                for m in msgs:
                    if m.get("method") == "Page.screencastFrame" and m is not (frames[-1] if frames else None):
                        continue
                    self.on_event(m)
            while self.c.events:
                self.on_event(self.c.events.pop(0))
            self.pump()


def load_key():
    os.makedirs(CONF, exist_ok=True)
    p = os.path.join(CONF, "key")
    if not os.path.exists(p):
        with open(os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as f:
            f.write(secrets.token_hex(16))
    return open(p).read().strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="192.168.7.2")
    ap.add_argument("--browser")
    ap.add_argument("--once", action="store_true", help="sai quando a ligação cair (testes)")
    ap.add_argument("--verbose", action="store_true", help="mostra os comandos do FOSTON")
    a = ap.parse_args()
    global VERBOSE
    VERBOSE = a.verbose
    # uma instância por usuário (autostart do pacote e um iniciado à mão não brigam)
    os.makedirs(os.path.expanduser("~/.cache"), exist_ok=True)
    lock = open(os.path.expanduser("~/.cache/bigfoston-web-pc.lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit("bfweb-pc: já está rodando")
    exe = find_browser(a.browser)
    key = load_key()
    name = socket.gethostname()[:40]
    log(f"navegador: {exe}; esperando o FOSTON em {a.host}:{PORT}")
    # kill (SIGTERM) e fim de sessão (SIGHUP) passam pelo finally abaixo, como o Ctrl+C
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: sys.exit(0))
    try:
        serve(a, exe, key, name)
    except KeyboardInterrupt:
        pass
    finally:
        if getattr(serve, "chrome", None):
            serve.chrome.close()
        if getattr(serve, "sink", None):
            serve.sink.close()


def serve(a, exe, key, name):
    serve.chrome = chrome = None
    serve.sink = None
    idle_since = time.time()
    while True:
        try:
            f = Foston(a.host, key, name)
        except OSError:
            # sem o FOSTON por 2 minutos: fecha o Chrome (a RAM do PC volta)
            if chrome and time.time() - idle_since > 120:
                chrome.close()
                serve.chrome = chrome = None
                log("Chrome fechado (FOSTON ausente)")
            time.sleep(2)
            continue
        try:
            # o FOSTON responde "A ok L A" (autorizado, largura e altura) ou "A no"
            ans, atoken, caps = None, None, []
            end = time.time() + 120               # tempo para tocar em Permitir na tela
            while ans is None and time.time() < end:
                r, _, _ = select.select([f.s], [], [], 1)
                if r:
                    for t, p in f.read():
                        if t == b"A":
                            ans = p.decode().split()
                        elif t == b"U":           # vem logo depois do A, às vezes no mesmo recv
                            atoken = p
                        elif t == b"C":
                            caps = p.decode(errors="replace").split()
            if not ans or ans[0] != "ok":
                log("o FOSTON recusou este PC" if ans else "sem resposta do FOSTON")
                f.s.close()
                time.sleep(10)
                continue
            w, h = int(ans[1]), int(ans[2])
            start = ans[3] if len(ans) > 3 else ""
            log(f"FOSTON ligado ({w}x{h})")
            if chrome is None or chrome.p.poll() is not None:
                if chrome:                       # o Chrome caiu: fecha a tela virtual dele também
                    chrome.close()
                if serve.sink is None:
                    serve.sink = AudioSink()
                    log("som: " + ("saída virtual BigFoston-Web" if serve.sink.ok
                                   else "sem pactl/parec, Chrome mudo"))
                serve.chrome = chrome = Chrome(exe, w, h, audio=serve.sink.ok)
            sess = None
            try:
                sess = Session(chrome, f, w, h, start, caps)
                log("imagem: " + ("LZ4" if LZ4 and "lz4" in caps else "zlib") +
                    (", vídeo em tela cheia em meia resolução" if LZ4 and "half" in caps else ""))
                if atoken:
                    sess.audio_connect(atoken)
                sess.run()
            finally:
                if sess:
                    sess.close()
        except (EOFError, OSError, ValueError, TimeoutError, RuntimeError) as e:
            log(f"ligação encerrada: {e}")
        finally:
            idle_since = time.time()
            try:
                f.s.close()
            except OSError:
                pass
        if a.once:
            break
        time.sleep(1)


if __name__ == "__main__":
    main()
