#!/usr/bin/env python3
"""从原语音项目抽取 Edge 实际使用的源码、模型和 RKLLM SDK。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ASR_MODEL = "voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"
SHERPA = "voice/sherpa-onnx"
RKLLM = "llm/rknn-llm/rkllm-runtime/Linux/librkllm_api"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def prepare(source, destination):
    copies = [
        (f"{SHERPA}/CMakeLists.txt", "asr/sherpa-onnx/CMakeLists.txt"),
        (f"{SHERPA}/cmake", "asr/sherpa-onnx/cmake"),
        (f"{SHERPA}/sherpa-onnx/csrc", "asr/sherpa-onnx/sherpa-onnx/csrc"),
        (f"{SHERPA}/sherpa-onnx/c-api", "asr/sherpa-onnx/sherpa-onnx/c-api"),
        (f"{SHERPA}/LICENSE", "asr/sherpa-onnx/LICENSE"),
        (f"{SHERPA}/README.md", "asr/sherpa-onnx/README.md"),
        ("tts/src", "tts/src"),
        ("tts/include", "tts/include"),
        ("tts/eigen-3.4.0/Eigen", "tts/eigen-3.4.0/Eigen"),
        ("tts/eigen-3.4.0/unsupported/Eigen", "tts/eigen-3.4.0/unsupported/Eigen"),
        ("tts/README.md", "tts/README.md"),
        (f"{RKLLM}/include", "rkllm/include"),
        (f"{RKLLM}/aarch64/librkllmrt.so", "rkllm/aarch64/librkllmrt.so"),
        ("llm/rknn-llm/LICENSE", "rkllm/LICENSE"),
        ("llm/rknn-llm/CHANGELOG.md", "rkllm/CHANGELOG.md"),
        ("tts/models/single_speaker_fast.bin", "models/tts/single_speaker_fast.bin"),
    ]
    for name in (
        "encoder-epoch-99-avg-1.int8.onnx", "decoder-epoch-99-avg-1.onnx",
        "joiner-epoch-99-avg-1.int8.onnx", "tokens.txt",
        "test_wavs/0.wav", "test_wavs/1.wav", "test_wavs/2.wav",
    ):
        copies.append((f"{ASR_MODEL}/{name}", f"models/asr/zipformer/{name}"))
    for path in sorted((source / "tts/eigen-3.4.0").glob("COPYING*")):
        copies.append((str(path.relative_to(source)), f"tts/eigen-3.4.0/{path.name}"))

    # 沿用原算法清单，不引入旧 tts_server、PortAudio 或 zmq-comm-kit。
    tts_cmake = source / "tts/CMakeLists.txt"
    sources = re.findall(r"\./src/[A-Za-z0-9_/.-]+\.(?:cpp|cc)", tts_cmake.read_text())
    if not sources or len(set(sources)) != len(sources):
        raise ValueError("cannot read a unique SummerTTS source list")
    for name in sources:
        if not (source / "tts" / name).is_file():
            raise FileNotFoundError(source / "tts" / name)
    for original, _ in copies:
        if not (source / original).exists():
            raise FileNotFoundError(source / original)

    manifest = {
        "source_project": "https://github.com/superxiaobai-1/LLM_Voice_Flow",
        "files": {},
        "generated_files": {},
        "source_lists": {"tts/CMakeLists.txt": sha256(tts_cmake)},
    }
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".voice-deps-", dir=destination.parent) as temporary:
        staging = Path(temporary) / "bundle"
        staging.mkdir()
        for original, target in copies:
            origin = source / original
            files = sorted(p for p in origin.rglob("*") if p.is_file()) if origin.is_dir() else [origin]
            for path in files:
                relative = Path(target) / path.relative_to(origin) if origin.is_dir() else Path(target)
                output = staging / relative
                output.parent.mkdir(parents=True, exist_ok=True)
                # copy2 复制真实文件内容；依赖目录不保留指回原工程的链接。
                shutil.copy2(path, output)
                digest = sha256(path)
                if sha256(output) != digest:
                    raise OSError(f"copy checksum mismatch: {path}")
                manifest["files"][relative.as_posix()] = {
                    "source": str(path.relative_to(source)),
                    "sha256": digest,
                    "bytes": output.stat().st_size,
                }

        generated = {
            "asr/sherpa-onnx/sherpa-onnx/CMakeLists.txt": (
                "# Edge uses only the core and C/C++ API; the legacy voice service is excluded.\n"
                "add_subdirectory(csrc)\nadd_subdirectory(c-api)\n"
            ),
            "tts/sources.cmake": (
                "# Extracted from the original SummerTTS source list.\n"
                "set(SUMMER_TTS_SOURCES\n" + "".join(f"    {name}\n" for name in sources) + ")\n"
            ),
            "README.md": (
                "# Edge 语音依赖\n\n"
                "由 scripts/prepare_voice_deps.py 抽取；算法和模型逐文件校验，原项目不再参与构建与运行。\n\n"
                "- asr/sherpa-onnx：识别核心、C/C++ API 和依赖下载配置。\n"
                "- tts：SummerTTS 算法、文本处理、Eigen 头文件和源码清单。\n"
                "- rkllm：原 SDK 头文件、真实 AArch64 运行库及许可说明。\n"
                "- models：实际使用的 ASR/TTS 模型及三段验收 WAV。\n\n"
                "manifest.json 记录每个文件的来源、大小与 SHA-256。\n"
                "此目录默认不提交 Git；迁移服务器时需单独携带。未包含板端 .rkllm 量化模型。\n"
            ),
        }
        for name, content in generated.items():
            path = staging / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            manifest["generated_files"][name] = {"sha256": sha256(path), "bytes": path.stat().st_size}
        (staging / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        if destination.exists():
            raise FileExistsError(f"refusing to overwrite existing dependency directory: {destination}")
        staging.rename(destination)
    size = sum(item["bytes"] for item in manifest["files"].values())
    print(f"Prepared {len(manifest['files'])} verified files ({size / 1024**2:.1f} MiB): {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="原项目根目录，应直接包含 voice、tts、llm")
    parser.add_argument("--output", type=Path, default=ROOT / "voice-deps")
    args = parser.parse_args()
    source = args.source.resolve()
    destination = args.output.resolve()
    if destination.exists():
        parser.error(f"输出目录已存在，不会覆盖：{destination}")
    if destination == source or source in destination.parents:
        parser.error("输出目录必须位于原项目之外")
    prepare(source, destination)


if __name__ == "__main__":
    main()
