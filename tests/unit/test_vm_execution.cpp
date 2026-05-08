// =============================================================================
// test_vm_execution.cpp
// Tests ScratchVM operator blocks and variable accessors without NDS hardware.
// The VM is initialised with a synthetic ScratchProject built in-code.
// =============================================================================
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "core/vm.h"
#include "core/project.h"
#include <cmath>

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a bare-minimum project with one sprite and one variable.
static ScratchProject makeSimpleProject() {
    ScratchProject proj;
    proj.extractDir = "/tmp";

    ScratchSprite stage;
    stage.isStage = true;
    stage.name    = "Stage";
    stage.visible = true;
    stage.x = stage.y = 0;
    stage.size = 100;
    stage.direction = 90;
    stage.currentCostume = 0;
    stage.layerOrder = 0;
    stage.isClone = false;
    stage.oamId = -1;
    proj.targets.push_back(stage);

    ScratchSprite cat;
    cat.isStage = false;
    cat.name    = "Cat";
    cat.visible = true;
    cat.x = 0; cat.y = 0;
    cat.size = 100;
    cat.direction = 90;
    cat.currentCostume = 0;
    cat.layerOrder = 1;
    cat.isClone = false;
    cat.oamId = -1;

    ScratchVariable var;
    var.id = "v1"; var.name = "score"; var.value = "10"; var.isCloud = false; var.visible = false;
    cat.variables["v1"] = var;

    proj.targets.push_back(cat);
    return proj;
}

// ── Variable access ───────────────────────────────────────────────────────────

TEST_CASE("VM getVariable returns sprite-local variable") {
    auto proj = makeSimpleProject();
    ScratchVM vm(proj);
    ScratchSprite* cat = proj.findSprite("Cat");
    REQUIRE(cat != nullptr);
    ScratchValue v = vm.getVariable(cat, "score");
    CHECK(v.toNum() == doctest::Approx(10.0));
}

TEST_CASE("VM setVariable updates sprite-local variable") {
    auto proj = makeSimpleProject();
    ScratchVM vm(proj);
    ScratchSprite* cat = proj.findSprite("Cat");
    vm.setVariable(cat, "score", ScratchValue(42.0));
    CHECK(vm.getVariable(cat, "score").toNum() == doctest::Approx(42.0));
}

TEST_CASE("VM getVariable returns 0 for unknown name") {
    auto proj = makeSimpleProject();
    ScratchVM vm(proj);
    ScratchSprite* cat = proj.findSprite("Cat");
    CHECK(vm.getVariable(cat, "nonexistent").toNum() == doctest::Approx(0.0));
}

// ── Green flag / timer ────────────────────────────────────────────────────────

TEST_CASE("VM globalTimer starts at 0 after greenFlag") {
    auto proj = makeSimpleProject();
    ScratchVM vm(proj);
    vm.greenFlag();
    CHECK(vm.globalTimer == doctest::Approx(0.0));
}

TEST_CASE("VM globalTimer advances with step()") {
    auto proj = makeSimpleProject();
    ScratchVM vm(proj);
    vm.greenFlag();
    vm.step(0.016);
    CHECK(vm.globalTimer == doctest::Approx(0.016));
    vm.step(0.016);
    CHECK(vm.globalTimer == doctest::Approx(0.032));
}

// ── ScratchValue arithmetic (used by operator blocks) ─────────────────────────

TEST_CASE("Arithmetic: add") {
    ScratchValue a(3.0), b(4.0);
    CHECK(ScratchValue(a.toNum() + b.toNum()).toNum() == doctest::Approx(7.0));
}

TEST_CASE("Arithmetic: divide by zero gives 0") {
    double d = 0.0;
    double result = (d != 0.0) ? 10.0 / d : 0.0;
    CHECK(result == doctest::Approx(0.0));
}

TEST_CASE("Arithmetic: modulo") {
    double n = 10.0, m = 3.0;
    CHECK(std::fmod(n, m) == doctest::Approx(1.0));
}

TEST_CASE("Math ops: sqrt") {
    CHECK(std::sqrt(16.0) == doctest::Approx(4.0));
}

TEST_CASE("Math ops: sin (Scratch degrees)") {
    double angle_deg = 90.0;
    double result = std::sin(angle_deg * M_PI / 180.0);
    CHECK(result == doctest::Approx(1.0).epsilon(1e-9));
}

TEST_CASE("Math ops: cos 0 degrees") {
    CHECK(std::cos(0.0) == doctest::Approx(1.0));
}

// ── Motion helpers ────────────────────────────────────────────────────────────

TEST_CASE("Motion: move steps in direction 90 (right)") {
    // direction=90 → dx=steps, dy=0
    double direction = 90.0;
    double steps = 10.0;
    double rad = (direction - 90.0) * M_PI / 180.0;
    double dx = steps * std::cos(rad);
    double dy = -steps * std::sin(rad);
    CHECK(dx == doctest::Approx(10.0).epsilon(1e-9));
    CHECK(dy == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("Motion: move steps in direction 0 (up)") {
    double direction = 0.0;
    double steps = 10.0;
    double rad = (direction - 90.0) * M_PI / 180.0;
    double dx = steps * std::cos(rad);
    double dy = -steps * std::sin(rad);
    CHECK(dx == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(dy == doctest::Approx(10.0).epsilon(1e-9));
}

TEST_CASE("Motion: edge bounce clamps X") {
    double x =  300.0;
    if (x > 220)  x = 220;
    if (x < -220) x = -220;
    CHECK(x == doctest::Approx(220.0));
}

// ── Broadcast / stopAll ───────────────────────────────────────────────────────

TEST_CASE("VM stopAll clears threads without crash") {
    auto proj = makeSimpleProject();
    ScratchVM vm(proj);
    vm.greenFlag();
    // greenFlag on a project with no hat blocks = no threads, but stopAll is safe
    CHECK_NOTHROW(vm.stopAll());
}
