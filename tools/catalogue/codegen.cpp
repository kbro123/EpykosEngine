#include "codegen.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "epykos/tape/op.hpp"

namespace epykos::catalogue::tool {

namespace {

std::string hex16(std::uint64_t v) {
  std::ostringstream os;
  os << std::hex << std::setw(16) << std::setfill('0') << v;
  return os.str();
}

// The exact C++ enumerator spelling for `op` (tape/op.hpp), for emitting `Op::<name>` as CODE in
// the generated registry — NOT epykos::to_string(Op), which is the stable lower-case NAME
// (op.hpp's own doc comment: "const", "input", "add", ...) meant for text, not identifiers.
const char* op_enum_name(Op op) {
  switch (op) {
    case Op::Const: return "Const";
    case Op::Input: return "Input";
    case Op::Add: return "Add";
    case Op::Sub: return "Sub";
    case Op::Mul: return "Mul";
    case Op::Div: return "Div";
    case Op::Neg: return "Neg";
    case Op::Exp: return "Exp";
    case Op::Log: return "Log";
    case Op::Sqrt: return "Sqrt";
    case Op::Recip: return "Recip";
    case Op::Fma: return "Fma";
    case Op::Select: return "Select";
    case Op::CmpLt: return "CmpLt";
    case Op::CmpLe: return "CmpLe";
    case Op::CmpGt: return "CmpGt";
    case Op::CmpGe: return "CmpGe";
    case Op::CmpEq: return "CmpEq";
    case Op::Sum: return "Sum";
    case Op::Affine: return "Affine";
    default: throw std::invalid_argument("codegen: op has no cataloguable enumerator spelling");
  }
}

// The exact C++ enumerator spelling for `s` (signature.hpp), for the same reason as
// op_enum_name above — catalogue::to_string(SlotShape) is the short display form ("step", "lit",
// ...), not the identifier.
const char* slotshape_enum_name(SlotShape s) {
  switch (s) {
    case SlotShape::None: return "None";
    case SlotShape::Step: return "Step";
    case SlotShape::Literal: return "Literal";
    case SlotShape::Column: return "Column";
    case SlotShape::Gather: return "Gather";
    case SlotShape::Segment: return "Segment";
  }
  throw std::invalid_argument("codegen: unknown SlotShape");
}

// Builds one operand's read expression, consuming (and advancing) the running Literal / Column /
// Gather occurrence counters exactly as src/catalogue/kernel.cpp's bind_domain does: one array
// slot per OCCURRENCE, in the same a/b/c/konst-per-step, step-in-order traversal. `k` is the
// current step index (for a Step-shaped operand's back-reference). Nothing here needs to know
// about D61's commutative canonicalisation: this walks a `Signature`, whose `a` / `b` are ALREADY
// in canonical order, and bind_domain applies the same order to the ir::Step it binds — so the
// k-th Gather this emits is the k-th one the binding supplies, as before.
struct Cursor {
  int lit = 0, col = 0, gat = 0;

  std::string expr(SlotShape shape, std::int32_t back, std::size_t k) {
    switch (shape) {
      case SlotShape::None:
        return {};
      case SlotShape::Step:
        return "s" + std::to_string(static_cast<long long>(k) - back);
      case SlotShape::Literal:
        return "(*literals[" + std::to_string(lit++) + "])";
      case SlotShape::Column:
        return "columns[" + std::to_string(col++) + "][row]";
      case SlotShape::Gather:
        return "values[static_cast<std::size_t>(gathers[" + std::to_string(gat++) + "][row]) * lz + l]";
      case SlotShape::Segment:
        throw std::invalid_argument("codegen: a Segment operand outside the terminal Sum/Affine step");
    }
    throw std::invalid_argument("codegen: unknown SlotShape");
  }
};

std::string binary_expr(Op op, const std::string& a, const std::string& b) {
  switch (op) {
    case Op::Add: return "(" + a + " + " + b + ")";
    case Op::Sub: return "(" + a + " - " + b + ")";
    case Op::Mul: return "(" + a + " * " + b + ")";
    case Op::Div: return "(" + a + " / " + b + ")";
    case Op::CmpLt: return "((" + a + " < " + b + ") ? 1.0 : 0.0)";
    case Op::CmpLe: return "((" + a + " <= " + b + ") ? 1.0 : 0.0)";
    case Op::CmpGt: return "((" + a + " > " + b + ") ? 1.0 : 0.0)";
    case Op::CmpGe: return "((" + a + " >= " + b + ") ? 1.0 : 0.0)";
    case Op::CmpEq: return "((" + a + " == " + b + ") ? 1.0 : 0.0)";
    default: throw std::invalid_argument("codegen: not a binary op");
  }
}

}  // namespace

std::string kernel_function_name(const Signature& sig) { return "kernel_" + hex16(sig.hash()); }

std::string generate_kernel(const Signature& sig, const std::string& function_name) {
  if (sig.steps.empty()) throw std::invalid_argument("codegen: an empty signature");
  std::ostringstream out;
  out << "void " << function_name
      << "(double* values, ir::value_id value_base, int r0, int n, int L,\n"
         "                    const double* const* literals, const double* const* columns,\n"
         "                    const std::int32_t* const* gathers, const std::int32_t* seg_offsets,\n"
         "                    const ir::value_id* seg_members, const double* seg_coefs) {\n"
         "  const std::size_t lz = static_cast<std::size_t>(L);\n"
         // Not every generated kernel's op sequence uses every table (most use none of
         // literals/columns/gathers/segment — exactly one signature shape uses each): silence
         // -Wunused-parameter uniformly rather than tracking, per signature, which tables this
         // particular one happens to need.
         "  (void)literals;\n"
         "  (void)columns;\n"
         "  (void)gathers;\n"
         "  (void)seg_offsets;\n"
         "  (void)seg_members;\n"
         "  (void)seg_coefs;\n"
         "  for (int i = 0; i < n; ++i) {\n"
         "    const int row = r0 + i;\n"
         "    for (int l = 0; l < L; ++l) {\n";

  Cursor cur;
  const std::size_t last = sig.steps.size() - 1;
  for (std::size_t k = 0; k < sig.steps.size(); ++k) {
    const StepShape& s = sig.steps[k];
    const std::string sk = "s" + std::to_string(k);

    if (s.a == SlotShape::Segment) {
      if (k != last || k != 0) {
        // is_whole_segment (rewrite/planner.hpp) only ever names a lone one-step group; the
        // generator only ever calls this function for such a signature (see generate_main.cpp).
        throw std::invalid_argument("codegen: a Segment step must be the group's only step");
      }
      const bool affine = s.op == Op::Affine;
      out << "      const std::int32_t seg_lo = seg_offsets[row];\n"
             "      const std::int32_t seg_hi = seg_offsets[row + 1];\n";
      if (affine) {
        const std::string konst_e = cur.expr(s.konst, s.konst_back, k);
        out << "      double " << sk << " = " << konst_e << ";\n"
               "      for (std::int32_t m = seg_lo; m < seg_hi; ++m) {\n"
               "        " << sk
            << " = " << sk
            << " + seg_coefs[m] * values[static_cast<std::size_t>(seg_members[m]) * lz + l];\n"
               "      }\n";
      } else if (s.op == Op::Sum) {
        out << "      double " << sk << " = values[static_cast<std::size_t>(seg_members[seg_lo]) * lz + l];\n"
               "      for (std::int32_t m = seg_lo + 1; m < seg_hi; ++m) {\n"
               "        " << sk << " = " << sk << " + values[static_cast<std::size_t>(seg_members[m]) * lz + l];\n"
               "      }\n";
      } else {
        throw std::invalid_argument("codegen: a Segment operand on a non-Sum/Affine op");
      }
      continue;
    }

    const std::string a = cur.expr(s.a, s.a_back, k);
    const std::string b = cur.expr(s.b, s.b_back, k);
    const std::string c = cur.expr(s.c, s.c_back, k);
    const std::string konst = cur.expr(s.konst, s.konst_back, k);

    std::string rhs;
    switch (s.op) {
      case Op::Const:
        rhs = konst;
        break;
      case Op::Add:
      case Op::Sub:
      case Op::Mul:
      case Op::Div:
      case Op::CmpLt:
      case Op::CmpLe:
      case Op::CmpGt:
      case Op::CmpGe:
      case Op::CmpEq:
        rhs = binary_expr(s.op, a, b);
        break;
      case Op::Neg:
        rhs = "(-" + a + ")";
        break;
      case Op::Exp:
        rhs = "std::exp(" + a + ")";
        break;
      case Op::Log:
        rhs = "std::log(" + a + ")";
        break;
      case Op::Sqrt:
        rhs = "std::sqrt(" + a + ")";
        break;
      case Op::Recip:
        rhs = "(1.0 / " + a + ")";
        break;
      case Op::Fma:
        rhs = "std::fma(" + a + ", " + b + ", " + c + ")";
        break;
      case Op::Select:
        rhs = "((" + a + " != 0.0) ? " + b + " : " + c + ")";
        break;
      case Op::Sum: {
        // A fixed-arity Sum (scan groups, DESIGN.md §5.6): a.kind != Segment already excluded
        // above, so this is the 2- or 3-operand left fold ir::is_fixed_sum names.
        rhs = s.c != SlotShape::None ? "((" + a + " + " + b + ") + " + c + ")" : "(" + a + " + " + b + ")";
        break;
      }
      default:
        throw std::invalid_argument(std::string("codegen: unsupported op ") + epykos::to_string(s.op));
    }
    out << "      const double " << sk << " = " << rhs << ";\n";
  }

  out << "      values[(static_cast<std::size_t>(value_base) + static_cast<std::size_t>(row)) * lz + "
         "static_cast<std::size_t>(l)] = s"
      << last
      << ";\n"
         "    }\n"
         "  }\n"
         "}\n";
  return out.str();
}

std::string generate_kernels_file(const std::vector<CatalogueEntry>& entries) {
  std::vector<CatalogueEntry> sorted = entries;
  std::sort(sorted.begin(), sorted.end(),
           [](const CatalogueEntry& x, const CatalogueEntry& y) { return x.signature.hash() < y.signature.hash(); });

  std::ostringstream out;
  out << "// EpykosEngine — GENERATED FILE. Do not hand-edit.\n"
         "//\n"
         "// Produced by tools/catalogue/generate_main.cpp (scripts/catalogue_regen.sh) from the "
         "reference\n"
         "// workloads (the Stage A tape and the M1 book), one function per distinct\n"
         "// epykos::catalogue::Signature the generator found among their catalogue-eligible domains.\n"
         "// Regenerating from the same workloads and seed (docs/WORKLOADS.md) reproduces this file\n"
         "// byte-for-byte; scripts/catalogue_regen.sh's `git diff --exit-code` is the CI check.\n"
         "//\n"
         "// Every kernel below shares one contract (include/epykos/catalogue/kernel.hpp): given a row\n"
         "// range [r0, r0+n) of lane width L, compute each row's Group value in the same op order, the\n"
         "// same operand-slot rules and the same left fold (Sum: from the first member; Affine: from\n"
         "// konst) as ir::evaluate / tape::replay / the interpreter's own generic per-step path, and "
         "write\n"
         "// it to values[(value_base + row)*L + l] — nothing else, no allocation, no side table. An\n"
         "// *_e0.cpp file (src/**/*_e0.cpp, CLAUDE.md's Build section, D25): -ffp-contract=off in every\n"
         "// preset, so an Op::Fma step here is the SAME explicit, one-rounding std::fma the reference\n"
         "// evaluator emits, never a compiler-contracted a*b+c silently replacing an Add of a Mul.\n"
         "#ifndef EPYKOS_FP_CONTRACT_OFF\n"
         "#error \"kernels_e0.cpp must be compiled with -ffp-contract=off (see the *_e0.cpp rule in "
         "CMakeLists.txt)\"\n"
         "#endif\n\n"
         "#include <cmath>\n"
         "#include <cstdint>\n\n"
         "#include \"epykos/ir/program.hpp\"\n\n"
         "namespace epykos::catalogue::generated {\n\n";
  for (const CatalogueEntry& e : sorted) {
    out << "// " << e.signature.to_string() << "  (found in: " << e.found_in << ")\n";
    out << generate_kernel(e.signature, kernel_function_name(e.signature)) << '\n';
  }
  out << "}  // namespace epykos::catalogue::generated\n";
  return out.str();
}

std::string generate_registry_file(const std::vector<CatalogueEntry>& entries) {
  std::vector<CatalogueEntry> sorted = entries;
  std::sort(sorted.begin(), sorted.end(),
           [](const CatalogueEntry& x, const CatalogueEntry& y) { return x.signature.hash() < y.signature.hash(); });

  std::ostringstream out;
  out << "// EpykosEngine — GENERATED FILE. Do not hand-edit. See kernels_e0.cpp's header.\n"
         "#include \"epykos/catalogue/registry.hpp\"\n\n"
         "namespace epykos::catalogue::generated {\n";
  for (const CatalogueEntry& e : sorted) {
    out << "void " << kernel_function_name(e.signature)
        << "(double*, ir::value_id, int, int, int, const double* const*, const double* const*,\n"
           "                    const std::int32_t* const*, const std::int32_t*, const ir::value_id*, "
           "const double*);\n";
  }
  out << "}  // namespace epykos::catalogue::generated\n\n"
         "namespace epykos::catalogue {\n\n"
         "const std::vector<RegistryEntry>& generated_entries() {\n"
         "  static const std::vector<RegistryEntry> table = {\n";
  for (const CatalogueEntry& e : sorted) {
    out << "      {Signature{{";
    for (std::size_t i = 0; i < e.signature.steps.size(); ++i) {
      const StepShape& s = e.signature.steps[i];
      if (i) out << ", ";
      out << "StepShape{Op::" << op_enum_name(s.op) << ", SlotShape::" << slotshape_enum_name(s.a)
          << ", SlotShape::" << slotshape_enum_name(s.b) << ", SlotShape::" << slotshape_enum_name(s.c)
          << ", SlotShape::" << slotshape_enum_name(s.konst) << ", " << s.a_back << ", " << s.b_back << ", "
          << s.c_back << ", " << s.konst_back << "}";
    }
    out << "}}, \"" << e.signature.to_string() << "\", epykos::catalogue::generated::" << kernel_function_name(e.signature)
        << "},\n";
  }
  out << "  };\n"
         "  return table;\n"
         "}\n\n"
         "}  // namespace epykos::catalogue\n";
  return out.str();
}

}  // namespace epykos::catalogue::tool
