/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "PeepholeV2.h"
#include "Transform.h"

// Helper to hold a list of instructions
struct DexInstructionList {
  // No copying, move only
  DexInstructionList(const DexInstructionList&) = delete;
  DexInstructionList& operator=(const DexInstructionList&) = delete;
  DexInstructionList(DexInstructionList&&) = default;
  DexInstructionList& operator=(DexInstructionList&&) = default;

  std::vector<std::unique_ptr<DexInstruction>> instructions;

  explicit DexInstructionList(std::initializer_list<DexInstruction*> in) {
    for (DexInstruction* insn : in) {
      instructions.emplace_back(insn); // moves insn into unique_ptr
    }
  }

  explicit DexInstructionList(
      std::unique_ptr<std::vector<DexInstruction*>>&& in) {
    for (DexInstruction* insn : *in) {
      instructions.emplace_back(insn); // moves insn into unique_ptr
    }
  }

  // Checks if two instructions are equal
  // Note this is woefully incomplete. It does not handle any of the subclasses
  // of DexInstruction for example. However it is sufficient for the peephole
  // use cases
  static bool instructions_equal(const std::unique_ptr<DexInstruction>& lhs,
                                 const std::unique_ptr<DexInstruction>& rhs) {
    if ((lhs->opcode() != rhs->opcode()) ||
        (lhs->has_literal() != rhs->has_literal()) ||
        (lhs->has_literal() && lhs->literal() != rhs->literal()) ||
        (lhs->srcs_size() != rhs->srcs_size()) ||
        (lhs->dest() != rhs->dest())) {
      return false;
    }
    for (unsigned i = 0; i < lhs->srcs_size(); i++) {
      if (lhs->src(i) != rhs->src(i)) {
        return false;
      }
    }
    return true;
  }

  bool operator==(const DexInstructionList& rhs) const {
    return instructions.size() == rhs.instructions.size() &&
           std::equal(instructions.begin(),
                      instructions.end(),
                      rhs.instructions.begin(),
                      instructions_equal);
  }
};

// Pretty-print instruction lists for gtest
static void PrintTo(const DexInstructionList& insn_list, std::ostream* os) {
  if (insn_list.instructions.empty()) {
    *os << "(empty)\n";
    return;
  }
  for (const auto& insn_ptr : insn_list.instructions) {
    *os << "\n\t" << show(insn_ptr.get());
  }
}

// Builds some arithmetic involving a literal instruction
// The opcode should be a literal-carrying opcode like OPCODE_ADD_INT_LIT16
// The source register is src_reg, dest register is 1
static DexInstructionList op_lit(DexOpcode opcode,
                                 int64_t literal,
                                 unsigned dst_reg = 1) {
  using namespace dex_asm;
  // note: args to dasm() go as dst, src, literal
  return DexInstructionList{
      dasm(OPCODE_CONST_16, {0_v, 42_L}),
      dasm(opcode,
           {Operand{VREG, dst_reg},
            0_v,
            Operand{LITERAL, static_cast<uint64_t>(literal)}}),
  };
}

// Builds arithmetic involving an opcode like MOVE or NEG
static DexInstructionList op_unary(DexOpcode opcode) {
  using namespace dex_asm;
  return DexInstructionList{dasm(OPCODE_CONST_16, {0_v, 42_L}),
                            dasm(opcode, {1_v, 0_v})};
}

class PeepholeTest : public ::testing::Test {
  RedexContext* saved_context = nullptr;
  ConfigFiles config;
  PeepholePassV2 peephole_pass;
  PassManager manager;
  std::vector<DexStore> stores;
  DexClass* dex_class = nullptr;

  // add a void->void static method to our dex_class
  DexMethod* make_void_method(const char* method_name,
                              const DexInstructionList& insns) const {
    auto ret = get_void_type();
    auto args = DexTypeList::make_type_list({});
    auto proto = DexProto::make_proto(ret, args); // I()
    DexMethod* method = DexMethod::make_method(
        dex_class->get_type(), DexString::make_string(method_name), proto);
    method->make_concrete(
        ACC_PUBLIC | ACC_STATIC, std::make_unique<DexCode>(), false);
    method->get_code()->balloon();

    // import our instructions
    auto mt = method->get_code()->get_entries();
    for (const auto& insn_ptr : insns.instructions) {
      mt->push_back(insn_ptr->clone());
    }
    return method;
  }

 public:
  PeepholeTest() : config(Json::nullValue), manager({&peephole_pass}) {
    manager.set_testing_mode();
  }

  virtual void SetUp() override {
    saved_context = g_redex;
    g_redex = new RedexContext();

    const char* dexfile = std::getenv("dexfile");
    ASSERT_NE(nullptr, dexfile);

    DexMetadata dm;
    dm.set_id("classes");
    DexStore root_store(dm);
    root_store.add_classes(load_classes_from_dex(dexfile));
    DexClasses& classes = root_store.get_dexen().back();
    stores.emplace_back(std::move(root_store));
    ASSERT_EQ(classes.size(), 1) << "Expected exactly one class in " << dexfile;
    dex_class = classes.at(0);
    ASSERT_NE(nullptr, dex_class);
  }

  virtual void TearDown() override {
    delete g_redex;
    g_redex = saved_context;
  }

  // Performs one peephole test. Applies peephole optimizations to the given
  // source instruction stream, and checks that it equals the expected result
  void test_1(const char* name,
              const DexInstructionList& src,
              const DexInstructionList& expected) {
    DexMethod* method = make_void_method(name, src);
    dex_class->add_method(method);
    manager.run_passes(stores, config);
    method->get_code()->sync();
    DexInstructionList result(method->get_code()->release_instructions());
    method->get_code()->reset_instructions();
    EXPECT_EQ(result, expected) << " for test " << name;
    dex_class->remove_method(method);
  }

  // Perform a negative peephole test.
  // We expect to NOT modify these instructions.
  void test_1_nochange(const char* name, const DexInstructionList& src) {
    test_1(name, src, src);
  }
};

TEST_F(PeepholeTest, Arithmetic) {
  DexInstructionList move16 = op_unary(OPCODE_MOVE_16); // move v0, v1
  DexInstructionList negate = op_unary(OPCODE_NEG_INT); // neg v0, v1
  test_1("add8_0_to_move", op_lit(OPCODE_ADD_INT_LIT8, 0), move16);
  test_1("add16_0_to_move", op_lit(OPCODE_ADD_INT_LIT16, 0), move16);

  test_1("mult8_1_to_move", op_lit(OPCODE_MUL_INT_LIT8, 1), move16);
  test_1("mult16_1_to_move", op_lit(OPCODE_MUL_INT_LIT16, 1), move16);

  test_1("mult8_neg1_to_neg", op_lit(OPCODE_MUL_INT_LIT8, -1), negate);
  test_1("mult16_neg1_to_neg", op_lit(OPCODE_MUL_INT_LIT16, -1), negate);

  test_1("div8_neg1_to_neg", op_lit(OPCODE_DIV_INT_LIT8, -1), negate);
  test_1("div16_neg1_to_neg", op_lit(OPCODE_DIV_INT_LIT16, -1), negate);

  // These should result in no changes
  test_1_nochange("add8_15", op_lit(OPCODE_ADD_INT_LIT8, 15));
  test_1_nochange("add16_1", op_lit(OPCODE_ADD_INT_LIT16, 1));
  test_1_nochange("mult8_3", op_lit(OPCODE_MUL_INT_LIT8, 3));
  test_1_nochange("mult16_12", op_lit(OPCODE_MUL_INT_LIT16, 12));

  // Negate only has 4 bits for dest register. Ensure we don't try to lower a
  // multiply to a negate if the register offset is too high
  test_1_nochange("mult16_neg1_far", op_lit(OPCODE_MUL_INT_LIT8, -1, 17));
}
