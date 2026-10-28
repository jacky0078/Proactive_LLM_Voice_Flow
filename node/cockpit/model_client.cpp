#include "model_client.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>

namespace cockpit {
ModelClient::ModelClient(std::string host, uint16_t port, std::function<bool(Json)> receive)
    : host_(std::move(host)), port_(port), receive_(std::move(receive))
{
}
ModelClient::~ModelClient()
{
    stop();
}
void ModelClient::start()
{
    thread_ = std::thread([this] { run(); });
}
void ModelClient::stop()
{
    stopping_ = true;
    wake_.notify();
    if (thread_.joinable()) thread_.join();
}
bool ModelClient::send(Json request)
{
    if (!connected_ || !outgoing_.push(std::move(request))) return false;
    wake_.notify();
    return true;
}
void ModelClient::run()
{
    int fd              = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    std::string failure = "manager connection failed";
    try {
        if (fd < 0) throw std::runtime_error(failure);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port   = htons(port_);
        if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1)
            throw std::runtime_error("manager host must be IPv4");
        int rc = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (rc && errno != EINPROGRESS) throw std::runtime_error(failure);
        pollfd opening[]{{wake_.fd(), POLLIN, 0}, {fd, POLLOUT, 0}};
        if (rc && (poll(opening, 2, 2000) <= 0 || !(opening[1].revents & POLLOUT))) throw std::runtime_error(failure);
        int error        = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) throw std::runtime_error(failure);
        connected_ = true;
        if (!receive_({{"object", "cockpit.link"}, {"data", {{"connected", true}}}}))
            throw std::runtime_error("result mailbox full");
        std::string input, output;
        size_t sent = 0;
        while (!stopping_) {
            if (output.empty()) {
                Json request;
                if (outgoing_.pop(request)) {
                    output = request.dump() + "\n";
                    sent   = 0;
                }
            }
            pollfd events[]{{wake_.fd(), POLLIN, 0},
                            {fd, static_cast<short>(POLLIN | (output.empty() ? 0 : POLLOUT)), 0}};
            int timeout = input.find('\n') == std::string::npos ? -1 : 0;
            int count   = poll(events, 2, timeout);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::runtime_error("manager poll failed");
            if (events[0].revents & POLLIN) wake_.drain();
            if (events[1].revents & (POLLHUP | POLLERR | POLLNVAL)) throw std::runtime_error("manager disconnected");
            if (events[1].revents & POLLIN) {
                char buffer[65536];
                auto bytes = recv(fd, buffer, sizeof(buffer), 0);
                if (!bytes) throw std::runtime_error("manager disconnected");
                if (bytes < 0 && errno != EINTR && errno != EAGAIN) throw std::runtime_error("manager read failed");
                if (bytes > 0) input.append(buffer, bytes);
                if (input.size() > 2 * 1024 * 1024) throw std::runtime_error("manager input buffer limit");
            }
            for (int batch = 0; batch < 32; ++batch) {
                auto end = input.find('\n');
                if (end == std::string::npos) break;
                if (end > 1024 * 1024) throw std::runtime_error("manager frame too large");
                auto message = Json::parse(input.begin(), input.begin() + end);
                input.erase(0, end + 1);
                if (!receive_(std::move(message))) throw std::runtime_error("model result mailbox full");
            }
            if (!output.empty() && (events[1].revents & POLLOUT)) {
                auto bytes = ::send(fd, output.data() + sent, output.size() - sent, MSG_NOSIGNAL);
                if (bytes < 0 && errno != EINTR && errno != EAGAIN) throw std::runtime_error("manager write failed");
                if (bytes > 0) sent += bytes;
                if (sent == output.size()) output.clear();
            }
        }
    } catch (const std::exception& error) {
        failure = error.what();
    }
    connected_ = false;
    if (fd >= 0) close(fd);
    if (!stopping_) receive_({{"object", "cockpit.link"}, {"data", {{"connected", false}, {"reason", failure}}}});
}
}  // namespace cockpit
