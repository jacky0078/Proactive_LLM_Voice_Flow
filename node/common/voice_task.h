#pragma once
#include "StackFlow.h"
#include <atomic>
#include <chrono>
#include <deque>
namespace StackFlows {
using Json = nlohmann::json;
struct VoiceMessage {
    Json body;
    RequestContext context;
};
// 每个任务一个业务线程；模型回调只入队，结果由输出线程交给信道。
class VoiceTask {
public:
    VoiceTask(std::shared_ptr<llm_channel_obj> channel, const Json& config);
    virtual ~VoiceTask();
    void start();
    void submit(const Json& body, const RequestContext& context);
    void stop();
    void set_paused(bool paused)
    {
        paused_ = paused;
    }

protected:
    virtual void process(const VoiceMessage& message) = 0;
    virtual void abort()
    {
    }
    virtual void on_idle()
    {
    }
    virtual void reset_request()
    {
    }
    virtual bool accepts_fragments() const
    {
        return false;
    }
    void emit(const RequestContext&, const std::string&, const Json&, bool terminal = false);
    void fail(const RequestContext&, const std::string&);
    bool stopping() const
    {
        return stopping_;
    }
    bool wait_or_stop(int milliseconds);
    Json config_;

private:
    struct Output {
        RequestContext context;
        std::string object;
        Json data;
        Json error;
    };
    std::shared_ptr<llm_channel_obj> channel_;
    std::mutex input_mutex_, output_mutex_;
    std::condition_variable input_cv_, output_cv_;
    std::deque<VoiceMessage> input_;
    std::deque<Output> output_;
    std::deque<std::string> completed_;
    std::string active_request_;
    std::thread worker_, publisher_;
    std::atomic<bool> stopping_{false}, paused_{false};
    bool output_stopped_ = false;
    void finish_request(const std::string& request);
    void run();
    void publish();
};
using VoiceTaskFactory = std::function<std::shared_ptr<VoiceTask>(std::shared_ptr<llm_channel_obj>, const Json&)>;
class VoiceNode : public StackFlow {
public:
    VoiceNode(const std::string& name, VoiceTaskFactory factory);
    ~VoiceNode() override;
    int setup(const std::string&, const std::string&, const std::string&) override;
    int exit(const std::string&, const std::string&, const std::string&) override;
    void pause(const std::string&, const std::string&, const std::string&) override;

private:
    VoiceTaskFactory factory_;
    std::shared_ptr<VoiceTask> task_;
    std::shared_ptr<llm_channel_obj> channel_;
};
std::shared_ptr<VoiceTask> make_voice_task(std::shared_ptr<llm_channel_obj>, const Json&);
}  // namespace StackFlows
