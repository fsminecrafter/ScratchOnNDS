// =============================================================================
// zip_loader.h / zip_loader.cpp
// Extracts a .sb3 (ZIP) file to a directory on the SD card.
// Uses miniz (single-header, embedded below) — no external dependency needed.
// =============================================================================
#pragma once
#include <string>

class ZipLoader {
public:
    // Extract zipPath -> destDir (creates subdirs as needed)
    // Returns true on success
    bool extract(const char* zipPath, const char* destDir);

private:
    bool ensureDir(const char* path);
    bool writeFile(const char* path, const void* data, size_t size);
};
