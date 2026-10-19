#!/usr/bin/env python3
"""单会话 WAV -> ASR -> LLM -> TTS 演示，仅使用 Python 标准库。"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import uuid
import wave

ROOT = Path(__file__).resolve().parents[1]
VOICE_DEPS_ROOT = Path(os.environ.get("VOICE_DEPS_ROOT", ROOT / "voice-deps"))


class NodeError(RuntimeError):
    def __init__(self, message):
        self.message = message
        super().__init__(f"{message.get('work_id')}: {message['error']['message']}")


class Client:
    def __init__(self, host, port, timeout=120, log=None):
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.timeout = timeout
        self.buffer = bytearray()
        self.log = log

    def close(self):
        self.socket.close()

    def send(self, work, action, obj, data, request=None):
        request = request or uuid.uuid4().hex
        message = dict(request_id=request, work_id=work, action=action, object=obj, data=data)
        self.socket.sendall((json.dumps(message, ensure_ascii=False) + "\n").encode())
        return request

    def receive(self, deadline):
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("waiting for node output timed out")
            self.socket.settimeout(remaining)
            data = self.socket.recv(65536)
            if not data:
                raise ConnectionError("manager disconnected")
            self.buffer.extend(data)
        line, _, rest = self.buffer.partition(b"\n")
        self.buffer = bytearray(rest)
        message = json.loads(line)
        if self.log:
            self.log.write(json.dumps(message, ensure_ascii=False) + "\n")
            self.log.flush()
        return message

    def wait(self, request, predicate=lambda message: True, timeout=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while True:
            message = self.receive(deadline)
            if message.get("request_id") != request:
                continue
            if message.get("error", {}).get("code", -1):
                raise NodeError(message)
            if predicate(message):
                return message

    def setup(self, node, config):
        request = self.send(node, "setup", f"{node}.setup", config)
        return self.wait(request, lambda message: message["object"] == "sys.setup")["work_id"]

    def exit(self, work):
        request = self.send(work, "exit", "None", {})
        return self.wait(request, lambda message: message["object"] == "sys.exit", timeout=min(self.timeout, 30))

    def probe(self, first, expected):
        # PUB/SUB 建连异步。重复探测直到每一跳确认收到，业务首包才允许发送。
        request = "probe-" + uuid.uuid4().hex
        seen = set()
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            self.send(first, "inference", "sys.probe", {"nonce": request}, request)
            attempt = min(deadline, time.monotonic() + 0.25)
            try:
                while True:
                    message = self.receive(attempt)
                    if message.get("request_id") != request:
                        continue
                    if message["error"]["code"]:
                        raise NodeError(message)
                    if message["object"] == "sys.probe":
                        seen.add(message["work_id"])
                    if seen >= set(expected):
                        return
            except TimeoutError:
                pass
        raise TimeoutError(f"subscription probe timed out; received {sorted(seen)}")


class VoiceRuntime:
    """只托管本次演示启动的进程，退出时按 TTS、LLM、ASR 逆序释放任务。"""
    def __init__(self, build_dir, output_dir, timeout=120, port=0, llm_binary=None):
        self.bin_dir = Path(build_dir).resolve() / "bin"
        self.output_dir = Path(output_dir).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.timeout = timeout
        self.port = port
        self.llm_binary = llm_binary
        self.processes = []
        self.logs = []
        self.tasks = []
        self.client = None
        self.temp = None
        self.returncodes = []

    def __enter__(self):
        self.temp = tempfile.TemporaryDirectory(prefix="edge-voice-")
        runtime_dir = Path(self.temp.name)
        environment = os.environ.copy()
        environment["EDGE_RPC_DIR"] = str(runtime_dir)
        environment.setdefault("OMP_NUM_THREADS", "2")
        if not self.port:
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                self.port = reservation.getsockname()[1]
        manager_config = runtime_dir / "manager.json"
        manager_config.write_text(json.dumps({
            "config_tcp_server": self.port,
            "config_zmq_min_port": 5010,
            "config_zmq_max_port": 5555,
            "config_zmq_s_format": f"ipc://{runtime_dir}/%i.sock",
            "config_zmq_c_format": f"ipc://{runtime_dir}/%i.sock",
        }))
        try:
            for name in ("unit_manager", "edge_asr", "edge_llm", "edge_tts"):
                binary = Path(self.llm_binary) if name == "edge_llm" and self.llm_binary else self.bin_dir / name
                log = open(self.output_dir / f"{name}.log", "w")
                self.logs.append(log)
                command = [str(binary)]
                if name == "unit_manager":
                    command += ["--config", str(manager_config)]
                self.processes.append(subprocess.Popen(command, stdout=log, stderr=log, env=environment))
            deadline = time.monotonic() + self.timeout
            while True:
                self.check_processes()
                if all((runtime_dir / f"rpc.{node}").exists() for node in ("sys", "asr", "llm", "tts")):
                    try:
                        log = open(self.output_dir / "messages.jsonl", "w")
                        self.logs.append(log)
                        self.client = Client("127.0.0.1", self.port, self.timeout, log)
                        break
                    except OSError:
                        log.close()
                if time.monotonic() >= deadline:
                    raise TimeoutError("process startup timed out")
                time.sleep(0.05)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def check_processes(self):
        for process in self.processes:
            if process.poll() is not None:
                raise RuntimeError(f"node exited ({process.returncode}); inspect logs in {self.output_dir}")

    def setup(self, config):
        previous = None
        for name in ("asr", "llm", "tts"):
            params = dict(config[name])
            params["enoutput"] = True
            if previous:
                params["input"] = previous
            if name == "tts":
                params["output_dir"] = str(self.output_dir)
            previous = self.client.setup(name, params)
            self.tasks.append(previous)
        self.client.probe(self.tasks[0], self.tasks)
        return self.tasks

    def release(self):
        errors = []
        for work in reversed(self.tasks):
            try:
                self.client.exit(work)
            except Exception as error:
                errors.append(str(error))
        self.tasks.clear()
        if errors:
            raise RuntimeError("task cleanup: " + "; ".join(errors))

    def round(self, wav, request=None):
        request = self.client.send(self.tasks[0], "inference", "asr.audio.file", {"path": str(Path(wav).resolve())}, request)
        deadline = time.monotonic() + self.timeout
        result = {"request_id": request, "recognized_text": None, "answer": "", "audio": None}
        expected_index = 0
        finished = False
        while result["audio"] is None or not finished or result["recognized_text"] is None:
            self.check_processes()
            message = self.client.receive(deadline)
            if message.get("request_id") != request:
                continue
            if message["error"]["code"]:
                raise NodeError(message)
            obj, work = message["object"], message["work_id"]
            if obj == "asr.utf-8":
                if work != self.tasks[0] or result["recognized_text"] is not None:
                    raise AssertionError("ASR work_id or duplicate completion")
                result["recognized_text"] = message["data"]
            elif obj == "llm.utf-8.stream":
                fragment = message["data"]
                if work != self.tasks[1] or fragment["index"] != expected_index or finished:
                    raise AssertionError("LLM stream work_id/order/finish mismatch")
                expected_index += 1
                result["answer"] += fragment["delta"]
                finished = fragment["finish"]
            elif obj == "tts.audio.file":
                if work != self.tasks[2] or result["audio"] is not None:
                    raise AssertionError("TTS work_id or duplicate completion")
                result["audio"] = message["data"]
        with wave.open(result["audio"]["path"], "rb") as audio:
            assert (audio.getframerate(), audio.getnchannels(), audio.getsampwidth()) == (16000, 1, 2)
            assert audio.getnframes() == result["audio"]["samples"] > 0
            assert any(audio.readframes(audio.getnframes()))
        return result

    def __exit__(self, exc_type, exc, traceback):
        try:
            if self.client and self.tasks:
                self.release()
        except Exception as error:
            print(f"cleanup: {error}")
        if self.client:
            self.client.close()
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
        for process in reversed(self.processes):
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        self.returncodes = [process.returncode for process in self.processes]
        for log in self.logs:
            log.close()
        if self.temp:
            self.temp.cleanup()


def load_config(path, voice_deps_root):
    config = json.loads(Path(path).read_text())
    root = str(Path(voice_deps_root).resolve())

    def expand(value):
        if isinstance(value, str):
            return value.replace("${VOICE_DEPS_ROOT}", root)
        if isinstance(value, dict):
            return {key: expand(item) for key, item in value.items()}
        if isinstance(value, list):
            return [expand(item) for item in value]
        return value

    return expand(config)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/host")
    parser.add_argument("--voice-deps-root", type=Path, default=VOICE_DEPS_ROOT)
    parser.add_argument("--config", type=Path, default=ROOT / "configs/voice-x86.json")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "artifacts/voice-demo")
    parser.add_argument("--wav", type=Path, action="append")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    if args.rounds < 1 or args.cycles < 1 or args.timeout <= 0:
        parser.error("rounds, cycles and timeout must be positive")
    config = load_config(args.config, args.voice_deps_root)
    wavs = args.wav or [Path(config["asr"]["model_dir"]) / "test_wavs/0.wav"]
    results = []
    with VoiceRuntime(args.build_dir, args.output_dir, args.timeout, args.port) as runtime:
        for cycle in range(args.cycles):
            runtime.setup(config)
            for index in range(args.rounds):
                result = runtime.round(wavs[index % len(wavs)], f"cycle-{cycle+1}-round-{index+1}-{uuid.uuid4().hex[:8]}")
                results.append(result)
                print(json.dumps(result, ensure_ascii=False), flush=True)
            runtime.release()
    if any(runtime.returncodes):
        raise RuntimeError(f"unclean process shutdown: {runtime.returncodes}")
    args.output_dir.joinpath("results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
