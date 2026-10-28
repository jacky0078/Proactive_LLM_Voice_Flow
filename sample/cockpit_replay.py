#!/usr/bin/env python3
"""通过本地 FD 回放车辆和乘员信号，不直接写座舱内部队列。"""
import argparse
import json
from pathlib import Path
import socket
import struct
import threading
import time

SIGNALS = ["cabin.temperature", "hvac.on", "vehicle.speed", "interaction.busy"]
for seat in ("driver", "front_passenger", "rear_left", "rear_right"):
    SIGNALS.extend(f"{seat}.{kind}" for kind in ("present", "fanning", "fatigue", "discomfort"))
FRAME = struct.Struct("<HHIIB3xd")


class SignalReplay:
    def __init__(self, paths, updates, refresh=.3, source=0, generation=1):
        self.paths = [str(path) for path in paths]
        self.updates = sorted(updates, key=lambda value: value.get("at_ms", 0))
        self.refresh = refresh
        self.source = source
        self.generation = generation
        self.sequence = 0
        self.state = {}
        self.stop_event = threading.Event()
        self.failure = None
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.socket.settimeout(.2)
        self.thread = threading.Thread(target=self._run, name="signal-replay", daemon=True)

    def start(self):
        self.thread.start()
        return self

    def close(self):
        self.stop_event.set()
        self.thread.join(timeout=2)
        self.socket.close()
        if self.failure:
            raise self.failure

    def _send(self, update):
        signal = SIGNALS.index(update["name"])
        if signal == 1:
            raise ValueError("hvac.on belongs to the vehicle simulator")
        self.sequence += 1
        encoded = FRAME.pack(signal, self.source, self.generation, self.sequence,
                             update.get("quality", 1), float(update["value"]))
        self.socket.sendto(encoded, self.paths[0 if signal < 4 else min(1, len(self.paths) - 1)])

    def _run(self):
        started = time.monotonic()
        index = 0
        next_refresh = started + self.refresh
        try:
            while not self.stop_event.is_set():
                now = time.monotonic()
                while index < len(self.updates) and now - started >= self.updates[index].get("at_ms", 0) / 1000:
                    update = self.updates[index]
                    self._send(update)
                    if not update["name"].endswith((".fanning", ".discomfort")):
                        self.state[update["name"]] = update
                    index += 1
                if now >= next_refresh:
                    for update in self.state.values():
                        self._send(update)
                    next_refresh = now + self.refresh
                deadline = next_refresh
                if index < len(self.updates):
                    deadline = min(deadline, started + self.updates[index].get("at_ms", 0) / 1000)
                self.stop_event.wait(max(.001, deadline - time.monotonic()))
        except (OSError, ValueError) as error:
            if not self.stop_event.is_set():
                self.failure = error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--scenario", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=60)
    args = parser.parse_args()
    replay = SignalReplay([args.runtime_dir / "vehicle.sock", args.runtime_dir / "occupant.sock"],
                          json.loads(args.scenario.read_text())["updates"]).start()
    try:
        replay.stop_event.wait(args.seconds)
    except KeyboardInterrupt:
        pass
    finally:
        replay.close()


if __name__ == "__main__":
    main()
