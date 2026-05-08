// =============================================================================
// tests/stubs/audio_manager_stub.cpp
// Host-build stub for AudioManager.
// vm.cpp calls AudioManager::getInstance() and a handful of methods.
// These no-op implementations satisfy the linker without pulling in
// maxmod, libfat, or any NDS hardware code.
// =============================================================================
#include "audio/audio_manager.h"

void AudioManager::init() {}

void AudioManager::update() {}

void AudioManager::loadSounds(ScratchProject&, const char*) {}

void AudioManager::playSound(ScratchSprite*, const std::string&) {}

void AudioManager::stopAll() {}

bool AudioManager::isPlaying() { return false; }

void AudioManager::setVolume(int) {}

bool AudioManager::loadWavToRam(ScratchSound&, const char*) { return false; }
bool AudioManager::loadMp3ToRam(ScratchSound&, const char*) { return false; }
bool AudioManager::setupStream(ScratchSound&, const char*)  { return false; }
void AudioManager::playRamSound(ScratchSound&) {}
void AudioManager::startStream(ScratchSound&) {}
