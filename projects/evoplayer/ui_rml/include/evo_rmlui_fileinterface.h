#pragma once
/*
 * #60: RmlUi::FileInterface that serves .rml/.rcss/.ttf out of the embedded
 * bundle (evo_rmlui_bundle.h) before ever touching disk, then falls back to
 * plain fopen() for anything not bundled (host dev running against loose
 * files under projects/evoplayer/assets/). Registered via
 * Rml::SetFileInterface() in EvoRmlApp::Initialize(), before Rml::Initialise().
 *
 * Textures (.png) do NOT go through this - RmlUi's render interface loads
 * those itself via stbi_load() rather than Rml::FileInterface, so the same
 * embedded-first/disk-fallback lookup is duplicated in
 * EvoRenderInterface::LoadTexture (evo_rmlui_render.cpp) using the same
 * evo_rmlui_bundle_find().
 */
#include <RmlUi/Core/FileInterface.h>

class EvoRmlFileInterface : public Rml::FileInterface {
public:
    EvoRmlFileInterface() = default;
    ~EvoRmlFileInterface() override = default;

    Rml::FileHandle Open(const Rml::String& path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
};
