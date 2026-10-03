#pragma once

enum class DeviceRole {

    CORE,
    MAIN,
    LAMP,
    RELAY,
    SENSOR,
    CAMERA

};


#if defined(DEVICE_ROLE_CORE)

    constexpr DeviceRole DEVICE_ROLE = DeviceRole::CORE;

#elif defined(DEVICE_ROLE_MAIN)

    constexpr DeviceRole DEVICE_ROLE = DeviceRole::MAIN;

#elif defined(DEVICE_ROLE_LAMP)

    constexpr DeviceRole DEVICE_ROLE = DeviceRole::LAMP;

#elif defined(DEVICE_ROLE_RELAY)

    constexpr DeviceRole DEVICE_ROLE = DeviceRole::RELAY;

#else

    #error "Aucun role de module defini"

#endif
