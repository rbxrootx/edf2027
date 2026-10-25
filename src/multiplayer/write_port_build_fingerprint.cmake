if(NOT DEFINED EDF_PORT_SOURCE_ROOT OR NOT DEFINED EDF_PORT_FINGERPRINT_OUTPUT)
    message(FATAL_ERROR "EDF port fingerprint source root and output path are required")
endif()

set(_edf_root "${EDF_PORT_SOURCE_ROOT}")
file(GLOB_RECURSE _edf_fingerprint_files
    "${_edf_root}/src/*.c"
    "${_edf_root}/src/*.cc"
    "${_edf_root}/src/*.cpp"
    "${_edf_root}/src/*.cxx"
    "${_edf_root}/src/*.h"
    "${_edf_root}/src/*.hpp"
    "${_edf_root}/src/*.inl"
    "${_edf_root}/generated/*.cmake"
    "${_edf_root}/generated/*.json"
    "${_edf_root}/generated/default/*.cpp"
    "${_edf_root}/generated/default/*.h"
    "${_edf_root}/generated/default/*.hpp"
    "${_edf_root}/cmake/*.cmake"
    "${_edf_root}/third_party/enet/*.c"
    "${_edf_root}/third_party/enet/*.cc"
    "${_edf_root}/third_party/enet/*.cpp"
    "${_edf_root}/third_party/enet/*.h"
    "${_edf_root}/third_party/enet/*.hpp"
    "${_edf_root}/third_party/luau/*.c"
    "${_edf_root}/third_party/luau/*.cc"
    "${_edf_root}/third_party/luau/*.cpp"
    "${_edf_root}/third_party/luau/*.h"
    "${_edf_root}/third_party/luau/*.hpp"
    "${_edf_root}/third_party/luau/*.cmake")
# Keep machine-local CMake outputs out of the source identity. These paths can
# appear inside vendored trees and are not reproducible port inputs.
list(FILTER _edf_fingerprint_files EXCLUDE REGEX "/(out|build|builds|CMakeFiles|_deps)/")
file(GLOB _edf_manifests "${_edf_root}/edf2017_*.toml")
list(APPEND _edf_fingerprint_files
    ${_edf_manifests}
    "${_edf_root}/CMakeLists.txt"
    "${_edf_root}/src/multiplayer/CMakeLists.txt"
    "${_edf_root}/src/multiplayer/write_port_build_fingerprint.cmake"
    "${_edf_root}/third_party/enet/CMakeLists.txt"
    "${_edf_root}/third_party/luau/CMakeLists.txt"
    "${_edf_root}/third_party/luau/fuzz/CMakeLists.txt"
    "${_edf_root}/third_party/luau/Sources.cmake")
list(REMOVE_DUPLICATES _edf_fingerprint_files)
list(SORT _edf_fingerprint_files)

set(_edf_fingerprint_material "")
foreach(_edf_file IN LISTS _edf_fingerprint_files)
    if(NOT EXISTS "${_edf_file}")
        continue()
    endif()
    file(READ "${_edf_file}" _edf_contents)
    string(REPLACE "\r\n" "\n" _edf_contents "${_edf_contents}")
    string(REPLACE "\r" "\n" _edf_contents "${_edf_contents}")
    string(SHA256 _edf_file_hash "${_edf_contents}")
    file(RELATIVE_PATH _edf_relative "${_edf_root}" "${_edf_file}")
    string(REPLACE "\\" "/" _edf_relative "${_edf_relative}")
    string(APPEND _edf_fingerprint_material "${_edf_relative}:${_edf_file_hash}\n")
endforeach()
string(SHA256 _edf_fingerprint "${_edf_fingerprint_material}")

get_filename_component(_edf_output_dir "${EDF_PORT_FINGERPRINT_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_edf_output_dir}")
file(WRITE "${EDF_PORT_FINGERPRINT_OUTPUT}"
    "#pragma once\n#include <string_view>\nnamespace edf::net {\ninline constexpr std::string_view kPortBuildFingerprint = \"${_edf_fingerprint}\";\n}\n")
