// =============================================================================
// zip_loader.cpp
// Extracts a .sb3 (ZIP) file using miniz (single-header).
// miniz.h must be present alongside this file — grab the amalgamated
// single-header release from https://github.com/richgel999/miniz
// =============================================================================
#include "zip_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Pull in miniz — define the implementation exactly once here.
#define MINIZ_IMPLEMENTATION
#include "miniz.h"

bool ZipLoader::extract(const char* zipPath, const char* destDir) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, zipPath, 0)) {
        return false;
    }

    ensureDir(destDir);

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    bool ok = true;

    for (mz_uint i = 0; i < numFiles; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            ok = false;
            continue;
        }

        // Build destination path
        char destPath[512];
        snprintf(destPath, sizeof(destPath), "%s/%s", destDir, stat.m_filename);

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            ensureDir(destPath);
            continue;
        }

        // Ensure parent directory exists
        // Find last slash in destPath and temporarily null-terminate there
        char tmp[512];
        strncpy(tmp, destPath, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* slash = strrchr(tmp, '/');
        if (slash && slash != tmp) {
            *slash = '\0';
            ensureDir(tmp);
        }

        // Extract file to memory then write
        size_t dataSize = 0;
        void*  data     = mz_zip_reader_extract_to_heap(&zip, i, &dataSize, 0);
        if (!data) {
            ok = false;
            continue;
        }

        if (!writeFile(destPath, data, dataSize)) {
            ok = false;
        }
        mz_free(data);
    }

    mz_zip_reader_end(&zip);
    return ok;
}

bool ZipLoader::ensureDir(const char* path) {
    // Walk the path and mkdir each component
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
    return true;
}

bool ZipLoader::writeFile(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}
