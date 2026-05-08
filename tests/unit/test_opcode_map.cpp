// =============================================================================
// test_opcode_map.cpp
// Verifies that all opcode strings used in project.json map to the correct
// BlockOpcode enum values (caught a real bug during development: a typo in
// opcodeFromStr caused NDS blocks to silently become UNKNOWN).
// =============================================================================
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "core/project.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

// We call opcodeFromStr indirectly by loading a tiny JSON and checking the
// parsed block opcode. We use the internal helper via a friend trick — or
// more simply, verify by round-tripping through load().

// Build a project JSON with a single block of the given opcode string,
// then confirm its parsed opcode value.
static BlockOpcode parseOpcode(const char* opcodeStr) {
    char json[2048];
    snprintf(json, sizeof(json), R"({
      "meta": { "semver": "3.0.0", "vm": "0", "agent": "" },
      "targets": [{
        "isStage": true, "name": "Stage",
        "variables": {}, "lists": {}, "broadcasts": {},
        "costumes": [{
          "name":"b","assetId":"x","dataFormat":"png",
          "bitmapResolution":1,"rotationCenterX":0,"rotationCenterY":0
        }],
        "sounds": [], "currentCostume": 0, "layerOrder": 0,
        "visible": true, "x": 0, "y": 0, "size": 100, "direction": 90,
        "blocks": {
          "block_a": {
            "opcode": "%s",
            "next": null, "parent": null,
            "inputs": {}, "fields": {},
            "topLevel": true, "shadow": false
          }
        }
      }]
    })", opcodeStr);

    const char* dir = "/tmp/opcode_test";
    mkdir(dir, 0777);
    char path[256];
    snprintf(path, sizeof(path), "%s/project.json", dir);
    FILE* f = fopen(path, "w");
    fputs(json, f);
    fclose(f);

    ScratchProject proj;
    if (!proj.load(dir)) return BlockOpcode::UNKNOWN;

    for (auto& kv : proj.targets[0].blocks) {
        return kv.second.opcode;
    }
    return BlockOpcode::UNKNOWN;
}

// ── Event hats ────────────────────────────────────────────────────────────────
TEST_CASE("opcode: event_whenflagclicked") {
    CHECK(parseOpcode("event_whenflagclicked") == BlockOpcode::EVENT_WHENFLAGCLICKED);
}
TEST_CASE("opcode: event_whenkeypressed") {
    CHECK(parseOpcode("event_whenkeypressed") == BlockOpcode::EVENT_WHENKEYPRESSED);
}
TEST_CASE("opcode: event_whenbroadcastreceived") {
    CHECK(parseOpcode("event_whenbroadcastreceived") == BlockOpcode::EVENT_WHENBROADCASTRECEIVED);
}

// ── Motion ────────────────────────────────────────────────────────────────────
TEST_CASE("opcode: motion_movesteps") {
    CHECK(parseOpcode("motion_movesteps") == BlockOpcode::MOTION_MOVESTEPS);
}
TEST_CASE("opcode: motion_gotoxy") {
    CHECK(parseOpcode("motion_gotoxy") == BlockOpcode::MOTION_GOTOXY);
}
TEST_CASE("opcode: motion_ifonedgebounce") {
    CHECK(parseOpcode("motion_ifonedgebounce") == BlockOpcode::MOTION_IFONEDGEBOUNCE);
}

// ── Looks ─────────────────────────────────────────────────────────────────────
TEST_CASE("opcode: looks_say") {
    CHECK(parseOpcode("looks_say") == BlockOpcode::LOOKS_SAY);
}
TEST_CASE("opcode: looks_nextcostume") {
    CHECK(parseOpcode("looks_nextcostume") == BlockOpcode::LOOKS_NEXTCOSTUME);
}

// ── Control ───────────────────────────────────────────────────────────────────
TEST_CASE("opcode: control_wait") {
    CHECK(parseOpcode("control_wait") == BlockOpcode::CONTROL_WAIT);
}
TEST_CASE("opcode: control_forever") {
    CHECK(parseOpcode("control_forever") == BlockOpcode::CONTROL_FOREVER);
}
TEST_CASE("opcode: control_if") {
    CHECK(parseOpcode("control_if") == BlockOpcode::CONTROL_IF);
}
TEST_CASE("opcode: control_repeat") {
    CHECK(parseOpcode("control_repeat") == BlockOpcode::CONTROL_REPEAT);
}

// ── Operators ─────────────────────────────────────────────────────────────────
TEST_CASE("opcode: operator_add") {
    CHECK(parseOpcode("operator_add") == BlockOpcode::OPERATOR_ADD);
}
TEST_CASE("opcode: operator_random") {
    CHECK(parseOpcode("operator_random") == BlockOpcode::OPERATOR_RANDOM);
}
TEST_CASE("opcode: operator_mathop") {
    CHECK(parseOpcode("operator_mathop") == BlockOpcode::OPERATOR_MATHOP);
}

// ── Sensing ───────────────────────────────────────────────────────────────────
TEST_CASE("opcode: sensing_loudness") {
    CHECK(parseOpcode("sensing_loudness") == BlockOpcode::SENSING_LOUDNESS);
}
TEST_CASE("opcode: sensing_timer") {
    CHECK(parseOpcode("sensing_timer") == BlockOpcode::SENSING_TIMER);
}

// ── NDS extension ─────────────────────────────────────────────────────────────
TEST_CASE("opcode: nds_buttonpressed") {
    CHECK(parseOpcode("nds_buttonpressed") == BlockOpcode::NDS_BUTTONPRESSED);
}
TEST_CASE("opcode: nds_buttonheld") {
    CHECK(parseOpcode("nds_buttonheld") == BlockOpcode::NDS_BUTTONHELD);
}
TEST_CASE("opcode: nds_touchx") {
    CHECK(parseOpcode("nds_touchx") == BlockOpcode::NDS_TOUCHX);
}
TEST_CASE("opcode: nds_rumble") {
    CHECK(parseOpcode("nds_rumble") == BlockOpcode::NDS_RUMBLE);
}

// ── Unknown opcode should return UNKNOWN ──────────────────────────────────────
TEST_CASE("opcode: unknown string returns UNKNOWN") {
    CHECK(parseOpcode("totally_fake_opcode") == BlockOpcode::UNKNOWN);
}
