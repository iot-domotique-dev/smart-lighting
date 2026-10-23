#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/types.h>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;

enum httpd_method_t : int { HTTP_GET = 0, HTTP_POST = 1, HTTP_PUT = 2,
                           HTTP_DELETE = 3, HTTP_PATCH = 4, HTTP_ANY = 5 };

struct httpd_req_t {
    httpd_method_t method = HTTP_GET;
    const char* uri = nullptr;
    size_t content_len = 0;
    const char* body = nullptr;
    size_t body_offset = 0;
    void* user_ctx = nullptr;
    std::string authorization;
    std::string contentType;
    std::string status = "200 OK";
    std::string responseType;
    std::string responseBody;
    std::string location;
};

using httpd_handle_t = void*;
using httpd_uri_handler_t = esp_err_t (*)(httpd_req_t* request);

struct httpd_uri_t {
    const char* uri = nullptr;
    httpd_method_t method = HTTP_GET;
    httpd_uri_handler_t handler = nullptr;
    void* user_ctx = nullptr;
};

struct httpd_config_t {
    size_t stack_size = 0;
    bool (*uri_match_fn)(const char*, const char*) = nullptr;
};

namespace smart_http_test {
struct Server {
    httpd_uri_t route = {};
    bool started = false;
    bool failNextResponseSend = false;
};

inline Server& server() {
    static Server value;
    return value;
}

inline void resetServer() { server() = {}; }

inline esp_err_t dispatch(httpd_req_t& request) {
    Server& current = server();
    if (!current.started || current.route.handler == nullptr) return ESP_FAIL;
    request.user_ctx = current.route.user_ctx;
    return current.route.handler(&request);
}
}  // namespace smart_http_test

#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}

inline bool httpd_uri_match_wildcard(const char*, const char*) { return true; }
inline esp_err_t httpd_start(httpd_handle_t* handle, httpd_config_t*) {
    if (handle == nullptr) return ESP_FAIL;
    smart_http_test::resetServer();
    smart_http_test::server().started = true;
    *handle = &smart_http_test::server();
    return ESP_OK;
}
inline esp_err_t httpd_stop(httpd_handle_t) {
    smart_http_test::resetServer();
    return ESP_OK;
}
inline esp_err_t httpd_register_uri_handler(httpd_handle_t handle,
                                            const httpd_uri_t* route) {
    if (handle == nullptr || route == nullptr || route->handler == nullptr)
        return ESP_FAIL;
    smart_http_test::server().route = *route;
    return ESP_OK;
}
inline size_t httpd_req_get_hdr_value_len(httpd_req_t* request,
                                         const char* name) {
    if (request == nullptr || name == nullptr) return 0;
    if (strcmp(name, "Authorization") == 0) return request->authorization.size();
    if (strcmp(name, "Content-Type") == 0) return request->contentType.size();
    return 0;
}
inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* request,
                                            const char* name, char* value,
                                            size_t capacity) {
    if (request == nullptr || name == nullptr || value == nullptr || capacity == 0)
        return ESP_FAIL;
    const std::string* source = nullptr;
    if (strcmp(name, "Authorization") == 0) source = &request->authorization;
    if (strcmp(name, "Content-Type") == 0) source = &request->contentType;
    if (source == nullptr || source->size() >= capacity) return ESP_FAIL;
    memcpy(value, source->c_str(), source->size() + 1);
    return ESP_OK;
}
inline int httpd_req_recv(httpd_req_t* request, char* buffer, size_t capacity) {
    if (request == nullptr || buffer == nullptr || request->body == nullptr ||
        request->body_offset >= request->content_len) return 0;
    const size_t remaining = request->content_len - request->body_offset;
    const size_t count = remaining < capacity ? remaining : capacity;
    memcpy(buffer, request->body + request->body_offset, count);
    request->body_offset += count;
    return static_cast<int>(count);
}
inline esp_err_t httpd_resp_set_status(httpd_req_t* request, const char* status) {
    if (request == nullptr || status == nullptr) return ESP_FAIL;
    request->status = status;
    return ESP_OK;
}
inline esp_err_t httpd_resp_set_type(httpd_req_t* request, const char* type) {
    if (request == nullptr || type == nullptr) return ESP_FAIL;
    request->responseType = type;
    return ESP_OK;
}
inline esp_err_t httpd_resp_set_hdr(httpd_req_t* request, const char* name,
                                    const char* value) {
    if (request == nullptr || name == nullptr || value == nullptr) return ESP_FAIL;
    if (strcmp(name, "Location") == 0) request->location = value;
    return ESP_OK;
}
inline esp_err_t httpd_resp_send(httpd_req_t* request, const char* body,
                                 ssize_t length) {
    if (smart_http_test::server().failNextResponseSend) {
        smart_http_test::server().failNextResponseSend = false;
        return ESP_FAIL;
    }
    if (request == nullptr || length < 0 || (length != 0 && body == nullptr))
        return ESP_FAIL;
    request->responseBody.assign(body == nullptr ? "" : body,
                                 static_cast<size_t>(length));
    return ESP_OK;
}
