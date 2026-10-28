# Edge-LLM-Comm-Infra

面向 Linux 端侧 AI 的 C++17 通信框架与主动智能座舱助手，为 ASR、LLM、TTS 等业务提供节点通信、任务生命周期管理和异步调用能力，并在此基础上实现持续感知、需求判断、主动询问、用户确认和车辆模拟执行。

当前采用单机多进程架构，提供 **x86 主机验证配置**和 **RK3588 部署配置**。ASR 使用真实 Sherpa-ONNX，TTS 使用真实 SummerTTS；LLM 在主机上使用显式 mock，在 RK 配置中使用真实 RKLLM 后端。

## 项目背景

车载语音问答从用户提交音频开始，依次完成识别、推理和语音合成。主动座舱还需要在用户没有发起请求时，持续接收车辆与乘员信号，判断是否出现服务需求，并选择合适的时机与用户交互。

Edge-LLM-Comm-Infra 通过分层设计，将底层通信、任务管理、模型推理和座舱业务分开：`unit_manager` 负责登记与路由，Supervisor 负责系统启动和 Task 生命周期编排，`edge_cockpit` 负责长期感知与服务决策，ASR/LLM/TTS 节点提供独立的模型能力。

系统支持两条业务链路：

- **主动服务**：车辆与乘员信号 → Context → 需求状态机 → 快慢路径决策 → 主动询问 → 用户确认 → 模拟执行 → 状态回读。
- **语音问答**：WAV 文件 → ASR 识别 → LLM 回答 → TTS 合成 → WAV 文件。

## 架构概览

```text
┌──────────────────────────────────────────────────────────────────────┐
│ 应用入口：Supervisor / Interaction Client / Signal Replay            │
│ 自主启动与监测 · 用户交互 · 场景回放                                 │
├──────────────────────────────────────────────────────────────────────┤
│ node：业务节点层                                                     │
│ edge_cockpit：感知、Context、FSM、决策、仲裁、交互与服务执行         │
│ edge_asr / edge_llm / edge_tts：识别、推理、合成                     │
├──────────────────────────────────────────────────────────────────────┤
│ infra-controller：信道管理与节点基类                                 │
│ StackFlow：管理 RPC、生命周期事件、Task 注册与释放                   │
│ llm_channel_obj：输入订阅、结果发布、显式请求上下文与回包            │
├──────────────────────────────────────────────────────────────────────┤
│ unit-manager：统一接入与任务路由                                     │
│ TCP Session · Task 登记 · IPC 地址分配 · action 路由 · 运行实例目录  │
├──────────────────────────────────────────────────────────────────────┤
│ network：TCP / FD Reactor 库                                         │
│ EventLoop · epoll · Channel · 连接管理 · 跨线程唤醒                  │
├──────────────────────────────────────────────────────────────────────┤
│ hybrid-comm：ZeroMQ 通信封装                                         │
│ PUB/SUB · PUSH/PULL · REQ/REP RPC · 消息与 Socket 生命周期           │
└──────────────────────────────────────────────────────────────────────┘
```

上图按职责展示分层。代码依赖主要为 `node → infra-controller → hybrid-comm`、`unit-manager → network + stackflow`；座舱的 Ingress 和核心循环另外复用 `network` 的 Reactor。

模型调用继续经过 manager；高频车辆信号通过本地 FD 直接进入座舱的 Ingress。

## 模块说明

### 1. hybrid-comm — 底层混合通信中间件

基于 ZeroMQ 封装发布订阅、点对点回传和 RPC，为 SDK 与 manager 提供统一的通信接口。

| 文件 | 功能 |
| --- | --- |
| `hybrid-comm/include/pzmq.hpp` | Socket 创建与收发、RPC action 注册、调用超时和所属线程管理 |
| `hybrid-comm/include/pzmq_data.h` | ZMQ 消息的 RAII 封装与参数读取接口 |
| `hybrid-comm/src/pzmq_data.cpp` | 消息存储、长度前缀参数封装与解析 |

关键设计：

- **Socket 线程归属**：持久 Socket 由专属线程创建、使用和关闭；其他线程通过发送队列提交数据。
- **RPC 调用**：服务端维护 `action → callback` 映射；客户端使用调用局部的 REQ Socket，发送 action 和数据并等待回复。
- **显式超时**：设置发送和接收超时，调用失败向上层返回错误。
- **运行目录隔离**：节点 RPC 地址由节点名和 `EDGE_RPC_DIR` 组成，默认形式为 `ipc:///tmp/rpc.<node>`。

### 2. network — TCP 网络库与 Reactor

采用主从 Reactor 组织 TCP 连接，提供独立于 ZeroMQ 的 FD 事件循环。manager 使用它处理客户端连接，座舱使用它处理感知 FD、`eventfd` 和 `timerfd`。

```text
EventLoop（每线程一个事件循环）
  ├── Poller                  epoll 监听与事件获取
  ├── Channel                 FD、关注事件和回调
  └── wakeupFd                跨线程唤醒

TcpServer
  ├── Acceptor                监听与接收连接
  ├── EventLoopThreadPool     分配连接所属的 I/O 线程
  └── TcpConnection           连接状态与收发
       ├── Socket             FD 资源管理
       ├── Channel            读写事件注册
       └── Buffer             输入与输出缓冲
```

| 核心类 | 职责 |
| --- | --- |
| `EventLoop` | 执行当前线程的事件循环和跨线程提交的任务 |
| `Poller` | 管理 epoll 注册、修改、移除和就绪事件 |
| `Channel` | 将 FD 的读、写、关闭和错误事件绑定到回调 |
| `TcpServer` / `Acceptor` | 监听端口、接收连接并交给工作线程 |
| `TcpConnection` | 管理单个连接的状态、缓冲区与发送过程 |
| `EventLoopThreadPool` | 管理 I/O 线程，按轮转方式分配连接 |
| `Buffer` / `Socket` | 管理网络数据缓冲与 Socket 资源 |

manager 的 TCP I/O 线程池与座舱感知线程相互独立。感知适配回调直接在单个 Ingress Reactor 内完成轻量处理。

### 3. infra-controller — 信道管理与节点基类

提供静态 SDK `libstackflow.a`，将节点生命周期、任务信道与底层传输衔接起来。

#### llm_channel_obj（任务信道）

每个 Task 持有自己的 Channel，连接输入、结果订阅者和请求发起者：

```text
llm_channel_obj
  ├── SUB        订阅本任务输入，或订阅指定上游 Task 的结果
  ├── PUB        发布本任务输出，供其他节点订阅
  └── PUSH       根据请求回包地址，将结果送回 manager 的 Session
```

- **输入订阅**：`subscribe_message(work_id, callback)` 提供解析后的 JSON 和显式请求上下文。
- **异步回包**：`send_with_context()` 使用本次请求的 `request_id` 和 `reply_url`，将结果送回正确的客户端。
- **输出控制**：`enoutput` 控制是否回传客户端；结果 PUB 可以供下游订阅。
- **上游连接**：原文件语音演示使用 `setup.data.input` 串联 ASR、LLM 和 TTS；主动模式由座舱分别调用三个模型。

#### StackFlow（业务节点基类）

- **生命周期接口**：提供 `setup()`、`exit()`、`pause()`、`taskinfo()` 等节点管理接口。
- **事件处理**：管理 RPC 将命令放入事件队列，再由 Flow 事件线程处理；当前使用标准库互斥锁和条件变量。
- **任务注册**：通过 manager 的 `sys.register_unit` 申请 Task ID 和输入、输出 IPC 地址。
- **资源释放**：退出时停止订阅和业务线程，释放 Channel，并向 manager 注销 Task。
- **派生类初始化**：节点可在成员初始化完成后调用 `start_events()`，避免启动请求提前访问业务对象。

管理 RPC 返回 `"None"` 表示命令已入队。客户端需要等待实际的 `sys.setup` 等完成响应，才能确认对应操作成功。

#### StackFlowUtil（协议工具）

| 接口 | 用途 |
| --- | --- |
| `sample_get_work_id_num/name()` | 解析 `llm.1` 等任务编号与节点名 |
| `sample_json_str_get()` | 使用 JSON 解析器读取字段 |
| `decode_stream()` | 拼接带 `index/delta/finish` 的流片段并检查重复、缺片 |
| `unit_call()` | 调用指定节点的管理 RPC |

### 4. unit-manager — 统一接入、任务登记与消息路由

为客户端提供 TCP 入口，为节点分配 Task ID 和通信地址，并将请求和结果送到对应目标。业务进程由 Supervisor 启动，座舱服务决策由 `edge_cockpit` 执行。

#### 核心组件

| 组件 | 文件 | 职责 |
| --- | --- | --- |
| 内存登记表 | `all.h`、`main.cpp`、`remote_server.cpp` | 保存配置、任务地址和活动任务对象，使用锁保护共享状态 |
| 管理 RPC | `remote_server.cpp` | 提供 `register_unit`、`release_unit` 和 `sql_select/set/unset` 等接口 |
| 请求分流 | `remote_server.cpp` | `unit_action_match()` 根据 action 选择推理、生命周期或运行目录路径 |
| 生命周期转发 | `remote_action.cpp` | 将 setup、exit 等命令转为目标节点的 RPC 调用 |
| TCP Session | `session.h`、`tcp_comm.cpp` | 保存客户端连接，建立该会话的回包 PULL，转发结果到 TCP |
| Task 输入通道 | `unit_data.cpp` | 持有 manager 侧的 PUB，将输入发布给目标 Task |
| 运行实例目录 | `runtime_directory.cpp` | 发布、查询和撤销当前运行实例及 cockpit/asr/llm/tts 的实际 Task 映射 |
| 启动配置 | `config.cpp`、`master_config.json` | 加载 TCP 端口、IPC 地址格式及任务地址池范围 |

#### 请求路由流程

```text
Supervisor / 交互客户端 / 座舱模型调用端
                  │ TCP JSON 请求
                  ▼
          TcpServer → TcpSession
                  │
                  ▼
          unit_action_match()
                  │
                  ├── action="runtime"
                  │     └── 运行实例目录：publish / lookup / remove
                  │
                  ├── action="setup/exit/pause/taskinfo"
                  │     └── 节点管理 RPC → Flow 事件队列 → 生命周期接口
                  │
                  └── action="inference"
                        └── 查找 work_id → Task 输入 PUB
                                              │
                                              ▼
                                     节点 Channel SUB
                                              │
                                              ▼
                                         Task 业务处理
                                              │
                                              ▼
客户端 TCP ← TcpSession PULL ← Channel PUSH（显式请求回包地址）
```

推理输入直接进入 Task 的订阅回调。座舱将控制消息放入核心信箱，模型节点将业务输入放入自己的工作队列。

#### 地址与 Task 发现

- **TCP 地址**：默认 `127.0.0.1:10001`，Supervisor 可通过 `--port` 修改监听端口。
- **Task ID**：setup 完成后返回，例如 `cockpit.0`、`llm.2`；运行时使用实际返回值。
- **Task IPC 地址**：默认从 `[5010, 5555)` 编号池分配，每个 Task 使用两个编号；这些编号用于 IPC 路径。
- **Session 回包地址**：每条 TCP 连接单独建立 PULL 地址，随请求传给节点。
- **运行实例目录**：交互端先查询当前 cockpit Task，再绑定会话；目录中的模型 ID 还需经过能力探测才能使用。

### 5. node — 语音模型与主动座舱业务

`VoiceNode` 和 `CockpitNode` 继承 `StackFlow`，分别创建模型任务和长期座舱任务。当前每个业务节点进程只维护一个活动 Task；Task 可以持续接收多次调用。

#### ASR / LLM / TTS（语音模型节点）

| 节点 | 主要文件 | 输入与输出 |
| --- | --- | --- |
| `edge_asr` | `node/asr/asr_task.cpp` | WAV 文件 → Sherpa-ONNX Zipformer → 最终识别文本 |
| `edge_llm` | `node/llm/llm_task.cpp`、`rkllm_backend.cpp` | 文本或结构化决策请求 → 流式回答或服务建议 |
| `edge_tts` | `node/tts/tts_task.cpp` | 文本或流式文本 → SummerTTS 合成 → WAV 文件 |
| 公共业务层 | `node/common/voice_task.*`、`wav.*` | 工作队列、输出队列、请求隔离、可选受理 ACK 和 WAV 读写 |

关键设计：

- **模型常驻**：setup 完成模型加载，后续 inference 复用同一个 Task。
- **后端显式选择**：LLM 使用 `mock` 或 `rkllm`；真实后端加载失败会返回错误。
- **请求级取消**：`llm.cancel` 指定待取消请求，取消处理独立于推理工作队列，确认退出后可继续复用模型。
- **流式关联**：同一次模型调用的片段共享 request_id，index 从 0 连续递增，finish 标识本次输出结束。
- **输出隔离**：SDK 回调复制输出，业务与发布线程通过队列交接，Socket 保持自身线程归属。

`node/test` 保留早期通信示例；真实语音与主动场景使用上述业务节点。

#### edge_cockpit（长期座舱任务）

```text
CockpitNode : StackFlow("cockpit")
  └── CockpitTask
       ├── Ingress             FD 接入、字段转换、过滤与入队
       ├── Context             最新信号、TTL、来源序号与语义版本
       ├── 需求状态机          观察窗口、次数证据、候选与冷却
       ├── 服务仲裁            优先级、交互占用、确认与执行跟踪
       ├── ModelClient         独立 TCP 连接，异步调用 ASR / LLM / TTS
       ├── VehicleSimulator    模拟受理、执行结果与车辆状态回读
       └── TraceLog            后台写入逐次耗时和终态记录
```

| 文件 | 职责 |
| --- | --- |
| `ingress.*` | 多 FD 非阻塞接入、去重、刷新、过载合并与来源缺口通知 |
| `context.*`、`timers.h` | 固定信号数组、依赖索引、需求判断和有界定时器 |
| `cockpit_task.*` | 核心循环、线程生命周期、控制与结果信箱 |
| `cockpit_control.cpp` | 会话租期、能力绑定、子调用登记和取消 |
| `cockpit_services.cpp`、`cockpit_results.cpp` | 快慢路径仲裁、用户反馈、结果复核和服务状态推进 |
| `model_client.*`、`vehicle_simulator.*` | 模型网络 I/O 与车辆模拟执行 |
| `types.h`、`metrics.*` | 精简业务结构、阶段计时、统计与追踪 |

#### 感知与线程模型

```text
车辆 / 乘员信号 FD ─────────┐
管理注入的 Socket 桥接 ────┼──→ Ingress Reactor（唯一生产者）
车辆状态回读 ──────────────┘            │
                                       ▼
                              SPSC Ring Buffer（1024）
                                       │
                                       ▼
                              Context 核心线程（唯一消费者）
                              Context / FSM / 策略 / 仲裁
                                       │
                          有界控制、结果与执行队列（128）
                                       │
                         模型 I/O / 输出发布 / 车辆执行线程
```

Ingress 的 `epoll` 回调直接执行字段读取、单位转换和过滤，过滤后的 `SignalUpdate` 才进入 SPSC。现有 ZMQ 线程提交的调试信号通过本地 Socket 桥接进入同一个 Ingress。

Context 核心线程独占业务状态。高频信号使用固定结构和整数 ID；只有慢路径候选保存所需的四个依赖字段。五种核心类型为：

| 类型 | 用途 |
| --- | --- |
| `SignalUpdate` | 一次信号更新及接收、入队时间 |
| `SignalState` | 最新值、更新时间、版本和有效性 |
| `Candidate` | 已确认需求、有效期及必要的慢路径快照 |
| `ServiceInstance` | 一项主动服务及其交互、执行状态 |
| `PendingCall` | 模型子调用与候选、服务、能力代次的关联 |

#### 主动服务场景

| 场景 | 默认判断条件 | 决策与服务 |
| --- | --- | --- |
| 热舒适 | 乘员在位、空调关闭、温度 ≥28℃，存在置信度 ≥0.8 的扇风证据，观察 800 ms；温度 ≤26℃解除热状态 | 本地快路径询问是否开启空调 |
| 疲劳提醒 | 驾驶员在位、车辆行驶、疲劳置信度 ≥0.8，持续有效 2 s | 本地快路径询问是否显示休息提醒 |
| 模糊舒适需求 | 5 s 内至少两次有效不适行为，且未命中明确快路径 | LLM 返回白名单服务建议或 `no_action` |

```text
需求确认 → 仲裁 → ASKING → WAITING_CONFIRMATION → EXECUTING → SUCCEEDED
                     │              │                │
                     │              │                ├── FAILED
                     │              │                └── UNKNOWN
                     └──────────────┴──→ CANCELLED / EXPIRED / FAILED
```

- **快慢路径分开**：明确需求直接使用模板请求 TTS；模糊需求才进入 LLM 决策。
- **一次主动询问**：服务仲裁检查有效期、证据、忙碌状态、会话和能力；休息提醒优先于舒适建议。
- **确认后执行**：WAV 就绪后等待用户确认，文本反馈携带服务 ID 和会话令牌；语音确认还要经过 ASR。
- **结果复核**：过期候选、旧会话和已失效能力的返回结果不能推进当前服务。
- **状态回读**：空调服务同时取得成功 ACK 和新鲜回读后才算成功；结果未知标记为 UNKNOWN。

### 6. sample / Supervisor — 系统启动与交互入口

Supervisor 是独立 Python 管理程序，通过 manager 的现有协议创建和配置 Task。它与交互客户端各自持有连接。

| 文件 | 职责 |
| --- | --- |
| `sample/cockpit_supervisor.py` | 启动进程、创建 Task、发布实例目录、绑定能力、监测故障与有序停止 |
| `sample/cockpit_client.py` | 查询座舱、绑定与续期会话、提交 WAV、接收主动事件和反馈确认 |
| `sample/cockpit_replay.py` | 按场景时间线通过本地 FD 回放信号 |
| `sample/cockpit_report.py` | 汇总候选、服务和模型调用的阶段时延 |
| `sample/voice_demo.py` | 保留原有 ASR → LLM → TTS 文件语音演示 |

#### 自主启动流程

```text
Supervisor
  ├── 1. 启动 unit_manager，建立管理连接
  ├── 2. 启动 edge_cockpit，发送 setup
  ├── 3. 等待感知就绪，发布实际 cockpit Task ID
  ├── 4. 启动 ASR / LLM / TTS，分别完成模型 setup
  ├── 5. 收集实际 Task ID，提交版本化能力绑定
  ├── 6. 座舱探测各项能力，返回逐项绑定结果
  └── 7. 持续监测进程退出，更新能力并执行有序停止

交互客户端（可稍后接入）
  └── 查询运行目录 → 绑定会话 → 提交输入 / 接收主动询问 → 续期或解绑
```

就绪状态分别表达：setup 表示 Task 创建完成，`PERCEPTION_READY` 表示感知循环可用，`cockpit.capabilities.bound` 返回当前版本各模型的绑定结果，`interaction_ready` 表示存在有效交互会话。

交互端默认每 2 s 续期，租期为 6 s。交互端退出只结束自己的会话；系统 Task 由 Supervisor 释放。模型初始化失败或进程退出时，对应能力降级，感知继续运行。

## 项目目录结构

```text
Edge-LLM-Comm-Infra/
├── CMakeLists.txt                    # 统一构建入口与功能开关
├── cmake/                            # 依赖查找、AArch64 工具链
├── hybrid-comm/                      # ZeroMQ 通信封装
│   ├── include/pzmq.hpp
│   ├── include/pzmq_data.h
│   └── src/pzmq_data.cpp
├── network/                          # Reactor 与 TCP 网络库
│   ├── include/network/              # EventLoop、Channel、TcpServer 等
│   └── src/
├── infra-controller/                 # StackFlow SDK
│   ├── include/                      # StackFlow.h、channel.h、工具接口
│   └── src/
├── unit-manager/                     # 网关、登记表、地址分配与路由
│   ├── include/
│   ├── src/                          # 包含 runtime_directory.cpp
│   └── master_config.json
├── node/
│   ├── common/                       # 模型任务公共层与 WAV 工具
│   ├── asr/                          # Sherpa-ONNX 识别适配
│   ├── llm/                          # mock / RKLLM 推理适配
│   ├── tts/                          # SummerTTS 合成适配
│   ├── cockpit/                      # 长期感知与主动服务运行时
│   └── test/                         # 早期通信示例
├── sample/
│   ├── cockpit_supervisor.py         # 自主启动与监测
│   ├── cockpit_client.py             # 交互客户端
│   ├── cockpit_replay.py             # 场景信号回放
│   ├── cockpit_report.py             # 逐次时延汇总
│   ├── voice_demo.py                 # 文件语音演示
│   └── rkllm_board_test.py            # 真实 RKLLM 板端验证入口
├── configs/
│   ├── cockpit-x86.json              # 主机主动座舱配置
│   ├── cockpit-rk3588.json            # RK3588 主动座舱配置
│   ├── voice-x86.json                # 主机文件语音配置
│   ├── voice-rk3588.json              # RK 文件语音配置
│   └── scenarios/                    # heat / rest / comfort 场景
├── scripts/
│   ├── build_cockpit.sh              # 座舱构建与测试
│   ├── build_voice.sh                # 通用构建与测试
│   ├── build_rkllm_aarch64.sh         # 真实 LLM 的交叉构建与链接检查
│   └── prepare_voice_deps.py         # 抽取和校验算法、模型及 SDK 依赖
├── tests/                            # 单元、集成、并发和性能验证
├── deploy/edge-cockpit.service        # RK 开机启动示例
├── docs/                             # 协议、部署、计划与验证记录
├── utils/                            # JSON 头文件与公共工具
├── voice-deps/                       # 独立算法依赖与模型，不提交 Git
├── build/                            # 构建产物，不提交 Git
└── artifacts/                        # 运行日志、WAV 与报告，不提交 Git
```

## 构建与运行

### 先看这次要跑什么

下面以 **Ubuntu / x86_64 服务器**为例，从启动到查看时延完整走一遍。服务器不需要麦克风、扬声器或真实车辆，输入使用场景文件和自带 WAV，输出保存为事件、音频文件和时延报告。

默认配置 `configs/cockpit-x86.json` 使用 **真实 ASR、真实 TTS、mock LLM 和车辆模拟器**。可以验证通信与业务闭环；这里的 LLM 耗时代表模拟后端，真实 RKLLM 推理需在板端测量。

```text
准备依赖 → 编译 → 启动系统 → 回放车辆信号 → 输入 y 确认 → 看到 SUCCEEDED
                                      ↓
                        提交测试 WAV → 得到语音回答文件
                                      ↓
                         正常停止 → 生成并查看时延报告
```

**已有 `voice-deps/` 和编译好的 `build/cockpit/bin/` 时，直接从第 3 步开始。** 想自动测试不同负载下的感知时延，使用第 7 步。

### 1. 准备服务器环境（首次运行）

先进入你实际存放项目的目录。后面的命令都在这个目录执行；新开终端也要先进入同一目录。

```bash
# 将这一行的路径换成你的项目路径
cd /path/to/Edge-LLM-Comm-Infra

# Ubuntu / Debian；已经安装这些依赖时可跳过
sudo apt-get update
sudo apt-get install build-essential cmake libzmq3-dev libgoogle-glog-dev \
    libboost-dev libssl-dev python3 curl
```

需要 GCC/G++ 9+、CMake 3.16+ 和 Python 3。Python 用于启动管理、命令行交互和回放；真正运行的 manager、座舱及模型节点是编译后的 C++ 程序。

**还需要准备 `voice-deps/`，只克隆 Git 仓库不会带上模型和算法依赖。** 已经有这份目录时，将它完整放到仓库根目录下；可以这样检查主要文件：

```bash
ls voice-deps/asr/sherpa-onnx/CMakeLists.txt \
   voice-deps/tts/CMakeLists.txt \
   voice-deps/models/asr/zipformer/tokens.txt \
   voice-deps/models/asr/zipformer/test_wavs/2.wav \
   voice-deps/models/tts/single_speaker_fast.bin
```

如果还没有 `voice-deps/`，但保留着原始语音项目，可用下面的命令抽取。源目录必须直接包含 `voice`、`tts`、`llm`：

```bash
python3 scripts/prepare_voice_deps.py /path/to/LLM_Voice_Flow
```

已有依赖目录时不必再次抽取。下面的快速启动统一使用仓库内的 `voice-deps/`；放在其他位置的配置方法见 [语音依赖说明](docs/voice-dependencies.md)。

### 2. 编译程序（首次运行或修改代码后）

```bash
./scripts/build_cockpit.sh
```

这条命令会编译所有节点并执行测试。首次编译需要下载固定版本的算法依赖，包括 ONNX Runtime，耗时通常比后续编译长。完成后应看到测试通过，并生成：

```text
build/cockpit/bin/
├── unit_manager      # 通信入口和任务登记
├── edge_cockpit      # 主动座舱核心
├── edge_asr          # 语音识别
├── edge_llm          # 语言模型节点
└── edge_tts          # 语音合成
```

如果服务器内存较小，可以用 `EDGE_BUILD_JOBS=2 ./scripts/build_cockpit.sh` 降低编译并行度。已有同版本 Sherpa 共享库时，可用文末的构建选项复用。

### 3. 启动整个系统（终端 1，保持运行）

打开三个连接到**同一台服务器**的终端，分别进入仓库根目录。先在终端 1 执行：

```bash
python3 sample/cockpit_supervisor.py
```

Supervisor 会自动启动 manager、座舱、ASR、LLM 和 TTS，并创建 Task。**不需要再手动逐个启动这些程序。**

等待终端打印包含 `runtime_id`、`port`、`tasks` 的一行 JSON，其中 `cockpit/asr/llm/tts` 应都有实际 Task ID。编号由运行时分配，不必与文档示例一致。再在另一个终端检查启动记录：

```bash
python3 -m json.tool artifacts/cockpit/state.json
```

本次完整演示需要 `tasks` 中四项都不是 `null`，且 `errors` 为 `{}`。如果有错误，先按后面的“常见问题”处理，再继续。终端 1 会持续运行，保持打开。

### 4. 触发一次主动服务（终端 2、终端 3）

**终端 2：打开交互窗口。** 它负责展示询问，并接收你输入的确认或拒绝。

```bash
python3 sample/cockpit_client.py --port 10001
```

看到“已连接座舱，等待主动询问。”后，保持这个终端打开，然后执行下一条命令。

**终端 3：模拟“车内热、驾驶员扇风”的场景。**

```bash
python3 sample/cockpit_replay.py \
    --runtime-dir artifacts/cockpit/runtime \
    --scenario configs/scenarios/heat.json --seconds 60
```

回放器发送温度 30℃、驾驶员在位和扇风等信号，并在 60 秒内持续刷新状态。回放期间通常没有终端输出，**回到终端 2 看结果**：

| 终端 2 中的内容 | 表示什么 / 你要做什么 |
| --- | --- |
| `"phase": "ASKING"`，以及“车内有些热，需要我帮你打开空调吗？” | 座舱确认了需求，开始生成询问音频 |
| `"phase": "WAITING_CONFIRMATION"`，附带 `audio.path` | WAV 已生成，等待你确认 |
| `确认请输入 y；拒绝请输入 n；取消请输入 c，然后回车：` | 在默认 30 秒内输入 `y` 并回车 |
| `"phase": "EXECUTING"` | 已确认，开始调用车辆模拟器 |
| `"phase": "SUCCEEDED"` | 执行成功，并收到了空调开启的状态回读 |

看到 `SUCCEEDED`，就完成了 **信号输入 → 需求判断 → 主动询问 → 用户确认 → 模拟执行 → 回读** 的闭环。服务器会生成 WAV 文件；当前客户端不会自动播放声音，可以将 `audio.path` 对应的文件下载到本机试听。

输入 `n` 会拒绝服务，输入 `c` 才会取消；空回车或其他字符只提示重输，不发送反馈。不输入有效选项会超时。第一次复现建议先输入 `y`，确认成功路径。服务已结束时再输入 `y`，客户端会提示“当前没有待确认的服务”，需要等待下一次询问。

### 5. 再走一遍 ASR → LLM → TTS（仍使用同一个系统）

热舒适是快路径，直接使用模板和 TTS。要把三个模型节点都调用一遍，可以使用依赖目录里自带的测试音频，不必自己录音。

先在终端 3 按 `Ctrl+C` 停止回放，再在终端 2 按 `Ctrl+C` 关闭原交互端。**终端 1 的 Supervisor 继续运行。** 然后在终端 2 执行：

```bash
python3 sample/cockpit_client.py --port 10001 \
    --wav voice-deps/models/asr/zipformer/test_wavs/2.wav
```

依次看到以下 `object` 就说明问答链路跑通了：

```text
asr.utf-8          识别出的文字
llm.utf-8.stream   mock LLM 返回的回答片段
  ...
tts.audio.file    最终回答音频的路径
```

出现 `tts.audio.file` 表示本轮完成；客户端随后继续等待主动事件，可以按 `Ctrl+C` 退出。当前只有一个有效交互会话，因此要先关闭原交互端再运行这条命令。

使用自己的音频时，把 `--wav` 后面的路径换成服务器上的 WAV 路径。格式要求为 **16 kHz、单声道、16-bit PCM，最长 120 秒**。

### 6. 正常停止，然后查看这次的时延

先停止回放和交互端，最后在终端 1 按 `Ctrl+C`，等 Supervisor 退出并回到 shell 提示符。它会释放 Task、停止进程，并写入最终统计。 子进程与终端信号隔离，由 Supervisor 统一安排退出；等这一轮完全结束后再重新启动。

**先正常停止再生成报告，报告才会包含最终感知统计 `final_status`。** 然后在任意终端、仓库根目录执行：

```bash
# 业务与感知耗时：本次主动服务、语音问答、模型子调用
python3 sample/cockpit_report.py artifacts/cockpit/traces.jsonl \
    --output artifacts/cockpit/latency-report.json

# 启动耗时：感知就绪、模型加载、能力绑定
python3 sample/cockpit_report.py artifacts/cockpit/supervisor-traces.jsonl \
    --output artifacts/cockpit/startup-report.json
```

成功时命令不会额外打印内容。直接复制下面这段，可以在终端查看感知指标和主要业务阶段，单位都是 **毫秒**：

```bash
python3 - <<'PY'
import json
from pathlib import Path

report = json.loads(Path("artifacts/cockpit/latency-report.json").read_text())
print("感知耗时（Ingress 收到信号 → Context 更新完成）：")
print(json.dumps(report["final_status"]["metrics"]["perception"], indent=2))
print("\n业务阶段：分类 / 需求 / 结果 / Task / 阶段，单位 ms")
for name, stats in report["groups"].items():
    if name.startswith(("service/", "question/", "call/", "decision/", "policy/")):
        print(name, "样本数=", stats["count"], "平均=", stats["mean_ms"],
              "P99=", stats["p99_ms"], "最大=", stats["max_ms"])
PY
```

先关注这些指标：

| 想了解什么 | 看哪里 | 时间范围 |
| --- | --- | --- |
| 信号处理快不快 | `final_status.metrics.perception` | Ingress 收到信号，到 Context 更新完成 |
| 时间花在入队、排队还是更新上 | 同一 `metrics` 下的 `ingress`、`queue`、`context` | 感知处理的三个阶段 |
| 需求观察了多久 | `groups` 中的 `decision/.../observation_ns` | 条件成立到确认需求；热舒适默认观察 800 ms |
| 多久生成主动询问音频 | `service/.../prompt_ready_ns` | 确认需求到询问 WAV 就绪，包含仲裁与 TTS |
| 用户等了多久才确认 | `service/.../user_wait_ns` | WAV 就绪到收到用户反馈 |
| 执行花了多久 | `service/.../execution_ns` | 发起模拟执行到服务终态；成功时包含状态回读 |
| ASR、LLM、TTS 各花了多久 | `call/.../completed/<实际 Task ID>/latency_ns` | 座舱发起子调用到收到完成结果，包含通信与排队 |
| 整轮语音问答花了多久 | `question/.../latency_ns` | 接收音频请求到回答 WAV 就绪 |

报告的分组名称保留原始 `_ns` 字段名，**分组内部的 `mean_ms/p50_ms/p99_ms/max_ms` 已换算为毫秒**。原始 `traces.jsonl` 才使用纳秒，换算关系为 `1 ms = 1,000,000 ns`。

`p50_ms` 表示约一半样本不超过这个耗时，`p99_ms` 表示约 99% 样本不超过这个耗时；感知百分位使用直方图桶上界估计。只跑一次服务时，平均值和 P99 可能相同，想观察大量样本的分布请继续第 7 步。

**不要把等待用户确认的时间算成算法性能。** `service/.../latency_ns` 包含交互等待；信号处理、需求观察、模型推理与交互等待应分别看。当前没有测量物理传感器、Ingress 接收前的内核排队或扬声器实际播放耗时。

本轮文件都在 `artifacts/cockpit/`：

| 文件 | 用途 |
| --- | --- |
| `state.json` | 实际 Task ID、进程 ID 和启动错误 |
| `audio/*.wav` | 主动询问与语音回答音频 |
| `traces.jsonl` | 每个候选、服务、子调用的原始阶段耗时和终态，以及退出时的感知汇总 |
| `latency-report.json` | 上面生成的业务与感知时延报告 |
| `supervisor-traces.jsonl` / `startup-report.json` | 启动阶段的原始记录与汇总 |
| `sys.log`、`cockpit.log`、`asr.log`、`llm.log`、`tts.log` | 排查对应进程的错误 |

高频信号逐条计时后累计到统计表，不会每条都写一行日志。记录不存在的阶段为 `null`，不表示耗时为零。

**再次完整复现前**，停止所有演示进程后归档旧目录，避免把多次运行的追加日志混入同一份报告：

```bash
mv artifacts/cockpit "artifacts/cockpit-$(date +%Y%m%d-%H%M%S)"
```

随后重新从第 3 步启动即可。不要在 Supervisor 运行期间移动它的目录。

### 7. 自动压测，查看不同负载下的 P50 / P99

先结束前面的手动演示，减少其他进程对测量的干扰。下面的脚本会**自行启动和停止系统、产生信号并生成报告**，只需要一个终端，无需手动启动三个窗口，也无需输入 `y`。

```bash
# 只测感知：50 条/s、1000 条/s、10000 条/s、不限速突发，各跑 3 秒
python3 tests/cockpit_benchmark.py --seconds 3 \
    --output-dir artifacts/cockpit-performance

# 感知与语音问答同时运行：真实 ASR/TTS + 人为延迟 1.5 秒的 mock LLM
python3 tests/cockpit_benchmark.py --seconds 3 --models \
    --output-dir artifacts/cockpit-performance-models
```

`--seconds 3` 指**每档信号负载**的时长，整条命令还包含系统启动、模型加载和停止时间。每档额外有另一个 FD 持续发送 100 条/s，用于观察多 FD 接入。`--models` 在每档启动一轮语音问答，并检查它是否完成。

每档结束会打印一行结果，最后分别生成两个目录中的 `report.json`。用下面的命令并排查看重点：

```bash
python3 - <<'PY'
import json
from pathlib import Path

for folder in ("cockpit-performance", "cockpit-performance-models"):
    report = json.loads(Path("artifacts", folder, "report.json").read_text())
    print("\n" + folder)
    print("负载 | 样本数 | P50(ms) | P99(ms) | 最大(ms) | 队列溢出 | 发送端丢弃")
    for row in report["scenarios"]:
        metric = row["metrics"]["perception"]
        print(row["load"], metric["count"], metric["p50_ms"], metric["p99_ms"],
              metric["max_ms"], row["metrics"]["overflow"], sum(row["producer_dropped"]), sep=" | ")
PY
```

把 `--seconds` 调大可增加采样时长。`normal`、`1k`、`10k`、`burst` 对应上述四档负载。先看 P99、最大值和丢弃计数，再比较加载模型前后的变化；突发压测允许暴露过载，不能只看耗时而忽略丢弃。

原始报告还保留 CPU、内存、队列最大深度、合并次数和日志丢弃等指标。CPU 以单核 100% 为基准，CPU 和内存只统计座舱进程；这里的感知时延同样从 Ingress 收到数据开始计算。短时压测用于复现和比较，持续负载下的稳定性还需更长时间验证。

### 其他场景与常见问题

要验证其他需求，先按第 6 步停止并归档，再启动新实例，替换第 4 步的场景文件：

| 场景文件 | 预期行为 |
| --- | --- |
| `configs/scenarios/heat.json` | 热舒适快路径，询问是否开启空调 |
| `configs/scenarios/rest.json` | 疲劳快路径，询问是否显示休息提醒 |
| `configs/scenarios/comfort.json` | 模糊舒适慢路径，经 mock LLM 给出建议后询问 |

换场景时启动新实例，可以同时重置空调状态、需求冷却和信号序号。只重新运行回放器可能保留上一轮状态，无法重现首次触发。

| 遇到的问题 | 先检查什么 |
| --- | --- |
| 编译提示缺少模型或算法源码 | `voice-deps/` 是否完整；Git 不包含它，需要单独复制或抽取 |
| 客户端连接失败 | 终端 1 是否已完成启动；查看 `artifacts/cockpit/sys.log` |
| 某个模型的 Task 为 `null` | 查看 `state.json` 的 `errors`，以及对应的 `asr.log/llm.log/tts.log` |
| 交互端一直没事件 | 确认终端 3 正在回放、路径对应同一运行实例，并使用新实例验证场景 |
| 没听到声音 | 当前只生成 WAV，查看事件里的 `audio.path` 或 `data.path` |
| 报告的 `final_status` 为 `null` | 先正常退出 Supervisor，再重新生成报告；强制结束可能缺少退出统计 |
| 端口 10001 被占用 | Supervisor 加 `--port 10002`，所有交互端也改为 `--port 10002` |
| 重启报 `cannot bind ingress socket` 或 `runtime socket still in use` | 新版 Supervisor 会清理已无进程使用的残留 Socket；仍在用的地址会拒绝覆盖。先结束旧实例；若路径被普通文件占用，检查 `state.json` 的 `errors.startup`，不要直接删除整个运行目录 |
| 目录路径过长，Unix Socket 创建失败 | Supervisor 改用 `--output-dir /tmp/cockpit-demo`；回放的 `--runtime-dir` 改为 `/tmp/cockpit-demo/runtime`，报告也从该目录读取 |

### 其他构建与运行方式

已存在同版本 Sherpa 共享库时，可以复用，避免重新构建算法依赖：

```bash
./scripts/build_cockpit.sh -DSHERPA_ONNX_BUILD_DIR="$PWD/build/host"
```

| CMake 选项 | 默认值 / 用途 |
| --- | --- |
| `EDGE_BUILD_COCKPIT` | ON，构建主动座舱 |
| `EDGE_BUILD_MANAGER/ASR/TTS` | ON，分别控制网关和对应模型节点 |
| `EDGE_ENABLE_RKLLM` | OFF；开启后链接真实 RKLLM，要求 AArch64 |
| `VOICE_DEPS_ROOT` | 项目内 `voice-deps/` |
| `RKLLM_ROOT` | `${VOICE_DEPS_ROOT}/rkllm` |
| `SHERPA_ONNX_BUILD_DIR` | 可选的同版本 Sherpa 共享库构建目录 |
| `BUILD_TESTING` | ON，生成测试目标 |

只检查感知时，Supervisor 可以加 `--no-models`；这样无法完成需要模型的完整交互。重新运行测试、或单独体验原文件语音演示，可使用：

```bash
(cd build/cockpit && ctest --output-on-failure)
python3 sample/voice_demo.py --build-dir build/cockpit --rounds 3 --cycles 2
```

`voice_demo.py` 自行启动另一套语音演示，运行前先停止主动座舱。更多接口与部署细节见 [主动座舱说明](docs/cockpit.md)。

### RK3588 部署

服务器复现使用前面的 x86 配置。部署到 RK3588 时，先准备匹配 SoC、SDK/runtime 和驱动的 `.rkllm` 权重，调整 `configs/cockpit-rk3588.json` 中的模型路径，再在板端原生构建：

```bash
cmake -S . -B build/rk-native -DCMAKE_BUILD_TYPE=Release \
    -DEDGE_BUILD_COCKPIT=ON -DEDGE_ENABLE_RKLLM=ON \
    -DVOICE_DEPS_ROOT=/opt/edge/voice-deps \
    -DRKLLM_ROOT=/opt/edge/voice-deps/rkllm -DBUILD_TESTING=OFF
cmake --build build/rk-native -j2

python3 sample/cockpit_supervisor.py --build-dir build/rk-native \
    --config configs/cockpit-rk3588.json --output-dir /var/lib/edge-cockpit
```

后续交互与回放步骤相同，回放目录改为 `/var/lib/edge-cockpit/runtime`。`deploy/edge-cockpit.service` 提供开机启动配置。`scripts/build_rkllm_aarch64.sh` 可在主机交叉构建 LLM 节点及通信 SDK，并检查真实 RKLLM 库链接；完整座舱仍需在板端验证。

## JSON 通信协议

TCP 管理与业务请求采用 JSON，以换行符 `\n` 分帧，单帧上限 1 MiB。节点间保留相同的业务字段；高频感知 FD 使用独立的 24 字节定长二进制帧。

能力绑定请求示例：

```json
{
    "request_id": "supervisor-bind-4",
    "work_id": "cockpit.0",
    "action": "inference",
    "object": "cockpit.capabilities.bind",
    "data": {
        "runtime_id": "runtime-example",
        "version": 4,
        "bindings": {
            "asr": "asr.1",
            "llm": "llm.2",
            "tts": "tts.3"
        }
    }
}
```

示例中的实例和 Task ID 仅用于说明格式；实际值由 Supervisor 从 setup 完成响应中取得。绑定使用完整快照，能力值为 null 表示撤销该项绑定。

| 字段 | 说明 |
| --- | --- |
| `request_id` | 一次管理操作或模型调用的标识；同一次流式输出共享该编号 |
| `work_id` | setup 时指定节点名，后续调用指定实际 Task ID |
| `action` | setup / exit / pause / taskinfo / inference / runtime |
| `object` | 具体操作或结果类型，例如 cockpit.feedback、tts.audio.file |
| `data` | 由 object 定义的输入、配置或结果 |
| `created` | 响应生成时的 Unix 秒时间；阶段时延另用单调时钟计算 |
| `error` | 响应中的 `{code, message}` 对象，code 为 0 表示成功 |

主要业务对象：

| object | 用途 |
| --- | --- |
| `sys.runtime.publish/lookup/remove` | 通过 `action: runtime` 操作 manager 中的运行实例目录 |
| `cockpit.capabilities.bind/bound` | 版本化能力绑定及逐项处理结果 |
| `cockpit.session.bind/renew/unbind` | 交互端绑定、续期和解绑 |
| `cockpit.signal.batch` | 兼容管理入口的批量信号注入，经桥接交给 Ingress |
| `cockpit.speech.file` | 提交普通问答或指定服务的确认音频 |
| `cockpit.feedback` | 携带 service_id、session_token 和 confirm/reject/cancel 动作 |
| `cockpit.service.event` | 主动询问、执行阶段和终态通知 |
| `cockpit.status` | 感知、能力、会话、服务及指标查询 |
| `llm.decision.request/result` | 结构化服务建议，接受白名单参数或 no_action |
| `llm.cancel` | 取消指定模型请求 |
| `asr.audio.file` / `asr.utf-8` | 识别输入与最终文本 |
| `llm.utf-8.stream` | index / delta / finish 流式回答 |
| `tts.utf-8` / `tts.audio.file` | 直接文本合成与 WAV 结果 |

座舱为每个模型子调用生成独立 request_id，通过候选 ID 和服务 ID 关联业务。主动事件具有自己的事件编号和回包会话。高频信号使用整数来源与序号，接收和入队时间由 Ingress 记录。

## 关键设计模式

| 模式 / 方法 | 应用位置 | 作用 |
| --- | --- | --- |
| 分层设计 | network / hybrid-comm / SDK / manager / node | 将传输、任务管理和业务处理分开维护 |
| 模板方法 | `StackFlow` 生命周期接口 | 基类组织管理流程，派生节点实现业务创建与清理 |
| 工厂方法 | `VoiceTaskFactory` | 根据节点类型创建对应的 ASR、LLM 或 TTS Task |
| 回调与订阅 | `Channel`、`llm_channel_obj` | 将就绪事件、输入到达与业务处理衔接 |
| Reactor | manager TCP、Ingress、Context 核心循环 | 通过 FD 就绪与定时事件驱动处理 |
| 单写者与 SPSC | Ingress → Context | 固定生产和消费线程，集中维护感知状态 |
| 有限状态机 | 需求判断与主动服务 | 明确观察、确认、执行、冷却和终态转换 |
| 显式上下文 | `RequestContext`、`PendingCall` | 关联异步结果，检查目标、能力代次和截止时间 |
| RAII 与弱引用 | Socket、线程资源、Task 回调 | 释放资源并避免回调延长已退出任务的生命周期 |

## 性能特性

- **轻量感知路径**：固定结构、整数索引和预分配 SPSC，普通更新直接修改 Context 数组。
- **有界调度**：FD 回调和感知处理按批推进，使用 eventfd 合并唤醒、timerfd 驱动逻辑截止时间。
- **推理与感知分离**：模型 I/O、语音合成、车辆执行和日志写盘各有执行线程，快路径决策不等待 LLM。
- **过载显式处理**：可合并状态进入有界缓存；必须保留的事件溢出时通知核心撤销相关证据。
- **逐次计时**：记录每条入队信号的阶段耗时，以及每个候选、服务和子调用的成功、失败、拒绝、取消与超时耗时。

### 主机实测

Intel Core i9-13900K、Linux 6.8.0 x86_64、Release 构建；主 FD 输入 10000 条/s，另一 FD 另发 100 条/s，单项持续 3 s：

| 场景 | 处理样本数 | 感知 P99 | 感知最大值 | 感知队列溢出 |
| --- | ---: | ---: | ---: | ---: |
| 仅感知 | 30311 | 0.019455 ms | 0.194953 ms | 0 |
| 语音问答并发 | 30312 | 0.065535 ms | 0.179845 ms | 0 |

感知时延从 Ingress 收到数据开始，到 Context 更新完成为止；百分位为固定直方图上界估计。上述数据不含物理传感器、内核排队、需求观察、模型推理和用户确认，也不能直接外推到板端。

### 验证与当前范围

最新主机验收记录为 **6/6 项 CTest 通过**，覆盖 SDK 适配、原语音链路、WAV、感知核心、并发信箱和主动座舱集成。真实 ASR/TTS 已参与闭环；RKLLM 已完成 SDK 替身测试和真实 AArch64 库链接检查，板端推理仍待设备与匹配权重验收。

当前车辆与乘员输入使用模拟或回放，车辆执行使用模拟器。真实 CAN/SOME-IP/DMS、麦克风和播放尚待接入；Supervisor 实现启动、监测与降级，自动重启、Task 重建、跨设备发现和持久化恢复留待后续。PUB/SUB 保持原有投递语义，调用失败或结果未知由业务明确处理。

进一步说明：

- [主动座舱运行、协议与部署](docs/cockpit.md)
- [主机测试与完整性能记录](docs/cockpit-validation.md)
- [文件语音助手与 RKLLM 接入](docs/voice-assistant.md)
- [模型和算法依赖准备](docs/voice-dependencies.md)

## License

SPDX-License-Identifier: MIT

第三方算法源码、模型和 SDK 沿用各自许可，具体内容见依赖说明及对应目录中的许可文件。
