// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/load-elimination.h"

#include "src/compiler/node-properties.h"
#include "src/compiler/simplified-operator.h"

namespace v8 {
namespace internal {
namespace compiler {

LoadElimination::~LoadElimination() {}


Reduction LoadElimination::Reduce(Node* node) {
  switch (node->opcode()) {
    case IrOpcode::kLoadField:
      return ReduceLoadField(node);
    default:
      break;
  }
  return NoChange();
}


Reduction LoadElimination::ReduceLoadField(Node* node) {
  DCHECK_EQ(IrOpcode::kLoadField, node->opcode());
  FieldAccess const access = FieldAccessOf(node->op());
  Node* object = NodeProperties::GetValueInput(node, 0);
  for (Node* effect = NodeProperties::GetEffectInput(node);;
       effect = NodeProperties::GetEffectInput(effect)) {
    switch (effect->opcode()) {
      case IrOpcode::kLoadField: {
        if (object == NodeProperties::GetValueInput(effect, 0) &&
            access == FieldAccessOf(effect->op())) {
          Node* const value = effect;
          ReplaceWithValue(node, value);
          return Replace(value);
        }
        break;
      }
      case IrOpcode::kStoreField: {
        if (access == FieldAccessOf(effect->op())) {
          if (object == NodeProperties::GetValueInput(effect, 0)) {
            Node* const value = NodeProperties::GetValueInput(effect, 1);
            ReplaceWithValue(node, value);
            return Replace(value);
          }
          // TODO(turbofan): Alias analysis to the rescue?
          return NoChange();
        }
        break;
      }
      case IrOpcode::kBeginRegion:
      case IrOpcode::kStoreBuffer:
      case IrOpcode::kStoreElement: {
        // These can never interfere with field loads.
        break;
      }
      case IrOpcode::kFinishRegion: {
        // "Look through" FinishRegion nodes to make LoadElimination capable
        // of looking into atomic regions.
        if (object == effect) object = NodeProperties::GetValueInput(effect, 0);
        break;
      }
      case IrOpcode::kAllocate: {
        // Allocations don't interfere with field loads. In case we see the
        // actual allocation for the {object} we can abort.
        if (object == effect) return NoChange();
        break;
      }
      default: {
        if (!effect->op()->HasProperty(Operator::kNoWrite) ||
            effect->op()->EffectInputCount() != 1) {
          return NoChange();
        }
        break;
      }
    }
  }
  UNREACHABLE();
  return NoChange();
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
