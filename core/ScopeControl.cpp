#include "ScopeControl.h"

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scopedeck
{

ControlChannel::~ControlChannel() { Close(); }

bool ControlChannel::Create(const char* p_Name) { return Map(p_Name, true); }
bool ControlChannel::Open(const char* p_Name)   { return Map(p_Name, false); }

#ifdef _WIN32

bool ControlChannel::Map(const char* p_Name, bool p_Create)
{
    if (m_Block) return true;

    const std::string name = std::string("Local\\") + p_Name;
    HANDLE mapping = p_Create
        ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                             sizeof(ControlBlock), name.c_str())
        : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (!mapping) return false;

    const bool existed = p_Create && GetLastError() == ERROR_ALREADY_EXISTS;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ControlBlock));
    if (!view)
    {
        CloseHandle(mapping);
        return false;
    }

    ControlBlock* block = static_cast<ControlBlock*>(view);

    if (p_Create && !existed)
    {
        // A fresh mapping is zeroed by the OS; stamp it before anyone reads.
        block->previewScalePct.store(kPreviewScaleAuto, std::memory_order_relaxed);
        block->rowStep.store(1, std::memory_order_relaxed);
        block->version.store(kControlVersion, std::memory_order_relaxed);
        block->magic.store(kControlMagic, std::memory_order_release);
    }
    else if (!p_Create &&
             (block->magic.load(std::memory_order_acquire) != kControlMagic ||
              block->version.load(std::memory_order_relaxed) != kControlVersion))
    {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return false;
    }

    m_Block   = block;
    m_Mapping = mapping;
    return true;
}

void ControlChannel::Close()
{
    if (m_Block)   UnmapViewOfFile(m_Block);
    if (m_Mapping) CloseHandle(static_cast<HANDLE>(m_Mapping));
    m_Block   = nullptr;
    m_Mapping = nullptr;
}

#else

// Not yet on other platforms: the Premiere plugin is Windows-only so far.
bool ControlChannel::Map(const char* /*p_Name*/, bool /*p_Create*/) { return false; }
void ControlChannel::Close() {}

#endif

} // namespace scopedeck
