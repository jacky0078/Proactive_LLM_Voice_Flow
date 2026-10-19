/* SPDX-License-Identifier: MIT */
#include "all.h"
#include "remote_server.h"
#include <signal.h>
#include <iostream>
pthread_spinlock_t key_sql_lock;
std::unordered_map<std::string, std::any> key_sql;
std::string zmq_s_format, zmq_c_format;
int main_exit_flage = 0;
void tcp_work();
int main(int argc, char** argv)
{
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    pthread_spin_init(&key_sql_lock, PTHREAD_PROCESS_PRIVATE);
    int result = 0;
    try {
        std::string config = EDGE_DEFAULT_CONFIG;
        if (argc == 3 && std::string(argv[1]) == "--config")
            config = argv[2];
        else if (argc != 1)
            throw std::runtime_error("usage: unit_manager [--config FILE]");
        load_default_config(config);
        zmq_s_format = std::any_cast<std::string>(key_sql.at("config_zmq_s_format"));
        zmq_c_format = std::any_cast<std::string>(key_sql.at("config_zmq_c_format"));
        remote_server_work();
        tcp_work();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    remote_server_stop_work();
    pthread_spin_destroy(&key_sql_lock);
    return result;
}
