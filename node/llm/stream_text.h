#pragma once
#include <functional>
#include <string>
namespace StackFlows {
// 保留原 llm_demo 按标点分段的行为，并跨 SDK 片段过滤思考标签。
class StreamText {
public:
    using Sink = std::function<void(const std::string&)>;
    explicit StreamText(Sink sink) : sink_(std::move(sink))
    {
    }
    void append(const std::string& text)
    {
        for (char c : text) {
            tag_buffer_ += c;
            while (!tag_buffer_.empty()) {
                if (tag_buffer_ == "<think>") {
                    thinking_ = true;
                    tag_buffer_.clear();
                    break;
                }
                if (tag_buffer_ == "</think>") {
                    thinking_ = false;
                    tag_buffer_.clear();
                    break;
                }
                if (std::string("<think>").rfind(tag_buffer_, 0) == 0 ||
                    std::string("</think>").rfind(tag_buffer_, 0) == 0)
                    break;
                if (!thinking_) sentence_ += tag_buffer_.front();
                tag_buffer_.erase(0, 1);
                split();
            }
        }
    }
    void finish()
    {
        // 不把未闭合的思考区或半个控制标签读出来。
        tag_buffer_.clear();
        if (!sentence_.empty()) {
            sink_(sentence_);
            sentence_.clear();
        }
    }

private:
    Sink sink_;
    bool thinking_ = false;
    std::string tag_buffer_, sentence_;
    void split()
    {
        for (const std::string punctuation : {"：", "，", "。", "\n", "；", "！", "？", ".", "!", "?"}) {
            if (sentence_.size() >= punctuation.size() &&
                sentence_.compare(sentence_.size() - punctuation.size(), punctuation.size(), punctuation) == 0) {
                sink_(sentence_);
                sentence_.clear();
                return;
            }
        }
    }
};
}  // namespace StackFlows
