# 车载语音助手接入说明

本文说明原有文件语音演示；自主启动、常驻感知和服务闭环见 [主动座舱运行说明](cockpit.md)。两种模式保留独立入口。

## 源码复用与任务边界

`VOICE_DEPS_ROOT` 指向抽取后的独立依赖目录，默认是项目下的 `voice-deps/`。ASR/TTS 算法、模型和 RKLLM SDK 均从这里读取，原 `LLM_Voice_Flow-master` 目录不再参与构建和运行。抽取清单、准备方法和 Git 边界见 [语音依赖说明](voice-dependencies.md)。

| 原实现 | Edge 适配 |
| --- | --- |
| `voice/sherpa-onnx` 在线 Zipformer 识别器 | `node/asr/asr_task.cpp`，每轮创建新 stream，读取 WAV、补尾部静音并输出最终文本 |
| `llm/rknn-llm/examples/DeepSeek-R1-Distill-Qwen-1.5B_Demo/deploy/src/llm_demo.cpp` | `node/llm/rkllm_backend.cpp`，保留 SDK 调用、默认参数和 DeepSeek 模板 |
| `tts/src` SummerTTS/VITS 和 `single_speaker_fast.bin` | `node/tts/tts_task.cpp`，复用 `SynthesizerTrn::infer` 和原算法源码清单 |

抽取脚本保留原算法文件，给 Sherpa 生成只包含 core 与 C/C++ API 的子目录入口，给 SummerTTS 保存原算法源码清单；不复制旧 microphone/ZMQ 服务。没有改 ASR/LLM/TTS 的量化实现，也没有链接旧 `zmq-comm-kit`。

ASR 是独立的 `StackFlow("asr")` 业务节点。StackFlow 是节点的通信与生命周期基类，不会替代 ASR 业务实现。LLM/TTS 同样分别使用 `StackFlow("llm")` 和 `StackFlow("tts")`。

新节点的 `VoiceTask` 使用输入工作队列与输出队列；文件模式下 TTS 在一个业务线程中按顺序合成和写盘，不创建音频播放线程。`WavWriter::append` 是未来接音频输出的 PCM 边界。每个节点只允许一个活动任务，每轮请求必须完成或出错后再进入下一轮。

## 协议

每条 TCP 消息为一个 JSON 加 `\n`。服务端支持半包、粘包、UTF-8 字节跨读取，单条消息上限 1 MiB。

```json
{"request_id":"setup-asr","work_id":"asr","action":"setup","object":"asr.setup","data":{"model_dir":"/opt/edge/models/zipformer","enoutput":true}}
{"request_id":"setup-llm","work_id":"llm","action":"setup","object":"llm.setup","data":{"backend":"mock","input":"asr.0","enoutput":true}}
{"request_id":"setup-tts","work_id":"tts","action":"setup","object":"tts.setup","data":{"model_path":"/opt/edge/models/single_speaker_fast.bin","output_dir":"/opt/edge/output","input":"llm.1","enoutput":true}}
{"request_id":"round-1","work_id":"asr.0","action":"inference","object":"asr.audio.file","data":{"path":"/opt/edge/input.wav"}}
```

示例中的 `asr.0/llm.1` 仅示意；必须使用 setup 返回的 `work_id`。文件路径由节点所在的机器解析，TCP 客户端不会上传文件。

一轮成功输出：

```json
{"request_id":"round-1","work_id":"asr.0","object":"asr.utf-8","data":"你好","created":0,"error":{"code":0,"message":""}}
{"request_id":"round-1","work_id":"llm.1","object":"llm.utf-8.stream","data":{"index":0,"delta":"你好。","finish":false},"created":0,"error":{"code":0,"message":""}}
{"request_id":"round-1","work_id":"llm.1","object":"llm.utf-8.stream","data":{"index":1,"delta":"","finish":true},"created":0,"error":{"code":0,"message":""}}
{"request_id":"round-1","work_id":"tts.2","object":"tts.audio.file","data":{"path":"/opt/edge/output/voice-....wav","sample_rate":16000,"channels":1,"bits_per_sample":16,"samples":16000},"created":0,"error":{"code":0,"message":""}}
```

`created` 实际为发送时的 Unix 秒数。内部发布消息还携带 `zmq_com` 回包地址。输入回调把 `request_id/reply_url` 复制进 `RequestContext`，异步结果不读取生命周期请求的可变编号。

- `sys.setup`：模型和信道建立完成；不代表 SUB 已收到上游首包。
- `sys.probe`：客户端通过 ASR 输入发送探测，各节点原样转发并回报自身 `work_id`。重复探测直到三跳确认才开始识别。
- `sys.error`：`error` 始终是 `{code,message}` 对象；上游错误会沿业务链传播，同一请求可能收到多个节点的错误。
- `pause` 的 `data.paused=true/false`：拒绝/恢复接收新输入，不是推理中途挂起。`exit` 才执行停止与资源释放。
- `taskinfo`：查询已注册任务；`exit` 成功响应在注册与信道释放后发送。
- 流索引从 0 连续递增；TTS 发现缺片、重复或乱序会终止该轮并清理临时文件，未收到 finish 则按流超时处理。
- 不再使用文本 `END`、`first_msg`、ASR/TTS 固定端口握手。LLM 的 `finish` 是明确的结束事件。

## 配置

通用：`input` 为上游任务 ID，`enoutput` 控制回传客户端。PUB 输出不受 `enoutput` 影响；演示客户端将三个节点的回传全部开启。

ASR：`model_dir` 必填，`num_threads` 默认为 2。当前固定使用原双语小型 Zipformer 的 int8 encoder/joiner、FP32 decoder 和 tokens。输入限制为 16 kHz/mono/PCM16、最长 120 秒；静音与空识别结果报错，不触发空回答。

LLM：`backend` 必须显式为 `mock` 或 `rkllm`。

| 参数 | 原默认值/含义 |
| --- | --- |
| `model_path` | 真实模式必填，已部署的 `.rkllm` 模型 |
| `system_prompt` | 默认空；板端场景配置提供简短中文助手提示词 |
| `top_k` / `top_p` / `temperature` | `1 / 0.95 / 0.8` |
| `repeat_penalty` / `frequency_penalty` / `presence_penalty` | `1.1 / 0 / 0` |
| `max_new_tokens` / `max_context_len` | `100 / 256`，按适配模型能力调整 |
| `enabled_cpus_num` / `enabled_cpus_mask` | `2 / 5`（原 CPU0、CPU2） |
| `mock_reply` | 可选固定模拟回答；默认回显识别文本并说明助手身份 |
| `mock_delay_ms` | 测试延时，默认 0，范围 0–30000 |

真实模式保留 `<｜User｜>` / `<｜Assistant｜>` 模板、`RKLLM_INPUT_PROMPT`、`RKLLM_INFER_GENERATE`、`keep_history=0`、`embed_flash=1` 和同步 SDK run。SDK `userdata` 指向本轮状态；回调复制文本入队，消费线程过滤跨片段 `<think>...</think>` 并按原中文标点分段。SDK 返回失败、错误回调或缺少结束事件均返回错误。

TTS：`model_path/output_dir` 必填，`num_threads=2`，`stream_timeout_seconds=120`。该超时从上一次片段完成处理后计时；客户端操作超时独立配置。原模型解析器缺少全面的格式边界检查，适配层默认校验随附 `single_speaker_fast.bin` 的 SHA-256：

```text
87b774813258a3636b3f1d9adddd3652ede503b4e8f4eb3c39065cba2a4951c4
```

只有已确认兼容的 SummerTTS 模型才应通过 `model_sha256` 指定其他摘要。原权重数据在 synthesizer 销毁后释放。音频先写 `.part`，收到 finish 后补 WAV 头并改名；错误与退出删除不完整文件。

## 构建选项

统一入口是根目录 CMake，不需要先安装 `libstackflow.a`，也不依赖硬编码的 simdjson 静态库。生命周期队列使用标准库条件变量，当前 SDK 不再依赖 eventpp。

| CMake 变量 | 默认值 |
| --- | --- |
| `VOICE_DEPS_ROOT` | 项目下的 `voice-deps/`；构建脚本和 Python 客户端也读取同名环境变量 |
| `EDGE_BUILD_MANAGER/ASR/TTS/COCKPIT` | `ON` |
| `EDGE_ENABLE_RKLLM` | `OFF`；开启时要求目标 AArch64 与匹配的 ELF runtime |
| `RKLLM_ROOT` | `${VOICE_DEPS_ROOT}/rkllm`；也可指定匹配设备的另一份 SDK |
| `ZMQ_ROOT` | 可选的目标平台 ZeroMQ 安装前缀 |
| `SHERPA_ONNX_BUILD_DIR` | 可选：同版本 Sherpa 共享库构建目录；默认空值，从抽取的源码构建 |
| `BUILD_TESTING` | `ON`；交叉构建关闭本机测试 |

当前工具链文件面向 Debian/Ubuntu AArch64 GNU/Linux。RK 板系统若使用其他 libc/sysroot，应提供匹配的 CMake toolchain，而不是将该二进制直接视为兼容所有 RK 系统。

## 板端部署

`./scripts/build_rkllm_aarch64.sh` 固定下载并校验 ZeroMQ 4.3.5，在本地交叉构建通信依赖，使用原 SDK 头文件和真实 ARM64 runtime 链接 `edge_llm`。安装包包含 `bin/edge_llm` 和 `lib/librkllmrt.so`，安装后的 RUNPATH 为 `$ORIGIN/../lib`。板端还须有该 runtime 所需的 glibc、libstdc++、libgomp 等依赖。

完整四进程业务可在 AArch64 板上原生构建：

```bash
cmake -S . -B build/rk-native -DCMAKE_BUILD_TYPE=Release \
    -DEDGE_ENABLE_RKLLM=ON -DVOICE_DEPS_ROOT=/opt/edge/voice-deps \
    -DRKLLM_ROOT=/opt/edge/voice-deps/rkllm -DBUILD_TESTING=OFF
cmake --build build/rk-native -j2
```

准备与 SoC、SDK 和驱动匹配的量化模型，将 `configs/voice-rk3588.json` 的各个模型路径改为板端实际路径。原项目提供真实推理源码和 SDK，但本工作区没有可加载的 `.rkllm` 权重，也没有重新执行模型量化。RK3588 的模型不能据此视为自动兼容 RK3576。

直接运行完整文件语音场景：

```bash
python3 sample/voice_demo.py --build-dir build/rk-native \
    --config configs/voice-rk3588.json --wav /opt/edge/input.wav \
    --output-dir /opt/edge/output --rounds 3 --timeout 120
```

也可单独验证真实 LLM：在两个终端启动以下进程，保持相同 `EDGE_RPC_DIR`（未设置时默认 `/tmp`）。

```bash
./build/rk-native/bin/unit_manager --config unit-manager/master_config.json
./build/rk-native/bin/edge_llm
```

再运行：

```bash
python3 sample/rkllm_board_test.py --model /opt/edge/models/model.rkllm \
    --config configs/voice-rk3588.json --rounds 3 --timeout 120
```

该脚本强制 `backend=rkllm`，执行 setup、订阅探测、逐轮流式推理、exit，核对请求、任务、索引和 finish，并保存板端回答。当前提供脚本但尚未在实际 RK 板执行。

## 仍然保留的边界

manager 不托管节点进程；演示客户端的进程管理仅服务本次演示。状态和注册表保存在内存中。推理仍使用 PUB/SUB，无法保证节点崩溃、断线或队列溢出后的投递与恢复。生命周期 RPC 在 I/O 线程上同步等待，默认超时 3 秒，业务完成响应另行异步返回。

这里没有万连接性能验证、车控功能或音频硬件验证。SDK 替身验证调用协议与生命周期，交叉编译验证架构和链接，二者都不能证明量化模型在目标 NPU 上的实际输出质量、性能或驱动兼容性。
