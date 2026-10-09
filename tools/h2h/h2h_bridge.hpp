// tools/h2h/ — THE EXCHANGE: this repository's row tables expressed in the second engine's
// problem schema (D78's relaxation of D11/D21 for `tools/h2h/` alone; widened by the owner on
// 2026-10-09 to permit READING that checkout, still not copying from it).
//
// What this file is, and what it deliberately is not
// --------------------------------------------------
// `bench/compare/README.md` §3 defines a NEUTRAL exchange form — year fractions and nothing else —
// which `src/fixtures/compare_ois_e0.cpp` writes and `scripts/compare_swapengine.py` then converts
// into the other engine's bundle/book shapes in Python. That round trip works for ONE curve and
// one product, and cannot carry Stage A: the neutral form has no vocabulary for four curves, two
// currencies, a tenor-basis quote, an averaged coupon, a lockout or a realised fixing.
//
// So the translation moves HERE, and three things change for the better:
//
//   1. It is TYPED. The target is `cal::BundleProblem` / `pf::MultiCurveBook`, built as C++
//      objects, so the compiler checks every field. The old route guessed a JSON schema from the
//      CLI's error messages (README §0) and D90 §4 records a whole run lost to handing the raw
//      exchange form to a codec that wanted the converted one.
//   2. It is in the ONE directory licensed to see that engine. Nothing under `include/epykos/` or
//      `src/` learns its schema; the clean-room property that makes this benchmark worth running
//      is untouched. `bundle_json` below serialises through THEIR OWN `bundle_to_json`, so the
//      exchange file is their schema BY CONSTRUCTION rather than by this file's belief about it.
//   3. It is EXACT, per coupon, and says so. Every mapping below is an algebraic identity between
//      `maths/instrument/coupon.hpp` and `pricing/cashflows.hpp`, chosen per coupon from the row
//      tables; `Mapping` carries the audit (how many coupons took which form) and `bridge` THROWS
//      rather than approximate. A coupon this schema cannot express is a reported finding, never a
//      silently different price.
//
// The identities, once, because they are the whole content of this file
// --------------------------------------------------------------------
// Ours (coupon.hpp): a float coupon is  N·τ·(R + s)·DF_d(t_pay)  with
//
//   RfrCompounded   R = (rf · Π_i (1 + f_i·w_i) − 1) / τ_obs,  f_i = (DF_p(t_r)/DF_p(t_r') − 1)/τ_r
//   RfrAveraged     R = (Σ_i r_i n_i (realised) + Σ_i f_i·nd_i) / obs_days
//   TermRate        R = the fixing when realised, else (DF_p(t_s)/DF_p(t_e) − 1)/τ_fix
//
// Theirs (cashflows.hpp): a FloatCoupon is  DF_dc(pay)·(A + s·τ_index)·(τ_pay/τ_index)·scale  with
//
//   arithmetic      A = Σ_k w_k·(DF(s_k)/DF(e_k) − 1) + realized
//   compounded      A = realized_factor · Π_k (1 + w_k·(DF(s_k)/DF(e_k) − 1)) − 1
//
// so, term by term:
//
//   our day i  ->  their sub-period k = (s_k, e_k) = (t_rate, t_next)
//   compounded     w_k = weight/tau_rate, τ_index = obs_tau, realized_factor = realised_factor
//   averaged       w_k = weight_days/tau_rate, τ_index = obs_days, realized = realised_sum
//   term, live     ONE sub-period (t_fix_start, t_fix_end), τ_index = fix_tau
//   term, fixed    NO sub-period, realized = rate·τ_index
//   every kind     pay = t_pay, τ_pay = accrual, spread = c.spread, scale = N_c/N_position
//
// and one collapse, which is the whole point of D81 being in this comparison at all: when every
// day of a compounded coupon has weight == tau_rate and the days are contiguous, the product
// telescopes to DF(t_rate of the first)/DF(t_next of the last), and that single ratio is handed
// over instead of the day list. `epykos::compile` DERIVES exactly that collapse on our side
// (D81), so handing them the collapsed form is what makes the timing a comparison of two engines
// rather than of two coupon representations (README §4 item 1). Plain and observation-shifted
// coupons telescope; a LOCKOUT does not, and is handed over as the day product it is — which is
// also what our tape is left holding, so the asymmetry does not reappear.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "swaps/calibration/bundle_problem.hpp"
#include "swaps/portfolio/portfolio.hpp"

#include "epykos/maths/instrument/instrument.hpp"

namespace h2h {

namespace cal = swaps::calibration;
namespace pf = swaps::portfolio;

// One curve of the problem, as both engines need to see it.
struct CurveView {
  std::string definition;   // our curve definition's name
  std::string index;        // the floating index it projects
  std::string currency;     // ISO code
  std::vector<double> knot_t;
};

// The problem, as the bridge reads it: this is what BOTH engines' objects are built from, so
// neither side can be handed a different problem by accident.
struct ProblemView {
  std::string label;
  double valuation_t = 0.0;                              // always 0; stated for symmetry
  std::vector<CurveView> curves;                         // slot order == their curve index order
  // The calibration instruments in QUOTE order (our `quotes` order is their residual order, which
  // is their instruments' insertion order -- so the two ladders index off the same thing).
  std::vector<const epykos::instrument::Instrument*> cal;
  std::vector<int> cal_slot;                             // the slot each one pins
  std::vector<double> quotes;                            // our quote units (a futures quote is a PRICE)
  std::vector<std::uint8_t> quote_is_price;              // 1 = a futures price, so their market is 1 - q/100
  // The book.
  std::vector<const epykos::instrument::Instrument*> book;
  std::vector<std::string> family;                       // per book trade: its blueprint
};

// What the translation had to do, per coupon, so the report can state it instead of asserting it.
struct Mapping {
  long long cal_instruments = 0, book_trades = 0, their_positions = 0;
  long long coupons_fixed = 0;
  long long coupons_telescoped = 0;      // compounded, collapsed to ONE arithmetic sub-period
  long long coupons_compounded = 0;      // compounded, day by day (lockout / seasoned)
  long long coupons_averaged = 0;        // arithmetic, one sub-period per day
  long long coupons_term_live = 0, coupons_term_fixed = 0;
  long long obs_days_handed_over = 0;    // sub-periods actually written
  long long obs_days_collapsed = 0;      // days the telescope removed
  long long seasoned_coupons = 0;
  long long basis_split = 0;             // float-vs-float trades written as TWO of their positions
  std::string to_string() const;
};

// The other engine's calibration problem and book, from ours. Throws std::runtime_error, naming
// the instrument and coupon, for anything this schema cannot carry exactly.
cal::BundleProblem bundle_of(const ProblemView& v, Mapping* m = nullptr);
pf::MultiCurveBook book_of(const ProblemView& v, Mapping* m = nullptr);
// The same book restricted to the trades whose family is `family` (all of them when empty).
pf::MultiCurveBook book_of_family(const ProblemView& v, const std::string& family, Mapping* m = nullptr);
// One trade's book on its own, for localising a per-family disagreement to a trade.
pf::MultiCurveBook book_of_trade(const ProblemView& v, std::size_t i);

// d(their market)/d(our quote) per quote: 1 for a rate quote, -1/100 for a futures PRICE (their
// row is quoted in RATE units where ours is a price, their market being 1 - price/100). So our
// ladder column k DIVIDED by this is their column k, and a market move of `b` in rate units is a
// move of `b / this` in our quote units. It is the ONE unit conversion in this comparison, it is
// applied to OUR side, and it is exact (both engines' futures residual is in rate units; neither
// carries a convexity adjustment -- ours is zero by D35, theirs is an input number set to zero).
std::vector<double> market_jacobian(const ProblemView& v);

// The latest curve time each slot is read at, over every calibration instrument and book trade
// (payment dates, accrual ends and projected observation days). README §5: inside [0, last knot]
// the two curves are the same function of the knot values and beyond it they are NOT -- we hold
// the variable flat, they continue the last forward -- so the comparison is sound only while
// nothing is priced past the last knot. This is what lets the harness assert that rather than
// assume it.
std::vector<double> max_time_read(const ProblemView& v);

// The exchange file: their bundle through THEIR OWN serialiser, plus the book in the schema
// `book_from_json` documents, as one document {"bundle": ..., "book": ...}.
std::string exchange_json(const ProblemView& v);

}  // namespace h2h
