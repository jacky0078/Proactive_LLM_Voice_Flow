#include "voice_task.h"
#include <signal.h>
#include <iostream>
int main()
{
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    try {
        StackFlows::VoiceNode node(EDGE_NODE_NAME, StackFlows::make_voice_task);
        std::cout << "edge_" << EDGE_NODE_NAME << " ready" << std::endl;
        int signal;
        sigwait(&signals, &signal);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
