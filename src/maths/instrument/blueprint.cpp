#include "epykos/maths/instrument/blueprint.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>

#ifndef EPYKOS_BLUEPRINTS_DIR
#define EPYKOS_BLUEPRINTS_DIR ""
#endif

namespace epykos::instrument {

using json::Value;
using conventions::Period;

namespace {

[[noreturn]] void fail(const Value& at, const std::string& what) { throw BlueprintError("blueprints: " + at.where() + ": " + what); }
[[noreturn]] void fail(const std::string& what) { throw BlueprintError("blueprints: " + what); }

void check_keys(const Value& obj, std::initializer_list<const char*> allowed, std::initializer_list<const char*> required,
                const std::string& what) {
  if (!obj.is_object()) fail(obj, what + ": expected an object, found " + obj.describe());
  for (const auto& m : obj.as_object()) {
    bool ok = false;
    for (const char* a : allowed) {
      if (m.first == a) {
        ok = true;
        break;
      }
    }
    if (!ok) fail(m.second, what + ": unknown key \"" + m.first + "\"");
  }
  for (const char* r : required) {
    if (!obj.has(r)) fail(obj, what + ": missing required key \"" + r + "\"");
  }
}

std::string str(const Value& obj, const char* key) { return obj.at(key).as_string(); }
std::string str_or(const Value& obj, const char* key, const std::string& dflt = "") {
  const Value* v = obj.find(key);
  return v ? v->as_string() : dflt;
}
double num_or(const Value& obj, const char* key, double dflt) {
  const Value* v = obj.find(key);
  return v ? v->as_double() : dflt;
}

std::vector<std::string> string_list(const Value& v, const std::string& what) {
  std::vector<std::string> out;
  if (v.is_string()) {
    out.push_back(v.as_string());
    return out;
  }
  if (!v.is_array()) fail(v, what + ": expected a string or an array of strings");
  for (const Value& s : v.as_array()) out.push_back(s.as_string());
  return out;
}

conventions::Sources parse_sources(const Value& obj) {
  conventions::Sources out;
  const Value* v = obj.find("sources");
  if (!v) return out;
  if (!v->is_object()) fail(*v, "\"sources\": expected an object { topic: citation | [citations] }");
  for (const auto& m : v->as_object()) out.emplace_back(m.first, string_list(m.second, "sources"));
  return out;
}

Period period_of(const Value& v) {
  try {
    return Period::parse(v.as_string());
  } catch (const conventions::DateError& e) {
    fail(v, e.what());
  }
}

int side_of(const Value& v) {
  if (v.is_number()) {
    const double s = v.as_double();
    if (s == 1.0) return +1;
    if (s == -1.0) return -1;
    fail(v, "\"side\" must be +1 / -1 or receive | pay | long | short | lend | borrow");
  }
  const std::string s = v.as_string();
  if (s == "receive" || s == "long" || s == "lend") return +1;
  if (s == "pay" || s == "short" || s == "borrow") return -1;
  fail(v, "\"side\" must be +1 / -1 or receive | pay | long | short | lend | borrow");
}

conventions::ObservationSpec parse_observation(const Value& v) {
  check_keys(v, {"method", "lookback_days", "lockout_days"}, {"method"}, "observation");
  conventions::ObservationSpec spec;
  try {
    spec.method = conventions::observation_method_from_string(str(v, "method"));
  } catch (const conventions::DateError& e) {
    fail(v.at("method"), e.what());
  }
  if (v.has("lookback_days")) spec.lookback_days = static_cast<int>(v.at("lookback_days").as_int());
  if (v.has("lockout_days")) spec.lockout_days = static_cast<int>(v.at("lockout_days").as_int());
  return spec;
}

Blueprint parse_blueprint(const std::string& name, const Value& o) {
  const std::string what = "blueprint \"" + name + "\"";
  check_keys(o, {"convention", "description", "legs", "notional", "side", "tenor", "start", "fixed_rate", "spread", "traded_price", "sources"},
             {"convention"}, what);
  Blueprint b;
  b.name = name;
  b.convention = str(o, "convention");
  b.description = str_or(o, "description");
  if (o.has("legs")) {
    const Value& legs = o.at("legs");
    if (!legs.is_array()) fail(legs, what + ": \"legs\" must be an array");
    for (const Value& lv : legs.as_array()) {
      check_keys(lv, {"coupon", "observation", "spread"}, {"coupon"}, what + " leg");
      LegBlueprint lb;
      try {
        lb.coupon = coupon_kind_from_string(str(lv, "coupon"));
      } catch (const std::invalid_argument& e) {
        fail(lv.at("coupon"), e.what());
      }
      if (lv.has("observation")) {
        if (lb.coupon != CouponKind::RfrCompounded && lb.coupon != CouponKind::RfrAveraged) {
          fail(lv.at("observation"), what + ": \"observation\" applies to rfr_compounded / rfr_averaged legs only");
        }
        lb.observation = parse_observation(lv.at("observation"));
      }
      if (lv.has("spread")) lb.spread = lv.at("spread").as_bool();
      b.legs.push_back(lb);
    }
  }
  b.notional = num_or(o, "notional", 1.0);
  if (o.has("side")) b.side = side_of(o.at("side"));
  if (o.has("tenor")) b.tenor = period_of(o.at("tenor"));
  b.start = str_or(o, "start", "spot");
  if (b.start != "spot") {
    try {
      (void)conventions::parse_iso(b.start);
    } catch (const conventions::DateError& e) {
      fail(o.at("start"), std::string("\"start\" must be \"spot\" or an ISO date: ") + e.what());
    }
  }
  b.fixed_rate = num_or(o, "fixed_rate", 0.0);
  b.spread = num_or(o, "spread", 0.0);
  b.traded_price = num_or(o, "traded_price", 0.0);
  b.sources = parse_sources(o);
  return b;
}

CurveDefinition parse_curve(const std::string& name, const Value& o) {
  const std::string what = "curve \"" + name + "\"";
  check_keys(o, {"index", "currency", "description", "scheme", "variable", "regions", "knots", "instruments", "sources"},
             {"index", "instruments"}, what);
  CurveDefinition c;
  c.name = name;
  c.index = str(o, "index");
  c.currency = str_or(o, "currency");
  c.description = str_or(o, "description");
  auto scheme_of = [&](const Value& v) {
    try {
      return curve::parse_scheme(v.as_string());
    } catch (const std::invalid_argument& e) {
      fail(v, e.what());
    }
  };
  auto variable_of = [&](const Value& v) {
    try {
      return curve::parse_variable(v.as_string());
    } catch (const std::invalid_argument& e) {
      fail(v, e.what());
    }
  };
  if (o.has("regions")) {
    if (o.has("scheme") || o.has("variable")) fail(o.at("regions"), what + ": give \"regions\" or \"scheme\" / \"variable\", not both");
    const Value& rs = o.at("regions");
    if (!rs.is_array() || rs.size() == 0) fail(rs, what + ": \"regions\" must be a non-empty array");
    for (std::size_t i = 0; i < rs.size(); ++i) {
      const Value& rv = rs.at(i);
      check_keys(rv, {"from", "scheme", "variable"}, {"scheme", "variable"}, what + " region");
      CurveRegionDef r;
      r.from = str_or(rv, "from");
      if (i == 0 && !r.from.empty()) fail(rv.at("from"), what + ": the first region starts at t = 0 (no \"from\")");
      if (i > 0 && r.from.empty()) fail(rv, what + ": region " + std::to_string(i) + " needs \"from\"");
      if (!r.from.empty()) (void)period_of(rv.at("from"));
      r.scheme = scheme_of(rv.at("scheme"));
      r.variable = variable_of(rv.at("variable"));
      c.regions.push_back(r);
    }
  } else {
    CurveRegionDef r;
    if (o.has("scheme")) r.scheme = scheme_of(o.at("scheme"));
    if (o.has("variable")) r.variable = variable_of(o.at("variable"));
    c.regions.push_back(r);
  }
  if (o.has("knots")) {
    const Value& k = o.at("knots");
    if (k.is_string()) {
      if (k.as_string() != "maturities") fail(k, what + ": \"knots\" must be \"maturities\" or a list of tenors");
    } else {
      c.knot_tenors = string_list(k, what + " knots");
      for (const std::string& t : c.knot_tenors) {
        try {
          (void)Period::parse(t);
        } catch (const conventions::DateError& e) {
          fail(k, e.what());
        }
      }
    }
  }
  const Value& ins = o.at("instruments");
  if (!ins.is_array() || ins.size() == 0) fail(ins, what + ": \"instruments\" must be a non-empty array");
  for (const Value& iv : ins.as_array()) {
    check_keys(iv, {"blueprint", "tenors", "tenor", "key", "contracts"}, {"blueprint"}, what + " instrument");
    CurveInstrumentEntry e;
    e.blueprint = str(iv, "blueprint");
    const int forms = (iv.has("tenors") ? 1 : 0) + (iv.has("tenor") ? 1 : 0) + (iv.has("contracts") ? 1 : 0);
    if (forms != 1) fail(iv, what + ": an instrument entry has exactly one of \"tenors\", \"tenor\", \"contracts\"");
    if (iv.has("tenors")) {
      if (iv.has("key")) fail(iv.at("key"), what + ": \"key\" goes with a single \"tenor\"");
      e.tenors = string_list(iv.at("tenors"), what + " tenors");
      if (e.tenors.empty()) fail(iv.at("tenors"), what + ": empty tenor list");
    } else if (iv.has("tenor")) {
      e.tenors.push_back(str(iv, "tenor"));
      if (iv.has("key")) e.keys.push_back(str(iv, "key"));
    } else {
      e.contracts = static_cast<int>(iv.at("contracts").as_int());
      if (e.contracts < 1) fail(iv.at("contracts"), what + ": \"contracts\" must be positive");
    }
    for (const std::string& t : e.tenors) {
      try {
        (void)Period::parse(t);
      } catch (const conventions::DateError& ex) {
        fail(iv, ex.what());
      }
    }
    c.instruments.push_back(e);
  }
  c.sources = parse_sources(o);
  return c;
}

}  // namespace

std::string Blueprints::default_root() {
  if (const char* env = std::getenv("EPYKOS_BLUEPRINTS")) {
    if (*env) return std::string(env);
  }
  const std::string compiled = EPYKOS_BLUEPRINTS_DIR;
  if (compiled.empty()) fail("no blueprints directory: set EPYKOS_BLUEPRINTS or build from the source tree");
  return compiled;
}

Blueprints Blueprints::load(const std::string& root) {
  std::vector<std::string> paths;
  for (const char* sub : {"instruments", "curves"}) {
    const std::string dir = root + "/" + sub;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (entry.path().extension() == ".json" && entry.path().filename().string()[0] != '.') {
        paths.push_back(entry.path().string());
      }
    }
    if (ec) fail("cannot read directory \"" + dir + "\": " + ec.message());
  }
  if (paths.empty()) fail("no *.json files under \"" + root + "/instruments\" or \"" + root + "/curves\"");
  std::sort(paths.begin(), paths.end());
  return load_files(paths);
}

Blueprints Blueprints::load_files(const std::vector<std::string>& paths) {
  Blueprints b;
  for (const std::string& p : paths) b.add_document(json::parse_file(p), p);
  return b;
}

void Blueprints::add_file(const std::string& path) { add_document(json::parse_file(path), path); }

void Blueprints::add_document(const Value& doc, const std::string& origin) {
  check_keys(doc, {"meta", "blueprints", "curves"}, {"meta"}, "document");
  const Value& meta = doc.at("meta");
  check_keys(meta, {"title", "description", "sources", "notes"}, {"title"}, "meta");
  BlueprintFileMeta fm;
  fm.path = origin;
  fm.title = str(meta, "title");
  fm.description = str_or(meta, "description");
  if (meta.has("sources")) fm.sources = string_list(meta.at("sources"), "meta.sources");
  if (meta.has("notes")) fm.notes = string_list(meta.at("notes"), "meta.notes");
  files_.push_back(fm);
  if (doc.has("blueprints")) {
    const Value& bs = doc.at("blueprints");
    if (!bs.is_object()) fail(bs, "\"blueprints\" must be an object keyed by name");
    for (const auto& m : bs.as_object()) {
      if (blueprints_.count(m.first)) fail(m.second, "blueprint \"" + m.first + "\" is defined twice");
      blueprints_.emplace(m.first, parse_blueprint(m.first, m.second));
    }
  }
  if (doc.has("curves")) {
    const Value& cs = doc.at("curves");
    if (!cs.is_object()) fail(cs, "\"curves\" must be an object keyed by name");
    for (const auto& m : cs.as_object()) {
      if (curves_.count(m.first)) fail(m.second, "curve \"" + m.first + "\" is defined twice");
      curves_.emplace(m.first, parse_curve(m.first, m.second));
    }
  }
}

const Blueprint& Blueprints::blueprint(const std::string& name) const {
  auto it = blueprints_.find(name);
  if (it == blueprints_.end()) fail("no blueprint named \"" + name + "\"");
  return it->second;
}

const CurveDefinition& Blueprints::curve(const std::string& name) const {
  auto it = curves_.find(name);
  if (it == curves_.end()) fail("no curve definition named \"" + name + "\"");
  return it->second;
}

std::vector<std::string> Blueprints::blueprint_names() const {
  std::vector<std::string> v;
  for (const auto& kv : blueprints_) v.push_back(kv.first);
  return v;
}

std::vector<std::string> Blueprints::curve_names() const {
  std::vector<std::string> v;
  for (const auto& kv : curves_) v.push_back(kv.first);
  return v;
}

CouponKind default_coupon_kind(const conventions::Registry& registry, const conventions::LegConvention& leg) {
  using conventions::IndexDef;
  using conventions::LegConvention;
  if (leg.role == LegConvention::Role::Fixed) return CouponKind::Fixed;
  const IndexDef& index = registry.index(leg.index);
  if (index.kind == IndexDef::Kind::Overnight) {
    if (leg.compounding == LegConvention::Compounding::Compounded) return CouponKind::RfrCompounded;
    if (leg.compounding == LegConvention::Compounding::Averaged) return CouponKind::RfrAveraged;
    fail("an overnight leg on " + leg.index + " must be compounded or averaged");
  }
  if (leg.compounding != LegConvention::Compounding::None) {
    fail("a term-rate leg on " + leg.index + " cannot be compounded / averaged");
  }
  return CouponKind::TermRate;
}

void Blueprints::validate(const conventions::Registry& registry) const {
  using conventions::InstrumentConvention;
  for (const auto& kv : blueprints_) {
    const Blueprint& b = kv.second;
    if (!registry.has_instrument(b.convention)) fail("blueprint \"" + b.name + "\": no convention named \"" + b.convention + "\"");
    const InstrumentConvention& conv = registry.instrument(b.convention);
    const bool swap = conv.type == InstrumentConvention::Type::OIS || conv.type == InstrumentConvention::Type::IRS ||
                      conv.type == InstrumentConvention::Type::Basis;
    if (!b.legs.empty()) {
      if (!swap) fail("blueprint \"" + b.name + "\": \"legs\" apply to a swap convention (" + b.convention + " is not one)");
      if (b.legs.size() != conv.legs.size()) {
        fail("blueprint \"" + b.name + "\": " + std::to_string(b.legs.size()) + " legs for a convention with " +
             std::to_string(conv.legs.size()));
      }
      for (std::size_t l = 0; l < b.legs.size(); ++l) {
        const CouponKind dflt = default_coupon_kind(registry, conv.legs[l]);
        const CouponKind kind = b.legs[l].coupon;
        const bool rfr = (kind == CouponKind::RfrCompounded || kind == CouponKind::RfrAveraged) &&
                         (dflt == CouponKind::RfrCompounded || dflt == CouponKind::RfrAveraged);
        if (kind != dflt && !rfr) {
          fail("blueprint \"" + b.name + "\": leg " + std::to_string(l) + " records as " + to_string(kind) + " but the convention's leg is " +
               to_string(dflt));
        }
      }
    }
  }
  for (const auto& kv : curves_) {
    const CurveDefinition& c = kv.second;
    if (!registry.has_index(c.index)) fail("curve \"" + c.name + "\": no index named \"" + c.index + "\"");
    for (const CurveInstrumentEntry& e : c.instruments) {
      auto it = blueprints_.find(e.blueprint);
      if (it == blueprints_.end()) fail("curve \"" + c.name + "\": no blueprint named \"" + e.blueprint + "\"");
      const InstrumentConvention& conv = registry.instrument(it->second.convention);
      const bool future = conv.type == InstrumentConvention::Type::Future;
      if (future != (e.contracts > 0)) {
        fail("curve \"" + c.name + "\": \"" + e.blueprint + "\" " + (future ? "is a future: give \"contracts\"" : "is not a future: give tenors"));
      }
    }
  }
}

}  // namespace epykos::instrument
