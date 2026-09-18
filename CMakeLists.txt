cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

if(NOT OS_WINDOWS)
  message(FATAL_ERROR "${_name} is Windows only: it uses Direct3D 11 and Windows event tracing.")
endif()

# a newer toolset than the installed runtime is known to crash in std::mutex, so the runtime comes along
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(compilerconfig)
include(defaults)
include(helpers)

# presentmon's frame tracking, vendored with stand-ins for the parts of its utility library it uses
set(PRESENTMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/presentmon")

add_library(presentdata STATIC)
target_sources(
  presentdata
  PRIVATE
    "${PRESENTMON_DIR}/IntelPresentMon/CommonUtilities/Hash.cpp"
    "${PRESENTMON_DIR}/IntelPresentMon/CommonUtilities/Shim.cpp"
    "${PRESENTMON_DIR}/PresentData/Debug.cpp"
    "${PRESENTMON_DIR}/PresentData/GpuTrace.cpp"
    "${PRESENTMON_DIR}/PresentData/InterPresentActivity.cpp"
    "${PRESENTMON_DIR}/PresentData/NvidiaTraceConsumer.cpp"
    "${PRESENTMON_DIR}/PresentData/PresentMonTraceConsumer.cpp"
    "${PRESENTMON_DIR}/PresentData/PresentMonTraceSession.cpp"
    "${PRESENTMON_DIR}/PresentData/TraceConsumer.cpp"
    "${PRESENTMON_DIR}/PresentData/TraceLogging.cpp"
)
target_include_directories(presentdata PUBLIC "${PRESENTMON_DIR}")
target_compile_definitions(presentdata PUBLIC PRESENTMON_ENABLE_DEBUG_TRACE=0 NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_features(presentdata PUBLIC cxx_std_20)
target_compile_options(presentdata PRIVATE /W0)
target_link_libraries(presentdata PUBLIC advapi32 tdh)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api presentdata d3d11)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin.cpp
    src/ft/game.cpp
    src/ft/game_timing.cpp
    src/ft/logs.cpp
    src/ft/probe.cpp
    src/ft/recording.cpp
    src/ft/sidecar.cpp
    src/ft/win.cpp
)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
