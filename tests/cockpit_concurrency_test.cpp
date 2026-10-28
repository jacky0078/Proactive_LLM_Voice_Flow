#include "mailbox.h"
#include "types.h"
#include <poll.h>
#include <bitset>
#include <iostream>
#include <thread>
#include <vector>

using namespace cockpit;
int main()
{
    // 多生产者只能进入控制信箱；合并唤醒不能使已经入队的消息永久等待。
    constexpr size_t producers  = 4;
    constexpr size_t per_thread = 10000;
    WakeFd wake;
    Mailbox<uint32_t> mailbox;
    std::atomic<bool> stopping{false};
    std::vector<std::thread> threads;
    for (size_t source = 0; source < producers; ++source) {
        threads.emplace_back([&, source] {
            for (size_t i = 0; i < per_thread && !stopping; ++i) {
                uint32_t id = source * per_thread + i;
                while (!mailbox.push(id) && !stopping) std::this_thread::yield();
                wake.notify();
            }
        });
    }
    std::bitset<producers * per_thread> seen;
    bool valid    = true;
    auto deadline = now_ns() + 10 * second;
    while (seen.count() < seen.size() && now_ns() < deadline && valid) {
        uint32_t id;
        while (mailbox.pop(id)) {
            if (id >= seen.size() || seen.test(id)) {
                valid = false;
                break;
            }
            seen.set(id);
        }
        if (seen.count() == seen.size()) break;
        pollfd event{wake.fd(), POLLIN, 0};
        if (poll(&event, 1, 100) > 0) wake.drain();
        // drain 清除标记后总会回到队列检查，覆盖生产者与清标记并发。
    }
    stopping = true;
    for (auto& thread : threads) thread.join();
    if (!valid || seen.count() != seen.size()) {
        std::cerr << "concurrent mailbox lost, duplicated or stranded messages\n";
        return 1;
    }
    std::cout << "40000 concurrent messages: bounded mailbox and coalesced wakeup passed\n";
}
