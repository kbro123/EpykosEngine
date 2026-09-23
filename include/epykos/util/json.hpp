// EpykosEngine — a small strict JSON reader and writer (D36: definitions are data, read by an
// in-house reader; no third-party dependency, D12).
//
//   * RFC 8259 strictly: objects, arrays, strings (UTF-8, all escapes incl. \uXXXX surrogate
//     pairs), numbers, true / false / null. No comments, no trailing commas, no NaN / Infinity,
//     no leading '+', no leading zeros, no control characters inside strings, no duplicate keys.
//   * Every error is a JsonError carrying "origin:line:column: what".
//   * Numbers keep their source lexeme, so a value round-trips to exactly the digits it was
//     written with; the double is strtod of that lexeme (correctly rounded on every libc we
//     build with). Integers are exact up to 2^53 through as_int().
//   * Objects keep insertion order (a vector of pairs), so a dump() of a parsed document lists
//     keys in the order of the file.
//   * Every node remembers the line it started on, so a schema error in the conventions
//     registry can say where the offending value is.
//
// This is structure only: nothing here touches a Scalar.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epykos::json {

class JsonError : public std::runtime_error {
 public:
  explicit JsonError(const std::string& what) : std::runtime_error(what) {}
};

class Value;
using Member = std::pair<std::string, Value>;

class Value {
 public:
  enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

  Value() = default;                        // null
  static Value null();
  static Value boolean(bool b);
  static Value number(double v);            // lexeme = shortest round-trip repr
  static Value number(std::int64_t v);      // lexeme = decimal digits
  static Value string(std::string s);
  static Value array(std::vector<Value> items = {});
  static Value object(std::vector<Member> members = {});

  Kind kind() const noexcept { return kind_; }
  bool is_null() const noexcept { return kind_ == Kind::Null; }
  bool is_bool() const noexcept { return kind_ == Kind::Bool; }
  bool is_number() const noexcept { return kind_ == Kind::Number; }
  bool is_string() const noexcept { return kind_ == Kind::String; }
  bool is_array() const noexcept { return kind_ == Kind::Array; }
  bool is_object() const noexcept { return kind_ == Kind::Object; }
  // A number whose lexeme has no fraction / exponent and fits in int64.
  bool is_integer() const noexcept;

  // Typed access; the wrong kind throws JsonError naming the node's line and the kind found.
  bool as_bool() const;
  double as_double() const;
  std::int64_t as_int() const;              // exact integers only (no fraction, no exponent)
  const std::string& as_string() const;
  const std::vector<Value>& as_array() const;
  std::vector<Value>& as_array();
  const std::vector<Member>& as_object() const;
  std::vector<Member>& as_object();

  // Object access by key. at() throws on a missing key; find() returns nullptr.
  const Value& at(std::string_view key) const;
  const Value* find(std::string_view key) const noexcept;
  bool has(std::string_view key) const noexcept { return find(key) != nullptr; }
  // Appends (name, value); a duplicate key throws.
  Value& set(std::string key, Value v);
  // Array element; out of range throws.
  const Value& at(std::size_t i) const;
  std::size_t size() const;                 // array items / object members; else throws

  // The numeric lexeme as written (or produced), e.g. "0.1" or "1e-9".
  const std::string& lexeme() const noexcept { return text_; }
  // Source position (1-based; 0 when the value was built in code).
  int line() const noexcept { return line_; }
  int column() const noexcept { return column_; }
  // "origin:line:column" of this value, or "<built>" for a value built in code.
  std::string where() const;

  // A short description for error messages: "object", "array", "number 0.25", "string \"x\"".
  std::string describe() const;

  friend bool operator==(const Value& a, const Value& b);
  friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }

 private:
  friend class Parser;
  Kind kind_ = Kind::Null;
  bool bool_ = false;
  double num_ = 0.0;
  std::string text_;                        // string value, or number lexeme
  std::vector<Value> items_;
  std::vector<Member> members_;
  int line_ = 0;
  int column_ = 0;
  std::string origin_;
};

// Parses a complete document (one value plus surrounding whitespace; anything else is an error).
// `origin` names the source in error messages (a file path, or a label for a literal).
Value parse(std::string_view text, const std::string& origin = "<string>");
// Reads and parses a file; a missing / unreadable file is a JsonError.
Value parse_file(const std::string& path);

// Serialises. indent = 0 gives one line; indent > 0 pretty-prints with that many spaces.
// Numbers print their lexeme, strings escape ", \, control characters (and nothing else).
std::string dump(const Value& v, int indent = 0);
void write_file(const std::string& path, const Value& v, int indent = 2);

}  // namespace epykos::json
