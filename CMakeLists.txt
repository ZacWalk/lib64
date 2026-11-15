cmake_minimum_required(VERSION 3.21)

project(lib64 LANGUAGES CXX VERSION 1.0.0)

# The emulator core is standard C++20 with no platform surface: it builds with
# MSVC on Windows (x64, x86, ARM64) and GCC on Linux. The Win32 test app is
# Windows-only, and neither the app nor the suite is built when another project
# brings lib64 in through add_subdirectory or FetchContent.
include(CMakeDependentOption)
cmake_dependent_option(LIB64_BUILD_APP "Build the Windows test app" ON "WIN32;PROJECT_IS_TOP_LEVEL" OFF)

if(LIB64_BUILD_APP)
    # Use the sibling checkout while developing, but keep a published commit
    # in the project so a clean configure is reproducible.
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIBDESKTOP
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libdesktop/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_LIBDESKTOP "${CMAKE_CURRENT_SOURCE_DIR}/../libdesktop"
            CACHE PATH "Local libdesktop checkout to build against")
    endif()

    include(FetchContent)
    FetchContent_Declare(libdesktop
        GIT_REPOSITORY https://github.com/ZacWalk/libdesktop.git
        GIT_TAG        d172a12c6e3df0831a641133b96d073559a99db5
    )
    FetchContent_MakeAvailable(libdesktop)
endif()

function(lib64_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /MP /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Every build tree writes its executables to bin/, so a 32-bit tree carries the
# width in their names, as the Visual Studio projects did, rather than
# overwriting the 64-bit ones.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(lib64_bits 32)
else()
    set(lib64_bits 64)
endif()

# The emulator core: no platform surface, so the tests can drive it headlessly.
add_library(lib64 STATIC
    src/assembler.cpp
    src/cartridge.cpp
    src/cpu.cpp
    src/debug.cpp
    src/disk.cpp
    src/machine.cpp
    src/opcodes.cpp
    src/rom-chars.cpp
    src/rom-kernal-basic.cpp
    src/rom-kernal.cpp
    src/sid.cpp
    src/vic.cpp
)
add_library(lib64::lib64 ALIAS lib64)

target_compile_features(lib64 PUBLIC cxx_std_20)
target_include_directories(lib64 PUBLIC src)
lib64_warnings(lib64)

if(LIB64_BUILD_APP)
    libdesktop_add_app(app8bit
        SOURCES     app/app.cpp
        ICON        app/res/c64.ico
        DESCRIPTION "lib64 - Commodore 64 emulator"
        OUTPUT_NAME app8bit
    )
    target_link_libraries(app8bit PRIVATE lib64::lib64 libdesktop::ui)
    lib64_warnings(app8bit)
    if(MSVC)
        target_compile_options(app8bit PRIVATE /EHsc)
    endif()

    set_target_properties(app8bit PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin"
    )
    # The historical names, app8bit64r / app8bit64d, rather than a plain suffix.
    set_target_properties(app8bit PROPERTIES
        OUTPUT_NAME_DEBUG app8bit${lib64_bits}d
        OUTPUT_NAME_RELEASE app8bit${lib64_bits}r
    )
endif()

if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    # The emulator's own console suite, which exercises the core directly.
    add_executable(lib64_tests test/emulator-tests.cpp)
    target_link_libraries(lib64_tests PRIVATE lib64::lib64)
    lib64_warnings(lib64_tests)
    set_target_properties(lib64_tests PROPERTIES
        DEBUG_POSTFIX d
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin"
    )
    if(lib64_bits EQUAL 32)
        set_target_properties(lib64_tests PROPERTIES OUTPUT_NAME lib64_tests32)
    endif()
    add_test(NAME lib64_emulator COMMAND lib64_tests)
    set_tests_properties(lib64_emulator PROPERTIES TIMEOUT 300 LABELS "lib64;headless")

    if(LIB64_BUILD_APP)
        add_test(NAME lib64_app_manifest COMMAND "${CMAKE_COMMAND}"
            "-DEXE=$<TARGET_FILE:app8bit>" -P "${PROJECT_SOURCE_DIR}/test/manifest.cmake")
        set_tests_properties(lib64_app_manifest PROPERTIES TIMEOUT 30 LABELS "lib64;headless")

        add_test(NAME lib64_app_headless COMMAND "$<TARGET_FILE:app8bit>" -test)
        set_tests_properties(lib64_app_headless PROPERTIES TIMEOUT 120 LABELS "lib64;headless")
    endif()
endif()
