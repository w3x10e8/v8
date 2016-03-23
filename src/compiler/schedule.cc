// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/schedule.h"

#include "src/compiler/node.h"
#include "src/compiler/node-properties.h"
#include "src/ostreams.h"

namespace v8 {
namespace internal {
namespace compiler {

BasicBlock::BasicBlock(Zone* zone, Id id)
    : loop_number_(-1),
      rpo_number_(-1),
      deferred_(false),
      dominator_depth_(-1),
      dominator_(nullptr),
      rpo_next_(nullptr),
      loop_header_(nullptr),
      loop_end_(nullptr),
      loop_depth_(0),
      control_(kNone),
      control_input_(nullptr),
      nodes_(zone),
      successors_(zone),
      predecessors_(zone),
      id_(id) {}


bool BasicBlock::LoopContains(BasicBlock* block) const {
  // RPO numbers must be initialized.
  DCHECK(rpo_number_ >= 0);
  DCHECK(block->rpo_number_ >= 0);
  if (loop_end_ == nullptr) return false;  // This is not a loop.
  return block->rpo_number_ >= rpo_number_ &&
         block->rpo_number_ < loop_end_->rpo_number_;
}


void BasicBlock::AddSuccessor(BasicBlock* successor) {
  successors_.push_back(successor);
}


void BasicBlock::AddPredecessor(BasicBlock* predecessor) {
  predecessors_.push_back(predecessor);
}


void BasicBlock::AddNode(Node* node) { nodes_.push_back(node); }


void BasicBlock::set_control(Control control) {
  control_ = control;
}


void BasicBlock::set_control_input(Node* control_input) {
  control_input_ = control_input;
}


void BasicBlock::set_loop_depth(int32_t loop_depth) {
  loop_depth_ = loop_depth;
}


void BasicBlock::set_rpo_number(int32_t rpo_number) {
  rpo_number_ = rpo_number;
}


void BasicBlock::set_loop_end(BasicBlock* loop_end) { loop_end_ = loop_end; }


void BasicBlock::set_loop_header(BasicBlock* loop_header) {
  loop_header_ = loop_header;
}


// static
BasicBlock* BasicBlock::GetCommonDominator(BasicBlock* b1, BasicBlock* b2) {
  while (b1 != b2) {
    if (b1->dominator_depth() < b2->dominator_depth()) {
      b2 = b2->dominator();
    } else {
      b1 = b1->dominator();
    }
  }
  return b1;
}


std::ostream& operator<<(std::ostream& os, const BasicBlock::Control& c) {
  switch (c) {
    case BasicBlock::kNone:
      return os << "none";
    case BasicBlock::kGoto:
      return os << "goto";
    case BasicBlock::kCall:
      return os << "call";
    case BasicBlock::kBranch:
      return os << "branch";
    case BasicBlock::kSwitch:
      return os << "switch";
    case BasicBlock::kDeoptimize:
      return os << "deoptimize";
    case BasicBlock::kTailCall:
      return os << "tailcall";
    case BasicBlock::kReturn:
      return os << "return";
    case BasicBlock::kThrow:
      return os << "throw";
  }
  UNREACHABLE();
  return os;
}


std::ostream& operator<<(std::ostream& os, const BasicBlock::Id& id) {
  return os << id.ToSize();
}


Schedule::Schedule(Zone* zone, size_t node_count_hint)
    : zone_(zone),
      all_blocks_(zone),
      nodeid_to_block_(zone),
      rpo_order_(zone),
      start_(NewBasicBlock()),
      end_(NewBasicBlock()) {
  nodeid_to_block_.reserve(node_count_hint);
}


BasicBlock* Schedule::block(Node* node) const {
  if (node->id() < static_cast<NodeId>(nodeid_to_block_.size())) {
    return nodeid_to_block_[node->id()];
  }
  return nullptr;
}


bool Schedule::IsScheduled(Node* node) {
  if (node->id() >= nodeid_to_block_.size()) return false;
  return nodeid_to_block_[node->id()] != nullptr;
}


BasicBlock* Schedule::GetBlockById(BasicBlock::Id block_id) {
  DCHECK(block_id.ToSize() < all_blocks_.size());
  return all_blocks_[block_id.ToSize()];
}


bool Schedule::SameBasicBlock(Node* a, Node* b) const {
  BasicBlock* block = this->block(a);
  return block != nullptr && block == this->block(b);
}


BasicBlock* Schedule::NewBasicBlock() {
  BasicBlock* block = new (zone_)
      BasicBlock(zone_, BasicBlock::Id::FromSize(all_blocks_.size()));
  all_blocks_.push_back(block);
  return block;
}


void Schedule::PlanNode(BasicBlock* block, Node* node) {
  if (FLAG_trace_turbo_scheduler) {
    OFStream os(stdout);
    os << "Planning #" << node->id() << ":" << node->op()->mnemonic()
       << " for future add to B" << block->id() << "\n";
  }
  DCHECK(this->block(node) == nullptr);
  SetBlockForNode(block, node);
}


void Schedule::AddNode(BasicBlock* block, Node* node) {
  if (FLAG_trace_turbo_scheduler) {
    OFStream os(stdout);
    os << "Adding #" << node->id() << ":" << node->op()->mnemonic() << " to B"
       << block->id() << "\n";
  }
  DCHECK(this->block(node) == nullptr || this->block(node) == block);
  block->AddNode(node);
  SetBlockForNode(block, node);
}


void Schedule::AddGoto(BasicBlock* block, BasicBlock* succ) {
  DCHECK_EQ(BasicBlock::kNone, block->control());
  block->set_control(BasicBlock::kGoto);
  AddSuccessor(block, succ);
}


void Schedule::AddCall(BasicBlock* block, Node* call, BasicBlock* success_block,
                       BasicBlock* exception_block) {
  DCHECK_EQ(BasicBlock::kNone, block->control());
  DCHECK_EQ(IrOpcode::kCall, call->opcode());
  block->set_control(BasicBlock::kCall);
  AddSuccessor(block, success_block);
  AddSuccessor(block, exception_block);
  SetControlInput(block, call);
}


void Schedule::AddBranch(BasicBlock* block, Node* branch, BasicBlock* tblock,
                         BasicBlock* fblock) {
  DCHECK_EQ(BasicBlock::kNone, block->control());
  DCHECK_EQ(IrOpcode::kBranch, branch->opcode());
  block->set_control(BasicBlock::kBranch);
  AddSuccessor(block, tblock);
  AddSuccessor(block, fblock);
  SetControlInput(block, branch);
}


void Schedule::AddSwitch(BasicBlock* block, Node* sw, BasicBlock** succ_blocks,
                         size_t succ_count) {
  DCHECK_EQ(BasicBlock::kNone, block->control());
  DCHECK_EQ(IrOpcode::kSwitch, sw->opcode());
  block->set_control(BasicBlock::kSwitch);
  for (size_t index = 0; index < succ_count; ++index) {
    AddSuccessor(block, succ_blocks[index]);
  }
  SetControlInput(block, sw);
}


void Schedule::AddTailCall(BasicBlock* block, Node* input) {
  DCHECK_EQ(BasicBlock::kNone, block->control());
  block->set_control(BasicBlock::kTailCall);
  SetControlInput(block, input);
  if (block != end()) AddSuccessor(block, end());
}


void Schedule::AddReturn(BasicBlock* block, Node* input) {
  DCHECK_EQ(BasicBlock::kNone, block->control());
  block->set_control(BasicBlock::kReturn);
  SetControlInput(block, input);
  if (block != end()) AddSuccessor(block, end());
}


void Schedule::AddDeoptimize(BasicBlock* block, Node* input) {
  DCHECK_EQ(BasicBlock::kNone, block->control());
  block->set_control(BasicBlock::kDeoptimize);
  SetControlInput(block, input);
  if (block != end()) AddSuccessor(block, end());
}


void Schedule::AddThrow(BasicBlock* block, Node* input) {
  DCHECK_EQ(BasicBlock::kNone, block->control());
  block->set_control(BasicBlock::kThrow);
  SetControlInput(block, input);
  if (block != end()) AddSuccessor(block, end());
}


void Schedule::InsertBranch(BasicBlock* block, BasicBlock* end, Node* branch,
                            BasicBlock* tblock, BasicBlock* fblock) {
  DCHECK_NE(BasicBlock::kNone, block->control());
  DCHECK_EQ(BasicBlock::kNone, end->control());
  end->set_control(block->control());
  block->set_control(BasicBlock::kBranch);
  MoveSuccessors(block, end);
  AddSuccessor(block, tblock);
  AddSuccessor(block, fblock);
  if (block->control_input() != nullptr) {
    SetControlInput(end, block->control_input());
  }
  SetControlInput(block, branch);
}


void Schedule::InsertSwitch(BasicBlock* block, BasicBlock* end, Node* sw,
                            BasicBlock** succ_blocks, size_t succ_count) {
  DCHECK_NE(BasicBlock::kNone, block->control());
  DCHECK_EQ(BasicBlock::kNone, end->control());
  end->set_control(block->control());
  block->set_control(BasicBlock::kSwitch);
  MoveSuccessors(block, end);
  for (size_t index = 0; index < succ_count; ++index) {
    AddSuccessor(block, succ_blocks[index]);
  }
  if (block->control_input() != nullptr) {
    SetControlInput(end, block->control_input());
  }
  SetControlInput(block, sw);
}

void Schedule::EnsureSplitEdgeForm() {
  // Make a copy of all the blocks for the iteration, since adding the split
  // edges will allocate new blocks.
  BasicBlockVector all_blocks_copy(all_blocks_);

  // Insert missing split edge blocks.
  for (auto block : all_blocks_copy) {
    if (block->PredecessorCount() > 1 && block != end_) {
      for (auto current_pred = block->predecessors().begin();
           current_pred != block->predecessors().end(); ++current_pred) {
        BasicBlock* pred = *current_pred;
        if (pred->SuccessorCount() > 1) {
          // Found a predecessor block with multiple successors.
          BasicBlock* split_edge_block = NewBasicBlock();
          split_edge_block->set_control(BasicBlock::kGoto);
          split_edge_block->successors().push_back(block);
          split_edge_block->predecessors().push_back(pred);
          split_edge_block->set_deferred(pred->deferred());
          *current_pred = split_edge_block;
          // Find a corresponding successor in the previous block, replace it
          // with the split edge block... but only do it once, since we only
          // replace the previous blocks in the current block one at a time.
          for (auto successor = pred->successors().begin();
               successor != pred->successors().end(); ++successor) {
            if (*successor == block) {
              *successor = split_edge_block;
              break;
            }
          }
        }
      }
    }
  }
}

void Schedule::PropagateDeferredMark() {
  // Push forward the deferred block marks through newly inserted blocks and
  // other improperly marked blocks until a fixed point is reached.
  // TODO(danno): optimize the propagation
  bool done = false;
  while (!done) {
    done = true;
    for (auto block : all_blocks_) {
      if (!block->deferred()) {
        bool deferred = block->PredecessorCount() > 0;
        for (auto pred : block->predecessors()) {
          if (!pred->deferred()) {
            deferred = false;
          }
        }
        if (deferred) {
          block->set_deferred(true);
          done = false;
        }
      }
    }
  }
}

void Schedule::AddSuccessor(BasicBlock* block, BasicBlock* succ) {
  block->AddSuccessor(succ);
  succ->AddPredecessor(block);
}


void Schedule::MoveSuccessors(BasicBlock* from, BasicBlock* to) {
  for (BasicBlock* const successor : from->successors()) {
    to->AddSuccessor(successor);
    for (BasicBlock*& predecessor : successor->predecessors()) {
      if (predecessor == from) predecessor = to;
    }
  }
  from->ClearSuccessors();
}


void Schedule::SetControlInput(BasicBlock* block, Node* node) {
  block->set_control_input(node);
  SetBlockForNode(block, node);
}


void Schedule::SetBlockForNode(BasicBlock* block, Node* node) {
  if (node->id() >= nodeid_to_block_.size()) {
    nodeid_to_block_.resize(node->id() + 1);
  }
  nodeid_to_block_[node->id()] = block;
}


std::ostream& operator<<(std::ostream& os, const Schedule& s) {
  for (BasicBlock* block :
       ((s.RpoBlockCount() == 0) ? *s.all_blocks() : *s.rpo_order())) {
    if (block->rpo_number() == -1) {
      os << "--- BLOCK B" << block->id().ToInt() << " (block id)";
    } else {
      os << "--- BLOCK B" << block->rpo_number();
    }
    if (block->deferred()) os << " (deferred)";
    if (block->PredecessorCount() != 0) os << " <- ";
    bool comma = false;
    for (BasicBlock const* predecessor : block->predecessors()) {
      if (comma) os << ", ";
      comma = true;
      if (predecessor->rpo_number() == -1) {
        os << "B" << predecessor->id().ToInt();
      } else {
        os << "B" << predecessor->rpo_number();
      }
    }
    os << " ---\n";
    for (Node* node : *block) {
      os << "  " << *node;
      if (NodeProperties::IsTyped(node)) {
        Type* type = NodeProperties::GetType(node);
        os << " : ";
        type->PrintTo(os);
      }
      os << "\n";
    }
    BasicBlock::Control control = block->control();
    if (control != BasicBlock::kNone) {
      os << "  ";
      if (block->control_input() != nullptr) {
        os << *block->control_input();
      } else {
        os << "Goto";
      }
      os << " -> ";
      comma = false;
      for (BasicBlock const* successor : block->successors()) {
        if (comma) os << ", ";
        comma = true;
        if (successor->rpo_number() == -1) {
          os << "B" << successor->id().ToInt();
        } else {
          os << "B" << successor->rpo_number();
        }
      }
      os << "\n";
    }
  }
  return os;
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
