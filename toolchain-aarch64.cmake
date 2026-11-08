# Cross-compile for the RG35XX SP (Allwinner H700, aarch64).
# Usage:  SYSROOT=/path/to/aarch64/sysroot cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-aarch64.cmake ..
# The sysroot must contain SDL2, SDL2_ttf and ALSA dev files (libasound2-dev) for the
# SAME glibc as muOS on your device. Easiest: use the PortMaster / muOS aarch64 build container.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
if(DEFINED ENV{SYSROOT})
  set(CMAKE_SYSROOT $ENV{SYSROOT})
  set(ENV{PKG_CONFIG_SYSROOT_DIR} $ENV{SYSROOT})
  set(ENV{PKG_CONFIG_LIBDIR} "$ENV{SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:$ENV{SYSROOT}/usr/lib/pkgconfig:$ENV{SYSROOT}/usr/share/pkgconfig")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
