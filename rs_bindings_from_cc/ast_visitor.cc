// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "rs_bindings_from_cc/ast_visitor.h"

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "base/logging.h"
#include "rs_bindings_from_cc/ast_convert.h"
#include "rs_bindings_from_cc/bazel_types.h"
#include "rs_bindings_from_cc/ir.h"
#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/container/flat_hash_set.h"
#include "third_party/absl/status/status.h"
#include "third_party/absl/status/statusor.h"
#include "third_party/absl/strings/cord.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/strings/substitute.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/ASTContext.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/Decl.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/DeclCXX.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/DeclTemplate.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/Mangle.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/RawCommentList.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/RecordLayout.h"
#include "third_party/llvm/llvm-project/clang/include/clang/AST/Type.h"
#include "third_party/llvm/llvm-project/clang/include/clang/Basic/SourceLocation.h"
#include "third_party/llvm/llvm-project/clang/include/clang/Basic/SourceManager.h"
#include "third_party/llvm/llvm-project/clang/include/clang/Basic/Specifiers.h"
#include "third_party/llvm/llvm-project/clang/include/clang/Sema/Sema.h"
#include "third_party/llvm/llvm-project/llvm/include/llvm/ADT/Optional.h"
#include "third_party/llvm/llvm-project/llvm/include/llvm/Support/Casting.h"
#include "util/gtl/flat_map.h"

namespace rs_bindings_from_cc {

constexpr absl::string_view kTypeStatusPayloadUrl =
    "type.googleapis.com/devtools.rust.cc_interop.rs_binding_from_cc.type";

// A mapping of C++ standard types to their equivalent Rust types.
// To produce more idiomatic results, these types receive special handling
// instead of using the generic type mapping mechanism.
static constexpr auto kWellKnownTypes =
    gtl::fixed_flat_map_of<absl::string_view, absl::string_view>({
        {"ptrdiff_t", "isize"},
        {"intptr_t", "isize"},
        {"size_t", "usize"},
        {"uintptr_t", "usize"},
        {"std::ptrdiff_t", "isize"},
        {"std::intptr_t", "isize"},
        {"std::size_t", "usize"},
        {"std::uintptr_t", "usize"},

        {"int8_t", "i8"},
        {"int16_t", "i16"},
        {"int32_t", "i32"},
        {"int64_t", "i64"},
        {"std::int8_t", "i8"},
        {"std::int16_t", "i16"},
        {"std::int32_t", "i32"},
        {"std::int64_t", "i64"},

        {"uint8_t", "u8"},
        {"uint16_t", "u16"},
        {"uint32_t", "u32"},
        {"uint64_t", "u64"},
        {"std::uint8_t", "u8"},
        {"std::uint16_t", "u16"},
        {"std::uint32_t", "u32"},
        {"std::uint64_t", "u64"},

        {"char16_t", "u16"},
        {"char32_t", "u32"},
        {"wchar_t", "i32"},
    });

static DeclId GenerateDeclId(const clang::Decl* decl) {
  return DeclId(reinterpret_cast<uintptr_t>(decl->getCanonicalDecl()));
}

bool AstVisitor::TraverseDecl(clang::Decl* decl) {
  // TODO(mboehme): I'm not sure if TraverseDecl() is supposed to be called with
  // null pointers or whether this is a bug in RecursiveASTVisitor, but I've
  // seen null pointers occur here in practice. In the case where this occurred,
  // TraverseDecl was being called from TraverseTemplateTemplateParmDecl().
  if (!decl) {
    return true;
  }

  // Skip declarations that we've already seen, except for namespaces, which
  // can and typically will contain new declarations when they are "reopened".
  if (seen_decls_.contains(decl->getCanonicalDecl()) &&
      !clang::isa<clang::NamespaceDecl>(decl)) {
    return true;
  }

  const clang::DeclContext* decl_context = decl->getDeclContext();
  if (decl_context && decl_context->isNamespace()) {
    PushUnsupportedItem(decl,
                        "Items contained in namespaces are not supported yet",
                        decl->getBeginLoc());

    return true;
  }

  // Emit all comments in the current file before the decl
  comment_manager_.TraverseDecl(decl);

  return Base::TraverseDecl(decl);
}

bool AstVisitor::TraverseTranslationUnitDecl(
    clang::TranslationUnitDecl* translation_unit_decl) {
  ctx_ = &translation_unit_decl->getASTContext();
  mangler_.reset(ctx_->createMangleContext());

  for (const HeaderName& header_name : public_header_names_) {
    ir_.used_headers.push_back(header_name);
  }

  ir_.current_target = current_target_;

  bool result = Base::TraverseTranslationUnitDecl(translation_unit_decl);

  // Emit comments after the last decl
  comment_manager_.FlushComments();

  EmitIRItems();

  return result;
}

void AstVisitor::EmitIRItems() {
  std::vector<std::tuple<clang::SourceLocation, int, IR::Item>> items;

  // We emit IR items in the order of the decls they were generated for.
  // For decls that emit multiple items we use a stable, but arbitrary order.

  for (const auto& [decl, decl_items] : seen_decls_) {
    for (const auto& decl_item : decl_items) {
      int local_order;

      if (clang::isa<clang::RecordDecl>(decl)) {
        local_order = decl->getDeclContext()->isRecord() ? 1 : 0;
      } else if (auto ctor = clang::dyn_cast<clang::CXXConstructorDecl>(decl)) {
        local_order = ctor->isDefaultConstructor() ? 2
                      : ctor->isCopyConstructor()  ? 3
                      : ctor->isMoveConstructor()  ? 4
                                                   : 5;
      } else if (clang::isa<clang::CXXDestructorDecl>(decl)) {
        local_order = 6;
      } else {
        local_order = 7;
      }

      items.push_back(
          std::make_tuple(decl->getBeginLoc(), local_order, decl_item));
    }
  }

  clang::SourceManager& sm = ctx_->getSourceManager();
  for (auto comment : comment_manager_.comments()) {
    items.push_back(std::make_tuple(
        comment->getBeginLoc(), 0,
        Comment{.text = comment->getFormattedText(sm, sm.getDiagnostics())}));
  }

  std::stable_sort(items.begin(), items.end(),
                   [&](const auto& a, const auto& b) {
                     auto aloc = std::get<0>(a);
                     auto bloc = std::get<0>(b);

                     if (!aloc.isValid() || !bloc.isValid()) {
                       return !aloc.isValid() && bloc.isValid();
                     }

                     return sm.isBeforeInTranslationUnit(aloc, bloc) ||
                            (aloc == bloc && std::get<1>(a) < std::get<1>(b));
                   });

  for (const auto& item : items) {
    ir_.items.push_back(std::get<2>(item));
  }
}

bool AstVisitor::VisitFunctionDecl(clang::FunctionDecl* function_decl) {
  if (!IsFromCurrentTarget(function_decl)) return true;
  if (function_decl->isDeleted()) return true;

  devtools_rust::LifetimeSymbolTable lifetime_symbol_table;
  llvm::Expected<devtools_rust::FunctionLifetimes> lifetimes =
      devtools_rust::GetLifetimeAnnotations(function_decl, lifetime_context_,
                                            &lifetime_symbol_table);
  llvm::DenseSet<devtools_rust::Lifetime> all_lifetimes;

  std::vector<FuncParam> params;
  bool success = true;
  // non-static member functions receive an implicit `this` parameter.
  if (auto* method_decl = llvm::dyn_cast<clang::CXXMethodDecl>(function_decl)) {
    if (method_decl->isInstance()) {
      std::optional<devtools_rust::TypeLifetimes> this_lifetimes;
      if (lifetimes) {
        this_lifetimes = lifetimes->this_lifetimes;
        all_lifetimes.insert(this_lifetimes->begin(), this_lifetimes->end());
      }
      auto param_type = ConvertType(method_decl->getThisType(), this_lifetimes,
                                    /*nullable=*/false);
      if (!param_type.ok()) {
        PushUnsupportedItem(function_decl, param_type.status().ToString(),
                            method_decl->getBeginLoc());

        success = false;
      } else {
        params.push_back({*std::move(param_type), Identifier("__this")});
      }
    }
  }

  if (lifetimes) {
    CHECK_EQ(lifetimes->param_lifetimes.size(), function_decl->getNumParams());
  }
  for (unsigned i = 0; i < function_decl->getNumParams(); ++i) {
    const clang::ParmVarDecl* param = function_decl->getParamDecl(i);
    std::optional<devtools_rust::TypeLifetimes> param_lifetimes;
    if (lifetimes) {
      param_lifetimes = lifetimes->param_lifetimes[i];
      all_lifetimes.insert(param_lifetimes->begin(), param_lifetimes->end());
    }
    auto param_type = ConvertType(param->getType(), param_lifetimes);
    if (!param_type.ok()) {
      PushUnsupportedItem(
          function_decl,
          absl::Substitute("Parameter type '$0' is not supported",
                           param->getType().getAsString()),
          param->getBeginLoc());
      success = false;
      continue;
    }

    if (const clang::RecordType* record_type =
            llvm::dyn_cast<clang::RecordType>(param->getType())) {
      if (clang::RecordDecl* record_decl =
              llvm::dyn_cast<clang::RecordDecl>(record_type->getDecl())) {
        // TODO(b/200067242): non-trivial_abi structs, when passed by value,
        // have a different representation which needs special support. We
        // currently do not support it.
        if (!record_decl->canPassInRegisters()) {
          PushUnsupportedItem(
              function_decl,
              absl::Substitute("Non-trivial_abi type '$0' is not "
                               "supported by value as a parameter",
                               param->getType().getAsString()),
              param->getBeginLoc());
          success = false;
        }
      }
    }

    std::optional<Identifier> param_name = GetTranslatedIdentifier(param);
    CHECK(param_name.has_value());  // No known cases where the above can fail.
    params.push_back({*param_type, *std::move(param_name)});
  }

  if (const clang::RecordType* record_return_type =
          llvm::dyn_cast<clang::RecordType>(function_decl->getReturnType())) {
    if (clang::RecordDecl* record_decl =
            llvm::dyn_cast<clang::RecordDecl>(record_return_type->getDecl())) {
      // TODO(b/200067242): non-trivial_abi structs, when passed by value,
      // have a different representation which needs special support. We
      // currently do not support it.
      if (!record_decl->canPassInRegisters()) {
        PushUnsupportedItem(
            function_decl,
            absl::Substitute("Non-trivial_abi type '$0' is not supported "
                             "by value as a return type",
                             function_decl->getReturnType().getAsString()),
            function_decl->getReturnTypeSourceRange());
        success = false;
      }
    }
  }

  std::optional<devtools_rust::TypeLifetimes> return_lifetimes;
  if (lifetimes) {
    return_lifetimes = lifetimes->return_lifetimes;
    all_lifetimes.insert(return_lifetimes->begin(), return_lifetimes->end());
  }
  auto return_type =
      ConvertType(function_decl->getReturnType(), return_lifetimes);
  if (!return_type.ok()) {
    PushUnsupportedItem(
        function_decl,
        absl::Substitute("Return type '$0' is not supported",
                         function_decl->getReturnType().getAsString()),
        function_decl->getReturnTypeSourceRange());
    success = false;
  }

  std::vector<Lifetime> lifetime_params;
  for (devtools_rust::Lifetime lifetime : all_lifetimes) {
    std::optional<llvm::StringRef> name =
        lifetime_symbol_table.LookupLifetime(lifetime);
    CHECK(name.has_value());
    lifetime_params.push_back(
        {.name = name->str(), .id = LifetimeId(lifetime.Id())});
  }
  std::sort(
      lifetime_params.begin(), lifetime_params.end(),
      [](const Lifetime& l1, const Lifetime& l2) { return l1.name < l2.name; });

  std::optional<MemberFuncMetadata> member_func_metadata;
  if (auto* method_decl = llvm::dyn_cast<clang::CXXMethodDecl>(function_decl)) {
    switch (method_decl->getAccess()) {
      case clang::AS_public:
        break;
      case clang::AS_protected:
      case clang::AS_private:
      case clang::AS_none:
        // No need for IR to include Func representing private methods.
        // TODO(lukasza): Revisit this for protected methods.
        return true;
    }
    std::optional<MemberFuncMetadata::InstanceMethodMetadata> instance_metadata;
    if (method_decl->isInstance()) {
      MemberFuncMetadata::ReferenceQualification reference;
      switch (method_decl->getRefQualifier()) {
        case clang::RQ_LValue:
          reference = MemberFuncMetadata::kLValue;
          break;
        case clang::RQ_RValue:
          reference = MemberFuncMetadata::kRValue;
          break;
        case clang::RQ_None:
          reference = MemberFuncMetadata::kUnqualified;
          break;
      }
      instance_metadata = MemberFuncMetadata::InstanceMethodMetadata{
          .reference = reference,
          .is_const = method_decl->isConst(),
          .is_virtual = method_decl->isVirtual(),
      };
    }

    member_func_metadata = MemberFuncMetadata{
        .record_id = GenerateDeclId(method_decl->getParent()),
        .instance_method_metadata = instance_metadata};
  }

  std::optional<UnqualifiedIdentifier> translated_name =
      GetTranslatedName(function_decl);
  if (success && translated_name.has_value()) {
    seen_decls_[function_decl->getCanonicalDecl()].push_back(Func{
        .name = *translated_name,
        .owning_target = GetOwningTarget(function_decl),
        .doc_comment = GetComment(function_decl),
        .mangled_name = GetMangledName(function_decl),
        .return_type = *return_type,
        .params = std::move(params),
        .lifetime_params = std::move(lifetime_params),
        .is_inline = function_decl->isInlined(),
        .member_func_metadata = std::move(member_func_metadata),
        .source_loc = ConvertSourceLocation(function_decl->getBeginLoc()),
    });
  }

  return true;
}

BlazeLabel AstVisitor::GetOwningTarget(const clang::Decl* decl) const {
  clang::SourceManager& source_manager = ctx_->getSourceManager();
  auto source_location = decl->getLocation();
  auto id = source_manager.getFileID(source_location);

  // If the header this decl comes from is not associated with a target we
  // consider it a textual header. In that case we go up the include stack
  // until we find a header that has an owning target.

  // TODO(b/208377928): We currently don't have a target for the headers in
  // Clang's resource directory, so for the time being we return a fictional
  // "//:virtual_clang_resource_dir_target" for system headers.
  while (source_location.isValid() &&
         !source_manager.isInSystemHeader(source_location)) {
    llvm::Optional<llvm::StringRef> filename =
        source_manager.getNonBuiltinFilenameForID(id);
    if (!filename) {
      return BlazeLabel("//:builtin");
    }
    if (filename->startswith("./")) {
      filename = filename->substr(2);
    }
    auto target_iterator =
        headers_to_targets_.find(HeaderName(filename->str()));
    if (target_iterator != headers_to_targets_.end()) {
      return target_iterator->second;
    }
    source_location = source_manager.getIncludeLoc(id);
    id = source_manager.getFileID(source_location);
  }

  return BlazeLabel("//:virtual_clang_resource_dir_target");
}

bool AstVisitor::IsFromCurrentTarget(const clang::Decl* decl) const {
  return current_target_ == GetOwningTarget(decl);
}

bool AstVisitor::VisitRecordDecl(clang::RecordDecl* record_decl) {
  const clang::DeclContext* decl_context = record_decl->getDeclContext();
  if (decl_context) {
    if (decl_context->isFunctionOrMethod()) {
      return true;
    }
    if (decl_context->isRecord()) {
      PushUnsupportedItem(record_decl, "Nested classes are not supported yet",
                          record_decl->getBeginLoc());
      return true;
    }
  }

  if (record_decl->isUnion()) {
    PushUnsupportedItem(record_decl, "Unions are not supported yet",
                        record_decl->getBeginLoc());
    return true;
  }

  // Make sure the record has a definition that we'll be able to call
  // ASTContext::getASTRecordLayout() on.
  record_decl = record_decl->getDefinition();
  if (!record_decl || record_decl->isInvalidDecl() ||
      !record_decl->isCompleteDefinition()) {
    return true;
  }

  clang::AccessSpecifier default_access = clang::AS_public;

  bool is_final = true;
  if (auto* cxx_record_decl =
          clang::dyn_cast<clang::CXXRecordDecl>(record_decl)) {
    if (cxx_record_decl->getDescribedClassTemplate() ||
        clang::isa<clang::ClassTemplateSpecializationDecl>(record_decl)) {
      PushUnsupportedItem(record_decl, "Class templates are not supported yet",
                          record_decl->getBeginLoc());
      return true;
    }

    sema_.ForceDeclarationOfImplicitMembers(cxx_record_decl);
    if (cxx_record_decl->isClass()) {
      default_access = clang::AS_private;
    }
    is_final = cxx_record_decl->isEffectivelyFinal();
  }
  std::optional<Identifier> record_name = GetTranslatedIdentifier(record_decl);
  if (!record_name.has_value()) {
    return true;
  }
  // Provisionally assume that we know this RecordDecl so that we'll be able
  // to import fields whose type contains the record itself.
  known_type_decls_.insert(record_decl);
  std::optional<std::vector<Field>> fields =
      ImportFields(record_decl, default_access);
  if (!fields.has_value()) {
    // Importing a field failed, so note that we didn't import this RecordDecl
    // after all.
    known_type_decls_.erase(record_decl);
    return true;
  }
  const clang::ASTRecordLayout& layout = ctx_->getASTRecordLayout(record_decl);
  seen_decls_[record_decl->getCanonicalDecl()].push_back(
      Record{.identifier = *record_name,
             .id = GenerateDeclId(record_decl),
             .owning_target = GetOwningTarget(record_decl),
             .doc_comment = GetComment(record_decl),
             .fields = *std::move(fields),
             .size = layout.getSize().getQuantity(),
             .alignment = layout.getAlignment().getQuantity(),
             .copy_constructor = GetCopyCtorSpecialMemberFunc(*record_decl),
             .move_constructor = GetMoveCtorSpecialMemberFunc(*record_decl),
             .destructor = GetDestructorSpecialMemberFunc(*record_decl),
             .is_trivial_abi = record_decl->canPassInRegisters(),
             .is_final = is_final});
  return true;
}

bool AstVisitor::VisitTypedefNameDecl(
    clang::TypedefNameDecl* typedef_name_decl) {
  const clang::DeclContext* decl_context = typedef_name_decl->getDeclContext();
  if (decl_context) {
    if (decl_context->isFunctionOrMethod()) {
      return true;
    }
    if (decl_context->isRecord()) {
      PushUnsupportedItem(typedef_name_decl,
                          "Typedefs nested in classes are not supported yet",
                          typedef_name_decl->getBeginLoc());
      return true;
    }
  }

  clang::QualType type =
      typedef_name_decl->getASTContext().getTypedefType(typedef_name_decl);
  if (kWellKnownTypes.contains(type.getAsString())) {
    return true;
  }

  std::optional<Identifier> identifier =
      GetTranslatedIdentifier(typedef_name_decl);
  if (!identifier.has_value()) {
    // This should never happen.
    LOG(FATAL) << "Couldn't get identifier for TypedefNameDecl";
    return true;
  }
  absl::StatusOr<MappedType> underlying_type =
      ConvertType(typedef_name_decl->getUnderlyingType());
  if (underlying_type.ok()) {
    known_type_decls_.insert(typedef_name_decl);
    seen_decls_[typedef_name_decl->getCanonicalDecl()].push_back(
        TypeAlias{.identifier = *identifier,
                  .id = GenerateDeclId(typedef_name_decl),
                  .owning_target = GetOwningTarget(typedef_name_decl),
                  .underlying_type = *underlying_type});
  } else {
    PushUnsupportedItem(typedef_name_decl, underlying_type.status().ToString(),
                        typedef_name_decl->getBeginLoc());
  }
  return true;
}

std::optional<std::string> AstVisitor::GetComment(
    const clang::Decl* decl) const {
  // This does currently not distinguish between different types of comments.
  // In general it is not possible in C++ to reliably only extract doc comments.
  // This is going to be a heuristic that needs to be tuned over time.

  clang::SourceManager& sm = ctx_->getSourceManager();
  clang::RawComment* raw_comment = ctx_->getRawCommentForDeclNoCache(decl);

  if (raw_comment == nullptr) {
    return {};
  } else {
    return raw_comment->getFormattedText(sm, sm.getDiagnostics());
  }
}

void AstVisitor::PushUnsupportedItem(const clang::Decl* decl,
                                     std::string message,
                                     clang::SourceLocation source_location) {
  if (!IsFromCurrentTarget(decl)) return;

  std::string name = "unnamed";
  if (const auto* named_decl = llvm::dyn_cast<clang::NamedDecl>(decl)) {
    name = named_decl->getQualifiedNameAsString();
  }
  seen_decls_[decl->getCanonicalDecl()].push_back(UnsupportedItem{
      .name = std::move(name),
      .message = std::move(message),
      .source_loc = ConvertSourceLocation(std::move(source_location))});
}

void AstVisitor::PushUnsupportedItem(const clang::Decl* decl,
                                     std::string message,
                                     clang::SourceRange source_range) {
  PushUnsupportedItem(decl, message, source_range.getBegin());
}

SourceLoc AstVisitor::ConvertSourceLocation(clang::SourceLocation loc) const {
  auto& sm = ctx_->getSourceManager();

  clang::StringRef filename = sm.getFilename(loc);
  if (filename.startswith("./")) {
    filename = filename.substr(2);
  }

  return SourceLoc{.filename = filename.str(),
                   .line = sm.getSpellingLineNumber(loc),
                   .column = sm.getSpellingColumnNumber(loc)};
}

absl::StatusOr<MappedType> AstVisitor::ConvertType(
    clang::QualType qual_type,
    std::optional<devtools_rust::TypeLifetimes> lifetimes,
    bool nullable) const {
  std::optional<MappedType> type = std::nullopt;
  // When converting the type to a string, don't include qualifiers -- we handle
  // these separately.
  std::string type_string = qual_type.getUnqualifiedType().getAsString();

  if (auto iter = kWellKnownTypes.find(type_string);
      iter != kWellKnownTypes.end()) {
    type = MappedType::Simple(std::string(iter->second), type_string);
  } else if (const auto* pointer_type =
                 qual_type->getAs<clang::PointerType>()) {
    std::optional<LifetimeId> lifetime;
    if (lifetimes.has_value()) {
      CHECK(!lifetimes->empty());
      lifetime = LifetimeId(lifetimes->back().Id());
      lifetimes->pop_back();
    }
    auto pointee_type = ConvertType(pointer_type->getPointeeType(), lifetimes);
    if (pointee_type.ok()) {
      type = MappedType::PointerTo(*pointee_type, lifetime, nullable);
    }
  } else if (const auto* lvalue_ref_type =
                 qual_type->getAs<clang::LValueReferenceType>()) {
    std::optional<LifetimeId> lifetime;
    if (lifetimes.has_value()) {
      CHECK(!lifetimes->empty());
      lifetime = LifetimeId(lifetimes->back().Id());
      lifetimes->pop_back();
    }
    auto pointee_type =
        ConvertType(lvalue_ref_type->getPointeeType(), lifetimes);
    if (pointee_type.ok()) {
      type = MappedType::LValueReferenceTo(*pointee_type, lifetime);
    }
  } else if (const auto* builtin_type =
                 // Use getAsAdjusted instead of getAs so we don't desugar
                 // typedefs.
             qual_type->getAsAdjusted<clang::BuiltinType>()) {
    switch (builtin_type->getKind()) {
      case clang::BuiltinType::Bool:
        type = MappedType::Simple("bool", "bool");
        break;
      case clang::BuiltinType::Float:
        type = MappedType::Simple("f32", "float");
        break;
      case clang::BuiltinType::Double:
        type = MappedType::Simple("f64", "double");
        break;
      case clang::BuiltinType::Void:
        type = MappedType::Void();
        break;
      default:
        if (builtin_type->isIntegerType()) {
          auto size = ctx_->getTypeSize(builtin_type);
          if (size == 8 || size == 16 || size == 32 || size == 64) {
            type = MappedType::Simple(
                absl::Substitute(
                    "$0$1", builtin_type->isSignedInteger() ? 'i' : 'u', size),
                type_string);
          }
        }
    }
  } else if (const auto* tag_type =
                 qual_type->getAsAdjusted<clang::TagType>()) {
    clang::TagDecl* tag_decl = tag_type->getDecl();

    if (known_type_decls_.contains(tag_decl)) {
      if (std::optional<Identifier> id = GetTranslatedIdentifier(tag_decl)) {
        std::string ident(id->Ident());
        DeclId decl_id = GenerateDeclId(tag_decl);
        type = MappedType::WithDeclIds(ident, decl_id, ident, decl_id);
      }
    }
  } else if (const auto* typedef_type =
                 qual_type->getAsAdjusted<clang::TypedefType>()) {
    clang::TypedefNameDecl* typedef_name_decl = typedef_type->getDecl();

    if (known_type_decls_.contains(typedef_name_decl)) {
      if (std::optional<Identifier> id =
              GetTranslatedIdentifier(typedef_name_decl)) {
        std::string ident(id->Ident());
        DeclId decl_id = GenerateDeclId(typedef_name_decl);
        type = MappedType::WithDeclIds(ident, decl_id, ident, decl_id);
      }
    }
  }

  if (!type.has_value()) {
    absl::Status error = absl::UnimplementedError(
        absl::Substitute("Unsupported type '$0'", type_string));
    error.SetPayload(kTypeStatusPayloadUrl, absl::Cord(type_string));
    return error;
  }

  // Add cv-qualification.
  type->cc_type.is_const = qual_type.isConstQualified();
  // Not doing volatile for now -- note that volatile pointers do not exist in
  // Rust, though volatile reads/writes still do.

  return *std::move(type);
}

std::optional<std::vector<Field>> AstVisitor::ImportFields(
    clang::RecordDecl* record_decl, clang::AccessSpecifier default_access) {
  std::vector<Field> fields;
  const clang::ASTRecordLayout& layout = ctx_->getASTRecordLayout(record_decl);
  for (const clang::FieldDecl* field_decl : record_decl->fields()) {
    auto type = ConvertType(field_decl->getType());
    if (!type.ok()) {
      PushUnsupportedItem(record_decl,
                          absl::Substitute("Field type '$0' is not supported",
                                           field_decl->getType().getAsString()),
                          field_decl->getBeginLoc());
      return std::nullopt;
    }
    clang::AccessSpecifier access = field_decl->getAccess();
    if (access == clang::AS_none) {
      access = default_access;
    }

    std::optional<Identifier> field_name = GetTranslatedIdentifier(field_decl);
    if (!field_name.has_value()) {
      PushUnsupportedItem(
          record_decl,
          absl::Substitute("Cannot translate name for field '$0'",
                           field_decl->getNameAsString()),
          field_decl->getBeginLoc());
      return std::nullopt;
    }
    fields.push_back(
        {.identifier = *std::move(field_name),
         .doc_comment = GetComment(field_decl),
         .type = *type,
         .access = TranslateAccessSpecifier(access),
         .offset = layout.getFieldOffset(field_decl->getFieldIndex())});
  }
  return fields;
}

std::string AstVisitor::GetMangledName(
    const clang::NamedDecl* named_decl) const {
  clang::GlobalDecl decl;

  // There are only three named decl types that don't work with the GlobalDecl
  // unary constructor: GPU kernels (which do not exist in standard C++, so we
  // ignore), constructors, and destructors. GlobalDecl does not support
  // constructors and destructors from the unary constructor because there is
  // more than one global declaration for a given constructor or destructor!
  //
  //   * (Ctor|Dtor)_Complete is a function which constructs / destroys the
  //     entire object. This is what we want. :)
  //   * Dtor_Deleting is a function which additionally calls operator delete.
  //   * (Ctor|Dtor)_Base is a function which constructs/destroys the object but
  //     NOT including virtual base class subobjects.
  //   * (Ctor|Dtor)_Comdat: I *believe* this is the identifier used to
  //     deduplicate inline functions, and is not callable.
  //   * Dtor_(Copying|Default)Closure: These only exist in the MSVC++ ABI,
  //     which we don't support for now. I don't know when they are used.
  //
  // It was hard to piece this together, so writing it down here to explain why
  // we magically picked the *_Complete variants.
  if (auto dtor = llvm::dyn_cast<clang::CXXDestructorDecl>(named_decl)) {
    decl = clang::GlobalDecl(dtor, clang::CXXDtorType::Dtor_Complete);
  } else if (auto ctor =
                 llvm::dyn_cast<clang::CXXConstructorDecl>(named_decl)) {
    decl = clang::GlobalDecl(ctor, clang::CXXCtorType::Ctor_Complete);
  } else {
    decl = clang::GlobalDecl(named_decl);
  }

  std::string name;
  llvm::raw_string_ostream stream(name);
  mangler_->mangleName(decl, stream);
  stream.flush();
  return name;
}

std::optional<UnqualifiedIdentifier> AstVisitor::GetTranslatedName(
    const clang::NamedDecl* named_decl) const {
  switch (named_decl->getDeclName().getNameKind()) {
    case clang::DeclarationName::Identifier: {
      auto name = std::string(named_decl->getName());
      if (name.empty()) {
        if (const clang::ParmVarDecl* param_decl =
                clang::dyn_cast<clang::ParmVarDecl>(named_decl)) {
          int param_pos = param_decl->getFunctionScopeIndex();
          return {Identifier(absl::StrCat("__param_", param_pos))};
        }
        // TODO(lukasza): Handle anonymous structs (probably this won't be an
        // issue until nested types are handled - b/200067824).
        return std::nullopt;
      }
      return {Identifier(std::move(name))};
    }
    case clang::DeclarationName::CXXConstructorName:
      return {SpecialName::kConstructor};
    case clang::DeclarationName::CXXDestructorName:
      return {SpecialName::kDestructor};
    default:
      // To be implemented later: operators, conversion functions.
      // There are also e.g. literal operators, deduction guides, etc., but
      // we might not need to implement them at all. Full list at:
      // https://clang.llvm.org/doxygen/classclang_1_1DeclarationName.html#a9ab322d434446b43379d39e41af5cbe3
      return std::nullopt;
  }
}

void AstVisitor::CommentManager::TraverseDecl(clang::Decl* decl) {
  ctx_ = &decl->getASTContext();

  // When we go to a new file we flush the comments from the previous file,
  // because source locations won't be comparable by '<' any more.
  clang::FileID file = ctx_->getSourceManager().getFileID(decl->getBeginLoc());
  // For example, we hit an invalid FileID for virtual destructor definitions.
  if (file.isInvalid()) {
    return;
  }
  if (file != current_file_) {
    FlushComments();
    current_file_ = file;
    LoadComments();
  }

  // Visit all comments from the current file up to the current decl.
  clang::RawComment* decl_comment = ctx_->getRawCommentForDeclNoCache(decl);
  while (next_comment_ != file_comments_.end() &&
         (*next_comment_)->getBeginLoc() < decl->getBeginLoc()) {
    // Skip the decl's doc comment, which will be emitted as part of the decl.
    if (*next_comment_ != decl_comment) {
      VisitTopLevelComment(*next_comment_);
    }
    ++next_comment_;
  }

  // Skip comments that are within the decl, e.g., comments in the body of an
  // inline function
  // TODO(forster): We should retain floating comments within `Record`s
  if (!clang::isa<clang::NamespaceDecl>(decl)) {
    while (next_comment_ != file_comments_.end() &&
           (*next_comment_)->getBeginLoc() < decl->getEndLoc()) {
      ++next_comment_;
    }
  }
}

void AstVisitor::CommentManager::LoadComments() {
  auto comments = ctx_->Comments.getCommentsInFile(current_file_);
  if (comments) {
    for (auto [_, comment] : *comments) {
      file_comments_.push_back(comment);
    }
  }
  next_comment_ = file_comments_.begin();
}

void AstVisitor::CommentManager::FlushComments() {
  while (next_comment_ != file_comments_.end()) {
    VisitTopLevelComment(*next_comment_);
    next_comment_++;
  }
  file_comments_.clear();
}

void AstVisitor::CommentManager::VisitTopLevelComment(
    const clang::RawComment* comment) {
  comments_.push_back(comment);
}

}  // namespace rs_bindings_from_cc
