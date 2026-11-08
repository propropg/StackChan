/*
 * Blocking HTTPS GET / POST (certificate bundle, redirects followed for GET) shared by the cloud clients
 */
#pragma once
#include "psram_allocator.h"
#include <string>
#include <utility>
#include <vector>

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// Reads the whole body into out (at most max_size bytes). Returns false only on transport errors;
// check status for HTTP errors.
bool https_get(const std::string& url, size_t max_size, PsramBytes& out, int& status, std::string& error);

// POST body with the given headers (Content-Type included) and read the response like https_get.
// timeout_ms covers each socket operation; LLM replies can take a while.
bool https_post(const std::string& url, const HttpHeaders& headers, const uint8_t* body, size_t body_size,
                size_t max_size, PsramBytes& out, int& status, std::string& error, int timeout_ms = 30000);
