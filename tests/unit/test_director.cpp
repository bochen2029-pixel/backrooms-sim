// test_director.cpp — M11 Director schema validator + JSON reader.
//
// The validator is the safety net that makes "100% schema-valid directives" hold
// even though KEEL's HTTP egress does not (yet) expose grammar-constrained decode:
// untrusted model output is parsed + range/enum-checked, and anything off-schema is
// rejected whole (never partially applied). Pure + total -> unit-testable offline.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "director/director.h"
#include "director/json.h"

using br::director::validate_directive;
using br::contracts::DirectiveKind;

TEST_CASE("valid flicker directive parses and clamps intensity") {
    auto r = validate_directive(R"({"type":"flicker","sector":12,"intensity":1.7})");
    REQUIRE(r.ok);
    REQUIRE(r.directive.kind == DirectiveKind::FlickerSector);
    REQUIRE(r.directive.sector == 12);
    REQUIRE(r.directive.intensity == 1.0f);  // clamped to [0,1]
}

TEST_CASE("valid intercom directive sanitises its caption") {
    auto r = validate_directive("{\"type\":\"intercom\",\"detail\":\"please remain\\tcalm\\n\"}");
    REQUIRE(r.ok);
    REQUIRE(r.directive.kind == DirectiveKind::Intercom);
    REQUIRE(std::string(r.directive.caption) == "please remaincalm");  // tab/newline dropped
}

TEST_CASE("valid biome_bias and note directives parse") {
    auto a = validate_directive(R"({"type":"biome_bias","biome":3,"detail":"the air thickens"})");
    REQUIRE(a.ok);
    REQUIRE(a.directive.kind == DirectiveKind::BiomeBias);
    REQUIRE(a.directive.biome == 3);

    auto b = validate_directive(R"({"type":"note","detail":"I have been here before"})");
    REQUIRE(b.ok);
    REQUIRE(b.directive.kind == DirectiveKind::WandererNote);
}

TEST_CASE("malformed JSON is rejected, not crashed") {
    REQUIRE_FALSE(validate_directive("{not json").ok);
    REQUIRE_FALSE(validate_directive("").ok);
    REQUIRE_FALSE(validate_directive(R"({"type":"flicker","sector":1} trailing)").ok);  // trailing garbage
}

TEST_CASE("non-object and missing type are rejected") {
    REQUIRE_FALSE(validate_directive(R"(["flicker"])").ok);
    REQUIRE_FALSE(validate_directive(R"(42)").ok);
    REQUIRE_FALSE(validate_directive(R"({"sector":1})").ok);          // no type
    REQUIRE_FALSE(validate_directive(R"({"type":7})").ok);            // type not a string
}

TEST_CASE("unknown directive type is rejected") {
    auto r = validate_directive(R"({"type":"selfdestruct","detail":"boom"})");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.reject_reason.find("unknown") != std::string::npos);
}

TEST_CASE("out-of-range and missing structured fields are rejected") {
    REQUIRE_FALSE(validate_directive(R"({"type":"flicker","sector":99})").ok);    // sector >= 64
    REQUIRE_FALSE(validate_directive(R"({"type":"flicker","sector":-1})").ok);
    REQUIRE_FALSE(validate_directive(R"({"type":"flicker"})").ok);                 // no sector
    REQUIRE_FALSE(validate_directive(R"({"type":"biome_bias","biome":5})").ok);    // biome >= 5
    REQUIRE_FALSE(validate_directive(R"({"type":"biome_bias","detail":"x"})").ok); // no numeric biome
}

TEST_CASE("intercom and note require a non-empty caption") {
    REQUIRE_FALSE(validate_directive(R"({"type":"intercom"})").ok);                 // missing detail
    REQUIRE_FALSE(validate_directive("{\"type\":\"note\",\"detail\":\"\\n\\t\"}").ok);  // empty after sanitise
}

TEST_CASE("over-long captions are truncated to the cap, not rejected") {
    std::string big(500, 'A');
    auto r = validate_directive(std::string(R"({"type":"intercom","detail":")") + big + R"("})");
    REQUIRE(r.ok);
    REQUIRE(std::string(r.directive.caption).size() == static_cast<size_t>(br::contracts::kDirectiveCaptionCap - 1));
}

TEST_CASE("json reader handles nesting, escapes, and rejects trailing junk") {
    using namespace br::director::json;
    Value v; std::string err;
    REQUIRE(parse(R"({"a":[1,2,{"b":"x\ny"}],"c":true,"d":null})", v, err));
    REQUIRE(v.is_object());
    REQUIRE(v.find("a")->at(2)->find("b")->str == "x\ny");
    REQUIRE(v.find("c")->b == true);
    REQUIRE_FALSE(parse(R"({"a":1}{)", v, err));   // trailing junk
    REQUIRE_FALSE(parse(R"({"a":)", v, err));       // truncated
}
