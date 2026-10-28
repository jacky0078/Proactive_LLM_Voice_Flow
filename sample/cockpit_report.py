#!/usr/bin/env python3
"""从逐次追踪生成耗时报告；不把缺失阶段作为零值。"""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path


def distribution(values):
    values = sorted(value / 1e6 for value in values)
    def percentile(rank):
        return values[max(0, math.ceil(len(values) * rank) - 1)] if values else None
    return dict(count=len(values), p50_ms=percentile(.5), p95_ms=percentile(.95), p99_ms=percentile(.99),
                p999_ms=percentile(.999) if len(values) >= 1000 else None,
                max_ms=max(values) if values else None,
                mean_ms=sum(values) / len(values) if values else None)


def report(path):
    groups = defaultdict(list)
    summaries = []
    records = []
    for line in path.read_text().splitlines():
        row = json.loads(line)
        records.append(row)
        if row.get("kind") == "summary":
            summaries.append(row["status"])
        label = "/".join(str(row.get(key, "-")) for key in ("kind", "need", "outcome", "work_id"))
        for key, value in row.items():
            if key.endswith("_ns") and key != "time_ns" and isinstance(value, (int, float)):
                groups[f"{label}/{key}"].append(value)
    return dict(source=str(path), records=len(records),
                groups={key: distribution(values) for key, values in groups.items()},
                final_status=summaries[-1] if summaries else None,
                physical_sensor_latency_observed=False, audio_playback_latency_observed=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(report(args.traces), ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(result)
    else:
        print(result, end="")


if __name__ == "__main__":
    main()
