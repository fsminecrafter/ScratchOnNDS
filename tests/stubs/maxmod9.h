// =============================================================================
// tests/stubs/maxmod9.h
// Stub for maxmod (ARM9 side) — only the symbols used in audio_manager.h/cpp
// =============================================================================
#pragma once
#include <stdint.h>
#include <stdlib.h>

// ── Types ─────────────────────────────────────────────────────────────────────
typedef uint32_t mm_word;
typedef uint16_t mm_hword;
typedef uint8_t  mm_byte;
typedef void*    mm_addr;
typedef int      mm_sfxhand;

typedef enum {
    MM_SAMPLE_8BIT  = 0,
    MM_SAMPLE_16BIT = 1,
} mm_sample_format;

typedef enum {
    MM_STREAM_8BIT_MONO    = 0,
    MM_STREAM_8BIT_STEREO  = 1,
    MM_STREAM_16BIT_MONO   = 2,
    MM_STREAM_16BIT_STEREO = 3,
} mm_stream_formats;

typedef enum {
    MM_TIMER0 = 0,
    MM_TIMER1 = 1,
    MM_TIMER2 = 2,
    MM_TIMER3 = 3,
} mm_timer;

// ── System init ───────────────────────────────────────────────────────────────
typedef struct {
    uint32_t mod_count;
    uint32_t samp_count;
    void*    mem_bank;
    int      fifo_channel;
} mm_ds_system;

#define FIFO_MAXMOD 0

static inline void mmInit(mm_ds_system* sys) { (void)sys; }
static inline void mmFrame(void) {}

// ── Sample / effect playback ──────────────────────────────────────────────────
typedef struct {
    void*     data;
    uint32_t  length;
    bool      loop;
    mm_sample_format format;
    uint32_t  rate;
} mm_ds_sample;

typedef struct {
    uint32_t   id;
    uint16_t   rate;
    mm_sfxhand handle;
    uint8_t    volume;
    uint8_t    panning;
} mm_sound_effect;

static inline int        mmLoadEffect(mm_ds_sample* s) { (void)s; return 0; }
static inline mm_sfxhand mmEffectEx(mm_sound_effect* s) { (void)s; return 0; }
static inline bool       mmEffectActive(mm_sfxhand h)   { (void)h; return false; }
static inline void       mmEffectCancel(mm_sfxhand h)   { (void)h; }
static inline void       mmEffectRelease(mm_sfxhand h)  { (void)h; }

// ── Streaming ─────────────────────────────────────────────────────────────────
typedef mm_word (*mm_stream_func)(mm_word length, mm_addr dest, mm_stream_formats format);

typedef struct {
    uint32_t         sampling_rate;
    uint16_t         buffer_length;
    mm_stream_func   callback;
    mm_stream_formats format;
    mm_timer         timer;
    bool             manual;
} mm_stream;

static inline void mmStreamOpen(mm_stream* s)  { (void)s; }
static inline void mmStreamBegin(void) {}
static inline void mmStreamClose(void) {}
static inline void mmStreamUpdate(void) {}

// ── Module playback (not used by ScratchDS but referenced by some headers) ───
static inline void mmStart(uint32_t id, int mode) { (void)id; (void)mode; }
static inline void mmStop(void) {}
static inline void mmPause(void) {}
static inline void mmResume(void) {}
static inline bool mmActive(void) { return false; }

