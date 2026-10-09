#include "Texture.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#endif

#ifdef __APPLE__
  #include <OpenGL/gl.h>
#else
  #include <GL/gl.h>
#endif

// Windows ships an OpenGL 1.1 header and has never updated it, so a handful of
// enums this file uses are simply absent even though the driver has supported
// them for two decades. Only the *enum values* are missing - these are not
// function entry points and need no loader; the value is fixed by the spec.
//
// GL_CLAMP_TO_EDGE arrived in GL 1.2. Without it the only clamp available is
// GL_CLAMP, which blends the border colour into the outermost texel and draws a
// dark fringe along every edge of a scaled-up trace.
#ifndef GL_CLAMP_TO_EDGE
  #define GL_CLAMP_TO_EDGE 0x812F
#endif

// GL_RGBA8 / GL_RGB8 are GL 1.1 sized internal formats and are present in the
// Microsoft header, but guard them anyway so a future platform header that omits
// them fails at the definition rather than somewhere confusing.
#ifndef GL_RGBA8
  #define GL_RGBA8 0x8058
#endif
#ifndef GL_RGB8
  #define GL_RGB8 0x8051
#endif

namespace scopedeck
{

Texture::~Texture()
{
    Release();
}

void Texture::Release()
{
    if (m_Id == 0) return;

    const GLuint id = m_Id;
    glDeleteTextures(1, &id);
    m_Id = 0;
}

void Texture::Ensure(int p_Width, int p_Height, uint32_t p_InternalFormat)
{
    if (m_Id == 0)
    {
        GLuint id = 0;
        glGenTextures(1, &id);
        m_Id = id;

        glBindTexture(GL_TEXTURE_2D, m_Id);

        // Linear min/mag: these images are stretched to whatever size the panel
        // happens to be, and a nearest-neighbour waveform at a non-integer scale
        // shows aliasing artefacts that read as real structure in the signal.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Clamp, so the edge column of a trace does not wrap around and paint a
        // ghost of the opposite edge when the panel scales it up.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        m_Width = m_Height = 0;
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, m_Id);
    }

    if (m_Width != p_Width || m_Height != p_Height || m_InternalFormat != p_InternalFormat)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(p_InternalFormat), p_Width, p_Height, 0,
                     p_InternalFormat == GL_RGBA8 ? GL_RGBA : GL_RGB,
                     GL_UNSIGNED_BYTE, nullptr);
        m_Width          = p_Width;
        m_Height         = p_Height;
        m_InternalFormat = p_InternalFormat;
    }
}

void Texture::UploadRGB(const uint8_t* p_Pixels, int p_Width, int p_Height)
{
    if (!p_Pixels || p_Width <= 0 || p_Height <= 0) return;

    Ensure(p_Width, p_Height, GL_RGB8);

    // Rows are tightly packed - 3 bytes per pixel means a row is only 4-byte
    // aligned when the width happens to be a multiple of 4. GL's default unpack
    // alignment of 4 would then skew every row of an odd-width preview, which
    // looks like a diagonal shear rather than an obvious failure.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p_Width, p_Height, GL_RGB, GL_UNSIGNED_BYTE, p_Pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void Texture::UploadRGBA(const uint8_t* p_Pixels, int p_Width, int p_Height)
{
    if (!p_Pixels || p_Width <= 0 || p_Height <= 0) return;

    Ensure(p_Width, p_Height, GL_RGBA8);
    // 4 bytes per pixel is always 4-byte aligned, so no unpack change needed.
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p_Width, p_Height, GL_RGBA, GL_UNSIGNED_BYTE, p_Pixels);
}

} // namespace scopedeck
