# EDF2027 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(edf2027 LANGUAGES CXX)

include(CTest)

# The release version: the one place it lives. The game shows it (with the git commit,
# tools/write-version.cmake) in its log and F1 menu, and package_release names the zip
# after it.
set(EDF2027_VERSION 0.3.38-beta)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A cached, empty CMAKE_CXX_FLAGS_RELEASE once built this whole game at -O0:
# CMake seeds the per-config flags only when the cache entry is missing, and
# an explicitly empty entry survives every reconfigure. Refuse to configure an
# optimized config whose flags carry no optimization level.
option(EDF2027_ALLOW_UNOPTIMIZED_RELEASE "Allow Release/RelWithDebInfo flags without -O" OFF)
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$" AND NOT EDF2027_ALLOW_UNOPTIMIZED_RELEASE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _edf_config)
    if(NOT " ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_edf_config}} " MATCHES " [-/]O[1-3sxz2]? ")
        message(FATAL_ERROR "CMAKE_CXX_FLAGS_${_edf_config} ('${CMAKE_CXX_FLAGS_${_edf_config}}') "
            "has no optimization level, so this ${CMAKE_BUILD_TYPE} build would compile at -O0. "
            "Reconfigure with -DCMAKE_CXX_FLAGS_${_edf_config}=\"-O2 -DNDEBUG\" (the preset does this), "
            "or pass -DEDF2027_ALLOW_UNOPTIMIZED_RELEASE=ON on purpose.")
    endif()
endif()

option(EDF2027_NATIVE_GRAPHICS_ONLY "Build native graphics tools without ReXGlue or Xenos" OFF)
option(EDF2027_HEADLESS_SERVER_ONLY "Build only the standalone multiplayer server tools" OFF)
option(EDF2027_BUILD_HEADLESS_GUEST_PROBE "Build an opt-in ReX headless guest simulation probe" OFF)
option(EDF2027_SKIP_GUEST_PROBE_INSTRUMENTATION
    "Build the headless guest probe without generated dispatch tracing (for stock ReXGlue codegen)" OFF)

# The headless multiplayer service is independent of the ReXGlue runtime and the game
# renderer, so it remains buildable on its own and is also available to dedicated hosts.
option(EDF2027_BUILD_MULTIPLAYER "Build the multiplayer runtime and dedicated server" ON)
if(EDF2027_BUILD_MULTIPLAYER)
    set(LUAU_BUILD_CLI OFF CACHE BOOL "Do not build Luau command-line tools" FORCE)
    set(LUAU_BUILD_TESTS OFF CACHE BOOL "Do not build Luau's upstream test suite" FORCE)
    set(LUAU_BUILD_WEB OFF CACHE BOOL "Do not build Luau's Web module" FORCE)
    add_subdirectory(third_party/enet EXCLUDE_FROM_ALL)
    add_subdirectory(third_party/luau EXCLUDE_FROM_ALL)
    add_subdirectory(src/multiplayer)
endif()

if(EDF2027_HEADLESS_SERVER_ONLY)
    if(NOT EDF2027_BUILD_MULTIPLAYER)
        message(FATAL_ERROR "EDF2027_HEADLESS_SERVER_ONLY requires EDF2027_BUILD_MULTIPLAYER=ON")
    endif()
    if(EDF2027_BUILD_HEADLESS_GUEST_PROBE)
        message(FATAL_ERROR "EDF2027_BUILD_HEADLESS_GUEST_PROBE requires the ReXGlue game build; disable EDF2027_HEADLESS_SERVER_ONLY")
    endif()
    return()
endif()

add_subdirectory(src/native_graphics)
if(EDF2027_NATIVE_GRAPHICS_ONLY)
    if(EDF2027_BUILD_HEADLESS_GUEST_PROBE)
        message(FATAL_ERROR "EDF2027_BUILD_HEADLESS_GUEST_PROBE requires the full ReXGlue game build; disable EDF2027_NATIVE_GRAPHICS_ONLY")
    endif()
    return()
endif()

if(NOT WIN32)
    message(FATAL_ERROR "The native game renderer currently requires Windows/D3D12. Native analysis tools remain available with EDF2027_NATIVE_GRAPHICS_ONLY=ON.")
endif()

include(generated/rexglue.cmake)

# The multiplayer fingerprint includes generated guest sources. Run codegen before
# producing that header, then let every target that includes protocol.h wait for it.
if(TARGET edf_port_build_fingerprint AND TARGET edf2017_codegen)
    add_dependencies(edf_port_build_fingerprint edf2017_codegen)
endif()

# Guest codegen: the local ReXGlue codegen fork (docs/codegen-fork.md). The game keeps
# linking the prebuilt SDK runtime; only the codegen tool differs. When
# edf2017_codegen_fork.toml turns fork options on, the codegen step must run the fork at
# exactly the pinned version: a stock rexglue.exe ignores those options and would silently
# regenerate the old code, so configure fails instead.
set(EDF_CODEGEN_FORK_VERSION "edf-codegen-3")  # what `rexglue --edf-fork-version` must print
set(_edf_fork_default "")
if(DEFINED ENV{EDF_REXGLUE_CODEGEN_EXE})
    set(_edf_fork_default "$ENV{EDF_REXGLUE_CODEGEN_EXE}")
elseif(EXISTS "D:/roms2/rexglue-sdk-edf/out/win-amd64/Release/rexglue.exe")
    set(_edf_fork_default "D:/roms2/rexglue-sdk-edf/out/win-amd64/Release/rexglue.exe")
endif()
set(EDF_REXGLUE_CODEGEN_EXE "${_edf_fork_default}" CACHE FILEPATH
    "rexglue.exe of the codegen fork (edf-codegen branch) used for the codegen step")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/edf2017_codegen_fork.toml" _edf_fork_toml)
string(REGEX MATCH "\n(edf_ipa_locals|edf_ipa_fast_abi|edf_ipa_abi_boundary|edf_ipa_vtables|edf_ipa_audit)[ \t]*=[ \t]*true" _edf_fork_needed "\n${_edf_fork_toml}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/edf2017_codegen_fork.toml")
if(_edf_fork_needed OR EDF_REXGLUE_CODEGEN_EXE)
    if(NOT EDF_REXGLUE_CODEGEN_EXE OR NOT EXISTS "${EDF_REXGLUE_CODEGEN_EXE}")
        message(FATAL_ERROR
            "edf2017_codegen_fork.toml enables codegen-fork options, but the fork's rexglue.exe "
            "was not found ('${EDF_REXGLUE_CODEGEN_EXE}'). Build it (docs/codegen-fork.md, "
            "'Building the fork') and pass -DEDF_REXGLUE_CODEGEN_EXE=<path> or set the "
            "EDF_REXGLUE_CODEGEN_EXE environment variable.")
    endif()
    execute_process(COMMAND "${EDF_REXGLUE_CODEGEN_EXE}" --edf-fork-version
        OUTPUT_VARIABLE _edf_fork_version OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _edf_fork_rc ERROR_QUIET)
    if(NOT _edf_fork_rc EQUAL 0 OR NOT _edf_fork_version STREQUAL EDF_CODEGEN_FORK_VERSION)
        message(FATAL_ERROR
            "${EDF_REXGLUE_CODEGEN_EXE} reports codegen version '${_edf_fork_version}', "
            "this tree needs '${EDF_CODEGEN_FORK_VERSION}'. Rebuild the fork at the matching "
            "edf-codegen commit (docs/codegen-fork.md).")
    endif()
    if(TARGET rex::rexglue)
        foreach(_edf_cfg "" _RELEASE _DEBUG _RELWITHDEBINFO _MINSIZEREL)
            set_target_properties(rex::rexglue PROPERTIES
                IMPORTED_LOCATION${_edf_cfg} "${EDF_REXGLUE_CODEGEN_EXE}")
        endforeach()
    endif()
    # Re-run codegen when the tool itself is rebuilt (the depfile only lists its inputs).
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp"
        DEPENDS "${EDF_REXGLUE_CODEGEN_EXE}"
        APPEND)
    message(STATUS "Codegen: fork ${_edf_fork_version} at ${EDF_REXGLUE_CODEGEN_EXE}")
endif()

# Sources
set(EDF2027_SOURCES
    src/main.cpp
    src/input_hooks.cpp
    src/launcher_cvars.cpp
    src/native_kbm.cpp
    src/manual_reload.cpp
    src/input_latency.cpp
    src/diagnostics.cpp
    src/kernel_tick_timer.cpp
    src/version.cpp
    # In-game console (docs/console.md).
    src/console/console.cpp
    src/console/console_game.cpp
    # Exactness gate for guest codegen changes (docs/codegen-overhead.md).
    src/guest_state_hash.cpp
)

if(WIN32)
    add_library(edf_native_sdk_ui STATIC src/native_graphics/native_immediate_drawer.cpp src/native_graphics/native_backend_immediate_drawer.cpp)
    target_link_libraries(edf_native_sdk_ui PUBLIC edf_native_d3d11 rex::runtime)
    add_executable(edf2027 WIN32 ${EDF2027_SOURCES})
    target_sources(edf2027 PRIVATE src/native_graphics/guest_shader_bridge.cpp)
    # Split out of the bridge (same target, so the same host optimization flags): its cvars,
    # hook timing and bridge locks, and the shared bridge state (State()).
    target_sources(edf2027 PRIVATE src/native_graphics/bridge/native_cvars.cpp
        src/native_graphics/bridge/hook_timing.cpp src/native_graphics/bridge/bridge_state.cpp)
    # The full frame's host and passes, out of the bridge the same way (bridge_helpers.h is what they share).
    target_sources(edf2027 PRIVATE src/native_graphics/edf/full_frame/full_frame_shared.cpp
        src/native_graphics/edf/full_frame/host.cpp src/native_graphics/edf/full_frame/sky_pass.cpp
        src/native_graphics/edf/full_frame/static_world_pass.cpp src/native_graphics/edf/full_frame/models_pass.cpp
        src/native_graphics/edf/full_frame/effects_pass.cpp src/native_graphics/edf/full_frame/transparent_pass.cpp)
    # The hooks, by subsystem, out of the bridge the same way (bridge_shared.h is what they share with it).
    target_sources(edf2027 PRIVATE src/native_graphics/edf/hooks/diagnostics.cpp
        src/native_graphics/edf/hooks/loading.cpp src/native_graphics/edf/hooks/resources.cpp
        src/native_graphics/edf/hooks/hud.cpp src/native_graphics/edf/hooks/effects.cpp
        src/native_graphics/edf/hooks/post.cpp src/native_graphics/edf/hooks/buffers.cpp
        src/native_graphics/edf/hooks/fences.cpp src/native_graphics/edf/hooks/models.cpp
        src/native_graphics/edf/hooks/scene.cpp src/native_graphics/edf/hooks/frame.cpp
        src/native_graphics/edf/hooks/draw.cpp)
    # Skeletal animation (native_skeletal.h): simulation hooks, not renderer.
    target_sources(edf2027 PRIVATE src/native_graphics/edf/hooks/animation.cpp)
    target_sources(edf2027 PRIVATE src/native_graphics/native_fallback_imports.cpp)
    target_sources(edf2027 PRIVATE src/native_graphics/native_full_frame_sky.cpp)
    target_sources(edf2027 PRIVATE src/native_graphics/native_preview_window.cpp)
    target_sources(edf2027 PRIVATE src/native_graphics/native_d3d12_preview.cpp)
    target_sources(edf2027 PRIVATE src/native_graphics/native_host_surface.cpp src/native_graphics/native_backend_host.cpp)
    # Fonts for the F1 menu and the performance overlay, built into the executable.
    set(_edf_menu_fonts ${CMAKE_CURRENT_BINARY_DIR}/edf_menu_fonts.cpp)
    add_custom_command(OUTPUT ${_edf_menu_fonts}
        COMMAND ${CMAKE_COMMAND} -DOUTPUT=${_edf_menu_fonts} -DFONT_DIR=${CMAKE_SOURCE_DIR}/third_party/fonts
            -DHEADER=${CMAKE_SOURCE_DIR}/src/menu_fonts.h -P ${CMAKE_SOURCE_DIR}/tools/embed-fonts.cmake
        DEPENDS third_party/fonts/Inter-Regular.ttf third_party/fonts/Inter-SemiBold.ttf
            third_party/fonts/fa-solid-900.ttf tools/embed-fonts.cmake
        COMMENT "Embedding menu fonts" VERBATIM)
    target_sources(edf2027 PRIVATE ${_edf_menu_fonts})
    target_link_libraries(edf2027 PRIVATE comctl32)
    target_link_libraries(edf2027 PRIVATE gdi32)
    target_link_libraries(edf2027 PRIVATE edf_native_d3d11)
    target_link_libraries(edf2027 PRIVATE edf_native_d3d12)
    target_link_libraries(edf2027 PRIVATE edf_native_sdk_ui)
    # Keep the explicit D3D11 fallback dormant: the default runtime must not
    # load D3D11 merely because its fallback factory is linked into the binary.
    target_link_libraries(edf2027 PRIVATE delayimp)
    target_link_options(edf2027 PRIVATE "LINKER:/DELAYLOAD:d3d11.dll")
else()
    add_executable(edf2027 ${EDF2027_SOURCES})
endif()

if(EDF2027_BUILD_MULTIPLAYER)
    target_compile_definitions(edf2027 PRIVATE EDF2027_BUILD_MULTIPLAYER=1)
    target_link_libraries(edf2027 PRIVATE edf_multiplayer_client edf_multiplayer_server_core)
    add_dependencies(edf2027 edf_port_build_fingerprint)
endif()

# The host owns native D3D12 rendering. Do not request an SDK GPU emulation plugin.
rexglue_setup_target(edf2027)
if(EDF2027_BUILD_HEADLESS_GUEST_PROBE)
    # The generated call sites and their no-op providers are only present in the
    # opt-in diagnostic configuration; ordinary performance builds compile neither.
    if(NOT TARGET edf2027_recomp)
        message(FATAL_ERROR "The generated guest probe instrumentation requires edf2027_recomp")
    endif()
    target_sources(edf2027 PRIVATE src/multiplayer/guest_probe_noop_hooks.cpp)
    if(EDF2027_SKIP_GUEST_PROBE_INSTRUMENTATION)
        message(STATUS "Guest probe dispatch tracing disabled for stock ReXGlue codegen")
    else()
        target_compile_definitions(edf2027_recomp PRIVATE EDF2027_GUEST_PROBE_TRACE=1)
        add_custom_target(edf2017_guest_probe_instrumentation
            COMMAND ${CMAKE_COMMAND}
                "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/generated/default/edf2017_recomp.65.cpp"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/instrument-guest-probe.cmake"
            DEPENDS edf2017_codegen tools/instrument-guest-probe.cmake
            COMMENT "Applying idempotent guest probe instrumentation"
            VERBATIM)
        add_dependencies(edf2027_recomp edf2017_guest_probe_instrumentation)
        # The build fingerprint hashes generated/default/*.cpp. Ensure it sees the
        # final post-codegen instrumentation state rather than racing the patch step.
        if(TARGET edf_port_build_fingerprint)
            add_dependencies(edf_port_build_fingerprint edf2017_guest_probe_instrumentation)
        endif()
    endif()
endif()
if(WIN32 AND EDF2027_BUILD_MULTIPLAYER)
    add_custom_command(TARGET edf2027 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:edf2027_sodium>" "$<TARGET_FILE_DIR:edf2027>"
        VERBATIM)
endif()

# This probe deliberately links the same generated EDF guest code while bypassing
# ReXApp and the native renderer. It verifies whether the real guest simulation
# dispatcher advances with ReXGlue's tool_mode (GPU initialization skipped).
if(EDF2027_BUILD_HEADLESS_GUEST_PROBE)
    add_executable(edf2027_headless_probe
        src/multiplayer/guest_probe_main.cpp
        src/multiplayer/guest_probe_hooks.cpp
        src/kernel_tick_timer.cpp)
    target_include_directories(edf2027_headless_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${REXGLUE_ENTRYPOINT_INCLUDE_DIR})
    target_compile_definitions(edf2027_headless_probe PRIVATE EDF2027_HEADLESS_GUEST=1)
    target_link_libraries(edf2027_headless_probe PRIVATE edf2027_recomp rex::runtime)
    if(TARGET edf_multiplayer_protocol)
        target_link_libraries(edf2027_headless_probe PRIVATE edf_multiplayer_protocol)
    endif()
    target_compile_features(edf2027_headless_probe PRIVATE cxx_std_23)
    rexglue_apply_target_settings(edf2027_headless_probe)
    add_dependencies(edf2027_headless_probe edf2017_codegen)
    if(TARGET edf_port_build_fingerprint)
        add_dependencies(edf2027_headless_probe edf_port_build_fingerprint)
    endif()

    # Match the runtime DLL staging normally supplied by rexglue_configure_target,
    # without adding its windowed entry point or ReXApp implementation.
    if(WIN32)
        add_custom_command(TARGET edf2027_headless_probe POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:edf2027_headless_probe>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:edf2027_headless_probe>;$<TARGET_FILE_DIR:edf2027_headless_probe>>"
            COMMAND_EXPAND_LISTS
            VERBATIM)
        foreach(_edf_probe_fx amd_fidelityfx_vk amd_fidelityfx_dx12)
            if(TARGET ${_edf_probe_fx})
                add_custom_command(TARGET edf2027_headless_probe POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:${_edf_probe_fx}>
                        $<TARGET_FILE_DIR:edf2027_headless_probe>
                    VERBATIM)
            endif()
        endforeach()
    endif()

    # Opt-in bridge host: the same real guest probe plus the self-hosted ENet
    # service. The independent edf2027_server target remains available when no
    # extracted game data or ReX runtime is present.
    if(EDF2027_BUILD_MULTIPLAYER)
        add_executable(edf2027_authoritative_host
            src/multiplayer/guest_probe_main.cpp
            src/multiplayer/guest_probe_hooks.cpp
            src/kernel_tick_timer.cpp)
        target_compile_definitions(edf2027_authoritative_host PRIVATE EDF2027_AUTHORITATIVE_HOST=1)
        target_compile_definitions(edf2027_authoritative_host PRIVATE EDF2027_HEADLESS_GUEST=1)
        target_include_directories(edf2027_authoritative_host PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${REXGLUE_ENTRYPOINT_INCLUDE_DIR})
        target_link_libraries(edf2027_authoritative_host PRIVATE
            edf2027_recomp rex::runtime edf_multiplayer_server_core)
        target_compile_features(edf2027_authoritative_host PRIVATE cxx_std_23)
        rexglue_apply_target_settings(edf2027_authoritative_host)
        add_dependencies(edf2027_authoritative_host edf2017_codegen)
        add_dependencies(edf2027_authoritative_host edf_port_build_fingerprint)
        install(TARGETS edf2027_authoritative_host RUNTIME DESTINATION .)

        if(WIN32)
            add_custom_command(TARGET edf2027_authoritative_host POST_BUILD
                COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:edf2027_authoritative_host>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:edf2027_authoritative_host>;$<TARGET_FILE_DIR:edf2027_authoritative_host>>"
                COMMAND_EXPAND_LISTS
                VERBATIM)
            add_custom_command(TARGET edf2027_authoritative_host POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:edf2027_sodium>" "$<TARGET_FILE_DIR:edf2027_authoritative_host>"
                VERBATIM)
            foreach(_edf_host_fx amd_fidelityfx_vk amd_fidelityfx_dx12)
                if(TARGET ${_edf_host_fx})
                    add_custom_command(TARGET edf2027_authoritative_host POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            $<TARGET_FILE:${_edf_host_fx}>
                            $<TARGET_FILE_DIR:edf2027_authoritative_host>
                        VERBATIM)
                endif()
            endforeach()
        endif()
    endif()
endif()

# Build identity (src/version.cpp): regenerated on every build, rewritten only when the
# commit or the dirty flag changes, and seen only by version.cpp.
set(_edf_version_dir ${CMAKE_CURRENT_BINARY_DIR}/edf_version)
add_custom_target(edf_version_info
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_edf_version_dir}/edf_version_info.h
        -DVERSION=${EDF2027_VERSION} -P ${CMAKE_SOURCE_DIR}/tools/write-version.cmake
    BYPRODUCTS ${_edf_version_dir}/edf_version_info.h
    COMMENT "Build identity ${EDF2027_VERSION}"
    VERBATIM)
add_dependencies(edf2027 edf_version_info)
set_source_files_properties(src/version.cpp PROPERTIES INCLUDE_DIRECTORIES ${_edf_version_dir})
if(WIN32)
    # Crash dumps (src/diagnostics.cpp).
    target_link_libraries(edf2027 PRIVATE dbghelp)
endif()

# A PDB for the shipped executable, so a player's crash dump can be symbolized: line
# tables for the host code, public symbols (every sub_XXXXXXXX) for the recompiled code.
# Debug information does not change the generated code; /OPT:REF,ICF stay as without
# /DEBUG, and /PDBALTPATH keeps the build machine's paths out of the executable. The PDB
# ships separately (package_release's -symbols zip).
option(EDF2027_RELEASE_PDB "Write edf2027.pdb for optimized Windows builds" ON)
if(WIN32 AND EDF2027_RELEASE_PDB AND CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
    foreach(_edf_pdb_target edf2027 edf_native_d3d12 edf_native_d3d11 edf_native_effects edf_native_sdk_ui)
        if(TARGET ${_edf_pdb_target})
            target_compile_options(${_edf_pdb_target} PRIVATE -gcodeview -gline-tables-only)
        endif()
    endforeach()
    target_link_options(edf2027 PRIVATE "LINKER:/DEBUG" "LINKER:/OPT:REF" "LINKER:/OPT:ICF" "LINKER:/PDBALTPATH:%_PDB%")
endif()

# Allow an independent candidate while the default executable is being playtested.
set(EDF2027_EXECUTABLE_NAME "edf2027" CACHE STRING "Game executable basename (no directory or extension required)")
if(NOT EDF2027_EXECUTABLE_NAME MATCHES "^[A-Za-z0-9][A-Za-z0-9_.-]*$")
    message(FATAL_ERROR "EDF2027_EXECUTABLE_NAME must be a simple executable basename")
endif()
set_target_properties(edf2027 PROPERTIES OUTPUT_NAME "${EDF2027_EXECUTABLE_NAME}")

# Opt-in -O3 / x86-64-v3 / ThinLTO / PGO experiments; all off by default.
include(cmake/edf_optimization.cmake)

# Start-up CPU check (src/cpu_check_entry.cpp): the executable's entry point,
# which runs before the CRT and every static initializer and, in an
# x86-64-v3 build, shows a message box instead of crashing on an older CPU.
# An object library of its own, so it never gets the v3 target options that
# edf_optimization.cmake gives edf2027; the source #errors if it does.
if(WIN32)
    add_library(edf_cpu_check OBJECT src/cpu_check_entry.cpp src/cpu_check_logic.cpp)
    if(EDF_GUEST_OPT_PROFILE MATCHES "v3" OR EDF_HOST_OPT_PROFILE MATCHES "v3")
        set(_edf_require_v3 1)
    else()
        set(_edf_require_v3 0)
    endif()
    target_compile_definitions(edf_cpu_check PRIVATE EDF_CPU_CHECK_REQUIRE_X86_64_V3=${_edf_require_v3})
    target_link_libraries(edf_cpu_check PUBLIC user32)
    target_link_libraries(edf2027 PRIVATE edf_cpu_check)
    target_link_options(edf2027 PRIVATE "LINKER:/ENTRY:edf_cpu_check_entry")
endif()

# Read-only import/staging check, including transitive non-system dependencies.
# Run from the VS developer environment so CMake can use dumpbin.
add_custom_target(audit_native_dependencies
    COMMAND ${CMAKE_COMMAND} -DEXECUTABLE=$<TARGET_FILE:edf2027>
        -P ${CMAKE_SOURCE_DIR}/tools/audit-native-dependencies.cmake
    DEPENDS edf2027
    COMMENT "Auditing native game's packaged PE dependency closure"
    VERBATIM)

# Route direct and registered file-read/word-fill imports through native
# producer adapters. Keep this private to generated code: each adapter
# itself calls the SDK's original symbol without recursion. Do not edit codegen.
target_compile_definitions(edf2027_recomp PRIVATE __imp__NtReadFile=edf_native_NtReadFile)
target_compile_definitions(edf2027_recomp PRIVATE __imp__RtlFillMemoryUlong=edf_native_RtlFillMemoryUlong)
option(EDF2027_OBSERVE_SCALAR_STORES "Diagnostic generated-store notifications (native providers still need coverage)" OFF)
if(EDF2027_OBSERVE_SCALAR_STORES)
    target_precompile_headers(edf2027_recomp PRIVATE
        "${CMAKE_SOURCE_DIR}/src/native_graphics/guest_raw_store_observer.h")
endif()
# Timing-only codegen overhead experiments (docs/codegen-overhead.md): a
# comma list of NO_VOLATILE, NO_PHYS, NO_FLUSH. Empty (the default) leaves
# the generated code untouched. Not exact; never for a shipped build.
set(EDF_CODEGEN_EXPERIMENT "" CACHE STRING "Timing-only guest codegen experiments, comma list: NO_VOLATILE,NO_PHYS,NO_FLUSH")
if(EDF_CODEGEN_EXPERIMENT)
    string(REPLACE "," ";" _edf_exps "${EDF_CODEGEN_EXPERIMENT}")
    foreach(_edf_exp ${_edf_exps})
        target_compile_definitions(edf2027_recomp PRIVATE EDF_EXP_${_edf_exp}=1)
    endforeach()
    target_precompile_headers(edf2027_recomp PRIVATE
        "${CMAKE_SOURCE_DIR}/src/codegen_experiments/guest_codegen_experiments.h")
    message(WARNING "EDF_CODEGEN_EXPERIMENT=${EDF_CODEGEN_EXPERIMENT}: timing-only, inexact guest code")
endif()
# Differential audit of the codegen fork's locals form (docs/codegen-fork.md). Needs
# edf_ipa_audit = true in edf2017_codegen_fork.toml (the generated wrappers call
# edf_ipa_audit()); journals every guest scalar store, so audit builds only.
option(EDF_CODEGEN_FORK_AUDIT "Codegen fork: link the differential audit and journal guest stores" OFF)
if(EDF_CODEGEN_FORK_AUDIT)
    target_sources(edf2027 PRIVATE src/codegen_fork/ipa_audit.cpp)
    if(EDF2027_BUILD_HEADLESS_GUEST_PROBE)
        target_sources(edf2027_headless_probe PRIVATE src/codegen_fork/ipa_audit.cpp)
        if(TARGET edf2027_authoritative_host)
            target_sources(edf2027_authoritative_host PRIVATE src/codegen_fork/ipa_audit.cpp)
        endif()
    endif()
    target_precompile_headers(edf2027_recomp PRIVATE
        "${CMAKE_SOURCE_DIR}/src/codegen_fork/ipa_audit_pch.h")
    message(WARNING "EDF_CODEGEN_FORK_AUDIT: audit build (journaled guest stores)")
endif()

set(_native_indexed_tail ${CMAKE_CURRENT_SOURCE_DIR}/src/native_graphics/native_indexed_completion.cpp)
target_sources(edf2027 PRIVATE ${_native_indexed_tail})
set(_native_immediate_tail ${CMAKE_CURRENT_BINARY_DIR}/native_immediate_cpu_tail.cpp)
add_custom_command(OUTPUT ${_native_immediate_tail}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_immediate_tail}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-immediate-tail.cmake
    DEPENDS generated/default/edf2017_recomp.42.cpp generated/default/edf2017_recomp.10.cpp tools/extract-native-immediate-tail.cmake tools/native-draw-prefix.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_immediate_tail})
set(_native_derived_tail ${CMAKE_CURRENT_SOURCE_DIR}/src/native_graphics/native_derived_state.cpp)
target_sources(edf2027 PRIVATE ${_native_derived_tail})
set(_native_shader_output_tail ${CMAKE_CURRENT_SOURCE_DIR}/src/native_graphics/native_shader_output.cpp)
target_sources(edf2027 PRIVATE ${_native_shader_output_tail})
set(_native_shader_upload_tail ${CMAKE_CURRENT_SOURCE_DIR}/src/native_graphics/native_shader_upload.cpp)
target_sources(edf2027 PRIVATE ${_native_shader_upload_tail})
set(_native_main_state_tail ${CMAKE_CURRENT_SOURCE_DIR}/src/native_graphics/native_main_state.cpp)
target_sources(edf2027 PRIVATE ${_native_main_state_tail})
set(_native_shader_cache_tail ${CMAKE_CURRENT_SOURCE_DIR}/src/native_graphics/native_shader_cache.cpp)
target_sources(edf2027 PRIVATE ${_native_shader_cache_tail})
set(_native_buffer_unlock ${CMAKE_CURRENT_BINARY_DIR}/native_buffer_unlock.cpp)
add_custom_command(OUTPUT ${_native_buffer_unlock}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_buffer_unlock}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-buffer-unlock.cmake
    DEPENDS generated/default/edf2017_recomp.44.cpp generated/default/edf2017_recomp.71.cpp
        generated/default/edf2017_recomp.16.cpp tools/extract-native-buffer-unlock.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_buffer_unlock})
set(_native_device_reset ${CMAKE_CURRENT_BINARY_DIR}/native_device_reset.cpp)
add_custom_command(OUTPUT ${_native_device_reset}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_device_reset}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-device-reset.cmake
    DEPENDS generated/default/edf2017_recomp.38.cpp generated/default/edf2017_recomp.7.cpp tools/extract-native-device-reset.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_device_reset})
set(_native_cache_range ${CMAKE_CURRENT_BINARY_DIR}/native_cache_range.cpp)
add_custom_command(OUTPUT ${_native_cache_range}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_cache_range}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-cache-range.cmake
    DEPENDS generated/default/edf2017_recomp.4.cpp tools/extract-native-cache-range.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_cache_range})
set(_native_buffer_lock ${CMAKE_CURRENT_BINARY_DIR}/native_buffer_lock.cpp)
add_custom_command(OUTPUT ${_native_buffer_lock}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_buffer_lock}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-buffer-lock.cmake
    DEPENDS generated/default/edf2017_recomp.74.cpp generated/default/edf2017_recomp.24.cpp
        generated/default/edf2017_recomp.76.cpp tools/extract-native-buffer-lock.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_buffer_lock})
set(_native_ring_submit ${CMAKE_CURRENT_BINARY_DIR}/native_ring_submit.cpp)
add_custom_command(OUTPUT ${_native_ring_submit}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_ring_submit}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-ring-submit.cmake
    DEPENDS generated/default/edf2017_recomp.33.cpp generated/default/edf2017_recomp.31.cpp generated/default/edf2017_recomp.19.cpp tools/extract-native-ring-submit.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_ring_submit})
set(_native_worker_audit ${CMAKE_CURRENT_BINARY_DIR}/native_worker_audit.cpp)
add_custom_command(OUTPUT ${_native_worker_audit}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_worker_audit}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-worker-audit.cmake
    DEPENDS generated/default/edf2017_recomp.14.cpp generated/default/edf2017_recomp.73.cpp tools/extract-native-worker-audit.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_worker_audit})
set(_native_worker_init ${CMAKE_CURRENT_BINARY_DIR}/native_worker_init.cpp)
add_custom_command(OUTPUT ${_native_worker_init}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_worker_init}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-worker-init.cmake
    DEPENDS generated/default/edf2017_recomp.51.cpp tools/extract-native-worker-init.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_worker_init})
set(_native_device_defaults ${CMAKE_CURRENT_BINARY_DIR}/native_device_defaults.cpp)
add_custom_command(OUTPUT ${_native_device_defaults}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_device_defaults}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-device-defaults.cmake
    DEPENDS generated/default/edf2017_recomp.19.cpp tools/extract-native-device-defaults.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_device_defaults})
set(_native_present ${CMAKE_CURRENT_BINARY_DIR}/native_present.cpp)
add_custom_command(OUTPUT ${_native_present}
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_present}
        -P ${CMAKE_SOURCE_DIR}/tools/extract-native-present.cmake
    DEPENDS generated/default/edf2017_recomp.26.cpp generated/default/edf2017_recomp.15.cpp
        generated/default/edf2017_recomp.27.cpp generated/default/edf2017_recomp.45.cpp
        generated/default/edf2017_recomp.71.cpp generated/default/edf2017_recomp.54.cpp
        generated/default/edf2017_recomp.66.cpp generated/default/edf2017_recomp.7.cpp
        generated/default/edf2017_recomp.41.cpp generated/default/edf2017_recomp.10.cpp
        generated/default/edf2017_recomp.69.cpp generated/default/edf2017_recomp.32.cpp
        generated/default/edf2017_recomp.40.cpp generated/default/edf2017_recomp.2.cpp
        generated/default/edf2017_recomp.56.cpp generated/default/edf2017_recomp.39.cpp
        generated/default/edf2017_recomp.65.cpp generated/default/edf2017_recomp.8.cpp generated/default/edf2017_recomp.25.cpp
        tools/extract-native-present.cmake tools/extract-native-counter-reset.cmake VERBATIM)
target_sources(edf2027 PRIVATE ${_native_present})

if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_scalar_effect_fixture ${CMAKE_CURRENT_BINARY_DIR}/renderer_scalar_effect_fixture.inc)
    file(GLOB _scalar_effect_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/edf2017_recomp.*.cpp)
    add_custom_command(OUTPUT ${_scalar_effect_fixture}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/renderer_scalar_effects.py --emit-cpp ${_scalar_effect_fixture}
        DEPENDS tools/renderer_scalar_effects.py tools/renderer_scalar_analysis.py
                tests/fixtures/renderer-scalar-pilot.json tests/fixtures/renderer-scalar-boundaries.json
                ${_scalar_effect_sources} VERBATIM)
    add_executable(edf_renderer_scalar_pipeline_tests tests/renderer_scalar_pipeline_tests.cpp ${_scalar_effect_fixture})
    target_include_directories(edf_renderer_scalar_pipeline_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/generated/default)
    target_link_libraries(edf_renderer_scalar_pipeline_tests PRIVATE rex::runtime)
    add_test(NAME edf_renderer_scalar_pipeline_tests COMMAND edf_renderer_scalar_pipeline_tests
        --report ${CMAKE_CURRENT_BINARY_DIR}/renderer_scalar_pipeline_report.json)
    add_custom_target(edf_renderer_scalar_effects_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_scalar_effects
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_renderer_scalar_effects_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_scalar_effects)
    set_tests_properties(edf_renderer_scalar_effects_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set(_scalar_fixture ${CMAKE_CURRENT_BINARY_DIR}/renderer_scalar_original_fixture.inc)
    file(GLOB _scalar_generated_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/edf2017_recomp.*.cpp)
    add_custom_command(OUTPUT ${_scalar_fixture}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/analyze-renderer-scalars.py --emit-cpp ${_scalar_fixture}
        DEPENDS tools/analyze-renderer-scalars.py tools/renderer_scalar_analysis.py
                tests/fixtures/renderer-scalar-pilot.json ${_scalar_generated_sources} VERBATIM)
    add_executable(edf_renderer_scalar_contract_tests tests/renderer_scalar_contract_tests.cpp ${_scalar_fixture})
    target_include_directories(edf_renderer_scalar_contract_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/generated/default)
    target_link_libraries(edf_renderer_scalar_contract_tests PRIVATE rex::runtime)
    add_test(NAME edf_renderer_scalar_contract_tests COMMAND edf_renderer_scalar_contract_tests
        ${CMAKE_CURRENT_BINARY_DIR}/renderer_scalar_contract_report.json)
    add_custom_target(edf_renderer_scalar_analysis_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_scalar_analysis
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_renderer_scalar_analysis_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_scalar_analysis)
    set_tests_properties(edf_renderer_scalar_analysis_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set(_setter_fixture ${CMAKE_CURRENT_BINARY_DIR}/renderer_setter_original_fixture.inc)
    add_custom_command(OUTPUT ${_setter_fixture}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/renderer_setter_analysis.py --emit-cpp ${_setter_fixture}
        DEPENDS tools/renderer_setter_analysis.py tools/renderer_scalar_analysis.py
                tests/fixtures/renderer-scalar-pilot.json tests/fixtures/renderer-setter-pilot.json
                tests/fixtures/renderer-setter-family-v2.json
                ${_scalar_generated_sources} VERBATIM)
    add_executable(edf_renderer_setter_contract_tests tests/renderer_setter_contract_tests.cpp ${_setter_fixture})
    target_include_directories(edf_renderer_setter_contract_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/generated/default)
    target_link_libraries(edf_renderer_setter_contract_tests PRIVATE rex::runtime)
    add_test(NAME edf_renderer_setter_contract_tests COMMAND edf_renderer_setter_contract_tests
        ${CMAKE_CURRENT_BINARY_DIR}/renderer_setter_contract_report.json)
    add_custom_target(edf_renderer_setter_analysis_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_setter_analysis
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_renderer_setter_analysis_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_setter_analysis)
    set_tests_properties(edf_renderer_setter_analysis_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # Offline Python tool tests: synthetic logs/images only, no game or game data.
    add_custom_target(edf_renderer_runtime_gate_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_runtime_gate
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_renderer_runtime_gate_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_runtime_gate)
    set_tests_properties(edf_renderer_runtime_gate_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(edf_frame_time_report_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_frame_time_report
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_frame_time_report_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_frame_time_report)
    set_tests_properties(edf_frame_time_report_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(edf_soak_report_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_soak_report
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_soak_report_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_soak_report)
    set_tests_properties(edf_soak_report_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(edf_coverage_report_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_coverage_report
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_coverage_report_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_coverage_report)
    set_tests_properties(edf_coverage_report_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(edf_renderer_scenario_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_scenarios
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_renderer_scenario_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_renderer_scenarios)
    set_tests_properties(edf_renderer_scenario_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(edf_renderer_image_compare_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_compare_renderer_images
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_renderer_image_compare_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_compare_renderer_images)
    set_tests_properties(edf_renderer_image_compare_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(edf_fsr_quality_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_fsr_quality
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_fsr_quality_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_fsr_quality)
    set_tests_properties(edf_fsr_quality_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(edf_shadow_diff_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_shadow_diff
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_shadow_diff_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_shadow_diff)
    set_tests_properties(edf_shadow_diff_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(edf_renderer_ab_capture_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tools.test_compare_renderer_ab_captures
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_renderer_ab_capture_tests COMMAND ${Python3_EXECUTABLE} -m unittest tools.test_compare_renderer_ab_captures)
    set_tests_properties(edf_renderer_ab_capture_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_custom_target(edf_renderer_ab_postprocess_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tools.test_renderer_ab_postprocess
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_renderer_ab_postprocess_tests COMMAND ${Python3_EXECUTABLE} -m unittest tools.test_renderer_ab_postprocess)
    set_tests_properties(edf_renderer_ab_postprocess_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME edf_native_hook_boundary_audit
      COMMAND ${CMAKE_COMMAND}
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/native_graphics/guest_shader_bridge.cpp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/audit-native-hook-boundaries.cmake)
    add_test(NAME edf_native_cvar_defaults_audit
      COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/audit-native-cvar-defaults.cmake)
    add_executable(edf_native_display_gamma_tests tests/native_display_gamma_tests.cpp)
    set(_worker_callback_fixture ${CMAKE_CURRENT_BINARY_DIR}/native_worker_callback_audit_fixture.inc)
    add_custom_command(OUTPUT ${_worker_callback_fixture}
        COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/native_graphics/edf/hooks/diagnostics.cpp
            -DOUTPUT=${_worker_callback_fixture} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract-worker-callback-audit-test.cmake
        DEPENDS src/native_graphics/edf/hooks/diagnostics.cpp tools/extract-worker-callback-audit-test.cmake VERBATIM)
    add_executable(edf_native_worker_callback_audit_tests tests/native_worker_callback_audit_tests.cpp ${_worker_callback_fixture})
    target_include_directories(edf_native_worker_callback_audit_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME edf_native_worker_callback_audit_tests COMMAND edf_native_worker_callback_audit_tests)
    set(_model_constructor_fixture ${CMAKE_CURRENT_BINARY_DIR}/native_model_constructor_fixture.inc)
    add_custom_command(OUTPUT ${_model_constructor_fixture}
        COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/native_graphics/edf/hooks/models.cpp
            -DOUTPUT=${_model_constructor_fixture} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract-model-constructor-test.cmake
        DEPENDS src/native_graphics/edf/hooks/models.cpp tools/extract-model-constructor-test.cmake VERBATIM)
    add_executable(edf_native_model_constructor_tests tests/native_model_constructor_tests.cpp ${_model_constructor_fixture})
    target_include_directories(edf_native_model_constructor_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME edf_native_model_constructor_tests COMMAND edf_native_model_constructor_tests)
    target_include_directories(edf_native_display_gamma_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME edf_native_display_gamma_tests COMMAND edf_native_display_gamma_tests)
    if(WIN32)
        set(_native_present_fixture ${CMAKE_CURRENT_BINARY_DIR}/native_present_fixture.cpp)
        add_custom_command(OUTPUT ${_native_present_fixture}
            COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_present_fixture}
                -DTEST_MODE=ON -P ${CMAKE_SOURCE_DIR}/tools/extract-native-present.cmake
            DEPENDS generated/default/edf2017_recomp.26.cpp generated/default/edf2017_recomp.58.cpp
                generated/default/edf2017_recomp.6.cpp generated/default/edf2017_recomp.15.cpp
                generated/default/edf2017_recomp.27.cpp generated/default/edf2017_recomp.17.cpp generated/default/edf2017_recomp.25.cpp
                generated/default/edf2017_recomp.45.cpp generated/default/edf2017_recomp.71.cpp
                generated/default/edf2017_recomp.68.cpp generated/default/edf2017_recomp.28.cpp generated/default/edf2017_recomp.54.cpp
                generated/default/edf2017_recomp.66.cpp generated/default/edf2017_recomp.7.cpp generated/default/edf2017_recomp.41.cpp
                generated/default/edf2017_recomp.24.cpp generated/default/edf2017_recomp.34.cpp
                generated/default/edf2017_recomp.42.cpp generated/default/edf2017_recomp.10.cpp
                generated/default/edf2017_recomp.69.cpp generated/default/edf2017_recomp.75.cpp generated/default/edf2017_recomp.13.cpp
                generated/default/edf2017_recomp.32.cpp generated/default/edf2017_recomp.40.cpp
                generated/default/edf2017_recomp.2.cpp generated/default/edf2017_recomp.56.cpp
                generated/default/edf2017_recomp.39.cpp generated/default/edf2017_recomp.65.cpp generated/default/edf2017_recomp.8.cpp
                generated/default/edf2017_recomp.23.cpp tools/extract-native-pix-test.cmake
                tools/extract-native-present.cmake tools/extract-native-counter-reset.cmake VERBATIM)
        add_executable(edf_native_present_tail_tests tests/native_present_tail_tests.cpp ${_native_present_fixture})
        target_include_directories(edf_native_present_tail_tests PRIVATE ${CMAKE_SOURCE_DIR}/generated/default ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(edf_native_present_tail_tests PRIVATE rex::runtime)
        add_test(NAME edf_native_present_tail_tests COMMAND edf_native_present_tail_tests)
        add_executable(edf_native_sdk_ui_tests tests/native_sdk_ui_tests.cpp)
        set(_native_tail_fixture ${CMAKE_CURRENT_BINARY_DIR}/native_immediate_tail_fixture.cpp)
        set(_native_tail_shards ${CMAKE_SOURCE_DIR}/generated/default/edf2017_recomp.74.cpp ${CMAKE_SOURCE_DIR}/generated/default/edf2017_recomp.46.cpp)
        foreach(_shard 42 10 12 17 25 50 48 7 16 15 75 13 31 4 22 14 20 72 73 58 6 54 66 65 34 24 5 32 38 39 68 28 62 11 52 33 3 29 19 51 44 71)
            list(APPEND _native_tail_shards ${CMAKE_SOURCE_DIR}/generated/default/edf2017_recomp.${_shard}.cpp)
        endforeach()
        add_custom_command(OUTPUT ${_native_tail_fixture}
            COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${_native_tail_fixture}
                -P ${CMAKE_SOURCE_DIR}/tools/extract-native-tail-test.cmake
            DEPENDS ${_native_tail_shards} tools/extract-native-tail-test.cmake src/native_graphics/edf/hooks/buffers.cpp src/native_graphics/edf/hooks/draw.cpp src/native_graphics/edf/hooks/models.cpp VERBATIM)
        add_executable(edf_native_immediate_tail_tests tests/native_immediate_tail_tests.cpp ${_native_tail_fixture} ${_native_indexed_tail} ${_native_derived_tail} ${_native_shader_output_tail} ${_native_shader_upload_tail} ${_native_main_state_tail} ${_native_device_reset} ${_native_cache_range} ${_native_ring_submit})
        target_sources(edf_native_immediate_tail_tests PRIVATE ${_native_immediate_tail})
        target_sources(edf_native_immediate_tail_tests PRIVATE ${_native_shader_cache_tail})
        target_sources(edf_native_immediate_tail_tests PRIVATE ${_native_buffer_unlock})
        target_include_directories(edf_native_immediate_tail_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/generated/default)
        target_sources(edf_native_immediate_tail_tests PRIVATE ${_native_buffer_lock})
        target_sources(edf_native_immediate_tail_tests PRIVATE ${_native_worker_init})
        target_sources(edf_native_immediate_tail_tests PRIVATE ${_native_device_defaults})
        target_sources(edf_native_immediate_tail_tests PRIVATE tests/native_static_group_gpu.cpp)
        target_link_libraries(edf_native_immediate_tail_tests PRIVATE rex::runtime edf_native_d3d11)
        add_test(NAME edf_native_immediate_tail_tests COMMAND edf_native_immediate_tail_tests)
        target_link_libraries(edf_native_sdk_ui_tests PRIVATE edf_native_sdk_ui)
        add_test(NAME edf_native_sdk_ui_tests COMMAND edf_native_sdk_ui_tests)
        add_executable(edf_native_backend_sdk_ui_tests tests/native_backend_sdk_ui_tests.cpp
            src/native_graphics/native_fallback_imports.cpp)
        target_link_libraries(edf_native_backend_sdk_ui_tests PRIVATE edf_native_sdk_ui delayimp)
        target_link_options(edf_native_backend_sdk_ui_tests PRIVATE "LINKER:/DELAYLOAD:d3d11.dll")
        add_test(NAME edf_native_backend_sdk_ui_tests COMMAND edf_native_backend_sdk_ui_tests)
        add_executable(edf_native_backend_host_tests tests/native_backend_host_tests.cpp
            src/native_graphics/native_backend_host.cpp src/input_latency.cpp)
        target_link_libraries(edf_native_backend_host_tests PRIVATE edf_native_d3d11 rex::runtime comctl32)
        add_test(NAME edf_native_backend_host_tests COMMAND edf_native_backend_host_tests)
        set_tests_properties(edf_native_backend_host_tests PROPERTIES TIMEOUT 30)
        add_executable(edf_native_host_lifetime_tests tests/native_host_lifetime_tests.cpp
            src/native_graphics/native_host_surface.cpp)
        target_link_libraries(edf_native_host_lifetime_tests PRIVATE edf_native_d3d11 rex::runtime comctl32)
        add_test(NAME edf_native_host_lifetime_tests COMMAND edf_native_host_lifetime_tests)
        set_tests_properties(edf_native_host_lifetime_tests PROPERTIES TIMEOUT 30)
        add_executable(edf_native_guest_memory_tests tests/native_guest_memory_tests.cpp)
        target_include_directories(edf_native_guest_memory_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(edf_native_guest_memory_tests PRIVATE rex::runtime)
        add_test(NAME edf_native_guest_memory_tests COMMAND edf_native_guest_memory_tests)
        set_tests_properties(edf_native_guest_memory_tests PROPERTIES TIMEOUT 30)
    endif()
    add_executable(edf2027_tests tests/unit_tests.cpp)
    target_include_directories(edf2027_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME edf2027_unit_tests COMMAND edf2027_tests)
    # The console's command language: parser, registry, validation, completion (console_logic.h).
    add_executable(edf_console_tests tests/console_tests.cpp)
    target_include_directories(edf_console_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME edf_console_tests COMMAND edf_console_tests)
    set_tests_properties(edf_console_tests PROPERTIES TIMEOUT 30)
    # Input latency: the trace, the low-latency presentation pieces and the mouse path
    # (input_latency_logic.h, native_present_slot.h, native_backend_frame_queue.h, ui_ticker.h).
    add_executable(edf_input_latency_tests tests/input_latency_tests.cpp)
    target_include_directories(edf_input_latency_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME edf_input_latency_tests COMMAND edf_input_latency_tests)
    set_tests_properties(edf_input_latency_tests PROPERTIES TIMEOUT 60)
    add_custom_target(edf_latency_report_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_latency_report
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} VERBATIM)
    add_test(NAME edf_latency_report_tests COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_latency_report)
    set_tests_properties(edf_latency_report_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(edf_frame_limiter_tests tests/frame_limiter_tests.cpp)
    target_include_directories(edf_frame_limiter_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME edf_frame_limiter_tests COMMAND edf_frame_limiter_tests)
    set_tests_properties(edf_frame_limiter_tests PROPERTIES TIMEOUT 60)
    add_executable(edf_cpu_check_tests tests/cpu_check_tests.cpp src/cpu_check_logic.cpp)
    target_include_directories(edf_cpu_check_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME edf_cpu_check_tests COMMAND edf_cpu_check_tests)
endif()

# ---------------------------------------------------------------------------
# Release package: out/package/EDF2027-PC-<version>-<commit>[-dirty]/ and .zip, plus
# -symbols.zip with the PDB (tools/package-release.cmake; no game data, ever).
# ---------------------------------------------------------------------------
set(EDF2027_PACKAGE_ROOT ${CMAKE_SOURCE_DIR}/out/package CACHE PATH "Where package_release writes its folder and zips")
# Derive the SDK's bin/ from where find_package landed (<sdk>/lib/cmake/rexglue).
# CMAKE_PREFIX_PATH is a list and may hold more than the SDK, so it cannot be used here.
get_filename_component(EDF2027_SDK_ROOT ${rexglue_DIR}/../../.. ABSOLUTE)
set(EDF2027_SDK_BIN ${EDF2027_SDK_ROOT}/bin)
# Keep gamecontrollerdb.txt next to the dev binary too (the SDL input driver loads it at startup).
add_custom_command(TARGET edf2027 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/gamecontrollerdb.txt $<TARGET_FILE_DIR:edf2027>/
    VERBATIM)

# SDK DX12/Vulkan post-processing is not used by the native renderer.
message(STATUS "EDF2027 renderer: native D3D12 scene/composition/presentation/completion; explicit delay-loaded D3D11 fallback; no GPU emulation plugin staged")
# Some SDK builds directly import FidelityFX from rexruntime.dll, even when no
# GPU plugin/upscaler is active. Satisfy that loader dependency without exposing
# unsupported native upscaling controls. This is not the Xenos plugin.
#
# Which amd_fidelityfx_dx12.dll: the native FSR path (native_ffx.h) loads it at
# run time, and wants AMD's signed prebuilt, which carries the frame-generation
# provider the SDK's own build (FFX_API_ENABLE_FRAMEGEN_PROVIDER OFF) leaves
# out. It exports the same five entry points (ffxCreateContext, ffxDestroyContext,
# ffxConfigure, ffxQuery, ffxDispatch), so rexruntime.dll's direct import of
# four of them is satisfied either way; the SDK presenter that would call them
# is never created in native mode. Its providers (ffxQuery GET_VERSIONS, run by
# edf_native_fsr_foundation_tests): upscale 3.1.4 and 2.3.3, the same as the
# SDK build's, plus frame generation 1.1.3. It also imports VCRUNTIME140_1.dll,
# USER32 and WINMM, which the SDK build does not. An option because it swaps a
# binary the SDK itself loads: OFF, or a signed DLL that is not on disk, stages
# the SDK's DLL exactly as before.
option(EDF2027_FFX_SIGNED_DLL "Stage AMD's signed amd_fidelityfx_dx12.dll (with frame generation) instead of the SDK build's" ON)
set(EDF2027_FFX_SIGNED_DLL_PATH
    ${EDF2027_SDK_ROOT}/../../rexglue-sdk/out/build/win-amd64/_deps/fidelityfx-src/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll
    CACHE FILEPATH "AMD's signed amd_fidelityfx_dx12.dll (FidelityFX-SDK PrebuiltSignedDLL)")
set(EDF2027_SDK_AUX_DLLS)
if(EDF2027_FFX_SIGNED_DLL AND EXISTS ${EDF2027_FFX_SIGNED_DLL_PATH})
    get_filename_component(_ffx_signed ${EDF2027_FFX_SIGNED_DLL_PATH} ABSOLUTE)
    message(STATUS "EDF2027 FidelityFX: staging signed ${_ffx_signed}")
    list(APPEND EDF2027_SDK_AUX_DLLS ${_ffx_signed})
elseif(TARGET rex::amd_fidelityfx_dx12)
    if(EDF2027_FFX_SIGNED_DLL)
        message(STATUS "EDF2027 FidelityFX: signed DLL not found at ${EDF2027_FFX_SIGNED_DLL_PATH}; staging the SDK's (no frame generation)")
    endif()
    list(APPEND EDF2027_SDK_AUX_DLLS $<TARGET_FILE:rex::amd_fidelityfx_dx12>)
endif()
if(EDF2027_SDK_AUX_DLLS)
    add_custom_command(TARGET edf2027 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${EDF2027_SDK_AUX_DLLS} $<TARGET_FILE_DIR:edf2027>/
        VERBATIM)
    foreach(_sdk_test edf_native_sdk_ui_tests edf_native_host_lifetime_tests edf_native_guest_memory_tests edf_native_immediate_tail_tests)
        if(TARGET ${_sdk_test})
            add_custom_command(TARGET ${_sdk_test} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${EDF2027_SDK_AUX_DLLS} $<TARGET_FILE_DIR:${_sdk_test}>/
                VERBATIM)
        endif()
    endforeach()
    # The FFX loader test is defined in src/native_graphics, where a POST_BUILD
    # from here cannot attach, so the same DLL is staged by a dependency.
    if(TARGET edf_native_fsr_foundation_tests)
        add_custom_target(edf_native_fsr_foundation_ffx_dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${EDF2027_SDK_AUX_DLLS}
                    $<TARGET_FILE_DIR:edf_native_fsr_foundation_tests>/
            VERBATIM)
        add_dependencies(edf_native_fsr_foundation_tests edf_native_fsr_foundation_ffx_dll)
        if(TARGET edf_native_fsr_tests)
            add_dependencies(edf_native_fsr_tests edf_native_fsr_foundation_ffx_dll)
        endif()
    endif()
endif()

# Which runtime DLLs an SDK install ships depends on how it was configured (Tracy is
# absent from some), so package only the ones that are actually there.
set(EDF2027_RUNTIME_DLLS)
foreach(_dll rexruntime TracyClient)
    set(_dll_path ${EDF2027_SDK_BIN}/${_dll}${CMAKE_SHARED_LIBRARY_SUFFIX})
    if(EXISTS ${_dll_path})
        list(APPEND EDF2027_RUNTIME_DLLS ${_dll_path})
    endif()
endforeach()

# Microsoft's redistributables, shipped app-local (LICENSES/Microsoft-redistributables.txt):
# - The Visual C++ runtime the exe, rexruntime.dll and the FidelityFX DLL import. Without
#   it a machine lacking the VC++ 2015-2022 redistributable cannot start the game, and one
#   with an older redistributable than the build's toolset can crash in std::mutex.
# - d3dcompiler_47.dll, which compiles the game's HLSL at run time. Windows has one too,
#   but Wine/Proton's builtin differs; the loader takes the one beside the exe first
#   (it is not a KnownDLL), and the startup report logs which one was loaded.
if(NOT EDF2027_VC_RUNTIME_DIR)
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _edf_vc_redist)
    file(GLOB _edf_vc_crt "${_edf_vc_redist}/x64/Microsoft.VC*.CRT")
    if(NOT _edf_vc_crt)
        file(GLOB _edf_vc_crt "C:/Program Files/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
    endif()
    if(_edf_vc_crt)
        list(SORT _edf_vc_crt COMPARE NATURAL ORDER DESCENDING)
        list(GET _edf_vc_crt 0 _edf_vc_crt)
    endif()
    set(EDF2027_VC_RUNTIME_DIR "${_edf_vc_crt}" CACHE PATH "Visual C++ runtime redist folder (Microsoft.VC14x.CRT) for package_release")
endif()
set(EDF2027_D3DCOMPILER_DLL "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/d3dcompiler_47.dll"
    CACHE FILEPATH "Windows SDK redistributable d3dcompiler_47.dll for package_release")
set(EDF2027_PACKAGE_DLLS ${EDF2027_RUNTIME_DLLS} ${EDF2027_SDK_AUX_DLLS} ${EDF2027_D3DCOMPILER_DLL})
foreach(_dll msvcp140 msvcp140_atomic_wait vcruntime140 vcruntime140_1)
    list(APPEND EDF2027_PACKAGE_DLLS ${EDF2027_VC_RUNTIME_DIR}/${_dll}.dll)
endforeach()
string(REPLACE ";" "|" _edf_package_dlls "${EDF2027_PACKAGE_DLLS}")
set(_edf_package_pdb "")
if(WIN32 AND EDF2027_RELEASE_PDB)
    set(_edf_package_pdb "$<TARGET_FILE_DIR:edf2027>/$<TARGET_FILE_BASE_NAME:edf2027>.pdb")
endif()
add_custom_target(package_release
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DPACKAGE_ROOT=${EDF2027_PACKAGE_ROOT}
        -DVERSION=${EDF2027_VERSION} -DEXECUTABLE=$<TARGET_FILE:edf2027> "-DDLLS=${_edf_package_dlls}"
        -DPDB=${_edf_package_pdb} -DAUDIT_SCRIPT=${CMAKE_SOURCE_DIR}/tools/audit-native-dependencies.cmake
        -P ${CMAKE_SOURCE_DIR}/tools/package-release.cmake
    DEPENDS edf2027
    COMMENT "Packaging EDF2027-PC-${EDF2027_VERSION} (port only, no game data)"
    VERBATIM)
