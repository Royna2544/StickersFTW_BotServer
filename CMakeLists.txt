# CMakeList.txt : CMake project for StickersTW_BotServer, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.10)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("StickersFTW_BotServer")

find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(cxxopts REQUIRED)
find_package(fmt REQUIRED)
find_package(OpenSSL REQUIRED)

# Add source to this project's executable.
add_executable (StickersFTW_BotServer "StickersFTW_BotServer.cpp")
target_link_libraries(StickersFTW_BotServer httplib::httplib nlohmann_json::nlohmann_json spdlog::spdlog_header_only cxxopts::cxxopts fmt::fmt-header-only OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(StickersFTW_BotServer PRIVATE /utf-8 /bigobj)

set_property(TARGET StickersFTW_BotServer PROPERTY CXX_STANDARD 23)
set_property(TARGET StickersFTW_BotServer PROPERTY CXX_STANDARD_REQUIRED ON)

# TODO: Add tests and install targets if needed.
