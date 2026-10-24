set(INFINIFUSION_PROFILE "Development" CACHE STRING "Product profile: Development or Release")
set_property(CACHE INFINIFUSION_PROFILE PROPERTY STRINGS Development Release)
if(NOT INFINIFUSION_PROFILE MATCHES "^(Development|Release)$")
    message(FATAL_ERROR "INFINIFUSION_PROFILE must be Development or Release")
endif()
if(INFINIFUSION_PROFILE STREQUAL "Release")
    set(INFINIFUSION_BUILD_LUA OFF CACHE BOOL "Lua is available only in Development" FORCE)
    set(INFINIFUSION_CONFIG_NAME infinifusion.ini)
else()
    set(INFINIFUSION_CONFIG_NAME infinifusion.ini)
endif()

option(INFINIFUSION_BUILD_RUNTIME "Build the Windows winmm runtime" ON)
option(INFINIFUSION_BUILD_API "Build the experimental native API" ON)
option(INFINIFUSION_BUILD_LUA "Build the Lua mod adapter (requires API)" ON)

if(INFINIFUSION_BUILD_RUNTIME AND NOT INFINIFUSION_BUILD_API)
    message(FATAL_ERROR "The standalone runtime requires the shared API; the legacy executor has been removed. Use BUILD_RUNTIME=OFF for portable-only builds.")
endif()

if(INFINIFUSION_BUILD_API)
    add_library(infinifusion_core STATIC
        src/core/value.cpp src/core/provider.cpp src/core/engine.cpp src/core/tbcr_patch.cpp src/core/table_patch.cpp)
    target_compile_features(infinifusion_core PUBLIC cxx_std_20)
    target_include_directories(infinifusion_core PUBLIC src)
    add_library(infinifusion_property_text STATIC src/unreal/property_text.cpp src/unreal/property_import.cpp)
    target_compile_features(infinifusion_property_text PUBLIC cxx_std_20)
    target_include_directories(infinifusion_property_text PUBLIC src)
    target_link_libraries(infinifusion_property_text PUBLIC infinifusion_core)
    add_library(infinifusion_tbcr STATIC src/formats/tbcr.cpp src/formats/tbcr_schema.cpp)
    target_compile_features(infinifusion_tbcr PUBLIC cxx_std_20)
    target_include_directories(infinifusion_tbcr PUBLIC src)
    target_link_libraries(infinifusion_tbcr PUBLIC infinifusion_core)
    add_library(infinifusion_host STATIC src/host/api_host.cpp src/host/client_session.cpp src/host/tbcr_service.cpp src/formats/json.cpp)
    target_compile_features(infinifusion_host PUBLIC cxx_std_20)
    target_include_directories(infinifusion_host PUBLIC src include)
    target_link_libraries(infinifusion_host PUBLIC infinifusion_core infinifusion_tbcr)
    add_library(infinifusion_json_mods STATIC src/adapters/json/json_mods.cpp src/adapters/json/property_values.cpp)
    target_compile_features(infinifusion_json_mods PUBLIC cxx_std_20)
    target_include_directories(infinifusion_json_mods PUBLIC src include)
    target_link_libraries(infinifusion_json_mods PUBLIC infinifusion_property_text infinifusion_host)
    if(MSVC)
        foreach(target infinifusion_core infinifusion_tbcr infinifusion_host infinifusion_json_mods infinifusion_property_text)
            # runtime fault handler. Native SEH must not strand the host mutex.
            target_compile_options(${target} PRIVATE /W4 /permissive- /EHa /utf-8)
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endforeach()
    endif()
endif()

if(INFINIFUSION_BUILD_API AND INFINIFUSION_BUILD_LUA)
    include(cmake/Lua.cmake)
    add_library(infinifusion_lua_clients STATIC src/host/lua_client.cpp)
    target_compile_features(infinifusion_lua_clients PUBLIC cxx_std_20)
    target_link_libraries(infinifusion_lua_clients PUBLIC infinifusion_host infinifusion_lua)
    set_property(TARGET infinifusion_lua_clients PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(INFINIFUSION_BUILD_RUNTIME)
if(NOT MSVC)
    message(FATAL_ERROR "Infinifusion requires the Microsoft Visual C++ toolchain.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Infinifusion is an x64-only winmm.dll proxy.")
endif()

enable_language(ASM_MASM)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap/winmm.def" INFINIFUSION_EXPORTS)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/winmm.exports.def" CONTENT "${INFINIFUSION_EXPORTS}")

add_library(infinifusion_winmm_proxy SHARED
    src/runtime/runtime.cpp
    src/formats/json.hpp
    src/bootstrap/proxy.cpp
    src/bootstrap/proxy_exports.inc
    src/bootstrap/proxy_stubs.asm
    "${CMAKE_CURRENT_BINARY_DIR}/winmm.exports.def"
)

set_source_files_properties(src/bootstrap/proxy_exports.inc PROPERTIES
    HEADER_FILE_ONLY TRUE
)

target_compile_features(infinifusion_winmm_proxy PRIVATE cxx_std_20)
set_target_properties(infinifusion_winmm_proxy PROPERTIES
    CXX_EXTENSIONS OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    OUTPUT_NAME "winmm"
    PDB_NAME "winmm"
    PREFIX ""
)

target_include_directories(infinifusion_winmm_proxy PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(infinifusion_winmm_proxy PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    $<$<STREQUAL:${INFINIFUSION_PROFILE},Development>:INFINIFUSION_DEVELOPMENT=1>
)

target_compile_options(infinifusion_winmm_proxy PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->
    $<$<COMPILE_LANGUAGE:CXX>:/EHa>
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:/Zi>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:RelWithDebInfo>>:/Zi>
)

target_link_options(infinifusion_winmm_proxy PRIVATE
    /PDBALTPATH:winmm.pdb
    $<$<CONFIG:Release>:/DEBUG:FULL>
    $<$<CONFIG:Release>:/INCREMENTAL:NO>
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
    $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
    $<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>
    $<$<CONFIG:RelWithDebInfo>:/OPT:REF>
    $<$<CONFIG:RelWithDebInfo>:/OPT:ICF>
)

target_link_libraries(infinifusion_winmm_proxy PRIVATE
    kernel32
    psapi
    version
)
if(INFINIFUSION_BUILD_API)
    target_link_libraries(infinifusion_winmm_proxy PRIVATE infinifusion_host infinifusion_json_mods)
    target_compile_definitions(infinifusion_winmm_proxy PRIVATE INFINIFUSION_NATIVE_API=1)
    if(INFINIFUSION_BUILD_LUA)
        target_link_libraries(infinifusion_winmm_proxy PRIVATE infinifusion_lua_clients)
        target_compile_definitions(infinifusion_winmm_proxy PRIVATE INFINIFUSION_LUA=1)
    endif()
endif()

set(INFINIFUSION_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/config/${INFINIFUSION_CONFIG_NAME}"
)
add_custom_command(TARGET infinifusion_winmm_proxy POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${INFINIFUSION_CONFIG}"
        "$<TARGET_FILE_DIR:infinifusion_winmm_proxy>/infinifusion.ini"
    VERBATIM
)

install(TARGETS infinifusion_winmm_proxy
    RUNTIME DESTINATION .
)
install(FILES "${INFINIFUSION_CONFIG}"
    DESTINATION .
    RENAME infinifusion.ini
)
endif()
