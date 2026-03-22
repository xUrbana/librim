# Examples CMake Configuration
add_executable(librim_demo 01_basic_demo.cpp)
add_executable(librim_sensor_network 02_sensor_network_udp.cpp)
add_executable(librim_hybrid_gateway 03_hybrid_gateway.cpp)

# Link explicitly against the core librim interface headers securely
target_link_libraries(librim_demo PRIVATE librim)
target_link_libraries(librim_sensor_network PRIVATE librim)
target_link_libraries(librim_hybrid_gateway PRIVATE librim)

# Append Threading Requirements actively
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

target_link_libraries(librim_demo PRIVATE Threads::Threads)
target_link_libraries(librim_sensor_network PRIVATE Threads::Threads)
target_link_libraries(librim_hybrid_gateway PRIVATE Threads::Threads)
