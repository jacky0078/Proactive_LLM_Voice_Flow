# 语音节点接入验证记录

验证日期：2026-10-16。环境：Ubuntu 20.04 x86_64、GCC 9、CMake 3.16、Python 3.13；没有音频设备和 RK 板。代码使用仓库既有 `.clang-format` 排版。

## 结果

| 验收项 | 结果与证据 |
| --- | --- |
| 本机音频闭环 | 真实 Sherpa-ONNX ASR → mock LLM → 真实 SummerTTS，使用原模型附带的 0/1/2.wav |
| 连续与重复生命周期 | 两次完整 setup → 三轮 inference → exit；另完成一次异常后的恢复轮，共保存 7 个独立输出 WAV |
| 请求与结束事件 | 校验每轮 request_id、输出节点 work_id、连续流索引、唯一 finish；生命周期 taskinfo 插入推理期间不改变推理请求号 |
| 无效模型 | ASR 模型缺失、TTS 模型摘要不符、未知 backend、x86 未编译 RK runtime 均返回结构化错误 |
| 音频异常 | 无效文件、缺失文件、静音和立体声输入均报错；随后正常音频仍完成闭环 |
| 流与写盘 | 缺片、缺 finish 超时、写入目录失效、重复完成事件均覆盖；`/dev/full` 模拟中途写失败，检查临时文件清理 |
| 超时与退出 | 客户端超时后取消任务；进程正常退出，四个自启动进程返回码均为 0 |
| TCP 与订阅 | 半包、粘包、中文 UTF-8 跨包、JSON 转义、无效 JSON、RPC 不可达、每跳首次订阅探测 |
| RKLLM 实际适配代码 | 同一 `rkllm_backend.cpp` 链接 SDK 替身，覆盖加载失败、模板、默认与可配置参数、userdata、临时文本复制、跨片段思考过滤、错误回调/返回值、缺 finish、abort/destroy；同时通过 StackFlow 节点完整测试 |
| AArch64 编译与真实链接 | GNU AArch64 工具链编译 edge_llm、stackflow、ZeroMQ；链接原项目真实 `librkllmrt.so`；ELF 为 AArch64，安装包 RUNPATH 为 `$ORIGIN/../lib` |
| 默认统一构建 | 不使用预构建 Sherpa 时，根 CMake 从原源码构建并成功链接 edge_asr；未链接旧 zmq-comm-kit |
| RK 板实测 | **未执行**，需实际 RK 设备、适配量化模型和驱动 |

最终 CTest：`rkllm_backend`、`voice_integration`、`wav_io`，**3/3 通过**。最后一轮合计约 14 秒，只用于记录本机验证，不作为性能或时延承诺。

## 可查看的产物

- [完整验收报告与逐轮识别/回答/请求编号](../artifacts/validation/report.json)
- [CTest 原始结果](../artifacts/validation/ctest.txt)
- [可试听输出 WAV](../artifacts/validation/reply.wav)
- [消息及节点日志](../artifacts/validation/logs/)
- [AArch64 ELF 头](../artifacts/validation/aarch64-elf-header.txt)
- [AArch64 动态依赖和 RUNPATH](../artifacts/validation/aarch64-elf-dynamic.txt)
- [编译器与二进制/runtime SHA-256](../artifacts/validation/build.json)

上述证据文件保存在当前工作区的 `artifacts/validation/`，不纳入源码版本控制。音频为模型真实识别、真实合成的结果；中间回答明确使用 mock，识别质量未做人工转写基准评估。

## 独立依赖验证

2026-10-16 将必要依赖抽取到 `voice-deps/` 后，临时移走原 `LLM_Voice_Flow-master` 目录，在其原路径完全不可用的情况下重新编译并验收。验证结束后已将原目录放回原位。

- 抽取 2003 个原文件，共约 164.3 MiB；逐个核对来源和副本 SHA-256。额外生成 Sherpa 子目录入口、SummerTTS 源码清单及说明。
- 从抽取的 Sherpa/SummerTTS 源码构建 ASR/TTS；只复用第三方下载缓存，未使用旧 Sherpa 预构建库。
- 修复第三方 Eigen 配置覆盖 `BUILD_TESTING` 的问题，保留 Edge 的测试开关。
- `rkllm_backend`、`voice_integration`、`wav_io` 共 **3/3 通过**；真实 ASR/TTS 完成 7 轮音频闭环及异常路径。
- 原 RKLLM 适配代码保持不变；使用抽取的原始 SDK 再次完成真实 AArch64 库链接，安装包仍为 `$ORIGIN/../lib` RUNPATH。
- 活动 host/cross 构建缓存和编译命令中，原目录路径引用为 0；ASR/TTS 动态依赖没有缺失。
- 本地原项目没有 `.rkllm` 量化权重，未执行真实 RK 板端推理。

证据：[依赖校验与隔离验证报告](../artifacts/dependency-isolation/report.json)、[CTest 结果](../artifacts/dependency-isolation/ctest.txt)、[7 轮语音结果](../artifacts/dependency-isolation/voice-report.json)、[可试听 WAV](../artifacts/dependency-isolation/reply.wav)、[AArch64 动态链接](../artifacts/dependency-isolation/aarch64-elf-dynamic.txt)。这些产物不提交 Git。

## 复现

```bash
./scripts/build_voice.sh
python3 tests/voice_integration.py --output-dir artifacts/integration
./scripts/build_rkllm_aarch64.sh
```

也可逐项运行：

```bash
(cd build/host && ctest --output-on-failure)
python3 sample/voice_demo.py --rounds 3 --cycles 2
```

板端测试必须使用 `sample/rkllm_board_test.py` 或真实后端场景配置，不能使用 `edge_llm_sdk_test`。测试替身和交叉编译证明调用路径、生命周期、架构及链接成立，不证明 NPU 执行、模型兼容性或板端输出质量。
