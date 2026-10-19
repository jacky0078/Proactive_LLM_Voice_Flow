#!/usr/bin/env python3
"""在 RK 板上连接已启动的 manager/edge_llm，强制真实后端完成流式推理生命周期。"""
import argparse
import json
from pathlib import Path
import time
from voice_demo import Client, NodeError


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10001)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--prompt", default="你好，请用一句话介绍你可以提供的车载语音帮助。")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--output", type=Path, default=Path("rkllm-board-results.json"))
    args = parser.parse_args()
    if args.rounds < 1 or args.timeout <= 0:
        parser.error("rounds and timeout must be positive")
    config = json.loads(args.config.read_text())["llm"] if args.config else {}
    config.update(backend="rkllm", model_path=str(args.model.resolve()), enoutput=True)
    config.pop("input", None)
    client = Client(args.host, args.port, args.timeout)
    work = None
    results = []
    try:
        work = client.setup("llm", config)
        client.probe(work, [work])
        for _ in range(args.rounds):
            request = client.send(work, "inference", "llm.utf-8", args.prompt)
            deadline = time.monotonic() + args.timeout
            parts = []
            while True:
                message = client.receive(deadline)
                if message["request_id"] != request:
                    continue
                if message["error"]["code"]:
                    raise NodeError(message)
                if message["work_id"] != work or message["object"] != "llm.utf-8.stream":
                    raise RuntimeError("unexpected task or stream object")
                fragment = message["data"]
                if fragment["index"] != len(parts):
                    raise RuntimeError("missing or duplicate stream fragment")
                parts.append(fragment["delta"])
                print(fragment["delta"], end="", flush=True)
                if fragment["finish"]:
                    break
            print()
            answer = "".join(parts)
            if not answer.strip() or "<think>" in answer or "</think>" in answer:
                raise RuntimeError("empty/unfiltered model output")
            results.append(dict(request_id=request, work_id=work, backend="rkllm", answer=answer))
    finally:
        try:
            if work:
                client.exit(work)
        finally:
            client.close()
    args.output.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
    print(f"PASS: {len(results)} real RKLLM rounds; results in {args.output}")


if __name__ == "__main__":
    main()
