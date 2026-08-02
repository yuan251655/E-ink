#pragma once

#include <string>

namespace photopainter::product {

struct XiaozhiRuntimeSnapshot {
    std::string state = "network_unconfigured";
    bool started = false;
    bool wake_word_enabled = false;
    bool active_only = true;
    std::string activation_code;
    std::string last_error_code;
};

// Starts the product supervisor. It never initializes Wi-Fi and it never
// changes any physical button mapping.
void InitializeXiaozhiRuntime();
XiaozhiRuntimeSnapshot GetXiaozhiRuntimeSnapshot();
void RetryXiaozhiRuntime();

}  // namespace photopainter::product
