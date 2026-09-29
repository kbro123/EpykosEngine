#include "epykos/util/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace epykos::json {

// ---------------------------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------------------------

Value Value::null() { return Value(); }

Value Value::boolean(bool b) {
  Value v;
  v.kind_ = Kind::Bool;
  v.bool_ = b;
  return v;
}

Value Value::number(double d) {
  if (!std::isfinite(d)) throw JsonError("json: a number must be finite (got " + std::to_string(d) + ")");
  Value v;
  v.kind_ = Kind::Number;
  v.num_ = d;
  // Shortest lexeme that round-trips: try %.15g, %.16g, %.17g.
  char buf[40];
  for (int prec = 15; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, d);
    if (std::strtod(buf, nullptr) == d) break;
  }
  v.text_ = buf;
  // "1e+20" style is valid JSON; "inf" cannot occur (checked above). Ensure a leading digit for
  // negatives / no leading '+'.
  return v;
}

Value Value::number(std::int64_t i) {
  Value v;
  v.kind_ = Kind::Number;
  v.num_ = static_cast<double>(i);
  v.text_ = std::to_string(i);
  return v;
}

Value Value::string(std::string s) {
  Value v;
  v.kind_ = Kind::String;
  v.text_ = std::move(s);
  return v;
}

Value Value::array(std::vector<Value> items) {
  Value v;
  v.kind_ = Kind::Array;
  v.items_ = std::move(items);
  return v;
}

Value Value::object(std::vector<Member> members) {
  Value v;
  v.kind_ = Kind::Object;
  for (auto& m : members) v.set(std::move(m.first), std::move(m.second));
  return v;
}

std::string Value::where() const {
  if (line_ == 0) return "<built>";
  return origin_ + ":" + std::to_string(line_) + ":" + std::to_string(column_);
}

std::string Value::describe() const {
  switch (kind_) {
    case Kind::Null: return "null";
    case Kind::Bool: return bool_ ? "true" : "false";
    case Kind::Number: return "number " + text_;
    case Kind::String: return "string \"" + text_ + "\"";
    case Kind::Array: return "array[" + std::to_string(items_.size()) + "]";
    case Kind::Object: return "object{" + std::to_string(members_.size()) + "}";
  }
  return "?";
}

[[noreturn]] static void wrong_kind(const Value& v, const char* wanted) {
  throw JsonError("json: " + v.where() + ": expected " + wanted + ", found " + v.describe());
}

bool Value::is_integer() const noexcept {
  if (kind_ != Kind::Number) return false;
  for (char c : text_) {
    if (c == '.' || c == 'e' || c == 'E') return false;
  }
  // fits int64: strtoll without overflow
  errno = 0;
  char* end = nullptr;
  (void)std::strtoll(text_.c_str(), &end, 10);
  return errno != ERANGE && end != nullptr && *end == '\0';
}

bool Value::as_bool() const {
  if (kind_ != Kind::Bool) wrong_kind(*this, "bool");
  return bool_;
}

double Value::as_double() const {
  if (kind_ != Kind::Number) wrong_kind(*this, "number");
  return num_;
}

std::int64_t Value::as_int() const {
  if (kind_ != Kind::Number) wrong_kind(*this, "integer");
  if (!is_integer()) throw JsonError("json: " + where() + ": expected an integer, found " + describe());
  return std::strtoll(text_.c_str(), nullptr, 10);
}

const std::string& Value::as_string() const {
  if (kind_ != Kind::String) wrong_kind(*this, "string");
  return text_;
}

const std::vector<Value>& Value::as_array() const {
  if (kind_ != Kind::Array) wrong_kind(*this, "array");
  return items_;
}

std::vector<Value>& Value::as_array() {
  if (kind_ != Kind::Array) wrong_kind(*this, "array");
  return items_;
}

const std::vector<Member>& Value::as_object() const {
  if (kind_ != Kind::Object) wrong_kind(*this, "object");
  return members_;
}

std::vector<Member>& Value::as_object() {
  if (kind_ != Kind::Object) wrong_kind(*this, "object");
  return members_;
}

const Value* Value::find(std::string_view key) const noexcept {
  if (kind_ != Kind::Object) return nullptr;
  for (const auto& m : members_) {
    if (m.first == key) return &m.second;
  }
  return nullptr;
}

const Value& Value::at(std::string_view key) const {
  if (kind_ != Kind::Object) wrong_kind(*this, "object");
  const Value* v = find(key);
  if (!v) throw JsonError("json: " + where() + ": missing key \"" + std::string(key) + "\"");
  return *v;
}

Value& Value::set(std::string key, Value v) {
  if (kind_ != Kind::Object) wrong_kind(*this, "object");
  if (find(key)) throw JsonError("json: " + where() + ": duplicate key \"" + key + "\"");
  members_.emplace_back(std::move(key), std::move(v));
  return members_.back().second;
}

const Value& Value::at(std::size_t i) const {
  if (kind_ != Kind::Array) wrong_kind(*this, "array");
  if (i >= items_.size()) {
    throw JsonError("json: " + where() + ": index " + std::to_string(i) + " out of range (size " +
                    std::to_string(items_.size()) + ")");
  }
  return items_[i];
}

std::size_t Value::size() const {
  if (kind_ == Kind::Array) return items_.size();
  if (kind_ == Kind::Object) return members_.size();
  wrong_kind(*this, "array or object");
}

bool operator==(const Value& a, const Value& b) {
  if (a.kind_ != b.kind_) return false;
  switch (a.kind_) {
    case Value::Kind::Null: return true;
    case Value::Kind::Bool: return a.bool_ == b.bool_;
    case Value::Kind::Number: return a.text_ == b.text_;   // lexeme identity: exact round-trip
    case Value::Kind::String: return a.text_ == b.text_;
    case Value::Kind::Array: return a.items_ == b.items_;
    case Value::Kind::Object: return a.members_ == b.members_;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------

class Parser {
 public:
  Parser(std::string_view text, std::string origin) : s_(text), origin_(std::move(origin)) {}

  Value parse_document() {
    skip_ws();
    if (at_end()) fail("empty document");
    Value v = parse_value(0);
    skip_ws();
    if (!at_end()) fail("trailing characters after the document");
    return v;
  }

 private:
  static constexpr int kMaxDepth = 512;

  std::string_view s_;
  std::string origin_;
  std::size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;

  bool at_end() const noexcept { return pos_ >= s_.size(); }
  char peek() const noexcept { return at_end() ? '\0' : s_[pos_]; }

  char take() {
    char c = s_[pos_++];
    if (c == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    return c;
  }

  [[noreturn]] void fail(const std::string& what) const {
    throw JsonError("json: " + origin_ + ":" + std::to_string(line_) + ":" + std::to_string(col_) + ": " + what);
  }

  [[noreturn]] void fail_at(int line, int col, const std::string& what) const {
    throw JsonError("json: " + origin_ + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " + what);
  }

  void skip_ws() {
    while (!at_end()) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        take();
      } else {
        break;
      }
    }
  }

  void expect_literal(const char* lit) {
    for (const char* p = lit; *p; ++p) {
      if (at_end() || peek() != *p) fail(std::string("invalid literal, expected \"") + lit + "\"");
      take();
    }
  }

  Value parse_value(int depth) {
    if (depth > kMaxDepth) fail("nesting deeper than " + std::to_string(kMaxDepth));
    const int line = line_;
    const int col = col_;
    Value v;
    switch (peek()) {
      case '{': v = parse_object(depth); break;
      case '[': v = parse_array(depth); break;
      case '"': v = Value::string(parse_string()); break;
      case 't': expect_literal("true"); v = Value::boolean(true); break;
      case 'f': expect_literal("false"); v = Value::boolean(false); break;
      case 'n': expect_literal("null"); v = Value::null(); break;
      default:
        if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
          v = parse_number();
        } else if (at_end()) {
          fail("unexpected end of input, expected a value");
        } else {
          fail(std::string("unexpected character '") + peek() + "', expected a value");
        }
    }
    v.line_ = line;
    v.column_ = col;
    v.origin_ = origin_;
    return v;
  }

  Value parse_object(int depth) {
    take();  // '{'
    Value obj = Value::object();
    skip_ws();
    if (peek() == '}') {
      take();
      return obj;
    }
    for (;;) {
      skip_ws();
      if (peek() != '"') {
        if (at_end()) fail("unexpected end of input inside an object");
        if (peek() == '}') fail("trailing comma before '}'");
        fail(std::string("expected a string key, found '") + peek() + "'");
      }
      const int kline = line_;
      const int kcol = col_;
      std::string key = parse_string();
      skip_ws();
      if (peek() != ':') {
        if (at_end()) fail("unexpected end of input, expected ':'");
        fail(std::string("expected ':' after key \"") + key + "\"");
      }
      take();
      skip_ws();
      Value val = parse_value(depth + 1);
      if (obj.find(key)) fail_at(kline, kcol, "duplicate key \"" + key + "\"");
      obj.members_.emplace_back(std::move(key), std::move(val));
      skip_ws();
      if (peek() == ',') {
        take();
        continue;
      }
      if (peek() == '}') {
        take();
        return obj;
      }
      if (at_end()) fail("unexpected end of input inside an object");
      fail(std::string("expected ',' or '}' in object, found '") + peek() + "'");
    }
  }

  Value parse_array(int depth) {
    take();  // '['
    Value arr = Value::array();
    skip_ws();
    if (peek() == ']') {
      take();
      return arr;
    }
    for (;;) {
      skip_ws();
      if (peek() == ']') fail("trailing comma before ']'");
      arr.items_.push_back(parse_value(depth + 1));
      skip_ws();
      if (peek() == ',') {
        take();
        continue;
      }
      if (peek() == ']') {
        take();
        return arr;
      }
      if (at_end()) fail("unexpected end of input inside an array");
      fail(std::string("expected ',' or ']' in array, found '") + peek() + "'");
    }
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  std::uint32_t parse_hex4() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      if (at_end()) fail("unexpected end of input inside a \\u escape");
      char c = take();
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        fail(std::string("invalid hex digit '") + c + "' in \\u escape");
      }
    }
    return v;
  }

  std::string parse_string() {
    take();  // opening quote
    std::string out;
    for (;;) {
      if (at_end()) fail("unterminated string");
      char c = take();
      if (c == '"') return out;
      if (c == '\\') {
        if (at_end()) fail("unterminated escape sequence");
        char e = take();
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            std::uint32_t cp = parse_hex4();
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              // high surrogate: a \uDC00..\uDFFF must follow
              if (at_end() || peek() != '\\') fail("high surrogate not followed by a low surrogate");
              take();
              if (at_end() || peek() != 'u') fail("high surrogate not followed by a low surrogate");
              take();
              std::uint32_t lo = parse_hex4();
              if (lo < 0xDC00 || lo > 0xDFFF) fail("high surrogate not followed by a low surrogate");
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              fail("lone low surrogate in \\u escape");
            }
            append_utf8(out, cp);
            break;
          }
          default: fail(std::string("invalid escape '\\") + e + "'");
        }
        continue;
      }
      if (static_cast<unsigned char>(c) < 0x20) fail("control character inside a string (escape it)");
      out += c;
    }
  }

  Value parse_number() {
    const std::size_t start = pos_;
    const int line = line_;
    const int col = col_;
    if (peek() == '-') take();
    if (at_end()) fail("unexpected end of input inside a number");
    if (peek() == '0') {
      take();
      if (peek() >= '0' && peek() <= '9') fail("leading zeros are not allowed in a number");
    } else if (peek() >= '1' && peek() <= '9') {
      while (peek() >= '0' && peek() <= '9') take();
    } else {
      fail(std::string("invalid number: expected a digit, found '") + peek() + "'");
    }
    if (peek() == '.') {
      take();
      if (!(peek() >= '0' && peek() <= '9')) fail("invalid number: expected a digit after '.'");
      while (peek() >= '0' && peek() <= '9') take();
    }
    if (peek() == 'e' || peek() == 'E') {
      take();
      if (peek() == '+' || peek() == '-') take();
      if (!(peek() >= '0' && peek() <= '9')) fail("invalid number: expected a digit in the exponent");
      while (peek() >= '0' && peek() <= '9') take();
    }
    std::string lex(s_.substr(start, pos_ - start));
    errno = 0;
    char* end = nullptr;
    double d = std::strtod(lex.c_str(), &end);
    if (end != lex.c_str() + lex.size()) fail_at(line, col, "invalid number \"" + lex + "\"");
    if (errno == ERANGE && std::isinf(d)) fail_at(line, col, "number out of range \"" + lex + "\"");
    Value v;
    v.kind_ = Value::Kind::Number;
    v.num_ = d;
    v.text_ = std::move(lex);
    return v;
  }
};

Value parse(std::string_view text, const std::string& origin) {
  Parser p(text, origin);
  return p.parse_document();
}

Value parse_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw JsonError("json: cannot open \"" + path + "\"");
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse(ss.str(), path);
}

// ---------------------------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------------------------

static void escape_string(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

static void dump_into(std::string& out, const Value& v, int indent, int level) {
  auto newline = [&](int lvl) {
    if (indent > 0) {
      out += '\n';
      out.append(static_cast<std::size_t>(indent * lvl), ' ');
    }
  };
  switch (v.kind()) {
    case Value::Kind::Null: out += "null"; break;
    case Value::Kind::Bool: out += v.as_bool() ? "true" : "false"; break;
    case Value::Kind::Number: out += v.lexeme(); break;
    case Value::Kind::String: escape_string(out, v.as_string()); break;
    case Value::Kind::Array: {
      const auto& items = v.as_array();
      if (items.empty()) {
        out += "[]";
        break;
      }
      out += '[';
      for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += ',';
        newline(level + 1);
        dump_into(out, items[i], indent, level + 1);
      }
      newline(level);
      out += ']';
      break;
    }
    case Value::Kind::Object: {
      const auto& members = v.as_object();
      if (members.empty()) {
        out += "{}";
        break;
      }
      out += '{';
      for (std::size_t i = 0; i < members.size(); ++i) {
        if (i) out += ',';
        newline(level + 1);
        escape_string(out, members[i].first);
        out += indent > 0 ? ": " : ":";
        dump_into(out, members[i].second, indent, level + 1);
      }
      newline(level);
      out += '}';
      break;
    }
  }
}

std::string dump(const Value& v, int indent) {
  std::string out;
  dump_into(out, v, indent, 0);
  return out;
}

void write_file(const std::string& path, const Value& v, int indent) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw JsonError("json: cannot write \"" + path + "\"");
  out << dump(v, indent) << '\n';
}

}  // namespace epykos::json
