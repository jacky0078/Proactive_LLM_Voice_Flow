#!/usr/bin/env python3
"""Real ASR/TTS integration, protocol failures, and the real adapter linked to a test SDK."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import time
import uuid
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sample"))
from voice_demo import NodeError, VoiceRuntime, VOICE_DEPS_ROOT, load_config


def expect_error(function, contains=None):
    try:
        function()
    except NodeError as error:
        if contains and contains not in str(error):
            raise AssertionError(f"expected {contains!r}, received {error}") from error
        return error.message
    raise AssertionError("expected a structured node error")


def stream(client, work, request, index, text="", finish=False):
    client.send(work, "inference", "llm.utf-8.stream", dict(index=index, delta=text, finish=finish), request)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/host")
    parser.add_argument("--voice-deps-root", type=Path, default=VOICE_DEPS_ROOT)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "artifacts/integration")
    args = parser.parse_args()
    config = load_config(ROOT / "configs/voice-x86.json", args.voice_deps_root)
    reports = []
    results = []
    with tempfile.TemporaryDirectory(prefix="edge-fixtures-") as fixture_dir:
        fixtures = Path(fixture_dir)
        for name in ("bad.wav", "bad-model.bin", "valid.rkllm", "init-fail.rkllm"):
            (fixtures / name).write_bytes(b"not a real model or audio")
        (fixtures / "bad-model.bin").write_bytes(bytes(32))
        for name, channels in (("silent.wav", 1), ("stereo.wav", 2)):
            with wave.open(str(fixtures / name), "wb") as wav:
                wav.setparams((channels, 2, 16000, 0, "NONE", "not compressed"))
                wav.writeframes(b"\0\0" * 16000 * channels)
        with VoiceRuntime(args.build_dir, args.output_dir / "host") as runtime:
            client = runtime.client
            for name, params, expected in (
                ("asr", {"model_dir": str(fixtures)}, "model file missing"),
                ("tts", {"model_path": str(fixtures / "bad-model.bin"), "output_dir": str(fixtures)}, "checksum mismatch"),
                ("llm", {"backend": "rkllm", "model_path": str(fixtures / "valid.rkllm")}, "no RKLLM runtime"),
                ("llm", {"backend": "other"}, "backend must be"),
            ):
                expect_error(lambda: client.setup(name, params), expected)
            reports.append("invalid ASR/TTS models and unavailable RKLLM return errors without mock fallback")
            previous_tasks = set()
            for cycle in range(2):
                runtime.setup(config)
                assert not previous_tasks.intersection(runtime.tasks)
                previous_tasks.update(runtime.tasks)
                for index in range(3):
                    wav = Path(config["asr"]["model_dir"]) / f"test_wavs/{index}.wav"
                    results.append(runtime.round(wav, f"validation-{cycle}-{index}"))
                if cycle == 0:
                    # 两个完整 JSON 粘在一起，首包按字节截断，截断点位于中文 UTF-8 内。
                    requests = []
                    for paused in (True, False):
                        request = uuid.uuid4().hex
                        requests.append(request)
                        body = dict(request_id=request, work_id=runtime.tasks[1], action="pause",
                                    object="sys.pause", data={"paused": paused, "escaped": '你好"\\\n'})
                        encoded = (json.dumps(body, ensure_ascii=False) + "\n").encode()
                        if paused:
                            cut = encoded.index("你好".encode()) + 1
                            client.socket.sendall(encoded[:cut])
                            try:
                                client.receive(time.monotonic() + 0.05)
                            except TimeoutError:
                                pass
                            else:
                                raise AssertionError("server processed an incomplete JSON frame")
                            pending = encoded[cut:]
                        else:
                            client.socket.sendall(pending + encoded)
                    for request, paused in zip(requests, (True, False)):
                        assert client.wait(request)["data"]["paused"] == paused
                    reports.append("TCP split UTF-8/escaped JSON and coalesced lifecycle frames; pause/resume")
                    client.socket.sendall(b'{"broken":\n')
                    expect_error(lambda: client.wait("0"))
                    request = client.send("missing_node", "setup", "None", {})
                    expect_error(lambda: client.wait(request), "RPC failed")
                    reports.append("malformed JSON and unavailable node RPC failures are structured")
                    for name, expected in (("bad.wav", "RIFF"), ("missing.wav", "cannot open WAV"),
                                           ("silent.wav", "silent"), ("stereo.wav", "mono")):
                        expect_error(lambda: runtime.round(fixtures / name), expected)
                    # 流水线错误传播完成后，下一轮仍能成功，且请求号独立。
                    results.append(runtime.round(Path(config["asr"]["model_dir"]) / "test_wavs/2.wav", "after-audio-errors"))
                    reports.append("invalid/missing/stereo/silent WAV and successful recovery")
                old = runtime.tasks[0]
                runtime.release()
                request = client.send(old, "inference", "sys.probe", {})
                expect_error(lambda: client.wait(request), "task not found")
            reports.append("two complete setup/inference/exit cycles, three distinct real ASR/TTS rounds each, registry release")
            tts_config = dict(config["tts"], output_dir=str(fixtures / "tts-output"), stream_timeout_seconds=0.2)
            work = client.setup("tts", tts_config)
            runtime.tasks = [work]
            client.probe(work, [work])
            request = "missing-fragment"
            stream(client, work, request, 1, "你好。", True)
            expect_error(lambda: client.wait(request), "fragment")
            request = "missing-finish"
            stream(client, work, request, 0)
            expect_error(lambda: client.wait(request), "timed out")
            directory = fixtures / "tts-output"
            assert not list(directory.glob("*.part"))
            directory.rmdir()
            directory.write_text("this is a regular file")
            request = "write-failure"
            stream(client, work, request, 0, "你好。", True)
            expect_error(lambda: client.wait(request))
            directory.unlink()
            directory.mkdir()
            request = "tts-recovery"
            stream(client, work, request, 0, "你好。", True)
            assert client.wait(request)["object"] == "tts.audio.file"
            stream(client, work, request, 0, "你好。", True)
            try:
                client.wait(request, timeout=0.2)
            except TimeoutError:
                pass
            else:
                raise AssertionError("duplicate finish was accepted")
            runtime.release()
            reports.append("TTS missing fragment/finish, timeout, write failure, recovery, duplicate completion suppression")
            work = client.setup("llm", {"backend": "mock", "mock_delay_ms": 500, "enoutput": True})
            runtime.tasks = [work]
            client.probe(work, [work])
            inference = client.send(work, "inference", "llm.utf-8", "异步请求")
            info = client.send(work, "taskinfo", "None", {})
            assert client.wait(info)["object"] == "sys.taskinfo"
            result = client.wait(inference, lambda message: message["data"]["finish"])
            assert result["work_id"] == work
            reports.append("async inference retains its request_id while lifecycle requests execute")
            request = client.send(work, "inference", "llm.utf-8", "你好")
            try:
                client.wait(request, timeout=0.05)
            except TimeoutError:
                pass
            else:
                raise AssertionError("expected client operation timeout")
            started = time.monotonic()
            runtime.release()
            assert time.monotonic() - started < 5
            reports.append("client operation timeout followed by task cancellation and clean exit")
        assert runtime.returncodes == [0, 0, 0, 0], runtime.returncodes
        sdk_binary = args.build_dir / "bin/edge_llm_sdk_test"
        with VoiceRuntime(args.build_dir, args.output_dir / "sdk", llm_binary=sdk_binary.resolve()) as runtime:
            client = runtime.client
            expect_error(lambda: client.setup("llm", {"backend": "rkllm", "model_path": str(fixtures / "init-fail.rkllm")}), "rkllm_init")
            work = client.setup("llm", {"backend": "rkllm", "model_path": str(fixtures / "valid.rkllm"), "enoutput": True})
            runtime.tasks = [work]
            client.probe(work, [work])
            for prompt in ("normal", "error", "return-fail", "missing-finish", "normal"):
                request = client.send(work, "inference", "llm.utf-8", prompt)
                parts = []
                def collect():
                    deadline = time.monotonic() + 10
                    while True:
                        message = client.receive(deadline)
                        if message["request_id"] != request:
                            continue
                        assert message["work_id"] == work
                        if message["error"]["code"]:
                            raise NodeError(message)
                        assert message["data"]["index"] == len(parts)
                        parts.append(message["data"]["delta"])
                        if message["data"]["finish"]:
                            return
                if prompt == "normal":
                    collect()
                    assert "".join(parts) == "你好，我是车载语音助手。"
                else:
                    expect_error(collect, "rkllm_run")
            client.send(work, "inference", "llm.utf-8", "abort")
            time.sleep(0.05)
            started = time.monotonic()
            runtime.release()
            assert time.monotonic() - started < 5
        assert runtime.returncodes == [0, 0, 0, 0], runtime.returncodes
        reports.append("StackFlow rkllm backend lifecycle through SDK test double: init failure, streaming, callback/run errors, abort/destroy")
    report = {"checks": reports, "audio_rounds": results, "hardware_inference_verified": False}
    args.output_dir.joinpath("report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    for item in reports:
        print("PASS:", item)


if __name__ == "__main__":
    main()
