#include "PbrChannels.h"
#include "NativeTextureSet.h"
#include <array>
#include <iostream>

// Only handle identity matters to the renderer binding cache.
struct BedrockTextureData {};

int main() {
    using namespace ysm;
    // Only the second, dielectric LabPBR texel has subsurface scattering.
    std::array<unsigned char, 8> source{255, 230, 255, 255, 0, 25, 255, 254};
    std::array<unsigned char, 8> output{};
    if (!convertPbrImage(source, output, PbrEncoding::LabPbr13, PbrLayer::Material)
        || output != std::array<unsigned char, 8>{255, 0, 0, 0, 0, 255, 255, 255}) return 1;

    source[6] = 65;
    if (convertPbrImage(source, output, PbrEncoding::LabPbr13, PbrLayer::Material)) return 2;
    if (convertPbrImage(source, output, PbrEncoding::OldPbr, PbrLayer::Material)
        || output != std::array<unsigned char, 8>{230, 255, 0, 0, 25, 65, 255, 0}) return 3;

    source = {128, 128, 0, 0, 255, 128, 32, 255};
    if (convertPbrImage(source, output, PbrEncoding::LabPbr13, PbrLayer::Normal)
        || output != std::array<unsigned char, 8>{128, 128, 255, 255, 255, 128, 128, 255}) return 4;
    try {
        convertPbrImage(source, std::span(output).first(4), PbrEncoding::OldPbr, PbrLayer::Material);
        return 5;
    } catch (std::invalid_argument const&) {}
    try {
        convertPbrImage(std::span(source).first(3), std::span(output).first(3), PbrEncoding::OldPbr, PbrLayer::Normal);
        return 6;
    } catch (std::invalid_argument const&) {}
    NativeTextureSet original{std::make_shared<BedrockTextureData>(), std::make_shared<BedrockTextureData>(),
        std::make_shared<BedrockTextureData>(), false};
    NativeTextureSet current = original;
    if (current != original) return 7;
    current.color = std::make_shared<BedrockTextureData>();
    if (current == original) return 8;
    current = original;
    current.normal = std::make_shared<BedrockTextureData>();
    if (current == original) return 9;
    current = original;
    current.material.reset();
    if (current == original) return 10;
    current = original;
    current.hasSubsurfaceChannel = true;
    if (current == original) return 11;
    std::cout << "PBR image conversion and texture binding identity passed\n";
}
