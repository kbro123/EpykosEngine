#include "epykos/maths/instrument/tables.hpp"

#include <stdexcept>

namespace epykos::instrument {

const char* to_string(CouponKind k) noexcept {
  switch (k) {
    case CouponKind::Fixed: return "fixed";
    case CouponKind::RfrCompounded: return "rfr_compounded";
    case CouponKind::RfrAveraged: return "rfr_averaged";
    case CouponKind::TermRate: return "term_rate";
  }
  return "?";
}

CouponKind coupon_kind_from_string(const std::string& s) {
  if (s == "fixed") return CouponKind::Fixed;
  if (s == "rfr_compounded") return CouponKind::RfrCompounded;
  if (s == "rfr_averaged") return CouponKind::RfrAveraged;
  if (s == "term_rate") return CouponKind::TermRate;
  throw std::invalid_argument("instrument: unknown coupon kind \"" + s + "\" (fixed | rfr_compounded | rfr_averaged | term_rate)");
}

const char* to_string(Kind k) noexcept {
  switch (k) {
    case Kind::Ois: return "ois";
    case Kind::Irs: return "irs";
    case Kind::Basis: return "basis";
    case Kind::Deposit: return "deposit";
    case Kind::Future: return "future";
  }
  return "?";
}

int Instrument::n_coupons() const noexcept {
  int n = 0;
  for (const Leg& l : legs) n += static_cast<int>(l.coupons.size());
  return n;
}

int Instrument::n_obs_days() const noexcept {
  int n = 0;
  for (const Leg& l : legs) n += static_cast<int>(l.obs.size());
  return n;
}

}  // namespace epykos::instrument
