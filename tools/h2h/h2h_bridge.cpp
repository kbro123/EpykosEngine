// The exchange, implemented. Read h2h_bridge.hpp's header comment first: it holds the per-coupon
// algebraic identities this file is nothing more than a transcription of.
#include "h2h_bridge.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

#include <boost/json.hpp>

#include "swaps/api/codec.hpp"

namespace h2h {

namespace {

namespace ei = epykos::instrument;
namespace px = swaps::pricing;
namespace json = boost::json;

[[noreturn]] void bad(const std::string& what) { throw std::runtime_error("h2h bridge: " + what); }

// Every coupon of a leg must share one notional for the position's scalar `notional` to carry it;
// otherwise it would have to ride each coupon's `scale`, which costs them their own `standard()`
// fast path and so would measure a handicap we imposed rather than their engine. Amortising legs
// do not occur in either fixture; if one ever does, this says so instead of quietly slowing them.
double one_notional(const ei::Leg& leg, const std::string& who) {
  if (leg.coupons.empty()) bad(who + ": an empty leg");
  const double n = leg.coupons.front().notional;
  for (const ei::Coupon& c : leg.coupons)
    if (c.notional != n) bad(who + ": the leg amortises (" + std::to_string(c.notional) + " vs " + std::to_string(n) +
                             "); their Position carries one notional per leg, so this needs per-coupon `scale`");
  return n;
}

// True when a compounded coupon's day product telescopes to ONE discount-factor ratio: every day's
// compounding weight equals its own forward accrual (w_i/tau_r == 1) and the days are contiguous.
// This is the SAME condition `epykos::compile`'s telescope peephole fires on (D81), so the two
// engines are handed the same algebra.
bool telescopes(const ei::Leg& leg, const ei::Coupon& c) {
  if (c.obs_end <= c.obs_begin) return false;
  for (int i = c.obs_begin; i < c.obs_end; ++i) {
    const ei::ObsDay& d = leg.obs[static_cast<std::size_t>(i)];
    if (!(d.tau_rate > 0.0)) return false;
    if (d.weight != d.tau_rate) return false;
    if (i > c.obs_begin && d.t_rate != leg.obs[static_cast<std::size_t>(i - 1)].t_next) return false;
  }
  return true;
}

// Our (leg, coupon) as their FloatCoupon. `unit` is the notional the enclosing Position carries.
px::FloatCoupon float_coupon(const ei::Leg& leg, const ei::Coupon& c, double unit, const std::string& who,
                             Mapping* m) {
  px::FloatCoupon f;
  f.pay = c.t_pay;
  f.tau_pay = c.accrual;
  f.spread = c.spread;
  f.scale = (unit != 0.0) ? c.notional / unit : c.notional;
  switch (c.kind) {
    case ei::CouponKind::RfrCompounded: {
      if (!(c.obs_tau > 0.0)) bad(who + ": a compounded coupon with obs_tau = " + std::to_string(c.obs_tau));
      f.obs.tau_index = c.obs_tau;
      const bool seasoned = c.realised_factor != 1.0;
      if (m != nullptr && seasoned) ++m->seasoned_coupons;
      if (c.obs_end <= c.obs_begin) {
        // Fully realised: their `rate = (realized_factor - 1)/tau_index`, ours `(rf - 1)/obs_tau`.
        f.obs.compounded = true;
        f.obs.realized_factor = c.realised_factor;
        if (m != nullptr) ++m->coupons_compounded;
        break;
      }
      const ei::ObsDay& first = leg.obs[static_cast<std::size_t>(c.obs_begin)];
      const ei::ObsDay& last = leg.obs[static_cast<std::size_t>(c.obs_end - 1)];
      const int days = c.obs_end - c.obs_begin;
      if (telescopes(leg, c)) {
        f.obs.sub_start.push_back(first.t_rate);
        f.obs.sub_end.push_back(last.t_next);
        if (seasoned) {
          // rf · (1 + (DF(s)/DF(e) - 1)) - 1 == rf · DF(s)/DF(e) - 1, which is ours exactly.
          f.obs.compounded = true;
          f.obs.realized_factor = c.realised_factor;
        }
        // else: one arithmetic sub-period, weight implicit 1 -- their `standard()` shape.
        if (m != nullptr) {
          ++m->coupons_telescoped;
          ++m->obs_days_handed_over;
          m->obs_days_collapsed += days - 1;
        }
        break;
      }
      // A lockout (or any lookback that does not shift): the product is the product.
      f.obs.compounded = true;
      f.obs.realized_factor = c.realised_factor;
      for (int i = c.obs_begin; i < c.obs_end; ++i) {
        const ei::ObsDay& d = leg.obs[static_cast<std::size_t>(i)];
        if (!(d.tau_rate > 0.0)) bad(who + ": an observation day with tau_rate = " + std::to_string(d.tau_rate));
        f.obs.sub_start.push_back(d.t_rate);
        f.obs.sub_end.push_back(d.t_next);
        f.obs.weight.push_back(d.weight / d.tau_rate);
      }
      if (m != nullptr) {
        ++m->coupons_compounded;
        m->obs_days_handed_over += days;
      }
      break;
    }
    case ei::CouponKind::RfrAveraged: {
      if (!(c.obs_days > 0.0)) bad(who + ": an averaged coupon with obs_days = " + std::to_string(c.obs_days));
      f.obs.tau_index = c.obs_days;      // ours divides by the observation period's CALENDAR DAYS
      f.obs.realized = c.realised_sum;   // ours seeds the sum with the realised day-weighted rates
      if (m != nullptr && c.realised_sum != 0.0) ++m->seasoned_coupons;
      for (int i = c.obs_begin; i < c.obs_end; ++i) {
        const ei::ObsDay& d = leg.obs[static_cast<std::size_t>(i)];
        if (!(d.tau_rate > 0.0)) bad(who + ": an observation day with tau_rate = " + std::to_string(d.tau_rate));
        f.obs.sub_start.push_back(d.t_rate);
        f.obs.sub_end.push_back(d.t_next);
        f.obs.weight.push_back(d.weight_days / d.tau_rate);
      }
      if (m != nullptr) {
        ++m->coupons_averaged;
        m->obs_days_handed_over += c.obs_end - c.obs_begin;
      }
      break;
    }
    case ei::CouponKind::TermRate: {
      if (c.realised) {
        // R = the fixing. Theirs: realized/tau_index with no sub-period.
        const double tau = (c.fix_tau > 0.0) ? c.fix_tau : c.accrual;
        if (!(tau > 0.0)) bad(who + ": a realised term coupon with no accrual to annualise on");
        f.obs.tau_index = tau;
        f.obs.realized = c.rate * tau;
        if (m != nullptr) {
          ++m->coupons_term_fixed;
          ++m->seasoned_coupons;
        }
        break;
      }
      if (!(c.fix_tau > 0.0)) bad(who + ": a projected term coupon with fix_tau = " + std::to_string(c.fix_tau));
      f.obs.tau_index = c.fix_tau;
      f.obs.sub_start.push_back(c.t_fix_start);
      f.obs.sub_end.push_back(c.t_fix_end);
      if (m != nullptr) {
        ++m->coupons_term_live;
        ++m->obs_days_handed_over;
      }
      break;
    }
    case ei::CouponKind::Fixed:
      bad(who + ": a fixed coupon reached the floating-coupon mapping");
  }
  return f;
}

px::FixedCoupon fixed_coupon(const ei::Coupon& c, double unit) {
  px::FixedCoupon x;
  x.pay = c.t_pay;
  x.tau = c.accrual;
  x.scale = (unit != 0.0) ? c.notional / unit : c.notional;
  return x;
}

// A whole floating leg, per unit of `unit`.
std::vector<px::FloatCoupon> float_leg(const ei::Leg& leg, double unit, const std::string& who, Mapping* m,
                                       bool drop_spread = false) {
  std::vector<px::FloatCoupon> out;
  out.reserve(leg.coupons.size());
  for (const ei::Coupon& c : leg.coupons) {
    px::FloatCoupon f = float_coupon(leg, c, unit, who, m);
    if (drop_spread) f.spread = 0.0;
    out.push_back(std::move(f));
  }
  return out;
}

std::vector<px::FixedCoupon> fixed_leg(const ei::Leg& leg, double unit, const std::string& who, Mapping* m) {
  std::vector<px::FixedCoupon> out;
  out.reserve(leg.coupons.size());
  for (const ei::Coupon& c : leg.coupons) {
    if (c.kind != ei::CouponKind::Fixed) bad(who + ": a non-fixed coupon on what should be the fixed leg");
    out.push_back(fixed_coupon(c, unit));
    if (m != nullptr) ++m->coupons_fixed;
  }
  return out;
}

// The fixed leg's per-coupon rates, or an empty vector when every coupon carries the same one (in
// which case their scalar `fixed_rate` is used and the expression stays byte-identical to a plain
// swap on their side).
std::vector<double> fixed_rates(const ei::Leg& leg) {
  const double r = leg.coupons.front().rate;
  bool uniform = true;
  for (const ei::Coupon& c : leg.coupons)
    if (c.rate != r) uniform = false;
  if (uniform) return {};
  std::vector<double> out;
  for (const ei::Coupon& c : leg.coupons) out.push_back(c.rate);
  return out;
}

// --- the annuity rows of a leg, for a ParRate / ParSpread denominator -----------------------------
std::vector<px::FixedCoupon> annuity_rows(const ei::Leg& leg, double unit, Mapping* m) {
  std::vector<px::FixedCoupon> out;
  out.reserve(leg.coupons.size());
  for (const ei::Coupon& c : leg.coupons) out.push_back(fixed_coupon(c, unit));
  if (m != nullptr) m->coupons_fixed += static_cast<long long>(leg.coupons.size());
  return out;
}

// --- one of our book trades as one or two of their positions ---------------------------------------
void positions_of(const ei::Instrument& in, std::vector<pf::MultiCurveBook::Position>& out, Mapping* m) {
  const std::string who = "trade '" + in.id + "' (" + ei::to_string(in.kind) + ")";
  const double side = static_cast<double>(in.side);
  switch (in.kind) {
    case ei::Kind::Ois:
    case ei::Kind::Irs:
    case ei::Kind::Deposit: {
      // Ours: pv = side·(Σ legs[0] − Σ legs[1]), legs[0] the fixed leg.
      // Theirs: notional·(float_leg_pv − fixed_rate·annuity).  => notional = −side·N.
      if (in.legs.size() != 2) bad(who + ": needs two legs");
      const ei::Leg& fx = in.legs[0];
      const ei::Leg& fl = in.legs[1];
      const double nf = one_notional(fx, who + " fixed leg");
      const double nl = one_notional(fl, who + " float leg");
      if (nf != nl) bad(who + ": the two legs carry different notionals (" + std::to_string(nf) + " vs " +
                        std::to_string(nl) + "); their Position carries ONE");
      pf::MultiCurveBook::Position p;
      p.kind = pf::MultiCurveBook::Kind::Swap;
      p.notional = -side * nl;
      p.float_coupons = float_leg(fl, nl, who, m);
      p.fwd_curve = fl.curve;
      p.disc_curve = fl.disc_curve;
      p.fixed_coupons = fixed_leg(fx, nf, who, m);
      p.fixed_curve = fx.disc_curve;
      p.fixed_rate = fx.coupons.front().rate;
      p.fixed_rates = fixed_rates(fx);
      out.push_back(std::move(p));
      break;
    }
    case ei::Kind::Basis: {
      // Float vs float. Their Position is ONE float leg plus a fixed annuity, so a basis trade is
      // written as TWO positions with opposite notionals: pv = side·(spread leg − flat leg). The
      // book NPV and the ladder are both linear in the positions, so the split is exact; it is
      // recorded in the Mapping because their `n` then counts more positions than we have trades.
      if (in.legs.size() != 2) bad(who + ": needs two legs");
      for (int l = 0; l < 2; ++l) {
        const ei::Leg& leg = in.legs[static_cast<std::size_t>(l)];
        const double n = one_notional(leg, who + " leg " + std::to_string(l));
        pf::MultiCurveBook::Position p;
        p.kind = pf::MultiCurveBook::Kind::Swap;
        p.notional = (l == 0 ? +1.0 : -1.0) * side * n;
        p.float_coupons = float_leg(leg, n, who, m);
        p.fwd_curve = leg.curve;
        p.disc_curve = leg.disc_curve;
        out.push_back(std::move(p));
      }
      if (m != nullptr) ++m->basis_split;
      break;
    }
    case ei::Kind::Future:
      // pv = side·N·(price − traded)/100, UNDISCOUNTED and not a swap. Their MultiCurveBook has
      // two position kinds, Swap and Xccy, and no undiscounted variation-margin flow: a futures
      // POSITION is not expressible in that book schema. Stage A's book mix carries none (futures
      // appear only as calibration instruments, where QuoteKind::Rate carries them exactly), so
      // this is a reported gap rather than a blocker -- see bench/compare/README.md §4.
      bad(who + ": their MultiCurveBook has no undiscounted futures position (Swap | Xccy only)");
  }
}

// --- one of our calibration instruments as one of their Instruments --------------------------------
cal::Instrument instrument_of(const ei::Instrument& in, double quote, bool quote_is_price, Mapping* m) {
  const std::string who = "calibration instrument '" + in.id + "' (" + ei::to_string(in.kind) + ")";
  cal::Instrument t;
  switch (in.kind) {
    case ei::Kind::Ois:
    case ei::Kind::Irs: {
      // ours: par = leg_pv(legs[1]) / leg_annuity(legs[0]).
      // theirs: par_rate = float_leg_pv(fwd) / annuity(fixed).
      if (in.legs.size() != 2) bad(who + ": needs two legs");
      const ei::Leg& fx = in.legs[0];
      const ei::Leg& fl = in.legs[1];
      t.quote = cal::QuoteKind::ParRate;
      t.fwd.coupons = float_leg(fl, one_notional(fl, who), who, m);
      t.fwd.forecast = fl.curve;
      t.fwd.discount = fl.disc_curve;
      t.fixed.coupons = annuity_rows(fx, one_notional(fx, who), m);
      t.fixed.discount = fx.disc_curve;
      t.market = quote;
      break;
    }
    case ei::Kind::Basis: {
      // ours: par = (leg_pv(legs[1]) − leg_pv(legs[0], no spread)) / leg_annuity(legs[0]).
      // theirs: par_spread = (pv(bench) − pv(fwd)) / annuity(fixed).
      if (in.legs.size() != 2) bad(who + ": needs two legs");
      const ei::Leg& sp = in.legs[0];   // the spread leg: the quote's own leg
      const ei::Leg& flat = in.legs[1];
      const double n = one_notional(sp, who);
      t.quote = cal::QuoteKind::ParSpread;
      t.fwd.coupons = float_leg(sp, n, who, m, /*drop_spread=*/true);
      t.fwd.forecast = sp.curve;
      t.fwd.discount = sp.disc_curve;
      t.bench.coupons = float_leg(flat, one_notional(flat, who), who, m);
      t.bench.forecast = flat.curve;
      t.bench.discount = flat.disc_curve;
      t.fixed.coupons = annuity_rows(sp, n, m);
      t.fixed.discount = sp.disc_curve;
      t.market = quote;
      break;
    }
    case ei::Kind::Deposit: {
      // ours: par = coupon_rate(legs[1], its one coupon). theirs: Rate = rate(obs) + convexity.
      if (in.legs.size() != 2) bad(who + ": needs two legs");
      const ei::Leg& fl = in.legs[1];
      if (fl.coupons.size() != 1) bad(who + ": a deposit needs exactly one index coupon");
      t.quote = cal::QuoteKind::Rate;
      t.obs = float_coupon(fl, fl.coupons.front(), one_notional(fl, who), who, m).obs;
      t.forecast = fl.curve;
      t.convexity = 0.0;
      t.market = quote;
      break;
    }
    case ei::Kind::Future: {
      // ours: residual = rate − (1 − q/100), convexity ZERO (D35). theirs: Rate, market in RATE
      // units -- so their market is the rate the price implies, and the ladder column that comes
      // back is per unit of RATE where ours is per unit of PRICE. `market_jacobian` is the one
      // conversion, applied to OUR ladder.
      if (in.legs.size() != 1 || in.legs[0].coupons.size() != 1)
        bad(who + ": a future needs one leg with one reference-period coupon");
      const ei::Leg& fl = in.legs[0];
      t.quote = cal::QuoteKind::Rate;
      t.obs = float_coupon(fl, fl.coupons.front(), one_notional(fl, who), who, m).obs;
      t.forecast = fl.curve;
      t.convexity = 0.0;
      t.market = quote_is_price ? (1.0 - quote / 100.0) : quote;
      break;
    }
  }
  if (!quote_is_price && in.kind == ei::Kind::Future)
    bad(who + ": a future whose quote is not flagged as a price");
  if (m != nullptr) ++m->cal_instruments;
  return t;
}

json::value fcpn_json(const px::FloatCoupon& c) {
  json::object o;
  json::object obs;
  obs["sub_start"] = json::array(c.obs.sub_start.begin(), c.obs.sub_start.end());
  obs["sub_end"] = json::array(c.obs.sub_end.begin(), c.obs.sub_end.end());
  obs["weight"] = json::array(c.obs.weight.begin(), c.obs.weight.end());
  obs["realized"] = c.obs.realized;
  obs["tau_index"] = c.obs.tau_index;
  obs["compounded"] = c.obs.compounded;
  obs["realized_factor"] = c.obs.realized_factor;
  o["obs"] = std::move(obs);
  o["pay"] = c.pay;
  o["tau_pay"] = c.tau_pay;
  o["spread"] = c.spread;
  o["scale"] = c.scale;
  return o;
}

json::value book_json(const pf::MultiCurveBook& b) {
  json::array ps;
  for (const pf::MultiCurveBook::Position& p : b.positions) {
    json::object o;
    o["kind"] = "swap";
    o["notional"] = p.notional;
    o["fixed_rate"] = p.fixed_rate;
    o["fwd_curve"] = p.fwd_curve;
    o["disc_curve"] = p.disc_curve;
    json::array fc;
    for (const px::FloatCoupon& c : p.float_coupons) fc.push_back(fcpn_json(c));
    o["float_coupons"] = std::move(fc);
    if (!p.fixed_coupons.empty()) {
      json::array xc;
      for (const px::FixedCoupon& c : p.fixed_coupons) {
        json::object x;
        x["pay"] = c.pay;
        x["tau"] = c.tau;
        x["scale"] = c.scale;
        xc.push_back(std::move(x));
      }
      o["fixed_coupons"] = std::move(xc);
      o["fixed_curve"] = p.fixed_curve;   // REQUIRED by their decoder when fixed_coupons is present
    }
    ps.push_back(std::move(o));
  }
  json::object out;
  out["positions"] = std::move(ps);
  return out;
}

}  // namespace

std::string Mapping::to_string() const {
  char b[1024];
  std::snprintf(b, sizeof b,
                "%lld calibration instruments, %lld book trades -> %lld of their positions "
                "(%lld basis trades split in two)\n"
                "  coupons: %lld fixed, %lld compounded-telescoped, %lld compounded day-by-day, "
                "%lld averaged, %lld term live, %lld term fixed; %lld seasoned\n"
                "  observation sub-periods handed over %lld, collapsed by the telescope %lld",
                cal_instruments, book_trades, their_positions, basis_split, coupons_fixed, coupons_telescoped,
                coupons_compounded, coupons_averaged, coupons_term_live, coupons_term_fixed, seasoned_coupons,
                obs_days_handed_over, obs_days_collapsed);
  return b;
}

cal::BundleProblem bundle_of(const ProblemView& v, Mapping* m) {
  cal::BundleProblem p;
  // One region per curve, Scheme::Flat over every knot: piecewise-constant instantaneous forward,
  // i.e. log DF piecewise linear on {0, knots...}. That is the family D71 MEASURED their bundle
  // curve to be, and the family our -LOGDF curve definitions are on. README §3.
  std::vector<std::string> ccy;
  for (const CurveView& c : v.curves) {
    cal::BundleCurveSpec s;
    s.base = -1;
    int tag = -1;
    for (std::size_t i = 0; i < ccy.size(); ++i)
      if (ccy[i] == c.currency) tag = static_cast<int>(i);
    if (tag < 0) {
      tag = static_cast<int>(ccy.size());
      ccy.push_back(c.currency);
    }
    s.currency = tag;
    swaps::curve::CurveModule mod;
    mod.scheme = swaps::curve::Scheme::Flat;
    mod.knots = c.knot_t;
    s.regions.push_back(std::move(mod));
    p.curves.push_back(std::move(s));
  }
  p.currency_codes = ccy;
  for (std::size_t k = 0; k < v.cal.size(); ++k)
    p.instruments.push_back(instrument_of(*v.cal[k], v.quotes[k], v.quote_is_price[k] != 0, m));
  cal::validate_problem(p, "h2h bundle");
  return p;
}

pf::MultiCurveBook book_of(const ProblemView& v, Mapping* m) { return book_of_family(v, std::string(), m); }

pf::MultiCurveBook book_of_family(const ProblemView& v, const std::string& family, Mapping* m) {
  pf::MultiCurveBook b;
  for (std::size_t i = 0; i < v.book.size(); ++i) {
    if (!family.empty() && v.family[i] != family) continue;
    positions_of(*v.book[i], b.positions, m);
    if (m != nullptr) ++m->book_trades;
  }
  if (m != nullptr) m->their_positions = static_cast<long long>(b.positions.size());
  return b;
}

pf::MultiCurveBook book_of_trade(const ProblemView& v, std::size_t i) {
  pf::MultiCurveBook b;
  positions_of(*v.book.at(i), b.positions, nullptr);
  return b;
}

std::vector<double> market_jacobian(const ProblemView& v) {
  std::vector<double> d(v.quotes.size(), 1.0);
  for (std::size_t k = 0; k < d.size(); ++k)
    if (v.quote_is_price[k] != 0) d[k] = -0.01;   // market = 1 - price/100
  return d;
}

std::vector<double> max_time_read(const ProblemView& v) {
  std::vector<double> t(v.curves.size(), 0.0);
  const auto hit = [&](int slot, double x) {
    if (slot >= 0 && static_cast<std::size_t>(slot) < t.size()) t[static_cast<std::size_t>(slot)] =
        std::max(t[static_cast<std::size_t>(slot)], x);
  };
  const auto walk = [&](const ei::Instrument& in) {
    for (const ei::Leg& leg : in.legs) {
      for (const ei::Coupon& c : leg.coupons) {
        hit(leg.disc_curve, c.t_pay);
        if (c.kind == ei::CouponKind::TermRate) hit(leg.curve, c.t_fix_end);
      }
      for (const ei::ObsDay& d : leg.obs) hit(leg.curve, d.t_next);
    }
  };
  for (const ei::Instrument* in : v.cal) walk(*in);
  for (const ei::Instrument* in : v.book) walk(*in);
  return t;
}

std::string exchange_json(const ProblemView& v) {
  Mapping m;
  const cal::BundleProblem p = bundle_of(v, &m);
  const pf::MultiCurveBook b = book_of(v, &m);
  json::object doc;
  doc["format"] = "epykos-h2h-exchange 1";
  doc["problem"] = v.label;
  // THEIR OWN serialiser, so the bundle half of this document is their schema by construction.
  doc["bundle"] = swaps::api::bundle_to_json(p);
  doc["book"] = book_json(b);
  return json::serialize(doc);
}

}  // namespace h2h
