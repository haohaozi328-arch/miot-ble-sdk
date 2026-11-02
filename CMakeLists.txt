cmake_minimum_required(VERSION 3.13)
project(miot-ble-sdk C)

set(CMAKE_C_STANDARD 11)

add_library(miot-ble-sdk STATIC
    src/miot_spec.c
    src/miot_props.c
    src/miot_crypto.c
    src/miot_ag2.c
    src/miot_device.c
    src/miot_auth.c
    src/miot_profiles.c
)
target_include_directories(miot-ble-sdk PUBLIC include)
target_compile_options(miot-ble-sdk PRIVATE -Wall -Wextra)

add_executable(test_spec test/test_spec.c)
target_link_libraries(test_spec PRIVATE miot-ble-sdk)

add_executable(example_scale
    examples/scale_s400/main.c
)
target_link_libraries(example_scale PRIVATE miot-ble-sdk)

enable_testing()
add_test(NAME spec COMMAND test_spec)
