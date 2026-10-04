#include "core_uart_transport.h"

#if defined(SMART_LIGHTING_CORE_UART)

#include <Arduino.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CORE_LINK_UART_PORT
#define CORE_LINK_UART_PORT 1
#endif
#ifndef CORE_LINK_UART_TX_PIN
#define CORE_LINK_UART_TX_PIN 4
#endif
#ifndef CORE_LINK_UART_RX_PIN
#define CORE_LINK_UART_RX_PIN 5
#endif
#ifndef CORE_LINK_UART_BAUD
#define CORE_LINK_UART_BAUD 115200
#endif

namespace {
constexpr size_t UART_RX_BUFFER_SIZE = 512;
constexpr size_t UART_TX_BUFFER_SIZE = 512;
}

CoreUartTransport::CoreUartTransport(int uartPort)
    : port(uartPort < 0 ? CORE_LINK_UART_PORT : uartPort), started(false) {}

bool CoreUartTransport::begin() {
    if (started) return true;

    uart_config_t config = {};
    config.baud_rate = CORE_LINK_UART_BAUD;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;

    const uart_port_t uartPort = static_cast<uart_port_t>(port);
    if (uart_param_config(uartPort, &config) != ESP_OK ||
        uart_set_pin(uartPort, CORE_LINK_UART_TX_PIN, CORE_LINK_UART_RX_PIN,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        Serial.println("[CORE-UART] UART configuration failed");
        return false;
    }

    const esp_err_t installResult = uart_driver_install(
        uartPort,
        UART_RX_BUFFER_SIZE,
        UART_TX_BUFFER_SIZE,
        0,
        nullptr,
        0
    );
    if (installResult != ESP_OK && installResult != ESP_ERR_INVALID_STATE) {
        Serial.println("[CORE-UART] UART driver installation failed");
        return false;
    }

    started = true;
    return true;
}

bool CoreUartTransport::send(const CoreLinkPacket& packet) {
    if (!started) return false;

    uint8_t frame[CORE_LINK_MAX_FRAME_SIZE] = {};
    size_t frameLength = 0;
    if (!encodeCoreLinkFrame(packet, frame, sizeof(frame), frameLength)) {
        return false;
    }
    const int written = uart_write_bytes(
        static_cast<uart_port_t>(port),
        frame,
        frameLength
    );
    return written == static_cast<int>(frameLength) &&
           uart_wait_tx_done(static_cast<uart_port_t>(port),
                             pdMS_TO_TICKS(100)) == ESP_OK;
}

bool CoreUartTransport::receive(CoreLinkPacket& packet) {
    if (!started) return false;

    uint8_t byte = 0;
    while (uart_read_bytes(static_cast<uart_port_t>(port), &byte, 1, 0) == 1) {
        if (decoder.pushByte(byte, packet)) {
            return true;
        }
    }
    return false;
}

bool CoreUartTransport::isReady() const {
    return started;
}

const char* CoreUartTransport::name() const {
    return "CORE_UART";
}

uint16_t CoreUartTransport::decoderErrorCount() const {
    return decoder.errors();
}

#else

CoreUartTransport::CoreUartTransport(int uartPort)
    : port(uartPort), started(false) {}

bool CoreUartTransport::begin() { return false; }
bool CoreUartTransport::send(const CoreLinkPacket&) { return false; }
bool CoreUartTransport::receive(CoreLinkPacket&) { return false; }
bool CoreUartTransport::isReady() const { return started; }
const char* CoreUartTransport::name() const { return "CORE_UART_UNAVAILABLE"; }
uint16_t CoreUartTransport::decoderErrorCount() const { return decoder.errors(); }

#endif
