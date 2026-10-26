cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK})
    set(PS2X_VITA_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake")
    if(EXISTS "${PS2X_VITA_TOOLCHAIN_FILE}")
        set(CMAKE_TOOLCHAIN_FILE "${PS2X_VITA_TOOLCHAIN_FILE}" CACHE PATH
            "Toolchain file used for cross-compiling" FORCE)
        message(STATUS "Using VitaSDK toolchain from VITASDK: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
endif()

# MSVC + Ninja Debug builds failed with C1041 (several cl.exe processes writing one target PDB). Embed debug info
# in the objects (/Z7) instead; must be set before project() so it also applies to fetched dependencies.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>" CACHE STRING
        "MSVC debug information format")
endif()

project(PS2RetroX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PS2X_BUILD_RECOMP "Build ps2xRecomp" ON)
option(PS2X_BUILD_RUNTIME "Build ps2xRuntime" ON)
option(PS2X_BUILD_ANALYZER "Build ps2xAnalyzer" ON)
option(PS2X_BUILD_TEST "Build ps2xTest" ON)
option(PS2X_BUILD_STUDIO "Build ps2xStudio" ON)

if(ANDROID)
    message(STATUS "Android target detected, building runtime only")
    set(PS2X_BUILD_RECOMP OFF)
    set(PS2X_BUILD_ANALYZER OFF)
    set(PS2X_BUILD_TEST OFF)
    set(PS2X_BUILD_STUDIO OFF)
endif()

set(PS2X_IS_ARM_TARGET OFF)
set(PS2X_IS_AARCH64_TARGET OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    set(PS2X_IS_ARM_TARGET ON)
    set(PS2X_IS_AARCH64_TARGET ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm|^ARM")
    set(PS2X_IS_ARM_TARGET ON)
endif()

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
   (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_ARM_TARGET ON)
endif()

if(PS2X_IS_ARM_TARGET)
    message(STATUS "ARM target detected, fetching sse2neon")

    include(FetchContent)
    FetchContent_Declare(
        sse2neon
        GIT_REPOSITORY https://github.com/DLTcollab/sse2neon.git
        GIT_TAG v1.9.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(sse2neon)

    include_directories(${sse2neon_SOURCE_DIR})
    add_compile_definitions(USE_SSE2NEON)

    if(PS2X_IS_AARCH64_TARGET AND APPLE)
        # macOS ARM64 already uses optimal defaults
        message(STATUS "macOS ARM64 - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET AND MSVC)
        # Windows ARM64 - MSVC already uses optimal defaults
        message(STATUS "Windows ARM64 (MSVC) - using default compiler flags")
    elseif(PS2X_IS_AARCH64_TARGET)
        # Linux ARM64 - add NEON flags
        message(STATUS "Non-Apple ARM64 - adding NEON compiler flags")
        add_compile_options(-march=armv8-a+fp+simd)

        # Try to enable crypto and CRC extensions if supported
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-march=armv8-a+fp+simd+crypto+crc" COMPILER_SUPPORTS_CRYPTO_CRC)
        if(COMPILER_SUPPORTS_CRYPTO_CRC)
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+fp+simd+crypto+crc")
            message(STATUS "Crypto and CRC extensions enabled")
        else()
            add_compile_options(-march=armv8-a+fp+simd)
        endif()
    endif()
endif()

# Sanitizer and fuzzing builds (docs/architecture/fuzzing.md). Instrumentation applies to every target, fetched
# dependencies included, so a defect reached through a dependency is reported too. Use RelWithDebInfo or Debug:
# Release adds /GL and /fp:fast through EnableFastReleaseMode.
set(PS2X_SANITIZE "" CACHE STRING "Sanitizers to enable: empty, address, or address,undefined (undefined: GCC/Clang)")
option(PS2X_BUILD_FUZZERS "Build the libFuzzer harnesses in tools/fuzz (MSVC or Clang, needs PS2X_SANITIZE=address)" OFF)
if(PS2X_SANITIZE)
    if(MSVC)
        if(NOT PS2X_SANITIZE STREQUAL "address")
            message(FATAL_ERROR "MSVC supports only PS2X_SANITIZE=address")
        endif()
        add_compile_options(/fsanitize=address)
        add_link_options(/INCREMENTAL:NO)
        if(PS2X_BUILD_FUZZERS)
            add_compile_options(/fsanitize-coverage=inline-8bit-counters /fsanitize-coverage=edge
                                /fsanitize-coverage=trace-cmp)
            # MSVC defines the coverage symbols only in the libFuzzer runtime, so executables without a fuzzer
            # entry point (ps2x_tests, runners) cannot link here; run the unit suite from a PS2X_SANITIZE-only build.
            set(PS2X_BUILD_TEST OFF)
            set(PS2X_BUILD_ANALYZER OFF)
        endif()
    else()
        add_compile_options(-fsanitize=${PS2X_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
        add_link_options(-fsanitize=${PS2X_SANITIZE})
        if(PS2X_BUILD_FUZZERS)
            add_compile_options(-fsanitize=fuzzer-no-link)
        endif()
    endif()
elseif(PS2X_BUILD_FUZZERS)
    message(FATAL_ERROR "PS2X_BUILD_FUZZERS requires PS2X_SANITIZE=address")
endif()

if(PS2X_BUILD_RECOMP)
    add_subdirectory("ps2xRecomp")
endif()

if(PS2X_BUILD_RUNTIME)
    add_subdirectory("ps2xIOP")
    add_subdirectory("ps2xRuntime")
endif()

if(PS2X_BUILD_ANALYZER)
    add_subdirectory("ps2xAnalyzer")
endif()

if(PS2X_BUILD_TEST)
    add_subdirectory("ps2xTest")
endif()

if(PS2X_BUILD_STUDIO)
    add_subdirectory("ps2xStudio")
endif()

if(PS2X_BUILD_FUZZERS)
    add_subdirectory("tools/fuzz")
endif()
