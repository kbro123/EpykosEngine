// EpykosEngine — the scalar tape: a node table recorded by `Rec` (D14).
//
// A Tape is a straight-line program in topological order: node i only reads nodes < i.
//   * Inputs are nodes (Op::Input) with an ordinal `a` that indexes the `inputs` array of a replay.
//   * Constants are leaves (Op::Const), one node per distinct bit pattern.
//   * Taint ("depends on an Input") is computed when a node is emitted and stored on the node.
//   * Outputs are registered explicitly; their ordinals index the `outputs` array of a replay.
//   * Variadic operands (Sum, Affine) live in a side array, [first_arg, first_arg + nargs); Affine
//     coefficients live in a parallel side array at the same offsets.
//
// Recording is scoped: `Tape::Scope guard(tape);` makes `tape` the current tape for the `Rec`
// operators (scalar/rec.hpp) until the guard is destroyed. Scopes nest (the previous current tape
// is restored). The current tape is thread-local; recording the same tape from two threads is
// not supported.
//
// Every node table has a serial (Tape::serial(), unique for the life of the process) that the
// values recorded on it carry (Rec::tape, D24): using a value on a tape other than the one it was
// recorded on throws RecordError instead of reinterpreting its node id. A copy of a tape is a new
// table with a new serial; a move keeps the serial; clear() and the passes (which swap in a
// rebuilt table) give the object a new serial, so stale values throw rather than alias.
//
// The passes in tape/passes.hpp rewrite and renumber the node table. Node ids held in `Rec`
// values are valid only until the first pass runs; input and output ordinals are stable forever.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "epykos/tape/op.hpp"

namespace epykos {

using node_id = std::int32_t;
inline constexpr node_id invalid_node = -1;
// The identity of a node table as seen by the values recorded on it (Rec::tape); 0 is "no tape".
using tape_serial = std::uint32_t;
inline constexpr tape_serial no_tape = 0;

// Thrown by the recording discipline: structural_if on a tainted predicate, .value() on a
// tainted value in record mode, a Rec node used outside its recording scope, bad node ids.
class RecordError : public std::runtime_error {
 public:
  explicit RecordError(const std::string& what) : std::runtime_error(what) {}
};

struct Node {
  Op op = Op::Const;
  bool tainted = false;             // depends on an Input
  node_id a = invalid_node;         // operand 1; Input: the input ordinal
  node_id b = invalid_node;         // operand 2
  node_id c = invalid_node;         // operand 3 (Fma addend, Select false-arm)
  double konst = 0.0;               // Const: the value; Input: value at record time; Affine: c_0
  std::int32_t nargs = 0;           // variadic: operand count
  std::int32_t first_arg = 0;       // variadic: offset into Tape::args() / Tape::coefs()
};

class Tape {
 public:
  Tape();
  Tape(const Tape& other);            // a new table with a new serial
  Tape(Tape&& other) noexcept;        // takes the serial; `other` gets a new one
  Tape& operator=(const Tape& other);
  Tape& operator=(Tape&& other) noexcept;
  ~Tape();

  // This node table's serial: what Rec::tape must equal for a value to be used on this tape.
  tape_serial serial() const noexcept { return serial_; }

  // ---- recording scope -------------------------------------------------------------------
  class Scope {
   public:
    explicit Scope(Tape& tape) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    Tape* prev_;
  };
  // The tape the Rec operators record onto, or nullptr when no Scope is active on this thread.
  static Tape* current() noexcept;
  bool recording() const noexcept { return current() == this; }

  // ---- leaves ----------------------------------------------------------------------------
  // A new Input node; its ordinal is num_inputs() before the call. `value` is the record-point
  // value (kept in Node::konst for debugging and dumps; a replay reads the inputs array).
  node_id input(double value);
  // The Const node for this bit pattern, created on first use (+0.0 and -0.0 are distinct).
  node_id constant(double value);
  // Sets the record-point value of input `ordinal` (Node::konst, what input_values() reports).
  // The solver layer writes an implicit node's solution here after its record-time solve, so the
  // record point of a tape with implicit nodes is the calibrated one. Throws RecordError for a
  // bad ordinal.
  void set_input_value(int ordinal, double value);

  // ---- nodes (low-level; the Rec operators are the normal way) ---------------------------
  node_id unary(Op op, node_id a);
  node_id binary(Op op, node_id a, node_id b);
  node_id ternary(Op op, node_id a, node_id b, node_id c);
  // Sum: coefs ignored (may be empty). Affine: coefs.size() == args.size(), konst = c_0.
  node_id variadic(Op op, std::span<const node_id> args, std::span<const double> coefs = {},
                   double konst = 0.0);

  // ---- outputs ---------------------------------------------------------------------------
  // Registers `id` as the next output; returns its ordinal. A node may be registered twice.
  int output(node_id id);

  // ---- inspection ------------------------------------------------------------------------
  std::size_t size() const noexcept { return nodes_.size(); }
  const std::vector<Node>& nodes() const noexcept { return nodes_; }
  // Throws RecordError (not std::out_of_range) for an id this tape does not hold.
  const Node& node(node_id id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= nodes_.size()) bad_node_id(id);
    return nodes_[static_cast<std::size_t>(id)];
  }
  const Node& operator[](node_id id) const noexcept {
    return nodes_[static_cast<std::size_t>(id)];
  }
  bool tainted(node_id id) const { return node(id).tainted; }
  std::span<const node_id> args(const Node& n) const noexcept {
    return op_is_variadic(n.op)
               ? std::span<const node_id>(args_.data() + n.first_arg,
                                          static_cast<std::size_t>(n.nargs))
               : std::span<const node_id>();
  }
  std::span<const node_id> args(node_id id) const { return args(node(id)); }
  std::span<const double> coefs(const Node& n) const noexcept {
    return n.op == Op::Affine ? std::span<const double>(coefs_.data() + n.first_arg,
                                                        static_cast<std::size_t>(n.nargs))
                              : std::span<const double>();
  }
  std::span<const double> coefs(node_id id) const { return coefs(node(id)); }
  // Side arrays (parallel: coefs_[k] belongs to args_[k]; 1.0 for Sum operands).
  const std::vector<node_id>& arg_table() const noexcept { return args_; }
  const std::vector<double>& coef_table() const noexcept { return coefs_; }

  // Input node ids in ordinal order and registered output node ids in ordinal order.
  const std::vector<node_id>& inputs() const noexcept { return inputs_; }
  const std::vector<node_id>& outputs() const noexcept { return outputs_; }
  std::size_t num_inputs() const noexcept { return inputs_.size(); }
  std::size_t num_outputs() const noexcept { return outputs_.size(); }
  // Record-point input values in ordinal order (Node::konst of each Input).
  std::vector<double> input_values() const;

  // Number of Const nodes.
  std::size_t num_constants() const noexcept { return const_index_.size(); }

  // Counts of nodes per op, indexed by static_cast<int>(Op).
  std::vector<std::size_t> op_histogram() const;

  // ---- whole-tape operations -------------------------------------------------------------
  void clear();
  void swap(Tape& other) noexcept;
  // Verifies topological order, operand ranges, taint bits, side-array bounds and the output
  // list; throws RecordError with a message on the first violation.
  void validate() const;

 private:
  node_id push(Node n);
  void taint_from(Node& n) const noexcept;
  [[noreturn]] void bad_node_id(node_id id) const;
  static tape_serial next_serial() noexcept;

  tape_serial serial_ = no_tape;
  std::vector<Node> nodes_;

  std::vector<node_id> args_;
  std::vector<double> coefs_;
  std::vector<node_id> inputs_;
  std::vector<node_id> outputs_;
  std::unordered_map<std::uint64_t, node_id> const_index_;  // bit pattern -> Const node
};

// One line per node: "#12 mul #3 #7 [t]" etc. For debugging and test failure messages.
void dump(const Tape& tape, std::ostream& os);
std::string to_string(const Tape& tape);

}  // namespace epykos
