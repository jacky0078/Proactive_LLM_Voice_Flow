# 主动智能座舱：运行、协议与实现

主机首版已经实现自主启动、持续感知、热舒适和疲劳快路径、模糊舒适慢路径、用户确认、车辆模拟执行和状态回读。ASR/TTS 使用现有真实模型，主机 LLM 显式使用 mock；RK 配置使用真实 RKLLM。当前不接麦克风、实际播放或真实车控。

## 启动和停止

在仓库根目录执行：

```bash
./scripts/build_cockpit.sh
python3 sample/cockpit_supervisor.py
```

已存在同版本 Sherpa 库时可复用，避免重新构建算法依赖：

```bash
./scripts/build_cockpit.sh -DSHERPA_ONNX_BUILD_DIR="$PWD/build/host"
```

Supervisor 创建 manager、cockpit、ASR、LLM、TTS 进程和 Task。它先开放感知，再并行加载三个模型；一个模型加载失败不阻止其他能力绑定。它打印实际 Task ID，并写入 `artifacts/cockpit/state.json`。默认端口为 10001，可用 `--port` 修改。程序不等待交互客户端启动。

分别在另外两个终端启动交互端和信号回放：

```bash
python3 sample/cockpit_client.py --port 10001
python3 sample/cockpit_replay.py \
    --runtime-dir artifacts/cockpit/runtime \
    --scenario configs/scenarios/heat.json --seconds 60
```

询问 WAV 生成后，交互端显示路径，输入 `y` 确认、`n` 拒绝、`c` 取消并回车。空行和其他输入不会发送反馈；等待输入期间仍接收取消与超时事件，终态后的按键会提示当前无待确认服务。文件生成不等于已经播放。`rest.json` 和 `comfort.json` 是另两个场景。空调状态由车辆模拟器维护：热舒适成功后已经开启空调，独立验收舒适场景时应启动新的运行实例。

普通文件问答：

```bash
python3 sample/cockpit_client.py --wav /absolute/path/input.wav
```

关闭交互端只解绑会话；关闭 Supervisor 才释放 Task 和本轮启动的进程。异常断线在默认 6 秒租期后失效，交互端每 2 秒续期。首版只有一个有效交互端；同一个 `client_id` 可重连替换，不同客户端占用时返回忙碌。

仅感知模式：

```bash
python3 sample/cockpit_supervisor.py --no-models
```

运行目录必须独占，Unix Socket 路径总长须小于 108 字节。可通过 `--output-dir /tmp/cockpit-demo` 使用短路径。正常退出清理 Socket；再次启动时，Supervisor 在持有目录锁后检查 Linux 内核中的地址占用，只删除已无人使用的 Socket 文件，保留普通文件和日志。发现仍在使用的地址会拒绝启动，不接管旧进程。子进程使用独立会话，终端 Ctrl+C 由 Supervisor 统一处理并有序停止。启动阶段的失败原因写入 `state.json` 的 `errors.startup`。首版不自动重启、重建或恢复 manager 注册表。

## 分层和线程

| 文件 | 职责 |
| --- | --- |
| `sample/cockpit_supervisor.py` | Python 管理客户端：启动、setup、登记实际 ID、绑定能力、监测退出、有序停止 |
| `unit-manager/src/runtime_directory.cpp` | 当前运行实例目录；保留原任务注册与消息路由 |
| `node/cockpit/main.cpp` | `CockpitNode : StackFlow`，创建/释放长期 `CockpitTask` |
| `ingress.*` | 复用 network Reactor；多 FD 适配、过滤、有界合并，唯一感知生产者 |
| `context.*`、`timers.h` | 固定 Context 数组、来源隔离、TTL、需求 FSM、依赖索引、定时器 |
| `cockpit_task.*` | 核心循环、线程生命周期、有界队列和状态查询 |
| `cockpit_control.cpp` | 会话、版本绑定、模型子调用关联与取消 |
| `cockpit_services.cpp`、`cockpit_results.cpp` | 仲裁、普通问答、确认、执行、结果复核 |
| `model_client.*` | 独立 TCP I/O 线程；不在核心线程里连接、收发、序列化 |
| `vehicle_simulator.*` | 独立模拟执行线程，执行 ACK 和车辆状态回读 |
| `metrics.*` | 固定直方图和有界后台追踪写盘 |

```text
车辆/乘员 Unix Datagram FD ─┐
管理注入 socketpair ────────┼→ Ingress Reactor → SPSC(1024) → Context 核心线程
车辆状态回读 socketpair ────┘                         │
                                         控制/结果有界队列(128)
                                                    │
                         模型 TCP I/O、输出发布、车辆执行、日志线程
```

Reactor 回调直接读取和转换轻量字段，没有回调线程池。正常回放直接进入 FD；`cockpit.signal.batch` 来自既有 ZMQ 接收线程，经控制入口和 socketpair 交给同一个 Ingress，不另设感知生产者。

核心按 32 条分批处理固定的当前队列前缀，完成此前缀后推进 FSM、定时器和低频消息；持续输入不能延长本轮前缀。这样可避免解除信号已经入队而旧状态先触发服务。所有 Context、候选、会话和绑定表只有核心线程修改。

五种核心类型见 `types.h`：`SignalUpdate`、`SignalState`、`Candidate`、`ServiceInstance`、`PendingCall`。信号和座位使用固定整数索引；只有慢路径复制四项依赖值/版本。候选 ID 永不复用，不引入候选 generation 和 trace UUID；SDK 的 `RequestContext` 保持请求编号和回包地址两个职责。

## 接口与边界

外层仍是 `request_id/work_id/action/object/data`。模型、管理、交互各使用独立 TCP 会话。SDK 异步回包继续使用显式 `RequestContext`。

| object | action / 目标 | data 要点 |
| --- | --- | --- |
| `sys.runtime.publish` | `runtime` / `sys` | `runtime_id`、`tasks`：cockpit/asr/llm/tts 的实际 ID 或 null |
| `sys.runtime.lookup` | `runtime` / `sys` | `{}`；返回 manager 实例、运行实例和 Task 映射 |
| `sys.runtime.remove` | `runtime` / `sys` | 仅发布者可以撤销自己的 `runtime_id` |
| `cockpit.status` | `inference` / 实际 cockpit ID | `{}`；独立报告感知、能力、交互、信号和指标 |
| `cockpit.capabilities.bind` | 同上 | `runtime_id/version/bindings` 完整快照；三个能力键都提供 |
| `cockpit.session.bind` | 同上 | `client_id`；返回 `session_token/generation/lease_ms` |
| `cockpit.session.renew/unbind` | 同上 | `session_token`，并核对实际回包会话 |
| `cockpit.speech.file` | 同上 | `path/session_token`；语音确认还必须携带 `service_id` |
| `cockpit.feedback` | 同上 | `service_id/session_token/action`，action 为 confirm/reject/cancel |
| `cockpit.signal.batch` | 同上 | `updates` 数组，1–32 项，每项 name/value/sequence，可选 source_id/source_generation/quality |
| `cockpit.pause` | 同上 | `paused`；同时保留原 `action: pause` 管理入口 |
| `cockpit.service.event` | 主动回包 | 独立事件请求号，event_id/service_id/candidate_id/phase/session_generation |
| `llm.decision.request` | `inference` / 已绑定 LLM | 温度、空调、乘员、车速、不适次数；只允许白名单结果 |
| `llm.cancel` | 同上 | 目标 request_id/capability_generation；取消命令自己也有独立 request_id |
| `tts.utf-8` | `inference` / 已绑定 TTS | 直接文本，仍返回 tts.audio.file |

`cockpit.capabilities.bound` 表示该版本已逐项处理，不表示所有能力都可用。状态为 `UNBOUND/PROBING/READY/UNAVAILABLE`。同版本同快照幂等；旧版本、冲突内容拒绝。探测确认协议版本、LLM 取消能力和 Task 空闲状态。取消未确认的实例不继续接收新调用。

结构化 LLM 结果仅接受：

```json
{"service":"air_conditioner","params":{"enabled":true}}
```

或 `{"service":"no_action"}`。核心复核白名单、候选截止时间、证据版本、会话和能力代次后才能询问。结果不能直接送到车辆执行器。

`WAITING_CONFIRMATION` 表示询问 WAV 就绪；确认后进入 `EXECUTING`。只有执行成功 ACK 和新鲜车辆回读同时成立，空调服务才进入 `SUCCEEDED`。超时结果为 `UNKNOWN`，不自动重发。无响应、拒绝、离座、条件失效、过期语音或旧会话反馈都不能触发执行。

## FD 信号协议

首版适配器为小端序的 24 字节 Unix Datagram；Python 编码为 `struct.Struct("<HHIIB3xd")`：

| 字段 | 类型 |
| --- | --- |
| signal_id / source_id | uint16 / uint16 |
| source_generation / sequence | uint32 / uint32，均从 1 开始 |
| quality | uint8，0 无效 / 1 有效 |
| reserved | 3 字节，置零 |
| value | double，规范化标量 |

全局信号 0–3 为 cabin.temperature（℃）、hvac.on（0/1）、vehicle.speed（km/h）、interaction.busy（0/1）。驾驶员、前排乘员、左后排、右后排各占四项：present、fanning、fatigue、discomfort；行为/疲劳为 0–1 置信度。完整名称见 `sample/cockpit_replay.py`。

每个信号绑定首次有效来源；同来源重启递增 generation。序号不回绕，耗尽时递增 generation。`source_id=15` 和 `hvac.on` 保留给车辆模拟器，管理注入和场景回放拒绝覆盖它。部署环境需限制本地 Socket 的写入权限；当前不是通用不可信数据源接入服务。

状态同值在 250 ms 内可过滤，持续状态至少周期刷新；行为事件只有重复序号被去重。`signal_ttl_ms` 应大于 250 ms 的刷新间隔。连续量合并保留温度 26/28℃、速度 0 等必要边沿；不可保留的行为/开关边沿溢出会带外通知核心撤销该来源相关证据。

真实 CAN/SOME-IP/DMS 接入留在 Ingress 的 FD 适配边界；当前没有真实 CAN 或 DMS SDK 实现。流式 FD 需要独立半包解码，不能直接套用数据报帧读取。

## 时延与验证

每条入队信号计算接入、排队、Context 更新、总处理时延；过滤/合并样本另计原始处理耗时。候选、策略、服务和模型子调用写入 `traces.jsonl`，Supervisor 启动/模型加载/绑定写入 `supervisor-traces.jsonl`。失败、取消、超时也保存终态和耗时；未发生阶段为 null。

```bash
(cd build/cockpit && ctest --output-on-failure)
python3 tests/cockpit_benchmark.py --seconds 3
python3 sample/cockpit_report.py artifacts/cockpit/traces.jsonl \
    --output artifacts/cockpit/latency-report.json
```

感知直方图按固定桶报告上界估计；低频追踪报告使用实际样本的最近秩百分位。样本不足 1000 时 P99.9 为 null。源设备延迟、内核排队和真实播放未被测量。快路径统计从候选确认开始，800 ms/2 s 的需求观察、TTS 合成、交互等待和车辆执行分别列出，不能合并后声称为纯决策耗时。

`tests/cockpit_benchmark.py` 默认不加载模型；`--models` 可以同场加载模型。CPU 是 cockpit 进程总量、单核 100% 口径，RSS 不包括 ASR/TTS/LLM。主机实测见 [座舱验证记录](cockpit-validation.md)。

原 `sample/voice_demo.py` 仍保留自动上游订阅流程。新模式不设置 `setup.data.input`，由座舱编排。共用节点增加了可选受理 ACK、请求取消和直接 TTS 文本入口，旧客户端默认收不到新增 ACK。

## RK3588 部署

准备匹配 SoC、SDK/runtime、驱动的 `.rkllm` 权重，将 `configs/cockpit-rk3588.json` 的模型路径改为板端实际路径。没有权重或驱动错误会明确失败，不降级成 mock。

在板端原生构建完整程序：

```bash
cmake -S . -B build/rk-native -DCMAKE_BUILD_TYPE=Release \
    -DEDGE_BUILD_COCKPIT=ON -DEDGE_ENABLE_RKLLM=ON \
    -DVOICE_DEPS_ROOT=/opt/edge/voice-deps \
    -DRKLLM_ROOT=/opt/edge/voice-deps/rkllm -DBUILD_TESTING=OFF
cmake --build build/rk-native -j2
python3 sample/cockpit_supervisor.py --build-dir build/rk-native \
    --config configs/cockpit-rk3588.json --output-dir /var/lib/edge-cockpit
```

`deploy/edge-cockpit.service` 提供开机启动示例，路径按部署目录调整，默认关闭自动重启。仓库只提供配置，不会自动安装或启动系统服务。

在板上运行同样的回放和交互流程，保存 trace、WAV、LLM 输出和性能报告。`scripts/build_rkllm_aarch64.sh` 仅验证真实 LLM 及通信 SDK 链接，不能替代完整座舱板端构建和实测。当前环境无 RK 设备和适配权重，P7 实机闭环、模型输出质量及板端性能仍待验收。
