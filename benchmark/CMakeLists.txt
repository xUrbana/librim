add_executable(librim_bench_server benchmark_server.cpp)
target_link_libraries(librim_bench_server PRIVATE librim fmt::fmt)

add_executable(librim_bench_client benchmark_client.cpp)
target_link_libraries(librim_bench_client PRIVATE librim fmt::fmt)
