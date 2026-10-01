file(GLOB SMART_LIGHTING_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")

idf_component_register(
    SRCS ${SMART_LIGHTING_SOURCES}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/compat"
        "${CMAKE_CURRENT_LIST_DIR}/../include"
    REQUIRES
        espressif__esp-zigbee-lib
        nvs_flash
        esp_timer
        driver
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SMART_LIGHTING_ZIGBEE=1)
