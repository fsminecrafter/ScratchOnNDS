// =============================================================================
// test_clap_detector.cpp
// Tests the ClapDetector state machine from nds_extension.h
// (pure logic, no hardware)
// =============================================================================
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// Pull in just the struct — it has no NDS dependencies
#include "scratch_extension/nds_extension.h"

TEST_CASE("ClapDetector: no clap when quiet") {
    ClapDetector clap;
    clap.update(5, 0.016f);
    CHECK(!clap.detected());
}

TEST_CASE("ClapDetector: clap detected on loud spike from quiet") {
    ClapDetector clap;
    // Several quiet frames
    for (int i = 0; i < 5; i++) clap.update(10, 0.016f);
    CHECK(!clap.detected());
    // One loud frame
    clap.update(80, 0.016f);
    CHECK(clap.detected());
}

TEST_CASE("ClapDetector: no second clap during cooldown") {
    ClapDetector clap;
    for (int i = 0; i < 5; i++) clap.update(5, 0.016f);
    clap.update(80, 0.016f);
    REQUIRE(clap.detected());
    // Immediately try again — cooldown should block it
    clap.update(80, 0.016f);
    CHECK(!clap.detected());
}

TEST_CASE("ClapDetector: second clap after cooldown expires") {
    ClapDetector clap;
    // First clap
    for (int i = 0; i < 5; i++) clap.update(5, 0.016f);
    clap.update(80, 0.016f);
    REQUIRE(clap.detected());

    // Wait out cooldown (0.3s) in quiet frames
    float elapsed = 0.0f;
    while (elapsed < 0.35f) {
        clap.update(5, 0.05f);
        elapsed += 0.05f;
    }
    CHECK(!clap.detected()); // still quiet here

    // Second clap
    clap.update(80, 0.016f);
    CHECK(clap.detected());
}

TEST_CASE("ClapDetector: no clap if never quiet first") {
    ClapDetector clap;
    // Sustained loud — should only fire once then cooldown blocks it
    clap.update(80, 0.016f); // first frame, wasQuiet=true initially → fires
    CHECK(clap.detected());
    clap.update(80, 0.016f); // cooldown
    CHECK(!clap.detected());
}

TEST_CASE("ClapDetector: below threshold does not trigger") {
    ClapDetector clap;
    for (int i = 0; i < 10; i++) clap.update(5, 0.016f);
    // Just below threshold (60)
    clap.update(59, 0.016f);
    CHECK(!clap.detected());
}

TEST_CASE("ClapDetector: exactly at threshold triggers") {
    ClapDetector clap;
    for (int i = 0; i < 5; i++) clap.update(5, 0.016f);
    clap.update(ClapDetector::CLAP_THRESHOLD, 0.016f);
    CHECK(clap.detected());
}
