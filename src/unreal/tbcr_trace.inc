using TbcrExec = void(__fastcall*)(void*, void*, void*);
class TbcrTrace;
void runtime_tbcr_before_parser(void*) noexcept;
std::atomic<TbcrTrace*> g_tbcr_trace{};
void __fastcall tbcr_trace_devil(void*, void*, void*);
void __fastcall tbcr_trace_unite(void*, void*, void*);
void __fastcall tbcr_trace_destroy(void*, void*, void*);

class TbcrTrace {

    static constexpr std::uint32_t unavailable_object = 0x200u | 0x400u | 0x1000u | 0x2000u |
        0x4000u | 0x8000u | 0x10000u | 0x20000u | 0x08000000u;
    struct Identity { void* object{}; std::int32_t index{-1}, serial{}; };
    struct Target {
        const char* owner;
        const char* name;
        std::uint8_t parameters;
        std::uint16_t size;
        TbcrExec wrapper;
        std::atomic<TbcrExec> original{};
        std::atomic<unsigned> calls{};
        bool armed{}, rejected{};
        void* function{};
    };
    Runtime& runtime_;
    EngineAllocator allocator_probe_;
    bool diagnostic_{};
    std::array<Target, 3> targets_{{
        {"ProjectGameInstanceBase", "MakeUpDevilDataTable", 3, 24, tbcr_trace_devil},
        {"UniteCtrl", "MakeUpUniteTable", 1, 8, tbcr_trace_unite},
        {"UniteCtrl", "DestroyUniteTable", 0, 0, tbcr_trace_destroy}
    }};
    std::array<Identity, 2> sources_{};
    std::atomic<DWORD> thread_{};
    std::uint64_t started_{};
    std::int32_t cursor_{};
    bool ended_{};

    static bool cas(void* slot, void* expected, void* desired) noexcept {
        __try {
            return InterlockedCompareExchangePointer(
                reinterpret_cast<PVOID volatile*>(slot), desired, expected) == expected;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool executable(const void* pointer) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto base = reinterpret_cast<std::uintptr_t>(runtime_.image_base);
        if (address < base || address - base >= runtime_.image_size) return false;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }
    const std::uint8_t* item(std::int32_t index) const {
        const auto header = safe_read<UObjectArrayHeader>(runtime_.object_array);
        if (!header || !header->objects || index < 0 || index >= header->num_elements) return nullptr;
        const auto chunk = safe_read<void*>(header->objects + index / kElementsPerChunk).value_or(nullptr);
        return chunk ? static_cast<const std::uint8_t*>(chunk) + (index % kElementsPerChunk) * kObjectItemSize : nullptr;
    }
    Identity identity(void* object) const {
        const auto index = safe_read_at<std::int32_t>(object, 0x0C).value_or(-1);
        const auto slot = item(index);
        if (!slot || safe_read<void*>(slot).value_or(nullptr) != object) return {};
        const auto serial = safe_read_at<std::int32_t>(slot, 0x10).value_or(0);
        const auto flags = safe_read_at<std::uint32_t>(slot, 8).value_or(0xFFFFFFFF);
        return serial > 0 && !(flags & ((1u << 28) | (1u << 29))) ? Identity{object, index, serial} : Identity{};
    }
    bool valid(const Identity& known) const {
        if (!known.object) return false;
        const auto now = identity(known.object);
        return now.object == known.object && now.index == known.index && now.serial == known.serial;
    }

    bool rooted_function(void* function, const Target& target) const {
        const auto index = safe_read_at<std::int32_t>(function, 0x0C).value_or(-1);
        const auto slot = item(index);
        constexpr std::uint32_t rooted_native = (1u << 25) | (1u << 30);
        const auto flags = safe_read_at<std::uint32_t>(slot, 8).value_or(0xFFFFFFFF);
        const auto object_flags = safe_read_at<std::uint32_t>(function, 8).value_or(0xFFFFFFFF);
        const auto outer = safe_read_at<void*>(function, 0x20).value_or(nullptr);
        return slot && safe_read<void*>(slot).value_or(nullptr) == function &&
            (flags & rooted_native) == rooted_native &&
            !(flags & ((1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 31))) &&
            !(object_flags & unavailable_object) &&
            class_name(runtime_, function) == "Function" && outer && class_name(runtime_, outer) == "Class" &&
            object_name(runtime_, outer) == target.owner &&
            object_path(runtime_, function) == std::string("/Script/Project.") + target.owner + "." + target.name;
    }

    std::optional<PropertyInfo> field(void* type, std::string_view name) {
        for (unsigned depth = 0; type && depth < 8; ++depth) {
            auto* field = safe_read_at<void*>(type, 0x50).value_or(nullptr);
            for (unsigned n = 0; field && n < 128; ++n) {
                auto info = inspect_property(runtime_, field);
                if (info.name == name) return info;
                field = safe_read_at<void*>(field, 0x20).value_or(nullptr);
            }
            type = safe_read_at<void*>(type, 0x40).value_or(nullptr);
        }
        return std::nullopt;
    }
    std::optional<PropertyInfo> property(void* object, std::string_view name) {
        return field(safe_read_at<void*>(object, 0x10).value_or(nullptr), name);
    }
    bool signature(void* function, const Target& target) {
        if (safe_read_at<std::uint8_t>(function, 0xB4).value_or(255) != target.parameters ||
            safe_read_at<std::uint16_t>(function, 0xB6).value_or(65535) != target.size ||
            !(safe_read_at<std::uint32_t>(function, 0xB0).value_or(0) & 0x400)) return false;
        auto* field = safe_read_at<void*>(function, 0x50).value_or(nullptr);
        unsigned parameters{};
        constexpr std::array<std::string_view, 3> names{"pAsset", "pDevilName", "pRaceName"};
        for (unsigned n = 0; field && n < 16; ++n) {
            const auto info = inspect_property(runtime_, field);
            if (info.flags & 0x80) {
                if (parameters >= target.parameters || info.type != "ObjectProperty" ||
                    info.name != names[parameters] || info.offset != static_cast<int>(parameters * 8) ||
                    info.element_size != 8 || info.array_dim != 1 || info.flags & 0x400) return false;
                const auto pointee = safe_read_at<void*>(field, 0x78).value_or(nullptr);
                if (!pointee || object_name(runtime_, pointee) != (parameters ? "ScriptMessageAsset" : "MMIAsset")) return false;
                ++parameters;
            }
            field = safe_read_at<void*>(field, 0x20).value_or(nullptr);
        }
        return !field && parameters == target.parameters;
    }
    void arm(void* function, Target& target) {
        const auto vtable = safe_read<void*>(function).value_or(nullptr);
        const auto vfirst = safe_read<void*>(vtable).value_or(nullptr);
        const auto original = safe_read_at<TbcrExec>(function, 0xD8).value_or(nullptr);
        auto* slot = static_cast<std::uint8_t*>(function) + 0xD8;
        const bool identity_ok = rooted_function(function, target), vtable_ok = executable(vfirst);
        const bool signature_ok = signature(function, target);
        const bool original_ok = executable(reinterpret_cast<const void*>(original));
        const bool aligned = reinterpret_cast<std::uintptr_t>(slot) % alignof(void*) == 0;
        const bool writable = is_writable_range(slot, sizeof(void*));
        if (!identity_ok || !vtable_ok || !signature_ok || !original_ok || !aligned || !writable) {
            target.rejected = true;
            const auto index = safe_read_at<std::int32_t>(function, 0x0C).value_or(-1);
            const auto registry_slot = item(index);
            std::ostringstream detail;
            detail << "TBCR trace rejected live layout: " << target.name
                << " guards={identity:" << identity_ok << ",vtable:" << vtable_ok
                << ",signature:" << signature_ok << ",original:" << original_ok
                << ",aligned:" << aligned << ",writable:" << writable << '}'
                << " index=" << index << " serial=" << safe_read_at<std::int32_t>(registry_slot, 0x10).value_or(-1)
                << " item_flags=" << hex_value(safe_read_at<std::uint32_t>(registry_slot, 8).value_or(0xFFFFFFFF))
                << " function_flags=" << hex_value(safe_read_at<std::uint32_t>(function, 0xB0).value_or(0))
                << " num_parms=" << static_cast<unsigned>(safe_read_at<std::uint8_t>(function, 0xB4).value_or(255))
                << " parms_size=" << safe_read_at<std::uint16_t>(function, 0xB6).value_or(65535)
                << " vfirst=" << hex_value(reinterpret_cast<std::uintptr_t>(vfirst))
                << " original=" << hex_value(reinterpret_cast<std::uintptr_t>(original));
            log_line("WARN", detail.str());
            return;
        }
        target.original.store(original, std::memory_order_release);
        if (!rooted_function(function, target)) {
            target.rejected = true;
            log_line("WARN", std::string("TBCR trace rooted function changed before CAS: ") + target.name);
            return;
        }
        if (!cas(slot, reinterpret_cast<void*>(original), reinterpret_cast<void*>(target.wrapper))) {
            target.rejected = true;
            log_line("WARN", std::string("TBCR trace exec slot changed before CAS: ") + target.name);
            return;
        }
        target.armed = true;
        target.function = function;
        log_line("INFO", std::string("TBCR trace armed ") + target.name + " original=" +
            hex_value(reinterpret_cast<std::uintptr_t>(original)) + "; observed native/rooted function; no durable identity claim; startup coverage unknown until actual calls recorded");
    }
    void array_snapshot(std::ostringstream& out, void* object, const char* name) {
        const auto info = property(object, name);
        if (!info || info->type != "ArrayProperty" || info->offset < 0 || info->element_size != 16) return;
        const auto inner = safe_read_at<void*>(info->address, 0x78).value_or(nullptr);
        if (!inner) return;
        const auto stride = inspect_property(runtime_, inner).element_size;
        const auto header = safe_read_at<FStringHeader>(object, info->offset);
        if (!header || header->num < 0 || header->max < header->num || header->max > 1'000'000 ||
            stride <= 0 || stride > 4096 || (header->num && !header->data)) return;
        out << ' ' << name << "={data:" << hex_value(reinterpret_cast<std::uintptr_t>(header->data))
            << ",num:" << header->num << ",max:" << header->max << ",stride:" << stride << '}';
    }
    void recipe_samples(std::ostringstream& out, void* table) {
        const auto skip = [&](const char* reason) { out << " recipe_samples=skip(" << reason << ')'; };
        const auto array = property(table, "UniteSpTableArray");
        if (!array || array->type != "ArrayProperty" || array->offset < 0 || array->element_size != 16 || array->array_dim != 1) {
            skip("outer array layout"); return;
        }
        const auto inner = safe_read_at<void*>(array->address, 0x78).value_or(nullptr);
        const auto element = inner ? inspect_property(runtime_, inner) : PropertyInfo{};
        const auto structure = inner ? safe_read_at<void*>(inner, 0x78).value_or(nullptr) : nullptr;
        if (!structure || element.type != "StructProperty" || element.element_size != 32 || element.array_dim != 1 ||
            object_path(runtime_, structure) != "/Script/Project.UniteSpTable") { skip("recipe struct layout"); return; }
        const auto sort = field(structure, "SortID"), ids = field(structure, "NkmId"), result = field(structure, "ResultId");
        const auto integer = [](const std::optional<PropertyInfo>& p, int offset) {
            return p && p->type == "IntProperty" && p->offset == offset && p->element_size == 4 && p->array_dim == 1;
        };
        const auto ids_inner = ids ? safe_read_at<void*>(ids->address, 0x78).value_or(nullptr) : nullptr;
        const auto ids_element = ids_inner ? inspect_property(runtime_, ids_inner) : PropertyInfo{};
        if (!integer(sort, 0) || !integer(result, 24) || !ids || ids->type != "ArrayProperty" || ids->offset != 8 ||
            ids->element_size != 16 || ids->array_dim != 1 || !ids_inner || ids_element.type != "IntProperty" ||
            ids_element.element_size != 4 || ids_element.array_dim != 1) { skip("recipe fields layout"); return; }
        const auto header = safe_read_at<FStringHeader>(table, array->offset);
        if (!header || header->num < 0 || header->num > 4096 || header->max < header->num || header->max > 4096 ||
            (header->num && !header->data)) { skip("outer header invalid"); return; }
        if (!header->num) { skip("outer array empty"); return; }
        const auto address = [](const void* base, std::size_t offset, std::size_t width) -> const void* {
            const auto begin = reinterpret_cast<std::uintptr_t>(base);
            const auto limit = std::numeric_limits<std::uintptr_t>::max();
            return begin && offset <= limit - begin && width <= limit - begin - offset
                ? reinterpret_cast<const void*>(begin + offset) : nullptr;
        };
        std::ostringstream samples;
        struct Copied { const void* entry{}; const void* payload{}; std::size_t count{};
            std::array<std::uint8_t, 32> bytes{}; std::array<std::int32_t, 4> values{}; };
        std::array<Copied, 2> copied{};
        const unsigned sample_count = header->num == 1 ? 1u : 2u;
        for (unsigned sample = 0; sample < sample_count; ++sample) {
            const auto index = sample ? header->num - 1 : 0;
            const auto entry = address(header->data, static_cast<std::size_t>(index) * 32, 32);
            std::array<std::uint8_t, 32> bytes{}, reread{};
            if (!entry || !safe_copy(entry, bytes.data(), bytes.size())) { skip("entry unreadable"); return; }
            std::int32_t sort_id{}, result_id{}; FStringHeader nested{};
            std::memcpy(&sort_id, bytes.data(), 4); std::memcpy(&nested, bytes.data() + 8, 16);
            std::memcpy(&result_id, bytes.data() + 24, 4);
            if (nested.num < 0 || nested.max < nested.num || nested.max > 64 || (nested.num && !nested.data)) {
                skip("ingredient header invalid"); return;
            }
            const auto count = std::min<std::size_t>(4, nested.num);
            std::array<std::int32_t, 4> values{}, final_values{};
            const auto payload = address(nested.data, 0, count * sizeof(std::int32_t));
            if (count && (!payload || !safe_copy(payload, values.data(), count * sizeof(std::int32_t)))) {
                skip("ingredients unreadable"); return;
            }
            const auto final_header = safe_read_at<FStringHeader>(table, array->offset);
            copied[sample] = {entry, payload, count, bytes, values};
            if (!final_header || final_header->data != header->data || final_header->num != header->num || final_header->max != header->max ||
                !safe_copy(entry, reread.data(), reread.size()) || reread != bytes ||
                (count && (!safe_copy(payload, final_values.data(), count * sizeof(std::int32_t)) || final_values != values))) {
                skip("recipe storage changed"); return;
            }
            samples << " recipe_sample=ObservedOnly{index:" << index << ",SortID:" << sort_id << ",ResultId:" << result_id
                << ",NkmIdNum:" << nested.num << ",NkmId:[";
            for (std::size_t i = 0; i < count; ++i) { if (i) samples << ','; samples << values[i]; }
            samples << "],sampled:" << count << '}';
        }

        const auto final_outer = safe_read_at<FStringHeader>(table, array->offset);
        if (!final_outer || final_outer->data != header->data || final_outer->num != header->num || final_outer->max != header->max) {
            skip("outer storage changed"); return;
        }
        for (unsigned sample = 0; sample < sample_count; ++sample) {
            std::array<std::uint8_t, 32> bytes{}; std::array<std::int32_t, 4> values{};
            const auto& original = copied[sample];
            if (!safe_copy(original.entry, bytes.data(), bytes.size()) || bytes != original.bytes ||
                (original.count && (!safe_copy(original.payload, values.data(), original.count * sizeof(std::int32_t)) || values != original.values))) {
                skip("endpoint changed before emission"); return;
            }
        }
        out << samples.str();
    }
    void table_headers(std::ostringstream& out, void* table) {

        const auto index = safe_read_at<std::int32_t>(table, 0x0C).value_or(-1);
        const auto slot = item(index);
        const auto serial = safe_read_at<std::int32_t>(slot, 0x10).value_or(-1);
        const auto klass = safe_read_at<void*>(table, 0x10).value_or(nullptr);
        const auto live = [&] {
            return slot && item(index) == slot && safe_read<void*>(slot).value_or(nullptr) == table &&
                safe_read_at<std::int32_t>(table, 0x0C).value_or(-1) == index &&
                safe_read_at<std::int32_t>(slot, 0x10).value_or(-1) == serial &&
                !(safe_read_at<std::uint32_t>(slot, 8).value_or(0xFFFFFFFF) &
                    ((1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 31))) &&
                !(safe_read_at<std::uint32_t>(table, 8).value_or(0xFFFFFFFF) & unavailable_object) &&
                safe_read_at<void*>(table, 0x10).value_or(nullptr) == klass && class_name(runtime_, table) == "UniteTable";
        };
        if (serial < 0 || !klass || !live()) return;
        std::ostringstream headers;
        headers << " table_index=" << index << " table_serial=" << serial
            << " headers_only=ObservedOnly (no generation/anti-reuse certificate)";
        for (const char* name : {"UniteDataTableArray", "UniteNormalTableArray", "UniteSeireiBornTableArray",
            "UniteSeireiTableArray", "UniteSpTableArray", "UniteAccidentProbTableArray", "UniteAccidentDevilTableArray",
            "UniteAccidentTableArray", "BibleCdfsArray", "BibleCoeArray", "InheritSkillTekiseiTableArray", "UniteDlcDevilTableArray"})
            array_snapshot(headers, table, name);
        recipe_samples(headers, table);
        if (live()) out << headers.str();
    }
    bool literal_asset(std::ostringstream& out, const Identity& context, void* stack, bool emit = true, void** source = nullptr) {
        const auto skip = [&](const char* reason) { out << " actual_pAsset=skip(" << reason << ')'; };
        if (!valid(context) || !targets_[1].armed || !targets_[1].function ||
            !rooted_function(targets_[1].function, targets_[1])) { skip("context/target uncertified"); return false; }

        const auto node = safe_read_at<void*>(stack, 0x10).value_or(nullptr);
        const auto object = safe_read_at<void*>(stack, 0x18).value_or(nullptr);
        const auto code = safe_read_at<void*>(stack, 0x20).value_or(nullptr);
        const auto caller = identity(node);
        const auto outer = safe_read_at<void*>(node, 0x20).value_or(nullptr);
        const auto caller_live = [&] {
            return valid(caller) && class_name(runtime_, node) == "Function" && outer &&
                safe_read_at<void*>(node, 0x20).value_or(nullptr) == outer && class_name(runtime_, outer) == "BlueprintGeneratedClass" &&
                object_name(runtime_, outer) == "BP_UniteCtrl_C" &&
                object_path(runtime_, node) == "/Game/Blueprints/Unit/Blueprints/BP_UniteCtrl.BP_UniteCtrl_C.ExecuteUbergraph_BP_UniteCtrl" &&
                !(safe_read_at<std::uint32_t>(item(caller.index), 8).value_or(0xFFFFFFFF) & ((1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 31))) &&
                !(safe_read_at<std::uint32_t>(node, 8).value_or(0xFFFFFFFF) & unavailable_object);
        };
        if (!caller_live() || object != context.object || !code) { skip("unsupported caller/frame"); return false; }
        const auto script = safe_read_at<FStringHeader>(node, 0x60);
        const auto begin = script ? reinterpret_cast<std::uintptr_t>(script->data) : 0;
        const auto pc = reinterpret_cast<std::uintptr_t>(code);
        if (!script || !begin || script->num < 19 || script->num > 1024 * 1024 || script->max < script->num ||
            script->max > 1024 * 1024 || pc < begin || pc - begin < 9 || pc - begin > static_cast<std::size_t>(script->num - 10)) {
            skip("script envelope out of bounds"); return false;
        }
        std::array<std::uint8_t, 19> expression{};
        if (!safe_copy(reinterpret_cast<void*>(pc - 9), expression.data(), expression.size())) { skip("envelope unreadable"); return false; }
        void* target{}; void* asset{};
        std::memcpy(&target, expression.data() + 1, sizeof(target));
        std::memcpy(&asset, expression.data() + 10, sizeof(asset));
        if (expression[0] != 0x1C || expression[9] != 0x20 || expression[18] != 0x16 || target != targets_[1].function || !asset) {
            skip("not verified FinalFunction ObjectConst envelope"); return false;
        }
        const auto index = safe_read_at<std::int32_t>(asset, 0x0C).value_or(-1);
        const auto slot = item(index);
        const auto serial = safe_read_at<std::int32_t>(slot, 0x10).value_or(-1);
        const auto klass = safe_read_at<void*>(asset, 0x10).value_or(nullptr);
        const auto live = [&] {
            return serial >= 0 && slot && item(index) == slot && safe_read<void*>(slot).value_or(nullptr) == asset &&
                safe_read_at<std::int32_t>(asset, 0x0C).value_or(-1) == index &&
                safe_read_at<std::int32_t>(slot, 0x10).value_or(-1) == serial &&
                !(safe_read_at<std::uint32_t>(slot, 8).value_or(0xFFFFFFFF) & ((1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 31))) &&
                !(safe_read_at<std::uint32_t>(asset, 8).value_or(0xFFFFFFFF) & unavailable_object) && klass &&
                safe_read_at<void*>(asset, 0x10).value_or(nullptr) == klass && class_name(runtime_, asset) == "MMIAsset" &&
                object_path(runtime_, asset) == "/Game/Blueprints/Gamedata/BinTable/Unite/UniteTable.UniteTable";
        };
        if (!live()) { skip("literal asset unavailable"); return false; }
        const auto info = property(asset, "MyFmtData");
        const auto inner = info ? safe_read_at<void*>(info->address, 0x78).value_or(nullptr) : nullptr;
        const auto inner_info = inner ? inspect_property(runtime_, inner) : PropertyInfo{};
        if (!info || info->type != "ArrayProperty" || info->offset < 0 || info->element_size != 16 || info->array_dim != 1 ||
            !inner || inner_info.type != "ByteProperty" || inner_info.element_size != 1 || inner_info.array_dim != 1) {
            skip("MyFmtData byte layout mismatch"); return false;
        }
        const auto header = safe_read_at<FStringHeader>(asset, info->offset);
        if (!header || header->num < 0 || header->max < header->num || header->max > 16 * 1024 * 1024 || (header->num && !header->data)) {
            skip("MyFmtData header invalid"); return false;
        }
        std::array<std::uint8_t, 16> prefix{};
        const auto count = std::min<std::size_t>(prefix.size(), header->num);
        if (count && !safe_copy(header->data, prefix.data(), count)) { skip("prefix unreadable"); return false; }
        const auto final_header = safe_read_at<FStringHeader>(asset, info->offset);
        const auto final_script = safe_read_at<FStringHeader>(node, 0x60);
        std::array<std::uint8_t, 19> final_expression{};
        if (!final_header || final_header->data != header->data || final_header->num != header->num || final_header->max != header->max ||
            !live() || !valid(context) || !caller_live() || !rooted_function(targets_[1].function, targets_[1]) ||
            !final_script || final_script->data != script->data || final_script->num != script->num || final_script->max != script->max ||
            safe_read_at<void*>(stack, 0x10).value_or(nullptr) != node || safe_read_at<void*>(stack, 0x18).value_or(nullptr) != object ||
            safe_read_at<void*>(stack, 0x20).value_or(nullptr) != code ||
            !safe_copy(reinterpret_cast<void*>(pc - 9), final_expression.data(), final_expression.size()) || final_expression != expression) {
            skip("state changed during copy"); return false;
        }
        if (count < 4 || std::memcmp(prefix.data(), "TBCR", 4) != 0) { skip("source prefix is not TBCR"); return false; }
        if (source) *source=asset;
        if (!emit) return true;
        out << " actual_pAsset=" << hex_value(reinterpret_cast<std::uintptr_t>(asset)) << " index=" << index << " serial=" << serial
            << " sample=ObservedOnly (same phase; no generation/anti-reuse certificate) MyFmtData={data:"
            << hex_value(reinterpret_cast<std::uintptr_t>(header->data)) << ",num:" << header->num << ",max:" << header->max << ",prefix:";
        for (std::size_t i = 0; i < count; ++i) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(prefix[i]);
        out << std::dec << '}';
        return true;
    }
    void snapshot(unsigned which, const Identity& known, const char* phase, unsigned call, void* stack) {
        if (!valid(known)) {
            log_line("INFO", std::string("TBCR trace ") + targets_[which].name + " call=" + std::to_string(call) +
                " " + phase + " context identity uncertified; no table/source read");
            return;
        }
        auto* context = known.object;
        std::ostringstream out;
        out << "TBCR trace " << targets_[which].name << " call=" << call << ' ' << phase
            << " thread=" << GetCurrentThreadId() << " context=" << hex_value(reinterpret_cast<std::uintptr_t>(context))
            << " index=" << known.index << " serial=" << known.serial;
        if (which == 1 && std::string_view(phase) == "before") literal_asset(out, known, stack);
        for (unsigned i = 0; i < sources_.size(); ++i) {
            if (!valid(sources_[i])) continue;
            out << " correlated_source=" << (i ? "UniteTable" : "NKMBaseTable")
                << " object=" << hex_value(reinterpret_cast<std::uintptr_t>(sources_[i].object))
                << " serial=" << sources_[i].serial << " (not decoded pAsset)";
            array_snapshot(out, sources_[i].object, "MyFmtData");
        }
        if (which > 0) {
            const auto pointer = property(context, "m_pUniteTable");
            if (pointer && pointer->type == "ObjectProperty" && pointer->offset >= 0 && pointer->element_size == 8) {
                auto* table = safe_read_at<void*>(context, pointer->offset).value_or(nullptr);
                out << " m_pUniteTable=" << hex_value(reinterpret_cast<std::uintptr_t>(table));
                if (table) table_headers(out, table);
            }
        }
        log_line("INFO", out.str());
    }
    void observe_cpp(unsigned which, const Identity& known, const char* phase, unsigned call, void* stack) noexcept {
        try { snapshot(which, known, phase, call, stack); } catch (...) {}
    }
    void observe(unsigned which, const Identity& known, const char* phase, unsigned call, void* stack) noexcept {
        __try { observe_cpp(which, known, phase, call, stack); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    void timing_cpp(unsigned which, unsigned call, std::int64_t observer_us, std::int64_t original_us) noexcept {
        try {
            log_line("INFO", std::string("TBCR trace timing ") + targets_[which].name + " call=" + std::to_string(call) +
                " observer_us=" + std::to_string(observer_us) + " original_us=" + std::to_string(original_us));
        } catch (...) {}
    }
    void timing(unsigned which, unsigned call, std::int64_t observer_us, std::int64_t original_us) noexcept {
        __try { timing_cpp(which, call, observer_us, original_us); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    Identity capture_identity(void* context) noexcept {
        __try { return identity(context); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
    }
    bool fresh_fusion_cpp(const Identity& context, void* stack, void** source) noexcept {
        try {
            if (!valid(context)) return false;
            const auto pointer = property(context.object, "m_pUniteTable");
            const auto value = pointer ? safe_read_at<void*>(context.object, pointer->offset) : std::optional<void*>{};
            std::ostringstream ignored;
            return pointer && pointer->type == "ObjectProperty" && pointer->offset >= 0 && pointer->element_size == 8 &&
                value && !*value && literal_asset(ignored, context, stack, false, source);
        } catch (...) { return false; }
    }
    bool fresh_fusion(const Identity& context, void* stack, void** source) noexcept {
        __try { return fresh_fusion_cpp(context, stack, source); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
public:
    explicit TbcrTrace(Runtime& runtime) : runtime_(runtime), allocator_probe_(runtime), diagnostic_(infinifusion::runtime::development_profile && GetPrivateProfileIntW(
        L"Diagnostics", L"TbcrTrace", 0, (runtime.module_directory / L"infinifusion.ini").c_str()) != 0) {}
    void tick(DWORD thread) {
        if (!thread_) {
            thread_ = thread; started_ = GetTickCount64();
            log_line("INFO", "Fusion initializer discovery <=128 slots/250us per tick for <=120s; optional TBCR diagnostics <=32 observations per target");
        }
        if (thread != thread_ || ended_) return;
        if (!diagnostic_ && targets_[1].armed) { ended_ = true; return; }
        if (runtime_.exe_timestamp != kKnownExeTimestamp || runtime_.image_size != kKnownImageSize) {
            ended_ = true;
            log_line("WARN", "TBCR trace refused unverified executable fingerprint; no exec slots changed");
            return;
        }
        if (GetTickCount64() - started_ >= 120'000) {
            ended_ = true;
            log_line("INFO", "TBCR trace discovery stopped after 120s; first-initializer/startup coverage remains unknown when not observed");
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(250);
        const auto registry = capture_object_registry(runtime_);
        if (!registry) return;
        if (cursor_ >= registry->header.num_elements) cursor_ = 0;
        for (unsigned visited = 0; visited < 128 && cursor_ < registry->header.num_elements &&
             std::chrono::steady_clock::now() < deadline; ++visited, ++cursor_) {
            auto* object = object_at(*registry, cursor_);
            if (!object) continue;
            const auto name = object_name(runtime_, object);
            for (auto& target : targets_) {
#ifdef INFINIFUSION_DEVELOPMENT
                if (&target == &targets_[0] && g_nkm_boundary.load(std::memory_order_acquire)) continue;
#endif
                if (!diagnostic_ && std::string_view(target.name) != "MakeUpUniteTable") continue;
                if (target.armed || target.rejected || name != target.name) continue;
                const auto outer = safe_read_at<void*>(object, 0x20).value_or(nullptr);
                if (class_name(runtime_, object) != "Function" || !outer || class_name(runtime_, outer) != "Class" ||
                    object_name(runtime_, outer) != target.owner ||
                    object_path(runtime_, object) != std::string("/Script/Project.") + target.owner + "." + target.name) continue;
                arm(object, target);
            }
            for (unsigned i = 0; diagnostic_ && i < sources_.size(); ++i) {
                const char* source = i ? "UniteTable" : "NKMBaseTable";
                if (name != source || class_name(runtime_, object) != "MMIAsset") continue;
                const auto path = std::string("/Game/Blueprints/Gamedata/BinTable/") + (i ? "Unite/UniteTable.UniteTable" : "Devil/NKMBaseTable.NKMBaseTable");
                if (object_path(runtime_, object) == path) sources_[i] = identity(object);
            }
        }
    }
    void invoke(unsigned which, void* context, void* stack, void* result) {
        auto& target = targets_[which];
        const auto original = target.original.load(std::memory_order_acquire);
        const auto call = target.calls.fetch_add(1, std::memory_order_relaxed) + 1;
        static thread_local bool observing{};
        const bool executor = !observing && GetCurrentThreadId() == thread_;
        const bool record = diagnostic_ && call <= 32 && executor;
        const auto observer_started = std::chrono::steady_clock::now();
        const auto known = (record || (executor && which == 1)) ? capture_identity(context) : Identity{};
        void* source{};
        const bool fresh = executor && which == 1 && fresh_fusion(known, stack, &source);
        if (record) { observing = true; observe(which, known, "before", call, stack); observing = false; }
        if (fresh && source) { observing=true; runtime_tbcr_before_parser(source); observing=false; }
        const auto before_done = std::chrono::steady_clock::now();
        // Deliberately outside observer guards: preserve game's original exception semantics.
        original(context, stack, result);
        const auto original_done = std::chrono::steady_clock::now();
        if (record && which == 1 && valid(known)) {
            observing = true; allocator_probe_.once(); observing = false;
        }
        if (record) { observing = true; observe(which, known, "after", call, stack); observing = false; }
        if (record) {
            const auto after_done = std::chrono::steady_clock::now();
            timing(which, call,
                std::chrono::duration_cast<std::chrono::microseconds>(before_done - observer_started + after_done - original_done).count(),
                std::chrono::duration_cast<std::chrono::microseconds>(original_done - before_done).count());
        }
    }
};
void __fastcall tbcr_trace_devil(void* context, void* stack, void* result) { g_tbcr_trace.load(std::memory_order_acquire)->invoke(0, context, stack, result); }
void __fastcall tbcr_trace_unite(void* context, void* stack, void* result) { g_tbcr_trace.load(std::memory_order_acquire)->invoke(1, context, stack, result); }
void __fastcall tbcr_trace_destroy(void* context, void* stack, void* result) { g_tbcr_trace.load(std::memory_order_acquire)->invoke(2, context, stack, result); }
