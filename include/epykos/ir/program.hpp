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
// Scan domains (DESIGN.md §3.1 `scan`, §5.6; M3/G3, D41). A class whose instances form chains
// x_{k+1} = f(x_k, ...) — the natural product loop of a compounded coupon, a short-rate path — is
// one domain whose rows read earlier rows of the same domain: `recurrent` is set, and
// `Program::scans[domain.scan]` describes the recurrence: the rows are the steps of every chain,
// chain-major (chain c is rows [chain_offsets[c], chain_offsets[c+1])), and the *carry* gather
// reads the previous row of the same chain for every row but a chain's first, whose carry reads
// the chain's initial value in an earlier domain (a Const initial value is a row of the const
// domain, like a Const Sum member). Every other gather and segment of a scan domain reads
// earlier domains, or earlier rows of the scan (validate: no forward reads). Sequential along a
// chain, parallel across chains and batch lanes: exec::Interpreter evaluates a scan wave by
// wave (all chains' step k at once) and adjoint::Adjoint reverses it row by row backwards (the
// reverse scan). A scan group may contain a fixed-arity Sum step — a Sum whose two or three
// members are operand slots (a, b, c) rather than a segment — because fold_sum has already
// turned the step's `+` into a Sum node; it folds ((a + b) + c) like the variadic one and the
// expander emits a variadic Sum node for it, so the round-trip identity holds.
//
// A class that reads itself in a way the scan builder does not accept (the carry inside an
// Affine, a step reading a class that reads the scan, fewer than three steps) is split by
// dependency level as before (D22, D23): every level-domain carries `scan_class` and none is
// `recurrent`. Classes that depend on each other through other classes (legs -> swaps -> book)
// are split by level the same way, so every domain is evaluable in one pass (domain.level; see
// signature.hpp). `scan_class` is set on scan domains too (the class reads itself). The M1 book
// has neither: `scan_class_domains(program).empty()` is asserted on it.
//
// Evaluation contract (P4 and evaluate.hpp): iterate `domains` in order; for each row evaluate
// the group's steps in order; the row's value is the last step's value; store it at
// value_base + row. Sum folds ((m_0 + m_1) + m_2) ...; Affine folds ((c_0 + k_0·m_0) + k_1·m_1) ...
// exactly as tape/replay.hpp does. Inputs domain: the row's value is inputs[ordinal_of_row].
// A recurrent domain's rows are evaluated in row order, so a gather into the same domain reads
// a row already evaluated.
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
// Affine use `a` = {Segment, i}; Affine's c_0 is `konst` (Literal or Column). A fixed-arity Sum
// (scan groups only) uses a, b and optionally c as its members in fold order, a.kind != Segment.
// Const steps (the Const domain: constants that are Sum members or outputs in their own right)
// have `konst` only. Input steps have a = {Input, -1}; their value at record time is
// Program::input_values.
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
  std::int32_t scan = -1;   // index into Program::scans when this domain is a scan (then recurrent)
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

// A scan domain's recurrence (D41): its rows are the steps of `chain_offsets.size() - 1` chains,
// chain-major; `carry_gather` (a gather of the domain) reads value_base + r - 1 for every row r
// that is not a chain's first, and a value of an earlier domain (the chain's initial value) for
// a chain's first row. The group's step reads the carry like any gather.
struct Scan {
  domain_id domain = -1;
  std::int32_t carry_gather = -1;
  std::vector<std::int32_t> chain_offsets;  // chains + 1; chain c is rows [off[c], off[c+1])
  bool operator==(const Scan&) const = default;
  std::int32_t chains() const noexcept { return static_cast<std::int32_t>(chain_offsets.size()) - 1; }
};

struct Program {
  std::vector<Domain> domains;  // evaluation order
  std::vector<Group> groups;    // groups[d] belongs to domains[d]
  std::vector<double> literals;
  std::vector<Column> columns;
  std::vector<Gather> gathers;
  std::vector<Segment> segments;
  std::vector<Scan> scans;      // one per scan domain (domains[s.domain].scan == index)
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
// Domains of a self-reading class (scan domains and level-split scan candidates alike), i.e.
// those with scan_class set.
std::vector<domain_id> scan_class_domains(const Program& p);
// Scan domains (Domain::scan >= 0), in evaluation order.
std::vector<domain_id> scan_domains(const Program& p);
// The Sum step form: true when the step is a Sum over operand slots (scan groups), false for a
// Sum over a segment.
inline bool is_fixed_sum(const Step& s) noexcept { return s.op == Op::Sum && s.a.kind != SlotKind::Segment; }
// Number of members of a fixed-arity Sum step (2 or 3).
inline int fixed_sum_arity(const Step& s) noexcept { return s.c.kind == SlotKind::None ? 2 : 3; }

// The rows of a domain: helpers for tests and consumers.
inline value_id value_of(const Program& p, domain_id d, row_id r) noexcept {
  return p.domains[static_cast<std::size_t>(d)].value_base + r;
}

// The group's op sequence as one line, e.g. "exp(mul(neg(@0),$0))": @k gather k of the group,
// $k column k, #k literal k, %k segment k (in order of first use in the group), "in" an Input,
// "^" the carry gather of a scan domain (e.g. "mul(^,@0)" for a compounding step). A group of
// more than 24 steps is named "<last op>[<steps>]", e.g. "sqrt[30]". Names never contain
// whitespace (serialize writes them as one token).
std::string shape_string(const Program& p, domain_id d);


// Structural checks: slot ranges, topological steps, segment offsets, value ids in range and
// earlier in evaluation order (no forward reads), inputs / outputs in range, every scan's chain
// layout and carry gather (a recurrent domain that is not a scan is permitted here; the
// interpreter and the adjoint refuse it). Throws std::runtime_error with a message on the first
// violation.
void validate(const Program& p);

// A readable summary: every domain with rows, level, what it reads, its op sequence and the
// per-slot classification (literal / column / gather / segment). One line per domain plus
// totals. Data columns are not printed (use serialize).
std::string to_string(const Program& p);
void dump(const Program& p, std::ostream& os);

// Exact text serialisation (doubles as hexadecimal bit patterns; format "epykos-ir 3": version 2
// plus the scan field of a domain and the scans section). deserialize(serialize(p)) == p.
std::string serialize(const Program& p);

Program deserialize(const std::string& text);  // throws std::runtime_error on malformed text

}  // namespace epykos::ir
