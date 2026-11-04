cmake_minimum_required(VERSION 3.20)
project(fastdl_mm VERSION 1.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# HLDS is 32-bit. MSVC gets this from the generator; GCC needs it in the flags
# before any target, so libmicrohttpd is built the same way.
if(NOT MSVC)
    foreach(_flags CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS
                   CMAKE_SHARED_LINKER_FLAGS)
        set(${_flags} "${${_flags}} -m32")
    endforeach()
endif()

# 32-bit off_t would cap transfers at 2 GiB. Global so every translation unit
# agrees on off_t and struct stat.
add_compile_definitions(_FILE_OFFSET_BITS=64)

set(FASTDL_MM_SDK_ROOT "" CACHE PATH
    "Directory containing cssdk/ and metamod/ (for example hitbox_fixer/include)")

if(NOT FASTDL_MM_SDK_ROOT)
    get_filename_component(_default_sdk "${CMAKE_CURRENT_SOURCE_DIR}/../hitbox_fixer/include" ABSOLUTE)
    set(FASTDL_MM_SDK_ROOT "${_default_sdk}" CACHE PATH "" FORCE)
endif()

foreach(_required
    "${FASTDL_MM_SDK_ROOT}/cssdk/common"
    "${FASTDL_MM_SDK_ROOT}/cssdk/dlls"
    "${FASTDL_MM_SDK_ROOT}/cssdk/engine"
    "${FASTDL_MM_SDK_ROOT}/metamod")
    if(NOT IS_DIRECTORY "${_required}")
        message(FATAL_ERROR "Missing SDK directory: ${_required}. Set FASTDL_MM_SDK_ROOT.")
    endif()
endforeach()

set(MHD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libmicrohttpd")
set(MHD_SOURCES
    ${MHD_ROOT}/microhttpd/connection.c
    ${MHD_ROOT}/microhttpd/reason_phrase.c
    ${MHD_ROOT}/microhttpd/daemon.c
    ${MHD_ROOT}/microhttpd/internal.c
    ${MHD_ROOT}/microhttpd/memorypool.c
    ${MHD_ROOT}/microhttpd/response.c
    ${MHD_ROOT}/microhttpd/tsearch.c
    ${MHD_ROOT}/microhttpd/postprocessor.c
    ${MHD_ROOT}/microhttpd/digestauth.c
    ${MHD_ROOT}/microhttpd/md5.c
    ${MHD_ROOT}/microhttpd/basicauth.c
    ${MHD_ROOT}/microhttpd/base64.c)
if(WIN32)
    list(APPEND MHD_SOURCES ${MHD_ROOT}/platform/w32functions.c)
endif()

add_library(microhttpd STATIC ${MHD_SOURCES})
target_include_directories(microhttpd PUBLIC ${MHD_ROOT}/include PRIVATE ${MHD_ROOT}/microhttpd)
target_compile_definitions(microhttpd PRIVATE BUILDING_MHD_LIB)
# Linked into a shared library.
set_target_properties(microhttpd PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(microhttpd PRIVATE /W3)
else()
    # MHD's Linux config uses MHD_USE_POSIX_THREADS.
    find_package(Threads REQUIRED)
    target_link_libraries(microhttpd PUBLIC Threads::Threads)
    target_compile_options(microhttpd PRIVATE -w)
endif()

add_library(fastdl_mm SHARED
    src/meta_plugin.cpp
    src/fastdl_server.cpp
    src/config_parser.cpp
    src/path_resolver.cpp
    src/rate_limiter.cpp
    src/logger.cpp)

if(WIN32)
    target_sources(fastdl_mm PRIVATE src/fastdl_mm.def)
endif()

target_include_directories(fastdl_mm PRIVATE src)
# SYSTEM: the HLSDK and Metamod headers raise warnings we do not control.
target_include_directories(fastdl_mm SYSTEM PRIVATE
    ${FASTDL_MM_SDK_ROOT}/cssdk/common
    ${FASTDL_MM_SDK_ROOT}/cssdk/dlls
    ${FASTDL_MM_SDK_ROOT}/cssdk/engine
    ${FASTDL_MM_SDK_ROOT}/cssdk/public
    ${FASTDL_MM_SDK_ROOT}/metamod
    ${MHD_ROOT}/include)

target_compile_definitions(fastdl_mm PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    FASTDL_MM_VERSION="${PROJECT_VERSION}")

target_link_libraries(fastdl_mm PRIVATE microhttpd)
if(WIN32)
    target_link_libraries(fastdl_mm PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(fastdl_mm PRIVATE /W4 /EHsc)
    target_compile_options(microhttpd PRIVATE /MT$<$<CONFIG:Debug>:d>)
    target_compile_options(fastdl_mm PRIVATE /MT$<$<CONFIG:Debug>:d>)
    target_link_options(fastdl_mm PRIVATE /MACHINE:X86)
else()
    # cvar_t takes char*, so literal initialisers are unavoidable.
    target_compile_options(fastdl_mm PRIVATE -Wall -Wextra -Wno-write-strings)
    # HLDS ships its own libstdc++ (GLIBCXX_3.4.16) and runs with
    # LD_LIBRARY_PATH=., so that copy shadows the system one and a modern
    # toolchain's symbols go unresolved. Link the runtime in, and keep its
    # symbols hidden so they cannot interpose on the rest of the process.
    target_link_options(fastdl_mm PRIVATE
        -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/fastdl_mm.map")
    set_target_properties(fastdl_mm PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/fastdl_mm.map")
endif()

# Metamod's Linux plugins carry the arch suffix by convention.
set_target_properties(fastdl_mm PROPERTIES
    PREFIX ""
    OUTPUT_NAME $<IF:$<BOOL:${WIN32}>,fastdl_mm,fastdl_mm_i386>)

add_custom_command(TARGET fastdl_mm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/fastdl_mm.cfg"
        "$<TARGET_FILE_DIR:fastdl_mm>/fastdl_mm.cfg"
    COMMENT "Copying fastdl_mm.cfg beside the plugin binary")

include(CTest)
if(BUILD_TESTING)
    # fastdl_server and microhttpd are here for one test: the request line
    # parser is only reachable over a socket, so that case needs a live server.
    add_executable(fastdl_tests
        tests/tests.cpp
        src/config_parser.cpp
        src/fastdl_server.cpp
        src/path_resolver.cpp
        src/rate_limiter.cpp
        src/logger.cpp)
    target_include_directories(fastdl_tests PRIVATE src)
    target_link_libraries(fastdl_tests PRIVATE microhttpd)
    if(MSVC)
        target_compile_options(fastdl_tests PRIVATE /W4 /EHsc /MT$<$<CONFIG:Debug>:d>)
        target_link_libraries(fastdl_tests PRIVATE ws2_32)
    else()
        target_compile_options(fastdl_tests PRIVATE -Wall -Wextra)
        find_package(Threads REQUIRED)
        target_link_libraries(fastdl_tests PRIVATE Threads::Threads)
    endif()
    add_test(NAME fastdl_tests COMMAND fastdl_tests)
endif()

# Compiled rather than read from CMAKE_SIZEOF_VOID_P, which was probed before
# -m32 reached the flags and would still say 8 on a 64-bit host.
include(CheckTypeSize)
check_type_size("void*" FASTDL_MM_POINTER_SIZE LANGUAGE CXX)
if(NOT FASTDL_MM_POINTER_SIZE EQUAL 4)
    message(FATAL_ERROR
        "HLDS/Metamod is 32-bit but this configuration builds ${FASTDL_MM_POINTER_SIZE}-byte "
        "pointers. Use a Win32/x86 generator, or install 32-bit development libraries "
        "(g++-multilib, libc6-dev-i386).")
endif()
