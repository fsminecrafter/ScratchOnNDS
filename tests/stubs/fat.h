// =============================================================================
// tests/stubs/fat.h
// Stub for libfat — only the symbols actually used in source files
// =============================================================================
#pragma once
#include <sys/stat.h>
#include <sys/types.h>

static inline bool fatInitDefault(void) { return true; }
static inline bool fatInit(uint32_t cacheSize, bool setAsDefaultDevice) {
    (void)cacheSize; (void)setAsDefaultDevice; return true;
}
