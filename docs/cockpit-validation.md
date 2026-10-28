# 主动座舱主机验证记录

本轮记录对应主机首版实现，测试日期 2026-10-20。真实 ASR、真实 SummerTTS，LLM 显式 mock；车辆动作由模拟器完成。

## 验收结果

- 6/6 CTest 通过，总墙钟时间 37.79 s：原有 RKLLM SDK 替身、真实语音集成和 WAV 测试，以及新增感知核心、并发信箱和主动座舱集成。
- 主动集成包含 15 组检查：三场景、普通问答、真实 ASR 歧义确认、无交互自主运行、绑定版本、租期重连、部分能力失败、拒绝/超时、旧候选撤销、模型取消后复用、进程退出降级。
- 4 个生产线程向有界控制信箱提交共 40000 条消息，无丢失、重复或永久等待。感知仍只有一个生产者。
- 感知核心通过 AddressSanitizer + UndefinedBehaviorSanitizer（含 leak detection）。这只覆盖该测试目标，不代表全部第三方模型代码经过内存审计。
- ThreadSanitizer 二进制构建成功，但运行前因 `unexpected memory mapping` 失败，未获得 TSan 竞争检查结果。
- 新取消逻辑通过真实 AArch64 RKLLM runtime 链接检查；ELF 为 AArch64，引用 `rkllm_init/run/abort/destroy`。没有运行真实板端推理。

完整记录：[`artifacts/cockpit-validation/summary.json`](../artifacts/cockpit-validation/summary.json)、[CTest 日志](../artifacts/cockpit-validation/ctest.log)、[集成报告](../artifacts/cockpit-validation/integration/report.json)。这些运行产物按项目规则不提交 Git。

## 感知时延

环境：Intel Core i9-13900K、Linux 6.8.0 x86_64、GCC 9.3、Release；每项持续 3 s。第二个 FD 始终另发 100 条/s 行为事件，车辆模拟器独立发布状态回读。未绑核，未启用实时调度；不能把短时主机结果外推到 RK3588 或车规负载。

下表是 Ingress 收到数据之后到 Context 更新完成的耗时。百分位来自固定直方图上界估计，不包含物理传感器、内核 socket 队列、需求观察、模型或用户等待。

| 模式 | 主 FD 负载 | 入队处理样本 | P50 ms | P99 ms | 最大 ms | 最大队列深度 | 感知溢出 | 发生器发送失败 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 仅感知 | normal | 463 | 0.086015 | 0.155647 | 0.212433 | 2 | 0 | 0 |
| 仅感知 | 1k | 3313 | 0.015359 | 0.086015 | 0.289569 | 2 | 0 | 0 |
| 仅感知 | 10k | 30311 | 0.006399 | 0.019455 | 0.194953 | 6 | 0 | 0 |
| 仅感知 | burst | 2142341 | 0.000991 | 0.002175 | 0.385722 | 127 | 0 | 500 |
| 语音问答并发 | normal | 466 | 0.063487 | 0.126975 | 0.352634 | 2 | 0 | 0 |
| 语音问答并发 | 1k | 3316 | 0.013823 | 0.077823 | 0.191593 | 2 | 0 | 0 |
| 语音问答并发 | 10k | 30312 | 0.005887 | 0.065535 | 0.179845 | 7 | 0 | 0 |
| 语音问答并发 | burst | 2162595 | 0.000991 | 0.002687 | 0.350205 | 60 | 0 | 1560 |

并发模式每项都完成一轮真实 ASR → 延迟 1.5 s 的 mock LLM → 真实 TTS。burst 为不节流发生器，发生器在内核入口遇到满队列的发送失败单独计数，不冒充无损吞吐。强制 SPSC 满队列及必须保留事件缺口由 `cockpit_core_test` 单独覆盖。

完整 JSON 含 P95、P99.9、CPU、RSS、来源分组、合并和追踪缺失计数：[仅感知](../artifacts/cockpit-performance/report.json)、[语音问答并发](../artifacts/cockpit-performance-models/report.json)。

## 单次服务分段示例

下面来自自动确认的集成测试，每场景仅 1 个服务样本，不能据此估算服务尾延迟。为加快异常验收，测试观察窗口使用热舒适 100 ms、疲劳 150 ms；默认演示配置仍为 800 ms 和 2000 ms。用户等待来自测试程序立即确认，不代表真人响应时间。

| 场景 | 观察 ms | 仲裁等待 ms | 候选确认到 WAV 就绪 ms | 用户等待 ms | 执行和回读 ms | 结果 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| heat | 100.087 | 0.016 | 318.008 | 2.407 | 20.332 | SUCCEEDED |
| rest | 150.148 | 0.053 | 316.658 | 1.141 | 20.384 | SUCCEEDED |
| comfort | 0.000 | 1.241 | 316.815 | 1.088 | 20.286 | SUCCEEDED |

TTS 的数百毫秒耗时与感知微秒级处理是不同指标。当前主要交互耗时在语音合成及必要的观察/确认阶段；快路径已不经过 LLM，模型执行也不阻塞感知线程。

## 保留的验收项

- RK3588 真机、匹配权重与驱动上的三个场景、输出质量、热/功耗及性能。
- 真实 CAN/SOME-IP/DMS、音频采集和播放延迟；当前只有 FD 模拟/回放与 WAV 文件交互。
- 长时间稳定性、模型加载期间持续高频输入、完整进程的 TSan，以及网络故障注入的更大组合矩阵。现有短时检查不能替代这些验证。
- 自动恢复、自动重建、跨设备发现和可靠投递不属于首版实现；结果未知的车辆执行不自动重试。

## 复现

```bash
./scripts/build_cockpit.sh -DSHERPA_ONNX_BUILD_DIR="$PWD/build/host"
(cd build/cockpit && ctest --output-on-failure)
python3 tests/cockpit_benchmark.py --seconds 3
python3 tests/cockpit_benchmark.py --seconds 3 --models \
    --output-dir artifacts/cockpit-performance-models
python3 sample/cockpit_report.py artifacts/cockpit/traces.jsonl \
    --output artifacts/cockpit/latency-report.json
```

没有可复用 Sherpa 库时省略 `SHERPA_ONNX_BUILD_DIR`。旧构建目录若来自迁移前的绝对路径，应使用新目录；本轮 RK 链接检查使用 `build/cockpit-aarch64`，未覆盖旧缓存。
