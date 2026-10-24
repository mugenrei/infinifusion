#pragma once

#include "infinifusion/api.h"
#include <functional>
#include <string>

namespace infinifusion::host {

class ClientSession {
public:
    using Initialize = std::function<uint32_t(const IF_Api&, IF_Owner)>;
    using Poll = std::function<bool(const IF_Api&, IF_Owner)>;
    enum class State { Empty, Active, Stopping, Stopped, Faulted };

    explicit ClientSession(IF_Api api) : api_(api) {}
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;
    uint32_t start(const IF_OwnerInfo&, Initialize, Poll = {});
    void poll();
    void stop();
    State state() const noexcept { return state_; }
    IF_Owner owner() const noexcept { return owner_; }
    const IF_Status& removal() const noexcept { return removal_; }
    const std::string& error() const noexcept { return error_; }

private:
    void advance_stop();
    IF_Api api_{};
    IF_Owner owner_{};
    IF_Request disable_{};
    IF_Status removal_{sizeof(IF_Status)};
    State state_{State::Empty};
    Poll poll_;
    std::string error_;
};

}
