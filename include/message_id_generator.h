#pragma once

#include <Arduino.h>

uint32_t generateMessageId();

// Seed once at CORE-WIFI boot to reduce collisions with a live LAMP cache.
void seedMessageIds(uint32_t firstId);
