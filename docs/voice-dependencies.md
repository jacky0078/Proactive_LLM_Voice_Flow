# 独立语音依赖

业务节点代码位于 `node/asr`、`node/llm`、`node/tts`。实际使用的算法、模型和 RKLLM SDK 已单独整理到项目下的 `voice-deps/`，构建与运行不再需要原 `LLM_Voice_Flow-master` 文件夹。

## 抽取清单

下表“原路径”相对于直接包含 `voice`、`tts`、`llm` 的原项目根目录。

| 用途 | 原路径 | 独立目录 |
| --- | --- | --- |
| ASR 算法与构建 | `voice/sherpa-onnx` 下的根 CMake、cmake、sherpa-onnx/csrc、sherpa-onnx/c-api、LICENSE、README | `voice-deps/asr/sherpa-onnx/` |
| TTS 算法与文本处理 | `tts/src`、`tts/include`、`tts/README.md` | `voice-deps/tts/` |
| TTS 数学库 | `tts/eigen-3.4.0/Eigen`、`unsupported/Eigen` 和 COPYING 许可文件 | `voice-deps/tts/eigen-3.4.0/` |
| RKLLM SDK | `llm/rknn-llm/rkllm-runtime/Linux/librkllm_api/include` 和 `aarch64/librkllmrt.so`，以及 rknn-llm 的 LICENSE、CHANGELOG | `voice-deps/rkllm/` |
| ASR 模型与测试音频 | `voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16` 中下述文件 | `voice-deps/models/asr/zipformer/` |
| TTS 模型 | `tts/models/single_speaker_fast.bin` | `voice-deps/models/tts/single_speaker_fast.bin` |

ASR 保留当前节点实际加载的四个文件：

```text
encoder-epoch-99-avg-1.int8.onnx
decoder-epoch-99-avg-1.onnx
joiner-epoch-99-avg-1.int8.onnx
tokens.txt
```

同时保留 `test_wavs/0.wav`、`1.wav`、`2.wav` 供默认演示和自动化验收使用。未复制 ASR 的其他精度组合、旧麦克风服务、tts_server、zmq-comm-kit、量化示例、无关语言绑定和示例工程。原目录中的文件不会因此被删除。

复制的算法、模型、SDK 文件保持原内容，每个文件都会与来源进行 SHA-256 比对。只有 Sherpa 的子目录构建入口和 SummerTTS 的源码清单由抽取脚本生成，用于排除旧服务程序。`voice-deps/manifest.json` 记录全部文件来源、大小和摘要；各组件原有许可文件一起保留。

## RK 板端真实 LLM

“不复制未使用模型”仅针对 ASR 中没有被当前节点引用的权重，不涉及删除或替换 LLM 模型。

`node/llm/rkllm_backend.cpp` 保留真实的 `rkllm_init`、`rkllm_run`、`rkllm_abort`、`rkllm_destroy` 调用。独立 SDK 使用原始 `rkllm.h` 与真实 AArch64 `librkllmrt.so`；模型路径由任务配置的 `model_path` 提供。选择 `backend=rkllm` 后失败会报错，不会转为 mock。

原项目在本工作区没有 `.rkllm` 权重，抽取目录也没有新增量化模型。部署时仍需准备匹配 SoC、SDK 和驱动的模型，并修改 `configs/voice-rk3588.json` 中的路径。交叉编译和 SDK 替身测试不代替真实 RK 板端推理验证。

## 准备和使用

当前工作区已经抽取完成，不需要重复运行。其他机器可直接复制 `voice-deps/`，或在持有原项目时执行：

```bash
python3 scripts/prepare_voice_deps.py /path/to/LLM_Voice_Flow
```

源目录必须直接包含 `voice`、`tts`、`llm`。例如原工作区嵌套目录对应：

```bash
python3 scripts/prepare_voice_deps.py \
    LLM_Voice_Flow-master/LLM_Voice_Flow-master
```

脚本先在临时目录中复制、校验，成功后才生成完整输出目录；已有输出目录不会被覆盖。也可以用 `--output /path/to/voice-deps` 指定其他位置。

默认构建与运行：

```bash
./scripts/build_voice.sh
python3 sample/voice_demo.py --rounds 3
./scripts/build_rkllm_aarch64.sh
```

依赖放在其他位置时，构建脚本、演示客户端和集成测试共用同一个环境变量：

```bash
export VOICE_DEPS_ROOT=/path/to/voice-deps
./scripts/build_voice.sh
python3 sample/voice_demo.py
./scripts/build_rkllm_aarch64.sh
```

直接调用 CMake 时传入 `-DVOICE_DEPS_ROOT=...`；Python 客户端也支持 `--voice-deps-root ...`。切换一份已经配置过的 RKLLM SDK 时，应更新 `RKLLM_ROOT` 并清除 `RKLLM_INCLUDE_DIR`、`RKLLM_LIBRARY` 缓存；交叉构建脚本已处理这些缓存。

## Git 与迁移

`voice-deps/` 和原 `LLM_Voice_Flow-master/` 均被 `.gitignore` 排除。Git 保存业务代码、配置、构建与抽取脚本、说明文档；源码依赖、SDK 和模型另行保存。

原项目目录可以移出当前工程，需保留 `voice-deps/`。仅克隆 Edge 仓库不会得到被忽略的依赖文件，换机器时应单独携带这一目录或重新抽取。已有 `build/unified`、`build/sherpa` 等历史构建缓存可能保留旧路径；新的统一构建及本次验收使用 `build/host`，板端 LLM 使用 `build/aarch64`。
