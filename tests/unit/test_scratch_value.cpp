// =============================================================================
// test_scratch_value.cpp
// Tests the ScratchValue runtime type (numeric, string, coercions, operators)
// =============================================================================
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "core/vm.h"

// ── Construction ──────────────────────────────────────────────────────────────

TEST_CASE("ScratchValue default is numeric zero") {
    ScratchValue v;
    CHECK(v.type == ScratchValue::NUM);
    CHECK(v.toNum() == 0.0);
    CHECK(v.toBool() == false);
}

TEST_CASE("ScratchValue from double") {
    ScratchValue v(42.5);
    CHECK(v.type == ScratchValue::NUM);
    CHECK(v.toNum() == doctest::Approx(42.5));
    CHECK(v.toStr() == "42.5");
    CHECK(v.toBool() == true);
}

TEST_CASE("ScratchValue from zero is falsy") {
    ScratchValue v(0.0);
    CHECK(v.toBool() == false);
}

TEST_CASE("ScratchValue from string") {
    ScratchValue v("hello");
    CHECK(v.type == ScratchValue::STR);
    CHECK(v.toStr() == "hello");
}

// ── Coercions ─────────────────────────────────────────────────────────────────

TEST_CASE("String coercion to number") {
    CHECK(ScratchValue("3.14").toNum() == doctest::Approx(3.14));
    CHECK(ScratchValue("0").toNum()    == doctest::Approx(0.0));
    CHECK(ScratchValue("abc").toNum()  == doctest::Approx(0.0));
    CHECK(ScratchValue("-7").toNum()   == doctest::Approx(-7.0));
}

TEST_CASE("Number coercion to string - integers") {
    CHECK(ScratchValue(1.0).toStr()   == "1");
    CHECK(ScratchValue(100.0).toStr() == "100");
    CHECK(ScratchValue(-5.0).toStr()  == "-5");
}

TEST_CASE("Number coercion to string - floats") {
    ScratchValue v(3.14);
    // Should not produce trailing zeros or scientific notation for small numbers
    std::string s = v.toStr();
    CHECK(s.find("3.14") != std::string::npos);
}

// ── Comparison operators ──────────────────────────────────────────────────────

TEST_CASE("ScratchValue equality - numeric") {
    CHECK(ScratchValue(5.0) == ScratchValue(5.0));
    CHECK(!(ScratchValue(5.0) == ScratchValue(6.0)));
}

TEST_CASE("ScratchValue equality - string vs string") {
    CHECK(ScratchValue("cat") == ScratchValue("cat"));
    CHECK(!(ScratchValue("cat") == ScratchValue("dog")));
}

TEST_CASE("ScratchValue less-than") {
    CHECK(ScratchValue(3.0) < ScratchValue(4.0));
    CHECK(!(ScratchValue(4.0) < ScratchValue(3.0)));
}

TEST_CASE("ScratchValue greater-than") {
    CHECK(ScratchValue(10.0) > ScratchValue(9.0));
    CHECK(!(ScratchValue(1.0) > ScratchValue(2.0)));
}

// ── Edge cases matching Scratch semantics ─────────────────────────────────────

TEST_CASE("Empty string is falsy") {
    ScratchValue v("");
    // Scratch: empty string coerces to 0
    CHECK(v.toNum() == doctest::Approx(0.0));
}

TEST_CASE("String true/false") {
    // Non-zero numeric strings are truthy
    CHECK(ScratchValue("1").toBool() == true);
    CHECK(ScratchValue("0").toBool() == false);
}
