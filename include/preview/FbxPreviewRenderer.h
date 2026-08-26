#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "include/core/SkImage.h"

class SkCanvas;

namespace DomDownToolPreview
{
enum class FbxPreviewMode
{
    Textured,
    Wireframe,
    TexturedWireframe
};

struct FbxPreviewInteractionSettings
{
    double rotateSpeed = 1.0;
    double zoomSpeed = 1.0;
    double panSpeed = 1.0;
    double moveSpeed = 1.0;
};

class FbxPreviewRenderer
{
  public:
    bool load(const std::vector<std::filesystem::path>& paths, std::string& error);

    void clear();

    [[nodiscard]] bool empty() const { return vertices_.empty() || triangles_.empty(); }

    [[nodiscard]] std::size_t vertexCount() const { return vertices_.size(); }

    [[nodiscard]] std::size_t triangleCount() const { return triangles_.size(); }

    void setMode(FbxPreviewMode mode) { mode_ = mode; }

    [[nodiscard]] FbxPreviewMode mode() const { return mode_; }

    void setInteractionSettings(FbxPreviewInteractionSettings settings)
    {
        interactionSettings_ = settings;
    }

    [[nodiscard]] FbxPreviewInteractionSettings interactionSettings() const
    {
        return interactionSettings_;
    }

    void resetView();

    void rotateBy(double deltaX, double deltaY);

    void panBy(double deltaX, double deltaY);

    void zoomBy(double delta);

    void draw(SkCanvas& canvas, int width, int height, float dpiScale) const;

  private:
    struct Vertex
    {
        std::array<float, 3> position{};
        std::array<float, 2> texCoord{};
    };

    struct Triangle
    {
        std::array<uint32_t, 3> indices{};
        uint32_t textureIndex = 0;
    };

    struct Texture
    {
        sk_sp<SkImage> image;
        std::filesystem::path path;
    };

    std::vector<Vertex> vertices_;
    std::vector<Triangle> triangles_;
    std::vector<Texture> textures_;
    std::array<float, 3> center_{};
    float modelScale_ = 1.F;
    double yaw_ = 0.0;
    double pitch_ = 1.5707963267948966;
    double panX_ = 0.0;
    double panY_ = 0.0;
    double zoom_ = 1.0;
    FbxPreviewInteractionSettings interactionSettings_;
    FbxPreviewMode mode_ = FbxPreviewMode::Textured;
};
} // namespace DomDownToolPreview
