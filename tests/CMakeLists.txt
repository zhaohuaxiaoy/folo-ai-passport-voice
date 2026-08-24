# tests/CMakeLists.txt —— 宿主机测试(独立工程,不依赖 ESP-IDF 构建系统)。
# 纯 C 模块直接编;cJSON 内置在 third_party/cJSON,协议测试永远可跑。
cmake_minimum_required(VERSION 3.16)
project(ai_passport_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../main)
set(CJSON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON)

enable_testing()

add_compile_options(-Wall -Wextra)

# cJSON 是第三方老代码,其 warning 不升级为 error(-Werror 门禁只约束自有代码)
set_source_files_properties(${CJSON_DIR}/cJSON.c PROPERTIES COMPILE_OPTIONS "-Wno-error")

# --- 状态机 ---
add_executable(test_app_state test_app_state.c ${MAIN_DIR}/app_state.c)
target_include_directories(test_app_state PRIVATE ${MAIN_DIR})
add_test(NAME app_state COMMAND test_app_state)

# --- UI 像素数学(仓库既有) ---
add_executable(test_ui_pixel_math test_ui_pixel_math.c ${MAIN_DIR}/ui_pixel_math.c)
target_include_directories(test_ui_pixel_math PRIVATE ${MAIN_DIR})
add_test(NAME ui_pixel_math COMMAND test_ui_pixel_math)

# --- 协议(内置 cJSON,无需 IDF_PATH;app_state.c 提供 APP_WORKFLOW_NAMES) ---
add_executable(test_app_protocol test_app_protocol.c ${MAIN_DIR}/app_protocol.c
               ${MAIN_DIR}/app_state.c ${CJSON_DIR}/cJSON.c)
target_include_directories(test_app_protocol PRIVATE ${MAIN_DIR} ${CJSON_DIR})
add_test(NAME app_protocol COMMAND test_app_protocol)

# --- BLE 音频/事件分片打包(纯 C;事件行与 CTRL 解析复用 app_protocol,SRCS 同 test_app_protocol) ---
add_executable(test_ble_audio test_ble_audio.c ${MAIN_DIR}/ble_audio.c
               ${MAIN_DIR}/app_protocol.c ${MAIN_DIR}/app_state.c ${CJSON_DIR}/cJSON.c)
target_include_directories(test_ble_audio PRIVATE ${MAIN_DIR} ${CJSON_DIR})
add_test(NAME ble_audio COMMAND test_ble_audio)
