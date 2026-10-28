#!/usr/bin/env python3
"""自主启动和监测座舱；交互客户端连接不参与系统 Task 生命周期。"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import fcntl
import json
import os
from pathlib import Path
import signal
import socket
import stat
import subprocess
import threading
import time
import uuid

from cockpit_client import Client
from voice_demo import ROOT, VOICE_DEPS_ROOT, load_config


class Supervisor:
    def __init__(self, build_dir, config, output_dir, port=None, models=True):
        self.build_dir = Path(build_dir).resolve()
        self.config = config
        self.output_dir = Path(output_dir).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.runtime_dir = self.output_dir / "runtime"
        self.runtime_dir.mkdir(exist_ok=True)
        self.lock = open(self.runtime_dir / "supervisor.lock", "a")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.lock.close()
            raise RuntimeError("another supervisor owns this runtime directory")
        try:
            self._prepare_runtime()
        except BaseException:
            self.lock.close()
            raise
        self.runtime_id = uuid.uuid4().hex
        self.port = config.get("manager_port", 10001) if port is None else port
        if not self.port:
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                self.port = reservation.getsockname()[1]
        self.models = models
        self.processes = {}
        self.logs = []
        self.tasks = {name: None for name in ("cockpit", "asr", "llm", "tts")}
        self.errors = {}
        self.version = 0
        self.client = None
        self.closed = False
        self.environment = os.environ.copy()
        self.environment["EDGE_RPC_DIR"] = str(self.runtime_dir)
        self.environment.setdefault("OMP_NUM_THREADS", "2")
        self.stop_event = threading.Event()
        self.metrics_lock = threading.Lock()
        self.started_ns = time.monotonic_ns()

    def _prepare_runtime(self):
        # 必须持有目录锁。先检查内核中的地址归属，再删除无主 Socket 文件。
        prefix = str(self.runtime_dir) + "/"
        for line in Path("/proc/net/unix").read_text().splitlines()[1:]:
            fields = line.split(maxsplit=7)
            if len(fields) == 8 and fields[7].startswith(prefix):
                raise RuntimeError(f"runtime socket still in use: {fields[7]}; "
                                   "stop the old instance or use another --output-dir")
        for path in self.runtime_dir.iterdir():
            if stat.S_ISSOCK(path.lstat().st_mode):
                path.unlink()

    def _record(self, phase, started, **details):
        row = dict(runtime_id=self.runtime_id, phase=phase, time_ns=time.monotonic_ns(),
                   latency_ns=time.monotonic_ns() - started, **details)
        with self.metrics_lock:
            with open(self.output_dir / "supervisor-traces.jsonl", "a") as output:
                output.write(json.dumps(row, ensure_ascii=False) + "\n")

    def _spawn(self, name):
        binary = self.build_dir / "bin" / ("unit_manager" if name == "sys" else f"edge_{name}")
        command = [str(binary)]
        if name == "sys":
            path = self.runtime_dir / "manager.json"
            path.write_text(json.dumps({"config_tcp_server": self.port, "config_zmq_min_port": 5010,
                "config_zmq_max_port": 5555, "config_zmq_s_format": f"ipc://{self.runtime_dir}/%i.sock",
                "config_zmq_c_format": f"ipc://{self.runtime_dir}/%i.sock"}))
            command += ["--config", str(path)]
        log = open(self.output_dir / f"{name}.log", "w")
        self.logs.append(log)
        # Ctrl+C 交由 Supervisor 处理，子进程按 exit -> terminate 的顺序有序退出。
        process = subprocess.Popen(command, stdout=log, stderr=log, env=self.environment, start_new_session=True)
        self.processes[name] = process
        deadline = time.monotonic() + 10
        while not (self.runtime_dir / f"rpc.{name}").exists():
            if self.stop_event.is_set():
                raise RuntimeError("supervisor stopping during startup")
            if process.poll() is not None:
                raise RuntimeError(f"{name} exited: {process.returncode}")
            if time.monotonic() >= deadline:
                raise TimeoutError(f"{name} RPC startup timed out")
            self.stop_event.wait(.02)

    def _state(self):
        state = dict(runtime_id=self.runtime_id, port=self.port, tasks=self.tasks, errors=self.errors,
            version=self.version, pids={name: process.pid for name, process in self.processes.items()})
        temporary = self.output_dir / "state.json.tmp"
        temporary.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n")
        temporary.replace(self.output_dir / "state.json")

    def _publish(self):
        self.client.request("sys", "runtime", "sys.runtime.publish", {"runtime_id": self.runtime_id, "tasks": self.tasks})
        self._state()

    def _bind(self):
        started = time.monotonic_ns()
        self.version += 1
        self.client.request(self.tasks["cockpit"], "inference", "cockpit.capabilities.bind", {
            "runtime_id": self.runtime_id, "version": self.version,
            "bindings": {name: self.tasks[name] for name in ("asr", "llm", "tts")}}, "cockpit.capabilities.bound", timeout=6)
        self._state()
        self._record("capabilities_bound", started, version=self.version)

    def start(self):
        self._spawn("sys")
        deadline = time.monotonic() + 10
        while True:
            try:
                self.client = Client(port=self.port, timeout=self.config.get("setup_timeout_seconds", 120))
                break
            except OSError:
                if self.stop_event.is_set():
                    raise RuntimeError("supervisor stopping during startup")
                if time.monotonic() >= deadline:
                    raise
                self.stop_event.wait(.02)
        self._spawn("cockpit")
        cockpit = dict(self.config.get("cockpit", {}), runtime_id=self.runtime_id, manager_port=self.port,
            manager_host="127.0.0.1", output_dir=str(self.output_dir),
            signal_sockets=[str(self.runtime_dir / "vehicle.sock"), str(self.runtime_dir / "occupant.sock")])
        self.tasks["cockpit"] = self.client.setup("cockpit", cockpit)
        self.client.probe(self.tasks["cockpit"])
        self._record("perception_ready", self.started_ns, work_id=self.tasks["cockpit"])
        self._publish()
        self._bind()
        if not self.models:
            return self
        # 模型加载互不等待；共用一个按 request_id 分发的管理连接。
        for name in ("asr", "llm", "tts"):
            try:
                self._spawn(name)
            except Exception as error:
                self.errors[name] = str(error)

        def setup_model(name):
            if name in self.errors:
                raise RuntimeError(self.errors[name])
            params = dict(self.config[name], enoutput=True)
            params.pop("input", None)
            if name == "tts":
                params["output_dir"] = str(self.output_dir / "audio")
            started = time.monotonic_ns()
            try:
                work = self.client.setup(name, params)
                self._record("model_setup", started, capability=name, work_id=work, outcome="created")
                return work
            except Exception as error:
                self._record("model_setup", started, capability=name, outcome="failed", reason=str(error))
                raise
        with ThreadPoolExecutor(max_workers=3, thread_name_prefix="model-setup") as pool:
            pending = {pool.submit(setup_model, name): name for name in ("asr", "llm", "tts")}
            for future in as_completed(pending):
                name = pending[future]
                try:
                    self.tasks[name] = future.result()
                except Exception as error:
                    self.errors[name] = str(error)
                self._publish()
                self._bind()
        return self

    def monitor_once(self):
        changed = False
        for name, process in list(self.processes.items()):
            if process.poll() is None or name in self.errors:
                continue
            self.errors[name] = f"process exited: {process.returncode}"
            if name in self.tasks:
                self.tasks[name] = None
            changed = True
        if changed:
            self._state()
            if self.processes["sys"].poll() is None and self.tasks["cockpit"]:
                try:
                    self._publish()
                    self._bind()
                except (OSError, RuntimeError, TimeoutError) as error:
                    self.errors["management"] = str(error)
                    self._state()

    def stop(self):
        if self.closed:
            return
        self.closed = True
        self.stop_event.set()
        if self.client:
            try:
                self.client.request("sys", "runtime", "sys.runtime.remove", {"runtime_id": self.runtime_id}, timeout=2)
            except (OSError, RuntimeError, TimeoutError):
                pass
            for name in ("cockpit", "tts", "llm", "asr"):
                work = self.tasks[name]
                if not work:
                    continue
                try:
                    self.client.request(work, "exit", "None", {}, "sys.exit", timeout=5)
                except (OSError, RuntimeError, TimeoutError) as error:
                    self.errors[f"stop_{name}"] = str(error)
            self.client.close()
        for process in reversed(list(self.processes.values())):
            if process.poll() is None:
                process.terminate()
        for process in reversed(list(self.processes.values())):
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for log in self.logs:
            log.close()
        self._state()
        fcntl.flock(self.lock, fcntl.LOCK_UN)
        self.lock.close()

    def __enter__(self):
        try:
            return self.start()
        except BaseException as error:
            self.errors["startup"] = str(error) or type(error).__name__
            self.stop()
            raise

    def __exit__(self, *unused):
        self.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/cockpit")
    parser.add_argument("--config", type=Path, default=ROOT / "configs/cockpit-x86.json")
    parser.add_argument("--voice-deps-root", type=Path, default=VOICE_DEPS_ROOT)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "artifacts/cockpit")
    parser.add_argument("--port", type=int)
    parser.add_argument("--no-models", action="store_true")
    parser.add_argument("--run-seconds", type=float)
    args = parser.parse_args()
    config = load_config(args.config, args.voice_deps_root)
    supervisor = Supervisor(args.build_dir, config, args.output_dir, args.port, not args.no_models)
    def stop(signum, frame):
        supervisor.stop_event.set()
        # 加载过程中也能中断管理等待；有序退出由 Supervisor 的 finally 完成。
        if supervisor.client:
            supervisor.client.close()
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    with supervisor:
        print(json.dumps({"runtime_id": supervisor.runtime_id, "port": supervisor.port, "tasks": supervisor.tasks}, ensure_ascii=False), flush=True)
        signal.signal(signal.SIGINT, lambda signum, frame: supervisor.stop_event.set())
        signal.signal(signal.SIGTERM, lambda signum, frame: supervisor.stop_event.set())
        deadline = time.monotonic() + args.run_seconds if args.run_seconds else float("inf")
        while time.monotonic() < deadline and not supervisor.stop_event.wait(.5):
            supervisor.monitor_once()


if __name__ == "__main__":
    main()
