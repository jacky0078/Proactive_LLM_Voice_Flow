#pragma once

#include "pzmq.hpp"
#include <vector>

using namespace StackFlows;

class unit_data {
private:
    std::unique_ptr<pzmq> user_inference_chennal_;

public:
    std::string work_id;
    std::string output_url;
    std::string inference_url;
    int port_;
    int output_slot = -1;
    int input_slot  = -1;

    unit_data();
    void init_zmq(const std::string &url);
    int send_msg(const std::string &json_str);
    ~unit_data();
};
