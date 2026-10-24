#include "adapters/json/json_mods.hpp"
#include "adapters/json/property_values.hpp"
#include "formats/json.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace infinifusion::json_mods {
namespace {
namespace j = smtvv::json;
constexpr std::size_t max_bytes = 4 * 1024 * 1024;
IF_String span(const std::string &s) {
    return {s.data(), static_cast<uint32_t>(s.size())};
}
std::string lower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return s;
}
std::string path_text(const std::filesystem::path &p) {
    const auto s = p.generic_u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}
std::string required(const j::Value &v, const char *key) {
    const auto *member = v.find(key);
    const auto *s = member ? member->string() : nullptr;
    if (!s || s->empty() || s->size() > 4096 || s->find('\0') != std::string::npos)
        throw std::runtime_error(std::string("required nonempty string: ") + key);
    return *s;
}
std::string optional(const j::Value &v, const char *key) {
    const auto *member = v.find(key);
    if (!member)
        return {};
    const auto *s = member->string();
    if (!s || s->size() > 4096 || s->find('\0') != std::string::npos)
        throw std::runtime_error(std::string("invalid string: ") + key);
    return *s;
}
int64_t integer(const j::Value &v) {
    const auto *n = v.number();
    if (!n)
        throw std::runtime_error("expected integer token");
    int64_t result{};
    const auto r = std::from_chars(n->text.data(), n->text.data() + n->text.size(), result);
    if (r.ec != std::errc{} || r.ptr != n->text.data() + n->text.size())
        throw std::runtime_error("invalid/out-of-range integer token");
    return result;
}
void schema(const j::Value &v) {
    const auto *n = v.find("schema_version");
    if (!v.object() || !n || integer(*n) != 1)
        throw std::runtime_error("schema_version must be integer 1");
}
Value value(const j::Value &v) {
    Value out;
    if (auto b = v.boolean()) {
        out.kind = IF_BOOL;
        out.unsigned_integer = *b;
    } else if (auto s = v.string()) {
        out.kind = IF_ENUM;
        out.text = *s;
    } else if (auto n = v.number()) {
        const auto begin = n->text.data(), end = begin + n->text.size();
        if (n->text.find_first_of(".eE") != std::string::npos) {
            out.kind = IF_FLOAT64;
            auto r = std::from_chars(begin, end, out.number);
            if (r.ec != std::errc{} || r.ptr != end || !std::isfinite(out.number))
                throw std::runtime_error("non-finite/out-of-range number");
        } else if (n->text.starts_with('-')) {
            out.kind = IF_INT64;
            out.integer = integer(v);
        } else {
            out.kind = IF_UINT64;
            auto r = std::from_chars(begin, end, out.unsigned_integer);
            if (r.ec != std::errc{} || r.ptr != end)
                throw std::runtime_error("integer exceeds uint64");
        }
    } else if (v.object() || v.array()) {
        out.kind = IF_BYTES;
        out.text = property_text(v);
    } else
        throw std::runtime_error("object value must be a scalar or typed reflected value");
    if (out.text.size() > 4096 || out.text.find('\0') != std::string::npos)
        throw std::runtime_error("text too large or contains NUL");
    return out;
}
bool within(const std::filesystem::path &file, const std::filesystem::path &root) {
    const auto relative =
        std::filesystem::canonical(file).lexically_relative(std::filesystem::canonical(root));
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
j::Value read(const std::filesystem::path &p, Candidate &result, std::size_t &bytes) {
    const auto size = std::filesystem::file_size(p);
    if (size > 1024 * 1024 || bytes > max_bytes - size)
        throw std::runtime_error("JSON input exceeds 1MiB/file or 4MiB/catalog");
    std::ifstream stream(p, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot open JSON file");
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), text.size());
    if (!stream || stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("file changed while reading");
    bytes += text.size();
    const auto path = path_text(p);
    result.fingerprint += std::to_string(path.size()) + ":" + path + std::to_string(text.size()) + ":" + text;
    auto parsed = j::parse(text);
    if (!parsed.value)
        throw std::runtime_error(parsed.error ? parsed.error->message + " at line " +
                                                    std::to_string(parsed.error->line)
                                              : "invalid JSON");
    return std::move(*parsed.value);
}
}
IF_Value Value::view() const {
    IF_Value v{};
    v.size = sizeof(v);
    v.kind = kind;
    v.signed_value = integer;
    v.unsigned_value = unsigned_integer;
    v.float_value = number;
    if (kind == IF_BYTES) v.bytes = {reinterpret_cast<const uint8_t*>(text.data()), static_cast<uint32_t>(text.size())};
    else v.text = span(text);
    return v;
}
IF_Assignment Edit::view() const {
    IF_Assignment a{};
    a.size = sizeof(a);
    a.flags = flags;
    a.target = {span(provider), span(object), span(row), span(guard)};
    a.property = span(property);
    a.value = value.view();
    a.has_expect = has_expect;
    a.expect = expect.view();
    return a;
}
Candidate load(const std::filesystem::path &directory, uint32_t flags) {
    Candidate result;
    std::size_t bytes{}, edits{};
    std::string source = path_text(directory);
    try {
        std::filesystem::create_directories(directory);
        std::vector<std::filesystem::path> folders;
        std::size_t entries{};
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            if (++entries > 1024)
                throw std::runtime_error("directory entry limit1024");
            if (entry.is_directory() && std::filesystem::is_regular_file(entry.path() / "mod.json"))
                folders.push_back(entry.path());
        }
        std::sort(folders.begin(), folders.end());
        std::set<std::string> ids;
        for (const auto &folder : folders) {
            source = path_text(folder / "mod.json");
            if (!within(folder, directory) || !within(folder / "mod.json", folder))
                throw std::runtime_error("manifest/mod escapes configured root");
            const auto manifest = read(folder / "mod.json", result, bytes);
            schema(manifest);
            if (const auto *enabled = manifest.find("enabled")) {
                if (!enabled->boolean())
                    throw std::runtime_error("enabled must be boolean");
                if (!*enabled->boolean())
                    continue;
            }
            Mod mod;
            mod.id = lower(required(manifest, "id"));
            mod.name = optional(manifest, "name");
            if (mod.name.empty())
                mod.name = mod.id;
            mod.ordering_key = lower(path_text(folder.filename()));
            if (mod.id.size() > 256 || mod.ordering_key.size() > 256 || mod.name.size() > 1024)
                throw std::runtime_error("mod metadata too long");
            if (!ids.insert(mod.id).second)
                throw std::runtime_error("duplicate enabled mod id: " + mod.id);
            if (const auto *priority = manifest.find("priority")) {
                auto n = integer(*priority);
                if (n < INT32_MIN || n > INT32_MAX)
                    throw std::runtime_error("priority exceeds int32");
                mod.priority = static_cast<int32_t>(n);
            }
            const auto *patches = manifest.find("patches");
            if (!patches || !patches->array())
                throw std::runtime_error("patches must be array");
            if (patches->array()->size() > 128)
                throw std::runtime_error("patch file count exceeds128");
            for (const auto &file : *patches->array()) {
                if (!file.string() || file.string()->empty() ||
                    file.string()->find('\0') != std::string::npos)
                    throw std::runtime_error("patch path must be nonempty string");
                const auto &utf8 = *file.string();
                const auto relative = std::filesystem::path(
                    std::u8string_view(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
                const auto path = folder / relative;
                if (relative.has_root_path() || !std::filesystem::is_regular_file(path) ||
                    !within(path, folder))
                    throw std::runtime_error("patch missing or escapes mod directory");
                source = path_text(path);
                const auto patch = read(path, result, bytes);
                schema(patch);
                const auto type = required(patch, "asset_type");
                if (type == "tbcr") {
                    for (const auto& [key, ignored] : *patch.object())
                        if (key != "schema_version" && key != "asset_type" && key != "asset" &&
                            key != "restore_policy" && key != "edits")
                            throw std::runtime_error("unknown/unsupported tbcr patch field: " + key);
                    const auto asset = required(patch, "asset");
                    const auto restore = optional(patch, "restore_policy");
                    if (!restore.empty() && restore != "required")
                        throw std::runtime_error("tbcr restore_policy must be required");
                    const auto* list = patch.find("edits");
                    if (!list || !list->array()) throw std::runtime_error("tbcr edits must be array");
                    std::size_t edit_index{};
                    for (const auto& item : *list->array()) {
                        if (++edits > 128) throw std::runtime_error("catalog assignment limit128");
                        const auto op = required(item, "op");
                        if (op != "append" && op != "update")
                            throw std::runtime_error("tbcr op must be append or update");
                        Edit edit; edit.tbcr = true; edit.object = asset;
                        edit.tbcr_update = op == "update";
                        edit.tbcr_id = required(item, "id");
                        if (edit.tbcr_id.size() > 128) throw std::runtime_error("tbcr edit id exceeds128 bytes");
                        edit.provenance = source + ": edit[" + std::to_string(edit_index++) + "] " + op;
                        if (!edit.tbcr_id.empty()) edit.provenance += " id=" + edit.tbcr_id;
                        edit.flags = IF_PERSISTENT_OVERLAY;
                        for (const auto& previous : mod.edits)
                            if (previous.tbcr && !edit.tbcr_id.empty() && previous.tbcr_id == edit.tbcr_id)
                                throw std::runtime_error("duplicate tbcr edit id: " + edit.tbcr_id);
                        for (const auto& [key, ignored] : *item.object())
                            if (key != "id" && key != "op" && key != "table" && key != "record_size" && key != "writes" &&
                                !(key == "entry" && edit.tbcr_update))
                                throw std::runtime_error("unknown/unsupported tbcr edit field: " + key);
                        const auto bounded = [&](const j::Value& v) {
                            const auto n = integer(v);
                            if (n < 0 || n > UINT32_MAX) throw std::runtime_error("tbcr index/size out of range");
                            return static_cast<uint32_t>(n);
                        };
                        const auto* table_id = item.find("table");
                        if (!table_id) throw std::runtime_error("tbcr table required");
                        if (table_id->string()) edit.table_alias = required(item, "table");
                        else edit.table_index = bounded(*table_id);
                        if (const auto* size = item.find("record_size")) edit.record_size = bounded(*size);
                        if (edit.tbcr_update) {
                            const auto* entry = item.find("entry");
                            if (!entry || !entry->object() || entry->object()->size() != 1 || !entry->find("index"))
                                throw std::runtime_error(edit.provenance + ": tbcr update entry must contain only index");
                            edit.record_index = bounded(*entry->find("index"));
                            edit.provenance += " entry[" + std::to_string(edit.record_index) + "]";
                        }
                        const auto* writes = item.find("writes");
                        if (!writes || !writes->array() || writes->array()->empty() || writes->array()->size() > 128)
                            throw std::runtime_error("tbcr requires 1..128 writes");
                        if (edit.tbcr_update) {
                            if (writes->array()->size() - 1 > 128 - edits)
                                throw std::runtime_error("catalog expanded assignment limit128");
                            edits += writes->array()->size() - 1;
                        }
                        for (const auto& input : *writes->array()) {
                            if (!input.object()) throw std::runtime_error("tbcr write must be object");
                            for (const auto& [key, ignored] : *input.object())
                                if (key != "property" && key != "type" && key != "offset" && key != "value")
                                    throw std::runtime_error("unknown/unsupported tbcr write field: " + key);
                            Edit::TbcrWrite write;
                            if (input.find("property")) {
                                write.property = required(input, "property");
                                if (input.find("offset") || input.find("type"))
                                    throw std::runtime_error("named write cannot specify offset/type");
                            } else {
                                write.type = required(input, "type");
                                const auto* offset = input.find("offset");
                                if (!offset || !edit.record_size) throw std::runtime_error("raw write needs offset and record_size");
                                write.offset = bounded(*offset);
                            }
                            const auto* values = input.find("value");
                            if (!values) throw std::runtime_error("tbcr write value required");
                            if (values->array()) {
                                if (values->array()->empty() || values->array()->size() > 4096)
                                    throw std::runtime_error("tbcr value array size invalid");
                                for (const auto& v : *values->array()) write.values.push_back(value(v));
                            } else write.values.push_back(value(*values));
                            edit.writes.push_back(std::move(write));
                        }
                        mod.edits.push_back(std::move(edit));
                    }
                    continue;
                }
                const bool message = type == "script_message";
                const bool table = type == "data_table";
                if (!message && !table && type != "object" && type != "blueprint_properties")
                    throw std::runtime_error("unsupported asset_type: " + type);
                const auto object = required(patch, table ? "table" : message ? "asset" : "object");
                const auto guard = optional(patch, table ? "row_struct" : "class");
                if ((table && !patch.find("rows")) || (message && !patch.find("definitions")))
                    throw std::runtime_error("asset definitions require rows (data_table) or definitions (script_message); edits is not supported");
                if ((table && patch.find("rows")) || (message && patch.find("definitions"))) {
                    const auto policy=optional(patch,"restore_policy");
                    if(!policy.empty() && policy!="required")
                        throw std::runtime_error("source definitions restore logical ownership; restore_policy must be required or omitted");
                    if (patch.find("edits")) throw std::runtime_error("data_table uses rows or edits, not both");
                    const auto* rows = patch.find(table ? "rows" : "definitions")->array();
                    if (!rows) throw std::runtime_error("data_table rows must be array");
                    for (const auto& row : *rows) {
                        const auto row_name = required(row,table ? "row" : "label");
                        auto op = optional(row,"op"); if (op.empty()) op = "update";
                        if (op == "clone") op += ':' + required(row,"from");
                        else if (row.find("from")) throw std::runtime_error("from requires clone operation");
                        if (op != "update" && op != "upsert" && op != "remove" && !op.starts_with("clone:"))
                            throw std::runtime_error("table row op must be update,upsert,clone or remove");
                        auto add = [&](std::string property, std::string text) {
                            if (++edits > 128) throw std::runtime_error("catalog assignment limit128");
                            Edit edit; edit.provider=table ? "unreal.table_definition" : "unreal.message_definition"; edit.object=object;
                            edit.guard=guard; edit.row=row_name; edit.property=std::move(property);
                            edit.value.kind=IF_STRING; edit.value.text=std::move(text);
                            edit.provenance=source+": row["+row_name+"]";
                            mod.edits.push_back(std::move(edit));
                        };
                        add("Operation",op);
                        if (const auto* expected = row.find("expect")) {
                            if (!expected->object()) throw std::runtime_error("row expect requires a typed root-field object");
                            std::set<std::string> names;
                            for (const auto& [name,input] : *expected->object()) {
                                if (name.empty() || name.find_first_of(".[]") != name.npos || !names.insert(name).second)
                                    throw std::runtime_error("expect requires unique root property names");
                                add("ExpectedFields."+name,property_text(input));
                            }
                        }
                        if (const auto* fields = row.find("fields")) {
                            if (op == "remove" || !fields->object()) throw std::runtime_error("row fields require object and a non-remove op");
                            std::set<std::string> names;
                            for (const auto& [name,input] : *fields->object()) {
                                if (name.empty() || name.find_first_of(".[]") != name.npos || !names.insert(name).second)
                                    throw std::runtime_error("fields require unique root property names");
                                add("Fields."+name,property_text(input));
                            }
                        }
                    }
                    continue;
                }
                const auto restore = optional(patch, "restore_policy");

                uint32_t patch_flags = flags & ~IF_PERSISTENT_OVERLAY;
                const auto drift = optional(patch, "drift_policy");
                if (drift == "persistent_overlay") patch_flags |= IF_PERSISTENT_OVERLAY;
                else if (!drift.empty() && drift != "preserve_external_change")
                    throw std::runtime_error("drift_policy must be preserve_external_change or persistent_overlay");
                if (restore == "none")
                    patch_flags |= IF_NO_RESTORE;
                else if (restore == "required")
                    patch_flags &= ~IF_NO_RESTORE;
                else if (!restore.empty())
                    throw std::runtime_error("restore_policy must be required or none");
                const auto *list = patch.find("edits");
                if (!list || !list->array())
                    throw std::runtime_error("edits must be array");
                std::size_t index{};
                for (const auto &item : *list->array()) {
                    if (++edits > 128)
                        throw std::runtime_error("catalog assignment limit128");
                    Edit edit;
                    edit.provenance = source + ": edit[" + std::to_string(index++) + "]";
                    if (!item.object())
                        throw std::runtime_error(edit.provenance + " must be object");
                    edit.provider = "unreal.object";
                    edit.object = object;
                    edit.row = optional(patch, "selector");
                    if (!edit.row.empty() && edit.row != "object" && edit.row != "defaults" &&
                        !(edit.row.starts_with("component:") && edit.row.size() > 10))
                        throw std::runtime_error("object selector must be object, defaults or component:<name>");
                    edit.guard = guard;
                    edit.flags = patch_flags;
                    edit.property = required(item, "property");
                    const auto *input = item.find("value");
                    if (!input)
                        throw std::runtime_error(edit.provenance + " missing value/text");
                    edit.value = value(*input);
                    if (auto expected = item.find("expect")) {
                        edit.expect = value(*expected);
                        edit.has_expect = true;
                    }
                    mod.edits.push_back(std::move(edit));
                }
            }
            result.mods.push_back(std::move(mod));
            if (result.mods.size() > 127)
                throw std::runtime_error("catalog member limit127");
        }
        std::sort(result.mods.begin(), result.mods.end(), [](const Mod &a, const Mod &b) {
            if (a.priority != b.priority)
                return a.priority < b.priority;
            if (a.ordering_key != b.ordering_key)
                return a.ordering_key < b.ordering_key;
            return a.id < b.id;
        });
    } catch (const std::exception &e) {
        result.errors.push_back(source + ": " + e.what());
        result.mods.clear();
    }
    return result;
}
uint32_t publish(const IF_Api &api, IF_Owner owner, const Candidate &candidate, uint64_t revision,
                 IF_Request *out) {
    if (!candidate.errors.empty() || !api.catalog_publish)
        return IF_INVALID_ARGUMENT;
    std::vector<std::vector<IF_Assignment>> edits;

    std::vector<IF_TbcrCompiled> compiled;
    size_t count = 0;
    for (const auto& mod : candidate.mods)
        for (const auto& edit : mod.edits) {
            const auto expanded = edit.tbcr && edit.tbcr_update ? edit.writes.size() : 1;
            if (!expanded) return IF_INVALID_ARGUMENT;
            if (expanded > 128 - count) return IF_LIMIT_EXCEEDED;
            count += expanded;
        }

    compiled.reserve(count);
    std::vector<IF_CatalogEntry> members;
    edits.reserve(candidate.mods.size());
    members.reserve(candidate.mods.size());
    const std::string set = "content";
    for (const auto &mod : candidate.mods) {
        edits.emplace_back();
        for (const auto &edit : mod.edits) {
            if (!edit.tbcr) { edits.back().push_back(edit.view()); continue; }
            if (edit.tbcr_update ? !api.tbcr_compile_update : !api.tbcr_compile_append)
                return IF_CAPABILITY_UNAVAILABLE;
            std::vector<std::vector<IF_Value>> values;
            std::vector<IF_TbcrWrite> writes;
            values.reserve(edit.writes.size()); writes.reserve(edit.writes.size());
            for (const auto& write : edit.writes) {
                values.emplace_back();
                for (const auto& v : write.values) values.back().push_back(v.view());
                writes.push_back({sizeof(IF_TbcrWrite), write.offset, span(write.property), span(write.type),
                    static_cast<uint32_t>(values.back().size()), 0, values.back().data()});
            }
            const auto prepared_count = edit.tbcr_update ? writes.size() : 1;
            std::vector<std::pair<uint64_t, uint64_t>> ranges;
            for (std::size_t i = 0; i < prepared_count; ++i) {
                compiled.emplace_back(); auto& result = compiled.back(); result.size = sizeof(result);
                uint32_t error;
                if (edit.tbcr_update) {
                    IF_TbcrUpdate input{sizeof(input), edit.table_index, span(edit.object), span(edit.table_alias),
                        edit.record_size, edit.record_index, &writes[i]};
                    error = api.tbcr_compile_update(&input, &result);
                } else {
                    IF_TbcrAppend input{sizeof(input), edit.table_index, span(edit.object), span(edit.table_alias),
                        edit.record_size, static_cast<uint32_t>(writes.size()), writes.data(), span(edit.tbcr_id)};
                    error = api.tbcr_compile_append(&input, &result);
                }
                std::string provenance = edit.provenance;
                if (edit.tbcr_update) {
                    provenance += " write[" + std::to_string(i) + "] ";
                    provenance += edit.writes[i].property.empty()
                        ? "offset=" + std::to_string(edit.writes[i].offset) + " type=" + edit.writes[i].type
                        : "property=" + edit.writes[i].property;
                }
                if (error != IF_OK) {
                    if (api.log) { const auto message = provenance + ": " + result.message; api.log(2, span(message)); }
                    return error;
                }
                if (edit.tbcr_update) {
                    const uint64_t begin = result.byte_offset, end = begin + result.byte_count;
                    if (!result.byte_count || result.byte_count > sizeof(result.bytes)) return IF_INVALID_ARGUMENT;
                    for (const auto& range : ranges)
                        if (begin < range.second && range.first < end) {
                            if (api.log) { const auto message = provenance + ": overlapping tbcr writes within edit"; api.log(2, span(message)); }
                            return IF_INVALID_ARGUMENT;
                        }
                    ranges.emplace_back(begin, end);
                }
                const auto chars = [](const char* s) { return IF_String{s, static_cast<uint32_t>(std::char_traits<char>::length(s))}; };
                IF_Assignment assignment{}; assignment.size = sizeof(assignment); assignment.flags = edit.flags;
                assignment.target = {chars(result.provider), chars(result.asset), chars(result.row), {}};
                assignment.property = chars(result.property); assignment.value = result.value;
                edits.back().push_back(assignment);
            }
        }
        IF_CatalogEntry entry{};
        entry.size = sizeof(entry);
        entry.owner = {sizeof(IF_OwnerInfo), mod.priority, span(mod.id), span(mod.name),
                       span(mod.ordering_key)};
        entry.patch = {
            sizeof(IF_PatchSet), static_cast<uint32_t>(edits.back().size()), span(set), revision, 0,
            edits.back().data()};
        members.push_back(entry);
    }
    IF_Catalog catalog{sizeof(IF_Catalog), static_cast<uint32_t>(members.size()), revision, members.data()};
    return api.catalog_publish(owner, &catalog, out);
}
Client::Client(IF_Api api, std::filesystem::path directory, uint32_t flags)
    : api_(api), directory_(std::move(directory)), flags_(flags) {}
Client::~Client() {
    if (request_)
        api_.release(owner_, request_);
}
void Client::log(uint32_t level, const std::string &text) {
    api_.log(level, span(text));
}
bool Client::start() {
    const std::string id = "infinifusion.json", name = "JSON mods";
    IF_OwnerInfo info{sizeof(info), 0, span(id), span(name), span(id)};
    const auto r = api_.register_owner(&info, &owner_);
    if (r != IF_OK) {
        log(2, "JSON controller registration error=" + std::to_string(r));
        return false;
    }
    return true;
}
void Client::poll(bool force, bool scan) {
    if (!owner_)
        return;
    manual_reload_ |= force;
    if (request_) {
        IF_Status state{sizeof(state)};
        const auto r = api_.status(owner_, request_, IF_STATUS_SUMMARY, &state);
        if (r != IF_OK) {
            log(2, "JSON status error=" + std::to_string(r));
            return;
        }
        const auto summary =
            "JSON revision=" + std::to_string(revision_) + " state=" + std::to_string(state.state) +
            " error=" + std::to_string(state.error) + " pending=" + std::to_string(state.pending_count) +
            " rejected=" + std::to_string(state.rejected_count) + " " + state.message;
        if (summary != last_summary_) {
            last_summary_ = summary;
            log(state.error ? 2u : 1u, summary);
        }
        if (awaiting_ && state.observed) {
            awaiting_ = false;

            if (state.operation_count ||
                (state.state != IF_CANCELLED && state.state != IF_FAULTED && state.state != IF_REJECTED)) {
                active_fingerprint_ = candidate_.fingerprint;
                have_active_ = true;
            } else {
                rejected_fingerprint_ = candidate_.fingerprint;
                have_rejected_ = true;
                log(2, "JSON publication rejected; previous catalog preserved");
            }
        }
        if (awaiting_)
            return;
        if (state.operation_count) {
            uint32_t index{};
            for (const auto &mod : candidate_.mods)
                for (const auto &edit : mod.edits) {
                  const auto expanded = edit.tbcr && edit.tbcr_update ? edit.writes.size() : 1;
                  for (std::size_t write_index = 0; write_index < expanded; ++write_index) {
                    if (index >= state.operation_count)
                        break;
                    IF_Status operation{sizeof(operation)};
                    const auto result = api_.status(owner_, request_, index, &operation);
                    if (result == IF_OK) {
                        const auto signature =
                            (static_cast<uint64_t>(operation.state) << 32) | operation.error;
                        if (operation_states_.size() <= index)
                            operation_states_.resize(index + 1, UINT64_MAX);
                        if (operation_states_[index] != signature) {
                            operation_states_[index] = signature;
                            auto provenance = edit.provenance;
                            if (edit.tbcr && edit.tbcr_update) {
                                const auto& write = edit.writes[write_index];
                                provenance += " write[" + std::to_string(write_index) + "] ";
                                provenance += write.property.empty()
                                    ? "offset=" + std::to_string(write.offset) + " type=" + write.type
                                    : "property=" + write.property;
                            }
                            log(operation.error ? 2u : 1u, "JSON " + mod.id + " " + provenance +
                                                               " state=" + std::to_string(operation.state) +
                                                               " error=" + std::to_string(operation.error) +
                                                               " " + operation.message);
                        }
                    }
                    ++index;
                  }
                }

            const auto remaining = state.operation_count - index;
            const auto count = std::min<uint32_t>(remaining, 128);
            if (remaining) {
                removal_cursor_ %= remaining;
                for (uint32_t n = 0; n < count; ++n) {
                    const auto operation_index = index + (removal_cursor_ + n) % remaining;
                    IF_Status operation{sizeof(operation)};
                    if (api_.status(owner_, request_, operation_index, &operation) != IF_OK)
                        continue;
                    const auto signature = (static_cast<uint64_t>(operation.state) << 32) | operation.error;

                    if (operation_index >= 131072)
                        break;
                    if (operation_states_.size() <= operation_index)
                        operation_states_.resize(operation_index + 1, UINT64_MAX);
                    if (operation_states_[operation_index] != signature) {
                        operation_states_[operation_index] = signature;
                        log(operation.error ? 2u : 1u,
                            "JSON prior removal index=" + std::to_string(operation_index) + " path=" +
                                operation.canonical_path + " state=" + std::to_string(operation.state) +
                                " error=" + std::to_string(operation.error) + " " + operation.message);
                    }
                }
                removal_cursor_ = (removal_cursor_ + count) % remaining;
            }
        }
    }
    if (!scan && !manual_reload_)
        return;
    auto next = load(directory_, flags_);
    if (!next.errors.empty()) {
        have_pending_ = false;
        manual_reload_ = false;
        const auto &error = next.errors.front();
        if (error != last_error_) {
            last_error_ = error;
            log(2, "JSON candidate rejected: " + error);
        }
        return;
    }
    last_error_.clear();
    if (!manual_reload_ && ((have_active_ && next.fingerprint == active_fingerprint_) ||
                            (have_rejected_ && next.fingerprint == rejected_fingerprint_))) {
        have_pending_ = false;
        return;
    }

    if (!have_pending_ || pending_fingerprint_ != next.fingerprint) {
        pending_fingerprint_ = next.fingerprint;
        have_pending_ = true;
        return;
    }
    IF_Request request{};
    const auto result = publish(api_, owner_, next, revision_ + 1, &request);
    if (result != IF_OK) {
        if (result != last_admission_error_)
            log(2, "JSON admission error=" + std::to_string(result) + "; previous catalog preserved");
        last_admission_error_ = result;
        if (result != IF_QUEUE_FULL && result != IF_LIMIT_EXCEEDED) {
            have_rejected_ = true;
            rejected_fingerprint_ = next.fingerprint;
            manual_reload_ = false;
            have_pending_ = false;
        }
        return;
    }
    last_admission_error_ = IF_OK;
    if (request_)
        api_.release(owner_, request_);
    request_ = request;
    candidate_ = std::move(next);
    ++revision_;
    awaiting_ = true;
    last_summary_.clear();
    have_pending_ = false;
    have_rejected_ = false;
    manual_reload_ = false;
    operation_states_.clear();
    removal_cursor_ = 0;
}
}
