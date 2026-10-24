#include "host/client_session.hpp"
#include <exception>

namespace infinifusion::host {

ClientSession::~ClientSession() {

    if (disable_) api_.release(owner_, disable_);
}

uint32_t ClientSession::start(const IF_OwnerInfo& info, Initialize initialize, Poll poll) {
    if (state_ != State::Empty || !initialize || api_.size != sizeof(IF_Api) ||
        api_.version != IF_API_VERSION || !api_.register_owner || !api_.disable_owner ||
        !api_.status || !api_.release) return IF_INVALID_ARGUMENT;
    auto result = api_.register_owner(&info, &owner_);
    if (result != IF_OK) { error_ = "owner registration failed: " + std::to_string(result); return result; }
    state_ = State::Active;
    poll_ = std::move(poll);
    try { result = initialize(api_, owner_); }
    catch (const std::exception& e) { error_ = e.what(); result = IF_INTERNAL; }
    catch (...) { error_ = "client initialization exception"; result = IF_INTERNAL; }
    if (result != IF_OK) {
        if (error_.empty()) error_ = "client initialization failed: " + std::to_string(result);
        stop();
    }
    return result;
}

void ClientSession::poll() {
    if (state_ == State::Stopping) { advance_stop(); return; }
    if (state_ != State::Active || !poll_) return;
    bool success{};
    try { success = poll_(api_, owner_); }
    catch (const std::exception& e) { error_ = e.what(); }
    catch (...) { error_ = "client poll exception"; }
    if (!success) {
        if (error_.empty()) error_ = "client poll failed";
        stop();
    }
}

void ClientSession::stop() {
    if (state_ == State::Active) { state_ = State::Stopping; poll_ = {}; }
    if (state_ == State::Stopping) advance_stop();
}

void ClientSession::advance_stop() {
    if (!disable_) {
        const auto result = api_.disable_owner(owner_, &disable_);
        if (result == IF_QUEUE_FULL || result == IF_LIMIT_EXCEEDED || result == IF_NOT_READY)
            return;
        if (result != IF_OK) {
            removal_.error = result;
            removal_.state = IF_FAULTED;
            state_ = State::Faulted;
            return;
        }
    }
    const auto result = api_.status(owner_, disable_, IF_STATUS_SUMMARY, &removal_);
    if (result != IF_OK || removal_.state == IF_FAULTED) {
        if (result != IF_OK) removal_.error = result;
        state_ = State::Faulted;
    } else if (removal_.observed && removal_.settled) {

        state_ = removal_.error == IF_OK ? State::Stopped : State::Faulted;
    } else {
        return;
    }
    api_.release(owner_, disable_);
    disable_ = 0;
}

}
