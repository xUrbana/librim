add_executable(librim_benchmark benchmark.cpp)

# Link against the core librim interface and formatting library
target_link_libraries(librim_benchmark PRIVATE librim fmt::fmt)

# Append Threading Requirements
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(librim_benchmark PRIVATE Threads::Threads)
