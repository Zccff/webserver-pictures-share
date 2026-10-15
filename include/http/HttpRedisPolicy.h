#pragma once

#include <string>

bool is_valid_upload_idempotency_key(const std::string& key);

std::string http_response_body(const std::string& response);
