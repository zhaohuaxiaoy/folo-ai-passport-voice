# tests/CMakeLists.txt —— 宿主机测试(独立工程,不依赖 ESP-IDF 构建系统)。
# 纯 C 模块直接编;cJSON 内置在 third_party/cJSON,协议测试永远可跑。
cmake_minimum_required(VERSION 3.16)
project(ai_passport_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../main)
set(CJSON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON)
set(FAKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fake)
set(BSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/bsp/include)

enable_testing()

add_compile_options(-Wall -Wextra)

# cJSON 是第三方老代码,其 warning 不升级为 error(-Werror 门禁只约束自有代码)
set_source_files_properties(${CJSON_DIR}/cJSON.c PROPERTIES COMPILE_OPTIONS "-Wno-error")

# --- 状态机 ---
add_executable(test_app_state test_app_state.c ${MAIN_DIR}/app_state.c
               ${FAKE_DIR}/fake_mode.c)
target_include_directories(test_app_state PRIVATE ${MAIN_DIR} ${FAKE_DIR})
add_test(NAME app_state COMMAND test_app_state)

# --- UI 像素数学(仓库既有) ---
add_executable(test_ui_pixel_math test_ui_pixel_math.c ${MAIN_DIR}/ui_pixel_math.c)
target_include_directories(test_ui_pixel_math PRIVATE ${MAIN_DIR})
add_test(NAME ui_pixel_math COMMAND test_ui_pixel_math)

# --- USB 帧协议(纯 C 零依赖,两端同构:companion/serial_frame.py 共享测试意图) ---
add_executable(test_usb_link test_usb_link.c ${MAIN_DIR}/usb_link_framing.c)
target_include_directories(test_usb_link PRIVATE ${MAIN_DIR})
add_test(NAME usb_link COMMAND test_usb_link)

# --- 协议(内置 cJSON,无需 IDF_PATH;app_state.c 提供 APP_WORKFLOW_NAMES) ---
# fake_events.c:app_protocol_dispatch_event 的事件投递桩(轻量,见该文件头注释)
add_executable(test_app_protocol test_app_protocol.c ${MAIN_DIR}/app_protocol.c
               ${MAIN_DIR}/app_state.c ${FAKE_DIR}/fake_mode.c
               ${FAKE_DIR}/fake_events.c ${CJSON_DIR}/cJSON.c)
target_include_directories(test_app_protocol PRIVATE ${MAIN_DIR} ${FAKE_DIR} ${CJSON_DIR})
add_test(NAME app_protocol COMMAND test_app_protocol)

# --- BLE 音频/事件分片打包(纯 C;事件行与 CTRL 解析复用 app_protocol,SRCS 同 test_app_protocol) ---
add_executable(test_ble_audio test_ble_audio.c ${MAIN_DIR}/ble_audio.c
               ${MAIN_DIR}/app_protocol.c ${MAIN_DIR}/app_state.c
               ${FAKE_DIR}/fake_mode.c ${FAKE_DIR}/fake_events.c ${CJSON_DIR}/cJSON.c)
target_include_directories(test_ble_audio PRIVATE ${MAIN_DIR} ${FAKE_DIR} ${CJSON_DIR})
add_test(NAME ble_audio COMMAND test_ble_audio)

# --- wall-clock 校时(纯 C;NVS 桩在测试文件内,settimeofday 走 ESP_PLATFORM 隔离) ---
add_executable(test_time_sync test_time_sync.c ${MAIN_DIR}/time_sync.c)
target_include_directories(test_time_sync PRIVATE ${MAIN_DIR} ${FAKE_DIR})
add_test(NAME time_sync COMMAND test_time_sync)

# --- 音频流管线(fake RTOS 层在宿主编译,真并发线程;编译期拦截"不存在的
#     ESP-IDF API"类回归 —— 见 test_audio_streamer.c 头注释;超时用例
#     注入 200ms(默认即 200,注入保持显式)) ---
find_package(Threads REQUIRED)
add_executable(test_audio_streamer test_audio_streamer.c ${MAIN_DIR}/audio_streamer.c
               ${FAKE_DIR}/fake_rtos.c)
target_include_directories(test_audio_streamer PRIVATE
    ${MAIN_DIR} ${FAKE_DIR} ${BSP_DIR})
target_compile_definitions(test_audio_streamer PRIVATE
    WORKER_EXIT_TIMEOUT_MS=200 CANCEL_DRAIN_TIMEOUT_MS=200)
target_link_libraries(test_audio_streamer PRIVATE Threads::Threads)
add_test(NAME audio_streamer COMMAND test_audio_streamer)
