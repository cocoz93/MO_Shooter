cmake_minimum_required(VERSION 3.20)
project(LyraPilot LANGUAGES CXX)

# Lyra 다리 파일럿 서버 — 각본만 흘린다. 플랜 = MyGit/MyDocs/claude-plans/mo-shooter/lyra-bridge-pilot-plan.md
#   cmake -S . -B build && cmake --build build --config Release

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 서브모듈 ServerCore 는 고치지 않고 가져다만 쓴다(MO_Shooter/CLAUDE.md)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../ServerCore ${CMAKE_CURRENT_BINARY_DIR}/ServerCore)

add_executable(LyraPilot main.cpp PilotProtocol.h)
target_link_libraries(LyraPilot PRIVATE ServerNetworkLib)
if(MSVC)
    target_compile_options(LyraPilot PRIVATE /utf-8)
    target_compile_definitions(LyraPilot PRIVATE NOMINMAX)   # windows.h 의 min/max 매크로가 std::max 를 깨뜨린다
endif()
