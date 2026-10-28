#include "cockpit_task.h"
#include "StackFlow.h"
#include <signal.h>
#include <glog/logging.h>
#include <iostream>

namespace cockpit {
class CockpitNode : public StackFlows::StackFlow {
public:
    CockpitNode() : StackFlow("cockpit", true)
    {
        start_events();
    }
    ~CockpitNode() override
    {
        stop_events();
        if (channel_) channel_->stop_subscriber("");
        if (task_) task_->stop();
    }
    int setup(const std::string& work, const std::string&, const std::string& raw) override
    {
        if (task_) throw std::runtime_error("cockpit already has a task");
        auto channel = get_channel(work);
        channel->set_output(true);
        auto task = std::make_shared<CockpitTask>(channel, Json::parse(raw));
        task->start();
        std::weak_ptr<CockpitTask> weak = task;
        channel->subscribe_message("", [weak](const Json& body, const RequestContext& context) {
            if (auto task = weak.lock()) task->submit(body, context);
        });
        task_    = std::move(task);
        channel_ = channel;
        send("sys.setup", {{"perception_ready", true}}, nullptr, work);
        return 0;
    }
    int exit(const std::string& work, const std::string&, const std::string&) override
    {
        if (!channel_ || channel_->work_id_ != work) throw std::runtime_error("unknown cockpit task");
        channel_->stop_subscriber("");
        task_->stop();
        task_.reset();
        channel_.reset();
        return 0;
    }
    void pause(const std::string& work, const std::string&, const std::string& raw) override
    {
        if (!channel_ || channel_->work_id_ != work) throw std::runtime_error("unknown cockpit task");
        task_->submit({{"object", "cockpit.pause"}, {"data", Json::parse(raw)}}, {request_id_, out_zmq_url_});
    }

private:
    std::shared_ptr<CockpitTask> task_;
    std::shared_ptr<StackFlows::llm_channel_obj> channel_;
};
}  // namespace cockpit
int main(int, char** argv)
{
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = true;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    try {
        cockpit::CockpitNode node;
        std::cout << "edge_cockpit ready" << std::endl;
        int signal;
        sigwait(&signals, &signal);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
