/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "FinalInlineV2.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"

struct FinalInlineTest : public RedexTest {
 public:
  void SetUp() override {
    m_cc = std::make_unique<ClassCreator>(DexType::make_type("LFoo;"));
    m_cc->set_super(get_object_type());
  }

 protected:
  DexField* create_field_with_value(const char* name, uint32_t value) {
    auto field = static_cast<DexField*>(DexField::make_field(name));
    auto encoded_value = DexEncodedValue::zero_for_type(get_int_type());
    encoded_value->value(value);
    field->make_concrete(ACC_PUBLIC | ACC_STATIC, encoded_value);
    m_cc->add_field(field);
    return field;
  }

  std::unique_ptr<ClassCreator> m_cc;
};

TEST_F(FinalInlineTest, encodeValues) {
  auto field = create_field_with_value("LFoo;.bar:I", 0);
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (const v0 1)
      (sput v0 "LFoo;.bar:I")
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  FinalInlinePassV2::run({cls});

  EXPECT_EQ(cls->get_clinit(), nullptr);
  EXPECT_EQ(field->get_static_value()->value(), 1);
}

TEST_F(FinalInlineTest, fieldSetInLoop) {
  auto field_bar = create_field_with_value("LFoo;.bar:I", 0);
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (:loop)
      (sget "LFoo;.bar:I")
      (move-result-pseudo v0)
      (add-int/lit8 v0 v0 1)
      (sput v0 "LFoo;.bar:I")
      (const v1 10)
      (if-ne v0 v1 :loop)
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  auto original = assembler::to_s_expr(cls->get_clinit()->get_code());
  FinalInlinePassV2::run({cls});
  EXPECT_EQ(assembler::to_s_expr(cls->get_clinit()->get_code()), original);
  EXPECT_EQ(field_bar->get_static_value()->value(), 0);
}

TEST_F(FinalInlineTest, fieldConditionallySet) {
  auto field_bar = create_field_with_value("LFoo;.bar:I", 0);
  auto field_baz = create_field_with_value("LFoo;.baz:I", 0);
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (sget "LUnknown;.field:I")
      (move-result-pseudo v0)
      (if-eqz v0 :true)
      (const v1 1)
      (sput v1 "LFoo;.bar:I")
      (:true)
      ; bar may be 0 or 1 here
      (sget "LFoo;.bar:I")
      (move-result-pseudo v0)
      (sput v0 "LFoo;.baz:I")
      (sput v1 "LFoo;.bar:I")
      ; bar is always 1 on exit
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  auto original = assembler::to_s_expr(cls->get_clinit()->get_code());
  FinalInlinePassV2::run({cls});
  EXPECT_EQ(assembler::to_s_expr(cls->get_clinit()->get_code()), original);
  EXPECT_EQ(field_bar->get_static_value()->value(), 0);
  EXPECT_EQ(field_baz->get_static_value()->value(), 0);
}

TEST_F(FinalInlineTest, dominatedSget) {
  auto field_bar = create_field_with_value("LFoo;.bar:I", 0);
  auto field_baz = create_field_with_value("LFoo;.baz:I", 0);
  m_cc->add_method(assembler::method_from_string(R"(
    (method (public static) "LFoo;.<clinit>:()V"
     (
      (const v0 1)
      (sput v0 "LFoo;.bar:I")
      (sget "LFoo;.bar:I")
      (move-result-pseudo v0)
      (sput v0 "LFoo;.baz:I")
      (return-void)
     )
    )
  )"));
  auto cls = m_cc->create();

  // This could be further optimized to remove the sput to the field bar. This
  // test illustrates that we are being overly conservative if a field is
  // ever read in its <clinit>. In practice though this rarely occurs.
  auto expected = assembler::ircode_from_string(R"(
    (
      (const v0 1)
      (sput v0 "LFoo;.bar:I")
      (return-void)
    )
  )");

  auto original = assembler::to_s_expr(cls->get_clinit()->get_code());
  FinalInlinePassV2::run({cls});
  EXPECT_EQ(assembler::to_s_expr(cls->get_clinit()->get_code()),
            assembler::to_s_expr(expected.get()));
  EXPECT_EQ(field_bar->get_static_value()->value(), 0);
  EXPECT_EQ(field_baz->get_static_value()->value(), 1);
}
