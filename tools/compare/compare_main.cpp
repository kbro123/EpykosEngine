// tools/compare/ — the MX head-to-head driver (ROADMAP.md §MX, D21, D71).
//
//   compare_ois --exchange <path> --results <path> [--trades N] [--noise-bp X] [--seed S]
//               [--valuation YYYY-MM-DD] [--samples N] [--obs-days]
//
// Builds the `fixtures::compare_ois` problem, records it as ONE tape, calibrates it through the
// implicit node, prices the book and takes the O3 ladder through the IFT; writes
//
//   --exchange   the problem in the neutral, purely time-based exchange form, which a second
//                engine is fed (bench/compare/README.md defines every field); --obs-days also
//                writes every projected observation day, so the reader can evaluate the coupon
//                the way THIS engine does instead of its collapsed one-sub-period form;
//   --results    THIS engine's answers to it: the calibrated knots, discount factors at the
//                sample times, the model par rate of every calibration instrument, the PV of
//                every trade, the book total, and the book's ladder d(PV)/d(quote).
//
// It knows nothing about any other engine: it reads no other checkout, links nothing of one and
// names none of its types (D11). `scripts/compare_swapengine.py` is the piece that feeds the
// exchange file to the other engine's own public JSON interface and diffs the two answer sets.
//
// Like everything under tools/, this is a measurement tool, not a gate and not a ctest target.
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/maths/instrument/calibrate.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/version.hpp"

using namespace epykos;

namespace {

std::string num(double v) {
  std::ostringstream s;
  s << std::setprecision(17) << v;
  return s.str();
}

template <class T>
void array(std::ostringstream& o, const std::vector<T>& v) {
  o << "[";
  for (std::size_t i = 0; i < v.size(); ++i) o << (i ? ", " : "") << num(static_cast<double>(v[i]));
  o << "]";
}

[[noreturn]] void usage(const char* why) {
  std::cerr << "compare_ois: " << why << "\n"
            << "usage: compare_ois --exchange <path> --results <path> [--trades N] [--noise-bp X]\n"
            << "                   [--seed S] [--valuation YYYY-MM-DD] [--samples N] [--obs-days]\n";
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::string exchange_path, results_path;
  fixtures::CompareOisOptions opt;
  int n_samples = 200;
  bool obs_days = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) usage(("missing value for " + a).c_str());
      return argv[++i];
    };
    if (a == "--exchange") exchange_path = next();
    else if (a == "--results") results_path = next();
    else if (a == "--trades") opt.trades = std::stoi(next());
    else if (a == "--noise-bp") opt.quote_noise_bp = std::stod(next());
    else if (a == "--seed") opt.seed = std::stoull(next());
    else if (a == "--valuation") opt.valuation = next();
    else if (a == "--samples") n_samples = std::stoi(next());
    else if (a == "--obs-days") obs_days = true;
    else usage(("unknown argument " + a).c_str());
  }
  if (exchange_path.empty() || results_path.empty()) usage("--exchange and --results are both required");

  const fixtures::CompareOis s = fixtures::make_compare_ois(opt);
  std::cerr << "[compare] " << s.n_quotes() << " calibration instruments on " << s.set.knot_t.size()
            << " knots, " << s.n_trades() << " book trades, valuation " << opt.valuation << "\n";

  {
    std::ofstream f(exchange_path);
    if (!f) usage(("cannot write " + exchange_path).c_str());
    f << fixtures::compare_ois_exchange_json(s, obs_days);
  }

  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  std::cerr << "[compare] tape " << t.nodes_raw << " raw nodes -> " << t.nodes_after_passes
            << " after the E0 passes; record-point solve converged=" << (t.record_report.converged ? 1 : 0)
            << " |Jtr|inf=" << t.record_report.jtr_inf << "\n";

  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::compare_ois_program_options(s));

  // O3: the book's ladder, and every trade's ladder row.
  std::vector<int> ordinals;
  ordinals.push_back(t.book_output);
  for (int o : t.pv_outputs) ordinals.push_back(o);
  std::vector<double> out;
  const std::vector<double> rows = fixtures::compare_ois_ladder(prog, s.quotes, ordinals, &out);
  const std::size_t n_q = static_cast<std::size_t>(s.n_quotes());

  // The calibrated knots and the model par rates at the solved state, read back from the run.
  std::vector<double> knots;
  for (int o : t.knot_outputs) knots.push_back(out[static_cast<std::size_t>(o)]);
  std::vector<double> pv;
  for (int o : t.pv_outputs) pv.push_back(out[static_cast<std::size_t>(o)]);
  const double book = out[static_cast<std::size_t>(t.book_output)];

  const curve::Composite comp = s.set.composite();
  auto df_solved = [&](int, double tt) { return comp.df(knots.data(), tt); };
  const std::vector<double> model = instrument::par_quotes(s.set, df_solved);

  std::vector<double> sample_t, sample_df;
  const double t_max = s.set.knot_t.back();
  for (int i = 0; i < n_samples; ++i) {
    const double tt = t_max * static_cast<double>(i + 1) / static_cast<double>(n_samples);
    sample_t.push_back(tt);
    sample_df.push_back(comp.df(knots.data(), tt));
  }

  std::ostringstream o;
  o << std::setprecision(17);
  o << "{\n";
  o << "  \"format\": \"epykos-compare-results 1\",\n";
  o << "  \"engine\": \"EpykosEngine\",\n";
  o << "  \"build_flags\": \"" << epykos::build_flags() << "\",\n";
  o << "  \"curve_variable\": \"logdf\",\n";
  o << "  \"n_quotes\": " << s.n_quotes() << ", \"n_knots\": " << s.set.knot_t.size()
    << ", \"n_trades\": " << s.n_trades() << ",\n";
  o << "  \"solve\": {\"converged\": " << (t.record_report.converged ? "true" : "false")
    << ", \"jtr_inf\": " << num(t.record_report.jtr_inf) << "},\n";
  o << "  \"knots_t\": "; array(o, s.set.knot_t); o << ",\n";
  o << "  \"knots_logdf\": "; array(o, knots); o << ",\n";
  o << "  \"model_par_rate\": "; array(o, model); o << ",\n";
  o << "  \"market\": "; array(o, s.quotes); o << ",\n";
  o << "  \"sample_t\": "; array(o, sample_t); o << ",\n";
  o << "  \"sample_df\": "; array(o, sample_df); o << ",\n";
  o << "  \"book_npv\": " << num(book) << ",\n";
  o << "  \"trade_npv\": "; array(o, pv); o << ",\n";
  o << "  \"book_ladder\": [";
  for (std::size_t k = 0; k < n_q; ++k) o << (k ? ", " : "") << num(rows[k]);
  o << "],\n";
  o << "  \"trade_ladder\": [\n";
  for (std::size_t r = 1; r < ordinals.size(); ++r) {
    o << "    [";
    for (std::size_t k = 0; k < n_q; ++k) o << (k ? ", " : "") << num(rows[r * n_q + k]);
    o << "]" << (r + 1 < ordinals.size() ? "," : "") << "\n";
  }
  o << "  ]\n";
  o << "}\n";

  std::ofstream f(results_path);
  if (!f) usage(("cannot write " + results_path).c_str());
  f << o.str();
  std::cerr << "[compare] wrote " << exchange_path << " and " << results_path << "\n";
  return 0;
}
