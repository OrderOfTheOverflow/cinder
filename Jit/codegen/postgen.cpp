#include "Jit/codegen/postgen.h"

using namespace jit::lir;

namespace jit::codegen {

Rewrite::RewriteResult PostGenerationRewrite::rewriteBinaryOpConstantPosition(
    instr_iter_t instr_iter) {
  auto instr = instr_iter->get();

  if (!instr->isAdd() && !instr->isSub() && !instr->isXor() &&
      !instr->isAnd() && !instr->isOr() && !instr->isMul() &&
      !instr->isCompare()) {
    return kUnchanged;
  }

  bool is_commutative = !instr->isSub();
  auto input0 = instr->getInput(0);
  auto input1 = instr->getInput(1);

  if (input0->type() != OperandBase::kImm) {
    return kUnchanged;
  }

  if (is_commutative && input1->type() != OperandBase::kImm) {
    // if the operation is commutative and the second input is not also an
    // immediate, just swap the operands
    if (instr->isCompare()) {
      instr->setOpcode(Instruction::flipComparisonDirection(instr->opcode()));
    }
    auto imm = instr->removeInputOperand(0);
    instr->appendInputOperand(std::move(imm));
    return kChanged;
  }

  // otherwise, need to insert a move instruction
  auto constant = input0->getConstant();
  auto constant_size = input0->dataType();

  auto block = instr->basicblock();
  auto move = block->allocateInstrBefore(
      instr_iter, Instruction::kMove, OutVReg(), Imm(constant, constant_size));

  instr->allocateLinkedInput(move);
  auto new_input = instr->removeInputOperand(instr->getNumInputs() - 1);
  instr->replaceInputOperand(0, std::move(new_input));

  return kChanged;
}

Rewrite::RewriteResult PostGenerationRewrite::rewriteBinaryOpLargeConstant(
    instr_iter_t instr_iter) {
  // rewrite
  //     Vreg2 = BinOp Vreg1, Imm64
  // to
  //     Vreg0 = Mov Imm64
  //     Vreg2 = BinOP Vreg1, VReg0

  auto instr = instr_iter->get();

  if (!instr->isAdd() && !instr->isSub() && !instr->isXor() &&
      !instr->isAnd() && !instr->isOr() && !instr->isMul() &&
      !instr->isCompare()) {
    return kUnchanged;
  }

  // if first operand is an immediate, we need to swap the operands
  if (instr->getInput(0)->type() == OperandBase::kImm) {
    // another rewrite will fix this later
    return kUnchanged;
  }

  JIT_CHECK(
      instr->getInput(0)->type() != OperandBase::kImm,
      "The first input operand of a binary op instruction should not be "
      "constant");

  auto in1 = instr->getInput(1);
  if (in1->type() != OperandBase::kImm || in1->sizeInBits() < 64) {
    return kUnchanged;
  }

  auto constant = in1->getConstant();

  if (fitsInt32(constant)) {
    return kUnchanged;
  }

  auto block = instr->basicblock();
  auto move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg(),
      Imm(constant, in1->dataType()));

  // remove the constant input
  instr->setNumInputs(instr->getNumInputs() - 1);
  instr->allocateLinkedInput(move);
  return kChanged;
}

Rewrite::RewriteResult PostGenerationRewrite::rewriteCondBranch(
    instr_iter_t instr_iter) {
  // find the pattern like
  // %3 = Compare<cc> %1, %2
  // CondBranch %3, ...
  // In this case, we don't need to generate a separate register for %3.
  // We can prevent this happening by removing the output of the first
  // instruction and the input of the second.
  // Assumptions for now, which can be lifted in the future:
  //   1. the output of Compare instruction should only be used by CondBranch
  // instruction;

  auto instr = instr_iter->get();
  if (!instr->isCondBranch()) {
    return kUnchanged;
  }

  auto cond = instr->getInput(0);
  if (!cond->isLinked() || cond->type() == OperandBase::kNone) {
    return kUnchanged;
  }

  Instruction* flag_affecting_instr = findRecentFlagAffectingInstr(instr_iter);

  if (flag_affecting_instr == nullptr) {
    return kUnchanged;
  }

  if (!flag_affecting_instr->isCompare()) {
    return kUnchanged;
  }

  // here, we can assume that the sole purpose of a compare instruction is to
  // generate the condition operand for a following conditional branch. There is
  // no other instruction in the current LIR that is able to consume the output
  // of compare instructions.
  JIT_CHECK(
      static_cast<LinkedOperand*>(cond)->getLinkedInstr() ==
          flag_affecting_instr,
      "The output of a Compare instruction is not used by a CondBranch "
      "instruction.");

  // Setting the output to None is effectively removing the output of
  // flag_affecting_instr and all the input operands that linked to it.
  // As a result, no register will be allocated for this operand.
  flag_affecting_instr->output()->setNone();
  return kChanged;
}

} // namespace jit::codegen
