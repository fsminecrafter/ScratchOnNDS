#include "zip_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool ZipLoader::extract(const char* zipPath, const char* destDir) {
    // TODO: implement using miniz or similar
    // For now return false so loadProject fails gracefully
    return false;
}

bool ZipLoader::ensureDir(const char* path) {
    mkdir(path, 0777);
    return true;
}

bool ZipLoader::writeFile(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(data, 1, size, f);
    fclose(f);
    return true;
}
