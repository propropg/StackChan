/*
 * Blocking HTTPS GET (certificate bundle, redirects followed) shared by the cloud clients
 */
#pragma once
#include "psram_allocator.h"
#include <string>

// Reads the whole body into out (at most max_size bytes). Returns false only on transport errors;
// check status for HTTP errors.
bool https_get(const std::string& url, size_t max_size, PsramBytes& out, int& status, std::string& error);
