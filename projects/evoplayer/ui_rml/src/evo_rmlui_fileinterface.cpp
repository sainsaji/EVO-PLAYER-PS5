#include "evo_rmlui_fileinterface.h"
#include "evo_rmlui_bundle.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {

/*
 * One handle struct for both cases so FileHandle (a bare uintptr_t) always
 * points at the same shape regardless of which branch Open() took - Close/
 * Read/Seek/Tell just switch on `memory`.
 */
struct Handle {
    bool memory;
    /* memory case: a cursor over the embedded bundle entry's bytes. */
    const unsigned char* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    /* disk case: the dev/host fallback. */
    FILE* fp = nullptr;
};

} // namespace

Rml::FileHandle EvoRmlFileInterface::Open(const Rml::String& path)
{
    if (const EvoRmlBundleFile* f = evo_rmlui_bundle_find(path)) {
        Handle* h = new Handle();
        h->memory = true;
        h->data = f->data;
        h->size = f->size;
        return reinterpret_cast<Rml::FileHandle>(h);
    }

    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp)
        return 0;
    Handle* h = new Handle();
    h->memory = false;
    h->fp = fp;
    return reinterpret_cast<Rml::FileHandle>(h);
}

void EvoRmlFileInterface::Close(Rml::FileHandle file)
{
    Handle* h = reinterpret_cast<Handle*>(file);
    if (!h) return;
    if (!h->memory && h->fp) fclose(h->fp);
    delete h;
}

size_t EvoRmlFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file)
{
    Handle* h = reinterpret_cast<Handle*>(file);
    if (!h) return 0;
    if (h->memory) {
        size_t remaining = (h->pos < h->size) ? (h->size - h->pos) : 0;
        size_t n = std::min(size, remaining);
        if (n) memcpy(buffer, h->data + h->pos, n);
        h->pos += n;
        return n;
    }
    return fread(buffer, 1, size, h->fp);
}

bool EvoRmlFileInterface::Seek(Rml::FileHandle file, long offset, int origin)
{
    Handle* h = reinterpret_cast<Handle*>(file);
    if (!h) return false;
    if (h->memory) {
        long base = (origin == SEEK_SET) ? 0 :
                    (origin == SEEK_CUR) ? (long)h->pos :
                    (long)h->size; /* SEEK_END */
        long target = base + offset;
        if (target < 0 || (size_t)target > h->size) return false;
        h->pos = (size_t)target;
        return true;
    }
    return fseek(h->fp, offset, origin) == 0;
}

size_t EvoRmlFileInterface::Tell(Rml::FileHandle file)
{
    Handle* h = reinterpret_cast<Handle*>(file);
    if (!h) return 0;
    return h->memory ? h->pos : (size_t)ftell(h->fp);
}
