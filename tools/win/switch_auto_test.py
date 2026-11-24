#!/usr/bin/env python3
"""Autonomous real-Switch test harness over Wi-Fi (ftpsrv + sys-botbase + log_network + nxlink).

Orchestrates one complete Switch run:
  1. Starts a non-blocking TCP listener on --pc-ip:--log-port for RexGlue's live log_network stream.
  2. Connects to ftpsrv (--ip:--ftp-port), snapshots existing log filenames, and uploads out/nx/sa2.nro
     + a generated test fnc-nx.toml.
  3. Connects to sys-botbase (--ip:--bot-port), closes any lingering app, opens full-RAM hbmenu
     (holding R + clicking A), launches fnc-nx (clicking A with nxlink fallback in --launch auto),
     and immediately calls detachController so input_script drives menus into a Fight Now match.
  4. Streams live.log while waiting for switch_auto_exit_s clean exit, optional pixelPeek grabs, or a
     hang/freeze (> freeze-timeout seconds without [renut] progress). On freeze, captures peek_frozen.jpg
     and force-closes via HOME -> X -> A + detachController.
  5. Pulls the newest logs/fnc-nx_*.log, logs/rex/rex_*.log, and captures/video_*.rgba + .txt via FTP
     and converts captured frames to PNGs for visual inspection.
"""

from __future__ import annotations

import argparse
import datetime as dt
import ftplib
import io
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[2]
NXLINK_EXE = Path(r"C:\devkitPro\tools\bin\nxlink.exe")

DEFAULT_INPUT_SCRIPT = "25:START,30:A,38:A,44:A,50:A,56:A,62:A,68:A,74:A"

BASE_TOML_SETTINGS: list[tuple[str, object]] = [
    ("asset_pack", False),
    ("asset_pack_build", False),
    ("sa2_renderer", "renut"),
    ("renut_gpu_backend", "deko3d"),
    ("sa2_render_height_handheld", 720),
    ("sa2_render_height_docked", 720),
    ("fnc_frame_rate", 30),
    ("fnc_simulation_rate", 60),
    ("sa2_vsync_relaxed", True),
    ("log_level", "info"),
    ("log_to_file", True),
    ("sa2_renut_setter_marks", False),
    ("sa2_renut_setter_marks_check", False),
    ("audio_xma_in_worker", 1),
    ("audio_switch_conceal_gaps", False),
    ("audio_switch_frames_in_queue", 8),
    ("fnc_audio_mixer_priority", 45),
    ("fnc_audio_job_priority", 46),
    ("fnc_audio_retry_silent_frames", False),
    ("sa2_io_ranges_min_kb", 16),
    ("sa2_io_windows_max", 32),
    ("sa2_io_window_kb", 512),
    ("sa2_cpu_enhancements", True),
    ("sa2_noncpu_enhancements", False),
    ("banjo_renut_store_cards", True),
    ("banjo_renut_store_cards_verify", False),
    ("banjo_deko3d_zcull", True),
    ("banjo_deko3d_depth_compression", True),
    ("banjo_deko3d_depth24", True),
    ("banjo_deko3d_depth_handover", True),
    ("banjo_deko3d_barrier_lean", True),
    ("banjo_deko3d_draw_resolves", True),
    ("banjo_deko3d_alpha_spec", True),
]


def parse_toml_scalar(raw: str) -> object:
    s = raw.strip()
    if s.lower() == "true":
        return True
    if s.lower() == "false":
        return False
    if (s.startswith('"') and s.endswith('"')) or (s.startswith("'") and s.endswith("'")):
        return s[1:-1]
    try:
        return int(s, 0)
    except ValueError:
        pass
    try:
        return float(s)
    except ValueError:
        return s


def format_toml_value(val: object) -> str:
    if isinstance(val, bool):
        return "true" if val else "false"
    if isinstance(val, (int, float)):
        return str(val)
    escaped = str(val).replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def build_test_toml(args: argparse.Namespace) -> str:
    cfg: dict[str, object] = dict(BASE_TOML_SETTINGS)
    profile_on = args.profile if args.profile is not None else (args.mode == "perf")
    cfg["log_network"] = f"{args.pc_ip}:{args.log_port}"
    cfg["switch_profile_stacks"] = bool(profile_on)
    cfg["switch_profile_report"] = bool(profile_on)
    cfg["switch_auto_exit_s"] = int(args.duration)
    if args.input_script:
        cfg["input_script"] = args.input_script

    if args.mode == "visual":
        cfg["sa2_video_s"] = int(args.video_at)
        cfg["sa2_video_frames"] = int(args.video_frames)
        cfg["sa2_video_every"] = int(args.video_every)
        cfg["sa2_video_half"] = bool(args.video_half)
    else:
        cfg["sa2_video_s"] = 0

    for item in args.extra_toml:
        if "=" not in item:
            raise ValueError(f"Invalid --extra-toml {item!r}; expected key=value")
        k, v = item.split("=", 1)
        cfg[k.strip()] = parse_toml_scalar(v)

    lines = [f"{k} = {format_toml_value(v)}" for k, v in cfg.items()]
    return "\n".join(lines) + "\n"


class LiveLogServer:
    """Non-blocking TCP listener receiving Switch log_network lines in real time."""

    def __init__(self, bind_ip: str, port: int, out_path: Path, echo: bool = True) -> None:
        self.bind_ip = bind_ip
        self.port = port
        self.out_path = out_path
        self.echo = echo
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self.connected = threading.Event()
        self.disconnected = threading.Event()
        self.clean_exit = threading.Event()
        self.line_count = 0
        self.last_any_log_ts: float | None = None
        self.first_renut_ts: float | None = None
        self.last_renut_ts: float | None = None
        self.lines: list[str] = []
        self._sock: socket.socket | None = None
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            srv.bind((self.bind_ip, self.port))
        except OSError:
            # Fall back to 0.0.0.0 if the specific local Wi-Fi IP isn't directly bindable yet.
            srv.bind(("0.0.0.0", self.port))
        srv.listen(4)
        srv.settimeout(0.5)
        self._sock = srv
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        assert self._sock is not None
        with open(self.out_path, "w", encoding="utf-8", buffering=1) as fp:
            while not self._stop.is_set():
                try:
                    conn, addr = self._sock.accept()
                except TimeoutError:
                    continue
                except OSError:
                    break
                print(f"[listener] Switch log connected from {addr[0]}:{addr[1]}")
                self.disconnected.clear()
                self.connected.set()
                now = time.monotonic()
                with self._lock:
                    if self.last_any_log_ts is None:
                        self.last_any_log_ts = now
                conn.settimeout(0.5)
                buf = ""
                eof = False
                try:
                    while not self._stop.is_set():
                        try:
                            chunk = conn.recv(4096)
                        except TimeoutError:
                            continue
                        except OSError:
                            break
                        if not chunk:
                            eof = True
                            break
                        buf += chunk.decode("utf-8", errors="replace")
                        while "\n" in buf:
                            line, buf = buf.split("\n", 1)
                            self._record_line(fp, line.rstrip("\r"))
                    if buf:
                        self._record_line(fp, buf.rstrip("\r"))
                finally:
                    if eof and self.line_count > 0:
                        self.disconnected.set()
                    try:
                        conn.close()
                    except OSError:
                        pass

    def _record_line(self, fp: io.TextIOWrapper, line: str) -> None:
        now = time.monotonic()
        fp.write(line + "\n")
        fp.flush()
        with self._lock:
            self.line_count += 1
            self.last_any_log_ts = now
            self.lines.append(line)
            if "[renut]" in line:
                if self.first_renut_ts is None:
                    self.first_renut_ts = now
                self.last_renut_ts = now
            if "Switch: exiting process" in line or "switch_auto_exit_s (" in line:
                self.clean_exit.set()
        if self.echo:
            print(f"[live] {line}")

    def snapshot_stats(self) -> tuple[int, float | None, float | None, float | None]:
        with self._lock:
            return self.line_count, self.last_any_log_ts, self.first_renut_ts, self.last_renut_ts

    def stop(self) -> None:
        self._stop.set()
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        if self._thread is not None:
            self._thread.join(timeout=2.0)


class SysBotbaseClient:
    """Minimal TCP client for olliz0r/sys-botbase (port 6000)."""

    def __init__(self, ip: str, port: int = 6000, timeout: float = 15.0) -> None:
        self.ip = ip
        self.port = port
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self._rx_buf = bytearray()

    def connect(self) -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(min(self.timeout, 6.0))
        s.connect((self.ip, self.port))
        s.settimeout(self.timeout)
        self.sock = s
        self._rx_buf.clear()

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def send_cmd(self, cmd: str) -> None:
        if self.sock is None:
            self.connect()
        assert self.sock is not None
        self.sock.sendall((cmd.rstrip("\r\n") + "\r\n").encode("ascii"))

    def _read_line(self, timeout_s: float | None = None) -> str:
        assert self.sock is not None
        old_to = self.sock.gettimeout()
        if timeout_s is not None:
            self.sock.settimeout(timeout_s)
        try:
            while True:
                idx = self._rx_buf.find(b"\n")
                if idx != -1:
                    line = bytes(self._rx_buf[:idx]).decode("utf-8", errors="replace").rstrip("\r")
                    del self._rx_buf[: idx + 1]
                    return line
                chunk = self.sock.recv(65536)
                if not chunk:
                    line = bytes(self._rx_buf).decode("utf-8", errors="replace").rstrip("\r")
                    self._rx_buf.clear()
                    return line
                self._rx_buf.extend(chunk)
        finally:
            self.sock.settimeout(old_to)

    def query_line(self, cmd: str, timeout_s: float = 10.0) -> str:
        self.send_cmd(cmd)
        return self._read_line(timeout_s=timeout_s).strip()

    def get_version(self) -> str:
        return self.query_line("getVersion")

    def get_title_id(self) -> str:
        return self.query_line("getTitleID")

    def click(self, button: str) -> None:
        self.send_cmd(f"click {button}")

    def press(self, button: str) -> None:
        self.send_cmd(f"press {button}")

    def release(self, button: str) -> None:
        self.send_cmd(f"release {button}")

    def detach_controller(self) -> None:
        self.send_cmd("detachController")

    def pixel_peek(self, out_jpg: Path) -> tuple[bool, str]:
        try:
            self.send_cmd("pixelPeek")
            line = self._read_line(timeout_s=20.0).strip()
        except Exception as exc:
            return False, f"socket error: {exc}"
        if not line:
            return False, "empty pixelPeek response"
        if line.startswith("capssc"):
            return False, line
        try:
            data = bytes.fromhex(line)
        except ValueError as exc:
            return False, f"invalid hex ({len(line)} chars): {exc}; prefix={line[:48]!r}"
        if not data.startswith(b"\xff\xd8"):
            return False, f"not JPEG header ({len(data)} bytes; head={data[:8].hex()})"
        out_jpg.parent.mkdir(parents=True, exist_ok=True)
        out_jpg.write_bytes(data)
        return True, f"saved {len(data)} bytes -> {out_jpg}"

    def force_close_app(self) -> bool:
        """Closes any foreground app via HOME -> X -> A + detachController (with L+R applet dismissal fallback)."""
        print("[bot] Force-closing foreground app (HOME -> X -> A)...")
        for attempt in range(2):
            if attempt == 1:
                print("[bot] Foreground title still open; trying L+R + A to dismiss any controller applet...")
                self.press("L")
                self.press("R")
                time.sleep(0.35)
                self.release("L")
                self.release("R")
                time.sleep(1.0)
                self.click("A")
                time.sleep(1.5)
            self.click("HOME")
            time.sleep(1.4)
            self.click("X")
            time.sleep(1.0)
            self.click("A")
            time.sleep(2.5)
            try:
                self.detach_controller()
            except Exception:
                pass
            tid = self.get_title_id()
            closed = tid in ("", "0000000000000000")
            print(f"[bot] After force-close attempt {attempt + 1} getTitleID={tid!r} (closed={closed})")
            if closed:
                return True
        return False

    def open_full_ram_hbmenu(self, run_dir: Path) -> None:
        """Closes any open app, grabs Home screenshot, holds R + A to enter full-RAM hbmenu."""
        tid = self.get_title_id()
        if tid not in ("", "0000000000000000"):
            print(f"[bot] Closing existing foreground title {tid} before launch...")
            self.force_close_app()

        # Return Home cursor to the top-left game tile (in case it sat on bottom icons like Settings/Album).
        self.click("B")
        time.sleep(0.35)
        self.click("HOME")
        time.sleep(0.6)

        ok, msg = self.pixel_peek(run_dir / "peek_home.jpg")
        print(f"[bot] peek_home: {msg}")

        # Keep R held well into title takeover so Atmosphere hbloader always samples it past the profile picker.
        print("[bot] Holding R + pressing A on selected tile for full-RAM hbmenu...")
        self.press("R")
        try:
            time.sleep(0.4)
            self.click("A")
            time.sleep(1.8)
            tid_now = self.get_title_id()
            if tid_now in ("", "0000000000000000"):
                print("[bot] Profile picker up (getTitleID empty); clicking A again while holding R...")
                self.click("A")
                time.sleep(2.0)
            # Keep R down long enough for hbloader to start under full-RAM title takeover.
            time.sleep(3.0)
        finally:
            self.release("R")

        time.sleep(1.5)
        tid_after = self.get_title_id()
        print(f"[bot] Foreground title ID after R+A: {tid_after!r}")
        ok, msg = self.pixel_peek(run_dir / "peek_hbmenu.jpg")
        print(f"[bot] peek_hbmenu: {msg}")


class SwitchFtpClient:
    """ftplib wrapper for ftpsrv (port 5000, anonymous)."""

    def __init__(self, ip: str, port: int = 5000, timeout: float = 30.0) -> None:
        self.ip = ip
        self.port = port
        self.timeout = timeout
        self.ftp = ftplib.FTP()
        self.base_dir = "/switch/fnc-nx"

    def connect(self) -> None:
        self.ftp.connect(self.ip, self.port, timeout=min(self.timeout, 6.0))
        if self.ftp.sock is not None:
            self.ftp.sock.settimeout(self.timeout)
        self.ftp.login("anonymous", "")
        for candidate in ("/switch/fnc-nx", "/sdmc:/switch/fnc-nx", "switch/fnc-nx"):
            try:
                self.ftp.cwd(candidate)
                self.base_dir = self.ftp.pwd().rstrip("/")
                return
            except ftplib.all_errors:
                continue
        raise RuntimeError("Could not locate /switch/fnc-nx directory on Switch FTP server")

    def close(self) -> None:
        try:
            self.ftp.quit()
        except Exception:
            try:
                self.ftp.close()
            except Exception:
                pass

    def _safe_nlst(self, remote_dir: str) -> list[str]:
        try:
            entries = self.ftp.nlst(remote_dir)
        except ftplib.all_errors:
            return []
        return [posix_basename(e) for e in entries if posix_basename(e) not in (".", "..")]

    def ensure_dir(self, remote_dir: str) -> None:
        parts = [p for p in remote_dir.split("/") if p]
        cur = "/"
        for p in parts:
            nxt = f"{cur.rstrip('/')}/{p}"
            try:
                self.ftp.cwd(nxt)
            except ftplib.all_errors:
                self.ftp.mkd(nxt)
                self.ftp.cwd(nxt)
            cur = nxt

    def upload_file(self, local_path: Path, remote_path: str) -> None:
        self.ensure_dir(remote_path.rsplit("/", 1)[0])
        size = local_path.stat().st_size
        print(f"[ftp] Uploading {local_path} ({size:,} bytes) -> {remote_path}...")
        t0 = time.monotonic()
        with open(local_path, "rb") as fp:
            self.ftp.storbinary(f"STOR {remote_path}", fp, blocksize=262144)
        print(f"[ftp] Uploaded {remote_path} in {time.monotonic() - t0:.1f} s")

    def upload_bytes(self, data: bytes, remote_path: str) -> None:
        self.ensure_dir(remote_path.rsplit("/", 1)[0])
        bio = io.BytesIO(data)
        self.ftp.storbinary(f"STOR {remote_path}", bio, blocksize=65536)
        print(f"[ftp] Uploaded {len(data):,} bytes -> {remote_path}")

    def download_file(self, remote_path: str, local_path: Path) -> bool:
        local_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with open(local_path, "wb") as fp:
                self.ftp.retrbinary(f"RETR {remote_path}", fp.write, blocksize=262144)
            return True
        except ftplib.all_errors:
            if local_path.exists() and local_path.stat().st_size == 0:
                local_path.unlink(missing_ok=True)
            return False

    def snapshot_existing(self) -> tuple[set[str], set[str]]:
        logs = set(self._safe_nlst(f"{self.base_dir}/logs"))
        caps = set(self._safe_nlst(f"{self.base_dir}/captures"))
        return logs, caps

    def pull_artifacts(self, run_dir: Path, prev_logs: set[str], prev_caps: set[str]) -> dict[str, object]:
        pulled: dict[str, object] = {"logs": [], "rex": [], "captures": []}

        # Pull new or highest-numbered fnc-nx_*.log files
        all_logs = sorted(f for f in self._safe_nlst(f"{self.base_dir}/logs") if re.match(r"fnc-nx_\d+\.log$", f))
        new_logs = [f for f in all_logs if f not in prev_logs]
        targets = new_logs if new_logs else (all_logs[-1:] if all_logs else [])
        for name in targets:
            dst = run_dir / name
            if self.download_file(f"{self.base_dir}/logs/{name}", dst):
                pulled["logs"].append(str(dst))
                print(f"[ftp] Pulled log: {dst} ({dst.stat().st_size:,} bytes)")

        # Pull rex/* logs (rex_profile.log, rex_crash.log, rex_stderr.log)
        rex_files = self._safe_nlst(f"{self.base_dir}/logs/rex")
        for name in ("rex_profile.log", "rex_crash.log", "rex_stderr.log"):
            if name in rex_files:
                dst = run_dir / name
                if self.download_file(f"{self.base_dir}/logs/rex/{name}", dst):
                    pulled["rex"].append(str(dst))
                    print(f"[ftp] Pulled rex log: {dst} ({dst.stat().st_size:,} bytes)")

        # Pull new video_* capture files (and any matching video_*s.* files if none were listed earlier)
        all_caps = sorted(self._safe_nlst(f"{self.base_dir}/captures"))
        new_caps = [f for f in all_caps if f not in prev_caps]
        cap_dir = run_dir / "captures"
        for name in new_caps:
            dst = cap_dir / name
            if self.download_file(f"{self.base_dir}/captures/{name}", dst):
                pulled["captures"].append(str(dst))
                print(f"[ftp] Pulled capture: {dst} ({dst.stat().st_size:,} bytes)")
        return pulled


def posix_basename(path: str) -> str:
    return path.rstrip("/").rsplit("/", 1)[-1]


def convert_rgba_captures(captures_dir: Path) -> list[str]:
    pngs: list[str] = []
    if not captures_dir.exists():
        return pngs
    for rgba_path in sorted(captures_dir.glob("video_*s.rgba")):
        txt_path = rgba_path.with_suffix(".txt")
        w, h = 640, 360
        if txt_path.exists():
            parts = txt_path.read_text(encoding="utf-8", errors="ignore").strip().split()
            if len(parts) >= 2:
                w, h = int(parts[0]), int(parts[1])
        else:
            size = rgba_path.stat().st_size
            for cand_w, cand_h in ((640, 360), (1280, 720), (960, 540)):
                if size % (cand_w * cand_h * 4) == 0:
                    w, h = cand_w, cand_h
                    break
        data = rgba_path.read_bytes()
        frame_bytes = w * h * 4
        n_frames = len(data) // frame_bytes
        stem = rgba_path.stem
        for idx in range(n_frames):
            chunk = data[idx * frame_bytes : (idx + 1) * frame_bytes]
            img = Image.frombytes("RGBA", (w, h), chunk).convert("RGB")
            out_png = captures_dir / f"{stem}_frame_{idx:02d}.png"
            img.save(out_png)
            pngs.append(str(out_png))
        print(f"[capture] Converted {n_frames} frame(s) ({w}x{h}) from {rgba_path.name} -> {captures_dir}")
    return pngs


def launch_via_nxlink(ip: str, nro_path: Path) -> bool:
    if not NXLINK_EXE.exists():
        print(f"[nxlink] Missing {NXLINK_EXE}; cannot use nxlink fallback")
        return False
    cmd = [str(NXLINK_EXE), "-a", ip, "-p", "/switch/fnc-nx/fnc-nx.nro", str(nro_path)]
    print(f"[nxlink] Running: {' '.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if proc.stdout:
        print(proc.stdout.strip())
    if proc.stderr:
        print(proc.stderr.strip())
    return proc.returncode == 0


def summarize_log_lines(lines: list[str]) -> dict[str, object]:
    renut_fps = [l for l in lines if "game frames (" in l]
    xma_lines = [l for l in lines if "[xma] on the calling thread" in l]
    uam_lines = [l for l in lines if "[deko3d] uam: compile" in l or "[deko3d] uam: run-time" in l]
    errors = [l for l in lines if "[error]" in l or "[FATAL]" in l or "crash" in l.lower()]
    return {
        "renut_fps_lines": renut_fps,
        "xma_lines": xma_lines,
        "uam_compile_lines": uam_lines,
        "error_lines": errors[-20:],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Autonomous real-Switch test harness for fnc-nx")
    ap.add_argument("--ip", required=True, help="Nintendo Switch Wi-Fi IP address")
    ap.add_argument("--pc-ip", default="192.168.0.78", help="PC local IP for log_network TCP callback")
    ap.add_argument("--log-port", type=int, default=9999, help="TCP port on PC for live Switch logs")
    ap.add_argument("--ftp-port", type=int, default=5000, help="ftpsrv FTP port on Switch")
    ap.add_argument("--bot-port", type=int, default=6000, help="sys-botbase TCP port on Switch")
    ap.add_argument("--mode", choices=("visual", "perf"), default="perf", help="Test preset mode")
    ap.add_argument("--name", default=None, help="Run folder name under out/nx/runs/")
    ap.add_argument("--nro", default=str(REPO_ROOT / "out" / "nx" / "sa2.nro"), help="Path to local Switch NRO")
    ap.add_argument("--skip-nro-upload", action="store_true", help="Upload fnc-nx.toml only; keep existing NRO")
    ap.add_argument("--duration", type=int, default=125, help="switch_auto_exit_s duration on Switch (seconds)")
    ap.add_argument("--freeze-timeout", type=int, default=25, help="Seconds without [renut] progress before freeze")
    ap.add_argument("--input-script", default=DEFAULT_INPUT_SCRIPT, help="input_script sent to fnc-nx")
    ap.add_argument("--video-at", type=int, default=85, help="sa2_video_s start time in visual mode")
    ap.add_argument("--video-frames", type=int, default=12, help="sa2_video_frames count in visual mode")
    ap.add_argument("--video-every", type=int, default=10, help="sa2_video_every frame stride in visual mode")
    ap.add_argument("--video-half", action=argparse.BooleanOptionalAction, default=True, help="sa2_video_half toggle")
    ap.add_argument("--pixelpeek-at", default="", help="Comma-separated seconds after launch to call pixelPeek")
    ap.add_argument("--profile", action=argparse.BooleanOptionalAction, default=None, help="Override switch_profile_*")
    ap.add_argument("--extra-toml", action="append", default=[], help="Extra key=value TOML overrides (repeatable)")
    ap.add_argument("--launch", choices=("menu", "nxlink", "auto"), default="auto", help="NRO launch strategy")
    ap.add_argument("--smoke-only", action="store_true", help="Only verify FTP + sys-botbase + pixelPeek")
    args = ap.parse_args()

    run_name = args.name or f"switch_run_{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}"
    run_dir = REPO_ROOT / "out" / "nx" / "runs" / run_name
    run_dir.mkdir(parents=True, exist_ok=True)
    nro_path = Path(args.nro)

    if not args.smoke_only and not args.skip_nro_upload and not nro_path.exists():
        print(f"ERROR: NRO not found at {nro_path}", file=sys.stderr)
        return 2

    print(f"=== fnc-nx autonomous Switch test: {run_name} (ip={args.ip}, mode={args.mode}) ===")
    print(f"Run folder: {run_dir}")

    # 1. Start live log listener before FTP upload / launch so we never miss the first log lines.
    listener = LiveLogServer(args.pc_ip, args.log_port, run_dir / "live.log", echo=not args.smoke_only)
    listener.start()

    ftp = SwitchFtpClient(args.ip, args.ftp_port)
    bot = SysBotbaseClient(args.ip, args.bot_port)

    try:
        # 2. Verify / use FTP (ftpsrv)
        try:
            ftp.connect()
        except Exception as exc:
            print(f"[error] Cannot connect to Switch ftpsrv at {args.ip}:{args.ftp_port}: {exc}", file=sys.stderr)
            return 1
        print(f"[ftp] Connected to {args.ip}:{args.ftp_port} (cwd={ftp.base_dir})")
        prev_logs, prev_caps = ftp.snapshot_existing()

        # 3. Verify sys-botbase connectivity & pixelPeek
        try:
            bot.connect()
        except Exception as exc:
            print(f"[error] Cannot connect to Switch sys-botbase at {args.ip}:{args.bot_port}: {exc}", file=sys.stderr)
            return 1
        ver = bot.get_version()
        tid = bot.get_title_id()
        print(f"[bot] Connected to {args.ip}:{args.bot_port} | version={ver!r} | titleID={tid!r}")

        if args.smoke_only:
            ok, msg = bot.pixel_peek(run_dir / "peek_smoke.jpg")
            print(f"[bot] Smoke pixelPeek: {msg}")
            ftp.close()
            bot.close()
            listener.stop()
            print("=== Smoke test complete ===")
            return 0 if ok else 1

        # Generate and upload fnc-nx.toml + NRO via FTP
        toml_text = build_test_toml(args)
        (run_dir / "fnc-nx.toml").write_text(toml_text, encoding="utf-8")
        ftp.upload_bytes(toml_text.encode("utf-8"), f"{ftp.base_dir}/fnc-nx.toml")
        if not args.skip_nro_upload and args.launch != "nxlink":
            ftp.upload_file(nro_path, f"{ftp.base_dir}/fnc-nx.nro")
        ftp.close()

        # 4. Enter full-RAM hbmenu via sys-botbase (R + A), launch NRO, and detach controller
        bot.open_full_ram_hbmenu(run_dir)
        launched_via_nxlink = False

        if args.launch == "nxlink":
            print("[bot] Opening hbmenu NetLoader with Y and pushing NRO via nxlink...")
            bot.click("Y")
            time.sleep(1.5)
            launched_via_nxlink = launch_via_nxlink(args.ip, nro_path)
            if not launched_via_nxlink:
                raise RuntimeError("nxlink failed to transfer/launch NRO")
        else:
            print("[bot] Launching starred fnc-nx in hbmenu with A...")
            bot.click("A")
            if not listener.connected.wait(timeout=15.0):
                if args.launch == "auto":
                    print("[warn] No log_network connection within 15 s; trying Y + nxlink fallback...")
                    bot.click("Y")
                    time.sleep(1.2)
                    launched_via_nxlink = launch_via_nxlink(args.ip, nro_path)
                if not launched_via_nxlink and not listener.connected.is_set():
                    print("[warn] Still waiting for live logs (check Windows firewall TCP port 9999 if blank)")

        # Detach sys-botbase virtual controller before gameplay so input_script owns player 1.
        time.sleep(0.4)
        bot.detach_controller()
        print("[bot] Called detachController; waiting for automated run...")

        peek_times: list[int] = []
        if args.pixelpeek_at:
            peek_times = sorted(int(x.strip()) for x in args.pixelpeek_at.split(",") if x.strip())
        elif args.mode == "visual":
            peek_times = [args.video_at, args.video_at + 12, min(max(10, args.duration - 5), args.video_at + 24)]
        peek_done: set[int] = set()

        # 5. Monitor live logs for clean exit, scheduled pixelPeek grabs, or freeze/timeout
        launch_ts = time.monotonic()
        hard_deadline = launch_ts + max(30, args.duration + 75)
        exit_reason = "unknown"

        while True:
            now = time.monotonic()
            elapsed = now - launch_ts

            for pt in peek_times:
                if pt not in peek_done and elapsed >= pt:
                    peek_done.add(pt)
                    ok, msg = bot.pixel_peek(run_dir / f"peek_{pt}s.jpg")
                    print(f"[monitor @{elapsed:.1f}s] pixelPeek: {msg}")
                    try:
                        bot.detach_controller()
                    except Exception:
                        pass

            if listener.clean_exit.is_set():
                print(f"[monitor @{elapsed:.1f}s] Clean exit log detected; waiting 5 s for process teardown...")
                time.sleep(5.0)
                exit_reason = "clean_exit"
                break

            line_count, last_any, first_renut, last_renut = listener.snapshot_stats()

            if first_renut is not None and last_renut is not None:
                stale_renut = now - last_renut
                if listener.disconnected.is_set() or (elapsed >= args.duration + 2 and stale_renut >= 8.0):
                    if listener.disconnected.is_set():
                        time.sleep(2.0)
                    try:
                        tid_now = bot.get_title_id()
                    except Exception:
                        tid_now = None
                    if tid_now in ("", "0000000000000000"):
                        print(f"[monitor @{elapsed:.1f}s] Process returned cleanly to Home Menu; clean_exit")
                        exit_reason = "clean_exit"
                        break
                if stale_renut >= args.freeze_timeout:
                    print(f"[monitor @{elapsed:.1f}s] FREEZE DETECTED: no [renut] line for {stale_renut:.1f} s!")
                    exit_reason = f"freeze_renut_{stale_renut:.1f}s"
                    break
            elif line_count > 0 and last_any is not None and (now - last_any) >= 45.0:
                print(f"[monitor @{elapsed:.1f}s] FREEZE DETECTED: no log lines for {now - last_any:.1f} s!")
                exit_reason = "freeze_no_logs"
                break

            if now >= hard_deadline:
                print(f"[monitor @{elapsed:.1f}s] Hard timeout ({args.duration + 75} s) reached!")
                exit_reason = "timeout"
                break

            time.sleep(0.5)

        if exit_reason != "clean_exit":
            ok, msg = bot.pixel_peek(run_dir / "peek_frozen.jpg")
            print(f"[monitor] Frozen screen capture: {msg}")
            bot.force_close_app()
        else:
            # Verify Switch actually returned to Home Menu; force close only if still foreground.
            tid_end = bot.get_title_id()
            if tid_end not in ("", "0000000000000000"):
                print(f"[bot] Title {tid_end} still active after auto-exit; waiting 5 s...")
                time.sleep(5.0)
                if bot.get_title_id() not in ("", "0000000000000000"):
                    bot.force_close_app()

        # 6. Pull final logs, profiler data, and captures via ftpsrv
        time.sleep(1.5)
        ftp = SwitchFtpClient(args.ip, args.ftp_port)
        ftp.connect()
        pulled = ftp.pull_artifacts(run_dir, prev_logs, prev_caps)
        ftp.close()

        pngs = convert_rgba_captures(run_dir / "captures")

        # Parse final pulled log (prefer full SD log over TCP live.log, in case tail flushed on exit)
        full_log_lines = listener.lines
        if pulled["logs"]:
            full_log_path = Path(pulled["logs"][-1])
            full_log_lines = full_log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        stats = summarize_log_lines(full_log_lines)

        summary = {
            "run_name": run_name,
            "switch_ip": args.ip,
            "mode": args.mode,
            "exit_reason": exit_reason,
            "duration_s": round(time.monotonic() - launch_ts, 2),
            "pulled": pulled,
            "converted_pngs": pngs,
            "stats": stats,
        }
        (run_dir / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

        print("\n=== RUN SUMMARY ===")
        print(f"Exit reason : {exit_reason}")
        print(f"Elapsed     : {summary['duration_s']} s")
        print(f"Pulled logs : {pulled['logs']}")
        print(f"Rex logs    : {pulled['rex']}")
        print(f"PNG frames  : {len(pngs)}")
        for l in stats["renut_fps_lines"][-6:]:
            print(" ", l)
        for l in stats["xma_lines"][-3:]:
            print(" ", l)
        if stats["error_lines"]:
            print("Errors/Warnings tail:")
            for l in stats["error_lines"][-5:]:
                print(" ", l)
        return 0 if exit_reason == "clean_exit" else 1

    finally:
        listener.stop()
        try:
            bot.release("R")
            bot.detach_controller()
        except Exception:
            pass
        bot.close()
        try:
            ftp.close()
        except Exception:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
