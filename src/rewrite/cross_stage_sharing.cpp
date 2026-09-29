#include "epykos/rewrite/cross_stage_sharing.hpp"

#include <utility>

namespace epykos::rewrite {

std::function<bool(const ir::Program&)> cross_stage_sharing_guard(std::vector<std::vector<int>> output_groups, epykos::Op op) {
  return [groups = std::move(output_groups), op](const ir::Program& program) {
    const ir::SharingReport report = ir::sharing(program, groups, op);
    return !ir::assert_all_shared(report);
  };
}

}  // namespace epykos::rewrite
