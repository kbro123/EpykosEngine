#include "epykos/ir/annotate.hpp"

namespace epykos::ir {

const char* to_string(Materialise m) noexcept {
  switch (m) {
    case Materialise::Materialize: return "materialise";
    case Materialise::FuseIntoReduction: return "fuse-into-reduction";
    case Materialise::InlineIntoConsumer: return "inline-into-consumer";
  }
  return "?";
}

const char* to_string(AdMode m) noexcept {
  switch (m) {
    case AdMode::Forward: return "forward";
    case AdMode::Reverse: return "reverse";
    case AdMode::ClosedFormAffine: return "closed-form-affine";
  }
  return "?";
}

}  // namespace epykos::ir
