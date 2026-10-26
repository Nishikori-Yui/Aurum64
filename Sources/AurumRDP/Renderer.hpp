// SPDX-License-Identifier: MIT
#pragma once
#include "RDP.hpp"
#include "Tmem.hpp"
#include <memory>
#include <string>

namespace aurum::rdp {
struct VideoInterface {
    uint32_t status, origin, width, hStart, vStart, xScale, yScale;
};
class Renderer {
public:
    Renderer();
    ~Renderer();
    bool initialize(const char* library, std::string& error);
    bool fill(uint8_t* memory, size_t length, const Fill& fill, std::string& error);
    bool textureCopy(uint8_t* memory, size_t length, const TextureRect& rect,
                     const aurum::tmem::Store& tmem, std::string& error);
    bool scanout(const uint8_t* memory, size_t length, const VideoInterface& vi,
                 std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
