#include "core_http_brightness_parser.h"

#include <string.h>

bool parseCoreBrightnessRequestBody(const char* body, size_t length,
                                    int32_t& percent) {
    if (body == nullptr || length == 0 || length > 64) return false;
    const char* cursor = body;
    const char* end = body + length;
    const auto skipWhitespace = [&cursor, end]() {
        while (cursor != end && (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n')) ++cursor;
    };
    skipWhitespace();
    if (cursor == end || *cursor++ != '{') return false;
    skipWhitespace();
    constexpr char PERCENT_KEY[] = "\"percent\"";
    if (static_cast<size_t>(end - cursor) < sizeof(PERCENT_KEY) - 1 ||
        memcmp(cursor, PERCENT_KEY, sizeof(PERCENT_KEY) - 1) != 0) return false;
    cursor += sizeof(PERCENT_KEY) - 1;
    skipWhitespace();
    if (cursor == end || *cursor++ != ':') return false;
    skipWhitespace();
    if (cursor == end || *cursor < '0' || *cursor > '9') return false;
    if (*cursor == '0' && cursor + 1 != end && cursor[1] >= '0' && cursor[1] <= '9')
        return false;
    int32_t parsed = 0;
    do {
        parsed = parsed * 10 + (*cursor++ - '0');
        if (parsed > 100) return false;
    } while (cursor != end && *cursor >= '0' && *cursor <= '9');
    skipWhitespace();
    if (cursor == end || *cursor++ != '}') return false;
    skipWhitespace();
    if (cursor != end) return false;
    percent = parsed;
    return true;
}
