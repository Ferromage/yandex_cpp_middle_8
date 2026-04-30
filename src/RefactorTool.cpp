#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

#include "RefactorTool.h"
#include <iostream>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

namespace {

ASTContext *context = nullptr;

bool hasDerivedClasses(const CXXRecordDecl *base) {
    if (!context || !base || !base->hasDefinition()) {
        return false;
    }

    const TranslationUnitDecl *unit = context->getTranslationUnitDecl();
    for (const Decl *decl : unit->decls()) {
        const auto *record = dyn_cast<CXXRecordDecl>(decl);
        if (!record || !record->hasDefinition()) {
            continue;
        }
        if (record == base) {
            continue;
        }

        if (record->isDerivedFrom(base)) {
            return true;
        }
    }
    return false;
}

SourceLocation FindOverrideInsertionLoc(const CXXMethodDecl *Method, const SourceManager &SM,
                                        const LangOptions &LangOpts) {
    SourceLocation startLoc = Method->getNameInfo().getEndLoc();
    if (startLoc.isInvalid())
        return SourceLocation();

    startLoc = SM.getSpellingLoc(startLoc);

    Token tok;
    SourceLocation currLoc = startLoc;

    while (!Lexer::getRawToken(currLoc, tok, SM, LangOpts, /*IgnoreFirst=*/true)) {
        if (tok.is(tok::r_paren)) {
            return tok.getEndLoc();
        }

        if (tok.is(tok::eof) || tok.is(tok::unknown)) {
            break;
        }

        currLoc = tok.getEndLoc();
    }

    return SourceLocation();
}

}  // namespace

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

// Метод run вызывается для каждого совпадения с матчем.
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
    auto &Diag = Result.Context->getDiagnostics();
    auto &SM = *Result.SourceManager;  // Получаем SourceManager для проверки isInMainFile

    if (const auto *Dtor = Result.Nodes.getNodeAs<CXXDestructorDecl>("nonVirtualDtor")) {
        handle_nv_dtor(Dtor, Diag, SM);
    }

    if (const auto *Method = Result.Nodes.getNodeAs<CXXMethodDecl>("missingOverride");
        Method && Method->size_overridden_methods() > 0 && !Method->hasAttr<OverrideAttr>()) {
        handle_miss_override(Method, Diag, SM);
    }

    if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("loopVar")) {
        handle_crange_for(LoopVar, Diag, SM);
    }
}

void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor, DiagnosticsEngine &Diag, SourceManager &SM) {
    if (!SM.isInMainFile(Dtor->getLocation()) || Dtor->isVirtual()) {
        return;
    }

    const CXXRecordDecl *record = Dtor->getParent();
    if (!record || !record->hasDefinition()) {
        return;
    }

    if (hasDerivedClasses(record)) {
        const auto locNum = Dtor->getLocation().getRawEncoding();
        if (virtualDtorLocations.count(locNum) == 0) {
            const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Объявлен деструктор");
            Diag.Report(Dtor->getLocation(), DiagID);
            rewriter.InsertTextBefore(Dtor->getLocation(), "virtual ");
        }
        virtualDtorLocations.insert(locNum);
    }
}

void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method, DiagnosticsEngine &Diag, SourceManager &SM) {
    if (!SM.isInMainFile(Method->getLocation())) {
        return;
    }

    const LangOptions &langOpts = context->getLangOpts();

    SourceLocation InsertLoc = FindOverrideInsertionLoc(Method, SM, langOpts);
    if (!InsertLoc.isValid()) {
        return;
    }

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Объявлен метод");
    Diag.Report(InsertLoc, DiagID);
    rewriter.InsertText(InsertLoc, " override");
}

void RefactorHandler::handle_crange_for(const VarDecl *LoopVar, DiagnosticsEngine &Diag, SourceManager &SM) {
    const QualType type = LoopVar->getType().getCanonicalType();

    if (type->isFundamentalType() || type->isPointerType() || type->isEnumeralType()) {
        return;
    }

    const unsigned DiagID = Diag.getCustomDiagID(DiagnosticsEngine::Remark, "Объявлена переменная");
    Diag.Report(LoopVar->getLocation(), DiagID);
    rewriter.InsertText(LoopVar->getLocation(), "&");
}

auto NvDtorMatcher() {
    return cxxDestructorDecl(unless(isImplicit()), ofClass(cxxRecordDecl(hasDefinition()))).bind("nonVirtualDtor");
}

auto NoOverrideMatcher() {
    return cxxMethodDecl(isVirtual(), unless(isImplicit()), unless(cxxDestructorDecl()),
                         hasParent(cxxRecordDecl(isDerivedFrom(cxxRecordDecl()))))
        .bind("missingOverride");
}

auto NoRefConstVarInRangeLoopMatcher() {
    return varDecl(hasType(isConstQualified()), hasParent(declStmt(hasParent(cxxForRangeStmt())))).bind("loopVar");
}

// Конструктор принимает Rewriter для изменения кода.
ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
    // Создаем MatchFinder и добавляем матчеры.
    Finder.addMatcher(NvDtorMatcher(), &Handler);
    Finder.addMatcher(NoOverrideMatcher(), &Handler);
    Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

// Метод HandleTranslationUnit вызывается для каждого файла.
void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) {
    context = &Context;
    Finder.matchAST(Context);
}

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

int main(int argc, const char **argv) {
    // Парсер опций: Обрабатывает флаги командной строки, компиляционные базы данных.
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    // Создаем ClangTool
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    // Запускаем RefactorAction.
    return Tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
}