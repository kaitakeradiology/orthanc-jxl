/*
 * Copyright (C) 2025 Ryan Walklin <ryan@kaitakeradiology.co.nz>
 *
 * This file is part of orthanc-jxl.
 *
 * orthanc-jxl is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * orthanc-jxl is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * orthanc-jxl. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"
#include <nlohmann/json.hpp>

namespace orthanc_jxl {

using json = nlohmann::json;

EncodeOptions PluginConfig::GetEncodeOptions(uint32_t imageWidth, uint32_t imageHeight) const {
    EncodeOptions opts = encodeOptions;

    // Apply center-first ordering if enabled
    if (centerFirstOrdering && opts.centerX < 0 && opts.centerY < 0) {
        opts.centerX = static_cast<int>(imageWidth / 2);
        opts.centerY = static_cast<int>(imageHeight / 2);
    }

    return opts;
}

PluginConfig PluginConfig::Default() {
    PluginConfig config;
    config.encodeOptions = EncodeOptions::ProgressiveLossless(7);
    config.centerFirstOrdering = true;
    return config;
}

PluginConfig PluginConfig::Parse(const char* jsonConfig) {
    PluginConfig config = Default();

    if (!jsonConfig) {
        return config;
    }

    try {
        json root = json::parse(jsonConfig);

        // Look for our plugin section
        if (!root.contains("OrthancJxl")) {
            return config;
        }

        const json& section = root["OrthancJxl"];

        // Parse encoding mode
        if (section.contains("Mode")) {
            std::string mode = section["Mode"].get<std::string>();
            if (mode == "Lossless") {
                config.encodeOptions.mode = EncodeMode::Lossless;
            } else if (mode == "ProgressiveLossless") {
                config.encodeOptions.mode = EncodeMode::ProgressiveLossless;
            } else if (mode == "ProgressiveVarDCT") {
                config.encodeOptions.mode = EncodeMode::ProgressiveVarDCT;
            }
        }

        // Parse effort (1-10)
        if (section.contains("Effort")) {
            int effort = section["Effort"].get<int>();
            if (effort >= 1 && effort <= 10) {
                config.encodeOptions.effort = effort;
            }
        }

        // Parse distance (0.0 = lossless)
        if (section.contains("Distance")) {
            float distance = section["Distance"].get<float>();
            if (distance >= 0.0f) {
                config.encodeOptions.distance = distance;
            }
        }

        // Parse center-first ordering
        if (section.contains("CenterFirstOrdering")) {
            config.centerFirstOrdering = section["CenterFirstOrdering"].get<bool>();
        }

        // Parse VarDCT progressive options
        if (section.contains("ProgressiveDC")) {
            int dc = section["ProgressiveDC"].get<int>();
            if (dc >= 0 && dc <= 2) {
                config.encodeOptions.progressiveDC = dc;
            }
        }

        if (section.contains("ProgressiveAC")) {
            config.encodeOptions.progressiveAC = section["ProgressiveAC"].get<bool>();
        }

    } catch (const json::exception&) {
        // Parse error - return default config
        return Default();
    }

    return config;
}

} // namespace orthanc_jxl
