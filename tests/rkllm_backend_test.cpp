#include "rkllm_backend.h"
#include <cstddef>
#include <cstdint>
#include "rkllm.h"
#include "stream_text.h"
#include "StackFlowUtil.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace StackFlows;
extern RKLLMParam stub_last_param;
extern std::string stub_system_prompt;
extern std::atomic<int> stub_loaded, stub_destroyed, stub_runs, stub_aborted;
void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
template <class Function>
void must_fail(Function function, const std::string& label)
{
    bool failed = false;
    try {
        function();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, "expected failure: " + label);
}
int main()
{
    auto root = std::filesystem::temp_directory_path() / ("edge-rkllm-test-" + std::to_string(getpid()));
    std::filesystem::create_directory(root);
    try {
        for (auto name : {"valid.rkllm", "init-fail.rkllm", "template-fail.rkllm"})
            std::ofstream(root / name) << "SDK fixture";
        nlohmann::json config{{"model_path", (root / "valid.rkllm").string()}, {"system_prompt", "test system prompt"}};
        must_fail(
            [&] {
                RkLlmBackend backend(nlohmann::json{{"model_path", (root / "missing").string()}});
            },
            "missing model");
        must_fail(
            [&] {
                RkLlmBackend backend(nlohmann::json{{"model_path", (root / "init-fail.rkllm").string()}});
            },
            "SDK init");
        must_fail(
            [&] {
                RkLlmBackend backend(nlohmann::json{{"model_path", (root / "template-fail.rkllm").string()}});
            },
            "SDK template");
        {
            RkLlmBackend backend(config);
            check(stub_last_param.top_k == 1 && stub_last_param.top_p == 0.95f && stub_last_param.temperature == 0.8f &&
                      stub_last_param.repeat_penalty == 1.1f && stub_last_param.max_new_tokens == 100 &&
                      stub_last_param.max_context_len == 256 && stub_last_param.extend_param.embed_flash == 1 &&
                      stub_last_param.extend_param.enabled_cpus_mask == 5,
                  "original sampling and CPU defaults preserved");
            check(stub_system_prompt == "test system prompt", "configured system prompt forwarded");
            for (int round = 0; round < 3; ++round) {
                std::string result;
                backend.run("normal", [&](const std::string& part) { result += part; });
                check(result == "你好，我是车载语音助手。", "userdata callback/text ownership/think filtering");
            }
            for (auto prompt : {"error", "return-fail", "missing-finish", "oversized-callback"})
                must_fail([&] { backend.run(prompt, [](const std::string&) {}); }, prompt);
            {
                std::atomic<bool> cancelled{false};
                int previous = stub_runs;
                std::thread request([&] {
                    try {
                        backend.run("abort", [](const std::string&) {});
                    } catch (...) {
                        cancelled = true;
                    }
                });
                while (stub_runs == previous) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                backend.cancel();
                request.join();
                check(cancelled, "request cancellation stops current inference");
                std::string next;
                backend.run("normal", [&](const std::string& part) { next += part; });
                check(!next.empty(), "request cancellation preserves model for next inference");
            }
            std::atomic<bool> failed{false};
            int previous = stub_runs;
            std::thread inference([&] {
                try {
                    backend.run("abort", [](const std::string&) {});
                } catch (...) {
                    failed = true;
                }
            });
            while (stub_runs == previous) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            backend.abort();
            inference.join();
            check(failed && stub_aborted > 0, "abort active inference");
            must_fail([&] { backend.run("normal", [](const std::string&) {}); }, "run after stop");
        }
        {
            auto overrides               = config;
            overrides["top_k"]           = 5;
            overrides["temperature"]     = 0.4;
            overrides["max_new_tokens"]  = 32;
            overrides["max_context_len"] = 512;
            RkLlmBackend backend(overrides);
            check(stub_last_param.top_k == 5 && stub_last_param.temperature == 0.4f &&
                      stub_last_param.max_new_tokens == 32 && stub_last_param.max_context_len == 512,
                  "configured generation parameters forwarded");
        }
        check(stub_loaded == stub_destroyed, "all model handles destroyed exactly once");
        for (const std::string text : {"<think>秘密</think>回答。", "<think>秘密</think><think>秘密</think>回答。"}) {
            for (size_t split = 0; split <= text.size(); ++split) {
                std::string result;
                StreamText filter([&](const std::string& part) { result += part; });
                filter.append(text.substr(0, split));
                filter.append(text.substr(split));
                filter.finish();
                check(result == "回答。", "tag or UTF-8 split boundary");
            }
        }
        std::unordered_map<int, std::string> fragments;
        std::string result;
        decode_stream(R"({"index":0,"delta":"a","finish":false})", result, fragments);
        must_fail([&] { decode_stream(R"({"index":2,"delta":"c","finish":true})", result, fragments); },
                  "missing stream chunk");
        check(fragments.empty(), "bad stream clears state");
        check(sample_json_str_get(R"({"data":"a\"b\\c\n你好"})", "data") == "a\"b\\c\n你好", "JSON escapes");
        pzmq_data data;
        auto encoded = pzmq_data::set_param(std::string(200, 'a'), "payload");
        check(data.get_param(0, encoded).size() == 200 && data.get_param(1, encoded) == "payload",
              "unsigned RPC length");
        must_fail([&] { data.get_param(1, std::string(1, char(255))); }, "truncated RPC frame");
        std::filesystem::remove_all(root);
        std::cout << "RKLLM adapter: load, callback ownership, errors, filtering, abort and destroy passed\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
