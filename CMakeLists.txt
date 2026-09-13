cmake_minimum_required(VERSION 3.20)

project(StreamService LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W3 /EHsc /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(BUILD_CLIENT "Build DXGI/NVENC SDL client (needs SDL2, ImGui, FFmpeg, NVENC SDK)" ON)

# ---------------- Relay server: zero external deps ----------------
add_executable(relay_server relay_server.cpp)
target_link_libraries(relay_server PRIVATE ws2_32 winmm)

if(MSVC)
  # UTF-8 source (client/server contain Cyrillic comments)
  target_compile_options(relay_server PRIVATE /utf-8)
endif()

message(STATUS "relay_server target configured (Winsock2 + WinMM only)")

# ---------------- Client (heavy deps) ----------------
if(BUILD_CLIENT)
  include(FetchContent)

  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.zip
  )

  FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.90.9.zip
  )

  FetchContent_MakeAvailable(SDL2 imgui)

  # ImGui has no CMakeLists -> build manually (core + SDL2/SDLRenderer2 backends)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
  )
  target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  target_link_libraries(imgui PUBLIC SDL2::SDL2)

  # ---- FFmpeg (prebuilt shared, BtbN or Gyan) ----
  # Ожидаем структуру:
  #   ${FFMPEG_ROOT}/include/libavcodec/avcodec.h ...
  #   ${FFMPEG_ROOT}/lib/avcodec.lib ...
  #   ${FFMPEG_ROOT}/bin/avcodec-*.dll ...
  # Скачай сам, либо укажи -DFFMPEG_ROOT=... при configure.
  # Рекомендуемый архив (zip):
  #   https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n7.1-latest-win64-gpl-shared-7.1.zip
  set(FFMPEG_ROOT "" CACHE PATH "Path to prebuilt FFmpeg shared dev package (include/, lib/, bin/)")

  if(NOT FFMPEG_ROOT OR NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(WARNING
      "FFMPEG_ROOT not set or incomplete. Client will NOT build.\n"
      "  1) Download ffmpeg-n*-win64-gpl-shared-*.zip from https://github.com/BtbN/FFmpeg-Builds/releases\n"
      "  2) Unpack to e.g. C:/libs/ffmpeg-shared\n"
      "  3) Reconfigure with -DFFMPEG_ROOT=C:/libs/ffmpeg-shared  (or the inner folder containing include/, lib/, bin/)\n"
      "Building relay_server only for now."
    )
    set(CLIENT_FFMPEG_OK FALSE)
  else()
    set(CLIENT_FFMPEG_OK TRUE)
    message(STATUS "FFmpeg root: ${FFMPEG_ROOT}")
  endif()

  # ---- NVENC SDK header ----
  # Нужен nvEncodeAPI.h из NVIDIA Video Codec SDK:
  #   https://developer.nvidia.com/video-codec-sdk-archive
  # Либо положи его в third_party/nvcodec/nvEncodeAPI.h
  set(NVCODEC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvcodec")
  if(NOT EXISTS "${NVCODEC_INCLUDE_DIR}/nvEncodeAPI.h")
    message(WARNING
      "third_party/nvcodec/nvEncodeAPI.h not found. Client will NOT build.\n"
      "  Download Video Codec SDK and copy Interface/nvEncodeAPI.h -> third_party/nvcodec/nvEncodeAPI.h"
    )
    set(CLIENT_NVENC_OK FALSE)
  else()
    set(CLIENT_NVENC_OK TRUE)
  endif()

  if(CLIENT_FFMPEG_OK AND CLIENT_NVENC_OK)
    add_executable(client client.cpp)
    target_compile_options(client PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

    target_include_directories(client PRIVATE
      ${FFMPEG_ROOT}/include
      ${NVCODEC_INCLUDE_DIR}
    )
    target_link_directories(client PRIVATE ${FFMPEG_ROOT}/lib)
    target_link_libraries(client PRIVATE
      SDL2::SDL2
      imgui
      avcodec avutil swscale
      ws2_32 winmm d3d11 dxgi d3dcompiler
    )

    # FFmpeg DLLs рядом с exe, чтобы запуск из build/ работал
    if(EXISTS "${FFMPEG_ROOT}/bin")
      add_custom_command(TARGET client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:client>"
        COMMENT "Copying FFmpeg DLLs next to client.exe"
      )
    endif()
    # SDL2 DLL рядом с exe
    add_custom_command(TARGET client POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:client>"
      COMMENT "Copying SDL2 DLL next to client.exe"
    )

    message(STATUS "client target configured")

    # Дистрибутив для синхронизации: только готовые бинарники в <root>/builds.
    # Обновление: cmake --build build --config Release --target dist
    file(GLOB FFMPEG_DIST_DLLS
      "${FFMPEG_ROOT}/bin/avcodec-*.dll"
      "${FFMPEG_ROOT}/bin/avutil-*.dll"
      "${FFMPEG_ROOT}/bin/swscale-*.dll"
      "${FFMPEG_ROOT}/bin/swresample-*.dll"
    )
    set(DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/builds")
    add_custom_target(dist
      COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:client>"
        "$<TARGET_FILE:relay_server>"
        "$<TARGET_FILE:SDL2::SDL2>"
        ${FFMPEG_DIST_DLLS}
        "${DIST_DIR}"
      DEPENDS client relay_server
      COMMENT "Copying binaries to ${DIST_DIR}"
    )
  else()
    message(STATUS "Skipping client target (missing FFmpeg and/or nvEncodeAPI.h). relay_server still builds.")
  endif()
endif()
