#pragma once

#include <stdint.h>

/* Wi-Fi station adapter for C6-WIFI. HTTP/application services are separate. */
class WiFiTransport {
private:
    bool started;
    bool credentialsConfigured;
    bool reconnectPending;
    uint32_t reconnectAtMs;
    char configuredSsid[33];

    bool requestConnection(bool reconnect);

public:
    WiFiTransport();

    bool begin();
    bool connect(const char* ssid, const char* password);
    void poll();
    bool isReady() const;
    const char* name() const;
};
