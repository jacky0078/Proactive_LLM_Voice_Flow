#!/usr/bin/env python3
"""真实 ASR/TTS、独立 Supervisor 和主动座舱的协议/生命周期验收。"""
import argparse
from contextlib import contextmanager
from copy import deepcopy
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sample"))
from cockpit_client import Client, Interaction, NodeError
from cockpit_replay import SignalReplay
from cockpit_supervisor import Supervisor
from voice_demo import VOICE_DEPS_ROOT, load_config


def eventually(function, predicate=bool, seconds=5):
    deadline = time.monotonic() + seconds
    while True:
        result = function()
        if predicate(result):
            return result
        if time.monotonic() >= deadline:
            raise AssertionError(f"condition did not become true: {result}")
        time.sleep(.02)


def expect_error(function, contains):
    try:
        function()
    except NodeError as error:
        assert contains in str(error), str(error)
        return
    raise AssertionError(f"expected error containing {contains}")


class Fixture:
    def __init__(self, supervisor):
        self.s = supervisor
        self.client = None
        self.ui = None
        self.replay = None
        self.events = []

    def command(self, obj, data=None):
        return self.s.client.request(self.s.tasks["cockpit"], "inference", obj, data or {})["data"]

    def status(self):
        return self.command("cockpit.status")

    def connect(self):
        self.client = Client(port=self.s.port, timeout=10)
        self.ui = Interaction(self.client)
        return self.ui

    def play(self, name, extra=None):
        updates = json.loads((ROOT / f"configs/scenarios/{name}.json").read_text())["updates"]
        # 保持原场景时序；缩短观察时长用于异常测试。
        self.replay = SignalReplay([self.s.runtime_dir / "vehicle.sock", self.s.runtime_dir / "occupant.sock"],
                                   updates + (extra or [])).start()

    def event(self, phase, seconds=8):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            message = self.client.next_event(max(.01, deadline - time.monotonic()))
            self.events.append(message)
            if message["data"]["phase"] == phase:
                return message["data"]
            if message["data"]["phase"] in ("FAILED", "CANCELLED", "EXPIRED", "UNKNOWN"):
                raise AssertionError(f"unexpected service terminal: {message}")
        raise AssertionError(f"missing service phase: {phase}")

    def close(self):
        if self.replay:
            self.replay.close()
        if self.ui:
            self.ui.close()
        if self.client:
            self.client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/cockpit")
    parser.add_argument("--voice-deps-root", type=Path, default=VOICE_DEPS_ROOT)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "artifacts/cockpit-validation")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    base = load_config(ROOT / "configs/cockpit-x86.json", args.voice_deps_root)
    base["cockpit"].update(heat_observe_ms=100, rest_observe_ms=150, model_timeout_ms=15000,
                           confirmation_timeout_ms=1800, execution_timeout_ms=500)
    checks = []

    @contextmanager
    def fixture(name, config=None, models=True):
        with tempfile.TemporaryDirectory(prefix="cc-it-") as directory:
            s = Supervisor(args.build_dir, deepcopy(config or base), directory, port=0, models=models)
            f = Fixture(s)
            try:
                with s:
                    try:
                        yield f
                    finally:
                        f.close()
            finally:
                (Path(directory) / "events.json").write_text(json.dumps(f.events, ensure_ascii=False, indent=2))
                destination = args.output_dir / name
                # 只复制常规证据文件，运行时 Unix socket 不属于交付物。
                destination.mkdir(exist_ok=True)
                for path in Path(directory).iterdir():
                    if path.is_file():
                        shutil.copy2(path, destination / path.name)
                    elif path.name == "audio":
                        shutil.copytree(path, destination / path.name, dirs_exist_ok=True)

    def passed(text):
        checks.append(text)
        print("PASS:", text, flush=True)

    with tempfile.TemporaryDirectory(prefix="cc-restart-") as directory:
        runtime = Path(directory) / "runtime"
        runtime.mkdir()
        # 关闭 FD 后仍留下路径，复现异常退出后的 Unix Socket 文件。
        for name, kind in (("vehicle.sock", socket.SOCK_DGRAM), ("rpc.sys", socket.SOCK_STREAM)):
            with socket.socket(socket.AF_UNIX, kind) as channel:
                channel.bind(str(runtime / name))
        note = runtime / "keep.txt"
        note.write_text("preserve ordinary files")
        for _ in range(2):
            with Supervisor(args.build_dir, deepcopy(base), directory, port=0, models=False) as supervisor:
                status = supervisor.client.request(supervisor.tasks["cockpit"], "inference", "cockpit.status", {})["data"]
                assert status["perception_ready"]
                assert note.read_text() == "preserve ordinary files"
            assert not (runtime / "vehicle.sock").exists()
            assert not (runtime / "occupant.sock").exists()
        passed("stale datagram/stream sockets are cleaned; same-directory restart preserves ordinary files")

    with tempfile.TemporaryDirectory(prefix="cc-owner-") as directory:
        runtime = Path(directory) / "runtime"
        runtime.mkdir()
        stale = runtime / "vehicle.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as channel:
            channel.bind(str(stale))
        live = runtime / "occupant.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as owner:
            owner.bind(str(live))
            try:
                supervisor = Supervisor(args.build_dir, deepcopy(base), directory, port=0, models=False)
            except RuntimeError as error:
                assert "still in use" in str(error)
            else:
                supervisor.stop()
                raise AssertionError("active runtime socket was taken over")
            assert live.is_socket() and stale.is_socket()
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
                sender.sendto(b"owner preserved", str(live))
            owner.settimeout(1)
            assert owner.recv(64) == b"owner preserved"
        passed("live socket owner blocks startup before any stale files are deleted")

    with tempfile.TemporaryDirectory(prefix="cc-conflict-") as directory:
        runtime = Path(directory) / "runtime"
        runtime.mkdir()
        conflict = runtime / "vehicle.sock"
        conflict.write_text("not a socket")
        try:
            with Supervisor(args.build_dir, deepcopy(base), directory, port=0, models=False):
                raise AssertionError("ordinary file at socket path was overwritten")
        except NodeError as error:
            assert "cannot bind ingress socket" in str(error)
        assert conflict.read_text() == "not a socket"
        state = json.loads((Path(directory) / "state.json").read_text())
        assert "cannot bind ingress socket" in state["errors"]["startup"]
        passed("ordinary path conflicts are preserved and startup failure is recorded in state.json")

    # 向整个终端进程组发送 SIGINT，模拟用户按 Ctrl+C，而非只调用 stop()。
    with tempfile.TemporaryDirectory(prefix="cc-signal-") as directory:
        log_path = Path(directory) / "console.log"
        with open(log_path, "w") as output:
            process = subprocess.Popen(
                [sys.executable, str(ROOT / "sample/cockpit_supervisor.py"),
                 "--build-dir", str(args.build_dir), "--voice-deps-root", str(args.voice_deps_root),
                 "--output-dir", directory, "--port", "0"],
                stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                eventually(log_path.read_text, lambda text: '"tasks"' in text, seconds=20)
                state = json.loads((Path(directory) / "state.json").read_text())
                assert all(state["tasks"].values()) and not state["errors"], state
                assert all(os.getpgid(pid) != process.pid for pid in state["pids"].values())
                os.killpg(process.pid, signal.SIGINT)
                assert process.wait(timeout=15) == 0, log_path.read_text()
                state = json.loads((Path(directory) / "state.json").read_text())
                assert not state["errors"], state
                assert all(not Path(f"/proc/{pid}").exists() for pid in state["pids"].values())
                assert not (Path(directory) / "runtime/vehicle.sock").exists()
                assert not (Path(directory) / "runtime/occupant.sock").exists()
                rows = map(json.loads, (Path(directory) / "traces.jsonl").read_text().splitlines())
                assert any(row.get("kind") == "summary" for row in rows)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=30)
        passed("terminal Ctrl+C exits through Supervisor; child processes stop and final metrics are flushed")

    with fixture("autonomous", models=False) as f:
        assert f.status()["perception_ready"] and not f.status()["interaction_ready"]
        f.play("heat")
        eventually(f.status, lambda state: state["signals"]["cabin.temperature"]["value"] == 30)
        assert all(value["state"] == "UNBOUND" for value in f.status()["capabilities"].values())
        before = dict(f.s.tasks)
        f.connect()
        assert f.status()["interaction_ready"]
        f.ui.close()
        f.ui = None
        assert not f.status()["interaction_ready"] and before == f.s.tasks
        before_updates = f.status()["updates"]
        eventually(f.status, lambda state: state["updates"] > before_updates)
        passed("no model/UI required for startup and perception; UI disconnect leaves tasks and Context alive")

    with fixture("heat") as f:
        assert not f.s.errors, f.s.errors
        assert all(value["state"] == "READY" for value in f.status()["capabilities"].values())
        f.connect()
        # 同一版本幂等；冲突、旧版本不能改变当前绑定。
        data = dict(runtime_id=f.s.runtime_id, version=f.s.version,
                    bindings={key: f.s.tasks[key] for key in ("asr", "llm", "tts")})
        assert f.command("cockpit.capabilities.bind", data)["version"] == f.s.version
        expect_error(lambda: f.command("cockpit.capabilities.bind", dict(data, version=1)), "version")
        conflict = dict(data, bindings=dict(data["bindings"], llm=None))
        expect_error(lambda: f.command("cockpit.capabilities.bind", conflict), "conflict")
        f.play("heat")
        prompt = f.event("WAITING_CONFIRMATION")
        with wave.open(prompt["audio"]["path"]) as audio:
            assert audio.getnframes() > 0
        expect_error(lambda: f.ui.feedback(prompt["service_id"] + 1, "confirm"), "awaiting")
        f.ui.feedback(prompt["service_id"], "confirm")
        expect_error(lambda: f.ui.feedback(prompt["service_id"], "confirm"), "awaiting")
        f.event("SUCCEEDED")
        assert f.status()["signals"]["hvac.on"]["value"] == 1
        # 普通 WAV 由核心编排，不配置模型之间的自动订阅。
        request = f.ui.speech(Path(base["asr"]["model_dir"]) / "test_wavs/2.wav")
        answer = f.client.wait(request, lambda message: message["object"] == "tts.audio.file", timeout=15)
        f.client.forget(request)
        assert Path(answer["data"]["path"]).is_file()
        passed("real ASR/TTS question; heat confirmation, duplicate rejection, vehicle readback; binding versions")

    # 使用真正的命令行客户端，覆盖提前回车、无效输入及服务结束后的按键。
    for name, answer, terminal in (("console-confirm", "Y", "SUCCEEDED"),
                                    ("console-reject", "n", "CANCELLED"),
                                    ("console-cancel", "c", "CANCELLED"),
                                    ("console-timeout", None, "EXPIRED")):
        with fixture(name) as f:
            log_path = f.s.output_dir / "console.log"
            with open(log_path, "w") as output:
                console = subprocess.Popen(
                    [sys.executable, str(ROOT / "sample/cockpit_client.py"), "--port", str(f.s.port)],
                    stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT, text=True)
                try:
                    eventually(f.status, lambda state: state["interaction_ready"])
                    console.stdin.write("\n")
                    console.stdin.flush()
                    f.play("heat")
                    eventually(log_path.read_text, lambda text: "WAITING_CONFIRMATION" in text)
                    if answer is not None:
                        console.stdin.write("\n   \ninvalid\n")
                        console.stdin.flush()
                        eventually(log_path.read_text, lambda text: "未发送反馈" in text)
                        assert f.status()["service"]["state"] == 1  # 仍在等待确认。
                        assert f.status()["signals"]["hvac.on"]["value"] == 0
                        console.stdin.write(answer + "\n")
                        console.stdin.flush()
                    eventually(log_path.read_text, lambda text: f'"phase": "{terminal}"' in text)
                    assert f.status()["signals"]["hvac.on"]["value"] == (1 if answer == "Y" else 0)
                    console.stdin.write("y\n")
                    console.stdin.flush()
                    eventually(log_path.read_text, lambda text: "当前没有待确认的服务" in text)
                    assert console.poll() is None, log_path.read_text()
                finally:
                    console.stdin.close()
                    try:
                        console.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        console.terminate()
                        console.wait(timeout=5)
                assert console.returncode == 0, log_path.read_text()
            passed(f"{name}: blank/invalid input sends no feedback; terminal events and late input remain visible")

    with fixture("rest") as f:
        f.connect()
        f.play("rest")
        prompt = f.event("WAITING_CONFIRMATION")
        assert prompt["service"] == "rest"
        f.ui.feedback(prompt["service_id"], "confirm")
        f.event("SUCCEEDED")
        passed("fatigue fast path completes without LLM decision")

    with fixture("comfort") as f:
        f.connect()
        f.play("comfort")
        prompt = f.event("WAITING_CONFIRMATION")
        assert prompt["service"] == "air_conditioner"
        f.ui.feedback(prompt["service_id"], "confirm")
        f.event("SUCCEEDED")
        passed("two discomfort events produce validated structured decision and confirmed service")

    for name, mode, terminal in (("vehicle-failure", "failure", "FAILED"),
                                 ("vehicle-timeout", "timeout", "UNKNOWN")):
        config = deepcopy(base)
        config["cockpit"]["vehicle"]["mode"] = mode
        with fixture(name, config) as f:
            f.connect()
            f.play("heat")
            prompt = f.event("WAITING_CONFIRMATION")
            f.ui.feedback(prompt["service_id"], "confirm")
            f.event(terminal)
            assert f.status()["signals"]["hvac.on"]["value"] == 0
            passed(f"vehicle {mode} ends as {terminal}; no invented readback or automatic retry")

    for name, action, terminal in (("rejected", "reject", "CANCELLED"),
                                   ("no-response", None, "EXPIRED")):
        with fixture(name) as f:
            f.connect()
            f.play("heat")
            prompt = f.event("WAITING_CONFIRMATION")
            if action:
                f.ui.feedback(prompt["service_id"], action)
            f.event(terminal)
            assert f.status()["signals"]["hvac.on"]["value"] == 0
            passed(f"user {name} cannot execute vehicle service")

    with fixture("voice-feedback-and-condition-change") as f:
        f.connect()
        f.play("heat")
        prompt = f.event("WAITING_CONFIRMATION")
        request = f.ui.speech(Path(base["asr"]["model_dir"]) / "test_wavs/2.wav", prompt["service_id"])
        feedback = f.client.wait(request, lambda message: message["object"] == "cockpit.feedback")
        f.client.forget(request)
        assert not feedback["data"]["accepted"], feedback
        # 管理入口仍必须经过 socket bridge 和同一个 Ingress。
        f.command("cockpit.signal.batch", {"updates": [
            {"name": "driver.present", "value": 0, "sequence": 1000000, "source_id": 0},
            {"name": "driver.present", "value": 1, "sequence": 1000001, "source_id": 0}]})
        f.event("CANCELLED")
        expect_error(lambda: f.ui.feedback(prompt["service_id"], "confirm"), "awaiting")
        assert f.status()["signals"]["hvac.on"]["value"] == 0
        passed("real ASR ambiguous feedback does not confirm; brief evidence loss cancels waiting service")

    config = deepcopy(base)
    config["asr"]["model_dir"] = "/nonexistent/cockpit-asr-test"
    with fixture("partial-capabilities", config) as f:
        assert "asr" in f.s.errors
        states = f.status()["capabilities"]
        assert states["asr"]["state"] == "UNBOUND" and states["tts"]["state"] == "READY"
        f.connect()
        f.play("heat")
        prompt = f.event("WAITING_CONFIRMATION")
        f.ui.feedback(prompt["service_id"], "reject")
        f.event("CANCELLED")
        passed("ASR setup failure permits perception and TTS/text interaction without rebuilding tasks")

    config = deepcopy(base)
    config["cockpit"]["session_lease_ms"] = 700
    with fixture("lease-and-reconnect", config) as f:
        f.connect()
        old_token = f.ui.token
        f.ui.stop_event.set()
        f.ui.renewal.join()
        f.client.close()  # 不发送 unbind，覆盖异常断线。
        eventually(f.status, lambda state: not state["interaction_ready"])
        f.ui = None
        f.connect()
        expect_error(lambda: f.client.request(f.ui.work, "inference", "cockpit.session.renew",
                                              {"session_token": old_token}), "invalid")
        assert f.status()["perception_ready"]
        passed("lease expiry, reconnect and stale-token rejection preserve perception/task lifecycle")

    for mode in ("invalid", "no_action", "timeout"):
        config = deepcopy(base)
        config["llm"]["mock_decision_mode"] = mode
        config["cockpit"]["llm_timeout_ms"] = 300
        with fixture(f"decision-{mode}", config) as f:
            f.connect()
            f.play("comfort")
            eventually(lambda: (Path(f.s.output_dir) / "traces.jsonl").read_text(),
                       lambda text: any(row.get("kind") == "candidate" and row.get("need") == "comfort"
                                        for row in map(json.loads, text.splitlines())), seconds=5)
            eventually(f.status, lambda state: state["candidates"] == 0 and state["pending_calls"] == 0)
            assert f.status()["service"] is None
            if mode == "timeout":
                assert f.status()["capabilities"]["llm"]["state"] == "READY"
                # 已取消的同一 Task 可以再次处理普通请求。
                request = f.s.client.send(f.s.tasks["llm"], "inference", "llm.utf-8", "继续")
                f.s.client.wait(request, lambda message: message["object"] == "llm.utf-8.stream" and message["data"]["finish"])
                f.s.client.forget(request)
            passed(f"structured decision {mode}: no unsafe service; cancellation recovers same LLM task")

    config = deepcopy(base)
    config["llm"]["mock_delay_ms"] = 1600
    config["cockpit"]["llm_timeout_ms"] = 3000
    with fixture("slow-fast-and-degrade", config) as f:
        f.connect()
        # 后排慢决策在运行时，驾驶员疲劳快路径仍独立确认并发起询问。
        f.play("comfort", [{"at_ms": 1300, "name": "driver.fatigue", "value": .9}])
        prompt = f.event("WAITING_CONFIRMATION")
        assert prompt["service"] == "rest", prompt
        f.ui.feedback(prompt["service_id"], "reject")
        f.event("CANCELLED")
        f.s.processes["llm"].terminate()
        f.s.processes["llm"].wait(timeout=5)
        f.s.monitor_once()
        assert f.status()["capabilities"]["llm"]["state"] == "UNBOUND"
        before = f.status()["updates"]
        eventually(f.status, lambda state: state["updates"] > before)
        assert f.s.tasks["llm"] is None
        passed("fast path progresses during slow LLM; process failure degrades binding without restart or perception loss")

    report = {"checks": checks, "hardware_inference_verified": False,
              "model_backends": {"asr": "sherpa-onnx", "llm": "mock", "tts": "SummerTTS"}}
    (args.output_dir / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
