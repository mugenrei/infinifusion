#pragma once
#include "infinifusion/api.h"
#include <filesystem>
#include <string>
#include <vector>

namespace infinifusion::json_mods {
struct Value {
    uint32_t kind{};
    int64_t integer{};
    uint64_t unsigned_integer{};
    double number{};
    std::string text;
    IF_Value view() const;
};
struct Edit {
    struct TbcrWrite { std::string property, type; uint32_t offset{}; std::vector<Value> values; };
    bool tbcr{};
    bool tbcr_update{};
    std::string tbcr_id, table_alias;
    uint32_t table_index{}, record_size{}, record_index{};
    std::vector<TbcrWrite> writes;
    std::string provider, object, row, guard, property, provenance;
    Value value, expect;
    bool has_expect{};
    uint32_t flags{};
    IF_Assignment view() const;
};
struct Mod {
    std::string id, name, ordering_key;
    int32_t priority{};
    std::vector<Edit> edits;
};
struct Candidate {
    std::vector<Mod> mods;
    std::vector<std::string> errors;
    std::string fingerprint;
};

Candidate load(const std::filesystem::path &directory, uint32_t flags);
uint32_t publish(const IF_Api &, IF_Owner controller, const Candidate &, uint64_t revision, IF_Request *);
class Client {
  public:
    Client(IF_Api api, std::filesystem::path directory, uint32_t flags);
    ~Client();
    bool start();
    void poll(bool force = false, bool scan = true);
    bool has_active() const noexcept {
        return have_active_;
    }
    bool has_submission() const noexcept {
        return request_ != 0;
    }

  private:
    void log(uint32_t level, const std::string &text);
    IF_Api api_{};
    IF_Owner owner_{};
    IF_Request request_{};
    uint64_t revision_{};
    std::filesystem::path directory_;
    uint32_t flags_{};
    Candidate candidate_;
    std::string pending_fingerprint_, active_fingerprint_, rejected_fingerprint_, last_error_;
    bool awaiting_{}, have_active_{}, have_pending_{}, have_rejected_{}, manual_reload_{};
    std::string last_summary_;
    uint32_t last_admission_error_{IF_OK};
    std::vector<uint64_t> operation_states_;
    uint32_t removal_cursor_{};
};
}
