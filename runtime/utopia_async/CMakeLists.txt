cmake_minimum_required(VERSION 3.16)
project(utopia_async CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(utopia_async STATIC utopia_async.cpp)

# The runtime is linked into user programs with a plain C linker (clang);
# sanitizer instrumentation would leave unresolved __ubsan_* symbols, so the
# runtime must be built sanitizer-free regardless of the compiler build.
# -fPIC keeps the archive linkable into shared libraries (utopia projects
# can target shared_library) and into the JIT host, which always emits PIC.
target_compile_options(utopia_async PRIVATE
    -fno-sanitize=all
    -fPIC
    -O2
)

find_package(Threads REQUIRED)
target_link_libraries(utopia_async PUBLIC Threads::Threads)

install(TARGETS utopia_async ARCHIVE DESTINATION lib/utopia/async)
