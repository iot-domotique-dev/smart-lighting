#pragma once

/* Wi-Fi station adapter for C6-WIFI. HTTP/application services are separate. */
class WiFiTransport {
private:
    bool started;

public:
    WiFiTransport();

    bool begin();
    bool connect(const char* ssid, const char* password);
    bool isReady() const;
    const char* name() const;
};
