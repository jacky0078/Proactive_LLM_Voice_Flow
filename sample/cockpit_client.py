#!/usr/bin/env python3
"""交互端只绑定会话，不创建或释放系统 Task。"""
import argparse
from collections import deque
import json
import os
import select
from pathlib import Path
import socket
import sys
import threading
import time
import uuid


class NodeError(RuntimeError):
    def __init__(self, message):
        self.message = message
        super().__init__(message.get("error", {}).get("message", "node request failed"))


class Client:
    """一个接收线程按 request_id 分发，主动事件有单独的有界收件箱。"""
    def __init__(self, host="127.0.0.1", port=10001, timeout=120):
        self.socket = socket.create_connection((host, port), timeout=min(timeout, 2))
        self.socket.settimeout(0.5)
        self.timeout = timeout
        self.prefix = uuid.uuid4().hex
        self.counter = 0
        self.condition = threading.Condition()
        self.send_lock = threading.Lock()
        self.responses = {}
        self.events = deque()
        self.failure = None
        self.stopped = threading.Event()
        self.reader = threading.Thread(target=self._read, name="cockpit-client-receive", daemon=True)
        self.reader.start()

    def close(self):
        self.stopped.set()
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.socket.close()
        self.reader.join(timeout=2)
        with self.condition:
            self.failure = self.failure or ConnectionError("client closed")
            self.condition.notify_all()

    def _read(self):
        pending = bytearray()
        try:
            while not self.stopped.is_set():
                try:
                    data = self.socket.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    raise ConnectionError("manager disconnected")
                pending.extend(data)
                if len(pending) > 2 * 1024 * 1024:
                    raise RuntimeError("manager input buffer limit")
                while b"\n" in pending:
                    line, _, pending = pending.partition(b"\n")
                    message = json.loads(line)
                    with self.condition:
                        if message.get("object") == "cockpit.service.event":
                            queue = self.events
                        else:
                            queue = self.responses.get(message.get("request_id"))
                        if queue is not None:
                            if len(queue) >= 256:
                                raise RuntimeError("client inbox full")
                            queue.append(message)
                        self.condition.notify_all()
        except (OSError, ValueError, RuntimeError) as error:
            with self.condition:
                self.failure = error
                self.condition.notify_all()

    def send(self, work, action, obj, data, request=None):
        with self.send_lock:
            with self.condition:
                if self.failure:
                    raise self.failure
                if request is None:
                    self.counter += 1
                    request = f"{self.prefix}-{self.counter}"
                if request not in self.responses:
                    if len(self.responses) >= 128:
                        raise RuntimeError("too many pending client requests")
                    self.responses[request] = deque()
            message = dict(request_id=request, work_id=work, action=action, object=obj, data=data)
            self.socket.sendall((json.dumps(message, ensure_ascii=False) + "\n").encode())
        return request

    def wait(self, request, predicate=lambda message: True, timeout=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        with self.condition:
            while True:
                queue = self.responses.get(request)
                if queue is None:
                    raise KeyError(request)
                while queue:
                    message = queue.popleft()
                    if message.get("error", {}).get("code", 0):
                        raise NodeError(message)
                    if predicate(message):
                        return message
                if self.failure:
                    raise self.failure
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"request timed out: {request}")
                self.condition.wait(remaining)

    def forget(self, request):
        with self.condition:
            self.responses.pop(request, None)

    def request(self, work, action, obj, data, expected=None, timeout=None):
        request = self.send(work, action, obj, data)
        try:
            return self.wait(request, lambda message: expected is None or message["object"] == expected, timeout)
        finally:
            self.forget(request)

    def setup(self, node, config):
        return self.request(node, "setup", f"{node}.setup", config, "sys.setup")["work_id"]

    def probe(self, work, timeout=5):
        request = self.send(work, "inference", "sys.probe", {})
        deadline = time.monotonic() + timeout
        try:
            while True:
                try:
                    return self.wait(request, lambda message: message["object"] == "sys.probe", min(.25, max(.001, deadline - time.monotonic())))
                except TimeoutError:
                    if time.monotonic() >= deadline:
                        raise
                    self.send(work, "inference", "sys.probe", {}, request)
        finally:
            self.forget(request)

    def next_event(self, timeout=1):
        deadline = time.monotonic() + timeout
        with self.condition:
            while not self.events:
                if self.failure:
                    raise self.failure
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("waiting for cockpit event")
                self.condition.wait(remaining)
            return self.events.popleft()


class Interaction:
    def __init__(self, client, client_id="console"):
        self.client = client
        runtime = client.request("sys", "runtime", "sys.runtime.lookup", {})["data"]
        self.work = runtime["tasks"]["cockpit"]
        client.probe(self.work)
        binding = client.request(self.work, "inference", "cockpit.session.bind", {"client_id": client_id})["data"]
        self.token = binding["session_token"]
        self.stop_event = threading.Event()
        self.failure = None
        interval = min(2, binding["lease_ms"] / 3000)
        self.renewal = threading.Thread(target=self._renew, args=(interval,), daemon=True)
        self.renewal.start()

    def _renew(self, interval):
        while not self.stop_event.wait(interval):
            try:
                self.client.request(self.work, "inference", "cockpit.session.renew", {"session_token": self.token}, timeout=2)
            except (OSError, RuntimeError, TimeoutError) as error:
                self.failure = error
                return

    def feedback(self, service, action):
        return self.client.request(self.work, "inference", "cockpit.feedback", {
            "session_token": self.token, "service_id": service, "action": action})

    def speech(self, path, service=None):
        data = {"session_token": self.token, "path": str(Path(path).resolve())}
        if service is not None:
            data["service_id"] = service
        return self.client.send(self.work, "inference", "cockpit.speech.file", data)

    def close(self):
        self.stop_event.set()
        self.renewal.join(timeout=3)
        try:
            self.client.request(self.work, "inference", "cockpit.session.unbind", {"session_token": self.token}, timeout=2)
        except (OSError, RuntimeError, TimeoutError):
            pass


def run_console(client, interaction):
    # 同时接收服务事件和终端输入，等待按键时也能显示取消、超时等终态。
    pending_service = None
    buffered = b""
    prompt = "确认请输入 y；拒绝请输入 n；取消请输入 c，然后回车："
    print("已连接座舱，等待主动询问。", flush=True)
    while True:
        if interaction.failure:
            raise interaction.failure
        while True:
            try:
                message = client.next_event(timeout=0)
            except TimeoutError:
                break
            print(json.dumps(message, ensure_ascii=False), flush=True)
            data = message["data"]
            if data["phase"] == "WAITING_CONFIRMATION":
                pending_service = data["service_id"]
                print(prompt, flush=True)
            elif data["service_id"] == pending_service:
                pending_service = None
                buffered = b""

        readable, _, _ = select.select([sys.stdin], [], [], .05)
        if not readable:
            continue
        # 直接读取 FD，避免 TextIO 的预读缓存使粘贴的多行输入滞留。
        chunk = os.read(sys.stdin.fileno(), 4096)
        if not chunk:
            return
        buffered += chunk
        while b"\n" in buffered:
            line, _, buffered = buffered.partition(b"\n")
            answer = line.decode("utf-8", errors="replace").strip().lower()
            if pending_service is None:
                if answer:
                    print("当前没有待确认的服务，请等待下一次询问。", flush=True)
                continue
            action = {"y": "confirm", "n": "reject", "c": "cancel"}.get(answer)
            if action is None:
                print("未发送反馈：请输入 y、n 或 c；空回车不会取消服务。", flush=True)
                continue
            service = pending_service
            pending_service = None
            try:
                interaction.feedback(service, action)
            except NodeError as error:
                print(f"反馈未被接受：{error}。请等待下一次询问。", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10001)
    parser.add_argument("--client-id", default="console")
    parser.add_argument("--wav", type=Path)
    args = parser.parse_args()
    client = Client(args.host, args.port)
    interaction = None
    try:
        interaction = Interaction(client, args.client_id)
        if args.wav:
            request = interaction.speech(args.wav)
            while True:
                message = client.wait(request)
                print(json.dumps(message, ensure_ascii=False), flush=True)
                if message["object"] == "tts.audio.file":
                    client.forget(request)
                    break
        run_console(client, interaction)
    except KeyboardInterrupt:
        pass
    finally:
        if interaction:
            interaction.close()
        client.close()


if __name__ == "__main__":
    main()
