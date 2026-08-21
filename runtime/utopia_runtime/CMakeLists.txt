set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(utopia_runtime STATIC utopia_runtime.cpp)

# The runtime is linked into user programs with a plain C linker (clang);
# sanitizer instrumentation would leave unresolved __ubsan_* symbols, so the
# runtime must be built sanitizer-free regardless of the compiler build.
target_compile_options(utopia_runtime PRIVATE
    -fno-sanitize=all
    -O2
)

install(TARGETS utopia_runtime ARCHIVE DESTINATION lib/utopia/runtime)
