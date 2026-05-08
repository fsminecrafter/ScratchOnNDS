// =============================================================================
// nds_vm_dispatch.h — Inline dispatch for NDS_* opcodes
// #include this at the end of the switch in ScratchVM::executeBlock()
// BEFORE the default: case.
//
// All NDS_* opcodes route here, delegating to NDSExtension so the VM
// doesn't need to know about hardware details.
// =============================================================================
#pragma once
// (Included inside vm.cpp's switch statement — not a standalone TU)

// The NDSExtension singleton must be accessible:
// #include "../scratch_extension/nds_extension.h"   <- add to vm.cpp includes

// ── Trigger reporters (return bool as 1.0/0.0) ───────────────────────────────

case BlockOpcode::NDS_BUTTONPRESSED: {
    auto it = b->fields.find("BUTTON");
    std::string btn = (it != b->fields.end()) ? it->second : "A";
    out = ScratchValue(NDSExtension::getInstance().isButtonPressed(btn) ? 1.0 : 0.0);
    break;
}
case BlockOpcode::NDS_BUTTONHELD: {
    auto it = b->fields.find("BUTTON");
    std::string btn = (it != b->fields.end()) ? it->second : "A";
    out = ScratchValue(NDSExtension::getInstance().isButtonHeld(btn) ? 1.0 : 0.0);
    break;
}
case BlockOpcode::NDS_BUTTONRELEASED: {
    auto it = b->fields.find("BUTTON");
    std::string btn = (it != b->fields.end()) ? it->second : "A";
    out = ScratchValue(NDSExtension::getInstance().isButtonReleased(btn) ? 1.0 : 0.0);
    break;
}

// Combo
// NDS_COMBO_HELD — field "COMBO" = "L+R" etc.
case BlockOpcode::NDS_BUTTONPRESSED + 100: { // placeholder — add NDS_COMBO_HELD to enum
    auto it = b->fields.find("COMBO");
    std::string combo = (it != b->fields.end()) ? it->second : "L+R";
    out = ScratchValue(NDSExtension::getInstance().isComboHeld(combo) ? 1.0 : 0.0);
    break;
}

// ── Touchscreen reporters ─────────────────────────────────────────────────────

case BlockOpcode::NDS_TOUCHX:
    out = ScratchValue((double)NDSExtension::getInstance().getTouchX());
    break;
case BlockOpcode::NDS_TOUCHY:
    out = ScratchValue((double)NDSExtension::getInstance().getTouchY());
    break;
case BlockOpcode::NDS_TOUCHPRESSED:
    out = ScratchValue(NDSExtension::getInstance().isTouching() ? 1.0 : 0.0);
    break;

// Touch delta (drag velocity per frame)
// NDS_TOUCH_DELTAX / NDS_TOUCH_DELTAY — add to BlockOpcode enum as needed
// Handled here via the UNKNOWN path until enum is extended:
// case BlockOpcode::NDS_TOUCH_DELTAX:
//     out = ScratchValue((double)NDSExtension::getInstance().getTouchDeltaX());
//     break;
// case BlockOpcode::NDS_TOUCH_DELTAY:
//     out = ScratchValue((double)NDSExtension::getInstance().getTouchDeltaY());
//     break;

// ── Microphone reporters ──────────────────────────────────────────────────────

case BlockOpcode::SENSING_LOUDNESS:
case BlockOpcode::NDS_MICROPHONE_LOUDNESS:
    out = ScratchValue((double)NDSExtension::getInstance().getMicLoudness());
    break;

// Clap detection (NDS_CLAP_DETECTED — add to enum)
// Broadcasts are fired from main.cpp; here it's a live reporter:
// case BlockOpcode::NDS_CLAP_DETECTED:
//     out = ScratchValue(NDSExtension::getInstance().isClapDetected() ? 1.0 : 0.0);
//     break;

// NDS_MIC_RECORDING
// case BlockOpcode::NDS_MIC_RECORDING:
//     out = ScratchValue(NDSExtension::getInstance().isMicRecording() ? 1.0 : 0.0);
//     break;

// ── System commands ───────────────────────────────────────────────────────────

case BlockOpcode::NDS_RUMBLE: {
    // Pulse rumble for 0.25s (default)
    NDSExtension::getInstance().pulseRumble(0.25f);
    break;
}
case BlockOpcode::NDS_SETVIBRATION: {
    // Field "STATE" = "on" or "off"
    auto it = b->fields.find("STATE");
    bool on = (it == b->fields.end() || it->second != "off");
    NDSExtension::getInstance().setRumble(on);
    break;
}
case BlockOpcode::NDS_BACKLIGHT_TOP: {
    auto it = b->fields.find("STATE");
    bool on = (it == b->fields.end() || it->second != "off");
    NDSExtension::getInstance().setTopBacklight(on);
    break;
}
case BlockOpcode::NDS_BACKLIGHT_BOTTOM: {
    auto it = b->fields.find("STATE");
    bool on = (it == b->fields.end() || it->second != "off");
    NDSExtension::getInstance().setBottomBacklight(on);
    break;
}
