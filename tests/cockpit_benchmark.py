#!/usr/bin/env python3
"""经真实多 FD 接入测量感知时延；负载发生器丢弃和队列过载分别报告。"""
import argparse
import json
import os
from pathlib import Path
import platform
import socket
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sample"))
from cockpit_replay import FRAME
from cockpit_client import Client, Interaction
from cockpit_supervisor import Supervisor
from voice_demo import load_config


def usage(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().split()
    return (int(fields[13]) + int(fields[14])) / os.sysconf("SC_CLK_TCK"), int(fields[23]) * os.sysconf("SC_PAGE_SIZE")


class Sender:
    def __init__(self, path, rate, seconds, signal, source):
        self.path, self.rate, self.seconds, self.signal, self.source = str(path), rate, seconds, signal, source
        self.sent = self.dropped = 0
        self.failure = None
        self.thread = threading.Thread(target=self.run)

    def run(self):
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as channel:
                channel.setblocking(False)
                started = time.monotonic()
                sequence = 0
                while time.monotonic() - started < self.seconds:
                    expected = int((time.monotonic() - started) * self.rate) + 1 if self.rate else sequence + 64
                    # 每轮有界，生成器本身不永久占用 GIL。
                    for _ in range(min(64, expected - sequence)):
                        sequence += 1
                        value = 24 + (sequence % 100) / 100 if self.signal == 0 else .7 + .2 * (sequence % 2)
                        try:
                            channel.sendto(FRAME.pack(self.signal, self.source, 1, sequence, 1, value), self.path)
                            self.sent += 1
                        except BlockingIOError:
                            self.dropped += 1
                    if self.rate:
                        time.sleep(.0005)
        except Exception as error:
            self.failure = error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/cockpit")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "artifacts/cockpit-performance")
    parser.add_argument("--seconds", type=float, default=3)
    parser.add_argument("--models", action="store_true", help="同场加载真实 ASR/TTS，显式 mock LLM")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    config = load_config(ROOT / "configs/cockpit-x86.json", ROOT / "voice-deps")
    if args.models:
        config["llm"]["mock_delay_ms"] = 1500
    results = []
    for label, rate in (("normal", 50), ("1k", 1000), ("10k", 10000), ("burst", 0)):
        with tempfile.TemporaryDirectory(prefix="cc-perf-") as directory:
            with Supervisor(args.build_dir, config, directory, port=0, models=args.models) as supervisor:
                client = interaction = None
                if args.models:
                    client = Client(port=supervisor.port)
                    interaction = Interaction(client, "benchmark")
                    question = interaction.speech(Path(config["asr"]["model_dir"]) / "test_wavs/2.wav")
                pid = supervisor.processes["cockpit"].pid
                before_cpu, _ = usage(pid)
                started = time.monotonic()
                # vehicle FD 高负载时，另一个 FD 持续产生必须保留的行为事件。
                senders = [Sender(supervisor.runtime_dir / "vehicle.sock", rate, args.seconds, 0, 0),
                           Sender(supervisor.runtime_dir / "occupant.sock", 100, args.seconds, 19, 1)]
                for sender in senders:
                    sender.thread.start()
                for sender in senders:
                    sender.thread.join()
                    if sender.failure:
                        raise sender.failure
                after_cpu, rss = usage(pid)
                elapsed = time.monotonic() - started
                state = supervisor.client.request(supervisor.tasks["cockpit"], "inference", "cockpit.status", {})["data"]
                results.append(dict(load=label, elapsed_seconds=elapsed, target_rate=rate or "unthrottled",
                    producer_sent=[sender.sent for sender in senders], producer_dropped=[sender.dropped for sender in senders],
                    cpu_percent_one_core=100 * (after_cpu - before_cpu) / elapsed, rss_bytes=rss,
                    metrics=state["metrics"], valid_updates=state["updates"], rejected_updates=state["rejected"]))
                if interaction:
                    try:
                        client.wait(question, lambda message: message["object"] == "tts.audio.file", timeout=15)
                        client.forget(question)
                        results[-1]["concurrent_question_completed"] = True
                    finally:
                        interaction.close()
                        client.close()
                print(json.dumps(results[-1], ensure_ascii=False), flush=True)
            (args.output_dir / f"{label}-traces.jsonl").write_bytes(Path(directory, "traces.jsonl").read_bytes())
    report = dict(platform=platform.platform(), machine=platform.machine(), cpu_count=os.cpu_count(),
        seconds_per_load=args.seconds, models_loaded=args.models, scenarios=results,
        notes=["计时从 Ingress 收到数据开始，不包含内核 socket 队列和物理传感器延迟。",
               "CPU 仅统计 cockpit 进程，百分比以一个 CPU 核心为 100%。",
               "固定直方图百分位为桶上界估计；不足 1000 样本不报告 P99.9。",
               "发生器发送失败与 Ingress 队列溢出分开计数；burst 不代表确定吞吐承诺。"])
    (args.output_dir / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
