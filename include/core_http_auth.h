#pragma once

#include <stddef.h>

bool coreHttpTokenConfigured(const char* configuredToken);
bool coreHttpHasValidBearer(const char* authorization, size_t headerLength,
                            const char* configuredToken);
