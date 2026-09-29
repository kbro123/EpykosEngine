// M3/G0: the strict in-house JSON reader (D36) — valid and invalid inputs, numbers to full
// precision, line-numbered errors, round trips.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "epykos/util/json.hpp"

using epykos::json::JsonError;
using epykos::json::Value;
using epykos::json::dump;
using epykos::json::parse;

TEST(Json, ParsesEveryKind) {
  const Value v = parse(R"({"n": null, "t": true, "f": false, "x": -12.5e3, "s": "a\"b\\c\n\u00e9\ud83d\ude00", "a": [1, [2, 3], {}], "o": {"k": "v"}})");
  ASSERT_TRUE(v.is_object());
  EXPECT_EQ(v.size(), 7u);
  EXPECT_TRUE(v.at("n").is_null());
  EXPECT_TRUE(v.at("t").as_bool());
  EXPECT_FALSE(v.at("f").as_bool());
  EXPECT_EQ(v.at("x").as_double(), -12500.0);
  EXPECT_EQ(v.at("x").lexeme(), "-12.5e3");
  EXPECT_EQ(v.at("s").as_string(), std::string("a\"b\\c\n\xC3\xA9\xF0\x9F\x98\x80"));
  EXPECT_EQ(v.at("a").size(), 3u);
  EXPECT_EQ(v.at("a").at(1).at(1).as_int(), 3);
  EXPECT_TRUE(v.at("a").at(2).is_object());
  EXPECT_EQ(v.at("a").at(2).size(), 0u);
  EXPECT_EQ(v.at("o").at("k").as_string(), "v");
  EXPECT_EQ(v.find("missing"), nullptr);
  EXPECT_THROW(v.at("missing"), JsonError);
}

TEST(Json, KeepsInsertionOrder) {
  const Value v = parse(R"({"z": 1, "a": 2, "m": 3})");
  const auto& m = v.as_object();
  ASSERT_EQ(m.size(), 3u);
  EXPECT_EQ(m[0].first, "z");
  EXPECT_EQ(m[1].first, "a");
  EXPECT_EQ(m[2].first, "m");
}

TEST(Json, NumbersToFullPrecision) {
  EXPECT_EQ(parse("0.1").as_double(), 0.1);
  EXPECT_EQ(parse("0.30000000000000004").as_double(), 0.1 + 0.2);
  EXPECT_EQ(parse("1e-9").as_double(), 1e-9);
  EXPECT_EQ(parse("1.7976931348623157e308").as_double(), 1.7976931348623157e308);
  EXPECT_EQ(parse("4.9e-324").as_double(), 4.9e-324);
  EXPECT_EQ(parse("2.2250738585072014e-308").as_double(), 2.2250738585072014e-308);
  EXPECT_EQ(parse("0.041234567890123456").as_double(), 0.041234567890123456);
  const Value neg0 = parse("-0.0");
  EXPECT_TRUE(std::signbit(neg0.as_double()));
  // exact integers
  EXPECT_TRUE(parse("123456789012345678").is_integer());
  EXPECT_EQ(parse("123456789012345678").as_int(), 123456789012345678LL);
  EXPECT_EQ(parse("-9007199254740993").as_int(), -9007199254740993LL);
  EXPECT_FALSE(parse("1.0").is_integer());
  EXPECT_FALSE(parse("1e2").is_integer());
  EXPECT_THROW(parse("1.5").as_int(), JsonError);
  EXPECT_THROW(parse("1e400"), JsonError);   // out of range
  // the lexeme is what was written
  EXPECT_EQ(parse("100.250").lexeme(), "100.250");
  EXPECT_EQ(dump(parse("100.250")), "100.250");
}

TEST(Json, RoundTripsThroughDump) {
  const std::string text =
      R"({"a":[1,2.5,-3e-7,"x\ty",null,true,false],"b":{"c":{"d":[]},"e":{}},"f":"\u001f","g":"caf\u00e9"})";
  const Value v = parse(text);
  const std::string once = dump(v);
  const Value again = parse(once);
  EXPECT_EQ(v, again);
  EXPECT_EQ(dump(again), once);
  // The only differences from the source are the escape spelling of control characters and
  // that non-ASCII is emitted raw (still valid UTF-8 JSON).
  EXPECT_EQ(once, "{\"a\":[1,2.5,-3e-7,\"x\\ty\",null,true,false],\"b\":{\"c\":{\"d\":[]},\"e\":{}},\"f\":\"\\u001f\",\"g\":\"caf\xC3\xA9\"}");
  // pretty print round-trips too
  const Value pretty = parse(dump(v, 2));
  EXPECT_EQ(pretty, v);
}

TEST(Json, BuiltValuesRoundTrip) {
  Value o = Value::object();
  o.set("pi", Value::number(3.141592653589793));
  o.set("n", Value::number(static_cast<std::int64_t>(-42)));
  o.set("tenth", Value::number(0.1));
  o.set("list", Value::array({Value::string("a"), Value::boolean(true), Value::null()}));
  const std::string text = dump(o);
  const Value back = parse(text);
  EXPECT_EQ(back.at("pi").as_double(), 3.141592653589793);
  EXPECT_EQ(back.at("n").as_int(), -42);
  EXPECT_EQ(back.at("tenth").as_double(), 0.1);
  EXPECT_EQ(back.at("tenth").lexeme(), "0.1");
  EXPECT_EQ(back.at("list").at(2).kind(), Value::Kind::Null);
  EXPECT_THROW(o.set("pi", Value::null()), JsonError);   // duplicate key
  EXPECT_THROW(Value::number(std::nan("")), JsonError);
}

TEST(Json, RejectsInvalidDocuments) {
  const char* bad[] = {
      "",                       // empty
      "   ",                    // whitespace only
      "{",                      // unterminated object
      "[1, 2,]",                // trailing comma
      "{\"a\": 1,}",            // trailing comma
      "{'a': 1}",               // single quotes
      "{\"a\" 1}",              // missing colon
      "{\"a\": 1 \"b\": 2}",    // missing comma
      "[1 2]",                  // missing comma
      "01",                     // leading zero
      "+1",                     // leading plus
      ".5",                     // no leading digit
      "1.",                     // no digit after point
      "1e",                     // no exponent digits
      "NaN",                    // not JSON
      "Infinity",               // not JSON
      "tru",                    // truncated literal
      "nul",                    // truncated literal
      "\"abc",                  // unterminated string
      "\"a\\x\"",               // bad escape
      "\"a\tb\"",               // raw control character
      "\"\\ud83d\"",            // lone high surrogate
      "\"\\ude00\"",            // lone low surrogate
      "\"\\u12G4\"",            // bad hex
      "{\"a\": 1, \"a\": 2}",   // duplicate key
      "{\"a\": 1} x",           // trailing garbage
      "[1] [2]",                // two documents
      "// c\n1",                // comments
      "{\"a\": undefined}",     // not JSON
  };
  for (const char* text : bad) {
    EXPECT_THROW(parse(text), JsonError) << "accepted: " << text;
  }
}

TEST(Json, ErrorsCarryLineAndColumn) {
  try {
    parse("{\n  \"a\": 1,\n  \"b\": tru\n}", "conv.json");
    FAIL() << "no error";
  } catch (const JsonError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("conv.json:3:"), std::string::npos) << what;
  }
  try {
    parse("{\n  \"a\": 1,\n  \"a\": 2\n}", "dup.json");
    FAIL() << "no error";
  } catch (const JsonError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("dup.json:3:3"), std::string::npos) << what;
    EXPECT_NE(what.find("duplicate key"), std::string::npos) << what;
  }
  // parsed values remember where they start
  const Value v = parse("{\n  \"k\": [\n    1,\n    2\n  ]\n}", "pos.json");
  EXPECT_EQ(v.at("k").line(), 2);
  EXPECT_EQ(v.at("k").at(1).line(), 4);
  EXPECT_EQ(v.at("k").at(1).where(), "pos.json:4:5");
  // and typed access names the place and the kind found
  try {
    (void)v.at("k").as_string();
    FAIL() << "no error";
  } catch (const JsonError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("pos.json:2:8"), std::string::npos) << what;
    EXPECT_NE(what.find("expected string"), std::string::npos) << what;
  }
}

TEST(Json, DeepNestingIsBounded) {
  std::string deep(600, '[');
  deep += std::string(600, ']');
  EXPECT_THROW(parse(deep), JsonError);
  std::string ok(100, '[');
  ok += std::string(100, ']');
  EXPECT_NO_THROW(parse(ok));
}
