#include "swift/IDE/CodeCompletion.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ModuleLoadListener.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Optional.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include "clang/Basic/Module.h"
#include <algorithm>
#include <functional>
#include <string>

using namespace swift;
using namespace ide;

std::string swift::ide::removeCodeCompletionTokens(
    StringRef Input, StringRef TokenName, unsigned *CompletionOffset) {
  assert(TokenName.size() >= 1);

  *CompletionOffset = ~0U;

  std::string CleanFile;
  CleanFile.reserve(Input.size());
  const std::string Token = std::string("#^") + TokenName.str() + "^#";

  for (const char *Ptr = Input.begin(), *End = Input.end();
       Ptr != End; ++Ptr) {
    const char C = *Ptr;
    if (C == '#' && Ptr <= End - Token.size() &&
        StringRef(Ptr, Token.size()) == Token) {
      Ptr += Token.size() - 1;
      *CompletionOffset = CleanFile.size();
      CleanFile += '\0';
      continue;
    }
    if (C == '#' && Ptr <= End - 2 && Ptr[1] == '^') {
      do {
        Ptr++;
      } while(*Ptr != '#');
      continue;
    }
    CleanFile += C;
  }
  return CleanFile;
}

CodeCompletionString::CodeCompletionString(ArrayRef<Chunk> Chunks) {
  Chunk *TailChunks = reinterpret_cast<Chunk *>(this + 1);
  std::copy(Chunks.begin(), Chunks.end(), TailChunks);
  NumChunks = Chunks.size();
}

void CodeCompletionString::print(raw_ostream &OS) const {
  unsigned PrevNestingLevel = 0;
  for (auto C : getChunks()) {
    if (C.getNestingLevel() < PrevNestingLevel) {
      OS << "#}";
    }
    switch (C.getKind()) {
    case Chunk::ChunkKind::Text:
    case Chunk::ChunkKind::LeftParen:
    case Chunk::ChunkKind::RightParen:
    case Chunk::ChunkKind::LeftBracket:
    case Chunk::ChunkKind::RightBracket:
    case Chunk::ChunkKind::Dot:
    case Chunk::ChunkKind::Comma:
    case Chunk::ChunkKind::CallParameterName:
    case Chunk::ChunkKind::CallParameterColon:
    case Chunk::ChunkKind::CallParameterType:
      OS << C.getText();
      break;
    case Chunk::ChunkKind::OptionalBegin:
    case Chunk::ChunkKind::CallParameterBegin:
      OS << "{#";
      break;
    case Chunk::ChunkKind::TypeAnnotation:
      OS << "[#";
      OS << C.getText();
      OS << "#]";
    }
    PrevNestingLevel = C.getNestingLevel();
  }
  while (PrevNestingLevel > 0) {
    OS << "#}";
    PrevNestingLevel--;
  }
}

void CodeCompletionString::dump() const {
  print(llvm::errs());
}

void CodeCompletionResult::print(raw_ostream &OS) const {
  switch (Kind) {
  case ResultKind::Declaration:
    OS << "SwiftDecl: ";
    break;
  case ResultKind::Keyword:
    OS << "Keyword: ";
    break;
  case ResultKind::Pattern:
    OS << "Pattern: ";
    break;
  }
  CompletionString->print(OS);
}

void CodeCompletionResult::dump() const {
  print(llvm::errs());
}

void CodeCompletionResultBuilder::addChunkWithText(
    CodeCompletionString::Chunk::ChunkKind Kind, StringRef Text) {
  Chunks.push_back(CodeCompletionString::Chunk::createWithText(
      Kind, CurrentNestingLevel, Context.copyString(Text)));
}

CodeCompletionResult *CodeCompletionResultBuilder::takeResult() {
  void *Mem = Context.Allocator
      .Allocate(sizeof(CodeCompletionResult) +
                    Chunks.size() * sizeof(CodeCompletionString::Chunk),
                llvm::alignOf<CodeCompletionString>());
  switch (Kind) {
  case CodeCompletionResult::ResultKind::Declaration:
    return new (Context.Allocator)
        CodeCompletionResult(new (Mem) CodeCompletionString(Chunks),
                             AssociatedDecl);
  case CodeCompletionResult::ResultKind::Keyword:
  case CodeCompletionResult::ResultKind::Pattern:
    return new (Context.Allocator)
        CodeCompletionResult(Kind, new (Mem) CodeCompletionString(Chunks));
  }
}

void CodeCompletionResultBuilder::finishResult() {
  Context.addResult(takeResult(), HasLeadingDot);
}

namespace swift {
namespace ide {
struct CodeCompletionContext::ClangCacheImpl {
  CodeCompletionContext &CompletionContext;

  /// Per-module completion result cache for results with a leading dot.
  llvm::DenseMap<const ClangModule *, std::vector<CodeCompletionResult *>>
      CacheWithDot;

  /// Per-module completion result cache for results without a leading dot.
  llvm::DenseMap<const ClangModule *, std::vector<CodeCompletionResult *>>
      CacheWithoutDot;

  /// A function that can generate results for the cache.
  std::function<void(bool NeedLeadingDot)> RefillCache;

  struct RequestedResultsTy {
    const ClangModule *Module;
    bool NeedLeadingDot;
  };

  /// Describes the results requested for current completion.
  Optional<RequestedResultsTy> RequestedResults = Nothing;

  ClangCacheImpl(CodeCompletionContext &CompletionContext)
      : CompletionContext(CompletionContext) {}

  void clear() {
    CacheWithDot.clear();
    CacheWithoutDot.clear();
  }

  void ensureCached() {
  if (!RequestedResults)
    return;

    bool NeedDot = RequestedResults->NeedLeadingDot;
    if ((NeedDot && CacheWithDot.empty()) ||
        (!NeedDot && CacheWithoutDot.empty())) {
      llvm::SaveAndRestore<ResultDestination> ChangeDestination(
          CompletionContext.CurrentDestination, ResultDestination::ClangCache);

      RefillCache(NeedDot);
    }
  }

  void requestUnqualifiedResults() {
    RequestedResults = RequestedResultsTy{nullptr, false};
  }

  void requestQualifiedResults(const ClangModule *Module,
                               bool NeedLeadingDot) {
    assert(Module && "should specify a non-null Module");
    RequestedResults = RequestedResultsTy{Module, NeedLeadingDot};
  }

  void addResult(CodeCompletionResult *R, bool HasLeadingDot);
  void takeResults();
};
} // namespace ide
} // namespace swift

void CodeCompletionContext::ClangCacheImpl::addResult(CodeCompletionResult *R,
                                                      bool HasLeadingDot) {
  const ClangModule *Module = nullptr;
  if (auto *D = R->getAssociatedDecl())
    Module = cast<ClangModule>(D->getModuleContext());
  auto &Cache = HasLeadingDot ? CacheWithDot : CacheWithoutDot;
  Cache[Module].push_back(R);
}

void CodeCompletionContext::ClangCacheImpl::takeResults() {
  if (!RequestedResults)
    return;

  ensureCached();

  std::vector<CodeCompletionResult *> &Results =
      CompletionContext.CurrentCompletionResults;
  // Add Clang results from the cache.
  auto *Module = RequestedResults->Module;
  auto &Cache = RequestedResults->NeedLeadingDot ? CacheWithDot :
                                                   CacheWithoutDot;
  for (auto KV : Cache) {
    // Include results from this module if:
    // * no particular module was requested, or
    // * this module was specifically requested, or
    // * this module is re-exported from the requested module.
    if (!Module || Module == KV.first ||
        Module->getClangModule()->isModuleVisible(KV.first->getClangModule()))
      for (auto R : KV.second)
        Results.push_back(R);
  }

  RequestedResults = Nothing;
}

CodeCompletionContext::CodeCompletionContext()
    : ClangResultCache(new ClangCacheImpl(*this)) {}

CodeCompletionContext::~CodeCompletionContext() {}

StringRef CodeCompletionContext::copyString(StringRef String) {
  char *Mem = Allocator.Allocate<char>(String.size());
  std::copy(String.begin(), String.end(), Mem);
  return StringRef(Mem, String.size());
}

MutableArrayRef<CodeCompletionResult *> CodeCompletionContext::takeResults() {
  ClangResultCache->takeResults();

  // Copy pointers to the results.
  const size_t Count = CurrentCompletionResults.size();
  CodeCompletionResult **Results =
      Allocator.Allocate<CodeCompletionResult *>(Count);
  std::copy(CurrentCompletionResults.begin(), CurrentCompletionResults.end(),
            Results);
  CurrentCompletionResults.clear();
  return MutableArrayRef<CodeCompletionResult *>(Results, Count);
}

namespace {
StringRef getFirstTextChunk(CodeCompletionResult *R) {
  for (auto C : R->getCompletionString()->getChunks()) {
    switch (C.getKind()) {
    case CodeCompletionString::Chunk::ChunkKind::Text:
    case CodeCompletionString::Chunk::ChunkKind::LeftParen:
    case CodeCompletionString::Chunk::ChunkKind::RightParen:
    case CodeCompletionString::Chunk::ChunkKind::LeftBracket:
    case CodeCompletionString::Chunk::ChunkKind::RightBracket:
    case CodeCompletionString::Chunk::ChunkKind::Dot:
    case CodeCompletionString::Chunk::ChunkKind::Comma:
      return C.getText();

    case CodeCompletionString::Chunk::ChunkKind::CallParameterName:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterColon:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterType:
    case CodeCompletionString::Chunk::ChunkKind::OptionalBegin:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterBegin:
    case CodeCompletionString::Chunk::ChunkKind::TypeAnnotation:
      continue;
    }
  }
  return StringRef();
}
} // unnamed namespace

void CodeCompletionContext::addResult(CodeCompletionResult *R,
                                      bool HasLeadingDot) {
  switch (CurrentDestination) {
  case ResultDestination::CurrentSet:
    CurrentCompletionResults.push_back(R);
    return;

  case ResultDestination::ClangCache:
    ClangResultCache->addResult(R, HasLeadingDot);
    return;
  }
}

void CodeCompletionContext::sortCompletionResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  std::sort(Results.begin(), Results.end(),
            [](CodeCompletionResult * LHS, CodeCompletionResult * RHS) {
    return getFirstTextChunk(LHS).compare_lower(getFirstTextChunk(RHS)) < 0;
  });
}

void CodeCompletionContext::clearClangCache() {
  ClangResultCache->clear();
}

void CodeCompletionContext::setCacheClangResults(
    std::function<void(bool NeedLeadingDot)> RefillCache) {
  ClangResultCache->RefillCache = RefillCache;
}

void CodeCompletionContext::includeUnqualifiedClangResults() {
  ClangResultCache->requestUnqualifiedResults();
}

void
CodeCompletionContext::includeQualifiedClangResults(const ClangModule *Module,
                                                    bool NeedLeadingDot) {
  ClangResultCache->requestQualifiedResults(Module, NeedLeadingDot);
}

namespace {
class CodeCompletionCallbacksImpl : public CodeCompletionCallbacks,
                                    public ModuleLoadListener {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;
  TranslationUnit *const TU;

  /// \brief Set to true when we have delivered code completion results
  /// to the \c Consumer.
  bool DeliveredResults = false;

  template<typename ExprType>
  bool typecheckExpr(ExprType *&E) {
    assert(E && "should have an expression");

    DEBUG(llvm::dbgs() << "\nparsed:\n";
          E->print(llvm::dbgs());
          llvm::dbgs() << "\n");

    Expr *AsExpr = E;
    if (!typeCheckCompletionContextExpr(TU, AsExpr))
      return false;
    E = cast<ExprType>(AsExpr);

    DEBUG(llvm::dbgs() << "\type checked:\n";
          E->print(llvm::dbgs());
          llvm::dbgs() << "\n");
    return true;
  }

public:
  CodeCompletionCallbacksImpl(Parser &P,
                              CodeCompletionContext &CompletionContext,
                              CodeCompletionConsumer &Consumer)
      : CodeCompletionCallbacks(P), CompletionContext(CompletionContext),
        Consumer(Consumer), TU(P.TU) {
    P.Context.addModuleLoadListener(*this);
  }

  ~CodeCompletionCallbacksImpl() {
    P.Context.removeModuleLoadListener(*this);
  }

  void completeExpr() override;
  void completeDotExpr(Expr *E) override;
  void completePostfixExprBeginning() override;
  void completePostfixExpr(Expr *E) override;
  void completeExprSuper(SuperRefExpr *SRE) override;
  void completeExprSuperDot(SuperRefExpr *SRE) override;

  void deliverCompletionResults();

  // Implement swift::ModuleLoadListener.
  void loadedModule(ModuleLoader *Loader, Module *M) override;
};
} // end unnamed namespace

void CodeCompletionCallbacksImpl::completeExpr() {
  if (DeliveredResults)
    return;

  Parser::ParserPositionRAII RestorePosition(P);
  P.restoreParserPosition(ExprBeginPosition);

  // FIXME: implement fallback code completion.

  deliverCompletionResults();
}

namespace {
/// Build completions by doing visible decl lookup from a context.
class CompletionLookup : swift::VisibleDeclConsumer
{
  CodeCompletionContext &CompletionContext;
  ASTContext &SwiftContext;
  const DeclContext *CurrDeclContext;

  enum class LookupKind {
    ValueExpr,
    DeclContext
  };

  LookupKind Kind;
  Type ExprType;
  bool HaveDot = false;
  bool NeedLeadingDot = false;
  bool IsSuperRefExpr = false;

  /// \brief True if we are code completing inside a static method.
  bool InsideStaticMethod = false;

  /// \brief DeclContext of the inner method of the code completion point.
  const DeclContext *CurrMethodDC = nullptr;

  bool needDot() const {
    return NeedLeadingDot;
  }

public:
  CompletionLookup(CodeCompletionContext &CompletionContext,
                   ASTContext &SwiftContext,
                   const DeclContext *CurrDeclContext)
      : CompletionContext(CompletionContext), SwiftContext(SwiftContext),
        CurrDeclContext(CurrDeclContext) {
    // Determine if we are doing code completion inside a static method.
    if (CurrDeclContext->isLocalContext()) {
      const DeclContext *FunctionDC = CurrDeclContext;
      while (FunctionDC->isLocalContext()) {
        const DeclContext *Parent = FunctionDC->getParent();
        if (!Parent->isLocalContext())
          break;
        FunctionDC = Parent;
      }
      if (auto *FE = dyn_cast<FuncExpr>(FunctionDC)) {
        auto *FD = FE->getDecl();
        if (FD->getDeclContext()->isTypeContext()) {
          CurrMethodDC = FunctionDC;
          InsideStaticMethod = FD->isStatic();
        }
      }
    }

    CompletionContext.setCacheClangResults([&](bool NeedLeadingDot) {
      llvm::SaveAndRestore<bool> S(this->NeedLeadingDot, NeedLeadingDot);
      if (auto Importer = SwiftContext.getClangModuleLoader())
        static_cast<ClangImporter &>(*Importer).lookupVisibleDecls(*this);
    });
  }

  void setHaveDot() {
    HaveDot = true;
  }

  void setIsSuperRefExpr() {
    IsSuperRefExpr = true;
  }

  void addTypeAnnotation(CodeCompletionResultBuilder &Builder, Type T) {
    if (T->isVoid())
      Builder.addTypeAnnotation("Void");
    else
      Builder.addTypeAnnotation(T.getString());
  }

  void addSwiftVarDeclRef(const VarDecl *VD) {
    StringRef Name = VD->getName().get();
    assert(!Name.empty() && "name should not be empty");

    assert(!(InsideStaticMethod &&
            VD->getDeclContext() == CurrMethodDC->getParent()) &&
           "name lookup bug -- can not see an instance variable "
           "in a static function");
    // FIXME: static variables.

    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration);
    Builder.setAssociatedDecl(VD);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(Name);
    addTypeAnnotation(Builder, VD->getType());
  }

  void addPatternParameters(CodeCompletionResultBuilder &Builder,
                            const Pattern *P) {
    if (auto *TP = dyn_cast<TuplePattern>(P)) {
      bool NeedComma = false;
      for (auto TupleElt : TP->getFields()) {
        if (NeedComma)
          Builder.addComma(", ");
        StringRef BoundNameStr;
        Builder.addCallParameter(TupleElt.getPattern()->getBoundName(),
                                 TupleElt.getPattern()->getType().getString());
        NeedComma = true;
      }
      return;
    }
    if (auto *PP = dyn_cast<ParenPattern>(P)) {
      Builder.addCallParameter(PP->getBoundName(), PP->getType().getString());
      return;
    }
    auto *TP = cast<TypedPattern>(P);
    Builder.addCallParameter(TP->getBoundName(), TP->getType().getString());
  }

  void addSwiftFunctionCall(const AnyFunctionType *AFT) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Pattern);
    Builder.addLeftParen();
    bool NeedComma = false;
    if (auto *TT = AFT->getInput()->getAs<TupleType>()) {
      for (auto TupleElt : TT->getFields()) {
        if (NeedComma)
          Builder.addComma(", ");
        Builder.addCallParameter(TupleElt.getName(),
                                 TupleElt.getType().getString());
        NeedComma = true;
      }
    } else {
      Type T = AFT->getInput();
      if (auto *PT = dyn_cast<ParenType>(T.getPointer())) {
        // Only unwrap the paren shugar, if it exists.
        T = PT->getUnderlyingType();
      }
      Builder.addCallParameter(Identifier(), T->getString());
    }
    Builder.addRightParen();
    addTypeAnnotation(Builder, AFT->getResult());
  }

  void addSwiftMethodCall(const FuncDecl *FD) {
    bool IsImlicitlyCurriedInstanceMethod;
    switch (Kind) {
    case LookupKind::ValueExpr:
      IsImlicitlyCurriedInstanceMethod = ExprType->is<MetaTypeType>() &&
                                         !FD->isStatic();
      break;
    case LookupKind::DeclContext:
      IsImlicitlyCurriedInstanceMethod =
          CurrMethodDC &&
          FD->getDeclContext() == CurrMethodDC->getParent() &&
          InsideStaticMethod && !FD->isStatic();
      break;
    }

    StringRef Name = FD->getName().get();
    assert(!Name.empty() && "name should not be empty");

    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration);
    Builder.setAssociatedDecl(FD);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(Name);
    Builder.addLeftParen();
    auto *FE = FD->getBody();
    auto Patterns = FE->getArgParamPatterns();
    unsigned FirstIndex = 0;
    if (!IsImlicitlyCurriedInstanceMethod && FE->getImplicitThisDecl())
      FirstIndex = 1;
    addPatternParameters(Builder, Patterns[FirstIndex]);
    Builder.addRightParen();
    // FIXME: Pattern should pretty-print itself.
    llvm::SmallString<32> TypeStr;
    for (unsigned i = FirstIndex + 1, e = Patterns.size(); i != e; ++i) {
      TypeStr += "(";
      bool NeedComma = false;
      if (auto *PP = dyn_cast<ParenPattern>(Patterns[i])) {
        TypeStr += PP->getType().getString();
      } else {
        auto *TP = cast<TuplePattern>(Patterns[i]);
        for (auto TupleElt : TP->getFields()) {
          if (NeedComma)
            TypeStr += ", ";
          Identifier BoundName = TupleElt.getPattern()->getBoundName();
          if (!BoundName.empty()) {
            TypeStr += BoundName.str();
            TypeStr += ": ";
          }
          TypeStr += TupleElt.getPattern()->getType().getString();
          NeedComma = true;
        }
      }
      TypeStr += ") -> ";
    }
    Type ResultType = FE->getResultType(SwiftContext);
    if (ResultType->isVoid())
      TypeStr += "Void";
    else
      TypeStr += ResultType.getString();
    Builder.addTypeAnnotation(TypeStr);

    // TODO: skip arguments with default parameters?
  }

  void addSwiftConstructorCall(const ConstructorDecl *CD) {
    assert(!HaveDot && "can not add a constructor call after a dot");
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration);
    Builder.setAssociatedDecl(CD);
    if (IsSuperRefExpr && isa<ConstructorDecl>(CurrDeclContext)) {
      Builder.addTextChunk("constructor");
    }
    Builder.addLeftParen();
    addPatternParameters(Builder, CD->getArguments());
    Builder.addRightParen();
    addTypeAnnotation(Builder, CD->getResultType());
  }

  void addSwiftSubscriptCall(const SubscriptDecl *SD) {
    assert(!HaveDot && "can not add a subscript after a dot");
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration);
    Builder.setAssociatedDecl(SD);
    Builder.addLeftBracket();
    addPatternParameters(Builder, SD->getIndices());
    Builder.addRightBracket();
    addTypeAnnotation(Builder, SD->getElementType());
  }

  void addSwiftNominalTypeRef(const NominalTypeDecl *NTD) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration);
    Builder.setAssociatedDecl(NTD);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(NTD->getName().str());
    addTypeAnnotation(Builder,
                      MetaTypeType::get(NTD->getDeclaredType(), SwiftContext));
  }

  void addSwiftTypeAliasRef(const TypeAliasDecl *TAD) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration);
    Builder.setAssociatedDecl(TAD);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(TAD->getName().str());
    Type TypeAnnotation;
    if (TAD->hasUnderlyingType())
      TypeAnnotation = MetaTypeType::get(TAD->getUnderlyingType(),
                                         SwiftContext);
    else
      TypeAnnotation = MetaTypeType::get(TAD->getDeclaredType(), SwiftContext);
    addTypeAnnotation(Builder, TypeAnnotation);
  }

  void addKeyword(StringRef Name, Type TypeAnnotation) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Keyword);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(Name);
    if (!TypeAnnotation.isNull())
      addTypeAnnotation(Builder, TypeAnnotation);
  }

  void addKeyword(StringRef Name, StringRef TypeAnnotation) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Keyword);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(Name);
    if (!TypeAnnotation.empty())
      Builder.addTypeAnnotation(TypeAnnotation);
  }

  // Implement swift::VisibleDeclConsumer
  void foundDecl(ValueDecl *D) override {
    switch (Kind) {
    case LookupKind::ValueExpr:
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        // Swift does not have class variables.
        // FIXME: add code completion results when class variables are added.
        assert(!ExprType->is<MetaTypeType>() && "name lookup bug");
        addSwiftVarDeclRef(VD);
        return;
      }

      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        // We can not call operators with a postfix parenthesis syntax.
        if (FD->isBinaryOperator() || FD->isUnaryOperator())
          return;

        // We can not call getters or setters.  We use VarDecls and
        // SubscriptDecls to produce completions that refer to getters and
        // setters.
        if (FD->isGetterOrSetter())
          return;

        addSwiftMethodCall(FD);
        return;
      }

      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addSwiftNominalTypeRef(NTD);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addSwiftTypeAliasRef(TAD);
        return;
      }

      if (HaveDot)
        return;

      // FIXME: super.constructor
      if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
        if (!ExprType->is<MetaTypeType>())
          return;
        if (IsSuperRefExpr && !isa<ConstructorDecl>(CurrDeclContext))
          return;
        addSwiftConstructorCall(CD);
        return;
      }

      if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
        if (ExprType->is<MetaTypeType>())
          return;
        addSwiftSubscriptCall(SD);
        return;
      }
      return;

    case LookupKind::DeclContext:
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        addSwiftVarDeclRef(VD);
        return;
      }

      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        // We can not call operators with a postfix parenthesis syntax.
        if (FD->isBinaryOperator() || FD->isUnaryOperator())
          return;

        // We can not call getters or setters.  We use VarDecls and
        // SubscriptDecls to produce completions that refer to getters and
        // setters.
        if (FD->isGetterOrSetter())
          return;

        addSwiftMethodCall(FD);
        return;
      }

      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addSwiftNominalTypeRef(NTD);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addSwiftTypeAliasRef(TAD);
        return;
      }

      return;
    }
  }

  void getValueExprCompletions(Type ExprType) {
    Kind = LookupKind::ValueExpr;
    NeedLeadingDot = !HaveDot;
    this->ExprType = ExprType;
    bool Done = false;
    if (auto AFT = ExprType->getAs<AnyFunctionType>()) {
      addSwiftFunctionCall(AFT);
      Done = true;
    }
    if (auto LVT = ExprType->getAs<LValueType>()) {
      if (auto AFT = LVT->getObjectType()->getAs<AnyFunctionType>()) {
        addSwiftFunctionCall(AFT);
        Done = true;
      }
    }
    if (auto MT = ExprType->getAs<ModuleType>()) {
      if (auto *M = dyn_cast<ClangModule>(MT->getModule())) {
        CompletionContext.includeQualifiedClangResults(M, needDot());
        Done = true;
      }
    }
    if (!Done) {
      lookupVisibleDecls(*this, ExprType, CurrDeclContext);
    }
    {
      // Add the special qualified keyword 'metatype' so that, for example,
      // 'Int.metatype' can be completed.
      Type Annotation = ExprType;

      // First, unwrap the outer LValue.  LValueness of the expr is unrelated
      // to the LValueness of the metatype.
      if (auto *LVT = dyn_cast<LValueType>(Annotation.getPointer())) {
        Annotation = LVT->getObjectType();
      }

      Annotation = MetaTypeType::get(Annotation, SwiftContext);

      // Use the canonical type as a type annotation because looking at the
      // '.metatype' in the IDE is a way to understand what type the expression
      // has.
      addKeyword("metatype", Annotation->getCanonicalType());
    }
  }

  void getCompletionsInDeclContext(SourceLoc Loc) {
    Kind = LookupKind::DeclContext;
    lookupVisibleDecls(*this, CurrDeclContext, Loc);

    // FIXME: The pedantically correct way to find the type is to resolve the
    // swift.StringLiteralType type.
    addKeyword("__FILE__", "String");
    // Same: swift.IntegerLiteralType.
    addKeyword("__LINE__", "Int");
    addKeyword("__COLUMN__", "Int");

    CompletionContext.includeUnqualifiedClangResults();
  }
};

} // end unnamed namespace

void CodeCompletionCallbacksImpl::completeDotExpr(Expr *E) {
  if (!typecheckExpr(E))
    return;

  CompletionLookup Lookup(CompletionContext, TU->Ctx, P.CurDeclContext);
  Lookup.setHaveDot();
  Lookup.getValueExprCompletions(E->getType());

  deliverCompletionResults();
}

void CodeCompletionCallbacksImpl::completePostfixExprBeginning() {
  CompletionLookup Lookup(CompletionContext, TU->Ctx, P.CurDeclContext);
  assert(P.Tok.is(tok::code_complete));
  Lookup.getCompletionsInDeclContext(P.Tok.getLoc());

  deliverCompletionResults();
}

void CodeCompletionCallbacksImpl::completePostfixExpr(Expr *E) {
  if (!typecheckExpr(E))
    return;

  CompletionLookup Lookup(CompletionContext, TU->Ctx, P.CurDeclContext);
  Lookup.getValueExprCompletions(E->getType());

  deliverCompletionResults();
}

void CodeCompletionCallbacksImpl::completeExprSuper(SuperRefExpr *SRE) {
  if (!typecheckExpr(SRE))
    return;

  CompletionLookup Lookup(CompletionContext, TU->Ctx, P.CurDeclContext);
  Lookup.setIsSuperRefExpr();
  Lookup.getValueExprCompletions(SRE->getType());

  deliverCompletionResults();
}

void CodeCompletionCallbacksImpl::completeExprSuperDot(SuperRefExpr *SRE) {
  if (!typecheckExpr(SRE))
    return;

  CompletionLookup Lookup(CompletionContext, TU->Ctx, P.CurDeclContext);
  Lookup.setIsSuperRefExpr();
  Lookup.setHaveDot();
  Lookup.getValueExprCompletions(SRE->getType());

  deliverCompletionResults();
}

void CodeCompletionCallbacksImpl::deliverCompletionResults() {
  auto Results = CompletionContext.takeResults();
  if (!Results.empty()) {
    Consumer.handleResults(Results);
    DeliveredResults = true;
  }
}

void CodeCompletionCallbacksImpl::loadedModule(ModuleLoader *Loader,
                                               Module *M) {
  CompletionContext.clearClangCache();
}

void PrintingCodeCompletionConsumer::handleResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  OS << "Begin completions\n";
  for (auto Result : Results) {
    Result->print(OS);
    OS << "\n";
  }
  OS << "End completions\n";
}

namespace {
class CodeCompletionCallbacksFactoryImpl
    : public CodeCompletionCallbacksFactory {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;

public:
  CodeCompletionCallbacksFactoryImpl(CodeCompletionContext &CompletionContext,
                                     CodeCompletionConsumer &Consumer)
      : CompletionContext(CompletionContext), Consumer(Consumer) {}

  CodeCompletionCallbacks *createCodeCompletionCallbacks(Parser &P) override {
    return new CodeCompletionCallbacksImpl(P, CompletionContext, Consumer);
  }
};
} // end unnamed namespace

CodeCompletionCallbacksFactory *
swift::ide::makeCodeCompletionCallbacksFactory(
    CodeCompletionContext &CompletionContext,
    CodeCompletionConsumer &Consumer) {
  return new CodeCompletionCallbacksFactoryImpl(CompletionContext, Consumer);
}

