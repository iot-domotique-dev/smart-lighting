#if defined(SMART_LIGHTING_ZIGBEE) || defined(SMART_LIGHTING_CORE_WIFI)
extern void setup();
extern void loop();

extern "C" void app_main(void) {
    setup();
    while (true) {
        loop();
    }
}
#endif
