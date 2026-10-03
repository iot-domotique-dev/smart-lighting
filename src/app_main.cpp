#if defined(SMART_LIGHTING_ZIGBEE)
extern void setup();
extern void loop();

extern "C" void app_main(void) {
    setup();
    while (true) {
        loop();
    }
}
#endif
