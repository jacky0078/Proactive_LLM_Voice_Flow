#include "network/EventLoopThread.h"

#include "network/EventLoop.h"

namespace network
{
  EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,
                                   const std::string &name)
      : loop_(NULL), exiting_(false), mutex_(), callback_(cb) {}

  EventLoopThread::~EventLoopThread()
  {
    exiting_ = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (loop_) loop_->quit();
    }
    if (thread_ && thread_->joinable()) thread_->join();
  }

  EventLoop *EventLoopThread::startLoop()
  {
    // assert(!thread_.started());
    // thread_.start();
    thread_ = std::make_unique<std::thread>(
        std::bind(&EventLoopThread::threadFunc, this));
    EventLoop *loop = NULL;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      while (loop_ == NULL)
      {
        cv_.wait(lock);
      }
      loop = loop_;
    }

    return loop;
  }

  void EventLoopThread::threadFunc()
  {
    pthread_setname_np(pthread_self(), "EventLoopThread");
    EventLoop loop;

    if (callback_)
    {
      callback_(&loop);
    }

    {
      std::unique_lock<std::mutex> lock(mutex_);
      loop_ = &loop;
      cv_.notify_all();
    }

    loop.loop();
    // assert(exiting_);
    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = NULL;
  }
} // namespace network