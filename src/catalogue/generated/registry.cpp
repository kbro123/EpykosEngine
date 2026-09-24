// EpykosEngine — GENERATED FILE. Do not hand-edit. See kernels_e0.cpp's header.
#include "epykos/catalogue/registry.hpp"

namespace epykos::catalogue::generated {
void kernel_05655c67bbabdf43(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_12441ddee83dfe58(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_25bc072d1fea93a9(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_340886b8281d784a(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_41adba680d15745b(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_4201be3e8813a35c(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_4d3019c24c0d8044(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_4deaa61a97cf0cb6(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_4e7edebd795bcae0(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_67bfed22409cf372(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_6a535eefecf825da(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_803594e6390b7977(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_8e424571576a8137(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_8e7aa74146ede084(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_9743bc8d7c4102a4(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_bb876e73c39bcfcc(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_d4b14e4b8ce98e4f(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_d5b6eb8bdc669bfe(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_dd2d4a5510d7a13d(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_e60c427e9aebcee8(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_f470d9c110757729(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
void kernel_f8a1f6ca7418abbf(double*, ir::value_id, int, int, int, const double* const*, const double* const*,
                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, const double*);
}  // namespace epykos::catalogue::generated

namespace epykos::catalogue {

const std::vector<RegistryEntry>& generated_entries() {
  static const std::vector<RegistryEntry> table = {
      {Signature{{StepShape{Op::Mul, SlotShape::Column, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Mul, SlotShape::Step, SlotShape::Gather, SlotShape::None, SlotShape::None, 1, 0, 0, 0}}}, "mul(col,gat);mul(step-1,gat)", epykos::catalogue::generated::kernel_05655c67bbabdf43},
      {Signature{{StepShape{Op::Mul, SlotShape::Literal, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}}}, "mul(lit,gat)", epykos::catalogue::generated::kernel_12441ddee83dfe58},
      {Signature{{StepShape{Op::Sum, SlotShape::Segment, SlotShape::None, SlotShape::None, SlotShape::None, 0, 0, 0, 0}}}, "sum(seg)", epykos::catalogue::generated::kernel_25bc072d1fea93a9},
      {Signature{{StepShape{Op::Div, SlotShape::Gather, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Sub, SlotShape::Step, SlotShape::Gather, SlotShape::None, SlotShape::None, 1, 0, 0, 0}}}, "div(gat,gat);sub(step-1,gat)", epykos::catalogue::generated::kernel_340886b8281d784a},
      {Signature{{StepShape{Op::Div, SlotShape::Gather, SlotShape::Literal, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Sub, SlotShape::Literal, SlotShape::Step, SlotShape::None, SlotShape::None, 0, 1, 0, 0}, StepShape{Op::Sub, SlotShape::Gather, SlotShape::Step, SlotShape::None, SlotShape::None, 0, 1, 0, 0}}}, "div(gat,lit);sub(lit,step-1);sub(gat,step-1)", epykos::catalogue::generated::kernel_41adba680d15745b},
      {Signature{{StepShape{Op::Sub, SlotShape::Literal, SlotShape::Literal, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Div, SlotShape::Step, SlotShape::Literal, SlotShape::None, SlotShape::None, 1, 0, 0, 0}}}, "sub(lit,lit);div(step-1,lit)", epykos::catalogue::generated::kernel_4201be3e8813a35c},
      {Signature{{StepShape{Op::Affine, SlotShape::Segment, SlotShape::None, SlotShape::None, SlotShape::Literal, 0, 0, 0, 0}}}, "affine(seg;k=lit)", epykos::catalogue::generated::kernel_4d3019c24c0d8044},
      {Signature{{StepShape{Op::Mul, SlotShape::Gather, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}}}, "mul(gat,gat)", epykos::catalogue::generated::kernel_4deaa61a97cf0cb6},
      {Signature{{StepShape{Op::Sum, SlotShape::Gather, SlotShape::Gather, SlotShape::Gather, SlotShape::None, 0, 0, 0, 0}}}, "sum(gat,gat,gat)", epykos::catalogue::generated::kernel_4e7edebd795bcae0},
      {Signature{{StepShape{Op::Neg, SlotShape::Gather, SlotShape::None, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Mul, SlotShape::Step, SlotShape::Column, SlotShape::None, SlotShape::None, 1, 0, 0, 0}, StepShape{Op::Exp, SlotShape::Step, SlotShape::None, SlotShape::None, SlotShape::None, 1, 0, 0, 0}}}, "neg(gat);mul(step-1,col);exp(step-1)", epykos::catalogue::generated::kernel_67bfed22409cf372},
      {Signature{{StepShape{Op::Mul, SlotShape::Column, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Exp, SlotShape::Step, SlotShape::None, SlotShape::None, SlotShape::None, 1, 0, 0, 0}}}, "mul(col,gat);exp(step-1)", epykos::catalogue::generated::kernel_6a535eefecf825da},
      {Signature{{StepShape{Op::Sub, SlotShape::Gather, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}}}, "sub(gat,gat)", epykos::catalogue::generated::kernel_803594e6390b7977},
      {Signature{{StepShape{Op::Div, SlotShape::Gather, SlotShape::Literal, SlotShape::None, SlotShape::None, 0, 0, 0, 0}}}, "div(gat,lit)", epykos::catalogue::generated::kernel_8e424571576a8137},
      {Signature{{StepShape{Op::Sum, SlotShape::Gather, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}}}, "sum(gat,gat)", epykos::catalogue::generated::kernel_8e7aa74146ede084},
      {Signature{{StepShape{Op::Neg, SlotShape::Gather, SlotShape::None, SlotShape::None, SlotShape::None, 0, 0, 0, 0}}}, "neg(gat)", epykos::catalogue::generated::kernel_9743bc8d7c4102a4},
      {Signature{{StepShape{Op::Div, SlotShape::Gather, SlotShape::Column, SlotShape::None, SlotShape::None, 0, 0, 0, 0}}}, "div(gat,col)", epykos::catalogue::generated::kernel_bb876e73c39bcfcc},
      {Signature{{StepShape{Op::Sub, SlotShape::Gather, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Div, SlotShape::Step, SlotShape::Gather, SlotShape::None, SlotShape::None, 1, 0, 0, 0}, StepShape{Op::Sub, SlotShape::Step, SlotShape::Gather, SlotShape::None, SlotShape::None, 1, 0, 0, 0}}}, "sub(gat,gat);div(step-1,gat);sub(step-1,gat)", epykos::catalogue::generated::kernel_d4b14e4b8ce98e4f},
      {Signature{{StepShape{Op::Div, SlotShape::Gather, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Sub, SlotShape::Step, SlotShape::Literal, SlotShape::None, SlotShape::None, 1, 0, 0, 0}, StepShape{Op::Div, SlotShape::Step, SlotShape::Column, SlotShape::None, SlotShape::None, 1, 0, 0, 0}}}, "div(gat,gat);sub(step-1,lit);div(step-1,col)", epykos::catalogue::generated::kernel_d5b6eb8bdc669bfe},
      {Signature{{StepShape{Op::Sub, SlotShape::Gather, SlotShape::Literal, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Div, SlotShape::Step, SlotShape::Column, SlotShape::None, SlotShape::None, 1, 0, 0, 0}}}, "sub(gat,lit);div(step-1,col)", epykos::catalogue::generated::kernel_dd2d4a5510d7a13d},
      {Signature{{StepShape{Op::Sub, SlotShape::Gather, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}, StepShape{Op::Mul, SlotShape::Step, SlotShape::Column, SlotShape::None, SlotShape::None, 1, 0, 0, 0}}}, "sub(gat,gat);mul(step-1,col)", epykos::catalogue::generated::kernel_e60c427e9aebcee8},
      {Signature{{StepShape{Op::Mul, SlotShape::Column, SlotShape::Gather, SlotShape::None, SlotShape::None, 0, 0, 0, 0}}}, "mul(col,gat)", epykos::catalogue::generated::kernel_f470d9c110757729},
      {Signature{{StepShape{Op::Const, SlotShape::None, SlotShape::None, SlotShape::None, SlotShape::Column, 0, 0, 0, 0}}}, "const(-;k=col)", epykos::catalogue::generated::kernel_f8a1f6ca7418abbf},
  };
  return table;
}

}  // namespace epykos::catalogue
