// Minimal GL texture wrapper.
//
// Phase 1 draws every scope by building an image on the CPU and uploading it, so
// the only GL this app needs is glGenTextures/glBindTexture/glTexImage2D and
// friends - all GL 1.1, exported directly by opengl32.dll on Windows and by
// OpenGL.framework on macOS. That is why there is no GL function loader in this
// tree yet: nothing here calls a GL 2.0+ entry point.
//
// Dear ImGui's own OpenGL3 backend does use modern GL, but it carries its own
// private loader (imgui_impl_opengl3_loader.h) and does not expose it - which is
// deliberate on its part, and why we must not borrow it.
//
// The first real shader (GPU binning, or the Enhanced Render line trace) is the
// point at which a loader has to arrive. See NEXT_STEPS.md.

#pragma once

#include <cstdint>

namespace scopedeck
{

class Texture
{
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // Movable, because panels live in a std::vector and closing one in the middle
    // has to shuffle the rest along. Copying stays deleted: two Texture objects
    // owning one GL name would delete it twice.
    Texture(Texture&& p_Other) noexcept { Steal(p_Other); }

    Texture& operator=(Texture&& p_Other) noexcept
    {
        if (this != &p_Other) { Release(); Steal(p_Other); }
        return *this;
    }

    // Uploads 8-bit RGB, reallocating only when the size actually changes -
    // glTexSubImage2D into an existing texture is materially cheaper than
    // glTexImage2D, and the preview size is constant for long stretches.
    void UploadRGB(const uint8_t* p_Pixels, int p_Width, int p_Height);

    // Same, for the RGBA images the scope trace builders produce.
    void UploadRGBA(const uint8_t* p_Pixels, int p_Width, int p_Height);

    bool  Valid()  const { return m_Id != 0; }
    int   Width()  const { return m_Width; }
    int   Height() const { return m_Height; }

    // For ImGui::Image. ImTextureID is a void*-sized handle, and a GL texture name
    // is a uint32 - the cast through uintptr_t is what ImGui's own examples do.
    void* ImGuiHandle() const { return reinterpret_cast<void*>(static_cast<uintptr_t>(m_Id)); }

private:
    void Ensure(int p_Width, int p_Height, uint32_t p_InternalFormat);
    void Release();

    void Steal(Texture& p_Other) noexcept
    {
        m_Id             = p_Other.m_Id;
        m_Width          = p_Other.m_Width;
        m_Height         = p_Other.m_Height;
        m_InternalFormat = p_Other.m_InternalFormat;
        p_Other.m_Id     = 0;   // the source must not delete the name we took
        p_Other.m_Width  = 0;
        p_Other.m_Height = 0;
    }

    uint32_t m_Id             = 0;
    int      m_Width          = 0;
    int      m_Height         = 0;
    uint32_t m_InternalFormat = 0;
};

} // namespace scopedeck
