// EpykosEngine — the domain IR (DESIGN.md §5, D14): plain data, serialisable, no pointers.
//
// A Program is the scalar tape re-expressed as a few array operations over index spaces:
//
//   domain    an isomorphism class of the signature pass: `rows` instances of one op sequence.
//             Every row produces one value. Values of all domains live in one flat "value space":
//             value id v = domain.value_base + row, with domains laid out in evaluation order.
//   group     the op sequence (Steps) of a domain, evaluated once per row. A Step's operands are
//             Slots: an earlier Step, a Literal (constant uniform across rows), a Column (constant
//             varying across rows), a Gather (a value id per row, read from the value space) or,
//             for Sum / Affine, a Segment.
//   column    one double per row of its domain.
//   literal   one double, shared by every row.
//   gather    one value id per row of the reading domain; the value may come from any domain
//             (a value id is global), including several domains for one gather.
//   segment   the members of a variadic op per row, CSR: members[offsets[r] .. offsets[r+1]) are
//             value ids in the recorded fold order; Affine segments carry a coefficient per member.
//   inputs    value id of every Input ordinal (they are rows of the Input domain).
//   outputs   value id of every output ordinal.
//
// Rows of a domain never read other rows of the same domain in a program infer() produces: a
// class whose instances read each other directly (a recurrence, DESIGN.md §5.6) is split by
// dependency level like any class in a cycle, and every level-domain carries `scan_class` (the
// M5 scan candidate; M1 consumers assert `scan_class_domains(program).empty()` on the M1 book).
// `recurrent` is the per-domain fact — the rows of this domain read this domain (earlier rows) —
// which validate() permits and exec::Interpreter refuses; infer() never sets it (D22, D23).
// Classes that depend on each other through other classes (legs -> swaps -> book) are split by
// level the same way, so every domain is evaluable in one pass (domain.level; see signature.hpp).
//
// Evaluation contract (P4 and evaluate.hpp): iterate `domains` in order; for each row evaluate
// the group's steps in order; the row's value is the last step's value; store it at
// value_base + row. Sum folds ((m_0 + m_1) + m_2) ...; Affine folds ((c_0 + k_0·m_0) + k_1·m_1) ...
// exactly as tape/replay.hpp does. Inputs domain: the row's value is inputs[ordinal_of_row].
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "epykos/tape/op.hpp"

namespace epykos::ir {

using value_id = std::int32_t;   // index into the value space (domain.value_base + row)
using domain_id = std::int32_t;
using row_id = std::int32_t;
inline constexpr value_id invalid_value = -1;

enum class SlotKind : std::uint8_t {
  None = 0,     // operand not used by this op
  Step = 1,     // index: an earlier step of the same group
  Literal = 2,  // index: Program::literals
  Column = 3,   // index: Program::columns (a column of this domain)
  Gather = 4,   // index: Program::gathers (a gather of this domain)
  Segment = 5,  // index: Program::segments (a segment table of this domain; Sum / Affine)
  Input = 6,    // the row's input ordinal (Input steps only; index unused)
};

const char* to_string(SlotKind kind) noexcept;

struct Slot {
  SlotKind kind = SlotKind::None;
  std::int32_t index = -1;
  bool operator==(const Slot&) const = default;
};

// One op of a group. Fixed-arity ops use a, b, c (Fma: a*b+c; Select: a ? b : c). Sum and
// Affine use `a` = {Segment, i}; Affine's c_0 is `konst` (Literal or Column). Const steps (the
// Const domain: constants that are Sum members or outputs in their own right) have `konst` only.
// Input steps have a = {Input, -1}; their value at record time is Program::input_values.
struct Step {
  Op op = Op::Const;
  Slot a, b, c;
  Slot konst;  // Affine c_0 / Const value; None otherwise
  bool operator==(const Step&) const = default;
};

struct Group {
  domain_id domain = -1;
  std::vector<Step> steps;  // topological; steps.back() is the row's value
  bool operator==(const Group&) const = default;
};

struct Domain {
  std::string name;         // the op sequence in a readable form (see shape_string)
  std::int32_t rows = 0;
  value_id value_base = 0;  // value id of row 0
  std::int32_t level = 0;   // dependency level inside a class cycle (0 when the class has none)
  bool recurrent = false;   // rows read earlier rows of this same domain (never set by infer())
  std::vector<domain_id> reads;  // domains this domain's gathers / segments read (sorted, unique)
  bool scan_class = false;  // the class this domain was split from reads itself (a scan candidate)
  bool operator==(const Domain&) const = default;
};

struct Column {
  domain_id domain = -1;
  std::vector<double> values;  // one per row
  bool operator==(const Column&) const = default;
};

struct Gather {
  domain_id domain = -1;
  std::vector<value_id> index;  // one value id per row of `domain`
  bool operator==(const Gather&) const = default;
};

struct Segment {
  domain_id domain = -1;
  std::vector<std::int32_t> offsets;  // rows + 1
  std::vector<value_id> members;      // fold order
  std::vector<double> coefs;          // Affine: one per member; Sum: empty
  bool operator==(const Segment&) const = default;
};

struct Program {
  std::vector<Domain> domains;  // evaluation order
  std::vector<Group> groups;    // groups[d] belongs to domains[d]
  std::vector<double> literals;
  std::vector<Column> columns;
  std::vector<Gather> gathers;
  std::vector<Segment> segments;
  std::vector<value_id> inputs;        // per input ordinal
  std::vector<double> input_values;    // per input ordinal: value at record time
  std::vector<value_id> outputs;       // per output ordinal
  bool operator==(const Program&) const = default;

  std::size_t num_values() const noexcept {
    return domains.empty() ? 0
                           : static_cast<std::size_t>(domains.back().value_base) +
                                 static_cast<std::size_t>(domains.back().rows);
  }
  // The domain holding value id v (domains are contiguous in value space).
  domain_id domain_of(value_id v) const noexcept;
  row_id row_of(value_id v) const noexcept;
};

// Domains flagged recurrent (rows reading rows of their own domain).
std::vector<domain_id> recurrent_domains(const Program& p);
// Domains of a self-reading class (scan candidates), i.e. those with scan_class set.
std::vector<domain_id> scan_class_domains(const Program& p);

// The rows of a domain: helpers for tests and consumers.
inline value_id value_of(const Program& p, domain_id d, row_id r) noexcept {
  return p.domains[static_cast<std::size_t>(d)].value_base + r;
}

// The group's op sequence as one line, e.g. "exp(mul(neg(@0),$0))": @k gather k of the group,
// $k column k, #k literal k, %k segment k (in order of first use in the group), "in" an Input.
// A group of more than 24 steps is named "<last op>[<steps>]", e.g. "sqrt[30]". Names never
// contain whitespace (serialize writes them as one token).
std::string shape_string(const Program& p, domain_id d);


// Structural checks: slot ranges, topological steps, segment offsets, value ids in range and
// earlier in evaluation order (no forward reads), inputs / outputs in range. Throws
// std::runtime_error with a message on the first violation.
void validate(const Program& p);

// A readable summary: every domain with rows, level, what it reads, its op sequence and the
// per-slot classification (literal / column / gather / segment). One line per domain plus
// totals. Data columns are not printed (use serialize).
std::string to_string(const Program& p);
void dump(const Program& p, std::ostream& os);

// Exact text serialisation (doubles as hexadecimal bit patterns; format "epykos-ir 2").
// deserialize(serialize(p)) == p.
std::string serialize(const Program& p);

Program deserialize(const std::string& text);  // throws std::runtime_error on malformed text

}  // namespace epykos::ir
