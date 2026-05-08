// =============================================================================
// test_combo_tracker.cpp
// Tests ComboTracker button combo detection
// =============================================================================
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "scratch_extension/nds_extension.h"

TEST_CASE("ComboTracker: combo not held when no keys pressed") {
    ComboTracker ct;
    ct.registerCombo("L+R", KEY_L | KEY_R);
    ct.update(0);
    CHECK(!ct.isHeld("L+R"));
    CHECK(!ct.justTriggered("L+R"));
}

TEST_CASE("ComboTracker: combo held when both keys down") {
    ComboTracker ct;
    ct.registerCombo("L+R", KEY_L | KEY_R);
    ct.update(KEY_L | KEY_R);
    CHECK(ct.isHeld("L+R"));
}

TEST_CASE("ComboTracker: justTriggered only on first frame") {
    ComboTracker ct;
    ct.registerCombo("A+B", KEY_A | KEY_B);
    ct.update(0);
    ct.update(KEY_A | KEY_B); // press
    CHECK(ct.justTriggered("A+B"));
    ct.update(KEY_A | KEY_B); // still held
    CHECK(!ct.justTriggered("A+B")); // no longer just-pressed
}

TEST_CASE("ComboTracker: partial combo not triggered") {
    ComboTracker ct;
    ct.registerCombo("L+R", KEY_L | KEY_R);
    ct.update(KEY_L); // only L
    CHECK(!ct.isHeld("L+R"));
}

TEST_CASE("ComboTracker: multiple combos independent") {
    ComboTracker ct;
    ct.registerCombo("L+R", KEY_L | KEY_R);
    ct.registerCombo("A+B", KEY_A | KEY_B);
    ct.update(KEY_L | KEY_R);
    CHECK(ct.isHeld("L+R"));
    CHECK(!ct.isHeld("A+B"));
}

TEST_CASE("ComboTracker: unknown combo returns false") {
    ComboTracker ct;
    ct.registerCombo("L+R", KEY_L | KEY_R);
    ct.update(KEY_L | KEY_R);
    CHECK(!ct.isHeld("X+Y")); // never registered
}

TEST_CASE("ComboTracker: L+R+B three-button combo") {
    ComboTracker ct;
    ct.registerCombo("L+R+B", KEY_L | KEY_R | KEY_B);
    ct.update(0);
    ct.update(KEY_L | KEY_R);         // partial
    CHECK(!ct.isHeld("L+R+B"));
    ct.update(KEY_L | KEY_R | KEY_B); // all three
    CHECK(ct.isHeld("L+R+B"));
}
