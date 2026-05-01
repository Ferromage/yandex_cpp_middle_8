#include "RefactorTool.h"
#include <fstream>
#include <gtest/gtest.h>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

namespace {
llvm::cl::OptionCategory toolCategory("refactor-tool options");
constexpr std::string_view fileName = "file_tmp.cpp";

const auto destructorTextRef = std::string_view(
    R"(#include <iostream>

class BaseNoDtor {
public:
    int x;
    virtual ~BaseNoDtor() = default;
};

class DerivedFromNoDtor : public BaseNoDtor {};

class BaseNonVirtual {  // Невиртуальный деструктор
public:
    virtual ~BaseNonVirtual() {}  //Убедимся что один раз virtual
};

class DerivedFromNonVirtual : public BaseNonVirtual {};

class DerivedFromNonVirtual1 : public DerivedFromNonVirtual {};

class BaseVirtual {  // Уже виртуальный деструктор, не меняется
public:
    virtual ~BaseVirtual() {}
};

class DerivedFromVirtual : public BaseVirtual {};

class Standalone {  // Нет наследников, не меняется
public:
    ~Standalone() {}
};
)");

const auto overrideTextRef = std::string_view(
    R"(#include <string>

class Base {
public:
    virtual void func() {}
    virtual void func(int a) = 0;
    virtual ~Base() {}
};

class Derived : public Base {
public:
    void func() override {}  // Переопределен без override
    void func(int a) override {}  // Переопределен без override
    ~Derived() {}  // Деструктор без override
};

class SubDerived : public Derived {
public:
    void func() override {}  // Переопределен без override
    void func(int a) override {}  // Переопределен без override
};

class BaseWithOverride {
public:
    virtual void func() {}
};

class DerivedWithOverride : public BaseWithOverride {
public:
    void func() override {}  // Уже с override, не меняется
};)");

const auto rangeForTextRef = std::string_view(
    R"(#include <vector>
#include <string>
#include <iostream>

struct CustomType {
    int id;
    std::string name;
};

void process() {
    std::vector<CustomType> vec = {{1, "a"}, {2, "b"}};

    // const auto без &
    for (const auto &x : vec) {
        std::cout << x.id;
    }

    // const явный тип без &
    for (const CustomType &x : vec) {
        std::cout << x.id;
    }

    // const decltype без &
    for (const decltype(vec)::value_type &x : vec) {
        std::cout << x.id;
    }

    // Фундаментальный тип, не меняется
    std::vector<int> ints = {1, 2};
    for (const int x : ints) {
        std::cout << x;
    }

    // Уже с &
    for (const auto &x : vec) {
        std::cout << x.id;
    }
})");

}  // namespace

class BaseClass : public ::testing::Test {
protected:
    const char *argv[2];
    int argc;

    void SetUp() override {
        argc = 2;
        argv[0] = "refactor_tool";
    }

    void TearDown() override {}

    void startFix(const char *filepath) {
        argv[1] = filepath;

        auto expectedParser = CommonOptionsParser::create(argc, argv, toolCategory);
        if (!expectedParser) {
            llvm::errs() << expectedParser.takeError();
            ASSERT_TRUE(false);
        }
        CommonOptionsParser &OptionsParser = expectedParser.get();
        ClangTool tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
        tool.run(newFrontendActionFactory<CodeRefactorAction>().get());
    }
};

TEST_F(BaseClass, DestructorCheckNoInsert) {
    std::fstream file(fileName.data(), std::ios::trunc | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file << destructorTextRef;
    file.close();

    startFix(fileName.data());

    file.open(fileName.data(), std::ios::in);
    ASSERT_TRUE(file.is_open());
    char buf[1024];
    file.read(buf, std::size(buf));
    std::string strIn(buf, file.gcount());
    ASSERT_EQ(strIn, destructorTextRef);
}

TEST_F(BaseClass, DestructorCheckInsert) {
    const auto textIn = std::string_view(
        R"(#include <iostream>

class BaseNoDtor {
public:
    int x;
    ~BaseNoDtor() = default;
};

class DerivedFromNoDtor : public BaseNoDtor {};

class BaseNonVirtual {  // Невиртуальный деструктор
public:
    ~BaseNonVirtual() {}  //Убедимся что один раз virtual
};

class DerivedFromNonVirtual : public BaseNonVirtual {};

class DerivedFromNonVirtual1 : public DerivedFromNonVirtual {};

class BaseVirtual {  // Уже виртуальный деструктор, не меняется
public:
    virtual ~BaseVirtual() {}
};

class DerivedFromVirtual : public BaseVirtual {};

class Standalone {  // Нет наследников, не меняется
public:
    ~Standalone() {}
};
)");

    std::fstream file(fileName.data(), std::ios::trunc | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file << textIn;
    file.close();

    startFix(fileName.data());

    file.open(fileName.data(), std::ios::in);
    ASSERT_TRUE(file.is_open());
    char buf[1024];
    file.read(buf, std::size(buf));
    std::string strIn(buf, file.gcount());
    ASSERT_EQ(strIn, destructorTextRef);
}

TEST_F(BaseClass, OverrideCheckNoInsert) {
    std::fstream file(fileName.data(), std::ios::trunc | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file << overrideTextRef;
    file.close();

    startFix(fileName.data());

    file.open(fileName.data(), std::ios::in);
    ASSERT_TRUE(file.is_open());
    char buf[1024];
    file.read(buf, std::size(buf));
    std::string strIn(buf, file.gcount());
    ASSERT_EQ(strIn, overrideTextRef);
}

TEST_F(BaseClass, OverrideCheckInsert) {
    const auto textIn = std::string_view(
        R"(#include <string>

class Base {
public:
    virtual void func() {}
    virtual void func(int a) = 0;
    virtual ~Base() {}
};

class Derived : public Base {
public:
    void func() {}  // Переопределен без override
    void func(int a) {}  // Переопределен без override
    ~Derived() {}  // Деструктор без override
};

class SubDerived : public Derived {
public:
    void func() {}  // Переопределен без override
    void func(int a) {}  // Переопределен без override
};

class BaseWithOverride {
public:
    virtual void func() {}
};

class DerivedWithOverride : public BaseWithOverride {
public:
    void func() override {}  // Уже с override, не меняется
};)");

    std::fstream file(fileName.data(), std::ios::trunc | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file << textIn;
    file.close();

    startFix(fileName.data());

    file.open(fileName.data(), std::ios::in);
    ASSERT_TRUE(file.is_open());
    char buf[1024];
    file.read(buf, std::size(buf));
    std::string strIn(buf, file.gcount());
    ASSERT_EQ(strIn, overrideTextRef);
}

TEST_F(BaseClass, RangeForCheckNoInsert) {
    std::fstream file(fileName.data(), std::ios::trunc | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file << rangeForTextRef;
    file.close();

    startFix(fileName.data());

    file.open(fileName.data(), std::ios::in);
    ASSERT_TRUE(file.is_open());
    char buf[1024];
    file.read(buf, std::size(buf));
    std::string strIn(buf, file.gcount());
    ASSERT_EQ(strIn, rangeForTextRef);
}

TEST_F(BaseClass, RangeForCheckInsert) {
    const auto textIn = std::string_view(
        R"(#include <vector>
#include <string>
#include <iostream>

struct CustomType {
    int id;
    std::string name;
};

void process() {
    std::vector<CustomType> vec = {{1, "a"}, {2, "b"}};

    // const auto без &
    for (const auto x : vec) {
        std::cout << x.id;
    }

    // const явный тип без &
    for (const CustomType x : vec) {
        std::cout << x.id;
    }

    // const decltype без &
    for (const decltype(vec)::value_type x : vec) {
        std::cout << x.id;
    }

    // Фундаментальный тип, не меняется
    std::vector<int> ints = {1, 2};
    for (const int x : ints) {
        std::cout << x;
    }

    // Уже с &
    for (const auto &x : vec) {
        std::cout << x.id;
    }
})");

    std::fstream file(fileName.data(), std::ios::trunc | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file << textIn;
    file.close();

    startFix(fileName.data());

    file.open(fileName.data(), std::ios::in);
    ASSERT_TRUE(file.is_open());
    char buf[1024];
    file.read(buf, std::size(buf));
    std::string strIn(buf, file.gcount());
    ASSERT_EQ(strIn, rangeForTextRef);
}