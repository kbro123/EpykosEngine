// EpykosEngine — GENERATED FILE. Do not hand-edit.
//
// Produced by tools/catalogue/generate_main.cpp (scripts/catalogue_regen.sh) from the reference
// workloads (the Stage A tape and the M1 book), one function per distinct
// epykos::catalogue::Signature the generator found among their catalogue-eligible domains.
// Regenerating from the same workloads and seed (docs/WORKLOADS.md) reproduces this file
// byte-for-byte; scripts/catalogue_regen.sh's `git diff --exit-code` is the CI check.
//
// Every kernel below shares one contract (include/epykos/catalogue/kernel.hpp): given a row
// range [r0, r0+n) of lane width L, compute each row's Group value in the same op order, the
// same operand-slot rules and the same left fold (Sum: from the first member; Affine: from
// konst) as ir::evaluate / tape::replay / the interpreter's own generic per-step path, and write
// it to values[(value_base + row)*L + l] — nothing else, no allocation, no side table. An
// *_e0.cpp file (src/**/*_e0.cpp, CLAUDE.md's Build section, D25): -ffp-contract=off in every
// preset, so an Op::Fma step here is the SAME explicit, one-rounding std::fma the reference
// evaluator emits, never a compiler-contracted a*b+c silently replacing an Add of a Mul.
#ifndef EPYKOS_FP_CONTRACT_OFF
#error "kernels_e0.cpp must be compiled with -ffp-contract=off (see the *_e0.cpp rule in CMakeLists.txt)"
#endif

#include <cmath>
#include <cstdint>

#include "epykos/ir/program.hpp"

namespace epykos::catalogue::generated {

// mul(col,gat);mul(step-1,gat)  (found in: m1_book)
void kernel_05655c67bbabdf43(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (columns[0][row] * values[static_cast<std::size_t>(gathers[0][row]) * lz + l]);
      const double s1 = (s0 * values[static_cast<std::size_t>(gathers[1][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s1;
    }
  }
}

// mul(lit,gat)  (found in: stage_a)
void kernel_12441ddee83dfe58(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = ((*literals[0]) * values[static_cast<std::size_t>(gathers[0][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// sum(seg)  (found in: m1_book, stage_a)
void kernel_25bc072d1fea93a9(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const std::int32_t seg_lo = seg_offsets[row];
      const std::int32_t seg_hi = seg_offsets[row + 1];
      double s0 = values[static_cast<std::size_t>(seg_members[seg_lo]) * lz + l];
      for (std::int32_t m = seg_lo + 1; m < seg_hi; ++m) {
        s0 = s0 + values[static_cast<std::size_t>(seg_members[m]) * lz + l];
      }
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// div(gat,gat);sub(step-1,gat)  (found in: stage_a)
void kernel_340886b8281d784a(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] / values[static_cast<std::size_t>(gathers[1][row]) * lz + l]);
      const double s1 = (s0 - values[static_cast<std::size_t>(gathers[2][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s1;
    }
  }
}

// div(gat,lit);sub(lit,step-1);sub(gat,step-1)  (found in: stage_a)
void kernel_41adba680d15745b(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] / (*literals[0]));
      const double s1 = ((*literals[1]) - s0);
      const double s2 = (values[static_cast<std::size_t>(gathers[1][row]) * lz + l] - s1);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s2;
    }
  }
}

// sub(lit,lit);div(step-1,lit)  (found in: stage_a)
void kernel_4201be3e8813a35c(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = ((*literals[0]) - (*literals[1]));
      const double s1 = (s0 / (*literals[2]));
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s1;
    }
  }
}

// affine(seg;k=lit)  (found in: m1_book, stage_a)
void kernel_4d3019c24c0d8044(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const std::int32_t seg_lo = seg_offsets[row];
      const std::int32_t seg_hi = seg_offsets[row + 1];
      double s0 = (*literals[0]);
      for (std::int32_t m = seg_lo; m < seg_hi; ++m) {
        s0 = s0 + seg_coefs[m] * values[static_cast<std::size_t>(seg_members[m]) * lz + l];
      }
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// mul(gat,gat)  (found in: stage_a)
void kernel_4deaa61a97cf0cb6(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] * values[static_cast<std::size_t>(gathers[1][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// sum(gat,gat,gat)  (found in: stage_a)
void kernel_4e7edebd795bcae0(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = ((values[static_cast<std::size_t>(gathers[0][row]) * lz + l] + values[static_cast<std::size_t>(gathers[1][row]) * lz + l]) + values[static_cast<std::size_t>(gathers[2][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// neg(gat);mul(step-1,col);exp(step-1)  (found in: m1_book, stage_a)
void kernel_67bfed22409cf372(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (-values[static_cast<std::size_t>(gathers[0][row]) * lz + l]);
      const double s1 = (s0 * columns[0][row]);
      const double s2 = std::exp(s1);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s2;
    }
  }
}

// mul(col,gat);exp(step-1)  (found in: stage_a)
void kernel_6a535eefecf825da(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (columns[0][row] * values[static_cast<std::size_t>(gathers[0][row]) * lz + l]);
      const double s1 = std::exp(s0);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s1;
    }
  }
}

// sub(gat,gat)  (found in: stage_a)
void kernel_803594e6390b7977(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] - values[static_cast<std::size_t>(gathers[1][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// div(gat,lit)  (found in: stage_a)
void kernel_8e424571576a8137(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] / (*literals[0]));
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// sum(gat,gat)  (found in: stage_a)
void kernel_8e7aa74146ede084(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] + values[static_cast<std::size_t>(gathers[1][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// neg(gat)  (found in: stage_a)
void kernel_9743bc8d7c4102a4(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (-values[static_cast<std::size_t>(gathers[0][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// div(gat,col)  (found in: stage_a)
void kernel_bb876e73c39bcfcc(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] / columns[0][row]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// sub(gat,gat);div(step-1,gat);sub(step-1,gat)  (found in: stage_a)
void kernel_d4b14e4b8ce98e4f(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] - values[static_cast<std::size_t>(gathers[1][row]) * lz + l]);
      const double s1 = (s0 / values[static_cast<std::size_t>(gathers[2][row]) * lz + l]);
      const double s2 = (s1 - values[static_cast<std::size_t>(gathers[3][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s2;
    }
  }
}

// div(gat,gat);sub(step-1,lit);div(step-1,col)  (found in: m1_book, stage_a)
void kernel_d5b6eb8bdc669bfe(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] / values[static_cast<std::size_t>(gathers[1][row]) * lz + l]);
      const double s1 = (s0 - (*literals[0]));
      const double s2 = (s1 / columns[0][row]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s2;
    }
  }
}

// sub(gat,lit);div(step-1,col)  (found in: stage_a)
void kernel_dd2d4a5510d7a13d(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] - (*literals[0]));
      const double s1 = (s0 / columns[0][row]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s1;
    }
  }
}

// sub(gat,gat);mul(step-1,col)  (found in: m1_book, stage_a)
void kernel_e60c427e9aebcee8(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (values[static_cast<std::size_t>(gathers[0][row]) * lz + l] - values[static_cast<std::size_t>(gathers[1][row]) * lz + l]);
      const double s1 = (s0 * columns[0][row]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s1;
    }
  }
}

// mul(col,gat)  (found in: m1_book, stage_a)
void kernel_f470d9c110757729(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = (columns[0][row] * values[static_cast<std::size_t>(gathers[0][row]) * lz + l]);
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

// const(-;k=col)  (found in: stage_a)
void kernel_f8a1f6ca7418abbf(double* values, ir::value_id value_base, int r0, int n, int L,
                    const double* const* literals, const double* const* columns,
                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                    const ir::value_id* seg_members, const double* seg_coefs) {
  const std::size_t lz = static_cast<std::size_t>(L);
  (void)literals;
  (void)columns;
  (void)gathers;
  (void)seg_offsets;
  (void)seg_members;
  (void)seg_coefs;
  for (int i = 0; i < n; ++i) {
    const int row = r0 + i;
    for (int l = 0; l < L; ++l) {
      const double s0 = columns[0][row];
      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + static_cast<std::size_t>(l)] = s0;
    }
  }
}

}  // namespace epykos::catalogue::generated
