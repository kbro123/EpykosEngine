#include "epykos/rewrite/verifier.hpp"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <vector>

#include "epykos/rewrite/greedy.hpp"

namespace epykos::rewrite {

namespace {

std::vector<int> sample_indices(int n, int max_count) {
  std::vector<int> idx;
  if (n <= 0) return idx;
  if (n <= max_count) {
    idx.resize(static_cast<std::size_t>(n));
    std::iota(idx.begin(), idx.end(), 0);
    return idx;
  }
  idx.reserve(static_cast<std::size_t>(max_count));
  for (int i = 0; i < max_count; ++i) {
    idx.push_back(static_cast<int>((static_cast<long long>(i) * n) / max_count));
  }
  return idx;
}

AdjointCheckResult run_adjoint_check(const adjoint::Adjoint& before, const adjoint::Adjoint& after,
                                     const double* record_point, const verify::StateBall& ball, int n_inputs,
                                     int n_outputs, const VerifyOptions& options) {
  AdjointCheckResult res;
  res.ran = true;
  const std::vector<int> outs = sample_indices(n_outputs, options.max_outputs_checked);
  const int draws = std::min(options.adjoint_ball_draws, ball.n_draws);

  std::vector<double> out_bar(static_cast<std::size_t>(n_outputs), 0.0);
  std::vector<double> out_b(static_cast<std::size_t>(n_outputs)), out_a(static_cast<std::size_t>(n_outputs));
  std::vector<double> state_bar_b(static_cast<std::size_t>(n_inputs)), state_bar_a(static_cast<std::size_t>(n_inputs));

  auto check_state = [&](const double* z, int draw_index) {
    for (int o : outs) {
      std::fill(out_bar.begin(), out_bar.end(), 0.0);
      out_bar[static_cast<std::size_t>(o)] = 1.0;
      before.run(z, 1, out_bar.data(), out_b.data(), state_bar_b.data());
      after.run(z, 1, out_bar.data(), out_a.data(), state_bar_a.data());
      for (int i = 0; i < n_outputs; ++i) {
        const std::size_t si = static_cast<std::size_t>(i);
        const double d = verify::ulp_distance(out_b[si], out_a[si]);
        if (!verify::within(out_b[si], out_a[si], options.adjoint_tolerance)) res.passed = false;
        if (d > res.max_out_ulps) {
          res.max_out_ulps = d;
          res.worst_output = o;
          res.worst_draw = draw_index;
        }
      }
      for (int k = 0; k < n_inputs; ++k) {
        const std::size_t sk = static_cast<std::size_t>(k);
        const double d = verify::ulp_distance(state_bar_b[sk], state_bar_a[sk]);
        if (!verify::within(state_bar_b[sk], state_bar_a[sk], options.adjoint_tolerance)) res.passed = false;
        if (d > res.max_state_bar_ulps) {
          res.max_state_bar_ulps = d;
          res.worst_output = o;
          res.worst_draw = draw_index;
        }
      }
    }
  };

  check_state(record_point, -1);
  for (int r = 0; r < draws; ++r) check_state(ball.state(r), r);
  return res;
}

}  // namespace

std::string AdjointCheckResult::summary() const {
  std::ostringstream os;
  if (!ran) {
    os << "adjoint: not checked";
    return os.str();
  }
  os << "adjoint: " << (passed ? "PASS" : "FAIL") << " (worst " << max_out_ulps << " ulps out, " << max_state_bar_ulps
     << " ulps state_bar, output " << worst_output << " draw " << worst_draw << ")";
  return os.str();
}

std::string VerifyReport::summary() const {
  std::ostringstream os;
  os << interpreter.summary() << "; " << adjoint.summary();
  return os.str();
}

VerifyReport compare_programs(const ir::Program& before, const ir::Program& after, const double* state, int n_inputs,
                              int n_outputs, const VerifyOptions& options) {
  VerifyReport report;

  exec::Interpreter before_interp(before, options.interpreter);
  exec::Interpreter after_interp(after, options.interpreter);

  const verify::StateBall ball = verify::make_state_ball(state, n_inputs, options.ball);
  const auto reference = [&](const double* z, double* out) { before_interp.run(z, 1, out); };
  verify::DifferentialOptions dopt;
  dopt.tolerance = options.interpreter_tolerance;
  dopt.batch = std::max(1, std::min(ball.n_draws, after_interp.max_batch()));
  report.interpreter = verify::differential(ball, n_outputs, reference, verify::interpreter_fn(after_interp), dopt);

  if (options.check_adjoint) {
    adjoint::Adjoint before_adj(before, options.adjoint);
    adjoint::Adjoint after_adj(after, options.adjoint);
    report.adjoint = run_adjoint_check(before_adj, after_adj, state, ball, n_inputs, n_outputs, options);
  }
  return report;
}

VerifyReport verify_annotations(const ir::Program& program, const ir::PlanAnnotations& plan, const double* state,
                                int n_inputs, int n_outputs, const VerifyOptions& options) {
  ir::Program bare = program;
  bare.plan = ir::PlanAnnotations{};
  ir::Program annotated = program;
  annotated.plan = plan;
  return compare_programs(bare, annotated, state, n_inputs, n_outputs, options);
}

RuleVerifyReport verify_rule(const Rule& rule, const ir::Program& program, const double* state, int n_inputs,
                            int n_outputs, const ir::PlanAnnotations& base_plan, const VerifyOptions& options) {
  RuleVerifyReport result;
  const std::vector<MatchSite> sites = rule.match(program, base_plan);
  result.sites_checked = static_cast<int>(sites.size());
  for (const MatchSite& site : sites) {
    Proposal p = rule.propose(program, base_plan, site);
    if (p.is_structural()) {
      result.report = compare_programs(program, *p.program, state, n_inputs, n_outputs, options);
    } else if (p.is_annotation()) {
      ir::PlanAnnotations merged = base_plan;
      merge_annotations(merged, *p.annotations);
      result.report = verify_annotations(program, merged, state, n_inputs, n_outputs, options);
    } else {
      result.report = VerifyReport{};  // empty Proposal at a matched site: a matcher bug (rule.hpp)
    }
    if (!result.report.passed()) return result;
  }
  return result;
}

}  // namespace epykos::rewrite
