#include "epykos/ir/program.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace epykos::ir {

namespace {

using std::size_t;

size_t idx(std::int32_t i) noexcept { return static_cast<size_t>(i); }

std::uint64_t bits_of(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

double double_of(std::uint64_t u) noexcept {
  double v;
  std::memcpy(&v, &u, sizeof v);
  return v;
}

std::string hex(double v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(bits_of(v)));
  return buf;
}

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("ir: " + what); }

// Per-group names of the slots in order of first use: #k literal, $k column, @k gather, %k segment.
struct SlotNames {
  std::map<std::pair<SlotKind, std::int32_t>, std::int32_t> number;
  std::vector<std::pair<SlotKind, std::int32_t>> order;
  void note(const Slot& s) {
    if (s.kind == SlotKind::None || s.kind == SlotKind::Step || s.kind == SlotKind::Input) return;
    const auto key = std::make_pair(s.kind, s.index);
    if (number.count(key) == 0) {
      number.emplace(key, static_cast<std::int32_t>(order.size()));
      order.push_back(key);
    }
  }
  std::string name(const Slot& s) const {
    switch (s.kind) {
      case SlotKind::Literal: return "#" + std::to_string(number.at({s.kind, s.index}));
      case SlotKind::Column: return "$" + std::to_string(number.at({s.kind, s.index}));
      case SlotKind::Gather: return "@" + std::to_string(number.at({s.kind, s.index}));
      case SlotKind::Segment: return "%" + std::to_string(number.at({s.kind, s.index}));
      case SlotKind::Input: return "in";
      default: return "?";
    }
  }
};

SlotNames slot_names(const Group& g) {
  SlotNames names;
  // Number per kind separately so that each kind starts at 0.
  std::map<SlotKind, std::int32_t> counters;
  for (const Step& s : g.steps) {
    for (const Slot* sl : {&s.a, &s.b, &s.c, &s.konst}) {
      if (sl->kind == SlotKind::None || sl->kind == SlotKind::Step || sl->kind == SlotKind::Input) continue;
      const auto key = std::make_pair(sl->kind, sl->index);
      if (names.number.count(key) == 0) {
        names.number.emplace(key, counters[sl->kind]++);
        names.order.push_back(key);
      }
    }
  }
  return names;
}

void render(const Group& g, const SlotNames& names, std::int32_t k, std::string& out) {
  const Step& s = g.steps[idx(k)];
  auto operand = [&](const Slot& sl) {
    if (sl.kind == SlotKind::Step) {
      render(g, names, sl.index, out);
    } else {
      out += names.name(sl);
    }
  };
  switch (s.op) {
    case Op::Input:
      out += "input";
      return;
    case Op::Const:
      out += "const(";
      out += names.name(s.konst);
      out += ')';
      return;
    case Op::Sum:
      out += "sum(";
      out += names.name(s.a);
      out += ')';
      return;
    case Op::Affine:
      out += "affine(";
      out += names.name(s.konst);
      out += ';';
      out += names.name(s.a);
      out += ')';
      return;
    default:
      break;
  }
  out += to_string(s.op);
  out += '(';
  const int arity = op_arity(s.op);
  if (arity >= 1) operand(s.a);
  if (arity >= 2) {
    out += ',';
    operand(s.b);
  }
  if (arity >= 3) {
    out += ',';
    operand(s.c);
  }
  out += ')';
}

}  // namespace

const char* to_string(SlotKind kind) noexcept {
  switch (kind) {
    case SlotKind::None: return "none";
    case SlotKind::Step: return "step";
    case SlotKind::Literal: return "literal";
    case SlotKind::Column: return "column";
    case SlotKind::Gather: return "gather";
    case SlotKind::Segment: return "segment";
    case SlotKind::Input: return "input";
  }
  return "?";
}

domain_id Program::domain_of(value_id v) const noexcept {
  // First domain whose base is > v, minus one.
  std::int32_t lo = 0, hi = static_cast<std::int32_t>(domains.size());
  while (lo < hi) {
    const std::int32_t mid = lo + (hi - lo) / 2;
    if (domains[idx(mid)].value_base <= v) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo - 1;
}

row_id Program::row_of(value_id v) const noexcept { return v - domains[idx(domain_of(v))].value_base; }

std::vector<domain_id> recurrent_domains(const Program& p) {
  std::vector<domain_id> out;
  for (size_t d = 0; d < p.domains.size(); ++d) {
    if (p.domains[d].recurrent) out.push_back(static_cast<domain_id>(d));
  }
  return out;
}

std::string shape_string(const Program& p, domain_id d) {
  const Group& g = p.groups[idx(d)];
  if (g.steps.empty()) return "<empty>";
  const SlotNames names = slot_names(g);
  if (g.steps.size() > 24) {
    // Long groups are named by their last op and step count. No whitespace: serialize() writes
    // the name as one token.
    return std::string(to_string(g.steps.back().op)) + "[" + std::to_string(g.steps.size()) + "]";
  }
  std::string out;
  render(g, names, static_cast<std::int32_t>(g.steps.size()) - 1, out);
  return out;
}

// ---- validate ---------------------------------------------------------------------------

void validate(const Program& p) {
  const size_t n_dom = p.domains.size();
  if (p.groups.size() != n_dom) fail("groups and domains differ in count");
  value_id base = 0;
  for (size_t d = 0; d < n_dom; ++d) {
    const Domain& dom = p.domains[d];
    if (dom.rows <= 0) fail("domain " + std::to_string(d) + " has no rows");
    if (dom.value_base != base) fail("domain " + std::to_string(d) + " value_base is not contiguous");
    base += dom.rows;
    if (p.groups[d].domain != static_cast<domain_id>(d)) fail("group " + std::to_string(d) + " belongs to another domain");
    for (domain_id r : dom.reads) {
      if (r < 0 || idx(r) >= n_dom) fail("domain " + std::to_string(d) + " reads an unknown domain");
      if (r > static_cast<domain_id>(d)) fail("domain " + std::to_string(d) + " reads a later domain");
      if (r == static_cast<domain_id>(d) && !dom.recurrent) fail("domain " + std::to_string(d) + " reads itself but is not recurrent");
    }
  }
  const value_id n_values = base;
  for (size_t i = 0; i < p.columns.size(); ++i) {
    const Column& c = p.columns[i];
    if (c.domain < 0 || idx(c.domain) >= n_dom) fail("column " + std::to_string(i) + " has a bad domain");
    if (c.values.size() != idx(p.domains[idx(c.domain)].rows)) fail("column " + std::to_string(i) + " has a bad length");
  }
  for (size_t i = 0; i < p.gathers.size(); ++i) {
    const Gather& g = p.gathers[i];
    if (g.domain < 0 || idx(g.domain) >= n_dom) fail("gather " + std::to_string(i) + " has a bad domain");
    const Domain& dom = p.domains[idx(g.domain)];
    if (g.index.size() != idx(dom.rows)) fail("gather " + std::to_string(i) + " has a bad length");
    for (size_t r = 0; r < g.index.size(); ++r) {
      const value_id v = g.index[r];
      const value_id limit = dom.recurrent ? dom.value_base + static_cast<value_id>(r) : dom.value_base;
      if (v < 0 || v >= limit) fail("gather " + std::to_string(i) + " row " + std::to_string(r) + " reads a value that is not evaluated yet");
    }
  }
  for (size_t i = 0; i < p.segments.size(); ++i) {
    const Segment& s = p.segments[i];
    if (s.domain < 0 || idx(s.domain) >= n_dom) fail("segment " + std::to_string(i) + " has a bad domain");
    const Domain& dom = p.domains[idx(s.domain)];
    if (s.offsets.size() != idx(dom.rows) + 1 || s.offsets.front() != 0 ||
        s.offsets.back() != static_cast<std::int32_t>(s.members.size())) {
      fail("segment " + std::to_string(i) + " has bad offsets");
    }
    if (!s.coefs.empty() && s.coefs.size() != s.members.size()) fail("segment " + std::to_string(i) + " has bad coefs");
    for (size_t r = 0; r < idx(dom.rows); ++r) {
      if (s.offsets[r + 1] < s.offsets[r] + 1) fail("segment " + std::to_string(i) + " row " + std::to_string(r) + " has no members");
      const value_id limit = dom.recurrent ? dom.value_base + static_cast<value_id>(r) : dom.value_base;
      for (std::int32_t m = s.offsets[r]; m < s.offsets[r + 1]; ++m) {
        const value_id v = s.members[idx(m)];
        if (v < 0 || v >= limit) fail("segment " + std::to_string(i) + " row " + std::to_string(r) + " reads a value that is not evaluated yet");
      }
    }
  }
  std::vector<std::int32_t> input_rows(idx(n_values), 0);
  for (size_t k = 0; k < p.inputs.size(); ++k) {
    const value_id v = p.inputs[k];
    if (v < 0 || v >= n_values) fail("input " + std::to_string(k) + " out of range");
    ++input_rows[idx(v)];
  }
  if (p.input_values.size() != p.inputs.size()) fail("input_values and inputs differ in length");
  for (size_t d = 0; d < n_dom; ++d) {
    const Group& g = p.groups[d];
    if (g.steps.empty()) fail("group " + std::to_string(d) + " is empty");
    for (size_t k = 0; k < g.steps.size(); ++k) {
      const Step& s = g.steps[k];
      const std::string where = "group " + std::to_string(d) + " step " + std::to_string(k);
      if (!op_is_supported(s.op)) fail(where + ": unsupported op");
      auto check = [&](const Slot& sl, bool required) {
        if (sl.kind == SlotKind::None) {
          if (required) fail(where + ": missing operand");
          return;
        }
        switch (sl.kind) {
          case SlotKind::Step:
            if (sl.index < 0 || idx(sl.index) >= k) fail(where + ": step operand is not earlier");
            break;
          case SlotKind::Literal:
            if (sl.index < 0 || idx(sl.index) >= p.literals.size()) fail(where + ": literal out of range");
            break;
          case SlotKind::Column:
            if (sl.index < 0 || idx(sl.index) >= p.columns.size() || p.columns[idx(sl.index)].domain != static_cast<domain_id>(d)) {
              fail(where + ": column out of range or of another domain");
            }
            break;
          case SlotKind::Gather:
            if (sl.index < 0 || idx(sl.index) >= p.gathers.size() || p.gathers[idx(sl.index)].domain != static_cast<domain_id>(d)) {
              fail(where + ": gather out of range or of another domain");
            }
            break;
          case SlotKind::Segment:
            if (sl.index < 0 || idx(sl.index) >= p.segments.size() || p.segments[idx(sl.index)].domain != static_cast<domain_id>(d)) {
              fail(where + ": segment out of range or of another domain");
            }
            break;
          case SlotKind::Input:
            if (s.op != Op::Input) fail(where + ": input slot on a non-Input step");
            break;
          default:
            break;
        }
      };
      if (s.op == Op::Input) {
        if (s.a.kind != SlotKind::Input) fail(where + ": Input step needs an input slot");
        if (g.steps.size() != 1) fail(where + ": Input must be the only step of its group");
      } else if (s.op == Op::Const) {
        if (s.konst.kind != SlotKind::Literal && s.konst.kind != SlotKind::Column) fail(where + ": Const step needs a value slot");
        check(s.konst, true);
      } else if (s.op == Op::Sum || s.op == Op::Affine) {
        if (s.a.kind != SlotKind::Segment) fail(where + ": variadic step needs a segment");
        check(s.a, true);
        const Segment& seg = p.segments[idx(s.a.index)];
        if (s.op == Op::Sum && !seg.coefs.empty()) fail(where + ": Sum segment carries coefficients");
        if (s.op == Op::Affine) {
          if (seg.coefs.size() != seg.members.size()) fail(where + ": Affine segment needs one coefficient per member");
          if (s.konst.kind != SlotKind::Literal && s.konst.kind != SlotKind::Column) fail(where + ": Affine needs a c_0 slot");
          check(s.konst, true);
        }
      } else {
        const int arity = op_arity(s.op);
        check(s.a, arity >= 1);
        check(s.b, arity >= 2);
        check(s.c, arity >= 3);
        if (s.a.kind == SlotKind::Segment || s.b.kind == SlotKind::Segment || s.c.kind == SlotKind::Segment) {
          fail(where + ": segment operand on a fixed-arity op");
        }
      }
    }
    // Input rows are exactly the inputs.
    const Domain& dom = p.domains[d];
    const bool is_input = g.steps.back().op == Op::Input;
    for (std::int32_t r = 0; r < dom.rows; ++r) {
      const std::int32_t c = input_rows[idx(dom.value_base + r)];
      if (is_input && c != 1) fail("Input domain " + std::to_string(d) + " row " + std::to_string(r) + " is registered " + std::to_string(c) + " times");
      if (!is_input && c != 0) fail("domain " + std::to_string(d) + " row " + std::to_string(r) + " is registered as an input but is not one");
    }
  }
  for (size_t k = 0; k < p.outputs.size(); ++k) {
    if (p.outputs[k] < 0 || p.outputs[k] >= n_values) fail("output " + std::to_string(k) + " out of range");
  }
}

// ---- dump -------------------------------------------------------------------------------

void dump(const Program& p, std::ostream& os) {
  os << "program: " << p.domains.size() << " domains, " << p.num_values() << " values, " << p.literals.size()
     << " literals, " << p.columns.size() << " columns, " << p.gathers.size() << " gathers, " << p.segments.size()
     << " segments, " << p.inputs.size() << " inputs, " << p.outputs.size() << " outputs\n";
  for (size_t d = 0; d < p.domains.size(); ++d) {
    const Domain& dom = p.domains[d];
    const Group& g = p.groups[d];
    os << "d" << d << " rows=" << dom.rows << " base=" << dom.value_base << " level=" << dom.level;
    if (dom.recurrent) os << " recurrent";
    os << " reads={";
    for (size_t i = 0; i < dom.reads.size(); ++i) os << (i ? "," : "") << 'd' << dom.reads[i];
    os << "} steps=" << g.steps.size() << ' ' << dom.name << '\n';
    const SlotNames names = slot_names(g);
    for (const auto& key : names.order) {
      const Slot sl{key.first, key.second};
      os << "    " << names.name(sl) << " = ";
      switch (key.first) {
        case SlotKind::Literal:
          os << "literal " << p.literals[idx(key.second)];
          break;
        case SlotKind::Column:
          os << "column " << key.second;
          break;
        case SlotKind::Gather: {
          const Gather& ga = p.gathers[idx(key.second)];
          std::map<domain_id, std::int32_t> from;
          for (value_id v : ga.index) ++from[p.domain_of(v)];
          os << "gather " << key.second << " from";
          for (const auto& [src, cnt] : from) os << " d" << src << "(" << cnt << ")";
          break;
        }
        case SlotKind::Segment: {
          const Segment& seg = p.segments[idx(key.second)];
          std::int32_t lo = 0, hi = 0;
          for (size_t r = 0; r + 1 < seg.offsets.size(); ++r) {
            const std::int32_t len = seg.offsets[r + 1] - seg.offsets[r];
            lo = (r == 0) ? len : std::min(lo, len);
            hi = std::max(hi, len);
          }
          std::map<domain_id, std::int32_t> from;
          for (value_id v : seg.members) ++from[p.domain_of(v)];
          os << "segment " << key.second << " members=" << seg.members.size() << " per-row " << lo << ".." << hi
             << (seg.coefs.empty() ? "" : " with coefs") << " from";
          for (const auto& [src, cnt] : from) os << " d" << src << "(" << cnt << ")";
          break;
        }
        default:
          break;
      }
      os << '\n';
    }
  }
  os << "inputs:";
  for (value_id v : p.inputs) os << ' ' << v;
  os << "\noutputs: " << p.outputs.size();
  if (!p.outputs.empty()) {
    std::map<domain_id, std::int32_t> from;
    for (value_id v : p.outputs) ++from[p.domain_of(v)];
    os << " from";
    for (const auto& [src, cnt] : from) os << " d" << src << "(" << cnt << ")";
  }
  os << '\n';
}

std::string to_string(const Program& p) {
  std::ostringstream os;
  dump(p, os);
  return os.str();
}

// ---- serialisation ----------------------------------------------------------------------

namespace {

void put_slot(std::ostream& os, const Slot& s) { os << ' ' << static_cast<int>(s.kind) << ' ' << s.index; }

Slot get_slot(std::istream& is) {
  int kind = 0;
  Slot s;
  if (!(is >> kind >> s.index)) fail("deserialize: bad slot");
  s.kind = static_cast<SlotKind>(kind);
  return s;
}

template <class T>
T get(std::istream& is, const char* what) {
  T v;
  if (!(is >> v)) fail(std::string("deserialize: expected ") + what);
  return v;
}

double get_double(std::istream& is) {
  std::string s;
  if (!(is >> s)) fail("deserialize: expected a hex double");
  return double_of(std::stoull(s, nullptr, 16));
}

void expect(std::istream& is, const char* word) {
  std::string s;
  if (!(is >> s) || s != word) fail(std::string("deserialize: expected '") + word + "', got '" + s + "'");
}

}  // namespace

std::string serialize(const Program& p) {
  std::ostringstream os;
  os << "epykos-ir 1\n";
  os << "literals " << p.literals.size();
  for (double v : p.literals) os << ' ' << hex(v);
  os << '\n';
  os << "domains " << p.domains.size() << '\n';
  for (const Domain& d : p.domains) {
    os << "domain " << d.rows << ' ' << d.value_base << ' ' << d.level << ' ' << (d.recurrent ? 1 : 0) << ' '
       << d.reads.size();
    for (domain_id r : d.reads) os << ' ' << r;
    // The name is one whitespace-delimited token (deserialize reads it with >>).
    for (char c : d.name) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') fail("serialize: domain name '" + d.name + "' contains whitespace");
    }
    os << ' ' << (d.name.empty() ? "-" : d.name) << '\n';
  }

  os << "groups " << p.groups.size() << '\n';
  for (const Group& g : p.groups) {
    os << "group " << g.domain << ' ' << g.steps.size() << '\n';
    for (const Step& s : g.steps) {
      os << "step " << static_cast<int>(s.op);
      put_slot(os, s.a);
      put_slot(os, s.b);
      put_slot(os, s.c);
      put_slot(os, s.konst);
      os << '\n';
    }
  }
  os << "columns " << p.columns.size() << '\n';
  for (const Column& c : p.columns) {
    os << "column " << c.domain << ' ' << c.values.size();
    for (double v : c.values) os << ' ' << hex(v);
    os << '\n';
  }
  os << "gathers " << p.gathers.size() << '\n';
  for (const Gather& g : p.gathers) {
    os << "gather " << g.domain << ' ' << g.index.size();
    for (value_id v : g.index) os << ' ' << v;
    os << '\n';
  }
  os << "segments " << p.segments.size() << '\n';
  for (const Segment& s : p.segments) {
    os << "segment " << s.domain << ' ' << s.offsets.size() << ' ' << s.members.size() << ' ' << s.coefs.size() << '\n';
    os << "offsets";
    for (std::int32_t o : s.offsets) os << ' ' << o;
    os << "\nmembers";
    for (value_id v : s.members) os << ' ' << v;
    os << "\ncoefs";
    for (double c : s.coefs) os << ' ' << hex(c);
    os << '\n';
  }
  os << "inputs " << p.inputs.size();
  for (value_id v : p.inputs) os << ' ' << v;
  os << '\n';
  os << "input_values " << p.input_values.size();
  for (double v : p.input_values) os << ' ' << hex(v);
  os << '\n';
  os << "outputs " << p.outputs.size();
  for (value_id v : p.outputs) os << ' ' << v;
  os << "\nend\n";
  return os.str();
}

Program deserialize(const std::string& text) {
  std::istringstream is(text);
  Program p;
  expect(is, "epykos-ir");
  if (get<int>(is, "version") != 1) fail("deserialize: unsupported version");
  expect(is, "literals");
  {
    const size_t n = get<size_t>(is, "literal count");
    for (size_t i = 0; i < n; ++i) p.literals.push_back(get_double(is));
  }
  expect(is, "domains");
  {
    const size_t n = get<size_t>(is, "domain count");
    for (size_t i = 0; i < n; ++i) {
      expect(is, "domain");
      Domain d;
      d.rows = get<std::int32_t>(is, "rows");
      d.value_base = get<value_id>(is, "base");
      d.level = get<std::int32_t>(is, "level");
      d.recurrent = get<int>(is, "recurrent") != 0;
      const size_t nr = get<size_t>(is, "reads count");
      for (size_t k = 0; k < nr; ++k) d.reads.push_back(get<domain_id>(is, "read"));
      d.name = get<std::string>(is, "name");
      if (d.name == "-") d.name.clear();
      p.domains.push_back(std::move(d));
    }
  }
  expect(is, "groups");
  {
    const size_t n = get<size_t>(is, "group count");
    for (size_t i = 0; i < n; ++i) {
      expect(is, "group");
      Group g;
      g.domain = get<domain_id>(is, "group domain");
      const size_t ns = get<size_t>(is, "step count");
      for (size_t k = 0; k < ns; ++k) {
        expect(is, "step");
        Step s;
        const int op = get<int>(is, "op");
        if (op < 0 || op >= op_count) fail("deserialize: bad op");
        s.op = static_cast<Op>(op);
        s.a = get_slot(is);
        s.b = get_slot(is);
        s.c = get_slot(is);
        s.konst = get_slot(is);
        g.steps.push_back(s);
      }
      p.groups.push_back(std::move(g));
    }
  }
  expect(is, "columns");
  {
    const size_t n = get<size_t>(is, "column count");
    for (size_t i = 0; i < n; ++i) {
      expect(is, "column");
      Column c;
      c.domain = get<domain_id>(is, "column domain");
      const size_t nv = get<size_t>(is, "column length");
      for (size_t k = 0; k < nv; ++k) c.values.push_back(get_double(is));
      p.columns.push_back(std::move(c));
    }
  }
  expect(is, "gathers");
  {
    const size_t n = get<size_t>(is, "gather count");
    for (size_t i = 0; i < n; ++i) {
      expect(is, "gather");
      Gather g;
      g.domain = get<domain_id>(is, "gather domain");
      const size_t nv = get<size_t>(is, "gather length");
      for (size_t k = 0; k < nv; ++k) g.index.push_back(get<value_id>(is, "gather index"));
      p.gathers.push_back(std::move(g));
    }
  }
  expect(is, "segments");
  {
    const size_t n = get<size_t>(is, "segment count");
    for (size_t i = 0; i < n; ++i) {
      expect(is, "segment");
      Segment s;
      s.domain = get<domain_id>(is, "segment domain");
      const size_t no = get<size_t>(is, "offset count");
      const size_t nm = get<size_t>(is, "member count");
      const size_t nc = get<size_t>(is, "coef count");
      expect(is, "offsets");
      for (size_t k = 0; k < no; ++k) s.offsets.push_back(get<std::int32_t>(is, "offset"));
      expect(is, "members");
      for (size_t k = 0; k < nm; ++k) s.members.push_back(get<value_id>(is, "member"));
      expect(is, "coefs");
      for (size_t k = 0; k < nc; ++k) s.coefs.push_back(get_double(is));
      p.segments.push_back(std::move(s));
    }
  }
  expect(is, "inputs");
  {
    const size_t n = get<size_t>(is, "input count");
    for (size_t k = 0; k < n; ++k) p.inputs.push_back(get<value_id>(is, "input"));
  }
  expect(is, "input_values");
  {
    const size_t n = get<size_t>(is, "input value count");
    for (size_t k = 0; k < n; ++k) p.input_values.push_back(get_double(is));
  }
  expect(is, "outputs");
  {
    const size_t n = get<size_t>(is, "output count");
    for (size_t k = 0; k < n; ++k) p.outputs.push_back(get<value_id>(is, "output"));
  }
  expect(is, "end");
  validate(p);
  return p;
}

}  // namespace epykos::ir
