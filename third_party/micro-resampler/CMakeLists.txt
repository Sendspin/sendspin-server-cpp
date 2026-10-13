# Check if building as ESP-IDF component
if(ESP_PLATFORM)
    # ESP-IDF Component Mode
    set(COMPONENT_SRCS "src/resampler.cpp" "src/sinc_table.cpp" "src/filter_design.cpp" "src/design_resolver.cpp" "src/extrapolator/extrapolator_zeros.cpp" "src/extrapolator/extrapolator_mirror.cpp" "src/extrapolator/extrapolator_burg.cpp")

    # Always add the standard Xtensa assembly (MAC16)
    list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod.S")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/xtensa/xtensa_dotprod_shift_ch1.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod_shift_ch1.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/xtensa/xtensa_dotprod_shift_ch2.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod_shift_ch2.S")
    endif()

    # Add S3 assembly files if they exist (let preprocessor handle target detection)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod_s3.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3_shift_ch1.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod_s3_shift_ch1.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3_shift_ch2.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod_s3_shift_ch2.S")
    endif()

    # Add S3 Float32 assembly files if they exist
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3_float32_ch1.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod_s3_float32_ch1.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3_float32_ch2.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod_s3_float32_ch2.S")
    endif()

    # Add Q31Lite scalar assembly files (all Xtensa targets) if they exist
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/xtensa/xtensa_dotprod_q31lite_ch1.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod_q31lite_ch1.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/xtensa/xtensa_dotprod_q31lite_ch2.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/xtensa/xtensa_dotprod_q31lite_ch2.S")
    endif()

    # Add ESP32-P4 RISC-V PIE assembly files if they exist (the preprocessor
    # guards keep them empty on every other target)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/riscv/riscv_dotprod_p4_shift_ch1.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/riscv/riscv_dotprod_p4_shift_ch1.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/riscv/riscv_dotprod_p4_shift_ch2.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/riscv/riscv_dotprod_p4_shift_ch2.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/riscv/riscv_dotprod_p4_float32_ch1.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/riscv/riscv_dotprod_p4_float32_ch1.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/riscv/riscv_dotprod_p4_float32_ch2.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/riscv/riscv_dotprod_p4_float32_ch2.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/riscv/riscv_dotprod_p4_q31lite_ch1.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/riscv/riscv_dotprod_p4_q31lite_ch1.S")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/dotprod/riscv/riscv_dotprod_p4_q31lite_ch2.S")
        list(APPEND COMPONENT_SRCS "src/dotprod/riscv/riscv_dotprod_p4_q31lite_ch2.S")
    endif()

    idf_component_register(
        SRCS ${COMPONENT_SRCS}
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src"
    )

    # Check for ESP32-S3 target and define it for assembly files
    if(IDF_TARGET STREQUAL "esp32s3")
        target_compile_definitions(${COMPONENT_LIB} PUBLIC ESP32S3)
    endif()

    # Check for ESP32-P4 target and define it for assembly files
    if(IDF_TARGET STREQUAL "esp32p4")
        target_compile_definitions(${COMPONENT_LIB} PUBLIC ESP32P4)
    endif()

    # The ESP32-S31's xespv 2.1 SIMD extension is a superset of the P4's PIE, so it
    # runs the same riscv_dotprod_p4_* kernels (see dotprod_dispatch.h)
    if(IDF_TARGET STREQUAL "esp32s31")
        target_compile_definitions(${COMPONENT_LIB} PUBLIC ESP32S31)
    endif()

    # ESP32 specific optimizations. The library never reads errno, and without
    # -fno-math-errno GCC keeps lround()/lroundf() as libm calls instead of
    # inlining them (a single fcvt.w.s on RISC-V)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -fno-math-errno)

    # First-party warning block on the ESP-IDF component's wrapper sources,
    # mirroring the host build. This is the only build that ever compiles the
    # ESP-only code paths, so it is the only place their warnings get caught.
    # -Werror is gated behind MICRO_RESAMPLER_ENABLE_WERROR (defaulting off, as on
    # host): a consumer building the component from the registry gets warnings
    # only, so a newer IDF/Xtensa toolchain can never hard-fail their build; CI's
    # pinned ESP build can pass -DMICRO_RESAMPLER_ENABLE_WERROR=ON to turn them
    # into errors. Every source
    # here is first-party (the Xtensa .S files above are ours) -- no vendored or
    # forked tree is put through the block.
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Wpedantic
        # Any function not declared in a header must be static; keeps
        # -Wunused-function able to see dead internal functions. Clang and GCC
        # spell the C++ variant of this check differently.
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wmissing-prototypes>
        $<$<CXX_COMPILER_ID:GNU>:-Wmissing-declarations>
        # C++-only (this component also compiles Xtensa assembly): require
        # static_cast/reinterpret_cast over C-style casts
        $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
        $<$<BOOL:${MICRO_RESAMPLER_ENABLE_WERROR}>:-Werror>
    )

else()
    # Standalone CMake Mode
    cmake_minimum_required(VERSION 3.14)
    project(micro-resampler VERSION 0.0.6 LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    # Whether this is the top-level project rather than a subproject pulled in
    # through add_subdirectory()/FetchContent. Compared by directory instead of
    # PROJECT_IS_TOP_LEVEL, which needs CMake 3.21 and this file supports 3.14.
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(MICRO_RESAMPLER_IS_TOP_LEVEL ON)
    else()
        set(MICRO_RESAMPLER_IS_TOP_LEVEL OFF)
    endif()

    # Options. Everything that builds more than the library, or changes how it
    # is compiled, defaults to off unless this is the top-level project, so a
    # consumer gets just the library with its own compiler settings.
    option(MICRO_RESAMPLER_BUILD_TESTS "Build the test suite (fetches GoogleTest)" ${MICRO_RESAMPLER_IS_TOP_LEVEL})
    option(MICRO_RESAMPLER_NATIVE_TUNING "Compile the library with -O3 -march=native -ffast-math" ${MICRO_RESAMPLER_IS_TOP_LEVEL})
    option(MICRO_RESAMPLER_ENABLE_WERROR "Treat warnings in first-party targets as errors" OFF)
    option(MICRO_RESAMPLER_ENABLE_SANITIZERS "Build first-party targets with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

    # Main library
    set(RESAMPLER_SOURCES
        src/resampler.cpp
        src/sinc_table.cpp
        src/filter_design.cpp
        src/design_resolver.cpp
        src/extrapolator/extrapolator_zeros.cpp
        src/extrapolator/extrapolator_mirror.cpp
        src/extrapolator/extrapolator_burg.cpp
        src/dotprod/arm/neon_q15.cpp
        src/dotprod/arm/neon_q31.cpp
        src/dotprod/arm/neon_float.cpp
        src/dotprod/arm/neon_float32.cpp
    )

    # Add assembly files for Xtensa architecture
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "xtensa" OR DEFINED XTENSA)
        list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod.S)
        # Add MAC16 shift variant if it exists
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dotprod/xtensa/xtensa_dotprod_shift_ch1.S")
            list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod_shift_ch1.S)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dotprod/xtensa/xtensa_dotprod_shift_ch2.S")
            list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod_shift_ch2.S)
        endif()
        # Add S3-specific assembly files if they exist
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3.S")
            list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod_s3.S)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3_shift_ch1.S")
            list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod_s3_shift_ch1.S)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3_shift_ch2.S")
            list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod_s3_shift_ch2.S)
        endif()
        # Add S3 Float32 assembly files if they exist
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3_float32_ch1.S")
            list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod_s3_float32_ch1.S)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dotprod/xtensa/xtensa_dotprod_s3_float32_ch2.S")
            list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod_s3_float32_ch2.S)
        endif()
        # Add Q31Lite scalar assembly files (all Xtensa targets) if they exist
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dotprod/xtensa/xtensa_dotprod_q31lite_ch1.S")
            list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod_q31lite_ch1.S)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/dotprod/xtensa/xtensa_dotprod_q31lite_ch2.S")
            list(APPEND RESAMPLER_SOURCES src/dotprod/xtensa/xtensa_dotprod_q31lite_ch2.S)
        endif()
        enable_language(ASM)
    endif()

    add_library(micro-resampler STATIC ${RESAMPLER_SOURCES})

    # Find Boost for quad precision mode (RESAMPLER_ENABLE_QUAD_PRECISION)
    # Use Boost's own CMake config (FindBoost module removed in CMake 3.30+)
    if(POLICY CMP0167)
        cmake_policy(SET CMP0167 NEW)
    endif()
    find_package(Boost QUIET CONFIG)
    if(Boost_FOUND)
        message(STATUS "Boost found: ${Boost_INCLUDE_DIRS}")
    elseif(EXISTS "/opt/homebrew/include/boost")
        # Homebrew on Apple Silicon
        set(Boost_INCLUDE_DIRS "/opt/homebrew/include")
        message(STATUS "Using Homebrew Boost: ${Boost_INCLUDE_DIRS}")
    elseif(EXISTS "/usr/local/include/boost")
        # Homebrew on Intel Mac or manual install
        set(Boost_INCLUDE_DIRS "/usr/local/include")
        message(STATUS "Using system Boost: ${Boost_INCLUDE_DIRS}")
    endif()

    # Consumers see only include/; src/ holds the implementation-private headers.
    target_include_directories(micro-resampler
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    # Boost.Multiprecision backs the opt-in QUAD coefficient precision; without
    # it QUAD falls back to DOUBLE.
    # Only the private headers in src/ include it, so consumers never need it.
    if(Boost_INCLUDE_DIRS)
        target_include_directories(micro-resampler SYSTEM PRIVATE ${Boost_INCLUDE_DIRS})
    endif()

    # Warnings and optional tuning, PRIVATE so none of it reaches a consumer.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(micro-resampler PRIVATE
            -Wall -Wextra -Wpedantic
            # Any function not declared in a header must be static; keeps
            # -Wunused-function able to see dead internal functions. Clang and
            # GCC spell the C++ variant of this check differently.
            $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wmissing-prototypes>
            $<$<CXX_COMPILER_ID:GNU>:-Wmissing-declarations>
            # C++-only (this target also compiles Xtensa assembly): require
            # static_cast/reinterpret_cast over C-style casts
            $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
            $<$<BOOL:${MICRO_RESAMPLER_ENABLE_WERROR}>:-Werror>
        )
        # Host benchmarking tuning; kept off for subprojects, which build with
        # the consuming project's own flags and build type.
        if(MICRO_RESAMPLER_NATIVE_TUNING)
            target_compile_options(micro-resampler PRIVATE -O3 -march=native -ffast-math)
        endif()
    endif()

    # AddressSanitizer + UndefinedBehaviorSanitizer for first-party code. CI's
    # build/test jobs turn this on. The library is compiled with instrumentation
    # PRIVATELY, so no consumer source is instrumented unless it asks: executables
    # that want that too (the unit tests, the resample_cli host example) link the
    # micro-resampler-sanitizers interface target. The sanitizer runtime link
    # flags are INTERFACE on the library itself, because the instrumented archive
    # cannot link without the runtime whether or not the consumer opts in. Kept off by default and
    # separate from the -O/-march tuning above; never enabled together with
    # ENABLE_TSAN.
    if(MICRO_RESAMPLER_ENABLE_SANITIZERS)
        # -fno-sanitize-recover makes UB fatal; otherwise UBSan only prints and tests still pass
        add_library(micro-resampler-sanitizers INTERFACE)
        target_compile_options(micro-resampler-sanitizers INTERFACE
            -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(micro-resampler-sanitizers INTERFACE
            -fsanitize=address,undefined -fno-sanitize-recover=undefined)
        target_compile_options(micro-resampler PRIVATE
            -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(micro-resampler INTERFACE
            -fsanitize=address,undefined -fno-sanitize-recover=undefined)
    endif()

    # Testing
    if(MICRO_RESAMPLER_BUILD_TESTS)
        enable_testing()

        # Download and configure Google Test
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        FetchContent_MakeAvailable(googletest)

        # Unit tests
        add_executable(test_fixed_point tests/unit/test_fixed_point.cpp)
        target_link_libraries(test_fixed_point micro-resampler GTest::gtest_main)

        add_executable(test_sinc tests/unit/test_sinc.cpp)
        target_link_libraries(test_sinc micro-resampler GTest::gtest_main)

        add_executable(test_resampler tests/unit/test_resampler.cpp)
        target_link_libraries(test_resampler micro-resampler GTest::gtest_main)

        add_executable(test_arbitrary_phases tests/unit/test_arbitrary_phases.cpp)
        target_link_libraries(test_arbitrary_phases micro-resampler GTest::gtest_main)

        add_executable(test_auto_optimization tests/unit/test_auto_optimization.cpp)
        target_link_libraries(test_auto_optimization micro-resampler GTest::gtest_main)

        add_executable(test_asrc tests/unit/test_asrc.cpp)
        target_link_libraries(test_asrc micro-resampler GTest::gtest_main)

        add_executable(test_input_formats tests/unit/test_input_formats.cpp)
        target_link_libraries(test_input_formats micro-resampler GTest::gtest_main)

        add_executable(test_design_resolver tests/unit/test_design_resolver.cpp)
        target_link_libraries(test_design_resolver micro-resampler GTest::gtest_main)

        # Quality tests (if files exist)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/quality/coefficient_precision_test.cpp")
            add_executable(coefficient_precision_test tests/quality/coefficient_precision_test.cpp)
            target_link_libraries(coefficient_precision_test micro-resampler)
            target_include_directories(coefficient_precision_test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR})
        endif()

        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/quality/snr_test.cpp")
            add_executable(snr_test tests/quality/snr_test.cpp)
            target_link_libraries(snr_test micro-resampler)
            target_include_directories(snr_test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/src)
        endif()

        # Performance benchmark
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/quality/benchmark_formats.cpp")
            add_executable(benchmark_formats tests/quality/benchmark_formats.cpp)
            target_link_libraries(benchmark_formats micro-resampler)
            target_include_directories(benchmark_formats PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/src)
        endif()

        # Hygiene warnings for first-party test/quality targets, matching the
        # library's own warning set (see cppcheck's unusedFunction rationale
        # in script/cppcheck.sh for why -Wmissing-prototypes matters here too).
        foreach(first_party_target
            test_fixed_point test_sinc test_resampler test_arbitrary_phases
            test_auto_optimization test_asrc test_input_formats
            test_design_resolver
            coefficient_precision_test snr_test benchmark_formats
        )
            if(NOT TARGET ${first_party_target})
                continue()
            endif()

            # White-box tests reach the private headers in src/, some of which
            # include Boost.Multiprecision.
            target_include_directories(${first_party_target} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src)
            if(Boost_INCLUDE_DIRS)
                target_include_directories(${first_party_target} SYSTEM PRIVATE
                    ${Boost_INCLUDE_DIRS})
            endif()
            if(TARGET micro-resampler-sanitizers)
                target_link_libraries(${first_party_target} micro-resampler-sanitizers)
            endif()

            if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                target_compile_options(${first_party_target} PRIVATE
                    -Wall -Wextra
                    # Any function not declared in a header must be static;
                    # keeps -Wunused-function able to see dead internal
                    # functions. Clang and GCC spell the C++ variant of this
                    # check differently.
                    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wmissing-prototypes>
                    $<$<CXX_COMPILER_ID:GNU>:-Wmissing-declarations>
                    -Wold-style-cast
                    $<$<BOOL:${MICRO_RESAMPLER_ENABLE_WERROR}>:-Werror>
                )
            endif()
        endforeach()

        # Register tests with CTest
        include(GoogleTest)
        gtest_discover_tests(test_fixed_point)
        gtest_discover_tests(test_sinc)
        gtest_discover_tests(test_resampler)
        gtest_discover_tests(test_arbitrary_phases)
        gtest_discover_tests(test_auto_optimization)
        gtest_discover_tests(test_asrc)
        gtest_discover_tests(test_input_formats)
        gtest_discover_tests(test_design_resolver)
    endif()
endif()
