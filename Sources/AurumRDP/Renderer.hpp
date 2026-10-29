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
    bool texturedTriangle(uint8_t* memory, size_t length, const Triangle& triangle,
                          const aurum::tmem::Store& tmem, std::string& error);
    bool combinerTriangle(uint8_t* memory, size_t length, const Triangle& triangle,
                          const aurum::tmem::Store& tmem, std::string& error);
    bool depthTriangle(uint8_t* memory, size_t length, const Triangle& triangle,
                       std::string& error);
    // Scissor arguments retain the RDP's raw 10.2 fixed-point coordinates.
    // Direct-shade mode evaluates attributes in Metal; otherwise uses color.
    bool flatTriangle(uint8_t* memory, size_t length, uint32_t framebufferAddress,
                      uint32_t framebufferWidth, uint32_t framebufferSize,
                      uint16_t color, const Triangle& triangle,
                      int32_t clipXh, int32_t clipYh,
                      int32_t clipXl, int32_t clipYl, std::string& error);
    bool scanout(const uint8_t* memory, size_t length, const VideoInterface& vi,
                 std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
