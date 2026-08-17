set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(utopia_async STATIC utopia_async.cpp)

# The runtime is linked into user programs with a plain C linker (clang);
# sanitizer instrumentation would leave unresolved __ubsan_* symbols, so the
# runtime must be built sanitizer-free regardless of the compiler build.
target_compile_options(utopia_async PRIVATE
    -fno-sanitize=all
    -O2
)

find_package(Threads REQUIRED)
target_link_libraries(utopia_async PUBLIC Threads::Threads)

install(TARGETS utopia_async ARCHIVE DESTINATION lib/utopia/async)
