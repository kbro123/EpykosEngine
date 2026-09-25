// SPIKE, MEASUREMENT ONLY — how much of Stage A is the compounding scan telescoping would remove
// (D74). Companion to `tests/spike/telescoping_prize_test.cpp`, which is the end-to-end
// measurement on the `compare_ois` fixture.
//
// **This is a PARTIAL answer and says so.** The `compare_ois` measurement records the problem
// twice, once per coupon form, and compares. Stage A is not recorded twice here: its recording
// path (`fixtures::record_stage_a`) prices four curves, deposits, SR3 futures and a two-thousand
// trade multi-currency book through one monolithic function, and a second copy of it is a bigger
// change than a spike should make. What IS cheap, and is what this file does, is to record Stage
// A ONCE as the engine writes it and report how much of the resulting program is the daily
// compounding — the scan domains of D41 — so that the `compare_ois` figure can be read against
// the realism gate rather than transplanted onto it.
//
// Read the share as a LOWER bound on the prize, not an estimate of it. On `compare_ois` the
// compounding scan is 9,946 of 80,938 IR steps-times-rows (12.3%), and telescoping nevertheless
// takes the whole program from 80,938 to 1,182 — because removing the scan also removes the
// discount factors, gathers, columns and segments that existed only to feed it. The scan's own
// share is the part of the prize that can be counted without recording the problem twice.
#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/solver/implicit_program.hpp"

using namespace epykos;

namespace {

struct ScanShare {
  std::size_t steps = 0;        // steps x rows over every domain
  std::size_t scan_steps = 0;   // the same, restricted to scan domains
  std::size_t scan_rows = 0;
  std::size_t scan_domains = 0;
  std::size_t values = 0;
};

ScanShare share_of(const ir::Program& p) {
  ScanShare s;
  s.values = p.num_values();
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    s.steps += p.groups[d].steps.size() * static_cast<std::size_t>(p.domains[d].rows);
  }
  for (const ir::domain_id d : ir::scan_domains(p)) {
    const ir::Domain& dom = p.domains[static_cast<std::size_t>(d)];
    ++s.scan_domains;
    s.scan_rows += static_cast<std::size_t>(dom.rows);
    s.scan_steps += p.groups[static_cast<std::size_t>(d)].steps.size() * static_cast<std::size_t>(dom.rows);
  }
  return s;
}

}  // namespace

TEST(SpikeTelescopingStageA, HowMuchOfStageAIsTheCompoundingScan) {
  // Two hundred trades rather than the blueprint's two thousand, and no scenario lanes (D64): the
  // compounding this measures is overwhelmingly on the CALIBRATION side, which does not shrink
  // with the book, and a full-size recording is minutes of CI for the same share.
  fixtures::StageAOptions o;
  o.trades = 200;
  o.scenarios = 1;
  const fixtures::StageA s = fixtures::make_stage_a(o);

  int obs_days = 0, coupons = 0, compounded = 0;
  for (const instrument::CalibrationSet& cs : s.sets) {
    for (const instrument::CalibrationInstrument& ci : cs.instruments) {
      obs_days += ci.instrument.n_obs_days();
      coupons += ci.instrument.n_coupons();
      for (const instrument::Leg& leg : ci.instrument.legs) {
        for (const instrument::Coupon& c : leg.coupons) {
          if (c.kind == instrument::CouponKind::RfrCompounded) ++compounded;
        }
      }
    }
  }
  int book_obs_days = 0;
  for (const instrument::Instrument& in : s.instruments) book_obs_days += in.n_obs_days();

  fixtures::StageATape t = fixtures::record_stage_a(s);
  solver::ProgramOptions po;
  solver::ImplicitProgram prog(t.tape, t.registry, po);
  const ScanShare whole = share_of(prog.program());

  ScanShare resid;
  for (int k = 0; k < prog.n_blocks(); ++k) {
    const ScanShare r = share_of(prog.residual_program(k).program());
    resid.values += r.values;
    resid.steps += r.steps;
    resid.scan_steps += r.scan_steps;
    resid.scan_rows += r.scan_rows;
    resid.scan_domains += r.scan_domains;
  }

  std::cout << "[spike/telescoping] Stage A at two hundred book trades and one scenario lane (D64): "
            << s.n_quotes() << " quotes across " << s.n_curves() << " curves; the calibration instruments carry "
            << obs_days << " projected observation days across " << coupons << " coupons (" << compounded
            << " of them compounded), the book " << book_obs_days << " more; " << t.tape.size()
            << " tape nodes after the passes\n";
  std::cout << "[spike/telescoping] Stage A whole program: " << whole.values << " IR values, " << whole.steps
            << " steps x rows, of which " << whole.scan_steps << " (" << (100.0 * static_cast<double>(whole.scan_steps) /
                                                                          static_cast<double>(whole.steps))
            << "%) are the " << whole.scan_domains << " scan domains' " << whole.scan_rows << " rows\n";
  std::cout << "[spike/telescoping] Stage A residual slices (what the calibration solves iterate): " << resid.steps
            << " steps x rows, of which " << resid.scan_steps << " ("
            << (resid.steps == 0 ? 0.0 : 100.0 * static_cast<double>(resid.scan_steps) / static_cast<double>(resid.steps))
            << "%) are the " << resid.scan_domains << " scan domains' " << resid.scan_rows << " rows\n";
  std::cout << "[spike/telescoping] read that share as a LOWER bound: on compare_ois the scan is 12.3% of "
               "steps x rows and telescoping still takes the whole program down 68.5x, because the curve "
               "evaluations that fed the scan go with it\n";

  EXPECT_GT(whole.scan_domains, 0u) << "Stage A should record the compounding as scan domains (D41)";
  EXPECT_GT(whole.scan_rows, 0u);
}
