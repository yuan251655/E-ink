#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_err.h"
#include "product_types.h"

namespace photopainter::product {

struct ModeCoverAsset {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::string system_asset_id;
};

esp_err_t GetModeCoverAsset(Feature feature, ModeCoverAsset* output);

}  // namespace photopainter::product
