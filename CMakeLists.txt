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
    src/miot_beacon.c
)
target_include_directories(miot-ble-sdk PUBLIC include)
target_compile_options(miot-ble-sdk PRIVATE -Wall -Wextra)

add_executable(test_spec test/test_spec.c)
target_link_libraries(test_spec PRIVATE miot-ble-sdk)

add_executable(test_psk test/test_psk.c)
target_link_libraries(test_psk PRIVATE miot-ble-sdk)

# ---- 每台设备一个示例目录（见 README 设备清单） ----
set(EXAMPLES
    scale_s400
    thermometer
    night_light
    repeller
    button
    button_event
    sensor_hcho
    sensor_door
    sensor_motion
    sensor_leak
    thermo_t3m
)
foreach(ex IN LISTS EXAMPLES)
    string(REPLACE "_" "-" bin "example-${ex}")
    add_executable(${bin} examples/${ex}/main.c)
    target_link_libraries(${bin} PRIVATE miot-ble-sdk)
endforeach()

enable_testing()
add_test(NAME spec COMMAND test_spec)
add_test(NAME psk COMMAND test_psk)
