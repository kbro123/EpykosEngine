#include "epykos/solver/curve_set.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/tape/slice.hpp"

namespace epykos::solver {

namespace {
std::size_t idx(int i) noexcept { return static_cast<std::size_t>(i); }
}  // namespace

int CurveSet::add_curve(CurveSpec spec) {
  if (spec.name.empty()) throw std::invalid_argument("CurveSet: a curve needs a name");
  if (find(spec.name) >= 0) throw std::invalid_argument("CurveSet: duplicate curve name '" + spec.name + "'");
  if (spec.knot_t.empty()) throw std::invalid_argument("CurveSet: curve '" + spec.name + "' has no knots");
  for (std::size_t k = 1; k < spec.knot_t.size(); ++k) {
    if (!(spec.knot_t[k] > spec.knot_t[k - 1])) throw std::invalid_argument("CurveSet: curve '" + spec.name + "' knots not ascending");
  }
  if (spec.start.size() != spec.knot_t.size()) {
    throw std::invalid_argument("CurveSet: curve '" + spec.name + "' start has " + std::to_string(spec.start.size()) + " values for " +
                                std::to_string(spec.knot_t.size()) + " knots");
  }
  if (spec.regions.empty()) {
    if (spec.scheme != "linear") {
      throw std::invalid_argument("CurveSet: scheme '" + spec.scheme + "' needs regions (a curve with no regions is the linear zero-rate one)");
    }
  } else {
    // A Composite over the same knots; std::invalid_argument on a bad definition (composite.hpp).
    spec.composite = curve::Composite(spec.knot_t, spec.regions);
    spec.scheme = "composite";
    for (std::size_t r = 0; r < spec.regions.size(); ++r) {
      spec.scheme += std::string(r == 0 ? ":" : ",") + curve::to_string(spec.regions[r].scheme) + "/" + curve::to_string(spec.regions[r].variable);
    }
  }
  curves_.push_back(std::move(spec));
  return static_cast<int>(curves_.size()) - 1;
}

int CurveSet::n_knots_total() const noexcept {
  int n = 0;
  for (const CurveSpec& c : curves_) n += static_cast<int>(c.knot_t.size());
  return n;
}

int CurveSet::find(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < curves_.size(); ++i) {
    if (curves_[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

std::vector<int> CurveSet::instruments_of(int curve) const {
  check_curve(curve);
  std::vector<int> v;
  for (std::size_t i = 0; i < instruments_.size(); ++i) {
    if (instruments_[i].curve == curve) v.push_back(static_cast<int>(i));
  }
  return v;
}

std::vector<std::vector<int>> CurveSet::instrument_reads() const {
  std::vector<std::vector<int>> reads(instruments_.size());
  for (std::size_t i = 0; i < instruments_.size(); ++i) {
    Tape scratch;
    std::vector<int> curve_of_ordinal;
    node_id root = invalid_node;
    {
      Tape::Scope scope(scratch);
      CurveStates<Rec> s;
      s.specs = &curves_;
      s.resize(curves_.size());
      for (std::size_t c = 0; c < curves_.size(); ++c) {
        std::vector<Rec> zc;
        for (double v : curves_[c].start) {
          curve_of_ordinal.push_back(static_cast<int>(c));
          zc.push_back(make_input(scratch, v));
        }
        s.set(static_cast<int>(c), zc);
      }
      curve_of_ordinal.push_back(-1);
      const Rec q = make_input(scratch, 0.0);
      const Rec F = instruments_[i].record(s, q);
      root = F.node_on(scratch);
    }
    const Slice sl = slice(scratch, std::vector<node_id>{root});
    for (int o : sl.input_ordinals) {
      const int c = curve_of_ordinal[idx(o)];
      if (c >= 0 && std::find(reads[i].begin(), reads[i].end(), c) == reads[i].end()) reads[i].push_back(c);
    }
    std::sort(reads[i].begin(), reads[i].end());
    if (reads[i].empty()) {
      throw std::runtime_error("CurveSet: instrument '" + instruments_[i].label + "' reads no curve");
    }
  }
  return reads;
}

std::vector<std::vector<int>> CurveSet::dependencies() const {
  const std::vector<std::vector<int>> reads = instrument_reads();
  std::vector<std::vector<int>> deps(curves_.size());
  for (std::size_t i = 0; i < instruments_.size(); ++i) {
    const int c = instruments_[i].curve;
    for (int d : reads[i]) {
      if (d != c && std::find(deps[idx(c)].begin(), deps[idx(c)].end(), d) == deps[idx(c)].end()) deps[idx(c)].push_back(d);
    }
  }
  for (auto& d : deps) std::sort(d.begin(), d.end());
  return deps;
}

std::vector<std::vector<int>> CurveSet::blocks(Mode mode) const {
  const int n = n_curves();
  if (mode == Mode::joint) {
    std::vector<int> all;
    for (int c = 0; c < n; ++c) all.push_back(c);
    return {all};
  }
  // Tarjan's strongly connected components over the edges c -> d (c depends on d). An SCC is
  // emitted after every SCC reachable from it, i.e. after its dependencies: solve order.
  const std::vector<std::vector<int>> deps = dependencies();
  std::vector<int> index(idx(n), -1), low(idx(n), 0);
  std::vector<std::uint8_t> on_stack(idx(n), 0);
  std::vector<int> stack;
  std::vector<std::vector<int>> out;
  int counter = 0;
  std::function<void(int)> visit = [&](int v) {
    index[idx(v)] = low[idx(v)] = counter++;
    stack.push_back(v);
    on_stack[idx(v)] = 1;
    for (int w : deps[idx(v)]) {
      if (index[idx(w)] < 0) {
        visit(w);
        low[idx(v)] = std::min(low[idx(v)], low[idx(w)]);
      } else if (on_stack[idx(w)]) {
        low[idx(v)] = std::min(low[idx(v)], index[idx(w)]);
      }
    }
    if (low[idx(v)] == index[idx(v)]) {
      std::vector<int> comp;
      int w;
      do {
        w = stack.back();
        stack.pop_back();
        on_stack[idx(w)] = 0;
        comp.push_back(w);
      } while (w != v);
      std::sort(comp.begin(), comp.end());
      out.push_back(comp);
    }
  };
  for (int c = 0; c < n; ++c) {
    if (index[idx(c)] < 0) visit(c);
  }
  return out;
}

CurveSet::Calibration CurveSet::calibrate(Tape& tape, ImplicitRegistry& registry, std::span<const Rec> quotes, Mode mode,
                                          const SolveOptions& options) const {
  if (!tape.recording()) throw RecordError("CurveSet::calibrate: the tape is not the current recording tape");
  if (static_cast<int>(quotes.size()) != n_instruments()) {
    throw RecordError("CurveSet::calibrate: " + std::to_string(quotes.size()) + " quotes for " + std::to_string(n_instruments()) +
                      " instruments");
  }
  Calibration cal;
  cal.states.specs = &curves_;
  cal.states.resize(curves_.size());
  for (const std::vector<int>& block : blocks(mode)) {
    std::vector<double> z0;
    std::string name;
    for (int c : block) {
      const CurveSpec& s = curves_[idx(c)];
      z0.insert(z0.end(), s.start.begin(), s.start.end());
      name += (name.empty() ? "" : "+") + s.name;
    }
    std::vector<int> insts;
    for (std::size_t i = 0; i < instruments_.size(); ++i) {
      if (std::find(block.begin(), block.end(), instruments_[i].curve) != block.end()) insts.push_back(static_cast<int>(i));
    }
    if (insts.size() < z0.size()) {
      throw std::invalid_argument("CurveSet::calibrate: block '" + name + "' has " + std::to_string(insts.size()) + " instruments for " +
                                  std::to_string(z0.size()) + " knots");
    }
    ImplicitResult r = implicit(
        tape, registry, name, z0, static_cast<int>(insts.size()),
        [&](const Rec* z, Rec* F) {
          CurveStates<Rec> s = cal.states;  // earlier blocks' curves have values; later ones none
          std::size_t off = 0;
          for (int c : block) {
            const std::size_t nk = curves_[idx(c)].knot_t.size();
            s.set(c, z + off, nk);
            off += nk;
          }
          for (std::size_t m = 0; m < insts.size(); ++m) {
            const int i = insts[m];
            F[m] = instruments_[idx(i)].record(s, quotes[idx(i)]);
          }
        },
        options);
    std::size_t off = 0;
    for (int c : block) {
      const std::size_t nk = curves_[idx(c)].knot_t.size();
      cal.states.set(c, r.z.data() + off, nk);
      off += nk;
    }
    cal.results.push_back(std::move(r));
    cal.block_curves.push_back(block);
    cal.block_instruments.push_back(insts);
  }
  return cal;
}

std::vector<double> CurveSet::residuals(const CurveStates<double>& states, std::span<const double> quotes) const {
  if (static_cast<int>(quotes.size()) != n_instruments()) throw std::invalid_argument("CurveSet::residuals: wrong quote count");
  std::vector<double> F(instruments_.size());
  for (std::size_t i = 0; i < instruments_.size(); ++i) F[i] = instruments_[i].value(states, quotes[i]);
  return F;
}

CurveStates<double> CurveSet::states_at(std::span<const double> z_all) const {
  if (static_cast<int>(z_all.size()) != n_knots_total()) throw std::invalid_argument("CurveSet::states_at: wrong length");
  CurveStates<double> s;
  s.specs = &curves_;
  s.resize(curves_.size());
  std::size_t off = 0;
  for (std::size_t c = 0; c < curves_.size(); ++c) {
    const std::size_t nk = curves_[c].knot_t.size();
    s.set(static_cast<int>(c), z_all.data() + off, nk);
    off += nk;
  }
  return s;
}

std::vector<double> CurveSet::start_all() const {
  std::vector<double> z;
  for (const CurveSpec& c : curves_) z.insert(z.end(), c.start.begin(), c.start.end());
  return z;
}

const char* to_string(CurveSet::Mode mode) noexcept {
  switch (mode) {
    case CurveSet::Mode::sequential: return "sequential";
    case CurveSet::Mode::joint: return "joint";
  }
  return "?";
}

}  // namespace epykos::solver
