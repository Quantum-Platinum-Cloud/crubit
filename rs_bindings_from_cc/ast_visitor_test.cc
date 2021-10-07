// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rs_bindings_from_cc/ir.h"
#include "rs_bindings_from_cc/ir_from_cc.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/strings/substitute.h"
#include "third_party/absl/types/span.h"

namespace rs_bindings_from_cc {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Property;
using ::testing::SizeIs;
using ::testing::VariantWith;

// Matches an IR node that has the given identifier.
MATCHER_P(IdentifierIs, identifier, "") {
  if (arg.identifier.Ident() == identifier) return true;

  *result_listener << "actual identifier: '" << arg.identifier.Ident() << "'";
  return false;
}

// Matches an IR node that has the given doc comment.
MATCHER_P(DocCommentIs, doc_comment, "") {
  if (arg.doc_comment && *arg.doc_comment == doc_comment) return true;

  *result_listener << "actual doc comment: '"
                   << (arg.doc_comment ? *arg.doc_comment : "<none>") << "'";
  return false;
}

// Matches a Func that has the given mangled name.
MATCHER_P(MangledNameIs, mangled_name, "") {
  if (arg.mangled_name == mangled_name) return true;

  *result_listener << "actual mangled name: '" << arg.mangled_name << "'";
  return false;
}

// Matches a Func that has a return type matching `matcher`.
template <typename Matcher>
auto ReturnType(const Matcher& matcher) {
  return testing::Field("return_type", &Func::return_type, matcher);
}

// Matches a Func that has parameters matching `matchers`.
template <typename... Args>
auto ParamsAre(const Args&... matchers) {
  return testing::Field("params", &Func::params, ElementsAre(matchers...));
}

// Matches a Func that is inline.
MATCHER(IsInline, "") { return arg.is_inline; }

// Matches a FuncParam with a type that matches all given matchers.
template <typename... Args>
auto ParamType(const Args&... matchers) {
  return testing::Field("type", &FuncParam::type, AllOf(matchers...));
}

// Matches a RsType or CcType that has the given name.
MATCHER_P(NameIs, name, "") {
  if (arg.name == name) return true;

  *result_listener << "actual name: '" << arg.name << "'";
  return false;
}

// Matches a MappedType with a CcType that matches all given matchers.
template <typename... Args>
auto CcTypeIs(const Args&... matchers) {
  return testing::Field("cc_type", &MappedType::cc_type, AllOf(matchers...));
}

// Matches a MappedType with a RsType that matches all given matchers.
template <typename... Args>
auto RsTypeIs(const Args&... matchers) {
  return testing::Field("rs_type", &MappedType::rs_type, AllOf(matchers...));
}

// Matches an RsType that has type parameters matching `matchers`.
template <typename... Args>
auto RsTypeParamsAre(const Args&... matchers) {
  return testing::Field("type_params", &RsType::type_params,
                        ElementsAre(matchers...));
}

// Matches a CcType that has type parameters matching `matchers`.
template <typename... Args>
auto CcTypeParamsAre(const Args&... matchers) {
  return testing::Field("type_params", &CcType::type_params,
                        ElementsAre(matchers...));
}

auto IsCcInt() { return AllOf(NameIs("int"), CcTypeParamsAre()); }

auto IsRsInt() { return AllOf(NameIs("i32"), RsTypeParamsAre()); }

// Matches a CcType that is a pointer to a type matching `matcher`.
template <typename Matcher>
auto CcPointsTo(const Matcher& matcher) {
  return AllOf(NameIs("*"), CcTypeParamsAre(matcher));
}

// Matches an RsType that is a pointer to a type matching `matcher`.
template <typename Matcher>
auto RsPointsTo(const Matcher& matcher) {
  return AllOf(NameIs("*mut"), RsTypeParamsAre(matcher));
}

// Matches a MappedType that is void.
MATCHER(IsVoid, "") { return arg.IsVoid(); }

// Matches a MappedType that is an integer.
auto IsInt() { return AllOf(CcTypeIs(IsCcInt()), RsTypeIs(IsRsInt())); }

// Matches a MappedType that is a pointer to integer.
auto IsIntPtr() {
  return AllOf(CcTypeIs(CcPointsTo(IsCcInt())),
               RsTypeIs(RsPointsTo(IsRsInt())));
}

// Matches a MappedType for cc and rs types with no type parameters.
auto IsSimpleType(absl::string_view rs_name, absl::string_view cc_name) {
  return AllOf(CcTypeIs(NameIs(cc_name), CcTypeParamsAre()),
               RsTypeIs(NameIs(rs_name), RsTypeParamsAre()));
}

// Matches a Record that has fields matching `matchers`.
template <typename... Args>
auto FieldsAre(const Args&... matchers) {
  return testing::Field("fields", &Record::fields, ElementsAre(matchers...));
}

// Matches a Record that has the given size.
MATCHER_P(RecordSizeIs, size, "") {
  if (arg.size == size) return true;

  *result_listener << "actual size: " << arg.size;
  return false;
}

// Matches a Record that has the given alignment.
MATCHER_P(AlignmentIs, alignment, "") {
  if (arg.alignment == alignment) return true;

  *result_listener << "actual alignment: " << arg.alignment;
  return false;
}

// Matches a Record with a copy_constructor that matches all given matchers.
template <typename... Args>
auto CopyConstructor(const Args&... matchers) {
  return testing::Field("copy_constructor", &Record::copy_constructor,
                        AllOf(matchers...));
}

// Matches a Record with a move_constructor that matches all given matchers.
template <typename... Args>
auto MoveConstructor(const Args&... matchers) {
  return testing::Field("move_constructor", &Record::move_constructor,
                        AllOf(matchers...));
}

// Matches a Record with a destructor that matches all given matchers.
template <typename... Args>
auto Destructor(const Args&... matchers) {
  return testing::Field("destructor", &Record::destructor, AllOf(matchers...));
}

// Matches a Record which is trivial for calls.
MATCHER(IsTrivialAbi, "") { return arg.is_trivial_abi; }

// Matches a SpecialMemberFunc that has the given definition.
MATCHER_P(DefinitionIs, definition, "") { return arg.definition == definition; }

// Matches a Field that has the given access specifier.
MATCHER_P(AccessIs, access, "") { return arg.access == access; }

// Matches a Field that has the given offset.
MATCHER_P(OffsetIs, offset, "") {
  if (arg.offset == offset) return true;

  *result_listener << "actual offset: " << arg.offset;
  return false;
}

// Matches a Field with a type that matches all given matchers.
template <typename... Args>
auto FieldType(const Args&... matchers) {
  return testing::Field("type", &Field::type, AllOf(matchers...));
}

TEST(AstVisitorTest, Noop) {
  IR ir = IrFromCc({"// nothing interesting there."}, {});
  EXPECT_THAT(ir.items, IsEmpty());
  EXPECT_THAT(ir.used_headers,
              ElementsAre(Property(&HeaderName::IncludePath,
                                   "test/testing_header_0.h")));
}

TEST(AstVisitorTest, IREmptyOnInvalidInput) {
  IR ir = IrFromCc({"int foo(); But this is not C++"}, {});
  EXPECT_THAT(ir.items, IsEmpty());
}

TEST(AstVisitorTest, FuncWithVoidReturnType) {
  IR ir = IrFromCc({"void Foo();"}, {});
  EXPECT_THAT(ir.items, ElementsAre(VariantWith<Func>(
                            AllOf(IdentifierIs("Foo"), MangledNameIs("_Z3Foov"),
                                  ReturnType(IsVoid()), ParamsAre()))));
}

TEST(AstVisitorTest, TwoFuncs) {
  IR ir = IrFromCc({"void Foo(); void Bar();"}, {});
  EXPECT_THAT(
      ir.items,
      ElementsAre(
          VariantWith<Func>(AllOf(IdentifierIs("Foo"), MangledNameIs("_Z3Foov"),
                                  ReturnType(IsVoid()), ParamsAre())),
          VariantWith<Func>(AllOf(IdentifierIs("Bar"), MangledNameIs("_Z3Barv"),
                                  ReturnType(IsVoid()), ParamsAre()))));
}

TEST(AstVisitorTest, TwoFuncsFromTwoHeaders) {
  IR ir = IrFromCc({"void Foo();", "void Bar();"}, {});
  EXPECT_THAT(ir.items, ElementsAre(VariantWith<Func>(IdentifierIs("Foo")),
                                    VariantWith<Func>(IdentifierIs("Bar"))));
}

TEST(AstVisitorTest, NonInlineFunc) {
  IR ir = IrFromCc({"void Foo() {}"}, {});
  EXPECT_THAT(ir.items, ElementsAre(VariantWith<Func>(
                            AllOf(IdentifierIs("Foo"), Not(IsInline())))));
}

TEST(AstVisitorTest, InlineFunc) {
  IR ir = IrFromCc({"inline void Foo() {}"}, {});
  EXPECT_THAT(
      ir.items,
      ElementsAre(VariantWith<Func>(AllOf(IdentifierIs("Foo"), IsInline()))));
}

TEST(AstVisitorTest, FuncJustOnce) {
  IR ir = IrFromCc({"void Foo(); void Foo();"}, {});
  EXPECT_THAT(ir.items,
              ElementsAre(VariantWith<Func>(AllOf(IdentifierIs("Foo")))));
}

TEST(AstVisitorTest, TestImportPointerFunc) {
  IR ir = IrFromCc({"int* Foo(int* a);"}, {});

  EXPECT_THAT(ir.items,
              ElementsAre(VariantWith<Func>(AllOf(
                  ReturnType(IsIntPtr()), ParamsAre(ParamType(IsIntPtr()))))));
}

TEST(AstVisitorTest, Struct) {
  IR ir = IrFromCc(
      {"struct SomeStruct { int first_field; int second_field; };"}, {});

  EXPECT_THAT(ir.items,
              ElementsAre(VariantWith<Record>(AllOf(
                  IdentifierIs("SomeStruct"), RecordSizeIs(8), AlignmentIs(4),
                  FieldsAre(AllOf(IdentifierIs("first_field"),
                                  FieldType(IsInt()), OffsetIs(0)),
                            AllOf(IdentifierIs("second_field"),
                                  FieldType(IsInt()), OffsetIs(32)))))));
}

TEST(AstVisitorTest, TrivialCopyConstructor) {
  absl::string_view file =
      "struct Implicit {};\n"
      "struct Defaulted { Defaulted(const Defaulted&) = default; };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(2));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(CopyConstructor(DefinitionIs(
                            SpecialMemberFunc::Definition::kTrivial)))));
}

TEST(AstVisitorTest, NontrivialCopyConstructor) {
  absl::string_view file = "struct Defined { Defined(const Defined&);};\n";
  // TODO(b/202113881): "struct MemberImplicit { Defined x; };\n"
  // TODO(b/202113881): "struct MemberDefaulted { MemberDefaulted(const
  // MemberDefaulted&) = default; Defined x; };\n"
  IR ir = IrFromCc({file}, {});
  EXPECT_THAT(ir.items, SizeIs(1));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(CopyConstructor(DefinitionIs(
                            SpecialMemberFunc::Definition::kNontrivial)))));
}

TEST(AstVisitorTest, DeletedCopyConstructor) {
  absl::string_view file =
      "struct Deleted { Deleted(const Deleted&) = delete;};\n"
      // TODO(b/202113881): "struct DeletedByMember { Deleted x; };\n"
      "struct DeletedByCtorDef { DeletedByCtorDef(DeletedByCtorDef&&) {} };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(2));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(CopyConstructor(DefinitionIs(
                            SpecialMemberFunc::Definition::kDeleted)))));
}

TEST(AstVisitorTest, PublicCopyConstructor) {
  absl::string_view file =
      "class Implicit {};\n"
      "struct Defaulted { Defaulted(const Defaulted&) = default; };\n"
      "class Section { public: Section(const Section&) = default; };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(3));
  EXPECT_THAT(ir.items,
              Each(VariantWith<Record>(CopyConstructor(AccessIs(kPublic)))));
}

TEST(AstVisitorTest, PrivateCopyConstructor) {
  absl::string_view file =
      "class Defaulted { Defaulted(const Defaulted&) = default; };\n"
      "struct Section { private: Section(const Section&) = default; };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(2));
  EXPECT_THAT(ir.items,
              Each(VariantWith<Record>(CopyConstructor(AccessIs(kPrivate)))));
}

TEST(AstVisitorTest, TrivialMoveConstructor) {
  absl::string_view file =
      "struct Implicit {};\n"
      "struct Defaulted { Defaulted(Defaulted&&) = default; };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(2));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(MoveConstructor(DefinitionIs(
                            SpecialMemberFunc::Definition::kTrivial)))));
}

TEST(AstVisitorTest, NontrivialMoveConstructor) {
  absl::string_view file = "struct Defined { Defined(Defined&&);};\n";
  // TODO(b/202113881): "struct MemberImplicit { Defined x; };\n"
  // TODO(b/202113881): "struct MemberDefaulted { MemberDefaulted(
  // MemberDefaulted&&) = default; Defined x; };\n"
  IR ir = IrFromCc({file}, {});
  EXPECT_THAT(ir.items, SizeIs(1));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(MoveConstructor(DefinitionIs(
                            SpecialMemberFunc::Definition::kNontrivial)))));
}

TEST(AstVisitorTest, DeletedMoveConstructor) {
  absl::string_view file =
      "struct Deleted { Deleted(Deleted&&) = delete;};\n"
      // TODO(b/202113881): "struct DeletedByMember { Deleted x; };\n"
      "struct SuppressedByCtorDef {"
      " SuppressedByCtorDef(const SuppressedByCtorDef&) {}};\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(2));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(MoveConstructor(DefinitionIs(
                            SpecialMemberFunc::Definition::kDeleted)))));
}

TEST(AstVisitorTest, PublicMoveConstructor) {
  absl::string_view file =
      "class Implicit {};\n"
      "struct Defaulted { Defaulted(Defaulted&&) = default; };\n"
      "class Section { public: Section(Section&&) = default; };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(3));
  EXPECT_THAT(ir.items,
              Each(VariantWith<Record>(MoveConstructor(AccessIs(kPublic)))));
}

TEST(AstVisitorTest, PrivateMoveConstructor) {
  absl::string_view file =
      "class Defaulted { Defaulted(Defaulted&&) = default; };\n"
      "struct Section { private: Section(Section&&) = default; };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(2));
  EXPECT_THAT(ir.items,
              Each(VariantWith<Record>(MoveConstructor(AccessIs(kPrivate)))));
}

TEST(AstVisitorTest, TrivialDestructor) {
  absl::string_view file =
      "struct Implicit {};\n"
      "struct Defaulted { ~Defaulted() = default; };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(2));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(Destructor(DefinitionIs(
                            SpecialMemberFunc::Definition::kTrivial)))));
}

TEST(AstVisitorTest, NontrivialDestructor) {
  absl::string_view file = "struct Defined { ~Defined();};\n";
  // TODO(b/202113881): "struct MemberImplicit { Defined x; };\n"
  // TODO(b/202113881): "struct MemberDefaulted { ~MemberDefaulted() = default;
  // Defined x; };\n"
  IR ir = IrFromCc({file}, {});
  EXPECT_THAT(ir.items, SizeIs(1));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(Destructor(DefinitionIs(
                            SpecialMemberFunc::Definition::kNontrivial)))));
}

TEST(AstVisitorTest, DeletedDestructor) {
  absl::string_view file = "struct Deleted { ~Deleted() = delete;};\n";
  // TODO(b/202113881): "struct DeletedByMember { Deleted x; };\n"
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(1));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(Destructor(DefinitionIs(
                            SpecialMemberFunc::Definition::kDeleted)))));
}

TEST(AstVisitorTest, PublicDestructor) {
  absl::string_view file =
      "class Implicit {};\n"
      "struct Defaulted { ~Defaulted() = default; };\n"
      "class Section { public: ~Section() = default; };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(3));
  EXPECT_THAT(ir.items,
              Each(VariantWith<Record>(Destructor(AccessIs(kPublic)))));
}

TEST(AstVisitorTest, PrivateDestructor) {
  absl::string_view file =
      "class Defaulted { ~Defaulted() = default; };\n"
      "struct Section { private: ~Section() = default; };\n";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(2));
  EXPECT_THAT(ir.items,
              Each(VariantWith<Record>(Destructor(AccessIs(kPrivate)))));
}

TEST(AstVisitorTest, TrivialAbi) {
  absl::string_view file = R"cc(
    struct Empty {};
    struct Defaulted {
      Defaulted(const Defaulted&) = default;
    };
    struct [[clang::trivial_abi]] Nontrivial {
      Nontrivial(const Nontrivial&) {}
    };
  )cc";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(3));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(IsTrivialAbi())));
}

TEST(AstVisitorTest, NotTrivialAbi) {
  absl::string_view file = R"cc(
    struct Nontrivial {
      Nontrivial(const Nontrivial&) {}
    };
  )cc";
  IR ir = IrFromCc({file}, {});

  EXPECT_THAT(ir.items, SizeIs(1));
  EXPECT_THAT(ir.items, Each(VariantWith<Record>(Not(IsTrivialAbi()))));
}

TEST(AstVisitorTest, MemberVariableAccessSpecifiers) {
  IR ir = IrFromCc({R"(
    struct SomeStruct {
      int default_access_int;
    public:
      int public_int;
    protected:
      int protected_int;
    private:
      int private_int;
    };

    class SomeClass {
      int default_access_int;
    };
  )"},
                   {});

  EXPECT_THAT(
      ir.items,
      ElementsAre(
          VariantWith<Record>(AllOf(
              IdentifierIs("SomeStruct"),
              FieldsAre(
                  AllOf(IdentifierIs("default_access_int"), AccessIs(kPublic)),
                  AllOf(IdentifierIs("public_int"), AccessIs(kPublic)),
                  AllOf(IdentifierIs("protected_int"), AccessIs(kProtected)),
                  AllOf(IdentifierIs("private_int"), AccessIs(kPrivate))))),
          VariantWith<Record>(
              AllOf(IdentifierIs("SomeClass"),
                    FieldsAre(AllOf(IdentifierIs("default_access_int"),
                                    AccessIs(kPrivate)))))));
}

TEST(AstVisitorTest, IntegerTypes) {
  auto ir = IrFromCc({"#include <stdint.h>\n"
                      "#include <stddef.h>\n"

                      "struct S { "
                      "  bool b;"

                      "  char c;"
                      "  unsigned char uc;"
                      "  signed char sc;"
                      "  char16_t c16;"
                      "  char32_t c32;"
                      "  wchar_t wc;"

                      "  short s;"
                      "  int i;"
                      "  long l;"
                      "  long long ll;"

                      "  unsigned short us;"
                      "  unsigned int ui;"
                      "  unsigned long ul;"
                      "  unsigned long long ull;"

                      "  signed short ss;"
                      "  signed int si;"
                      "  signed long sl;"
                      "  signed long long sll;"

                      "  int8_t i8;"
                      "  int16_t i16;"
                      "  int32_t i32;"
                      "  int64_t i64;"

                      "  uint8_t u8;"
                      "  uint16_t u16;"
                      "  uint32_t u32;"
                      "  uint64_t u64;"

                      "  ptrdiff_t pt;"
                      "  size_t st;",
                      "  intptr_t ip;"
                      "  uintptr_t up;"

                      "  float f;"
                      "  double d;"
                      "};"},
                     {});

  EXPECT_THAT(
      ir.items,
      ElementsAre(VariantWith<Record>(FieldsAre(
          FieldType(IsSimpleType("bool", "bool")),

          FieldType(IsSimpleType("i8", "char")),
          FieldType(IsSimpleType("u8", "unsigned char")),
          FieldType(IsSimpleType("i8", "signed char")),
          FieldType(IsSimpleType("u16", "char16_t")),
          // We cannot map C++ char32_t or wchar_t to Rust char,
          // because Rust requires that chars are valid UTF scalar values.
          FieldType(IsSimpleType("u32", "char32_t")),
          FieldType(IsSimpleType("i32", "wchar_t")),

          FieldType(IsSimpleType("i16", "short")),
          FieldType(IsSimpleType("i32", "int")),
          FieldType(IsSimpleType("i64", "long")),
          FieldType(IsSimpleType("i64", "long long")),

          FieldType(IsSimpleType("u16", "unsigned short")),
          FieldType(IsSimpleType("u32", "unsigned int")),
          FieldType(IsSimpleType("u64", "unsigned long")),
          FieldType(IsSimpleType("u64", "unsigned long long")),

          FieldType(IsSimpleType("i16", "short")),
          FieldType(IsSimpleType("i32", "int")),
          FieldType(IsSimpleType("i64", "long")),
          FieldType(IsSimpleType("i64", "long long")),

          FieldType(IsSimpleType("i8", "int8_t")),
          FieldType(IsSimpleType("i16", "int16_t")),
          FieldType(IsSimpleType("i32", "int32_t")),
          FieldType(IsSimpleType("i64", "int64_t")),

          FieldType(IsSimpleType("u8", "uint8_t")),
          FieldType(IsSimpleType("u16", "uint16_t")),
          FieldType(IsSimpleType("u32", "uint32_t")),
          FieldType(IsSimpleType("u64", "uint64_t")),

          FieldType(IsSimpleType("isize", "ptrdiff_t")),
          FieldType(IsSimpleType("usize", "size_t")),
          FieldType(IsSimpleType("isize", "intptr_t")),
          FieldType(IsSimpleType("usize", "uintptr_t")),

          FieldType(IsSimpleType("f32", "float")),
          FieldType(IsSimpleType("f64", "double"))))));
}

TEST(AstVisitorTest, DocComment) {
  IR ir = IrFromCc({R"(
    /// Doc comment
    ///
    ///  * with three slashes
    struct DocCommentSlashes {};

    //! Doc comment
    //!
    //!  * with slashes and bang
    struct DocCommentBang {};

    /** Multiline comment

         * with two stars */
    struct MultilineCommentTwoStars {};

    // Line comment
    //
    //  * with two slashes
    struct LineComment {};

    /* Multiline comment

        * with one star */
    struct MultilineOneStar {};
    )"},
                   {});

  EXPECT_THAT(
      ir.items,
      ElementsAre(VariantWith<Record>(AllOf(
                      IdentifierIs("DocCommentSlashes"),
                      DocCommentIs("Doc comment\n\n * with three slashes"))),
                  VariantWith<Record>(AllOf(
                      IdentifierIs("DocCommentBang"),
                      DocCommentIs("Doc comment\n\n * with slashes and bang"))),
                  // TODO(forster): The bullet point is not retained in this
                  // case. Instead we get the space at the end. Not sure if this
                  // can be fixed easily...
                  VariantWith<Record>(AllOf(
                      IdentifierIs("MultilineCommentTwoStars"),
                      DocCommentIs("Multiline comment\n\n with two stars "))),
                  VariantWith<Record>(AllOf(
                      IdentifierIs("LineComment"),
                      DocCommentIs("Line comment\n\n * with two slashes"))),
                  // TODO(forster): The bullet point is not retained in this
                  // case. Instead we get the space at the end. Not sure if this
                  // can be fixed easily...
                  VariantWith<Record>(AllOf(
                      IdentifierIs("MultilineOneStar"),
                      DocCommentIs("Multiline comment\n\n with one star ")))));
}

}  // namespace
}  // namespace rs_bindings_from_cc
