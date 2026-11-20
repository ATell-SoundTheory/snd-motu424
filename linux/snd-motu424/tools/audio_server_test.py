#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
PipeWire and JACK compatibility test for snd-motu424.

Runs on the machine with the card and checks what a sound server needs
from the driver:

  env       versions, module, card layout
  alsa      constraint space and position granularity (tools/pcm_caps.c)
  pipewire  pro-audio profile: node formats, capture and playback at
            several quanta and rates, xruns, errors in the journal
  pwjack    JACK clients through PipeWire (pw-jack)
  jack      jackd with the ALSA backend: period sizes, period counts,
            channel subsets, rates, xruns, shutdown

Everything it plays is digital silence, except with --loopback: then a
-40 dBFS 997 Hz tone goes to one output only (default 24I/O output 19,
zero-based 18), the matching input is recorded, and the recording is
checked for dropouts. Keep monitors muted.

The PipeWire profile, forced quantum and forced rate are restored at the
end. jackd needs the card to itself, so the jack phase switches the
PipeWire profile off for its duration.

  python3 tools/audio_server_test.py [--phases env,alsa,pipewire,pwjack,jack]
         [--seconds 10] [--loopback] [--out 18 --in 18] [--log DIR]
"""
import argparse
import json
import math
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import wave

TOOLS = os.path.dirname(os.path.abspath(__file__))


class Report:
    def __init__(self, logdir):
        self.rows = []
        self.logdir = logdir
        self.log = open(os.path.join(logdir, "test.log"), "w")

    def note(self, text):
        print(text, flush=True)
        self.log.write(text + "\n")
        self.log.flush()

    def add(self, status, name, detail=""):
        self.rows.append((status, name, detail))
        self.note(f"{status:4} {name}: {detail}")

    def save(self, name, text):
        with open(os.path.join(self.logdir, name), "w") as f:
            f.write(text)

    def summary(self):
        self.note("\n==== summary ====")
        for status, name, detail in self.rows:
            self.note(f"{status:4} {name}: {detail}")
        counts = {s: sum(1 for r in self.rows if r[0] == s)
                  for s in ("PASS", "WARN", "FAIL", "SKIP")}
        self.note(" ".join(f"{k} {v}" for k, v in counts.items()))
        self.note(f"logs: {self.logdir}")
        return counts["FAIL"]


def run(cmd, timeout=30, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, timeout=timeout,
                              text=True, errors="replace", **kw)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return subprocess.CompletedProcess(cmd, -999, out, "timeout")
    except FileNotFoundError:
        return subprocess.CompletedProcess(cmd, -998, "", "not found")


def have(tool):
    return shutil.which(tool) is not None


def journal(since, user=True, kernel=False):
    cmd = ["journalctl", "--no-pager", "-o", "short-precise",
           "--since", f"@{since:.3f}"]
    if kernel:
        cmd.append("-k")
    elif user:
        cmd += ["--user", "-u", "pipewire", "-u", "wireplumber"]
    return run(cmd).stdout


def proc_card(card_id, name):
    try:
        with open(f"/proc/asound/{card_id}/{name}") as f:
            return f.read()
    except OSError:
        return ""


def hw_ptr(card_id, d):
    """(monotonic time, hw_ptr) of a running substream, or None."""
    status = proc_card(card_id, f"pcm0{d}/sub0/status")
    m = re.search(r"hw_ptr\s*:\s*(\d+)", status)
    if "RUNNING" not in status or not m:
        return None
    return time.monotonic(), int(m.group(1))


def wait_running(card_id, d, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        s = hw_ptr(card_id, d)
        if s:
            return s
        time.sleep(0.02)
    return None


def stop_proc(p, sig=signal.SIGINT, wait=5):
    """Stop a process; True if it exited by itself after the signal."""
    if p.poll() is not None:
        return True
    p.send_signal(sig)
    try:
        p.wait(wait)
        return True
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        return False


# ------------------------------------------------------------------ audio files

def write_wav(path, rate, seconds, channels=1, tone_ch=None, amp=0.0, freq=997.0):
    """S32 WAV; silence, or a sine on tone_ch at amp (peak, full scale 1)."""
    frames = int(rate * seconds)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(4)
        w.setframerate(rate)
        chunk = 4800
        silent = bytes(4 * channels * chunk)
        for start in range(0, frames, chunk):
            n = min(chunk, frames - start)
            if tone_ch is None:
                w.writeframes(silent[:4 * channels * n])
                continue
            buf = bytearray(4 * channels * n)
            for i in range(n):
                v = int(amp * 2147483647 * math.sin(2 * math.pi * freq * (start + i) / rate))
                struct.pack_into("<i", buf, 4 * (i * channels + tone_ch), v)
            w.writeframes(bytes(buf))


def read_mono(path):
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        data = w.readframes(n)
    if width != 4:
        raise ValueError(f"{path}: sample width {width}")
    vals = struct.unpack(f"<{len(data) // 4}i", data)
    return rate, [vals[i] / 2147483648.0 for i in range(0, len(vals), ch)]


def tone_dropouts(rate, samples, block_ms=5):
    """Level of a steady tone per block: (median dBFS, bad blocks, blocks)."""
    block = int(rate * block_ms / 1000)
    skip = rate // 2  # settle at both ends
    body = samples[skip:len(samples) - skip]
    levels = []
    for i in range(0, len(body) - block, block):
        seg = body[i:i + block]
        rms = math.sqrt(sum(x * x for x in seg) / block)
        levels.append(20 * math.log10(rms) if rms > 0 else -200.0)
    if not levels:
        return -200.0, 0, 0
    med = sorted(levels)[len(levels) // 2]
    bad = sum(1 for lv in levels if abs(lv - med) > 3.0)
    return med, bad, len(levels)


# ------------------------------------------------------------------ env / alsa

def phase_env(rep, args):
    rep.note("\n## env")
    info = [run(["uname", "-r"]).stdout.strip()]
    for tool, flag in (("pipewire", "--version"), ("wireplumber", "--version"),
                       ("jackd", "--version")):
        if have(tool):
            info.append(" ".join(run([tool, flag]).stdout.split()[:6]))
    try:
        with open("/sys/module/snd_motu424/srcversion") as f:
            info.append("snd_motu424 " + f.read().strip())
    except OSError:
        rep.add("FAIL", "env.module", "snd_motu424 not loaded")
    rep.save("env.txt", "\n".join(info) + "\n\n" + proc_card(args.card, "motu424"))
    rep.note("\n".join(info))
    rep.note(proc_card(args.card, "motu424"))


def build_tool(rep, name, libs):
    src = os.path.join(TOOLS, name + ".c")
    exe = os.path.join(tempfile.gettempdir(), "motu424-" + name)
    if os.path.exists(exe) and os.path.getmtime(exe) > os.path.getmtime(src):
        return exe
    r = run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", src, "-o", exe] + libs, timeout=120)
    if r.returncode:
        rep.add("FAIL", f"build.{name}", r.stderr.strip()[-300:])
        return None
    return exe


def phase_alsa(rep, args):
    rep.note("\n## alsa")
    caps = build_tool(rep, "pcm_caps", ["-lasound"])
    if not caps:
        return
    r = run([caps, args.device], timeout=60)
    rep.save("pcm_caps.txt", r.stdout + r.stderr)
    rep.note(r.stdout.strip())
    out = r.stdout

    m = re.search(r"capture 48000: channels (\d+)-(\d+).* batch (\d)", out)
    if m:
        rep.add("PASS" if m.group(3) == "1" else "WARN", "alsa.batch-flag",
                f"batch={m.group(3)} (position moves per interrupt, PipeWire "
                "needs BATCH to add headroom)")
    sect = out.split("capture 48000:")[1].split("\nplayback")[0] if "capture 48000:" in out else ""
    for label, pat, want in (
            ("alsa.channels-64", r" 64:(\w+)", "yes"),
            ("alsa.channels-2", r" 2:(\w+)", "yes"),
            ("alsa.periods-2", r"periods accepted: 2:(\w+)", "yes"),
            ("alsa.period-128", r" 128:(\w+)", "yes"),
            ("alsa.period-512", r" 512:(\w+)", "yes")):
        mm = re.search(pat, sect)
        if mm:
            rep.add("PASS" if mm.group(1) == want else "WARN", label,
                    f"accepted={mm.group(1)}")
    mm = re.search(r"buffer near\(max\)=(\d+)", sect)
    if mm:
        frames = int(mm.group(1))
        rep.add("PASS" if frames >= 8192 else "WARN", "alsa.buffer-max",
                f"{frames} frames at full channel count")
    rates = {}
    for mm in re.finditer(r"capture (\d+): channels (\d+)-(\d+)", out):
        rates[int(mm.group(1))] = (int(mm.group(2)), int(mm.group(3)))
    maxes = {v[1] for v in rates.values()}
    rep.add("PASS" if len(maxes) <= 1 else "WARN", "alsa.channels-across-rates",
            ", ".join(f"{k}:{v[0]}-{v[1]}" for k, v in sorted(rates.items())) +
            ("" if len(maxes) <= 1 else " (channel count changes with the rate)"))


# ------------------------------------------------------------------ pipewire

class PipeWire:
    def __init__(self, rep, args):
        self.rep, self.args = rep, args
        self.card_num = os.path.basename(os.path.realpath(f"/proc/asound/{args.card}")).replace("card", "")
        self.dev = None
        self.saved_profile = None

    def dump(self):
        r = run(["pw-dump"], timeout=20)
        try:
            return json.loads(r.stdout)
        except ValueError:
            return []

    def device(self):
        for o in self.dump():
            if not o.get("type", "").endswith("Device"):
                continue
            p = (o.get("info") or {}).get("props") or {}
            if str(p.get("api.alsa.card")) == self.card_num and p.get("device.api") == "alsa":
                return o
        return None

    def nodes(self):
        res = {}
        for o in self.dump():
            if not o.get("type", "").endswith("Node"):
                continue
            info = o.get("info") or {}
            p = info.get("props") or {}
            if str(p.get("device.id")) != str(self.dev):
                continue
            fmt = (info.get("params") or {}).get("EnumFormat") or [{}]
            res[p.get("media.class")] = {
                "id": o["id"], "name": p.get("node.name"),
                "state": info.get("state"), "error": info.get("error"),
                "channels": fmt[0].get("channels") if fmt else None,
                "ports": info.get("n-input-ports", 0) + info.get("n-output-ports", 0),
            }
        return res

    def set_profile(self, name):
        dev = self.device()
        for p in (dev["info"]["params"].get("EnumProfile") or []):
            if p.get("name") == name:
                run(["wpctl", "set-profile", str(self.dev), str(p["index"])])
                time.sleep(3)
                return True
        return False

    def active_profile(self):
        dev = self.device()
        prof = (dev["info"]["params"].get("Profile") or [{}]) if dev else [{}]
        return prof[0].get("name")

    def setting(self, key, value):
        run(["pw-metadata", "-n", "settings", "0", key, str(value)])

    def wait_suspended(self, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            if all(n["state"] in ("suspended", "error") for n in self.nodes().values()):
                return True
            time.sleep(0.5)
        return False

    def start(self):
        dev = self.device()
        if not dev:
            self.rep.add("FAIL", "pw.device", f"no PipeWire device for ALSA card {self.card_num}")
            return False
        self.dev = dev["id"]
        self.saved_profile = self.active_profile()
        if not self.set_profile("pro-audio"):
            self.rep.add("FAIL", "pw.profile", "no pro-audio profile")
            return False
        return True

    def restore(self):
        self.setting("clock.force-quantum", 0)
        self.setting("clock.force-rate", 0)
        if self.dev is not None and self.saved_profile:
            self.set_profile(self.saved_profile)


def pw_top_errors(names, seconds=2):
    """ERR column of pw-top for the given node names (cumulative)."""
    r = run(["pw-top", "-b", "-n", str(seconds + 1)], timeout=seconds + 10)
    errs, quant = {}, {}
    for line in r.stdout.splitlines():
        cols = line.split()
        if len(cols) >= 9 and cols[-1] in names:
            try:
                errs[cols[-1]] = int(cols[8])
                quant[cols[-1]] = cols[2] + "@" + cols[3]
            except ValueError:
                pass
    return errs, quant


def pw_run(rep, pw, label, seconds, rate=None, quantum=None, tmp="/tmp"):
    """Silent capture + playback through the pro-audio nodes."""
    if quantum:
        pw.setting("clock.force-quantum", quantum)
    if rate:
        pw.setting("clock.force-rate", rate)
    pw.wait_suspended()
    nodes = pw.nodes()
    src, sink = nodes.get("Audio/Source"), nodes.get("Audio/Sink")
    if not src or not sink:
        rep.add("FAIL", label, f"nodes missing: {list(nodes)}")
        return
    ch = src["channels"]
    ch = ch.get("default") if isinstance(ch, dict) else ch
    r = rate or 48000
    silence = os.path.join(tmp, "motu424-silence.wav")
    write_wav(silence, r, seconds + 4)
    cap = os.path.join(tmp, "motu424-pwcap.wav")
    t0 = time.time()
    m0 = time.monotonic()
    rec = subprocess.Popen(["pw-record", "--target", src["name"], "--rate", str(r),
                            "--channels", str(ch), "--format", "s32", cap],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    play = subprocess.Popen(["pw-play", "--target", sink["name"], silence],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    # startup includes the driver's AudioWire settle after a multiplier change
    started = wait_running(pw.args.card, "c")
    startup = started[0] - m0 if started else None
    time.sleep(1)
    errs0, _ = pw_top_errors({src["name"], sink["name"]}, 1)
    hwp = {d: proc_card(pw.args.card, f"pcm0{d}/sub0/hw_params") for d in "pc"}
    pos0 = {d: hw_ptr(pw.args.card, d) for d in "pc"}
    time.sleep(max(seconds - 4, 1))
    pos1 = {d: hw_ptr(pw.args.card, d) for d in "pc"}
    errs1, quant = pw_top_errors({src["name"], sink["name"]}, 1)
    stop_proc(play)
    stop_proc(rec)
    m1 = time.monotonic()
    log = journal(t0)
    nodes = pw.nodes()
    rep.save(f"{label}.journal.txt", log)
    rep.save(f"{label}.hw_params.txt", "playback\n" + hwp["p"] + "\ncapture\n" + hwp["c"])

    frames = 0
    try:
        with wave.open(cap) as w:
            frames = w.getnframes()
            wch = w.getnchannels()
    except (OSError, EOFError, wave.Error):
        wch = 0
    # the file should hold the time the stream ran, less a little for
    # pw-record's own start and drain
    expect = int(r * ((m1 - started[0]) - 0.5)) if started else r * (seconds - 2)
    # steady-state hardware position rate of each direction
    speed = {}
    for d in "pc":
        if pos0[d] and pos1[d] and pos1[d][0] > pos0[d][0]:
            speed[d] = (pos1[d][1] - pos0[d][1]) / (pos1[d][0] - pos0[d][0])
    speed_bad = [d for d in "pc" if d not in speed or abs(speed[d] / r - 1) > 0.01]
    xruns = sum(errs1.get(n, 0) - errs0.get(n, 0) for n in errs1)
    problems = [l for l in log.splitlines()
                if re.search(r"error|xrun|doesn't match|invalid|failed", l, re.I)]
    states = {k: (v["state"], v["error"]) for k, v in nodes.items()}
    period = re.search(r"period_size: (\d+)", hwp["c"] or hwp["p"])
    buf = re.search(r"buffer_size: (\d+)", hwp["c"] or hwp["p"])
    detail = (f"startup {startup:.2f} s, " if startup is not None else "never started, ")
    detail += ("hw_ptr " + ", ".join(f"{d} {speed[d]:.0f}/s" for d in sorted(speed)) + ", "
               if speed else "")
    detail += (f"captured {frames} frames x {wch} ch (expected >= {expect}), "
               f"pw-top {quant}, ERR +{xruns}, alsa period {period.group(1) if period else '-'}"
               f" buffer {buf.group(1) if buf else '-'}")
    if (any(s[0] == "error" for s in states.values()) or frames < expect
            or not started or speed_bad):
        rep.add("FAIL", label, detail + f"; states {states}; " + " | ".join(problems[-3:]))
    elif xruns or problems:
        rep.add("WARN", label, detail + "; " + " | ".join(problems[-3:]))
    else:
        rep.add("PASS", label, detail)
    for f in (cap, silence):
        if os.path.exists(f):
            os.unlink(f)


def pw_loopback(rep, pw, args, seconds, tmp="/tmp"):
    """-40 dBFS tone on one sink port, recorded from one source port."""
    pw.setting("clock.force-quantum", 0)
    pw.setting("clock.force-rate", 0)
    pw.wait_suspended()
    nodes = pw.nodes()
    src, sink = nodes.get("Audio/Source"), nodes.get("Audio/Sink")
    tone = os.path.join(tmp, "motu424-tone.wav")
    cap = os.path.join(tmp, "motu424-loop.wav")
    write_wav(tone, 48000, seconds + 3, 1, 0, 0.01)
    play = subprocess.Popen(["pw-play", "--target", "0", "-P", "{ node.name = motu424-test-tone }",
                             tone], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rec = subprocess.Popen(["pw-record", "--target", "0", "-P", "{ node.name = motu424-test-rec }",
                            "--channels", "1", "--format", "s32", "--rate", "48000", cap],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    out_port = f"{sink['name']}:playback_AUX{args.out}"
    in_port = f"{src['name']}:capture_AUX{args.inp}"
    links = []
    for a, b in ((in_port, "motu424-test-rec:input_MONO"),
                 ("motu424-test-tone:output_MONO", out_port)):
        r = run(["pw-link", a, b])
        links.append(f"{a} -> {b}: {'ok' if not r.returncode else r.stderr.strip()}")
    listing = run(["pw-link", "-l"]).stdout
    tone_links = [l for l in listing.splitlines() if "motu424-test-tone" in l or "playback_AUX" in l]
    rep.save("pw-loopback-links.txt", "\n".join(links) + "\n\n" + listing)
    time.sleep(seconds)
    stop_proc(play)
    stop_proc(rec)
    try:
        rate, samples = read_mono(cap)
        med, bad, blocks = tone_dropouts(rate, samples)
    except (OSError, EOFError, wave.Error, ValueError) as e:
        rep.add("FAIL", "pw.loopback", f"no recording: {e}; {links}")
        return
    finally:
        for f in (tone, cap):
            if os.path.exists(f):
                os.unlink(f)
    status = "PASS" if med > -60 and bad == 0 else "FAIL"
    rep.add(status, "pw.loopback",
            f"out {args.out + 1} -> in {args.inp + 1}: level {med:.1f} dBFS, "
            f"{bad}/{blocks} 5 ms blocks off by >3 dB; links: {'; '.join(links)}")


def phase_pipewire(rep, args):
    rep.note("\n## pipewire")
    if not have("pw-dump"):
        rep.add("SKIP", "pipewire", "pw-dump not installed")
        return
    pw = PipeWire(rep, args)
    t0 = time.time()
    try:
        if not pw.start():
            return
        nodes = pw.nodes()
        for cls, n in sorted(nodes.items()):
            rep.add("PASS" if n["state"] != "error" else "FAIL", f"pw.node.{cls}",
                    f"{n['name']} channels {n['channels']} ports {n['ports']} state {n['state']}")
        for q in args.quanta:
            pw_run(rep, pw, f"pw.quantum-{q}", args.seconds, quantum=q)
        for r in args.rates:
            pw_run(rep, pw, f"pw.rate-{r}", args.seconds, rate=r)
        if args.loopback:
            pw_loopback(rep, pw, args, args.seconds)
        if "pwjack" in args.phases:
            phase_pwjack(rep, args, pw)
        # the desktop profile ACP offers when the PCM takes two channels
        profiles = [p.get("name") for p in
                    (pw.device()["info"]["params"].get("EnumProfile") or [])]
        rep.note("profiles: " + ", ".join(str(p) for p in profiles))
        stereo = "output:stereo-fallback+input:stereo-fallback"
        if stereo in profiles and pw.set_profile(stereo):
            pw.setting("clock.force-quantum", 0)
            pw.setting("clock.force-rate", 0)
            pw_run(rep, pw, "pw.stereo-fallback", args.seconds)
        else:
            rep.add("WARN", "pw.stereo-fallback", "no stereo desktop profile")
    finally:
        pw.restore()
        rep.save("pipewire.journal.txt", journal(t0))
        rep.save("kernel.journal.txt", journal(t0, kernel=True))


def phase_pwjack(rep, args, pw):
    rep.note("\n## pw-jack")
    if not have("pw-jack"):
        rep.add("SKIP", "pwjack", "pw-jack not installed (package pipewire-jack)")
        return
    pw.setting("clock.force-quantum", 0)
    pw.setting("clock.force-rate", 0)
    nodes = pw.nodes()
    r = run(["pw-jack", "jack_lsp"], timeout=20)
    names = [n["name"] for n in nodes.values()]
    desc = run(["pw-cli", "i", str(nodes["Audio/Source"]["id"])]).stdout if "Audio/Source" in nodes else ""
    nick = re.search(r'node.description = "([^"]+)"', desc)
    ports = [l for l in r.stdout.splitlines() if nick and l.startswith(nick.group(1) + ":")]
    cap = sum(1 for p in ports if ":capture_" in p)
    play = sum(1 for p in ports if ":playback_" in p)
    rep.save("pwjack-ports.txt", r.stdout)
    rep.add("PASS" if cap and play else "FAIL", "pwjack.ports",
            f"{cap} capture, {play} playback ports for {nick.group(1) if nick else names}")
    b = run(["pw-jack", "jack_bufsize"], timeout=20)
    rep.add("PASS" if b.returncode == 0 else "WARN", "pwjack.bufsize", b.stdout.strip() or b.stderr.strip())


# ------------------------------------------------------------------ jack

def jack_case(rep, args, label, extra, seconds, expect_ok=True):
    log_path = os.path.join(rep.logdir, f"{label}.log")
    logf = open(log_path, "w")
    cmd = ["jackd", "-R", "-P", "70", "-t", "5000", "-d", "alsa", "-d", args.device] + extra
    p = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT)
    ready = run(["jack_wait", "-w", "-t", "8"], timeout=12)
    ok = "server is available" in ready.stdout or ready.returncode == 0
    if p.poll() is not None:
        ok = False
    cap = play = 0
    if ok:
        lsp = run(["jack_lsp"], timeout=10).stdout.splitlines()
        cap = sum(1 for l in lsp if l.startswith("system:capture_"))
        play = sum(1 for l in lsp if l.startswith("system:playback_"))
        time.sleep(seconds)
    alive = p.poll() is None
    clean = stop_proc(p, signal.SIGINT, 8)
    logf.close()
    text = open(log_path, errors="replace").read()
    xruns = len(re.findall(r"xrun", text, re.I))
    errs = [l for l in text.splitlines()
            if re.search(r"cannot|error|failed|timed out|time out", l, re.I)][:3]
    detail = (f"jackd {' '.join(extra)}: ports {cap}/{play}, xruns {xruns}, "
              f"{'ran' if alive else 'exited'}, {'stopped' if clean else 'HUNG (killed)'}"
              + (f"; {' | '.join(errs)}" if errs else ""))
    if not clean:
        rep.add("FAIL", label, detail)
    elif not expect_ok:
        rep.add("PASS" if not ok else "WARN", label, detail + " (refusal expected)")
    elif ok and alive and cap and play and not xruns:
        rep.add("PASS", label, detail)
    elif ok and alive and cap and play:
        rep.add("WARN", label, detail)
    else:
        rep.add("FAIL", label, detail)
    run(["sleep", "1"])


def phase_jack(rep, args):
    rep.note("\n## jack")
    if not have("jackd"):
        rep.add("SKIP", "jack", "jackd not installed (package jackd2)")
        return
    pw = None
    if have("pw-dump"):
        pw = PipeWire(rep, args)
        dev = pw.device()
        if dev:
            pw.dev = dev["id"]
            pw.saved_profile = pw.active_profile()
            if pw.saved_profile != "off":
                pw.set_profile("off")
    users = run(["fuser", "-v"] + [f"/dev/snd/pcmC{PipeWire(rep, args).card_num}D0{d}" for d in "pc"]).stderr
    if re.search(r"\d+\s+F", users):
        rep.add("FAIL", "jack.device-free", "PCM still in use:\n" + users)
        return
    s = args.seconds
    try:
        for period, n in ((256, 3), (1024, 3), (256, 2), (128, 2), (64, 2), (512, 2), (48, 2), (32, 3)):
            jack_case(rep, args, f"jack.p{period}-n{n}",
                      ["-r", "48000", "-p", str(period), "-n", str(n)], s)
        jack_case(rep, args, "jack.p256-n2-2ch", ["-r", "48000", "-p", "256", "-n", "2", "-i", "2", "-o", "2"], s)
        jack_case(rep, args, "jack.p256-n2-64ch", ["-r", "48000", "-p", "256", "-n", "2", "-i", "64", "-o", "64"], s)
        jack_case(rep, args, "jack.96k-p256-n2", ["-r", "96000", "-p", "256", "-n", "2"], s)
        jack_case(rep, args, "jack.p24-n2", ["-r", "48000", "-p", "24", "-n", "2"], 2, expect_ok=False)
        jack_case(rep, args, "jack.back-to-48k", ["-r", "48000", "-p", "256", "-n", "2"], s)
    finally:
        if pw and pw.saved_profile and pw.saved_profile != "off":
            pw.set_profile(pw.saved_profile)


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--card", default="M424", help="ALSA card id")
    ap.add_argument("--device", default="hw:M424,0")
    ap.add_argument("--phases", default="env,alsa,pipewire,pwjack,jack")
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--quanta", default="64,256,1024,2048")
    ap.add_argument("--rates", default="44100,96000,48000")
    ap.add_argument("--loopback", action="store_true",
                    help="send a -40 dBFS tone to --out and record --in")
    ap.add_argument("--out", type=int, default=18, help="zero-based output channel")
    ap.add_argument("--in", dest="inp", type=int, default=18, help="zero-based input channel")
    ap.add_argument("--log", default=None)
    args = ap.parse_args()
    args.phases = args.phases.split(",")
    args.quanta = [int(q) for q in args.quanta.split(",") if q]
    args.rates = [int(r) for r in args.rates.split(",") if r]
    logdir = args.log or tempfile.mkdtemp(prefix="motu424-servers-")
    os.makedirs(logdir, exist_ok=True)
    rep = Report(logdir)
    if "env" in args.phases:
        phase_env(rep, args)
    if "alsa" in args.phases:
        phase_alsa(rep, args)
    if "pipewire" in args.phases:
        phase_pipewire(rep, args)
    elif "pwjack" in args.phases:
        pw = PipeWire(rep, args)
        if pw.start():
            try:
                phase_pwjack(rep, args, pw)
            finally:
                pw.restore()
    if "jack" in args.phases:
        phase_jack(rep, args)
    return 1 if rep.summary() else 0


if __name__ == "__main__":
    sys.exit(main())
