# LDD_ROG 2026.7.29
# RTSP 实时显示 + 触控录制 — Windows 平板端（封箱用）
#
# 录制流程: RTSP → .ts（流式容器，中断可播）→ 停止后自动转 .mp4
# TS 格式无 moov 原子依赖，任何时刻中断文件均完整可播。
# MP4 转换仅做容器重封装（-c copy），速度极快。
#
# 依赖: Python 标准库 + ffmpeg/ffplay (需在PATH中)
# 用法:
#   python rtsp_recorder.py --url rtsp://192.168.137.36:8554/live
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


# ======================== 工具 ========================

def _find_in_path(name):
    """在 PATH 中搜索可执行文件（等价 shutil.which，避免版本警告）"""
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
    i = 0
    while True:
        # 匹配 ball_000_*.mp4 格式
        prefix = f"ball_{i:03d}_"
        found = False
        if os.path.isdir(output_dir):
            for f in os.listdir(output_dir):
                if f.startswith(prefix) and f.endswith(".mp4"):
                    found = True
                    break
        if not found:
            return i
        i += 1


# ======================== 全局状态 ========================
_shutdown = False
_recording = False
_recorder_proc = None
_display_proc = None
_session_id = 0
_output_dir = ""
_record_start_time = None


# ======================== 子进程管理 ========================

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


def start_recording(ffmpeg_path, rtsp_url, output_dir, session_id):
    """录制 RTSP 流到 TS 文件（流式容器，中断可播）"""
    global _recorder_proc, _record_start_time
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    ts_path = os.path.join(output_dir, f"ball_{session_id:03d}_{ts}.ts")
    cmd = [
        ffmpeg_path,
        "-loglevel", "warning",
        "-rtsp_transport", "tcp",
        "-analyzeduration", "10000000",
        "-probesize", "10000000",
        "-i", rtsp_url,
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-profile:v", "main",
        "-crf", "24",
        "-pix_fmt", "yuv420p",
        "-r", "30",
        "-vsync", "cfr",
        "-an",
        "-f", "mpegts",
        ts_path, "-y",
    ]
    try:
        _recorder_proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        _record_start_time = time.time()
        print(f"[{_now()}] 录制开始 Session {session_id:03d} → {os.path.basename(ts_path)}")
        return ts_path
    except Exception as e:
        print(f"[{_now()}] ffmpeg 启动失败: {e}")
        return None


def stop_recording(ts_path=None):
    """停止录制，将 TS 无损转为 MP4"""
    global _recorder_proc, _record_start_time
    if _recorder_proc is None:
        return None
    print(f"[{_now()}] 停止录制...")
    try:
        _recorder_proc.terminate()
        _recorder_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        _recorder_proc.kill()
        _recorder_proc.wait()
    except Exception:
        try:
            _recorder_proc.kill()
        except Exception:
            pass
    _recorder_proc = None
    _record_start_time = None
    print(f"[{_now()}] 录制已停止")

    # TS → MP4 无损转换
    if ts_path and os.path.isfile(ts_path):
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
            # 保留 TS 作为备份，万一 MP4 有问题可回退
            return mp4_path
        else:
            print(f"[{_now()}] 转换失败，保留 TS 文件")
            return ts_path
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


# ======================== Tkinter 触控面板 ========================

class RecorderPanel:
    def __init__(self, root, ffmpeg_path, ffplay_path, rtsp_url,
                 output_dir, show_display):
        self.root = root
        self.ffmpeg_path = ffmpeg_path
        self.ffplay_path = ffplay_path
        self.rtsp_url = rtsp_url
        self.output_dir = output_dir
        self.show_display = show_display
        self.current_ts_path = None   # 当前录制对应的 TS 文件路径

        global _session_id, _output_dir
        _session_id = get_next_session_id(output_dir)
        _output_dir = output_dir

        self.root.title("RTSP 录制控制")
        self.root.configure(bg="#1a1a2e")

        # 窗口大小和位置
        win_w, win_h = 400, 320
        screen_w = self.root.winfo_screenwidth()
        screen_h = self.root.winfo_screenheight()
        x = screen_w - win_w - 20
        y = screen_h - win_h - 60
        self.root.geometry(f"{win_w}x{win_h}+{x}+{y}")
        self.root.resizable(False, False)
        self.root.attributes("-topmost", True)

        # ---- 样式 ----
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

        # ---- 标题 ----
        ttk.Label(root, text="钢球平衡 — 图传录制", style="Title.TLabel"
                  ).pack(pady=(15, 2))
        ttk.Label(root, text=f"Session #{_session_id:03d}", style="Info.TLabel"
                  ).pack()

        # ---- 计时器 ----
        self.timer_label = ttk.Label(root, text="00:00", style="Timer.TLabel")
        self.timer_label.pack(pady=(10, 5))

        # ---- 状态 ----
        self.status_label = ttk.Label(root, text="● 就绪 — 点击按钮开始录制",
                                      style="Status.TLabel")
        self.status_label.pack(pady=(0, 15))

        # ---- 录制按钮（巨大，触摸友好） ----
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

        # ---- 底部按钮行 ----
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

        # ---- 定时更新 UI ----
        self.update_ui()

        # ---- 关闭窗口 = 退出 ----
        self.root.protocol("WM_DELETE_WINDOW", self.quit_app)

    def toggle_recording(self):
        global _recording, _session_id
        if not _recording:
            # ---- 开始 ----
            ts_path = start_recording(
                self.ffmpeg_path, self.rtsp_url, self.output_dir,
                _session_id)
            if ts_path:
                self.current_ts_path = ts_path
                _recording = True
                self.rec_btn.config(
                    text="■  停 止 录 制",
                    bg="#cc2222",
                    activebackground="#ee3333")
                self.status_label.config(
                    text=f"● 录制中 — {os.path.basename(ts_path)}",
                    foreground="#ff4444")
        else:
            # ---- 停止 + 转 MP4 ----
            self.status_label.config(
                text="⏳ 转换 MP4 中...", foreground="#ffaa00")
            self.rec_btn.config(state="disabled")
            self.root.update()              # 立即刷新 UI

            mp4_path = stop_recording(self.current_ts_path)
            self.current_ts_path = None
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
        """用系统默认播放器打开最近的录制文件"""
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
        """定时刷新计时器和ffmpeg存活检测"""
        global _recording, _recorder_proc, _record_start_time

        if _recording:
            # 更新计时
            if _record_start_time is not None:
                elapsed = int(time.time() - _record_start_time)
                mm, ss = divmod(elapsed, 60)
                self.timer_label.config(text=f"{mm:02d}:{ss:02d}")

            # 检测 ffmpeg 意外退出
            if _recorder_proc is not None:
                ret = _recorder_proc.poll()
                if ret is not None:
                    self.status_label.config(
                        text=f"⚠ ffmpeg 意外退出 (code={ret})！",
                        foreground="#ff4444")
                    self.rec_btn.config(state="disabled")

        self.root.after(500, self.update_ui)

    def quit_app(self):
        global _shutdown
        if _recording:
            stop_recording(self.current_ts_path)
        _shutdown = True
        self.root.destroy()


# ======================== 主入口 ========================

def run():
    global _shutdown

    parser = argparse.ArgumentParser(
        description="MaixCam RTSP 触控录制器")
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

    # 启动实时显示
    if show_display:
        start_display(ffplay, args.url)

    # 启动 Tkinter 触控面板
    root = tk.Tk()
    app = RecorderPanel(
        root, ffmpeg, ffplay, args.url,
        args.output, show_display,
    )

    # 信号处理
    signal.signal(signal.SIGINT, lambda *a: app.quit_app())

    root.mainloop()

    # 清理
    print(f"[{_now()}] 正在退出...")
    if _recording:
        stop_recording()
    if show_display:
        stop_display()
    print(f"[{_now()}] 程序已退出。")


if __name__ == "__main__":
    run()
