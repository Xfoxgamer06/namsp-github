cmake_minimum_required(VERSION 3.16)
project(namsp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Where NAM Core lives (git submodule, see README).
set(NAM_DIR ${CMAKE_SOURCE_DIR}/deps/NeuralAmpModelerCore)
if(NOT EXISTS ${NAM_DIR}/NAM)
  message(FATAL_ERROR "NeuralAmpModelerCore not found in deps/. See README step 1.")
endif()

# Same source list the official TONE3000 plugin compiles (plugin/CMakeLists.txt).
# Compiled straight into the executable so the static model-parser registrations
# are not stripped by the linker.
set(NAM_SRC
  ${NAM_DIR}/NAM/activations.cpp ${NAM_DIR}/NAM/conv1d.cpp ${NAM_DIR}/NAM/container.cpp
  ${NAM_DIR}/NAM/convnet.cpp ${NAM_DIR}/NAM/dsp.cpp ${NAM_DIR}/NAM/get_dsp.cpp
  ${NAM_DIR}/NAM/lstm.cpp ${NAM_DIR}/NAM/ring_buffer.cpp ${NAM_DIR}/NAM/util.cpp
  ${NAM_DIR}/NAM/wavenet/a2_fast.cpp ${NAM_DIR}/NAM/wavenet/model.cpp ${NAM_DIR}/NAM/wavenet/slimmable.cpp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(CURL REQUIRED libcurl)

add_executable(namsp src/main.cpp ${NAM_SRC})

target_include_directories(namsp PRIVATE
  src
  ${NAM_DIR}
  ${NAM_DIR}/Dependencies/eigen
  ${NAM_DIR}/Dependencies/nlohmann
  ${SDL2_INCLUDE_DIRS} ${SDL2_TTF_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS})

# NAM_SAMPLE stays double, as in the official plugin. NAM_ENABLE_A2_FAST enables the
# hand-optimised A2 path; if it fails to compile on aarch64, remove it (slower but works).
target_compile_definitions(namsp PRIVATE NAM_ENABLE_A2_FAST NDEBUG EIGEN_DONT_PARALLELIZE)

target_compile_options(namsp PRIVATE -O3 -ffast-math -fno-math-errno)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  # Allwinner H700 = 4x Cortex-A53
  target_compile_options(namsp PRIVATE -mcpu=cortex-a53 -mtune=cortex-a53)
endif()

target_link_libraries(namsp PRIVATE ${SDL2_LIBRARIES} ${SDL2_TTF_LIBRARIES} ${ALSA_LIBRARIES} ${CURL_LIBRARIES} pthread)
# Avoids libstdc++ version mismatches between the build container and muOS.
target_link_options(namsp PRIVATE -static-libstdc++ -static-libgcc)

# Unit tests for the dependency-free DSP code:  cmake --build . --target run_tests
add_executable(test_dsp tests/test_dsp.cpp)
add_executable(test_t3k tests/test_t3k.cpp)
add_custom_target(run_tests COMMAND test_dsp COMMAND test_t3k DEPENDS test_dsp test_t3k)

# Optional go/no-go benchmark: copy test/src/os_bench.cpp from the TONE3000 plugin repo
# (MIT) to tests/os_bench.cpp, then `cmake --build . --target a2_bench` and run it on the SP.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/os_bench.cpp)
  add_executable(a2_bench tests/os_bench.cpp ${NAM_SRC})
  target_include_directories(a2_bench PRIVATE ${NAM_DIR} ${NAM_DIR}/Dependencies/eigen ${NAM_DIR}/Dependencies/nlohmann)
  target_compile_definitions(a2_bench PRIVATE NAM_ENABLE_A2_FAST NDEBUG)
  target_compile_options(a2_bench PRIVATE -O3)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_options(a2_bench PRIVATE -mcpu=cortex-a53 -mtune=cortex-a53)
  endif()
  target_link_libraries(a2_bench PRIVATE pthread)
endif()
