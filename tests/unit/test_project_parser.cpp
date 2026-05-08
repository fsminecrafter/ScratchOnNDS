// =============================================================================
// test_project_parser.cpp
// Tests ScratchProject::parseJson with minimal synthetic JSON payloads.
// =============================================================================
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "core/project.h"
#include <cstring>
#include <cstdio>

// ── Helpers ───────────────────────────────────────────────────────────────────

// Write a temp file and load it via ScratchProject::load()
static bool loadFromString(ScratchProject& proj, const char* json) {
    // Write to a temp directory structure
    const char* dir = "/tmp/scratch_test_proj";
    mkdir(dir, 0777);
    char path[256];
    snprintf(path, sizeof(path), "%s/project.json", dir);
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fputs(json, f);
    fclose(f);
    return proj.load(dir);
}

// ── Minimal valid project ─────────────────────────────────────────────────────

static const char* MINIMAL_PROJECT = R"({
  "meta": { "semver": "3.0.0", "vm": "0.2.0", "agent": "test" },
  "targets": [
    {
      "isStage": true,
      "name": "Stage",
      "variables": {},
      "lists": {},
      "broadcasts": {},
      "blocks": {},
      "costumes": [
        {
          "name": "backdrop1",
          "assetId": "abc123",
          "dataFormat": "png",
          "bitmapResolution": 1,
          "rotationCenterX": 240,
          "rotationCenterY": 180
        }
      ],
      "sounds": [],
      "currentCostume": 0,
      "layerOrder": 0,
      "volume": 100,
      "visible": true,
      "x": 0, "y": 0, "size": 100, "direction": 90
    },
    {
      "isStage": false,
      "name": "Cat",
      "variables": {
        "id_score": ["score", "0"]
      },
      "lists": {},
      "broadcasts": {},
      "blocks": {},
      "costumes": [
        {
          "name": "costume1",
          "assetId": "def456",
          "dataFormat": "png",
          "bitmapResolution": 1,
          "rotationCenterX": 48,
          "rotationCenterY": 55
        }
      ],
      "sounds": [],
      "currentCostume": 0,
      "layerOrder": 1,
      "volume": 100,
      "visible": true,
      "x": 10, "y": -20, "size": 100, "direction": 90,
      "rotationStyle": "all around"
    }
  ]
})";

TEST_CASE("Parse minimal project - metadata") {
    ScratchProject proj;
    REQUIRE(loadFromString(proj, MINIMAL_PROJECT));
    CHECK(proj.meta.semver == "3.0.0");
    CHECK(proj.meta.vm     == "0.2.0");
}

TEST_CASE("Parse minimal project - target count") {
    ScratchProject proj;
    REQUIRE(loadFromString(proj, MINIMAL_PROJECT));
    CHECK(proj.targets.size() == 2);
}

TEST_CASE("Parse minimal project - stage") {
    ScratchProject proj;
    REQUIRE(loadFromString(proj, MINIMAL_PROJECT));
    ScratchSprite* stage = proj.getStage();
    REQUIRE(stage != nullptr);
    CHECK(stage->isStage == true);
    CHECK(stage->name    == "Stage");
}

TEST_CASE("Parse minimal project - sprite properties") {
    ScratchProject proj;
    REQUIRE(loadFromString(proj, MINIMAL_PROJECT));
    REQUIRE(proj.targets.size() >= 2);
    ScratchSprite& cat = proj.targets[1];
    CHECK(cat.name    == "Cat");
    CHECK(cat.x       == doctest::Approx(10.0));
    CHECK(cat.y       == doctest::Approx(-20.0));
    CHECK(cat.visible == true);
    CHECK(cat.isStage == false);
}

TEST_CASE("Parse minimal project - costume") {
    ScratchProject proj;
    REQUIRE(loadFromString(proj, MINIMAL_PROJECT));
    ScratchSprite& cat = proj.targets[1];
    REQUIRE(cat.costumes.size() == 1);
    CHECK(cat.costumes[0].name       == "costume1");
    CHECK(cat.costumes[0].assetId    == "def456");
    CHECK(cat.costumes[0].dataFormat == "png");
}

TEST_CASE("Parse minimal project - variable") {
    ScratchProject proj;
    REQUIRE(loadFromString(proj, MINIMAL_PROJECT));
    ScratchSprite& cat = proj.targets[1];
    bool found = false;
    for (auto& kv : cat.variables) {
        if (kv.second.name == "score") {
            found = true;
            CHECK(kv.second.value == "0");
        }
    }
    CHECK(found);
}

TEST_CASE("findSprite by name") {
    ScratchProject proj;
    REQUIRE(loadFromString(proj, MINIMAL_PROJECT));
    ScratchSprite* cat = proj.findSprite("Cat");
    REQUIRE(cat != nullptr);
    CHECK(cat->name == "Cat");
}

TEST_CASE("findSprite returns nullptr for missing name") {
    ScratchProject proj;
    REQUIRE(loadFromString(proj, MINIMAL_PROJECT));
    CHECK(proj.findSprite("DoesNotExist") == nullptr);
}

TEST_CASE("Empty targets JSON fails gracefully") {
    ScratchProject proj;
    const char* json = R"({"meta":{},"targets":[]})";
    // load() returns false when targets is empty
    bool ok = loadFromString(proj, json);
    CHECK(!ok);  // empty targets should fail
}

TEST_CASE("Malformed JSON returns false") {
    ScratchProject proj;
    const char* json = "{ not valid json !!!";
    bool ok = loadFromString(proj, json);
    CHECK(!ok);
}
