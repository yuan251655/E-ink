#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
namespace photopainter::product { esp_err_t RegisterProductApi(httpd_handle_t server); }
