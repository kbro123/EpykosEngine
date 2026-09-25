// EpykosEngine — name tables for the term-rewriting DESIGN headers (P1, D73).
//
// This is the ONLY .cpp the term-rewriting design package ships, and it deliberately contains no
// design logic: three `to_string` tables, matching the convention every other enum in the
// repository follows (tape/op.hpp, rewrite/rule.hpp). It exists so that the enumerations are
// linkable and so `tests/rewrite/term_interface_test.cpp` can assert that every `Axiom` and every
// `RecurrenceClass` has a name — a cheap guard against the enum in the header and the identity
// table in `docs/TERM_REWRITING.md` §3 drifting apart, which is the most likely way a
// design-only package rots before it is implemented.
//
// Everything else the headers declare is intentionally undefined: there is no implementation yet
// and there is not meant to be one until the interface has been reviewed.

#include "epykos/optimise/term_egraph.hpp"
#include "epykos/optimise/term_extract.hpp"
#include "epykos/rewrite/error_model.hpp"
#include "epykos/rewrite/recurrence.hpp"
#include "epykos/rewrite/term_rule.hpp"

namespace epykos::rewrite {

const char* to_string(OutputClass c) noexcept {
  switch (c) {
    case OutputClass::Valuation: return "valuation";
    case OutputClass::Sensitivity: return "sensitivity";
    case OutputClass::Diagnostic: return "diagnostic";
    case OutputClass::Count_: break;
  }
  return "?";
}

const char* to_string(ExactnessHint h) noexcept {
  switch (h) {
    case ExactnessHint::Unknown: return "unknown";
    case ExactnessHint::Exact: return "exact";
    case ExactnessHint::ExactGivenCondition: return "exact_given_condition";
    case ExactnessHint::Inexact: return "inexact";
  }
  return "?";
}

const char* to_string(PathKind p) noexcept {
  switch (p) {
    case PathKind::GenericInterpreter: return "generic_interpreter";
    case PathKind::CatalogueKernel: return "catalogue_kernel";
    case PathKind::Unfused: return "unfused";
    case PathKind::Fused: return "fused";
    case PathKind::Adjoint: return "adjoint";
    case PathKind::ForwardDual: return "forward_dual";
    case PathKind::Oracle: return "oracle";
  }
  return "?";
}

const char* to_string(RecurrenceClass c) noexcept {
  switch (c) {
    case RecurrenceClass::None: return "none";
    case RecurrenceClass::ConsecutiveRatioProduct: return "consecutive_ratio_product";
    case RecurrenceClass::Geometric: return "geometric";
    case RecurrenceClass::Arithmetic: return "arithmetic";
    case RecurrenceClass::LinearConstantCoefficient: return "linear_constant_coefficient";
    case RecurrenceClass::Count_: break;
  }
  return "?";
}

const char* to_string(Axiom a) noexcept {
  switch (a) {
    case Axiom::NegNeg: return "neg_neg";
    case Axiom::SubAsAddNeg: return "sub_as_add_neg";
    case Axiom::NegSub: return "neg_sub";
    case Axiom::MulOne: return "mul_one";
    case Axiom::DivOne: return "div_one";
    case Axiom::SelectSameArms: return "select_same_arms";
    case Axiom::SelectPushUnary: return "select_push_unary";
    case Axiom::CommuteAddMul: return "commute_add_mul";
    case Axiom::AddZero: return "add_zero";
    case Axiom::SubZero: return "sub_zero";
    case Axiom::MulZero: return "mul_zero";
    case Axiom::SubSelf: return "sub_self";
    case Axiom::DivSelf: return "div_self";
    case Axiom::AffineDropZeroCoef: return "affine_drop_zero_coef";
    case Axiom::AddAssoc: return "add_assoc";
    case Axiom::MulAssoc: return "mul_assoc";
    case Axiom::MulDistribAdd: return "mul_distrib_add";
    case Axiom::DivAsMulRecip: return "div_as_mul_recip";
    case Axiom::MulRecipAsDiv: return "mul_recip_as_div";
    case Axiom::FmaContract: return "fma_contract";
    case Axiom::FmaExpand: return "fma_expand";
    case Axiom::RecipRecip: return "recip_recip";
    case Axiom::ExpProduct: return "exp_product";
    case Axiom::ExpSumSegment: return "exp_sum_segment";
    case Axiom::LogProduct: return "log_product";
    case Axiom::LogRecip: return "log_recip";
    case Axiom::ExpLog: return "exp_log";
    case Axiom::LogExp: return "log_exp";
    case Axiom::SqrtProduct: return "sqrt_product";
    case Axiom::SumFactorCommon: return "sum_factor_common";
    case Axiom::GatherPushUnary: return "gather_push_unary";
    case Axiom::GatherPushBinary: return "gather_push_binary";
    case Axiom::Count_: break;
  }
  return "?";
}

}  // namespace epykos::rewrite

namespace epykos::optimise {

const char* to_string(PlanningMode m) noexcept {
  switch (m) {
    case PlanningMode::InSearch: return "in_search";
    case PlanningMode::PostExtractionPass: return "post_extraction_pass";
  }
  return "?";
}

}  // namespace epykos::optimise
