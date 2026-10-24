struct NativeApiRuntime
{
    std::unique_ptr<Runtime> runtime;
    std::unique_ptr<NativeObjectDiscovery> discovery;
    infinifusion::host::Host host;
    std::unique_ptr<NativeObjectTargets> targets;
    std::shared_ptr<NativeObjectPropertyProvider> objects;
    std::shared_ptr<NativeObjectInspectionProvider> inspection;
    std::shared_ptr<NativeGameServices> game_services;
    std::unique_ptr<TbcrTrace> tbcr_trace;
    std::uint64_t frame{};
    std::atomic<DWORD> executor_thread{};
    DWORD game_thread{};
    bool object_boundary_in_progress{};
    std::atomic<bool> initial_clients_admitted{};
    std::atomic<bool> discovery_ready{};
    std::uint32_t callback_budget_us{1500};
    bool object_property_timing{};
    std::chrono::steady_clock::time_point next_object_timing{};
    std::chrono::steady_clock::time_point next_object_idle_timing{};
    std::uint64_t object_window_validations{},object_window_callbacks{};

    std::map<std::string,bool> loaded_object_targets;
    std::vector<std::unique_ptr<infinifusion::host::ClientSession>> clients;
#ifdef INFINIFUSION_LUA
    std::vector<std::unique_ptr<infinifusion::host::LuaClient>> lua_clients;
#endif
    std::vector<HMODULE> modules;
};
std::atomic<NativeApiRuntime*> g_native_api{};

struct RuntimeObjectLoadScope {
    bool& active;
    bool previous;
    explicit RuntimeObjectLoadScope(bool& value):active(value),previous(value){active=true;}
    ~RuntimeObjectLoadScope(){active=previous;}
};

void runtime_drain_loaded_objects(NativeApiRuntime& state,std::chrono::steady_clock::time_point deadline,
                                  std::string_view phase) {
    if (state.object_boundary_in_progress || state.loaded_object_targets.empty() ||
        !state.initial_clients_admitted.load(std::memory_order_acquire)) return;
    RuntimeObjectLoadScope scope(state.object_boundary_in_progress);
    std::set<std::string> attempted;
    while (attempted.size()<16 && std::chrono::steady_clock::now()<deadline) {
        const auto found=std::find_if(state.loaded_object_targets.begin(),state.loaded_object_targets.end(),
            [&](const auto& event){return !attempted.contains(event.first);});
        if (found==state.loaded_object_targets.end()) break;
        const auto path=found->first;
        const bool needs_refresh=found->second;

        state.loaded_object_targets.erase(found);
        attempted.insert(path);
        const auto ready=state.discovery->resolve({"unreal.object",path,{}, {}});
        const auto started=std::chrono::steady_clock::now();
        const bool completed=ready && state.host.executor_object_definitions(path,deadline,needs_refresh);

        if (!completed && !(needs_refresh && state.host.notify_target("unreal.object",path)))
            state.loaded_object_targets[path]|=needs_refresh;
        if (state.object_property_timing) log_line("OBJECT-LOAD","target="+path+
            " phase="+std::string(phase)+" thread="+std::to_string(GetCurrentThreadId())+
            " target_state="+(ready ? "resolved" : "pending")+
            " pass="+(completed ? "completed" : "deferred")+
            " elapsed_us="+std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-started).count()));
    }
}

void runtime_object_before_component(void* actor,void* source) noexcept {
    auto* state=g_native_api.load(std::memory_order_acquire);
    const auto thread=GetCurrentThreadId();
    if (!state || !actor || !source || !state->discovery_ready.load(std::memory_order_acquire) ||
        !state->game_thread || state->game_thread!=thread) return;
    DWORD expected{};
    state->executor_thread.compare_exchange_strong(expected,thread);
    if (state->executor_thread.load()!=thread) return;
    try {
        const auto owner=state->discovery->certify(actor),actual_source=state->discovery->certify(source);
        if (!owner || !actual_source) return;
        const auto outer=state->discovery->certify(actual_source->outer);

        const std::array<void*,4> candidates{owner->klass,source,actual_source->outer,
            outer ? outer->klass : nullptr};
        for (auto* candidate:candidates) if (candidate) {
            const auto path=state->discovery->requested_path(candidate);
            if (path.empty()) continue;
            bool changed{};
            state->discovery->loaded(candidate,&changed);
            state->loaded_object_targets[path]|=changed;
        }

        runtime_drain_loaded_objects(*state,std::chrono::steady_clock::now()+std::chrono::milliseconds(50),
                                     "before-template-copy");
    } catch (const std::exception& error) {
        try {log_line("WARN","Template-copy boundary: "+std::string(error.what()));} catch (...) {}
    } catch (...) {
        try {log_line("WARN","Template-copy boundary unavailable; original construction preserved");} catch (...) {}
    }
}

void runtime_object_loaded(void* object) noexcept {
    auto* state=g_native_api.load(std::memory_order_acquire);
    const auto thread=GetCurrentThreadId();
    if (!state || !state->discovery_ready.load(std::memory_order_acquire) ||
        !state->game_thread || state->game_thread!=thread) return;
    DWORD expected{};
    state->executor_thread.compare_exchange_strong(expected,thread);
    if (state->executor_thread.load()!=thread) return;
    try {
        const auto path=state->discovery->requested_path(object);
        if (path.empty()) return;
        state->loaded_object_targets[path]=true;

        state->discovery->loaded(object);
        if (state->object_boundary_in_progress) return;
        runtime_drain_loaded_objects(*state,std::chrono::steady_clock::now()+std::chrono::milliseconds(50),"completed-load");
    } catch (const std::exception& error) {log_line("WARN","Object load boundary: "+std::string(error.what()));}
      catch (...) {log_line("WARN","Object load boundary unavailable; pending intent retained");}
}

bool runtime_initial_definitions_ready() noexcept {
    const auto* state=g_native_api.load(std::memory_order_acquire);
    return state && state->discovery_ready.load(std::memory_order_acquire) &&
        state->initial_clients_admitted.load(std::memory_order_acquire);
}

NativeApiRuntime& runtime_definition_pass() {
    auto* state=g_native_api.load(std::memory_order_acquire);
    if (!state) throw std::runtime_error("TBCR loader callback preceded API readiness; restart required");
    if (!runtime_initial_definitions_ready()) throw std::runtime_error("TBCR loader callback preceded initial client admission; restart required");
    const auto thread=GetCurrentThreadId();
    DWORD expected{};
    state->executor_thread.compare_exchange_strong(expected, thread);
    if (state->executor_thread.load()!=thread) throw std::runtime_error("TBCR loader callback is not on the API executor thread");

    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(25);
    if (!state->host.executor_definitions(deadline))
        throw std::runtime_error("shared asset definitions unavailable within loading deadline; source unchanged");
    return *state;
}
std::vector<infinifusion::core::TbcrPatchProvider::Definition> runtime_tbcr_definitions() {
    return runtime_definition_pass().host.tbcr_definitions();
}
std::vector<infinifusion::core::TablePatchProvider::Definition> runtime_table_definitions() {
    return runtime_definition_pass().host.table_definitions();
}
std::vector<infinifusion::core::TablePatchProvider::Definition> runtime_message_definitions() {
    return runtime_definition_pass().host.message_definitions();
}
void runtime_table_observed(const std::string& asset, IF_TableState outcome, const std::string& message) {
    outcome.size=sizeof(outcome);
    const auto length=std::min(message.size(),sizeof(outcome.message)-1);
    std::memcpy(outcome.message,message.data(),length); outcome.message[length]='\0';
    if (auto* state=g_native_api.load(std::memory_order_acquire)) state->host.table_observed(asset,outcome);
    log_line(outcome.error ? "WARN" : "INFO", "TABLE outcome asset="+asset+
        " application="+std::to_string(outcome.application)+" state="+std::to_string(outcome.state)+
        " error="+std::to_string(outcome.error)+" updated="+std::to_string(outcome.updated_rows)+
        " added="+std::to_string(outcome.added_rows)+" removed="+std::to_string(outcome.removed_rows)+
        " fields="+std::to_string(outcome.imported_fields)+" "+message);
}

void runtime_tbcr_observed(const std::string& asset, IF_TbcrState outcome, const std::string& message) {
    outcome.size=sizeof(outcome);
    const auto length=std::min(message.size(),sizeof(outcome.message)-1);
    std::memcpy(outcome.message,message.data(),length); outcome.message[length]='\0';
    if (auto* state=g_native_api.load(std::memory_order_acquire)) state->host.tbcr_observed(asset,outcome);
    log_line(outcome.error ? "WARN" : "INFO", "TBCR outcome asset="+asset+
        " application="+std::to_string(outcome.application)+" state="+std::to_string(outcome.state)+
        " error="+std::to_string(outcome.error)+" "+message);
}

void native_api_tick_cpp()
{
    auto* installed = g_native_api.load(std::memory_order_acquire);
    if (!installed || !installed->discovery_ready.load(std::memory_order_acquire)) return;
    auto& state = *installed;
    const auto thread = GetCurrentThreadId();
    DWORD expected{};
    state.executor_thread.compare_exchange_strong(expected, thread);
    if (state.executor_thread.load() != thread) {
        state.host.fault("Tick executor moved to another thread; API stopped"); return;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::microseconds(state.callback_budget_us);
    if (state.tbcr_trace) state.tbcr_trace->tick(thread);
#ifdef INFINIFUSION_DEVELOPMENT
    if (auto* probe=g_blueprint_creation_probe.load(std::memory_order_acquire)) probe->tick();
#endif
    ++state.frame;
    state.objects->frame(state.frame);
    state.inspection->frame(state.frame);
    const auto discovery_deadline = started + std::chrono::microseconds(state.callback_budget_us / 4);
    {
        RuntimeObjectLoadScope scope(state.object_boundary_in_progress);
        state.discovery->discover(discovery_deadline);
        for (auto it=state.loaded_object_targets.begin();it!=state.loaded_object_targets.end();) {
            if (it->second && !state.host.notify_target("unreal.object",it->first)) break;
            it=state.loaded_object_targets.erase(it);
        }
        state.host.executor_tick(deadline, started);
    }

    runtime_drain_loaded_objects(state,std::chrono::steady_clock::now()+std::chrono::milliseconds(50),"deferred-load");
    if (state.object_property_timing) {
        state.object_window_validations+=state.objects->timings().validations;
        ++state.object_window_callbacks;
        if (started>=state.next_object_idle_timing) {
            log_line("API-OBJECT-IDLE","callbacks="+std::to_string(state.object_window_callbacks)+
                " validations="+std::to_string(state.object_window_validations));
            state.object_window_validations=state.object_window_callbacks=0;
            state.next_object_idle_timing=started+std::chrono::seconds(1);
        }
    }
    if(state.object_property_timing && started>=state.next_object_timing) {
        const auto stats=state.objects->timings();
        const auto export_stats=state.objects->export_timings();
        if(stats.validations) {
            state.next_object_timing=started+std::chrono::seconds(1);
            log_line("API-OBJECT-PERF","validations="+std::to_string(stats.validations)+
                " roles_us="+std::to_string(stats.roles_us)+" membership_us="+std::to_string(stats.membership_us)+
                " schema_us="+std::to_string(stats.schema_us)+" exports_us="+std::to_string(stats.exports_us)+
                " preparations_us="+std::to_string(stats.preparations_us)+" prepared_cache_hits="+std::to_string(stats.cache_hits)+
                " export_layout_us="+std::to_string(export_stats.layout_us)+" export_buffer_us="+std::to_string(export_stats.buffer_us)+
                " export_native_us="+std::to_string(export_stats.native_us)+" export_copy_us="+std::to_string(export_stats.copy_us));
        }
    }
}
void native_api_fault() noexcept
{
    try {
        if (auto* installed = g_native_api.load(std::memory_order_acquire))
            installed->host.fault("native exception; further API writes stopped");
    } catch (...) {}
}
void native_api_tick_cpp_guarded() noexcept
{
    try { native_api_tick_cpp(); } catch (...) { native_api_fault(); }
}
void native_api_tick_guarded() noexcept
{
    __try { native_api_tick_cpp_guarded(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { native_api_fault(); }
}

#ifdef INFINIFUSION_DEVELOPMENT
uint32_t native_plugin_entry(IF_PluginEntry entry, const IF_Api* api, IF_Plugin* plugin) noexcept
{
    try { return entry(api, plugin); } catch (...) { return IF_INTERNAL; }
}
uint32_t native_plugin_initialize(const IF_Plugin& plugin, const IF_Api* api, IF_Owner owner) noexcept
{
    try { return plugin.initialize(api, owner); } catch (...) { return IF_INTERNAL; }
}
bool native_plugin_poll(const IF_Plugin& plugin, const IF_Api* api, IF_Owner owner) noexcept
{
    try { plugin.poll(api, owner); return true; } catch (...) { return false; }
}

void native_api_load_plugins(NativeApiRuntime& state, const std::filesystem::path& directory)
{
    const auto ini = directory / L"infinifusion.ini";
    wchar_t root[2048]{}, list[2048]{};
    GetPrivateProfileStringW(L"API", L"PluginsDirectory", L"infinifusion_plugins", root, 2048, ini.c_str());
    GetPrivateProfileStringW(L"API", L"NativePlugins", L"", list, 2048, ini.c_str());
    auto plugin_root = std::filesystem::path(root);
    if (plugin_root.is_relative()) plugin_root = directory / plugin_root;
    std::wstringstream input(list); std::wstring name;
    while (std::getline(input, name, L';')) {
        if (name.empty()) continue;
        if (state.modules.size() >= 16) { log_line("ERROR", "Native plugin limit (16) reached"); break; }
        const auto filename = std::filesystem::path(name);
        if (filename.has_parent_path() || filename.extension() != L".dll") { log_line("ERROR", "Invalid native plugin basename"); continue; }
        const auto path = std::filesystem::absolute(plugin_root / filename);
        const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) { log_line("ERROR", "Native plugin load failed: " + narrow(name)); continue; }
        auto entry = reinterpret_cast<IF_PluginEntry>(GetProcAddress(module, "Infinifusion_PluginEntry"));
        if (!entry) { log_line("ERROR", "Native plugin entry missing: " + narrow(name)); FreeLibrary(module); continue; }

        state.modules.push_back(module);
        IF_Plugin plugin{}; plugin.size = sizeof(plugin); const auto api = state.host.api();
        if (native_plugin_entry(entry, &api, &plugin) != IF_OK || plugin.size != sizeof(plugin) ||
            plugin.required_api_version != IF_API_VERSION || !plugin.initialize) {
            log_line("ERROR", "Native plugin descriptor invalid: " + narrow(name)); continue;
        }
        auto client = std::make_unique<infinifusion::host::ClientSession>(api);
        const auto result = client->start(plugin.owner,
            [plugin](const IF_Api& api, IF_Owner owner) { return native_plugin_initialize(plugin, &api, owner); },
            [plugin](const IF_Api& api, IF_Owner owner) { return !plugin.poll || native_plugin_poll(plugin, &api, owner); });

        state.clients.push_back(std::move(client));
        if (result != IF_OK) {
            log_line("ERROR", "Native plugin initialization/registration failed: " + narrow(name)); continue;
        }
        log_line("INFO", "Native plugin initialized through public API: " + narrow(name));
    }
#ifdef INFINIFUSION_LUA
    GetPrivateProfileStringW(L"API", L"LuaDirectory", L"infinifusion_mods", root, 2048, ini.c_str());
    GetPrivateProfileStringW(L"API", L"LuaMods", L"", list, 2048, ini.c_str());
    auto lua_root = std::filesystem::path(root);
    if (lua_root.is_relative()) lua_root = directory / lua_root;
    std::wstringstream lua_input(list);
    while (std::getline(lua_input, name, L';')) {
        name = trim(name);
        if (name.empty()) continue;
        if (state.lua_clients.size() >= 16) { log_line("ERROR", "Lua mod limit (16) reached"); break; }
        auto client = std::make_unique<infinifusion::host::LuaClient>(state.host.api());
        const auto ok = client->load(lua_root, narrow(name));
        log_line(ok ? "INFO" : "ERROR", ok ? "Lua mod initialized through public API: " + narrow(name) :
            "Lua mod load failed: " + narrow(name) + ": " + client->error());
        state.lua_clients.push_back(std::move(client));
    }
#endif

}

#endif
