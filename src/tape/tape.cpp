#include "epykos/tape/tape.hpp"

#include <atomic>
#include <cstring>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>


namespace epykos {

namespace {

thread_local Tape* g_current_tape = nullptr;
std::atomic<tape_serial> g_next_serial{1};

std::uint64_t bits_of(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

[[noreturn]] void bad_operand(const char* what, node_id id, std::size_t size) {
  throw RecordError(std::string("tape: ") + what + " node id " + std::to_string(id) +
                    " out of range (tape has " + std::to_string(size) + " nodes)");
}

}  // namespace

// ---- scope -------------------------------------------------------------------------------

Tape::Scope::Scope(Tape& tape) noexcept : prev_(g_current_tape) { g_current_tape = &tape; }
Tape::Scope::~Scope() { g_current_tape = prev_; }
Tape* Tape::current() noexcept { return g_current_tape; }

// ---- construction ------------------------------------------------------------------------

tape_serial Tape::next_serial() noexcept {
  // Serials never repeat within a process (2^32 tapes); 0 is reserved for "no tape".
  const tape_serial s = g_next_serial.fetch_add(1, std::memory_order_relaxed);
  return s == no_tape ? g_next_serial.fetch_add(1, std::memory_order_relaxed) : s;
}

Tape::Tape() : serial_(next_serial()) {}

Tape::Tape(const Tape& other)
    : serial_(next_serial()),
      nodes_(other.nodes_),
      args_(other.args_),
      coefs_(other.coefs_),
      inputs_(other.inputs_),
      outputs_(other.outputs_),
      const_index_(other.const_index_) {}

Tape::Tape(Tape&& other) noexcept
    : serial_(other.serial_),
      nodes_(std::move(other.nodes_)),
      args_(std::move(other.args_)),
      coefs_(std::move(other.coefs_)),
      inputs_(std::move(other.inputs_)),
      outputs_(std::move(other.outputs_)),
      const_index_(std::move(other.const_index_)) {
  other.serial_ = next_serial();
  other.nodes_.clear();
  other.args_.clear();
  other.coefs_.clear();
  other.inputs_.clear();
  other.outputs_.clear();
  other.const_index_.clear();
}

Tape& Tape::operator=(const Tape& other) {
  if (this != &other) {
    Tape copy(other);
    swap(copy);  // this takes the copy's new serial; the old table (and serial) dies with `copy`
  }
  return *this;
}

Tape& Tape::operator=(Tape&& other) noexcept {
  if (this != &other) {
    Tape moved(std::move(other));
    swap(moved);
  }
  return *this;
}

Tape::~Tape() {
  // A tape destroyed while it is the current tape leaves a dangling pointer: clear it.
  if (g_current_tape == this) g_current_tape = nullptr;
}

void Tape::bad_node_id(node_id id) const {
  throw RecordError("tape: node id " + std::to_string(id) + " is not on this tape (" +
                    std::to_string(nodes_.size()) + " nodes)");
}

void Tape::clear() {
  serial_ = next_serial();  // values recorded before the clear must not alias the new table
  nodes_.clear();
  args_.clear();
  coefs_.clear();
  inputs_.clear();
  outputs_.clear();
  const_index_.clear();
}

void Tape::swap(Tape& other) noexcept {
  std::swap(serial_, other.serial_);
  nodes_.swap(other.nodes_);

  args_.swap(other.args_);
  coefs_.swap(other.coefs_);
  inputs_.swap(other.inputs_);
  outputs_.swap(other.outputs_);
  const_index_.swap(other.const_index_);
}

// ---- emission ----------------------------------------------------------------------------

node_id Tape::push(Node n) {
  const node_id id = static_cast<node_id>(nodes_.size());
  nodes_.push_back(n);
  return id;
}

void Tape::taint_from(Node& n) const noexcept {
  bool t = false;
  if (n.a != invalid_node) t = t || nodes_[static_cast<std::size_t>(n.a)].tainted;
  if (n.b != invalid_node) t = t || nodes_[static_cast<std::size_t>(n.b)].tainted;
  if (n.c != invalid_node) t = t || nodes_[static_cast<std::size_t>(n.c)].tainted;
  for (std::int32_t k = 0; k < n.nargs; ++k) {
    t = t || nodes_[static_cast<std::size_t>(args_[static_cast<std::size_t>(n.first_arg + k)])].tainted;
  }
  n.tainted = t;
}

node_id Tape::input(double value) {
  Node n;
  n.op = Op::Input;
  n.tainted = true;
  n.a = static_cast<node_id>(inputs_.size());
  n.konst = value;
  const node_id id = push(n);
  inputs_.push_back(id);
  return id;
}

node_id Tape::constant(double value) {
  const std::uint64_t key = bits_of(value);
  auto it = const_index_.find(key);
  if (it != const_index_.end()) return it->second;
  Node n;
  n.op = Op::Const;
  n.tainted = false;
  n.konst = value;
  const node_id id = push(n);
  const_index_.emplace(key, id);
  return id;
}

node_id Tape::unary(Op op, node_id a) {
  if (op_arity(op) != 1) throw RecordError(std::string("tape: ") + to_string(op) + " is not unary");
  if (a < 0 || static_cast<std::size_t>(a) >= nodes_.size()) bad_operand("unary", a, nodes_.size());
  Node n;
  n.op = op;
  n.a = a;
  taint_from(n);
  return push(n);
}

node_id Tape::binary(Op op, node_id a, node_id b) {
  if (op_arity(op) != 2) throw RecordError(std::string("tape: ") + to_string(op) + " is not binary");
  if (a < 0 || static_cast<std::size_t>(a) >= nodes_.size()) bad_operand("binary", a, nodes_.size());
  if (b < 0 || static_cast<std::size_t>(b) >= nodes_.size()) bad_operand("binary", b, nodes_.size());
  Node n;
  n.op = op;
  n.a = a;
  n.b = b;
  taint_from(n);
  return push(n);
}

node_id Tape::ternary(Op op, node_id a, node_id b, node_id c) {
  if (op_arity(op) != 3) throw RecordError(std::string("tape: ") + to_string(op) + " is not ternary");
  if (a < 0 || static_cast<std::size_t>(a) >= nodes_.size()) bad_operand("ternary", a, nodes_.size());
  if (b < 0 || static_cast<std::size_t>(b) >= nodes_.size()) bad_operand("ternary", b, nodes_.size());
  if (c < 0 || static_cast<std::size_t>(c) >= nodes_.size()) bad_operand("ternary", c, nodes_.size());
  Node n;
  n.op = op;
  n.a = a;
  n.b = b;
  n.c = c;
  taint_from(n);
  return push(n);
}

node_id Tape::variadic(Op op, std::span<const node_id> args, std::span<const double> coefs,
                       double konst) {
  if (!op_is_variadic(op)) throw RecordError(std::string("tape: ") + to_string(op) + " is not variadic");
  if (args.empty()) throw RecordError(std::string("tape: ") + to_string(op) + " needs >= 1 operand");
  if (op == Op::Affine && coefs.size() != args.size()) {
    throw RecordError("tape: affine needs one coefficient per operand");
  }
  for (node_id a : args) {
    if (a < 0 || static_cast<std::size_t>(a) >= nodes_.size()) bad_operand("variadic", a, nodes_.size());
  }
  Node n;
  n.op = op;
  n.konst = (op == Op::Affine) ? konst : 0.0;
  n.nargs = static_cast<std::int32_t>(args.size());
  n.first_arg = static_cast<std::int32_t>(args_.size());
  args_.insert(args_.end(), args.begin(), args.end());
  if (op == Op::Affine) {
    coefs_.insert(coefs_.end(), coefs.begin(), coefs.end());
  } else {
    coefs_.insert(coefs_.end(), args.size(), 1.0);
  }
  taint_from(n);
  return push(n);
}

int Tape::output(node_id id) {
  if (id < 0 || static_cast<std::size_t>(id) >= nodes_.size()) bad_operand("output", id, nodes_.size());
  outputs_.push_back(id);
  return static_cast<int>(outputs_.size()) - 1;
}

// ---- inspection --------------------------------------------------------------------------

std::vector<double> Tape::input_values() const {
  std::vector<double> v;
  v.reserve(inputs_.size());
  for (node_id id : inputs_) v.push_back(nodes_[static_cast<std::size_t>(id)].konst);
  return v;
}

std::vector<std::size_t> Tape::op_histogram() const {
  std::vector<std::size_t> h(static_cast<std::size_t>(op_count), 0);
  for (const Node& n : nodes_) ++h[static_cast<std::size_t>(n.op)];
  return h;
}

void Tape::validate() const {
  const std::size_t n_nodes = nodes_.size();
  auto check = [&](node_id id, node_id operand, const char* slot) {
    if (operand < 0 || operand >= id) {
      throw RecordError("tape: node " + std::to_string(id) + " operand " + slot + " = " +
                        std::to_string(operand) + " is not an earlier node");
    }
  };
  std::size_t n_inputs = 0;
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const Node& n = nodes_[i];
    const node_id id = static_cast<node_id>(i);
    if (!op_is_supported(n.op)) {
      throw RecordError("tape: node " + std::to_string(id) + " has unsupported op " + to_string(n.op));
    }
    const int arity = op_arity(n.op);
    bool expect_taint = false;
    if (n.op == Op::Input) {
      if (n.a != static_cast<node_id>(n_inputs) || inputs_.size() <= n_inputs ||
          inputs_[n_inputs] != id) {
        throw RecordError("tape: input node " + std::to_string(id) + " has a bad ordinal");
      }
      ++n_inputs;
      expect_taint = true;
    } else if (arity >= 1) {
      check(id, n.a, "a");
      expect_taint = expect_taint || nodes_[static_cast<std::size_t>(n.a)].tainted;
      if (arity >= 2) {
        check(id, n.b, "b");
        expect_taint = expect_taint || nodes_[static_cast<std::size_t>(n.b)].tainted;
      }
      if (arity >= 3) {
        check(id, n.c, "c");
        expect_taint = expect_taint || nodes_[static_cast<std::size_t>(n.c)].tainted;
      }
    } else if (op_is_variadic(n.op)) {
      if (n.nargs < 1 || n.first_arg < 0 ||
          static_cast<std::size_t>(n.first_arg) + static_cast<std::size_t>(n.nargs) > args_.size() ||
          coefs_.size() != args_.size()) {
        throw RecordError("tape: node " + std::to_string(id) + " has a bad variadic range");
      }
      for (std::int32_t k = 0; k < n.nargs; ++k) {
        const node_id operand = args_[static_cast<std::size_t>(n.first_arg + k)];
        check(id, operand, "args");
        expect_taint = expect_taint || nodes_[static_cast<std::size_t>(operand)].tainted;
      }
    }
    if (n.tainted != expect_taint) {
      throw RecordError("tape: node " + std::to_string(id) + " has a wrong taint bit");
    }
  }
  if (n_inputs != inputs_.size()) throw RecordError("tape: input list does not match the nodes");
  for (node_id o : outputs_) {
    if (o < 0 || static_cast<std::size_t>(o) >= n_nodes) {
      throw RecordError("tape: output node " + std::to_string(o) + " out of range");
    }
  }
  for (const auto& [bits, id] : const_index_) {
    if (id < 0 || static_cast<std::size_t>(id) >= n_nodes || nodes_[static_cast<std::size_t>(id)].op != Op::Const ||
        bits_of(nodes_[static_cast<std::size_t>(id)].konst) != bits) {
      throw RecordError("tape: constant index is inconsistent");
    }
  }
}

// ---- dump --------------------------------------------------------------------------------

void dump(const Tape& tape, std::ostream& os) {
  const auto& nodes = tape.nodes();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const Node& n = nodes[i];
    os << '#' << i << ' ' << to_string(n.op);
    switch (n.op) {
      case Op::Const:
        os << ' ' << n.konst;
        break;
      case Op::Input:
        os << " @" << n.a << " (" << n.konst << ')';
        break;
      case Op::Sum: {
        for (node_id a : tape.args(n)) os << " #" << a;
        break;
      }
      case Op::Affine: {
        os << ' ' << n.konst;
        const auto args = tape.args(n);
        const auto coefs = tape.coefs(n);
        for (std::size_t k = 0; k < args.size(); ++k) os << " + " << coefs[k] << "*#" << args[k];
        break;
      }
      default: {
        const int arity = op_arity(n.op);
        if (arity >= 1) os << " #" << n.a;
        if (arity >= 2) os << " #" << n.b;
        if (arity >= 3) os << " #" << n.c;
        break;
      }
    }
    if (n.tainted) os << " [t]";
    os << '\n';
  }
  os << "inputs:";
  for (node_id id : tape.inputs()) os << " #" << id;
  os << "\noutputs:";
  for (node_id id : tape.outputs()) os << " #" << id;
  os << '\n';
}

std::string to_string(const Tape& tape) {
  std::ostringstream os;
  dump(tape, os);
  return os.str();
}

}  // namespace epykos
