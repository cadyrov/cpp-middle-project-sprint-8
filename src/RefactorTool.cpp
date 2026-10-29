#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Lex/Lexer.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

// Метод run вызывается для каждого совпадения с матчем.
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
    auto &Diag = Result.Context->getDiagnostics();
    auto &SM = *Result.SourceManager;  // Получаем SourceManager для проверки isInMainFile

    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("nonVirtualDtor")) {
        handle_nv_dtor(Dtor, Diag, SM);
    }

    if (const auto *Method = Result.Nodes.getNodeAs<CXXMethodDecl>("missingOverride")) {
        handle_miss_override(Method, Diag, SM);
    }

    if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("loopVar")) {
        handle_crange_for(LoopVar, Diag, SM);
    }
}

void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor, DiagnosticsEngine &Diag, SourceManager &SM) {
    Dtor = Dtor->getCanonicalDecl();
    const SourceLocation Loc = Dtor->getLocation();
    if (Dtor->isImplicit() || Dtor->isVirtual() || !Rewriter::isRewritable(Loc) || !SM.isInMainFile(Loc)) {
        return;
    }

    const unsigned LocationID = Loc.getRawEncoding();
    if (virtualDtorLocations.count(LocationID) || Rewrite.InsertTextBefore(Loc, "virtual ")) {
        return;
    }
    virtualDtorLocations.insert(LocationID);

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Добавлен virtual к деструктору");
    Diag.Report(Loc, DiagID);
}

void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method, DiagnosticsEngine &Diag, SourceManager &SM) {
    Method = Method->getCanonicalDecl();
    if (Method->hasAttr<OverrideAttr>() || !SM.isInMainFile(Method->getLocation()) || !Method->getTypeSourceInfo()) {
        return;
    }

    const auto FunctionLoc = Method->getTypeSourceInfo()->getTypeLoc().getAs<FunctionProtoTypeLoc>();
    if (FunctionLoc.isNull()) {
        return;
    }

    SourceLocation LastToken = FunctionLoc.getRParenLoc();
    if (!Rewriter::isRewritable(LastToken)) {
        return;
    }
    // Пропускаем квалификаторы и спецификацию noexcept, включая вложенные выражения.
    unsigned Depth = 0;
    while (const auto Next = Lexer::findNextToken(LastToken, SM, Rewrite.getLangOpts())) {
        if (Next->is(tok::eof) || !Rewriter::isRewritable(Next->getLocation())) {
            return;
        }
        if (Depth == 0 && Next->isOneOf(tok::l_brace, tok::semi, tok::equal, tok::kw_try)) {
            const SourceLocation Loc = Lexer::getLocForEndOfToken(LastToken, 0, SM, Rewrite.getLangOpts());
            if (!Rewriter::isRewritable(Loc) || !SM.isInMainFile(Loc) || overrideLocations.count(Loc.getRawEncoding()) ||
                Rewrite.InsertTextBefore(Loc, " override")) {
                return;
            }
            overrideLocations.insert(Loc.getRawEncoding());
            const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Добавлен override к методу");
            Diag.Report(Loc, DiagID);
            return;
        }
        if (Next->isOneOf(tok::l_paren, tok::l_square, tok::l_brace)) {
            ++Depth;
        } else if (Next->isOneOf(tok::r_paren, tok::r_square, tok::r_brace)) {
            if (Depth == 0) {
                return;
            }
            --Depth;
        }
        LastToken = Next->getLocation();
    }
}

void RefactorHandler::handle_crange_for(const VarDecl *LoopVar, DiagnosticsEngine &Diag, SourceManager &SM) {
    const QualType Type = LoopVar->getType().getCanonicalType();
    if (!Type.isConstQualified() || Type->isReferenceType() || Type->isBuiltinType() ||
        !SM.isInMainFile(LoopVar->getLocation()) || !LoopVar->getTypeSourceInfo()) {
        return;
    }

    SourceLocation TypeEnd = LoopVar->getTypeSourceInfo()->getTypeLoc().getEndLoc();
    if (!Rewriter::isRewritable(TypeEnd)) {
        return;
    }
    // Учитываем const после имени типа: T const x -> T const& x.
    while (const auto Next = Lexer::findNextToken(TypeEnd, SM, Rewrite.getLangOpts())) {
        const auto Spelling = Lexer::getSpelling(*Next, SM, Rewrite.getLangOpts());
        if (Spelling != "const" && Spelling != "volatile") {
            break;
        }
        TypeEnd = Next->getLocation();
    }
    const SourceLocation Loc = Lexer::getLocForEndOfToken(TypeEnd, 0, SM, Rewrite.getLangOpts());
    if (!Rewriter::isRewritable(Loc) || !SM.isInMainFile(Loc) || loopRefLocations.count(Loc.getRawEncoding()) ||
        Rewrite.InsertTextBefore(Loc, "&")) {
        return;
    }
    loopRefLocations.insert(Loc.getRawEncoding());

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Добавлена ссылка к переменной range-for");
    Diag.Report(Loc, DiagID);
}

// todo: ниже необходимо реализовать матчеры для поиска узлов AST
// note: синтаксис написания матчеров точно такой же как и для использования clang-query
/*
    Пример того, как может выглядеть реализация:
    auto AllClassesMatcher()
    {
        return cxxRecordDecl().bind("classDecl");
    }
*/
auto NvDtorMatcher() {
    return cxxDestructorDecl(unless(isImplicit()), unless(isVirtual()), hasParent(cxxRecordDecl().bind("baseClass")),
                             hasAncestor(translationUnitDecl(
                                 hasDescendant(cxxRecordDecl(isDerivedFrom(equalsBoundNode("baseClass")))))))
        .bind("nonVirtualDtor");
}

auto NoOverrideMatcher() {
    return cxxMethodDecl(isOverride(), unless(isImplicit()), unless(hasAttr(attr::Override)))
        .bind("missingOverride");
}

auto NoRefConstVarInRangeLoopMatcher() {
    return cxxForRangeStmt(hasLoopVariable(
        varDecl(hasType(qualType(hasCanonicalType(
            qualType(isConstQualified(), unless(referenceType()), unless(builtinType()))))))
            .bind("loopVar")));
}

// Конструктор принимает Rewriter для изменения кода.
ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
    // Создаем MatchFinder и добавляем матчеры.
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

// Метод HandleTranslationUnit вызывается для каждого файла.
void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) { Finder.matchAST(Context); }

std::unique_ptr<ASTConsumer> CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<ComplexConsumer>(RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction(CompilerInstance &CI) {
    // Инициализируем Rewriter для рефакторинга.
    RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return true;  // Возвращаем true, чтобы продолжить обработку файла.
}

void CodeRefactorAction::EndSourceFileAction() {
    // Применяем изменения в файле.
    if (RewriterForCodeRefactor.overwriteChangedFiles()) {
        llvm::errs() << "Error applying changes to files.\n";
    }
}
