#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

const char* to_string(Exactness e) noexcept {
  switch (e) {
    case Exactness::E0: return "E0";
    case Exactness::E1: return "E1";
  }
  return "?";
}

const char* to_string(SiteKind k) noexcept {
  switch (k) {
    case SiteKind::Program: return "program";
    case SiteKind::Domain: return "domain";
    case SiteKind::Step: return "step";
    case SiteKind::Gather: return "gather";
    case SiteKind::Segment: return "segment";
  }
  return "?";
}

}  // namespace epykos::rewrite
