#include "epykos/maths/instrument/builder.hpp"

#include <algorithm>
#include <stdexcept>

#include "epykos/conventions/calendar.hpp"
#include "epykos/conventions/imm.hpp"
#include "epykos/conventions/rfr.hpp"
#include "epykos/conventions/schedule.hpp"

namespace epykos::instrument {

using namespace conventions;

namespace {

[[noreturn]] void fail(const std::string& what) { throw BlueprintError("instrument builder: " + what); }

double basis_of(DayCount dc, const std::string& index) {
  switch (dc) {
    case DayCount::Act360: return 360.0;
    case DayCount::Act365F: return 365.0;
    default: break;
  }
  fail("index " + index + ": an overnight index needs an ACT/360 or ACT/365F day count for compounding");
}

std::string discount_index_of(const Registry& reg, const InstrumentConvention& conv) {
  if (!conv.discount_index.empty()) return conv.discount_index;
  return reg.currency(conv.currency).discount_index;
}

// The trade's fields with the blueprint's defaults filled in.
struct Resolved {
  std::string id;
  double notional = 1.0;
  int side = +1;
  Period tenor{0, 'D'};
  bool has_tenor = false;
  double fixed_rate = 0.0;
  double spread = 0.0;
  double traded_price = 0.0;
};

Resolved resolve(const Blueprint& bp, const Trade& trade) {
  Resolved r;
  r.notional = trade.notional.value_or(bp.notional);
  r.side = trade.side.value_or(bp.side);
  if (r.side != 1 && r.side != -1) fail("trade '" + trade.id + "': side must be +1 or -1");
  if (trade.tenor) {
    r.tenor = *trade.tenor;
    r.has_tenor = true;
  } else if (bp.tenor) {
    r.tenor = *bp.tenor;
    r.has_tenor = true;
  }
  r.fixed_rate = trade.fixed_rate.value_or(bp.fixed_rate);
  r.spread = trade.spread.value_or(bp.spread);
  r.traded_price = trade.traded_price.value_or(bp.traded_price);
  r.id = trade.id;
  return r;
}

Date effective_of(const BuildContext& ctx, const Blueprint& bp, const InstrumentConvention& conv, const Trade& trade, Date trade_date) {
  if (trade.effective != 0) return trade.effective;
  if (bp.start != "spot") return parse_iso(bp.start);
  return spot_date(trade_date, conv.spot_lag, ctx.registry->calendar(conv.calendar));
}

// The accrual to the valuation date of a period that started before it (0 otherwise), and the
// flags of the row.
void finish_coupon(const BuildContext& ctx, Coupon& c, DayCount dc, double rate_known) {
  c.current = c.start < ctx.valuation && ctx.valuation < c.end;
  if (c.start < ctx.valuation) {
    const Date to = std::min(c.end, ctx.valuation);
    c.accrued = c.notional * (rate_known + c.spread) * year_fraction(dc, c.start, to);
  }
}

Coupon fixed_coupon(const BuildContext& ctx, Date start, Date end, Date pay, double notional, double K, DayCount dc) {
  Coupon c;
  c.kind = CouponKind::Fixed;
  c.start = start;
  c.end = end;
  c.pay = pay;
  c.notional = notional;
  c.accrual = year_fraction(dc, start, end);
  c.t_start = ctx.time_of(start);
  c.t_end = ctx.time_of(end);
  c.t_pay = ctx.time_of(pay);
  c.rate = K;
  finish_coupon(ctx, c, dc, K);
  return c;
}

// An RFR coupon over [start, end): the observation days on the index's fixing calendar, the
// realised prefix from the history, the projected days appended to leg.obs.
Coupon rfr_coupon(const BuildContext& ctx, Leg& leg, CouponKind kind, const IndexDef& index, const ObservationSpec& spec,
                  Date start, Date end, Date pay, double notional, double spread, DayCount dc) {
  const Calendar& fixing_cal = ctx.registry->calendar(index.fixing_calendar);
  const double basis = basis_of(index.day_count, index.name);
  const ObservationPeriod period = observation_days(start, end, spec, fixing_cal);
  const RealisedObservations ro = realised_observations(period, *ctx.fixings, index.name, basis);
  Coupon c;
  c.kind = kind;
  c.start = start;
  c.end = end;
  c.pay = pay;
  c.notional = notional;
  c.accrual = year_fraction(dc, start, end);
  c.t_start = ctx.time_of(start);
  c.t_end = ctx.time_of(end);
  c.t_pay = ctx.time_of(pay);
  c.spread = spread;
  c.realised_factor = ro.factor;
  c.realised_sum = ro.weighted_sum;
  c.fixed_days = ro.fixed_weight;
  c.obs_start = period.obs_start;
  c.obs_end_date = period.obs_end;
  c.obs_days = static_cast<double>(period.weight_total());
  c.obs_tau = c.obs_days / basis;
  c.obs_begin = static_cast<int>(leg.obs.size());
  for (const ObservationDay& od : ro.unfixed) {
    if (od.rate_date < ctx.valuation) {
      fail(index.name + ": no fixing for " + to_iso(od.rate_date) + " (period " + to_iso(start) + " to " + to_iso(end) + ")");
    }
    ObsDay d;
    d.rate_date = od.rate_date;
    const Date next = fixing_cal.next_business_day(od.rate_date + 1);
    d.t_rate = ctx.time_of(od.rate_date);
    d.t_next = ctx.time_of(next);
    d.tau_rate = static_cast<double>(next - od.rate_date) / basis;
    d.weight = static_cast<double>(od.weight_days) / basis;
    d.weight_days = static_cast<double>(od.weight_days);
    leg.obs.push_back(d);
  }
  c.obs_end = static_cast<int>(leg.obs.size());
  double rate_known = 0.0;
  if (ro.fixed_weight > 0) {
    rate_known = kind == CouponKind::RfrCompounded ? (ro.factor - 1.0) * basis / static_cast<double>(ro.fixed_weight)
                                                   : ro.weighted_sum / static_cast<double>(ro.fixed_weight);
  }
  finish_coupon(ctx, c, dc, rate_known);
  return c;
}

// A term-rate coupon over [start, end) fixing `fixing_lag` business days before `start` on the
// index's fixing calendar (or on `fixing_date` when given), realised when the history has it.
Coupon term_coupon(const BuildContext& ctx, const IndexDef& index, Date start, Date end, Date pay, double notional, double spread,
                   DayCount dc, Date fixing_date, DayCount forward_dc) {
  Coupon c;
  c.kind = CouponKind::TermRate;
  c.start = start;
  c.end = end;
  c.pay = pay;
  c.notional = notional;
  c.accrual = year_fraction(dc, start, end);
  c.t_start = ctx.time_of(start);
  c.t_end = ctx.time_of(end);
  c.t_pay = ctx.time_of(pay);
  c.spread = spread;
  c.fixing_date = fixing_date;
  if (auto r = ctx.fixings->get(index.name, fixing_date)) {
    c.realised = true;
    c.rate = *r;
  } else if (fixing_date < ctx.valuation) {
    fail(index.name + ": no fixing for " + to_iso(fixing_date) + " (period " + to_iso(start) + " to " + to_iso(end) + ")");
  }
  c.t_fix_start = c.t_start;
  c.t_fix_end = c.t_end;
  c.fix_tau = year_fraction(forward_dc, start, end);
  finish_coupon(ctx, c, dc, c.realised ? c.rate : 0.0);
  return c;
}

Instrument build_swap(const BuildContext& ctx, const Blueprint& bp, const InstrumentConvention& conv, const Trade& trade) {
  const Registry& reg = *ctx.registry;
  const Resolved r = resolve(bp, trade);
  if (conv.legs.size() != 2) fail("convention " + conv.name + " has " + std::to_string(conv.legs.size()) + " legs");
  if (!bp.legs.empty() && bp.legs.size() != conv.legs.size()) fail("blueprint " + bp.name + ": leg count differs from the convention's");
  Instrument in;
  switch (conv.type) {
    case InstrumentConvention::Type::OIS: in.kind = Kind::Ois; break;
    case InstrumentConvention::Type::IRS: in.kind = Kind::Irs; break;
    case InstrumentConvention::Type::Basis: in.kind = Kind::Basis; break;
    default: fail("build_swap on a non-swap convention");
  }
  in.blueprint = bp.name;
  in.convention = conv.name;
  in.currency = conv.currency;
  in.side = r.side;
  in.notional = r.notional;
  in.fixed_rate = r.fixed_rate;
  in.spread = r.spread;
  in.trade_date = trade.trade_date != 0 ? trade.trade_date : ctx.valuation;
  in.effective = effective_of(ctx, bp, conv, trade, in.trade_date);
  if (trade.termination != 0) {
    in.termination = trade.termination;
  } else {
    if (!r.has_tenor) fail("trade '" + trade.id + "' of blueprint " + bp.name + ": no tenor and no termination date");
    in.termination = add_period(in.effective, r.tenor, conv.eom);
  }
  if (in.termination <= in.effective) fail("trade '" + trade.id + "': termination is not after the effective date");
  in.id = !r.id.empty() ? r.id : bp.name + " " + (r.has_tenor ? r.tenor.to_string() : to_iso(in.termination));
  const int disc_slot = ctx.curves->slot(discount_index_of(reg, conv));
  Date last_pay = 0;
  for (std::size_t l = 0; l < conv.legs.size(); ++l) {
    const LegConvention& lc = conv.legs[l];
    const LegBlueprint* lb = bp.legs.empty() ? nullptr : &bp.legs[l];
    CouponKind kind = default_coupon_kind(reg, lc);
    if (lb != nullptr) {
      const bool rfr_swap = (lb->coupon == CouponKind::RfrCompounded || lb->coupon == CouponKind::RfrAveraged) &&
                            (kind == CouponKind::RfrCompounded || kind == CouponKind::RfrAveraged);
      if (lb->coupon != kind && !rfr_swap) {
        fail("blueprint " + bp.name + " leg " + std::to_string(l) + ": " + to_string(lb->coupon) + " on a " + to_string(kind) + " convention leg");
      }
      kind = lb->coupon;
    }
    Leg leg;
    leg.day_count = lc.day_count;
    leg.payment_lag = lc.payment_lag >= 0 ? lc.payment_lag : conv.payment_lag;
    leg.disc_curve = disc_slot;
    leg.spread_leg = lb != nullptr && lb->spread ? *lb->spread : lc.spread;
    const Calendar& roll_cal = reg.calendar(lc.calendar.empty() ? conv.calendar : lc.calendar);
    ScheduleSpec ss;
    ss.effective = in.effective;
    ss.termination = in.termination;
    ss.frequency = lc.frequency;
    ss.term = lc.term_frequency || (lc.term_up_to.n > 0 && in.termination <= add_period(in.effective, lc.term_up_to, conv.eom));
    ss.bdc = conv.bdc;
    ss.calendar = &roll_cal;
    ss.stub = conv.stub;
    ss.eom = conv.eom;
    const Schedule sched = generate_schedule(ss);
    const std::vector<Date> pays = payment_dates(sched, leg.payment_lag, roll_cal);
    leg.schedule = sched.adjusted;
    leg.payments = pays;
    const IndexDef* index = nullptr;
    ObservationSpec spec;
    if (kind != CouponKind::Fixed) {
      index = &reg.index(lc.index);
      leg.index = index->name;
      leg.curve = ctx.curves->slot(index->name);
      spec = lb != nullptr && lb->observation ? *lb->observation : lc.observation;
    }
    const double spread = leg.spread_leg ? r.spread : 0.0;
    for (std::size_t i = 0; i < sched.periods(); ++i) {
      const Date start = sched.adjusted[i], end = sched.adjusted[i + 1], pay = pays[i];
      if (pay <= ctx.valuation) continue;   // paid
      last_pay = std::max(last_pay, pay);
      switch (kind) {
        case CouponKind::Fixed:
          leg.coupons.push_back(fixed_coupon(ctx, start, end, pay, r.notional, r.fixed_rate, lc.day_count));
          break;
        case CouponKind::RfrCompounded:
        case CouponKind::RfrAveraged:
          leg.coupons.push_back(rfr_coupon(ctx, leg, kind, *index, spec, start, end, pay, r.notional, spread, lc.day_count));
          break;
        case CouponKind::TermRate: {
          const Calendar& fixing_cal = reg.calendar(index->fixing_calendar);
          const Date fixing = fixing_cal.add_business_days(start, -index->fixing_lag);
          leg.coupons.push_back(term_coupon(ctx, *index, start, end, pay, r.notional, spread, lc.day_count, fixing, index->day_count));
          break;
        }
      }
    }
    in.legs.push_back(std::move(leg));
  }
  if (last_pay == 0) fail("trade '" + in.id + "' has no unpaid cash flow (matured before " + to_iso(ctx.valuation) + ")");
  in.last_payment = last_pay;
  in.t_maturity = ctx.time_of(last_pay);
  return in;
}

Instrument build_deposit(const BuildContext& ctx, const Blueprint& bp, const InstrumentConvention& conv, const Trade& trade) {
  const Registry& reg = *ctx.registry;
  const Resolved r = resolve(bp, trade);
  Instrument in;
  in.kind = Kind::Deposit;
  in.blueprint = bp.name;
  in.convention = conv.name;
  in.currency = conv.currency;
  in.side = r.side;
  in.notional = r.notional;
  in.fixed_rate = r.fixed_rate;
  in.trade_date = trade.trade_date != 0 ? trade.trade_date : ctx.valuation;
  in.effective = effective_of(ctx, bp, conv, trade, in.trade_date);
  const Calendar& cal = reg.calendar(conv.calendar);
  if (trade.termination != 0) {
    in.termination = trade.termination;
  } else {
    if (!r.has_tenor) fail("deposit '" + trade.id + "' of blueprint " + bp.name + ": no tenor and no termination date");
    in.termination = cal.adjust(add_period(in.effective, r.tenor, conv.eom), conv.bdc);
  }
  if (in.termination <= in.effective) fail("deposit '" + trade.id + "': termination is not after the effective date");
  in.id = !r.id.empty() ? r.id : bp.name + " " + (r.has_tenor ? r.tenor.to_string() : to_iso(in.termination));
  const Date pay = cal.add_business_days(in.termination, conv.payment_lag);
  if (pay <= ctx.valuation) fail("deposit '" + in.id + "' matured before " + to_iso(ctx.valuation));
  const IndexDef& index = reg.index(conv.index);
  const int disc_slot = ctx.curves->slot(discount_index_of(reg, conv));
  Leg fixed;
  fixed.day_count = conv.day_count;
  fixed.payment_lag = conv.payment_lag;
  fixed.disc_curve = disc_slot;
  fixed.schedule = {in.effective, in.termination};
  fixed.payments = {pay};
  fixed.coupons.push_back(fixed_coupon(ctx, in.effective, in.termination, pay, r.notional, r.fixed_rate, conv.day_count));
  Leg flt = fixed;
  flt.coupons.clear();
  flt.index = index.name;
  flt.curve = ctx.curves->slot(index.name);
  const Calendar& fixing_cal = reg.calendar(index.fixing_calendar);
  const Date fixing = fixing_cal.add_business_days(in.effective, -index.fixing_lag);
  flt.coupons.push_back(term_coupon(ctx, index, in.effective, in.termination, pay, r.notional, 0.0, conv.day_count, fixing, conv.day_count));
  in.legs.push_back(std::move(fixed));
  in.legs.push_back(std::move(flt));
  in.last_payment = pay;
  in.t_maturity = ctx.time_of(pay);
  return in;
}

Instrument build_future(const BuildContext& ctx, const Blueprint& bp, const InstrumentConvention& conv, const Trade& trade) {
  const Registry& reg = *ctx.registry;
  const Resolved r = resolve(bp, trade);
  const IndexDef& index = reg.index(conv.index);
  const Calendar& fixing_cal = reg.calendar(index.fixing_calendar);
  const Date from = trade.trade_date != 0 ? trade.trade_date : ctx.valuation;
  const int want = std::max(trade.contract + 1, trade.contract_code.empty() ? 0 : 40);
  std::vector<FuturesPeriod> periods;
  if (conv.period == "imm_quarter") {
    periods = sofr_3m_futures(from, want, fixing_cal);
  } else if (conv.period == "calendar_month") {
    periods = sofr_1m_futures(from, want, fixing_cal);
  } else if (conv.period == "imm_quarter_deposit") {
    periods = euribor_3m_futures(from, want, reg.calendar(conv.calendar), conv.spot_lag);
  } else {
    fail("convention " + conv.name + ": unknown futures period \"" + conv.period + "\"");
  }
  const FuturesPeriod* fp = nullptr;
  if (!trade.contract_code.empty()) {
    // "SR3 Z26" in full, or the month code "Z26" alone.
    const std::string& want_code = trade.contract_code;
    for (const FuturesPeriod& p : periods) {
      const bool full = p.code == want_code;
      const bool suffix = p.code.size() > want_code.size() + 1 &&
                          p.code.compare(p.code.size() - want_code.size(), std::string::npos, want_code) == 0 &&
                          p.code[p.code.size() - want_code.size() - 1] == ' ';
      if (full || suffix) {
        fp = &p;
        break;
      }
    }
    if (fp == nullptr) fail("no " + conv.name + " contract \"" + trade.contract_code + "\" after " + to_iso(from));
  } else {
    if (trade.contract < 0 || static_cast<std::size_t>(trade.contract) >= periods.size()) {
      fail(conv.name + ": contract ordinal " + std::to_string(trade.contract) + " out of range");
    }
    fp = &periods[static_cast<std::size_t>(trade.contract)];
  }
  Instrument in;
  in.kind = Kind::Future;
  in.blueprint = bp.name;
  in.convention = conv.name;
  in.currency = conv.currency;
  in.side = r.side;
  in.notional = r.notional;
  in.traded_price = r.traded_price;
  in.trade_date = from;
  in.effective = fp->start;
  in.termination = fp->end;
  in.contract_code = fp->code;
  in.id = !r.id.empty() ? r.id : fp->code;
  Leg leg;
  leg.day_count = conv.day_count;
  leg.payment_lag = 0;
  leg.disc_curve = ctx.curves->slot(discount_index_of(reg, conv));
  leg.index = index.name;
  leg.curve = ctx.curves->slot(index.name);
  leg.schedule = {fp->start, fp->end};
  leg.payments = {fp->end};
  if (conv.accrual == "compounded") {
    leg.coupons.push_back(rfr_coupon(ctx, leg, CouponKind::RfrCompounded, index, ObservationSpec{}, fp->start, fp->end, fp->end, r.notional, 0.0, conv.day_count));
  } else if (conv.accrual == "averaged") {
    leg.coupons.push_back(rfr_coupon(ctx, leg, CouponKind::RfrAveraged, index, ObservationSpec{}, fp->start, fp->end, fp->end, r.notional, 0.0, conv.day_count));
  } else if (conv.accrual == "fixing") {
    leg.coupons.push_back(term_coupon(ctx, index, fp->start, fp->end, fp->end, r.notional, 0.0, conv.day_count, fp->fixing, conv.day_count));
  } else {
    fail("convention " + conv.name + ": unknown futures accrual \"" + conv.accrual + "\"");
  }
  in.legs.push_back(std::move(leg));
  in.last_payment = fp->end;
  in.t_maturity = ctx.time_of(fp->end);
  return in;
}

}  // namespace

int CurveSlots::add(const std::string& index) {
  if (slots_.count(index)) throw std::invalid_argument("CurveSlots: '" + index + "' already has a slot");
  const int s = static_cast<int>(names_.size());
  slots_.emplace(index, s);
  names_.push_back(index);
  return s;
}

int CurveSlots::slot(const std::string& index) const {
  auto it = slots_.find(index);
  if (it == slots_.end()) throw std::invalid_argument("CurveSlots: no curve slot for index '" + index + "'");
  return it->second;
}

Instrument build_instrument(const BuildContext& ctx, const Blueprint& bp, const Trade& trade) {
  if (ctx.registry == nullptr || ctx.fixings == nullptr || ctx.curves == nullptr) fail("BuildContext needs a registry, a fixings history and curve slots");
  const InstrumentConvention& conv = ctx.registry->instrument(bp.convention);
  switch (conv.type) {
    case InstrumentConvention::Type::OIS:
    case InstrumentConvention::Type::IRS:
    case InstrumentConvention::Type::Basis:
      return build_swap(ctx, bp, conv, trade);
    case InstrumentConvention::Type::Deposit:
      return build_deposit(ctx, bp, conv, trade);
    case InstrumentConvention::Type::Future:
      return build_future(ctx, bp, conv, trade);
  }
  fail("convention " + conv.name + ": unknown type");
}

Instrument build_instrument(const BuildContext& ctx, const Blueprints& blueprints, const Trade& trade) {
  return build_instrument(ctx, blueprints.blueprint(trade.blueprint), trade);
}

std::vector<std::string> CalibrationSet::keys() const {
  std::vector<std::string> v;
  for (const CalibrationInstrument& ci : instruments) v.push_back(ci.key);
  return v;
}

std::vector<Trade> calibration_trades(const CurveDefinition& def, const Blueprints& blueprints, const BuildContext& ctx) {
  std::vector<Trade> trades;
  for (const CurveInstrumentEntry& e : def.instruments) {
    const Blueprint& bp = blueprints.blueprint(e.blueprint);
    (void)ctx;
    if (e.contracts > 0) {
      for (int k = 0; k < e.contracts; ++k) {
        Trade t;
        t.blueprint = bp.name;
        t.contract = k;
        t.notional = 1.0;
        t.side = +1;
        trades.push_back(t);
      }
    } else {
      for (std::size_t i = 0; i < e.tenors.size(); ++i) {
        Trade t;
        t.blueprint = bp.name;
        t.tenor = Period::parse(e.tenors[i]);
        t.id = i < e.keys.size() ? e.keys[i] : bp.name + " " + e.tenors[i];
        t.notional = 1.0;
        t.side = +1;
        t.fixed_rate = 0.0;
        t.spread = 0.0;
        trades.push_back(t);
      }
    }
  }
  return trades;
}

CalibrationSet build_calibration_set(const BuildContext& ctx, const Blueprints& blueprints, const CurveDefinition& def) {
  CalibrationSet cs;
  cs.curve = def.name;
  cs.index = def.index;
  cs.slot = ctx.curves->slot(def.index);
  for (const Trade& t : calibration_trades(def, blueprints, ctx)) {
    CalibrationInstrument ci;
    ci.instrument = build_instrument(ctx, blueprints, t);
    ci.tenor = t.tenor ? t.tenor->to_string() : ci.instrument.contract_code;
    ci.key = !t.id.empty() ? t.id : ci.instrument.id;
    cs.instruments.push_back(std::move(ci));
  }
  std::stable_sort(cs.instruments.begin(), cs.instruments.end(),
                   [](const CalibrationInstrument& a, const CalibrationInstrument& b) { return a.instrument.t_maturity < b.instrument.t_maturity; });
  for (std::size_t i = 1; i < cs.instruments.size(); ++i) {
    if (!(cs.instruments[i].instrument.t_maturity > cs.instruments[i - 1].instrument.t_maturity)) {
      fail("curve " + def.name + ": instruments '" + cs.instruments[i - 1].key + "' and '" + cs.instruments[i].key + "' share the maturity " +
           to_iso(cs.instruments[i].instrument.last_payment));
    }
  }
  if (def.knot_tenors.empty()) {
    for (const CalibrationInstrument& ci : cs.instruments) cs.knot_t.push_back(ci.instrument.t_maturity);
  } else {
    for (const std::string& t : def.knot_tenors) cs.knot_t.push_back(ctx.time_of(add_period(ctx.valuation, Period::parse(t))));
    for (std::size_t i = 1; i < cs.knot_t.size(); ++i) {
      if (!(cs.knot_t[i] > cs.knot_t[i - 1])) fail("curve " + def.name + ": knot tenors are not increasing");
    }
  }
  // Region boundaries: the knot of the instrument quoted at that tenor when there is one, else
  // the tenor from the valuation date.
  for (std::size_t i = 0; i < def.regions.size(); ++i) {
    const CurveRegionDef& rd = def.regions[i];
    curve::RegionSpec rs;
    rs.scheme = rd.scheme;
    rs.variable = rd.variable;
    rs.t_a = 0.0;
    if (i > 0) {
      double t = ctx.time_of(add_period(ctx.valuation, Period::parse(rd.from)));
      for (const CalibrationInstrument& ci : cs.instruments) {
        if (ci.tenor == rd.from) {
          t = ci.instrument.t_maturity;
          break;
        }
      }
      rs.t_a = t;
      cs.regions.back().t_b = t;
    }
    cs.regions.push_back(rs);
  }
  return cs;
}

}  // namespace epykos::instrument
