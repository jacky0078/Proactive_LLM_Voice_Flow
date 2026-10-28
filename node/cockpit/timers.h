#pragma once
#include "types.h"
#include <algorithm>
#include <stdexcept>

namespace cockpit {
// 有界、可替换的截止时间堆。每个逻辑定时器只有一个槽，不积累失效节点。
class Timers {
public:
    void set(uint32_t id, Time deadline)
    {
        erase(id);
        if (deadline == never) return;
        if (size_ == entries_.size()) throw std::runtime_error("timer capacity exceeded");
        entries_[size_++] = {id, deadline};
        std::push_heap(entries_.begin(), entries_.begin() + size_, later);
    }
    void erase(uint32_t id)
    {
        for (size_t i = 0; i < size_; ++i) {
            if (entries_[i].id != id) continue;
            entries_[i] = entries_[--size_];
            std::make_heap(entries_.begin(), entries_.begin() + size_, later);
            return;
        }
    }
    Time next() const
    {
        return size_ ? entries_[0].deadline : never;
    }
    bool pop(Time now, uint32_t& id)
    {
        if (next() > now) return false;
        id = entries_[0].id;
        std::pop_heap(entries_.begin(), entries_.begin() + size_, later);
        --size_;
        return true;
    }
    size_t size() const
    {
        return size_;
    }

private:
    struct Entry {
        uint32_t id;
        Time deadline;
    };
    static bool later(const Entry& a, const Entry& b)
    {
        return a.deadline > b.deadline;
    }
    std::array<Entry, 128> entries_{};
    size_t size_ = 0;
};
}  // namespace cockpit
