# 项目速览

## 定位与边界

- Linux/C++17 单机多进程通信与任务生命周期框架，现有 WAV 文件车载语音助手场景。
- 已有 TCP 网关、任务注册与地址分配、RPC 生命周期、ZMQ 输入与节点订阅；无资源调度、跨设备发现、持久化或故障恢复。
- `edge_asr` 使用真实 Sherpa-ONNX Zipformer；`edge_llm` 显式选择 mock 或 RKLLM；`edge_tts` 使用真实 SummerTTS 输出 WAV。
- 首版单会话、每节点一个活动任务、逐轮处理。没有麦克风、播放、唤醒、插话、多轮记忆或车控。
- RKLLM 原有真实推理已迁移、SDK 替身已验证、AArch64 真实库链接已检查；没有 RK 设备和适配 `.rkllm` 权重，板端效果未实测。

## 架构

| 模块 | 职责 |
| --- | --- |
| `node/common` | `VoiceNode : StackFlow`、业务队列、输出队列、WAV 读写 |
| `node/asr` | WAV → 原 Zipformer 在线识别器 → 最终文本 |
| `node/llm` | 原 llm_demo 的加载、模板、采样、RKLLM run/abort/destroy；mock 共用输出协议 |
| `node/tts` | 原 SummerTTS 核心、模型、分段合成与 WAV 文件完成事件 |
| `infra-controller` | 静态 SDK `stackflow`；生命周期事件、信道与显式 `RequestContext` |
| `unit-manager` | 中心网关进程；注册、地址池、action 路由；不创建节点进程 |
| `network` | 独立 epoll Reactor TCP 库；不依赖 ZMQ |
| `hybrid-comm` | ZMQ 封装；持久 socket 由专属线程创建、使用、关闭，RPC 客户端 socket 为调用局部对象 |
| `sample` / `tests` | 文件语音演示、板端脚本、RK SDK 替身测试和真实 ASR/TTS 集成测试 |

依赖：`node → infra-controller → hybrid-comm`；`unit-manager → network + stackflow`。所需算法、模型和 RK SDK 已抽取到独立的 `voice-deps/`，通过 `VOICE_DEPS_ROOT` 引用；构建与运行不读取原项目目录。`node/test` 仍是旧通信演示，不加载模型，不作为语音验收入口。

## 运行与协议

- 启动 manager 后启动节点。`llm` 为 RPC 名，`llm.N` 为任务 ID，`request_id` 为本轮请求 ID。
- setup 注册两条 IPC 地址；manager 绑定任务输入 PUB，节点绑定任务结果 PUB。退出后释放注册与地址。
- 生命周期 RPC 回 `None` 仅表示入队；实际完成响应通过会话 PULL 回 TCP。事件队列使用标准库条件变量，不再依赖 eventpp。
- 派生节点完成成员初始化后再 `start_events()`；析构先 `stop_events()`，再断开输入、停止业务线程，最后释放任务。旧基类调用方式仍提供自动启动兼容。
- 新异步节点必须使用 `subscribe_message` / `send_with_context`。每条消息显式复制请求 ID 和回包地址，输出 work_id 属于当前节点。
- `subscriber_work_id("")` 为本任务输入；指定其他 work_id 则查询上游 `.out_port`。这是订阅连接，不是工作流调度。
- 外层 JSON 为 `request_id/work_id/action/object/data`，响应带 `created/error`；error 始终为对象。TCP 按换行分帧，上限 1 MiB。
- 业务对象：`asr.audio.file`（data.path）、`asr.utf-8`、`llm.utf-8.stream`（index/delta/finish）、`tts.audio.file`（path/audio metadata）。
- LLM/TTS 的 `setup.data.input` 指向上游任务。首次输入前用 `sys.probe` 逐跳确认订阅，不能用固定 sleep 假定首包可靠。
- 流索引每请求从 0 开始。缺片、乱序、重复片段报错；TTS 没有 finish 则流超时。完成后拒绝短期重复请求，不提供持久幂等。
- 默认 RPC 为 `ipc:///tmp/rpc.<node>`；环境变量 `EDGE_RPC_DIR` 可隔离会话。默认 manager TCP 10001；任务 IPC 编号池 `[5010,5555)`，每任务两个编号。IPC 编号不是 TCP 端口。
- manager `key_sql` 仍为带锁内存 map；任务注册持有 shared_ptr，避免发布与释放并发时悬空。无 SQL 引擎。

## 构建与验证

```bash
./scripts/build_voice.sh
python3 sample/voice_demo.py --rounds 3 --cycles 2
(cd build/host && ctest --output-on-failure)
python3 tests/voice_integration.py
./scripts/build_rkllm_aarch64.sh
```

- 根目录统一 CMake，Linux/GCC/CMake/ZeroMQ/Boost/glog/OpenSSL/OpenMP/Python；ASR 首次构建下载固定版本依赖。
- `VOICE_DEPS_ROOT` 默认为项目内 `voice-deps/`；`RKLLM_ROOT` 默认是其中的 `rkllm/`，保留原 SDK include 和真实 AArch64 runtime。
- `scripts/prepare_voice_deps.py` 按白名单复制并校验依赖，记录来源与 SHA-256；不覆盖已有目录。`voice-deps/` 和原项目均被 Git 忽略，换机器需单独携带依赖。详细清单见 `docs/voice-dependencies.md`。
- 已在原目录不可用时验证源码构建、3 项测试及 7 轮真实 ASR/TTS 闭环、真实 RKLLM AArch64 链接；证据见 `artifacts/dependency-isolation/`。根 CMake 保留 `BUILD_TESTING`，避免第三方 Eigen 配置关闭 Edge 验收。
- 真实 RKLLM 构建要求 `EDGE_ENABLE_RKLLM=ON` 和 AArch64；没有运行时 mock 降级。交叉脚本只构建 LLM 节点及 SDK/通信依赖，完整板端场景参见 `docs/voice-assistant.md`。
- manager 接受 `--config FILE`，未传时使用构建时指定的默认配置路径，不依赖启动目录。
- 默认 demo 操作超时 120 秒，可配置；等待 TTS 完成或错误后才进入下一轮。退出逆序释放任务，仅回收自己启动的进程。
- 旧 `build.sh` 安装系统依赖且会删除归档，不用于检查。旧独立 CMake/示例脚本不再作为推荐构建入口。

## 修改原则

- 服从仓库 `.clang-format`，保持四空格、原命名习惯、清晰分层和必要的中文注释，避免多步逻辑压在一行。
- 模型代码放 `node/`；注册和路由放 manager；传输改动放相应通信库。避免创建不必要的通用插件框架或重写已存在的推理实现。
- LLM 迁移来源是真实 `llm_demo.cpp`，不是 `llm/test/llm_test.cpp`。保留 SDK 调用、默认采样和 DeepSeek 模板；回调复制文本入队，不能直接使用 SDK 临时指针或跨线程操作 ZMQ。
- 真正退出时先 abort 活跃 RKLLM 推理并等待线程，再 destroy；错误必须回客户端，不能结束整个节点进程。ASR/TTS 的 CPU 工作在退出时等待当前操作停止/完成。
- 新请求必须显式携带上下文；业务缓存、序号和文件按请求隔离。暂停只控制新输入，不是插话或中途挂起。
- 修改协议同时核对网关、SDK、三个节点与 Python 客户端。至少覆盖 setup → inference → exit 和变更涉及的异常、并发路径。
- 单元测试使用实际 RK 适配代码配 SDK 替身；真实 ASR/TTS 集成测试和 AArch64 ELF 检查不能替代板端推理。
- 仍有同步生命周期 RPC、每连接 ZMQ 线程开销和非可靠 PUB/SUB；不可宣称高并发、可靠投递、车规或完整故障恢复。
- 架构或运行方式改变时同步更新本文件、README 和部署文档，以源码和实际测试为准。
