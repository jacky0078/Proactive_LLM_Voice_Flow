# Edge-LLM-Comm-Infra

面向 Linux 边缘 AI 的 C++17 通信与任务生命周期框架。当前实现为**单机多进程原型**：TCP 网关接收请求，StackFlow 管理业务任务，ZMQ 传递输入和结果。

项目现已接入车载语音助手业务层，复用从 `LLM_Voice_Flow` 抽取的算法源码、模型与 RKLLM SDK：

```text
WAV → edge_asr（真实 Sherpa-ONNX）
    → edge_llm（本机 mock / RK 板 rkllm）
    → edge_tts（真实 SummerTTS）→ WAV
```

每个节点独立运行，继承 `StackFlow`，每个节点只允许一个活动任务。当前提供逐轮语音问答；未接入麦克风、扬声器、唤醒、插话、多轮记忆或车辆控制。

## 构建与演示

运行所需依赖单独放在 `voice-deps/`，该目录不提交 Git。当前工作区已完成抽取；其他机器可单独复制这份依赖目录，或在原项目仍可用时执行一次：

```bash
python3 scripts/prepare_voice_deps.py /path/to/LLM_Voice_Flow
```

传入的原项目根目录应直接包含 `voice`、`tts`、`llm`。抽取后，编译和运行均不再读取原项目目录。具体抽取清单见 [语音依赖说明](docs/voice-dependencies.md)。

在仓库根目录执行。Ubuntu/Debian 依赖示例：

```bash
sudo apt-get install build-essential cmake libzmq3-dev libgoogle-glog-dev \
    libboost-dev libssl-dev python3 curl
./scripts/build_voice.sh
python3 sample/voice_demo.py --rounds 3 --cycles 2
```

首次配置会下载 Sherpa-ONNX 1.11.3 的固定版本依赖，包括 ONNX Runtime。依赖目录默认是项目下的 `voice-deps/`；放在其他位置时，可统一指定：

```bash
export VOICE_DEPS_ROOT=/path/to/voice-deps
./scripts/build_voice.sh
python3 sample/voice_demo.py \
    --wav /absolute/path/input.wav --timeout 120
```

输入必须为 16 kHz、单声道、16-bit PCM WAV，最长 120 秒。默认示例使用原 ASR 模型附带的测试音频。客户端启动四个进程、创建任务、探测每一跳订阅，再提交音频；每轮打印识别文本、回答和请求编号，并保存 WAV、`results.json`、`messages.jsonl` 和进程日志。退出时逆序释放任务并回收自己启动的进程。

产物位于 `build/host/bin/`，演示结果位于 `artifacts/voice-demo/`。`build.sh` 是旧依赖安装脚本，不是当前统一构建入口。

## 模块

| 模块 | 当前职责 |
| --- | --- |
| `node/asr` | 原 Zipformer 在线识别器的 WAV 输入适配，CPU ONNX Runtime 推理 |
| `node/llm` | 原 `llm_demo.cpp` 的 RKLLM 模型加载、流式推理和销毁；显式选择 `mock` 或 `rkllm` |
| `node/tts` | 原 SummerTTS/VITS 算法与模型，分段合成并顺序写入 WAV |
| `node/common` | 单任务业务队列、输出队列、请求隔离及 WAV 读写 |
| `infra-controller` | `StackFlow` 生命周期、信道和显式请求上下文，产物为静态 SDK |
| `unit-manager` | TCP 接入、任务注册、地址分配和 action 路由；不创建业务节点进程 |
| `hybrid-comm` | ZMQ 封装，持久 socket 由所属线程创建、收发、关闭 |
| `network` | epoll Reactor TCP 库，独立于 ZMQ |
| `sample` / `tests` | 演示客户端、板端测试、SDK 替身测试和真实 ASR/TTS 集成验收 |

外层协议仍为 `request_id/work_id/action/object/data`；结果包含 `created/error`。TCP 使用换行分帧。推理整轮保持相同 `request_id`，每条结果的 `work_id` 标识当前输出节点。LLM/TTS 通过 `setup.data.input` 订阅上游任务。

## RK 板端真实 LLM

真实后端沿用原项目的 `rkllm_init → rkllm_run → rkllm_destroy`，退出推理时使用 `rkllm_abort`；默认采样参数和 DeepSeek 模板保留，模型路径、系统提示词和生成参数可配置。选择 `rkllm` 后加载或运行失败会返回错误，**不会降级为 mock**。

```bash
sudo apt-get install g++-aarch64-linux-gnu
./scripts/build_rkllm_aarch64.sh
```

此脚本交叉编译 LLM 节点及通信 SDK，链接真实 `librkllmrt.so`，并生成 `build/aarch64/package/`。它不交叉编译 ASR、TTS 或 manager；完整板端场景可在板上使用统一 CMake 原生构建。

板端启动 manager 和真实 LLM 节点后执行：

```bash
python3 sample/rkllm_board_test.py --model /opt/edge/models/model.rkllm \
    --config configs/voice-rk3588.json --rounds 3
```

量化模型必须匹配目标 SoC、RKLLM SDK/runtime 和板端驱动。仓库未包含适配当前设备的 `.rkllm` 权重，配置中的模型文件名是部署示例。

## 验证与边界

```bash
(cd build/host && ctest --output-on-failure)
python3 tests/voice_integration.py
```

已验证真实 ASR + 模拟 LLM + 真实 TTS 连续处理、重复生命周期、错误恢复、TCP 分帧、订阅探测，以及真实 RKLLM 适配代码的 SDK 替身测试。AArch64 产物的架构、真实库链接和动态依赖已检查。**当前环境没有 RK 设备，板端实际推理尚未验证。**

框架仍无资源调度、跨设备发现、进程托管、持久化和故障恢复；PUB/SUB 不提供可靠投递，探测只确认启动链路，后续缺片会报错或超时。未宣称高并发性能或车规能力。

详细协议、配置、源码复用边界和部署步骤见 [车载语音助手接入说明](docs/voice-assistant.md)，验收证据见 [验证记录](docs/validation.md)。原通信框架保留 MIT 许可声明，第三方源码、模型和 SDK 沿用各自许可。
