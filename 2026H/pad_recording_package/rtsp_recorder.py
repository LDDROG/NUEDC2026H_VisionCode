# LDD_ROG 2026.7.29
# -*- coding: utf-8 -*-
# 用法:
#   python rtsp_recorder.py --url rtsp://...
#   python rtsp_recorder.py --url rtsp://... --output D:\videos
#   python rtsp_recorder.py --url rtsp://... --no-display

import subprocess
import sys
import time
import os
import signal
import argparse
import tkinter as tk
from tkinter import ttk
from datetime import datetime

# 优先使用环境变量的ffmpeg和ffplay
def _find_in_path(name):
    exe_name = f"{name}.exe" if sys.platform == "win32" else name
    for dirpath in os.environ.get("PATH", "").split(os.pathsep):
        full = os.path.join(dirpath, exe_name)
        if os.path.isfile(full):
            return full
    return None

def find_exe(name):
    path = _find_in_path(name)
    if path:
        return path
    candidates = [
        os.path.join(os.path.dirname(os.path.abspath(__file__)), f"{name}.exe"),
        f"C:\\ffmpeg\\bin\\{name}.exe",
        f"C:\\Program Files\\ffmpeg\\bin\\{name}.exe",
    ]
    for p in candidates:
        if os.path.isfile(p):
            return p
    return None

def _now():
    return datetime.now().strftime("%H:%M:%S")

def get_next_session_id(output_dir):
    os.makedirs(output_dir, exist_ok=True)
    i = 1
    while True:
        # 匹配文件名格式用于更新命名
        prefix = f"ball_{i}."
        found = False
        if os.path.isdir(output_dir):
            for f in os.listdir(output_dir):
                if f.startswith(prefix) and f.endswith(".mp4"):
                    found = True
                    break
        if not found:
            return i
        i += 1



# 全局状态变量初始化
# 该脚本的逻辑是录制为.ts，结束后再编码成h264的mp4，尽可能减少录制时的编码压力
_shutdown = False
_recording = False
_recorder_proc = None        # 预录制，防止开头延迟
_display_proc = None
_session_id = 0
_output_dir = ""
_record_start_time = None
_ring_offset = 0             # 点“开始”时.ts文件的时间偏移
_RING_PRE_ROLL = 300 * 1024  # 回放窗口

def start_display(ffplay_path, rtsp_url):
    global _display_proc
    cmd = [
        ffplay_path,
        "-rtsp_transport", "tcp",
        "-window_title", "MaixCam RTSP — 实时显示",
        "-fflags", "nobuffer",
        "-flags", "low_delay",
        "-framedrop",
        "-sync", "ext",
        "-probesize", "32",
        "-analyzeduration", "0",
        rtsp_url,
    ]
    try:
        _display_proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(f"[{_now()}] ffplay 已启动 (PID={_display_proc.pid})")
        return True
    except Exception as e:
        print(f"[{_now()}] ffplay 启动失败: {e}")
        return False

def start_ring(ffmpeg_path, rtsp_url, output_dir):
    global _recorder_proc
    ring_path = os.path.join(output_dir, "ring.ts")
    try:
        if os.path.exists(ring_path):
            os.remove(ring_path)
    except OSError:
        pass                     # 文件可能仍被旧进程占用，忽略后ffmpeg -y会覆盖
    cmd = [
        ffmpeg_path,
        "-loglevel", "warning",
        "-rtsp_transport", "tcp",
        "-fflags", "nobuffer",
        "-flags", "low_delay",
        "-analyzeduration", "500000",
        "-probesize", "500000",
        "-i", rtsp_url,
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-profile:v", "main",
        "-crf", "24",
        "-forced-idr", "1",
        "-g", "30",              
        "-pix_fmt", "yuv420p",
        "-an",
        "-vsync", "passthrough",
        "-muxdelay", "0",
        "-t", "1800",            # 30分钟自动退出防止爆盘
        "-f", "mpegts",
        ring_path, "-y",
    ]
    try:
        _recorder_proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(f"[{_now()}] 启动预录制 (ring.ts)")
        return True
    except Exception as e:
        print(f"[{_now()}] 预录制启动失败: {e}")
        _recorder_proc = None
        return False

def stop_ring():
    global _recorder_proc
    if _recorder_proc is None:
        return
    try:
        _recorder_proc.terminate()
        _recorder_proc.wait(timeout=5)
    except Exception:
        try:
            _recorder_proc.kill()
        except Exception:
            pass
    _recorder_proc = None

def slice_recording(output_dir, session_id):
    try:
        ring_path = os.path.join(output_dir, "ring.ts")
        if not os.path.isfile(ring_path) or os.path.getsize(ring_path) <= _ring_offset:
            return None

        ts_path = os.path.join(output_dir, f"ball_{session_id}.ts")
        with open(ring_path, "rb") as fr:
            fr.seek(_ring_offset)
            data = fr.read()
        with open(ts_path, "wb") as fw:
            fw.write(data)
        print(f"[{_now()}] 已截取: {os.path.basename(ts_path)} "
              f"({len(data)/1024:.0f} KB, 含预录制的 {_RING_PRE_ROLL/1024:.0f}KB)")

        mp4_path = ts_path.replace(".ts", ".mp4")
        print(f"[{_now()}] 转换 TS → MP4...")
        ret = subprocess.run([
            find_exe("ffmpeg") or "ffmpeg",
            "-loglevel", "error",
            "-i", ts_path,
            "-c", "copy",
            "-movflags", "+faststart",
            mp4_path, "-y",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if ret.returncode == 0 and os.path.isfile(mp4_path):
            print(f"[{_now()}] 转换完成: {os.path.basename(mp4_path)} "
                  f"({os.path.getsize(mp4_path) / 1024:.0f} KB)")
            return mp4_path
        else:
            print(f"[{_now()}] 转换失败 (code={ret.returncode})，保留 TS 文件")
            return ts_path
    except Exception as e:
        import traceback
        traceback.print_exc()
        print(f"[{_now()}] 截取失败: {e}")
        return None

def stop_display():
    global _display_proc
    if _display_proc is None:
        return
    try:
        _display_proc.terminate()
        _display_proc.wait(timeout=3)
    except Exception:
        try:
            _display_proc.kill()
        except Exception:
            pass
    _display_proc = None

# UI
class RecorderPanel:
    def __init__(self, root, ffmpeg_path, ffplay_path, rtsp_url,
                 output_dir, show_display):
        self.root = root
        self.ffmpeg_path = ffmpeg_path
        self.ffplay_path = ffplay_path
        self.rtsp_url = rtsp_url
        self.output_dir = output_dir
        self.show_display = show_display
        self.current_ts_path = None       # 当前对应的.ts文件路径

        global _session_id, _output_dir
        _session_id = get_next_session_id(output_dir)
        _output_dir = output_dir

        self.root.title("RTSP 录制控制")
        self.root.configure(bg="#1a1a2e")

        win_w, win_h = 400, 320
        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        x = screen_w - win_w - 20
        y = screen_h - win_h - 60
        self.root.geometry(f"{win_w}x{win_h}+{x}+{y}")
        self.root.resizable(False, False)
        self.root.attributes("-topmost", True)

        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Title.TLabel", background="#1a1a2e",
                        foreground="#e0e0e0", font=("Microsoft YaHei", 13, "bold"))
        style.configure("Info.TLabel", background="#1a1a2e",
                        foreground="#a0a0a0", font=("Microsoft YaHei", 10))
        style.configure("Timer.TLabel", background="#1a1a2e",
                        foreground="#00ff88", font=("Consolas", 24, "bold"))
        style.configure("Status.TLabel", background="#1a1a2e",
                        foreground="#888888", font=("Microsoft YaHei", 11))



        ttk.Label(root, text="钢球平衡无线图传录制", style="Title.TLabel"
                  ).pack(pady=(15, 2))
        ttk.Label(root, text=f"Session #{_session_id}", style="Info.TLabel"
                  ).pack()

        self.timer_label = ttk.Label(root, text="00:00", style="Timer.TLabel")
        self.timer_label.pack(pady=(10, 5))

        self.status_label = ttk.Label(root, text="● 就绪 — 点击按钮开始录制",
                                      style="Status.TLabel")
        self.status_label.pack(pady=(0, 15))

        btn_frame = tk.Frame(root, bg="#1a1a2e")
        btn_frame.pack()

        self.rec_btn = tk.Button(
            btn_frame,
            text="●  开 始 录 制",
            font=("Microsoft YaHei", 16, "bold"),
            bg="#00aa44", fg="white",
            activebackground="#00cc55", activeforeground="white",
            relief="flat", borderwidth=0,
            width=14, height=2,
            command=self.toggle_recording,
        )
        self.rec_btn.pack()

        bottom = tk.Frame(root, bg="#1a1a2e")
        bottom.pack(pady=(12, 0))

        tk.Button(
            bottom,
            text="回放",
            font=("Microsoft YaHei", 10, "bold"),
            bg="#333388", fg="white",
            activebackground="#4444aa", activeforeground="white",
            relief="flat", borderwidth=0,
            width=8, height=1,
            command=self.replay_last,
        ).pack(side="left", padx=5)

        tk.Button(
            bottom,
            text="退出",
            font=("Microsoft YaHei", 10, "bold"),
            bg="#883333", fg="white",
            activebackground="#aa4444", activeforeground="white",
            relief="flat", borderwidth=0,
            width=8, height=1,
            command=self.quit_app,
        ).pack(side="left", padx=5)

        # 定时更新UI
        self.update_ui()

        self.root.protocol("WM_DELETE_WINDOW", self.quit_app)

    def toggle_recording(self):
        global _recording, _session_id, _ring_offset, _record_start_time
        if not _recording:
            ring_path = os.path.join(self.output_dir, "ring.ts")
            size = os.path.getsize(ring_path) if os.path.isfile(ring_path) else 0
            _ring_offset = max(0, size - _RING_PRE_ROLL)
            _record_start_time = time.time()
            _recording = True
            self.rec_btn.config(
                text = "■  停 止 录 制",
                bg = "#cc2222",
                activebackground="#ee3333")
            self.status_label.config(
                text = f"● 录制中",
                foreground="#ff4444")
        else:
            self.status_label.config(
                text = "转换MP4中...", foreground="#ffaa00")
            self.rec_btn.config(state="disabled")
            self.root.update()              # 立即刷新 UI

            stop_ring()                     # 先停写, 保证截取数据完整
            mp4_path = slice_recording(self.output_dir, _session_id)
            start_ring(self.ffmpeg_path, self.rtsp_url, self.output_dir)
            _recording = False
            _session_id += 1
            self.rec_btn.config(
                text="●  开 始 录 制",
                bg="#00aa44",
                activebackground="#00cc55",
                state="normal")
            self.timer_label.config(text="00:00")
            if mp4_path:
                self.status_label.config(
                    text=f"✔ 已保存 — {os.path.basename(mp4_path)}",
                    foreground="#00ff88")
            else:
                self.status_label.config(
                    text=f"⚠ 录制异常 — 检查 TS 文件",
                    foreground="#ff4444")

    def replay_last(self):
        files = []
        if os.path.isdir(self.output_dir):
            for f in os.listdir(self.output_dir):
                if f.endswith('.mp4'):
                    files.append(os.path.join(self.output_dir, f))
        if files:
            latest = max(files, key=os.path.getmtime)
            print(f"[{_now()}] 回放: {latest}")
            os.startfile(latest)
        else:
            self.status_label.config(
                text="⚠ 没有可回放的文件", foreground="#ffaa00")

    def update_ui(self):
        global _recording, _recorder_proc, _record_start_time

        if _recording:
            if _record_start_time is not None:
                elapsed = int(time.time() - _record_start_time)
                mm, ss = divmod(elapsed, 60)
                self.timer_label.config(text=f"{mm:02d}:{ss:02d}")

        if _recorder_proc is not None:
            ret = _recorder_proc.poll()
            if ret is not None:
                print(f"[{_now()}] 预录退出 (code={ret})，自动重启...")
                start_ring(self.ffmpeg_path, self.rtsp_url, self.output_dir)

        self.root.after(500, self.update_ui)

    def quit_app(self):
        global _shutdown
        if _recording:
            slice_recording(self.output_dir, _session_id)
        _shutdown = True
        self.root.destroy()



def run():
    global _shutdown

    parser = argparse.ArgumentParser(
        description="RTSP推流录制")
    parser.add_argument("--url", required=True, help="RTSP地址")
    parser.add_argument("--output", default="./recordings", help="输出目录")
    parser.add_argument("--no-display", action="store_true",
                        help="禁用实时显示（仅录制）")
    args = parser.parse_args()

    ffmpeg = find_exe("ffmpeg")
    if ffmpeg is None:
        print("[FATAL] 找不到 ffmpeg！")
        sys.exit(1)

    ffplay = None if args.no_display else find_exe("ffplay")
    show_display = ffplay is not None

    print(f"[{_now()}] RTSP: {args.url}")
    print(f"[{_now()}] 输出: {os.path.abspath(args.output)}")
    print(f"[{_now()}] 格式: TS录制 → 自动转MP4 | 显示: {'开' if show_display else '关'}")

    start_ring(ffmpeg, args.url, args.output)

    if show_display:
        start_display(ffplay, args.url)

    root = tk.Tk()
    app = RecorderPanel(
        root, ffmpeg, ffplay, args.url,
        args.output, show_display,
    )

    signal.signal(signal.SIGINT, lambda *a: app.quit_app())

    root.mainloop()

    print(f"[{_now()}] 正在退出...")
    stop_ring()
    if show_display:
        stop_display()
    print(f"[{_now()}] 程序已退出。")

if __name__ == "__main__":
    run()