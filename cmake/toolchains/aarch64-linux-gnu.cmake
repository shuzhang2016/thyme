# Generic GNU/Linux AArch64 cross toolchain (for example, Ubuntu's
# gcc-aarch64-linux-gnu and g++-aarch64-linux-gnu packages).
#
# For an extracted/non-system toolchain, pass -DCMAKE_SYSROOT=/path/to/sysroot.
# When qemu-aarch64-static is installed, CTest runs target binaries through it;
# set QEMU_LD_PREFIX to the runtime sysroot (Ubuntu: /usr/aarch64-linux-gnu).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TOOLCHAIN_PREFIX aarch64-linux-gnu-)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_PREFIX}gcc)

# Match the target A-core's ARMv8 CRC32 extension so both the hardware and
# software CRC paths receive architecture coverage. Override the flags from
# the CMake command line when targeting a different AArch64 CPU.
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -march=armv8-a+crc")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -march=armv8-a+crc")

# Host programs (CMake, Python and QEMU) remain visible, while target headers
# and libraries must come from the cross toolchain or its explicit sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

find_program(AARCH64_LINUX_GNU_QEMU qemu-aarch64-static)
if(AARCH64_LINUX_GNU_QEMU)
  set(CMAKE_CROSSCOMPILING_EMULATOR "${AARCH64_LINUX_GNU_QEMU}")
else()
  message(STATUS "qemu-aarch64-static not found; target tests cannot run")
endif()
