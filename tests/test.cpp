#include <gtest/gtest.h>

#include "RefactorTool.h"
#include "clang/Frontend/CompilerInstance.h"

#include <string>

namespace {
class RefactorTestAction : public clang::ASTFrontendAction {
public:
    explicit RefactorTestAction(std::string &Output) : Output(Output) {}

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override {
        Rewrite.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        return std::make_unique<ComplexConsumer>(Rewrite);
    }

    void EndSourceFileAction() override {
        const auto File = Rewrite.getSourceMgr().getMainFileID();
        if (const auto *Buffer = Rewrite.getRewriteBufferFor(File)) {
            Output.assign(Buffer->begin(), Buffer->end());
        } else {
            Output = Rewrite.getSourceMgr().getBufferData(File).str();
        }
    }

private:
    clang::Rewriter Rewrite;
    std::string &Output;
};

void ExpectRefactoring(const std::string &Input, const std::string &Expected) {
    std::string Output;
    ASSERT_TRUE(clang::tooling::runToolOnCodeWithArgs(std::make_unique<RefactorTestAction>(Output), Input,
                                                      {"-std=c++17"}, "test.cpp"));
    EXPECT_EQ(Output, Expected);
    ASSERT_TRUE(clang::tooling::runToolOnCodeWithArgs(std::make_unique<clang::SyntaxOnlyAction>(), Output,
                                                      {"-std=c++17"}, "test.cpp"));

    std::string Repeated;
    ASSERT_TRUE(clang::tooling::runToolOnCodeWithArgs(std::make_unique<RefactorTestAction>(Repeated), Output,
                                                      {"-std=c++17"}, "test.cpp"));
    EXPECT_EQ(Repeated, Output);
}
}  // namespace

TEST(VirtualDestructor, AddsVirtualOnceForMultipleDerivedClasses) {
    ExpectRefactoring("struct B { ~B() = default; }; struct D : B {}; struct E : B {};",
                      "struct B { virtual ~B() = default; }; struct D : B {}; struct E : B {};");
}

TEST(VirtualDestructor, PreservesVirtualStandaloneAndImplicitDestructors) {
    const std::string Code = "struct B { virtual ~B() {} }; struct D : B {}; "
                             "struct S { ~S() {} }; struct I {}; struct J : I {};";
    ExpectRefactoring(Code, Code);
}

TEST(VirtualDestructor, ChangesDeclarationRatherThanOutOfLineDefinition) {
    ExpectRefactoring("struct B { ~B(); }; B::~B() {} struct D : B {};",
                      "struct B { virtual ~B(); }; B::~B() {} struct D : B {};");
}

TEST(MissingOverride, HandlesQualifiersNestedParametersAndPureMethods) {
    ExpectRefactoring("struct B { virtual void f(int = (1 + 2)) const noexcept = 0; }; "
                      "struct D : B { void f(int = (1 + 2)) const noexcept = 0; };",
                      "struct B { virtual void f(int = (1 + 2)) const noexcept = 0; }; "
                      "struct D : B { void f(int = (1 + 2)) const noexcept override = 0; };");
}

TEST(MissingOverride, PreservesExistingOverrideAndNonOverridingOverloads) {
    const std::string Code = "struct B { virtual void f() {} }; "
                             "struct D : B { void f() override {} void f(int) {} };";
    ExpectRefactoring(Code, Code);
}

TEST(MissingOverride, ChangesDeclarationRatherThanOutOfLineDefinition) {
    ExpectRefactoring("struct B { virtual void f() {} }; struct D : B { void f(); }; void D::f() {}",
                      "struct B { virtual void f() {} }; struct D : B { void f() override; }; void D::f() {}");
}

TEST(MissingOverride, HandlesDestructor) {
    ExpectRefactoring("struct B { virtual ~B() {} }; struct D : B { ~D() {} };",
                      "struct B { virtual ~B() {} }; struct D : B { ~D() override {} };");
}

TEST(RangeFor, AddsReferencesToExplicitAutoAndAliasTypes) {
    ExpectRefactoring("struct T {}; using Alias = T; void f() { T xs[2]; "
                      "for (const T x : xs) {} for (const auto x : xs) {} "
                      "for (const Alias x : xs) {} for (T const x : xs) {} }",
                      "struct T {}; using Alias = T; void f() { T xs[2]; "
                      "for (const T& x : xs) {} for (const auto& x : xs) {} "
                      "for (const Alias& x : xs) {} for (T const& x : xs) {} }");
}

TEST(RangeFor, PreservesReferencesFundamentalTypesAndNonLoopVariables) {
    const std::string Code = "struct T {}; using Int = int; using Ref = const T&; void f() { "
                             "T xs[2]; int ns[2] = {}; const T local{}; "
                             "for (const T& x : xs) {} for (Ref x : xs) {} for (T x : xs) {} "
                             "for (const int n : ns) {} for (const auto n : ns) {} for (const Int n : ns) {} }";
    ExpectRefactoring(Code, Code);
}
