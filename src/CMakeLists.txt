file(GLOB SMART_LIGHTING_SOURCES "${CMAKE_CURRENT_LIST_DIR}/*.cpp")

option(SMART_LIGHTING_BUILD_ZIGBEE "Build the ESP-Zigbee transport" ON)
option(SMART_LIGHTING_BUILD_WIFI "Build the Wi-Fi station transport" OFF)

set(SMART_LIGHTING_COMPONENTS
    nvs_flash
    esp_timer
    driver
)
if(SMART_LIGHTING_BUILD_ZIGBEE)
    list(APPEND SMART_LIGHTING_COMPONENTS espressif__esp-zigbee-lib)
endif()
if(SMART_LIGHTING_BUILD_WIFI)
    list(APPEND SMART_LIGHTING_COMPONENTS esp_wifi esp_event esp_netif)
endif()

idf_component_register(
    SRCS ${SMART_LIGHTING_SOURCES}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/compat"
        "${CMAKE_CURRENT_LIST_DIR}/../include"
    REQUIRES ${SMART_LIGHTING_COMPONENTS}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
if(SMART_LIGHTING_BUILD_ZIGBEE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SMART_LIGHTING_ZIGBEE=1)
endif()
