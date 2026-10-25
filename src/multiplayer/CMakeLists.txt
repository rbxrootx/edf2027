get_filename_component(_edf_port_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Keep the gameplay transport on ENet and use libsodium only for cryptographic
# primitives. The Windows private build carries the official upstream runtime;
# Linux self-host builds link the distribution-provided library.
if(WIN32)
    set(EDF2027_SODIUM_ROOT "${_edf_port_source_root}/third_party/libsodium" CACHE PATH
        "Root of the bundled libsodium Windows package")
    set(_edf_sodium_include "${EDF2027_SODIUM_ROOT}/include")
    set(_edf_sodium_implib "${EDF2027_SODIUM_ROOT}/lib/libsodium.lib")
    set(_edf_sodium_dll "${EDF2027_SODIUM_ROOT}/bin/libsodium.dll")
    foreach(_edf_sodium_file IN ITEMS
            "${_edf_sodium_include}/sodium.h" "${_edf_sodium_implib}" "${_edf_sodium_dll}")
        if(NOT EXISTS "${_edf_sodium_file}")
            message(FATAL_ERROR "Bundled libsodium file is missing: ${_edf_sodium_file}")
        endif()
    endforeach()
    add_library(edf2027_sodium SHARED IMPORTED GLOBAL)
    set_target_properties(edf2027_sodium PROPERTIES
        IMPORTED_IMPLIB "${_edf_sodium_implib}"
        IMPORTED_LOCATION "${_edf_sodium_dll}"
        INTERFACE_INCLUDE_DIRECTORIES "${_edf_sodium_include}")
else()
    find_path(EDF2027_SODIUM_INCLUDE_DIR sodium.h REQUIRED)
    find_library(EDF2027_SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
    add_library(edf2027_sodium UNKNOWN IMPORTED GLOBAL)
    set_target_properties(edf2027_sodium PROPERTIES
        IMPORTED_LOCATION "${EDF2027_SODIUM_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${EDF2027_SODIUM_INCLUDE_DIR}")
endif()

file(GLOB_RECURSE _edf_port_fingerprint_sources CONFIGURE_DEPENDS
    "${_edf_port_source_root}/src/*.c"
    "${_edf_port_source_root}/src/*.cc"
    "${_edf_port_source_root}/src/*.cpp"
    "${_edf_port_source_root}/src/*.cxx"
    "${_edf_port_source_root}/src/*.h"
    "${_edf_port_source_root}/src/*.hpp"
    "${_edf_port_source_root}/src/*.inl"
    "${_edf_port_source_root}/generated/*.cmake"
    "${_edf_port_source_root}/generated/*.json"
    "${_edf_port_source_root}/cmake/*.cmake"
    "${_edf_port_source_root}/third_party/enet/*.c"
    "${_edf_port_source_root}/third_party/enet/*.cc"
    "${_edf_port_source_root}/third_party/enet/*.cpp"
    "${_edf_port_source_root}/third_party/enet/*.h"
    "${_edf_port_source_root}/third_party/enet/*.hpp"
    "${_edf_port_source_root}/third_party/luau/*.c"
    "${_edf_port_source_root}/third_party/luau/*.cc"
    "${_edf_port_source_root}/third_party/luau/*.cpp"
    "${_edf_port_source_root}/third_party/luau/*.h"
    "${_edf_port_source_root}/third_party/luau/*.hpp"
    "${_edf_port_source_root}/third_party/luau/*.cmake"
    "${_edf_port_source_root}/third_party/libsodium/include/*.h"
    "${_edf_port_source_root}/generated/default/*.cpp"
    "${_edf_port_source_root}/generated/default/*.h"
    "${_edf_port_source_root}/generated/default/*.hpp"
    "${_edf_port_source_root}/third_party/enet/CMakeLists.txt"
    "${_edf_port_source_root}/third_party/luau/CMakeLists.txt"
    "${_edf_port_source_root}/third_party/luau/fuzz/CMakeLists.txt"
    "${_edf_port_source_root}/third_party/luau/Sources.cmake")
# Ignore nested build outputs accidentally found by the recursive source glob.
# They are not port inputs and can contain compiler probes with paths that are
# invalid to Ninja on Windows.
list(FILTER _edf_port_fingerprint_sources EXCLUDE REGEX "/(out|build|builds|CMakeFiles|_deps)/")
if(WIN32)
    list(APPEND _edf_port_fingerprint_sources "${_edf_sodium_implib}" "${_edf_sodium_dll}")
endif()
file(GLOB _edf_port_fingerprint_manifests CONFIGURE_DEPENDS
    "${_edf_port_source_root}/edf2017_*.toml")
list(APPEND _edf_port_fingerprint_sources
    "${_edf_port_source_root}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    ${_edf_port_fingerprint_manifests}
    "${CMAKE_CURRENT_SOURCE_DIR}/write_port_build_fingerprint.cmake")
list(REMOVE_DUPLICATES _edf_port_fingerprint_sources)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_edf_port_fingerprint_sources})
set(_edf_port_fingerprint_header "${CMAKE_CURRENT_BINARY_DIR}/generated/edf_port_build_fingerprint.h")
add_custom_command(
    OUTPUT "${_edf_port_fingerprint_header}"
    COMMAND "${CMAKE_COMMAND}"
        "-DEDF_PORT_SOURCE_ROOT=${_edf_port_source_root}"
        "-DEDF_PORT_FINGERPRINT_OUTPUT=${_edf_port_fingerprint_header}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/write_port_build_fingerprint.cmake"
    DEPENDS ${_edf_port_fingerprint_sources}
    COMMENT "Computing EDF port source/build-input fingerprint"
    VERBATIM)
add_custom_target(edf_port_build_fingerprint DEPENDS "${_edf_port_fingerprint_header}")

add_library(edf_multiplayer_protocol STATIC protocol.cpp)
target_include_directories(edf_multiplayer_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(edf_multiplayer_protocol PUBLIC cxx_std_23)
add_dependencies(edf_multiplayer_protocol edf_port_build_fingerprint)

add_library(edf_multiplayer_discovery STATIC discovery.cpp)
target_include_directories(edf_multiplayer_discovery PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edf_multiplayer_discovery PUBLIC edf_multiplayer_protocol)
target_compile_features(edf_multiplayer_discovery PUBLIC cxx_std_23)
add_dependencies(edf_multiplayer_discovery edf_port_build_fingerprint)
if(WIN32)
    target_link_libraries(edf_multiplayer_discovery PUBLIC ws2_32)
endif()

add_library(edf_multiplayer_transport STATIC enet_lifecycle.cpp)
target_include_directories(edf_multiplayer_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edf_multiplayer_transport PUBLIC enet)
target_compile_features(edf_multiplayer_transport PUBLIC cxx_std_23)

add_library(edf_multiplayer_secure STATIC secure_channel.cpp)
target_include_directories(edf_multiplayer_secure PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edf_multiplayer_secure PUBLIC edf_multiplayer_protocol edf2027_sodium)
target_compile_features(edf_multiplayer_secure PUBLIC cxx_std_23)
add_dependencies(edf_multiplayer_secure edf_port_build_fingerprint)

add_library(edf_multiplayer_resolver STATIC resolve_ipv4.cpp)
target_include_directories(edf_multiplayer_resolver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edf_multiplayer_resolver PUBLIC enet)
target_compile_features(edf_multiplayer_resolver PUBLIC cxx_std_23)
add_dependencies(edf_multiplayer_resolver edf_port_build_fingerprint)
if(WIN32)
    target_link_libraries(edf_multiplayer_resolver PUBLIC ws2_32)
endif()

add_library(edf_multiplayer_client STATIC client_session.cpp client_input.cpp directory_client.cpp)
target_include_directories(edf_multiplayer_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edf_multiplayer_client PUBLIC edf_multiplayer_protocol edf_multiplayer_discovery edf_multiplayer_transport edf_multiplayer_secure edf_multiplayer_resolver enet)
target_compile_features(edf_multiplayer_client PUBLIC cxx_std_23)
add_dependencies(edf_multiplayer_client edf_port_build_fingerprint)

add_library(edf_multiplayer_server_core STATIC server_world.cpp script_runtime.cpp server_runtime.cpp master_runtime.cpp)
target_include_directories(edf_multiplayer_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edf_multiplayer_server_core PUBLIC edf_multiplayer_protocol edf_multiplayer_discovery edf_multiplayer_transport edf_multiplayer_secure edf_multiplayer_resolver enet Luau.Compiler Luau.VM)
target_compile_features(edf_multiplayer_server_core PUBLIC cxx_std_23)
add_dependencies(edf_multiplayer_server_core edf_port_build_fingerprint)

add_executable(edf2027_server server_main.cpp)
target_link_libraries(edf2027_server PRIVATE edf_multiplayer_server_core)
target_compile_features(edf2027_server PRIVATE cxx_std_23)
add_dependencies(edf2027_server edf_port_build_fingerprint)
if(WIN32)
    target_link_libraries(edf2027_server PRIVATE ws2_32)
endif()

install(TARGETS edf2027_server RUNTIME DESTINATION .)
add_executable(edf2027_master master_main.cpp)
target_link_libraries(edf2027_master PRIVATE edf_multiplayer_server_core)
target_compile_features(edf2027_master PRIVATE cxx_std_23)
add_dependencies(edf2027_master edf_port_build_fingerprint)
install(TARGETS edf2027_master RUNTIME DESTINATION .)

if(WIN32)
    function(edf2027_stage_sodium target)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_edf_sodium_dll}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endfunction()
    edf2027_stage_sodium(edf2027_server)
    edf2027_stage_sodium(edf2027_master)
    install(FILES "${_edf_sodium_dll}" DESTINATION .)
endif()

if(WIN32)
    # Windows headers otherwise define min/max macros that break std::min/max
    # in the independent browser, client, and standalone-server translation units.
    foreach(_edf_multiplayer_target IN ITEMS
            edf_multiplayer_protocol edf_multiplayer_discovery edf_multiplayer_transport edf_multiplayer_secure
            edf_multiplayer_client edf_multiplayer_server_core edf2027_server edf2027_master)
        target_compile_definitions(${_edf_multiplayer_target} PRIVATE NOMINMAX)
    endforeach()
endif()

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(edf_multiplayer_tests ../../tests/multiplayer_tests.cpp)
    target_link_libraries(edf_multiplayer_tests PRIVATE edf_multiplayer_server_core edf_multiplayer_client)
    target_compile_features(edf_multiplayer_tests PRIVATE cxx_std_23)
    add_dependencies(edf_multiplayer_tests edf_port_build_fingerprint)
    add_test(NAME edf_multiplayer_tests COMMAND edf_multiplayer_tests)
    set_tests_properties(edf_multiplayer_tests PROPERTIES
        ENVIRONMENT "LOCALAPPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-localappdata")
    add_dependencies(edf_multiplayer_tests edf2027_server)
    add_test(NAME edf_multiplayer_loopback
        COMMAND ${Python3_EXECUTABLE} "${_edf_port_source_root}/tools/run-multiplayer-loopback.py"
                --server "$<TARGET_FILE:edf2027_server>"
                --client-test "$<TARGET_FILE:edf_multiplayer_tests>"
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/loopback")
    set_tests_properties(edf_multiplayer_loopback PROPERTIES TIMEOUT 45)
    if(WIN32)
        target_compile_definitions(edf_multiplayer_tests PRIVATE NOMINMAX)
        edf2027_stage_sodium(edf_multiplayer_tests)
    endif()
endif()
