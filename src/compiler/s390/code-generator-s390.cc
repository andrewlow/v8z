// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/code-generator.h"

#include "src/ast/scopes.h"
#include "src/compiler/code-generator-impl.h"
#include "src/compiler/gap-resolver.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/osr.h"
#include "src/s390/macro-assembler-s390.h"

namespace v8 {
namespace internal {
namespace compiler {

#define __ masm()->

#define kScratchReg ip


// Adds S390-specific methods to convert InstructionOperands.
class S390OperandConverter final : public InstructionOperandConverter {
 public:
  S390OperandConverter(CodeGenerator* gen, Instruction* instr)
      : InstructionOperandConverter(gen, instr) {}

  size_t OutputCount() { return instr_->OutputCount(); }

  bool CompareLogical() const {
    switch (instr_->flags_condition()) {
      case kUnsignedLessThan:
      case kUnsignedGreaterThanOrEqual:
      case kUnsignedLessThanOrEqual:
      case kUnsignedGreaterThan:
        return true;
      default:
        return false;
    }
    UNREACHABLE();
    return false;
  }

  Operand InputImmediate(size_t index) {
    Constant constant = ToConstant(instr_->InputAt(index));
    switch (constant.type()) {
      case Constant::kInt32:
        return Operand(constant.ToInt32());
      case Constant::kFloat32:
        return Operand(
            isolate()->factory()->NewNumber(constant.ToFloat32(), TENURED));
      case Constant::kFloat64:
        return Operand(
            isolate()->factory()->NewNumber(constant.ToFloat64(), TENURED));
      case Constant::kInt64:
#if V8_TARGET_ARCH_S390X
        return Operand(constant.ToInt64());
#endif
      case Constant::kExternalReference:
      case Constant::kHeapObject:
      case Constant::kRpoNumber:
        break;
    }
    UNREACHABLE();
    return Operand::Zero();
  }

  MemOperand MemoryOperand(AddressingMode* mode, size_t* first_index) {
    const size_t index = *first_index;
    *mode = AddressingModeField::decode(instr_->opcode());
    switch (*mode) {
      case kMode_None:
        break;
      case kMode_MRI:
        *first_index += 2;
        return MemOperand(InputRegister(index + 0), InputInt32(index + 1));
      case kMode_MRR:
        *first_index += 2;
        return MemOperand(InputRegister(index + 0), InputRegister(index + 1));
    }
    UNREACHABLE();
    return MemOperand(r0);
  }


  MemOperand MemoryOperand(AddressingMode* mode, size_t first_index = 0) {
    return MemoryOperand(mode, &first_index);
  }

  MemOperand ToMemOperand(InstructionOperand* op) const {
    DCHECK_NOT_NULL(op);
    DCHECK(op->IsStackSlot() || op->IsDoubleStackSlot());
    FrameOffset offset = frame_access_state()->GetFrameOffset(
        AllocatedOperand::cast(op)->index());
    return MemOperand(offset.from_stack_pointer() ? sp : fp, offset.offset());
  }
};


static inline bool HasRegisterInput(Instruction* instr, int index) {
  return instr->InputAt(index)->IsRegister();
}


namespace {

class OutOfLineLoadNAN32 final : public OutOfLineCode {
 public:
  OutOfLineLoadNAN32(CodeGenerator* gen, DoubleRegister result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() final {
    __ LoadDoubleLiteral(result_, std::numeric_limits<float>::quiet_NaN(),
                         kScratchReg);
  }

 private:
  DoubleRegister const result_;
};


class OutOfLineLoadNAN64 final : public OutOfLineCode {
 public:
  OutOfLineLoadNAN64(CodeGenerator* gen, DoubleRegister result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() final {
    __ LoadDoubleLiteral(result_, std::numeric_limits<double>::quiet_NaN(),
                         kScratchReg);
  }

 private:
  DoubleRegister const result_;
};


class OutOfLineLoadZero final : public OutOfLineCode {
 public:
  OutOfLineLoadZero(CodeGenerator* gen, Register result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() final { __ LoadImmP(result_, Operand::Zero()); }

 private:
  Register const result_;
};


class OutOfLineRecordWrite final : public OutOfLineCode {
 public:
  OutOfLineRecordWrite(CodeGenerator* gen, Register object, Register offset,
                       Register value, Register scratch0, Register scratch1,
                       RecordWriteMode mode)
      : OutOfLineCode(gen),
        object_(object),
        offset_(offset),
        value_(value),
        scratch0_(scratch0),
        scratch1_(scratch1),
        mode_(mode) {}

  void Generate() final {
    if (mode_ > RecordWriteMode::kValueIsPointer) {
      __ JumpIfSmi(value_, exit());
    }
    if (mode_ > RecordWriteMode::kValueIsMap) {
      __ CheckPageFlag(value_, scratch0_,
                       MemoryChunk::kPointersToHereAreInterestingMask, eq,
                       exit());
    }
    SaveFPRegsMode const save_fp_mode =
        frame()->DidAllocateDoubleRegisters() ? kSaveFPRegs : kDontSaveFPRegs;
    // TODO(turbofan): Once we get frame elision working, we need to save
    // and restore lr properly here if the frame was elided.
    RecordWriteStub stub(isolate(), object_, scratch0_, scratch1_,
                         EMIT_REMEMBERED_SET, save_fp_mode);
    __ AddP(scratch1_, object_, offset_);
    __ CallStub(&stub);
  }

 private:
  Register const object_;
  Register const offset_;
  Register const value_;
  Register const scratch0_;
  Register const scratch1_;
  RecordWriteMode const mode_;
};


Condition FlagsConditionToCondition(FlagsCondition condition, ArchOpcode op) {
  switch (condition) {
    case kEqual:
      return eq;
    case kNotEqual:
      return ne;
    case kSignedLessThan:
    case kUnsignedLessThan:
      return lt;
    case kSignedGreaterThanOrEqual:
    case kUnsignedGreaterThanOrEqual:
      return ge;
    case kSignedLessThanOrEqual:
    case kUnsignedLessThanOrEqual:
      return le;
    case kSignedGreaterThan:
    case kUnsignedGreaterThan:
      return gt;
    case kOverflow:
      // Overflow checked for AddP/SubP only.
      switch (op) {
#if V8_TARGET_ARCH_S390X
        case kS390_Add:
        case kS390_Sub:
          return lt;
#endif
        case kS390_AddWithOverflow32:
        case kS390_SubWithOverflow32:
#if V8_TARGET_ARCH_S390X
          return ne;
#else
          return lt;
#endif
        default:
          break;
      }
      break;
    case kNotOverflow:
      switch (op) {
#if V8_TARGET_ARCH_S390X
        case kS390_Add:
        case kS390_Sub:
          return ge;
#endif
        case kS390_AddWithOverflow32:
        case kS390_SubWithOverflow32:
#if V8_TARGET_ARCH_S390X
          return eq;
#else
          return ge;
#endif
        default:
          break;
      }
      break;
    default:
      break;
  }
  UNREACHABLE();
  return kNoCondition;
}

}  // namespace

#define ASSEMBLE_FLOAT_UNOP(asm_instr)                            \
  do {                                                               \
    __ asm_instr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));\
  } while (0)


#define ASSEMBLE_FLOAT_BINOP(asm_instr)                           \
  do {                                                               \
    __ asm_instr(i.OutputDoubleRegister(), i.InputDoubleRegister(0), \
                 i.InputDoubleRegister(1));                          \
  } while (0)


#define ASSEMBLE_BINOP(asm_instr_reg, asm_instr_imm)        \
  do {                                                         \
    if (HasRegisterInput(instr, 1)) {                          \
      __ asm_instr_reg(i.OutputRegister(), i.InputRegister(0), \
                       i.InputRegister(1));                    \
    } else {                                                   \
      __ asm_instr_imm(i.OutputRegister(), i.InputRegister(0), \
                       i.InputImmediate(1));                   \
    }                                                          \
  } while (0)


#define ASSEMBLE_BINOP_INT(asm_instr_reg, asm_instr_imm)    \
  do {                                                         \
    if (HasRegisterInput(instr, 1)) {                          \
      __ asm_instr_reg(i.OutputRegister(), i.InputRegister(0), \
                       i.InputRegister(1));   \
    } else {                                                   \
      __ asm_instr_imm(i.OutputRegister(), i.InputRegister(0), \
                       i.InputInt32(1));      \
    }                                                          \
  } while (0)


#define ASSEMBLE_ADD_WITH_OVERFLOW()                                    \
  do {                                                                  \
    if (HasRegisterInput(instr, 1)) {                                   \
      __ AddAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0), \
                                i.InputRegister(1), kScratchReg, r0);   \
    } else {                                                            \
      __ AddAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0), \
                                i.InputInt32(1), kScratchReg, r0);      \
    }                                                                   \
  } while (0)


#define ASSEMBLE_SUB_WITH_OVERFLOW()                                    \
  do {                                                                  \
    if (HasRegisterInput(instr, 1)) {                                   \
      __ SubAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0), \
                                i.InputRegister(1), kScratchReg, r0);   \
    } else {                                                            \
      __ AddAndCheckForOverflow(i.OutputRegister(), i.InputRegister(0), \
                                -i.InputInt32(1), kScratchReg, r0);     \
    }                                                                   \
  } while (0)


#if V8_TARGET_ARCH_S390X
#define ASSEMBLE_ADD_WITH_OVERFLOW32()           \
  do {                                           \
    ASSEMBLE_BINOP(AddP, AddP);                   \
    __ TestIfInt32(i.OutputRegister(), r0); \
  } while (0)


#define ASSEMBLE_SUB_WITH_OVERFLOW32()           \
  do {                                           \
    ASSEMBLE_BINOP(SubP, SubP);                   \
    __ TestIfInt32(i.OutputRegister(), r0); \
  } while (0)
#else
#define ASSEMBLE_ADD_WITH_OVERFLOW32 ASSEMBLE_ADD_WITH_OVERFLOW
#define ASSEMBLE_SUB_WITH_OVERFLOW32 ASSEMBLE_SUB_WITH_OVERFLOW
#endif


#define ASSEMBLE_COMPARE(cmp_instr, cmpl_instr)                        \
  do {                                                                 \
    if (HasRegisterInput(instr, 1)) {                                  \
      if (i.CompareLogical()) {                                        \
        __ cmpl_instr(i.InputRegister(0), i.InputRegister(1));         \
      } else {                                                         \
        __ cmp_instr(i.InputRegister(0), i.InputRegister(1));          \
      }                                                                \
    } else {                                                           \
      if (i.CompareLogical()) {                                        \
        __ cmpl_instr(i.InputRegister(0), i.InputImmediate(1));        \
      } else {                                                         \
        __ cmp_instr(i.InputRegister(0), i.InputImmediate(1));         \
      }                                                                \
    }                                                                  \
  } while (0)


#define ASSEMBLE_FLOAT_COMPARE(cmp_instr)                                 \
  do {                                                                    \
    __ cmp_instr(i.InputDoubleRegister(0), i.InputDoubleRegister(1);      \
  } while (0)



// Divide instruction dr will implicity use register pair
// r0 & r1 below.
// R0:R1 = R1 / divisor - R0 remainder
// Copy remainder to output reg
#define ASSEMBLE_MODULO(div_instr, shift_instr)       \
  do {                                                \
  __ LoadRR(r0, i.InputRegister(0));                  \
  __ shift_instr(r0, Operand(32));                    \
  __ div_instr(r0, i.InputRegister(1));               \
  __ ltr(i.OutputRegister(), r0);                     \
  } while (0)


#define ASSEMBLE_FLOAT_MODULO()                                             \
  do {                                                                      \
  FrameScope scope(masm(), StackFrame::MANUAL);                             \
  __ PrepareCallCFunction(0, 2, kScratchReg);                               \
  __ MovToFloatParameters(i.InputDoubleRegister(0),                         \
  i.InputDoubleRegister(1));                                                \
  __ CallCFunction(ExternalReference::mod_two_doubles_operation(isolate()), \
  0, 2);                                                                    \
  __ MovFromFloatResult(i.OutputDoubleRegister());                          \
  } while (0)


#define ASSEMBLE_FLOAT_MAX(double_scratch_reg, general_scratch_reg)           \
  do {                                                                        \
    Label ge, done;                                                           \
    __ cdbr(i.InputDoubleRegister(0), i.InputDoubleRegister(1));              \
    __ bge(&ge, Label::kNear);                                                \
    __ Move(i.OutputDoubleRegister(), i.InputDoubleRegister(1));              \
    __ b(&done, Label::kNear);                                                \
    __ bind(&ge);                                                             \
    __ Move(i.OutputDoubleRegister(), i.InputDoubleRegister(0));              \
    __ bind(&done);                                                           \
  } while (0)


#define ASSEMBLE_FLOAT_MIN(double_scratch_reg, general_scratch_reg)           \
  do {                                                                        \
    Label ge, done;                                                           \
    __ cdbr(i.InputDoubleRegister(0), i.InputDoubleRegister(1));              \
    __ bge(&ge, Label::kNear);                                                \
    __ Move(i.OutputDoubleRegister(), i.InputDoubleRegister(0));              \
    __ b(&done, Label::kNear);                                                \
    __ bind(&ge);                                                             \
    __ Move(i.OutputDoubleRegister(), i.InputDoubleRegister(1));              \
    __ bind(&done);                                                           \
  } while (0)


// Only MRI mode for these instructions available
#define ASSEMBLE_LOAD_FLOAT(asm_instr)                \
  do {                                                \
    DoubleRegister result = i.OutputDoubleRegister(); \
    AddressingMode mode = kMode_None;                 \
    MemOperand operand = i.MemoryOperand(&mode);      \
    __ asm_instr(result, operand);                    \
  } while (0)


#define ASSEMBLE_LOAD_INTEGER(asm_instr)             \
  do {                                               \
    Register result = i.OutputRegister();            \
    AddressingMode mode = kMode_None;                \
    MemOperand operand = i.MemoryOperand(&mode);     \
    __ asm_instr(result, operand);                   \
  } while (0)


#define ASSEMBLE_STORE_FLOAT32()                         \
  do {                                                   \
    size_t index = 0;                                    \
    AddressingMode mode = kMode_None;                    \
    MemOperand operand = i.MemoryOperand(&mode, &index); \
    DoubleRegister value = i.InputDoubleRegister(index); \
    __ StoreDoubleAsFloat32(value, operand, kScratchDoubleReg);             \
  } while (0)


#define ASSEMBLE_STORE_DOUBLE()                          \
  do {                                                   \
    size_t index = 0;                                    \
    AddressingMode mode = kMode_None;                    \
    MemOperand operand = i.MemoryOperand(&mode, &index); \
    DoubleRegister value = i.InputDoubleRegister(index); \
    __ StoreF(value, operand);                           \
  } while (0)


#define ASSEMBLE_STORE_INTEGER(asm_instr)                \
  do {                                                   \
    size_t index = 0;                                    \
    AddressingMode mode = kMode_None;                    \
    MemOperand operand = i.MemoryOperand(&mode, &index); \
    Register value = i.InputRegister(index);             \
    __ asm_instr(value, operand);                        \
  } while (0)


// TODO(Tara): Check this implementation and those of all the
// ASSEMBLE_CHECKED_* methods
// TODO(mbrandy): fix paths that produce garbage in offset's upper 32-bits.
#define ASSEMBLE_CHECKED_LOAD_FLOAT(asm_instr, width)              \
  do {                                                             \
    DoubleRegister result = i.OutputDoubleRegister();              \
    size_t index = 0;                                              \
    AddressingMode mode = kMode_None;                              \
    MemOperand operand = i.MemoryOperand(&mode, index);            \
    Register offset = operand.rb();                                \
    __ lgfr(offset, offset);                                       \
    if (HasRegisterInput(instr, 2)) {                              \
      __ CmpLogical32(offset, i.InputRegister(2));                 \
    } else {                                                       \
      __ CmpLogical32(offset, i.InputImmediate(2));                \
    }                                                              \
    auto ool = new (zone()) OutOfLineLoadNAN##width(this, result); \
    __ bge(ool->entry());                                          \
    __ asm_instr(result, operand);                                 \
    __ bind(ool->exit());                                          \
  } while (0)


// TODO(mbrandy): fix paths that produce garbage in offset's upper 32-bits.
#define ASSEMBLE_CHECKED_LOAD_INTEGER(asm_instr)             \
  do {                                                       \
    Register result = i.OutputRegister();                    \
    size_t index = 0;                                        \
    AddressingMode mode = kMode_None;                        \
    MemOperand operand = i.MemoryOperand(&mode, index);      \
    Register offset = operand.rb();                          \
    __ lgfr(offset, offset);                                 \
    if (HasRegisterInput(instr, 2)) {                        \
      __ CmpLogical32(offset, i.InputRegister(2));           \
    } else {                                                 \
      __ CmpLogical32(offset, i.InputImmediate(2));          \
    }                                                        \
    auto ool = new (zone()) OutOfLineLoadZero(this, result); \
    __ bge(ool->entry());                                    \
    __ asm_instr(result, operand);                           \
    __ bind(ool->exit());                                    \
  } while (0)


// TODO(mbrandy): fix paths that produce garbage in offset's upper 32-bits.
#define ASSEMBLE_CHECKED_STORE_FLOAT32()             \
  do {                                                      \
    Label done;                                             \
    size_t index = 0;                                       \
    AddressingMode mode = kMode_None;                       \
    MemOperand operand = i.MemoryOperand(&mode, index);     \
    Register offset = operand.rb();                         \
    __ lgfr(offset, offset);                                \
    if (HasRegisterInput(instr, 2)) {                       \
      __ CmpLogical32(offset, i.InputRegister(2));          \
    } else {                                                \
      __ CmpLogical32(offset, i.InputImmediate(2));         \
    }                                                       \
    __ bge(&done);                                          \
    DoubleRegister value = i.InputDoubleRegister(3);        \
    __ StoreDoubleAsFloat32(value, operand,                 \
            kScratchDoubleReg);                             \
    __ bind(&done);                                         \
  } while (0)


// TODO(mbrandy): fix paths that produce garbage in offset's upper 32-bits.
#define ASSEMBLE_CHECKED_STORE_DOUBLE()                     \
  do {                                                      \
    Label done;                                             \
    size_t index = 0;                                       \
    AddressingMode mode = kMode_None;                       \
    MemOperand operand = i.MemoryOperand(&mode, index);     \
    DCHECK_EQ(kMode_MRR, mode);                             \
    Register offset = operand.rb();                         \
    __ lgfr(offset, offset);                                \
    if (HasRegisterInput(instr, 2)) {                       \
      __ CmpLogical32(offset, i.InputRegister(2));          \
    } else {                                                \
      __ CmpLogical32(offset, i.InputImmediate(2));         \
    }                                                       \
    __ bge(&done);                                          \
    DoubleRegister value = i.InputDoubleRegister(3);        \
    __ StoreF(value, operand);                              \
    __ bind(&done);                                         \
  } while (0)


// TODO(mbrandy): fix paths that produce garbage in offset's upper 32-bits.
#define ASSEMBLE_CHECKED_STORE_INTEGER(asm_instr)             \
  do {                                                        \
    Label done;                                               \
    size_t index = 0;                                         \
    AddressingMode mode = kMode_None;                         \
    MemOperand operand = i.MemoryOperand(&mode, index);       \
    Register offset = operand.rb();                           \
    __ lgfr(offset, offset);                                  \
    if (HasRegisterInput(instr, 2)) {                         \
      __ CmpLogical32(offset, i.InputRegister(2));            \
    } else {                                                  \
      __ CmpLogical32(offset, i.InputImmediate(2));           \
    }                                                         \
    __ bge(&done);                                            \
    Register value = i.InputRegister(3);                      \
    __ asm_instr(value, operand);                             \
    __ bind(&done);                                           \
  } while (0)


void CodeGenerator::AssembleDeconstructActivationRecord(int stack_param_delta) {
  int sp_slot_delta = TailCallFrameStackSlotDelta(stack_param_delta);
  if (sp_slot_delta > 0) {
    __ AddP(sp, sp, Operand(sp_slot_delta * kPointerSize));
  }
  frame_access_state()->SetFrameAccessToDefault();
}


void CodeGenerator::AssemblePrepareTailCall(int stack_param_delta) {
  int sp_slot_delta = TailCallFrameStackSlotDelta(stack_param_delta);
  if (sp_slot_delta < 0) {
    __ AddP(sp, sp, Operand(sp_slot_delta * kPointerSize));
    frame_access_state()->IncreaseSPDelta(-sp_slot_delta);
  }
  if (frame()->needs_frame()) {
    __ LoadP(r14, MemOperand(fp, StandardFrameConstants::kCallerPCOffset));
    __ LoadP(fp, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  }
  frame_access_state()->SetFrameAccessToSP();
}


// Assembles an instruction after register allocation, producing machine code.
void CodeGenerator::AssembleArchInstruction(Instruction* instr) {
  S390OperandConverter i(this, instr);
  ArchOpcode opcode = ArchOpcodeField::decode(instr->opcode());

  switch (opcode) {
    case kArchCallCodeObject: {
      v8::internal::Assembler::BlockTrampolinePoolScope block_trampoline_pool(
          masm());
      EnsureSpaceForLazyDeopt();
      if (HasRegisterInput(instr, 0)) {
        __ AddP(ip, i.InputRegister(0),
                Operand(Code::kHeaderSize - kHeapObjectTag));
        __ Call(ip);
      } else {
        __ Call(Handle<Code>::cast(i.InputHeapObject(0)),
                RelocInfo::CODE_TARGET);
      }
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchTailCallCodeObject: {
      int stack_param_delta = i.InputInt32(instr->InputCount() - 1);
      AssembleDeconstructActivationRecord(stack_param_delta);
      if (HasRegisterInput(instr, 0)) {
        __ AddP(ip, i.InputRegister(0),
                Operand(Code::kHeaderSize - kHeapObjectTag));
        __ Jump(ip);
      } else {
        // We cannot use the constant pool to load the target since
        // we've already restored the caller's frame.
        ConstantPoolUnavailableScope constant_pool_unavailable(masm());
        __ Jump(Handle<Code>::cast(i.InputHeapObject(0)),
                RelocInfo::CODE_TARGET);
      }
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchCallJSFunction: {
      v8::internal::Assembler::BlockTrampolinePoolScope block_trampoline_pool(
          masm());
      EnsureSpaceForLazyDeopt();
      Register func = i.InputRegister(0);
      if (FLAG_debug_code) {
        // Check the function's context matches the context argument.
        __ LoadP(kScratchReg,
                 FieldMemOperand(func, JSFunction::kContextOffset));
        __ CmpP(cp, kScratchReg);
        __ Assert(eq, kWrongFunctionContext);
      }
      __ LoadP(ip, FieldMemOperand(func, JSFunction::kCodeEntryOffset));
      __ Call(ip);
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchTailCallJSFunction: {
      Register func = i.InputRegister(0);
      if (FLAG_debug_code) {
        // Check the function's context matches the context argument.
        __ LoadP(kScratchReg,
                 FieldMemOperand(func, JSFunction::kContextOffset));
        __ CmpP(cp, kScratchReg);
        __ Assert(eq, kWrongFunctionContext);
      }
      int stack_param_delta = i.InputInt32(instr->InputCount() - 1);
      AssembleDeconstructActivationRecord(stack_param_delta);
      __ LoadP(ip, FieldMemOperand(func, JSFunction::kCodeEntryOffset));
      __ Jump(ip);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchLazyBailout: {
      v8::internal::Assembler::BlockTrampolinePoolScope block_trampoline_pool(
          masm());
      EnsureSpaceForLazyDeopt();
      RecordCallPosition(instr);
      break;
    }
    case kArchPrepareCallCFunction: {
      int const num_parameters = MiscField::decode(instr->opcode());
      __ PrepareCallCFunction(num_parameters, kScratchReg);
      // Frame alignment requires using FP-relative frame addressing.
      frame_access_state()->SetFrameAccessToFP();
      break;
    }
    case kArchPrepareTailCall:
      AssemblePrepareTailCall(i.InputInt32(instr->InputCount() - 1));
      break;
    case kArchCallCFunction: {
      int const num_parameters = MiscField::decode(instr->opcode());
      if (instr->InputAt(0)->IsImmediate()) {
        ExternalReference ref = i.InputExternalReference(0);
        __ CallCFunction(ref, num_parameters);
      } else {
        Register func = i.InputRegister(0);
        __ CallCFunction(func, num_parameters);
      }
      frame_access_state()->SetFrameAccessToDefault();
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchJmp:
      AssembleArchJump(i.InputRpo(0));
      break;
    case kArchLookupSwitch:
      AssembleArchLookupSwitch(instr);
      break;
    case kArchTableSwitch:
      AssembleArchTableSwitch(instr);
      break;
    case kArchNop:
    case kArchThrowTerminator:
      // don't emit code for nops.
      break;
    case kArchDeoptimize: {
      int deopt_state_id =
          BuildTranslation(instr, -1, 0, OutputFrameStateCombine::Ignore());
      Deoptimizer::BailoutType bailout_type =
          Deoptimizer::BailoutType(MiscField::decode(instr->opcode()));
      AssembleDeoptimizerCall(deopt_state_id, bailout_type);
      break;
    }
    case kArchRet:
      AssembleReturn();
      break;
    case kArchStackPointer:
      __ LoadRR(i.OutputRegister(), sp);
      break;
    case kArchFramePointer:
      __ LoadRR(i.OutputRegister(), fp);
      break;
    case kArchTruncateDoubleToI:
      // TODO(mbrandy): move slow call to stub out of line.
      __ TruncateDoubleToI(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kArchStoreWithWriteBarrier: {
      RecordWriteMode mode =
          static_cast<RecordWriteMode>(MiscField::decode(instr->opcode()));
      Register object = i.InputRegister(0);
      Register offset = i.InputRegister(1);
      Register value = i.InputRegister(2);
      Register scratch0 = i.TempRegister(0);
      Register scratch1 = i.TempRegister(1);
      auto ool = new (zone()) OutOfLineRecordWrite(this, object, offset, value,
                                                   scratch0, scratch1, mode);
      __ StoreP(value, MemOperand(object, offset));
      __ CheckPageFlag(object, scratch0,
                       MemoryChunk::kPointersFromHereAreInterestingMask, ne,
                       ool->entry());
      __ bind(ool->exit());
      break;
    }
    case kS390_And:
      ASSEMBLE_BINOP(AndP, AndP);
      break;
    case kS390_AndComplement:
      __ NotP(i.InputRegister(1));
      __ AndP(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      break;
    case kS390_Or:
      ASSEMBLE_BINOP(OrP, OrP);
      break;
    case kS390_OrComplement:
      __ NotP(i.InputRegister(1));
      __ OrP(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      break;
    case kS390_Xor:
      ASSEMBLE_BINOP(XorP, XorP);
      break;
    case kS390_ShiftLeft32:
      if (HasRegisterInput(instr, 1)) {
        if (i.OutputRegister().is(i.InputRegister(1))) {
          __ LoadRR(kScratchReg, i.InputRegister(1));
          __ ShiftLeft(i.OutputRegister(), i.InputRegister(0), kScratchReg);
        } else {
          ASSEMBLE_BINOP(ShiftLeft, ShiftLeft);
        }
      } else {
        ASSEMBLE_BINOP(ShiftLeft, ShiftLeft);
      }
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_ShiftLeft64:
      ASSEMBLE_BINOP(sllg, sllg);
      break;
#endif
    case kS390_ShiftRight32:
      if (HasRegisterInput(instr, 1)) {
          if (i.OutputRegister().is(i.InputRegister(1))) {
            __ LoadRR(kScratchReg, i.InputRegister(1));
            __ ShiftRight(i.OutputRegister(), i.InputRegister(0), kScratchReg);
          } else {
            ASSEMBLE_BINOP(ShiftRight, ShiftRight);
          }
      } else {
        ASSEMBLE_BINOP(ShiftRight, ShiftRight);
      }
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_ShiftRight64:
      ASSEMBLE_BINOP(srlg, srlg);
      break;
#endif
    case kS390_ShiftRightAlg32:
      if (HasRegisterInput(instr, 1)) {
        if (i.OutputRegister().is(i.InputRegister(1))) {
          __ LoadRR(kScratchReg, i.InputRegister(1));
          __ ShiftRightArith(i.OutputRegister(), i.InputRegister(0),
              kScratchReg);
        } else {
          ASSEMBLE_BINOP(ShiftRightArith, ShiftRightArith);
        }
      } else {
        ASSEMBLE_BINOP(ShiftRightArith, ShiftRightArith);
      }
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_ShiftRightAlg64:
      ASSEMBLE_BINOP(srag, srag);
      break;
#endif
    case kS390_RotRight32:
      if (HasRegisterInput(instr, 1)) {
        __ LoadComplementRR(kScratchReg, i.InputRegister(1));
        __ rll(i.OutputRegister(), i.InputRegister(0), kScratchReg);
      } else {
        __ rll(i.OutputRegister(),
            i.InputRegister(0), Operand(32 - i.InputInt32(1)));
        // DCHECK(0);  // Not implemented for now
      }
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_RotRight64:
      if (HasRegisterInput(instr, 1)) {
        __ LoadComplementRR(kScratchReg, i.InputRegister(1));
        __ rll(i.OutputRegister(), i.InputRegister(0), kScratchReg,
               Operand(32));
        __ lgfr(i.OutputRegister(), i.OutputRegister());
      } else {
        UNIMPLEMENTED();  // Not implemented for now
      }
      break;
#endif
    case kS390_Not:
      __ LoadRR(i.OutputRegister(), i.InputRegister(0));
      __ NotP(i.OutputRegister());
      break;
    case kS390_RotLeftAndMask32:
      if (CpuFeatures::IsSupported(GENERAL_INSTR_EXT)) {
        int shiftAmount = i.InputInt32(1);
        int endBit = 63 - i.InputInt32(3);
        int startBit = 63 - i.InputInt32(2);
        __ rll(i.OutputRegister(), i.InputRegister(0), Operand(shiftAmount));
        __ risbg(i.OutputRegister(), i.OutputRegister(), Operand(startBit),
                 Operand(endBit), Operand::Zero(), true);
      } else {
        UNIMPLEMENTED();
      }
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_RotLeftAndClear64:
//      __ rldic(i.OutputRegister(), i.InputRegister(0), i.InputInt32(1),
//               63 - i.InputInt32(2), i.OutputRCBit());
        UNIMPLEMENTED();  // Find correct instruction
      break;
    case kS390_RotLeftAndClearLeft64:
      if (CpuFeatures::IsSupported(GENERAL_INSTR_EXT)) {
        int shiftAmount = i.InputInt32(1);
        int endBit = 63;
        int startBit = 63 - i.InputInt32(2);
        __ risbg(i.OutputRegister(), i.InputRegister(0), Operand(startBit),
            Operand(endBit), Operand(shiftAmount), true);
      } else {
        UNIMPLEMENTED();
      }
      break;
    case kS390_RotLeftAndClearRight64:
      if (CpuFeatures::IsSupported(GENERAL_INSTR_EXT)) {
        int shiftAmount = i.InputInt32(1);
        int endBit = 63 - i.InputInt32(2);
        int startBit = 0;
        __ risbg(i.OutputRegister(), i.InputRegister(0), Operand(startBit),
            Operand(endBit), Operand(shiftAmount), true);
      } else {
        UNIMPLEMENTED();
      }
      break;
#endif
    case kS390_Add:
#if V8_TARGET_ARCH_S390X
      if (FlagsModeField::decode(instr->opcode()) != kFlags_none) {
        ASSEMBLE_ADD_WITH_OVERFLOW();
      } else {
#endif
        ASSEMBLE_BINOP(AddP, AddP);
#if V8_TARGET_ARCH_S390X
      }
#endif
      break;
    case kS390_AddWithOverflow32:
      ASSEMBLE_ADD_WITH_OVERFLOW32();
      break;
    case kS390_AddDouble:
    // Ensure we don't clobber right/InputReg(1)
    if (i.OutputDoubleRegister().is(i.InputDoubleRegister(1))) {
        ASSEMBLE_FLOAT_UNOP(adbr);
    } else {
        if (!i.OutputDoubleRegister().is(i.InputDoubleRegister(0)))
          __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      __ adbr(i.OutputDoubleRegister(), i.InputDoubleRegister(1));
    }
      break;
    case kS390_Sub:
#if V8_TARGET_ARCH_S390X
      if (FlagsModeField::decode(instr->opcode()) != kFlags_none) {
        ASSEMBLE_SUB_WITH_OVERFLOW();
      } else {
#endif
        ASSEMBLE_BINOP(SubP, SubP);
#if V8_TARGET_ARCH_S390X
      }
#endif
      break;
    case kS390_SubWithOverflow32:
      ASSEMBLE_SUB_WITH_OVERFLOW32();
      break;
    case kS390_SubDouble:
    // OutputDoubleReg() = i.InputDoubleRegister(0) - i.InputDoubleRegister(1)
    if (i.OutputDoubleRegister().is(i.InputDoubleRegister(1))) {
        __ ldr(kScratchDoubleReg, i.InputDoubleRegister(1));
        __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
        __ sdbr(i.OutputDoubleRegister(), kScratchDoubleReg);
      } else {
        if (!i.OutputDoubleRegister().is(i.InputDoubleRegister(0))) {
          __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
          }
        __ sdbr(i.OutputDoubleRegister(), i.InputDoubleRegister(1));
      }
      break;
    case kS390_Mul32:
#if V8_TARGET_ARCH_S390X
    case kS390_Mul64:
#endif
      __ Mul(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      break;
    case kS390_MulHigh32:
      __ LoadRR(r1, i.InputRegister(0));
      __ mr_z(r0, i.InputRegister(1));
      __ LoadRR(i.OutputRegister(), r0);
      break;
    case kS390_MulHighU32:
      __ LoadRR(r1, i.InputRegister(0));
      __ mlr(r0, i.InputRegister(1));
      __ LoadRR(i.OutputRegister(), r0);
      break;
    case kS390_MulDouble:
      // Ensure we don't clobber right
      if (i.OutputDoubleRegister().is(i.InputDoubleRegister(1))) {
        ASSEMBLE_FLOAT_UNOP(mdbr);
      } else {
        if (!i.OutputDoubleRegister().is(i.InputDoubleRegister(0)))
          __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
        __ mdbr(i.OutputDoubleRegister(), i.InputDoubleRegister(1));
      }
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Div64:
#endif
    case kS390_Div32:
      __ LoadRR(r0, i.InputRegister(0));
      __ srda(r0, Operand(32));
      __ dr(r0, i.InputRegister(1));
      __ ltr(i.OutputRegister(), r1);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_DivU64:
#endif
    case kS390_DivU32:
      __ LoadRR(r0, i.InputRegister(0));
      __ srdl(r0, Operand(32));
      __ dlr(r0, i.InputRegister(1));   // R0:R1 = R1 / divisor -
      __ ltr(i.OutputRegister(), r1);  // Copy remainder to output reg
      break;

    case kS390_DivDouble:
      // InputDoubleRegister(1)=InputDoubleRegister(0)/InputDoubleRegister(1)
      if (i.OutputDoubleRegister().is(i.InputDoubleRegister(1))) {
      __ ldr(kScratchDoubleReg, i.InputDoubleRegister(1));
      __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      __ ddbr(i.OutputDoubleRegister(), kScratchDoubleReg);
      } else {
      if (!i.OutputDoubleRegister().is(i.InputDoubleRegister(0)))
      __ ldr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      __ ddbr(i.OutputDoubleRegister(), i.InputDoubleRegister(1));
}
      break;
    case kS390_Mod32:
      ASSEMBLE_MODULO(dr, srda);
      break;
    case kS390_ModU32:
      ASSEMBLE_MODULO(dlr, srdl);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Mod64:
      ASSEMBLE_MODULO(dr, srda);
      break;
    case kS390_ModU64:
      ASSEMBLE_MODULO(dlr, srdl);
      break;
#endif
    case kS390_ModDouble:
      ASSEMBLE_FLOAT_MODULO();
      break;
    case kS390_Neg:
      __ LoadComplementRR(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_MaxDouble:
      ASSEMBLE_FLOAT_MAX(kScratchDoubleReg, kScratchReg);
      break;
    case kS390_MinDouble:
      ASSEMBLE_FLOAT_MIN(kScratchDoubleReg, kScratchReg);
      break;
    case kS390_AbsDouble:
      __ lpdbr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_SqrtDouble:
      ASSEMBLE_FLOAT_UNOP(sqdbr);
      break;
    case kS390_FloorDouble:
//      ASSEMBLE_FLOAT_UNOP_RC(frim);
      __ FloatFloor64(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                      kScratchReg);
      break;
    case kS390_CeilDouble:
      __ FloatCeiling64(i.OutputDoubleRegister(),
                        i.InputDoubleRegister(0),
                        kScratchReg);
      break;
    case kS390_TruncateDouble:
      __ fidbra(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
          v8::internal::Assembler::FIDBRA_ROUND_TOWARD_0);
      break;
    case kS390_RoundDouble:
      __ fidbra(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
          v8::internal::Assembler::FIDBRA_ROUND_TO_NEAREST_AWAY_FROM_0);
      break;
    case kS390_NegDouble:
      ASSEMBLE_FLOAT_UNOP(lcdbr);
      break;
    case kS390_Cntlz32:
      {
       Label done;
       __ llgfr(i.OutputRegister(), i.InputRegister(0));
       __ flogr(r0, i.OutputRegister());
       __ LoadRR(i.OutputRegister(), r0);
       __ CmpP(r0, Operand::Zero());
       __ beq(&done, Label::kNear);
       __ SubP(i.OutputRegister(), Operand(32));
       __ bind(&done);
      }
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Cntlz64:
      UNIMPLEMENTED();
      // __ cntlzd_(i.OutputRegister(), i.InputRegister(0));
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
#endif
    case kS390_Popcnt32:
      __ Popcnt32(i.OutputRegister(), i.InputRegister(0));
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Popcnt64:
      UNIMPLEMENTED();
      // __ popcntd(i.OutputRegister(), i.InputRegister(0));
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
#endif
    case kS390_Cmp32:
      ASSEMBLE_COMPARE(Cmp32, CmpLogical32);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Cmp64:
      ASSEMBLE_COMPARE(CmpP, CmpLogicalP);
      break;
#endif
    case kS390_CmpDouble:
      __ cdbr(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      break;
    case kS390_Tst32:
      if (HasRegisterInput(instr, 1)) {
        __ AndP(r0, i.InputRegister(0), i.InputRegister(1));
      } else {
        __ AndP(r0, i.InputRegister(0), i.InputImmediate(1));
      }
#if V8_TARGET_ARCH_S390X
      __ lgfr(r0, r0);
      __ LoadAndTestP(r0, r0);
#endif
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Tst64:
      if (HasRegisterInput(instr, 1)) {
        __ AndP(r0, i.InputRegister(0), i.InputRegister(1));
      } else {
        __ AndP(r0, i.InputRegister(0), i.InputImmediate(1));
      }
      break;
#endif
    case kS390_Push:
      if (instr->InputAt(0)->IsDoubleRegister()) {
        __ StoreF(i.InputDoubleRegister(0), MemOperand(sp, -kDoubleSize));
        __ lay(sp, MemOperand(sp, -kDoubleSize));
        frame_access_state()->IncreaseSPDelta(kDoubleSize / kPointerSize);
      } else {
        __ Push(i.InputRegister(0));
        frame_access_state()->IncreaseSPDelta(1);
      }
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
    case kS390_PushFrame: {
      int num_slots = i.InputInt32(1);
      if (instr->InputAt(0)->IsDoubleRegister()) {
        __ StoreF(i.InputDoubleRegister(0),
                 MemOperand(sp, -num_slots * kPointerSize));
      } else {
        __ StoreP(i.InputRegister(0),
                  MemOperand(sp, -num_slots * kPointerSize));
      }
      __ lay(sp, MemOperand(sp, -num_slots * kPointerSize));
      break;
    }
    case kS390_StoreToStackSlot: {
      int slot = i.InputInt32(1);
      if (instr->InputAt(0)->IsDoubleRegister()) {
        __ StoreF(i.InputDoubleRegister(0),
                  MemOperand(sp, slot * kPointerSize));
      } else {
        __ StoreP(i.InputRegister(0), MemOperand(sp, slot * kPointerSize));
      }
      break;
    }
    case kS390_ExtendSignWord8:
    #if V8_TARGET_ARCH_S390X
      __ lgbr(i.OutputRegister(), i.InputRegister(0));
    #else
      __ lbr(i.OutputRegister(), i.InputRegister(0));
    #endif
      break;
    case kS390_ExtendSignWord16:
    #if V8_TARGET_ARCH_S390X
      __ lghr(i.OutputRegister(), i.InputRegister(0));
    #else
      __ lhr(i.OutputRegister(), i.InputRegister(0));
    #endif
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_ExtendSignWord32:
      __ lgfr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_Uint32ToUint64:
      // Zero extend
      __ llgfr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_Int64ToInt32:
      // sign extend
      __ lgfr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_Int64ToFloat32:
      __ ConvertInt64ToFloat(i.InputRegister(0), i.OutputDoubleRegister());
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
    case kS390_Int64ToDouble:
      __ ConvertInt64ToDouble(i.InputRegister(0), i.OutputDoubleRegister());
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
    case kS390_Uint64ToFloat32:
      __ ConvertUnsignedInt64ToFloat(i.InputRegister(0),
                                     i.OutputDoubleRegister());
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
    case kS390_Uint64ToDouble:
      __ ConvertUnsignedInt64ToDouble(i.InputRegister(0),
                                      i.OutputDoubleRegister());
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
#endif
    case kS390_Int32ToFloat32:
      __ ConvertIntToFloat(i.OutputDoubleRegister(), i.InputRegister(0),
                           kScratchReg);
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
    case kS390_Int32ToDouble:
      __ ConvertIntToDouble(i.InputRegister(0), i.OutputDoubleRegister());
      break;
    case kS390_Uint32ToDouble:
      __ ConvertUnsignedIntToDouble(i.InputRegister(0),
                                    i.OutputDoubleRegister());
      break;
    case kS390_DoubleToInt32:
    case kS390_DoubleToUint32:
    case kS390_DoubleToInt64: {
#if V8_TARGET_ARCH_S390X
      bool check_conversion =
          (opcode == kS390_DoubleToInt64 && i.OutputCount() > 1);
      if (check_conversion) {
        UNIMPLEMENTED();
        // __ mtfsb0(VXCVI);  // clear FPSCR:VXCVI bit
      }
#endif
      __ ConvertDoubleToInt64(i.InputDoubleRegister(0),
#if !V8_TARGET_ARCH_S390X
                              kScratchReg,
#endif
                              i.OutputRegister(0), kScratchDoubleReg);
#if V8_TARGET_ARCH_S390X
      if (check_conversion) {
        UNIMPLEMENTED();
        // Set 2nd output to zero if conversion fails.
        // CRBit crbit = static_cast<CRBit>(VXCVI % CRWIDTH);
        // __ mcrfs(cr7, VXCVI);// extract FPSCR field containing VXCVI into cr7
        // __ LoadImmP(i.OutputRegister(1), Operand(1));
        // __ isel(i.OutputRegister(1), r0, i.OutputRegister(1),
        //         v8::internal::Assembler::encode_crbit(cr7, crbit));
      }
#endif
      break;
    }
#if V8_TARGET_ARCH_S390X
    case kS390_DoubleToUint64: {
      bool check_conversion = (i.OutputCount() > 1);
      if (check_conversion) {
        UNIMPLEMENTED();
        // Set 2nd output to zero if conversion fails.
        // We must check explicitly for negative values here since the
        // conversion instruction rounds the input toward zero before
        // checking for validity (otherwise, values between -1 and 0
        // would produce incorrect results).
        // __ LoadImmP(i.OutputRegister(1), Operand::Zero());
        // __ fcmpu(i.InputDoubleRegister(0), kDoubleRegZero);
        // __ blt(&done);
        // __ mtfsb0(VXCVI);  // clear FPSCR:VXCVI bit
      }
      UNIMPLEMENTED();
      __ ConvertDoubleToUnsignedInt64(i.InputDoubleRegister(0),
                                      i.OutputRegister(0), kScratchDoubleReg);
      if (check_conversion) {
        UNIMPLEMENTED();
        // CRBit crbit = static_cast<CRBit>(VXCVI % CRWIDTH);
        // __ mcrfs(cr7, VXCVI);// extract FPSCR field containing VXCVI into cr7
        // __ LoadImmP(i.OutputRegister(1), Operand(1));
        // __ isel(i.OutputRegister(1), r0, i.OutputRegister(1),
        //         v8::internal::Assembler::encode_crbit(cr7, crbit));
      }
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
    }
#endif
    case kS390_DoubleToFloat32:
      __ ledbr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      __ ldebr(i.OutputDoubleRegister(), i.OutputDoubleRegister());
      break;
    case kS390_Float32ToDouble:
      // Nothing to do.
      __ Move(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_DoubleExtractLowWord32:
     // __ MovDoubleLowToInt(i.OutputRegister(), i.InputDoubleRegister(0));
      __ stdy(i.InputDoubleRegister(0), MemOperand(sp, -kDoubleSize));
      __ LoadlW(i.OutputRegister(),
                  MemOperand(sp, -kDoubleSize + Register::kMantissaOffset));
      break;
    case kS390_DoubleExtractHighWord32:
     // __ MovDoubleHighToInt(i.OutputRegister(), i.InputDoubleRegister(0));
      __ stdy(i.InputDoubleRegister(0), MemOperand(sp, -kDoubleSize));
      __ LoadlW(i.OutputRegister(),
                 MemOperand(sp, -kDoubleSize + Register::kExponentOffset));
      break;
    case kS390_DoubleInsertLowWord32:
      __ InsertDoubleLow(i.OutputDoubleRegister(), i.InputRegister(1));
      break;
    case kS390_DoubleInsertHighWord32:
      __ InsertDoubleHigh(i.OutputDoubleRegister(), i.InputRegister(1));
      break;
    case kS390_DoubleConstruct:
    // TODO(Tara): Use ldgr
    #if V8_TARGET_LITTLE_ENDIAN
      __ StoreW(i.InputRegister(0), MemOperand(sp, -kDoubleSize / 2));
      __ StoreW(i.InputRegister(1), MemOperand(sp, -kDoubleSize));
    #else
      __ StoreW(i.InputRegister(1), MemOperand(sp, -kDoubleSize / 2));
      __ StoreW(i.InputRegister(0), MemOperand(sp, -kDoubleSize));
    #endif
      __ ldy(i.OutputDoubleRegister(), MemOperand(sp, -kDoubleSize));
      break;
    case kS390_LoadWordS8:
      ASSEMBLE_LOAD_INTEGER(LoadlB);
#if V8_TARGET_ARCH_S390X
      __ lgbr(i.OutputRegister(), i.OutputRegister());
#else
      __ lbr(i.OutputRegister(), i.OutputRegister());
#endif
      // DCHECK_EQ(LeaveRC, i.OutputRCBit());
      break;
    case kS390_BitcastFloat32ToInt32:
      __ MovFloatToInt(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_BitcastInt32ToFloat32:
      __ MovIntToFloat(i.OutputDoubleRegister(), i.InputRegister(0));
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_BitcastDoubleToInt64:
      __ MovDoubleToInt64(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_BitcastInt64ToDouble:
      __ MovInt64ToDouble(i.OutputDoubleRegister(), i.InputRegister(0));
      break;
#endif
    case kS390_LoadWordU8:
      ASSEMBLE_LOAD_INTEGER(LoadlB);
      break;
    case kS390_LoadWordU16:
      ASSEMBLE_LOAD_INTEGER(LoadLogicalHalfWordP);
      break;
    case kS390_LoadWordS16:
      ASSEMBLE_LOAD_INTEGER(LoadHalfWordP);
      break;
    case kS390_LoadWordS32:
      ASSEMBLE_LOAD_INTEGER(LoadW);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_LoadWord64:
      ASSEMBLE_LOAD_INTEGER(lg);
      break;
#endif
    case kS390_LoadFloat32:
      ASSEMBLE_LOAD_FLOAT(LoadShortConvertToDoubleF);
      break;
    case kS390_LoadDouble:
      ASSEMBLE_LOAD_FLOAT(LoadF);
      break;
    case kS390_StoreWord8:
      ASSEMBLE_STORE_INTEGER(StoreByte);
      break;
    case kS390_StoreWord16:
      ASSEMBLE_STORE_INTEGER(StoreHalfWord);
      break;
    case kS390_StoreWord32:
      ASSEMBLE_STORE_INTEGER(StoreW);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_StoreWord64:
      ASSEMBLE_STORE_INTEGER(StoreP);
      break;
#endif
    case kS390_StoreFloat32:
      ASSEMBLE_STORE_FLOAT32();
      break;
    case kS390_StoreDouble:
      ASSEMBLE_STORE_DOUBLE();
      break;
    case kCheckedLoadInt8:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadlB);
#if V8_TARGET_ARCH_S390X
      __ lgbr(i.OutputRegister(), i.OutputRegister());
#else
      __ lbr(i.OutputRegister(), i.OutputRegister());
#endif
      break;
    case kCheckedLoadUint8:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadlB);
      break;
    case kCheckedLoadInt16:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadHalfWordP);
      break;
    case kCheckedLoadUint16:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadLogicalHalfWordP);
      break;
    case kCheckedLoadWord32:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadW);
      break;
    case kCheckedLoadWord64:
#if V8_TARGET_ARCH_S390X
      // TODO(all): port for s390
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadP);
#else
      UNREACHABLE();
#endif
      break;
    case kCheckedLoadFloat32:
      ASSEMBLE_CHECKED_LOAD_FLOAT(LoadShortConvertToDoubleF, 32);
      break;
    case kCheckedLoadFloat64:
      ASSEMBLE_CHECKED_LOAD_FLOAT(LoadF, 64);
      break;
    case kCheckedStoreWord8:
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreByte);
      break;
    case kCheckedStoreWord16:
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreHalfWord);
      break;
    case kCheckedStoreWord32:
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreW);
      break;
    case kCheckedStoreWord64:
#if V8_TARGET_ARCH_S390X
      // TODO(all): port for s390
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreP);
#else
      UNREACHABLE();
#endif
      break;
    case kCheckedStoreFloat32:
      ASSEMBLE_CHECKED_STORE_FLOAT32();
      break;
    case kCheckedStoreFloat64:
      ASSEMBLE_CHECKED_STORE_DOUBLE();
      break;
    default:
      UNREACHABLE();
      break;
  }
}


// Assembles branches after an instruction.
void CodeGenerator::AssembleArchBranch(Instruction* instr, BranchInfo* branch) {
  S390OperandConverter i(this, instr);
  Label* tlabel = branch->true_label;
  Label* flabel = branch->false_label;
  ArchOpcode op = instr->arch_opcode();
  FlagsCondition condition = branch->condition;

  Condition cond = FlagsConditionToCondition(condition, op);
  if (op == kS390_CmpDouble) {
    // check for unordered if necessary
    // Branching to flabel/tlabel according to what's expected by tests
    if (cond == le || cond == eq || cond == lt) {
      __ bunordered(flabel);
    } else if (cond == gt || cond == ne || cond == ge) {
      __ bunordered(tlabel);
    }
  }
  __ b(cond, tlabel);
  if (!branch->fallthru) __ b(flabel);  // no fallthru to flabel.
}


void CodeGenerator::AssembleArchJump(RpoNumber target) {
  if (!IsNextInAssemblyOrder(target)) __ b(GetLabel(target));
}


// Assembles boolean materializations after an instruction.
void CodeGenerator::AssembleArchBoolean(Instruction* instr,
                                        FlagsCondition condition) {
  S390OperandConverter i(this, instr);
  Label done;
  ArchOpcode op = instr->arch_opcode();
  bool check_unordered = (op == kS390_CmpDouble);

  // Overflow checked for add/sub only.
  DCHECK((condition != kOverflow && condition != kNotOverflow) ||
         (op == kS390_AddWithOverflow32 || op == kS390_SubWithOverflow32));

  // Materialize a full 32-bit 1 or 0 value. The result register is always the
  // last output of the instruction.
  DCHECK_NE(0u, instr->OutputCount());
  Register reg = i.OutputRegister(instr->OutputCount() - 1);
  Condition cond = FlagsConditionToCondition(condition, op);
  switch (cond) {
    case ne:
    case ge:
    case gt:
      if (check_unordered) {
        __ LoadImmP(reg, Operand(1));
        __ LoadImmP(kScratchReg, Operand::Zero());
        __ bunordered(&done);
        Label cond_true;
        __ b(cond, &cond_true, Label::kNear);
        __ LoadRR(reg, kScratchReg);
        __ bind(&cond_true);
      } else {
      Label cond_true, done_here;
        __ LoadImmP(reg, Operand(1));
        __ b(cond, &cond_true, Label::kNear);
        __ LoadImmP(reg, Operand::Zero());
        __ bind(&cond_true);
      }
      break;
    case eq:
    case lt:
    case le:
      if (check_unordered) {
        __ LoadImmP(reg, Operand::Zero());
        __ LoadImmP(kScratchReg, Operand(1));
        __ bunordered(&done);
        Label cond_false;
        __ b(NegateCondition(cond), &cond_false, Label::kNear);
        __ LoadRR(reg, kScratchReg);
        __ bind(&cond_false);
      } else {
        __ LoadImmP(reg, Operand::Zero());
        Label cond_false;
        __ b(NegateCondition(cond), &cond_false, Label::kNear);
        __ LoadImmP(reg, Operand(1));
        __ bind(&cond_false);
      }
      break;
    default:
      UNREACHABLE();
      break;
  }
  __ bind(&done);
}


void CodeGenerator::AssembleArchLookupSwitch(Instruction* instr) {
  S390OperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  for (size_t index = 2; index < instr->InputCount(); index += 2) {
    __ CmpP(input, Operand(i.InputInt32(index + 0)));
    __ beq(GetLabel(i.InputRpo(index + 1)));
  }
  AssembleArchJump(i.InputRpo(1));
}


void CodeGenerator::AssembleArchTableSwitch(Instruction* instr) {
  S390OperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  int32_t const case_count = static_cast<int32_t>(instr->InputCount() - 2);
  Label** cases = zone()->NewArray<Label*>(case_count);
  for (int32_t index = 0; index < case_count; ++index) {
    cases[index] = GetLabel(i.InputRpo(index + 2));
  }
  Label* const table = AddJumpTable(cases, case_count);
  __ CmpLogicalP(input, Operand(case_count));
  __ bge(GetLabel(i.InputRpo(1)));
  __ larl(kScratchReg, table);
  __ ShiftLeftP(r1, input, Operand(kPointerSizeLog2));
  __ LoadP(kScratchReg, MemOperand(kScratchReg, r1));
  __ Jump(kScratchReg);
}


void CodeGenerator::AssembleDeoptimizerCall(
    int deoptimization_id, Deoptimizer::BailoutType bailout_type) {
  Address deopt_entry = Deoptimizer::GetDeoptimizationEntry(
      isolate(), deoptimization_id, bailout_type);
  __ Call(deopt_entry, RelocInfo::RUNTIME_ENTRY);
}


void CodeGenerator::AssemblePrologue() {
  CallDescriptor* descriptor = linkage()->GetIncomingDescriptor();

  if (descriptor->IsCFunctionCall()) {
    __ Push(r14, fp);
    __ LoadRR(fp, sp);
  } else if (descriptor->IsJSFunctionCall()) {
    __ Prologue(this->info()->GeneratePreagedPrologue(), ip);
  } else if (frame()->needs_frame()) {
    if (!ABI_CALL_VIA_IP && info()->output_code_kind() == Code::WASM_FUNCTION) {
      // TODO(mbrandy): Restrict only to the wasm wrapper case.
      __ StubPrologue();
    } else {
      __ StubPrologue(ip);
    }
  } else {
    frame()->SetElidedFrameSizeInSlots(0);
  }
  frame_access_state()->SetFrameAccessToDefault();

  int stack_shrink_slots = frame()->GetSpillSlotCount();
  if (info()->is_osr()) {
    // TurboFan OSR-compiled functions cannot be entered directly.
    __ Abort(kShouldNotDirectlyEnterOsrFunction);

    // Unoptimized code jumps directly to this entrypoint while the unoptimized
    // frame is still on the stack. Optimized code uses OSR values directly from
    // the unoptimized frame. Thus, all that needs to be done is to allocate the
    // remaining stack slots.
    if (FLAG_code_comments) __ RecordComment("-- OSR entrypoint --");
    osr_pc_offset_ = __ pc_offset();
    // TODO(titzer): cannot address target function == local #-1
    __ LoadP(r3, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
    stack_shrink_slots -= OsrHelper(info()).UnoptimizedFrameSlots();
  }

  const RegList double_saves = descriptor->CalleeSavedFPRegisters();
  if (double_saves != 0) {
    stack_shrink_slots += frame()->AlignSavedCalleeRegisterSlots();
  }
  if (stack_shrink_slots > 0) {
    __ lay(sp, MemOperand(sp, -stack_shrink_slots * kPointerSize));
  }

  // Save callee-saved Double registers.
  if (double_saves != 0) {
    __ MultiPushDoubles(double_saves);
    DCHECK(kNumCalleeSavedDoubles ==
           base::bits::CountPopulation32(double_saves));
    frame()->AllocateSavedCalleeRegisterSlots(kNumCalleeSavedDoubles *
                                              (kDoubleSize / kPointerSize));
  }

  // Save callee-saved registers.
  const RegList saves = descriptor->CalleeSavedRegisters();
  if (saves != 0) {
    __ MultiPush(saves);
    // register save area does not include the fp or constant pool pointer.
    const int num_saves =
        kNumCalleeSaved - 1 - (FLAG_enable_embedded_constant_pool ? 1 : 0);
    DCHECK(num_saves == base::bits::CountPopulation32(saves));
    frame()->AllocateSavedCalleeRegisterSlots(num_saves);
  }
}


void CodeGenerator::AssembleReturn() {
  CallDescriptor* descriptor = linkage()->GetIncomingDescriptor();
  int pop_count = static_cast<int>(descriptor->StackParameterCount());

  // Restore registers.
  const RegList saves = descriptor->CalleeSavedRegisters();
  if (saves != 0) {
    __ MultiPop(saves);
  }

  // Restore double registers.
  const RegList double_saves = descriptor->CalleeSavedFPRegisters();
  if (double_saves != 0) {
    __ MultiPopDoubles(double_saves);
  }

  if (descriptor->IsCFunctionCall()) {
    __ LeaveFrame(StackFrame::MANUAL, pop_count * kPointerSize);
  } else if (frame()->needs_frame()) {
    // Canonicalize JSFunction return sites for now.
    if (return_label_.is_bound()) {
      __ b(&return_label_);
      return;
    } else {
      __ bind(&return_label_);
      __ LeaveFrame(StackFrame::MANUAL, pop_count * kPointerSize);
    }
  } else {
    __ Drop(pop_count);
  }
  __ Ret();
}


void CodeGenerator::AssembleMove(InstructionOperand* source,
                                 InstructionOperand* destination) {
  S390OperandConverter g(this, nullptr);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      __ Move(g.ToRegister(destination), src);
    } else {
      __ StoreP(src, g.ToMemOperand(destination));
    }
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    MemOperand src = g.ToMemOperand(source);
    if (destination->IsRegister()) {
      __ LoadP(g.ToRegister(destination), src);
    } else {
      Register temp = kScratchReg;
      __ LoadP(temp, src, r0);
      __ StoreP(temp, g.ToMemOperand(destination));
    }
  } else if (source->IsConstant()) {
    Constant src = g.ToConstant(source);
    if (destination->IsRegister() || destination->IsStackSlot()) {
      Register dst =
          destination->IsRegister() ? g.ToRegister(destination) : kScratchReg;
      switch (src.type()) {
        case Constant::kInt32:
          __ mov(dst, Operand(src.ToInt32()));
          break;
        case Constant::kInt64:
          __ mov(dst, Operand(src.ToInt64()));
          break;
        case Constant::kFloat32:
          __ Move(dst,
                  isolate()->factory()->NewNumber(src.ToFloat32(), TENURED));
          break;
        case Constant::kFloat64:
          __ Move(dst,
                  isolate()->factory()->NewNumber(src.ToFloat64(), TENURED));
          break;
        case Constant::kExternalReference:
          __ mov(dst, Operand(src.ToExternalReference()));
          break;
        case Constant::kHeapObject: {
          Handle<HeapObject> src_object = src.ToHeapObject();
          Heap::RootListIndex index;
          int offset;
          if (IsMaterializableFromFrame(src_object, &offset)) {
            __ LoadP(dst, MemOperand(fp, offset));
          } else if (IsMaterializableFromRoot(src_object, &index)) {
            __ LoadRoot(dst, index);
          } else {
            __ Move(dst, src_object);
          }
          break;
        }
        case Constant::kRpoNumber:
          UNREACHABLE();  // TODO(dcarney): loading RPO constants on PPC.
          break;
      }
      if (destination->IsStackSlot()) {
        __ StoreP(dst, g.ToMemOperand(destination), r0);
      }
    } else {
      DoubleRegister dst = destination->IsDoubleRegister()
                               ? g.ToDoubleRegister(destination)
                               : kScratchDoubleReg;
      double value = (src.type() == Constant::kFloat32) ? src.ToFloat32()
                                                        : src.ToFloat64();
      __ LoadDoubleLiteral(dst, value, kScratchReg);
      if (destination->IsDoubleStackSlot()) {
        __ StoreF(dst, g.ToMemOperand(destination));
      }
    }
  } else if (source->IsDoubleRegister()) {
    DoubleRegister src = g.ToDoubleRegister(source);
    if (destination->IsDoubleRegister()) {
      DoubleRegister dst = g.ToDoubleRegister(destination);
      __ Move(dst, src);
    } else {
      DCHECK(destination->IsDoubleStackSlot());
      __ StoreF(src, g.ToMemOperand(destination));
    }
  } else if (source->IsDoubleStackSlot()) {
    DCHECK(destination->IsDoubleRegister() || destination->IsDoubleStackSlot());
    MemOperand src = g.ToMemOperand(source);
    if (destination->IsDoubleRegister()) {
      __ LoadF(g.ToDoubleRegister(destination), src);
    } else {
      DoubleRegister temp = kScratchDoubleReg;
      __ LoadF(temp, src);
      __ StoreF(temp, g.ToMemOperand(destination));
    }
  } else {
    UNREACHABLE();
  }
}


void CodeGenerator::AssembleSwap(InstructionOperand* source,
                                 InstructionOperand* destination) {
  S390OperandConverter g(this, nullptr);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    // Register-register.
    Register temp = kScratchReg;
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      Register dst = g.ToRegister(destination);
      __ LoadRR(temp, src);
      __ LoadRR(src, dst);
      __ LoadRR(dst, temp);
    } else {
      DCHECK(destination->IsStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      __ LoadRR(temp, src);
      __ LoadP(src, dst);
      __ StoreP(temp, dst);
    }
#if V8_TARGET_ARCH_S390X
  } else if (source->IsStackSlot() || source->IsDoubleStackSlot()) {
#else
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsStackSlot());
#endif
    Register temp_0 = kScratchReg;
    Register temp_1 = r0;
    MemOperand src = g.ToMemOperand(source);
    MemOperand dst = g.ToMemOperand(destination);
    __ LoadP(temp_0, src);
    __ LoadP(temp_1, dst);
    __ StoreP(temp_0, dst);
    __ StoreP(temp_1, src);
  } else if (source->IsDoubleRegister()) {
    DoubleRegister temp = kScratchDoubleReg;
    DoubleRegister src = g.ToDoubleRegister(source);
    if (destination->IsDoubleRegister()) {
      DoubleRegister dst = g.ToDoubleRegister(destination);
      __ ldr(temp, src);
      __ ldr(src, dst);
      __ ldr(dst, temp);
    } else {
      DCHECK(destination->IsDoubleStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      __ ldr(temp, src);
      __ LoadF(src, dst);
      __ StoreF(temp, dst);
    }
#if !V8_TARGET_ARCH_S390X
  } else if (source->IsDoubleStackSlot()) {
    DCHECK(destination->IsDoubleStackSlot());
    DoubleRegister temp_0 = kScratchDoubleReg;
    DoubleRegister temp_1 = d0;
    MemOperand src = g.ToMemOperand(source);
    MemOperand dst = g.ToMemOperand(destination);
    __ LoadF(temp_0, src);
    __ LoadF(temp_1, dst);
    __ StoreF(temp_0, dst);
    __ StoreF(temp_1, src);
#endif
  } else {
    // No other combinations are possible.
    UNREACHABLE();
  }
}


void CodeGenerator::AssembleJumpTable(Label** targets, size_t target_count) {
  for (size_t index = 0; index < target_count; ++index) {
    __ emit_label_addr(targets[index]);
  }
}


void CodeGenerator::AddNopForSmiCodeInlining() {
  // We do not insert nops for inlined Smi code.
}


void CodeGenerator::EnsureSpaceForLazyDeopt() {
  if (!info()->ShouldEnsureSpaceForLazyDeopt()) {
    return;
  }

  int space_needed = Deoptimizer::patch_size();
  // Ensure that we have enough space after the previous lazy-bailout
  // instruction for patching the code here.
  int current_pc = masm()->pc_offset();
  if (current_pc < last_lazy_deopt_pc_ + space_needed) {
    // Block tramoline pool emission for duration of padding.
    v8::internal::Assembler::BlockTrampolinePoolScope block_trampoline_pool(
        masm());
    int padding_size = last_lazy_deopt_pc_ + space_needed - current_pc;
    DCHECK_EQ(0, padding_size % 2);
    while (padding_size > 0) {
      __ nop();
      padding_size -= 2;
    }
  }
}

#undef __

}  // namespace compiler
}  // namespace internal
}  // namespace v8
