#pragma once
#include <sys/eventfd.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace cockpit {
class WakeFd {
public:
    WakeFd() : fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    {
        if (fd_ < 0) throw std::runtime_error("eventfd failed");
    }
    ~WakeFd()
    {
        close(fd_);
    }
    WakeFd(const WakeFd&) = delete;
    WakeFd& operator=(const WakeFd&) = delete;
    int fd() const
    {
        return fd_;
    }
    void notify()
    {
        if (pending_.exchange(true, std::memory_order_acq_rel)) return;
        uint64_t value = 1;
        ssize_t result;
        do {
            result = write(fd_, &value, sizeof(value));
        } while (result < 0 && errno == EINTR);
    }
    void drain()
    {
        uint64_t value;
        while (read(fd_, &value, sizeof(value)) > 0) {
        }
        pending_.store(false, std::memory_order_release);
        // 调用者必须在处理后复查工作队列；与生产者 notify 配合避免丢失唤醒。
    }

private:
    int fd_;
    std::atomic<bool> pending_{false};
};

// 仅用于低频控制、模型结果和执行命令；感知通路使用独立的 SPSC。
template <typename T, size_t Capacity = 128>
class Mailbox {
public:
    bool push(T value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == Capacity) return false;
        items_[(head_ + size_) % Capacity] = std::move(value);
        ++size_;
        return true;
    }
    bool pop(T& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!size_) return false;
        value = std::move(*items_[head_]);
        items_[head_].reset();
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

private:
    mutable std::mutex mutex_;
    std::array<std::optional<T>, Capacity> items_;
    size_t head_ = 0, size_ = 0;
};
}  // namespace cockpit
