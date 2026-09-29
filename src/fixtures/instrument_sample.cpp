// The instrument sample generator (test-only fixture, D28). SYNTHETIC: trades, fixings and the
// record-point curves come from the seed; conventions and blueprints are the repository's data.
#include "epykos/fixtures/instrument_sample.hpp"

#include <cmath>

#include "epykos/rng/philox.hpp"

namespace epykos::fixtures {

using namespace conventions;
using namespace instrument;

namespace {

const char* const sample_blueprints[] = {"USD-SOFR-OIS", "USD-SOFR-OIS-SHIFT2", "USD-SOFR-OIS-SHIFT2-LOCKOUT2", "USD-SOFR-OIS-LOOKBACK2",
                                         "USD-SOFR-AVG-SWAP", "USD-SOFR-ON-DEPOSIT", "USD-SOFR-3M-FUTURE", "USD-SOFR-1M-FUTURE",
                                         "EUR-ESTR-OIS", "EUR-ESTR-OIS-LAG1", "EUR-ESTR-ON-DEPOSIT", "EUR-EURIBOR-3M-DEPOSIT",
                                         "EUR-EURIBOR-6M-DEPOSIT", "EUR-EURIBOR-3M-IRS", "EUR-EURIBOR-6M-IRS", "EUR-3S6S-BASIS",
                                         "EUR-EURIBOR-3M-FUTURE"};
const char* const sample_tenors[] = {"1Y", "2Y", "3Y", "5Y", "7Y"};

}  // namespace

InstrumentSample make_instrument_sample(std::uint64_t seed, Date valuation) {
  InstrumentSample s;
  s.valuation = valuation;
  s.registry = Registry::load();
  s.blueprints = Blueprints::load();
  s.blueprints.validate(s.registry);
  s.slots.add("USD-SOFR");
  s.slots.add("EUR-ESTR");
  s.slots.add("EUR-EURIBOR-3M");
  s.slots.add("EUR-EURIBOR-6M");
  s.knot_t = {0.25, 0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0};
  const double base[sample_curves] = {0.038, 0.019, 0.021, 0.024};
  for (int c = 0; c < sample_curves; ++c) {
    for (int k = 0; k < sample_knots; ++k) s.z0.push_back(base[c] + 0.005 * static_cast<double>(k) / static_cast<double>(sample_knots - 1));
  }
  // fixings: one stream per index, levels near the curves' short ends
  const Date from = parse_iso("2025-01-01");
  const char* const indices[sample_curves] = {"USD-SOFR", "EUR-ESTR", "EUR-EURIBOR-3M", "EUR-EURIBOR-6M"};
  for (int c = 0; c < sample_curves; ++c) {
    SyntheticFixingsSpec spec;
    spec.seed = seed + static_cast<std::uint64_t>(c);
    spec.level = base[c];
    synthetic_fixings(s.fixings, indices[c], s.registry.calendar(s.registry.index(indices[c]).fixing_calendar), from, valuation - 1, spec);
  }
  const BuildContext ctx = s.context();
  auto df0 = [&](int slot, double t) { return sample_df<double>(s, s.z0.data(), slot, t); };
  int b = 0;
  for (const char* name : sample_blueprints) {
    s.blueprint_names.push_back(name);
    const Blueprint& bp = s.blueprints.blueprint(name);
    const InstrumentConvention& conv = s.registry.instrument(bp.convention);
    rng::Philox g(seed, sample_substream_base + static_cast<std::uint64_t>(b));
    for (int i = 0; i < sample_per_blueprint; ++i) {
      Trade t;
      t.id = std::string(name) + " #" + std::to_string(i);
      t.blueprint = name;
      const bool future = conv.type == InstrumentConvention::Type::Future;
      const bool deposit = conv.type == InstrumentConvention::Type::Deposit;
      const int tenor_pick = static_cast<int>(g.uniform_int(0, 4));
      const double notional = g.log_uniform(1.0e6, 1.0e8);
      const int side = g.uniform() < 0.5 ? +1 : -1;
      const int offset = static_cast<int>(g.uniform_int(30, 300));   // < 1Y: a seasoned 1Y trade still has a cash flow
      const double eps = g.uniform_range(-0.05, 0.05);
      const double price_eps = g.uniform_range(-0.1, 0.1);
      t.notional = notional;
      t.side = side;
      if (future) {
        t.contract = i;
      } else if (deposit) {
        t.tenor = bp.tenor;
      } else {
        t.tenor = Period::parse(sample_tenors[tenor_pick]);
        if (i < sample_seasoned_per_blueprint) t.effective = valuation - offset;
      }
      // the par quote at the record point, then the trade's rate off it
      Trade probe = t;
      probe.fixed_rate = 0.0;
      probe.spread = 0.0;
      probe.traded_price = 0.0;
      const Instrument p = build_instrument(ctx, bp, probe);
      const double par0 = instrument::par<double>(p, df0);
      if (future) {
        t.traded_price = par0 + price_eps;
      } else if (p.kind == Kind::Basis) {
        t.spread = par0 * (1.0 + eps);
      } else {
        t.fixed_rate = par0 * (1.0 + eps);
      }
      s.trades.push_back(t);
      s.instruments.push_back(build_instrument(ctx, bp, t));
    }
    ++b;
  }
  return s;
}

std::vector<double> sample_scales(const InstrumentSample& s, const double* z) {
  auto df = [&](int slot, double t) { return sample_df<double>(s, z, slot, t); };
  std::vector<double> scale(static_cast<std::size_t>(s.n_outputs()), 0.0);
  double total = 0.0;
  for (int i = 0; i < s.n_trades(); ++i) {
    const Instrument& in = s.instruments[static_cast<std::size_t>(i)];
    double sc;
    if (in.kind == Kind::Future) {
      sc = std::fabs(in.notional);
    } else {
      sc = std::fabs(leg_pv<double>(in.legs[0], df)) + std::fabs(leg_pv<double>(in.legs[1], df));
    }
    scale[static_cast<std::size_t>(i)] = sc;
    total += std::fabs(instrument::pv<double>(in, df));
  }
  scale[static_cast<std::size_t>(s.n_trades())] = total;
  return scale;
}

std::vector<std::vector<double>> sample_states(const InstrumentSample& s, int R, double rho) {
  std::vector<std::vector<double>> states;
  for (int r = 0; r < R; ++r) {
    rng::Philox g(sample_seed, sample_substream_base + 1000 + static_cast<std::uint64_t>(r));
    std::vector<double> z = s.z0;
    for (double& v : z) v += rho * g.uniform_range(-1.0, 1.0);
    states.push_back(z);
  }
  return states;
}

}  // namespace epykos::fixtures
