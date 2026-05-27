#include "Formatter.h"

#include "topo/Basic/Diagnostic.h"
#include "topo/Lexer/Lexer.h"
#include "topo/Parser/Parser.h"

#include <gtest/gtest.h>
#include <sstream>
#include <string>

using namespace topo;
using namespace topo::lsp;

// Helper: parse source with comment preservation, then format
static std::string formatSource(const std::string& source) {
    DiagnosticEngine diag;
    Lexer lexer(source, "<test>", diag);
    lexer.setPreserveComments(true);
    Parser parser(lexer, diag);
    auto ast = parser.parseTopoFile();
    if (!ast || diag.hasErrors()) return "";

    Formatter formatter;
    return formatter.format(static_cast<const TopoFile&>(*ast), source, lexer.comments());
}

// Helper: count leading spaces
static int countIndent(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ')
            ++n;
        else
            break;
    }
    return n;
}

// Helper: split into lines
static std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

// ============================================================================
// Basic formatting tests
// ============================================================================

TEST(Formatter, FourSpaceIndent) {
    std::string source =
        "namespace app {\n"
        "  public:\n"
        "    void run();\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());

    auto lines = splitLines(result);
    // namespace line: 0 indent
    EXPECT_EQ(countIndent(lines[0]), 0);
    // public: label: 4 spaces (1 level)
    bool foundPublic = false;
    for (const auto& line : lines) {
        if (line.find("public:") != std::string::npos) {
            EXPECT_EQ(countIndent(line), 4);
            foundPublic = true;
        }
        if (line.find("void run()") != std::string::npos) {
            EXPECT_EQ(countIndent(line), 8);
        }
    }
    EXPECT_TRUE(foundPublic);
}

TEST(Formatter, VisibilitySectionSpacing) {
    std::string source =
        "namespace engine {\n"
        "public:\n"
        "void render();\n"
        "protected:\n"
        "void loadShaders();\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());

    auto lines = splitLines(result);
    // There should be a blank line between public section and protected section
    bool foundBlank = false;
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (lines[i].find("render()") != std::string::npos) {
            // Look for blank line before protected:
            for (size_t j = i + 1; j < lines.size(); ++j) {
                if (lines[j].find("protected:") != std::string::npos) {
                    // Check that there's a blank line between them
                    for (size_t k = i + 1; k < j; ++k) {
                        if (lines[k].empty()) foundBlank = true;
                    }
                    break;
                }
            }
        }
    }
    EXPECT_TRUE(foundBlank);
}

TEST(Formatter, FunctionDeclAndFnBlockNoBlankLine) {
    std::string source =
        "namespace app {\n"
        "  public:\n"
        "    void run();\n"
        "\n"
        "    fn run {\n"
        "      stage<1> init();\n"
        "    }\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());

    auto lines = splitLines(result);
    // Find "void run();" and "fn run {" — they should be consecutive
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (lines[i].find("void run();") != std::string::npos) {
            EXPECT_EQ(lines[i + 1].find("fn run {"), 8u) << "fn block should immediately follow its declaration";
            break;
        }
    }
}

TEST(Formatter, SingleLineParams) {
    std::string source =
        "namespace sig {\n"
        "  public:\n"
        "    void short_fn(int x);\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    // Should be on one line
    EXPECT_NE(result.find("void short_fn(int x);"), std::string::npos);
}

TEST(Formatter, ImportSorting) {
    std::string source =
        "import zebra;\n"
        "import alpha;\n"
        "\n"
        "namespace app {\n"
        "  public:\n"
        "    void run();\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());

    // alpha should come before zebra
    auto alphaPos = result.find("import alpha;");
    auto zebraPos = result.find("import zebra;");
    ASSERT_NE(alphaPos, std::string::npos);
    ASSERT_NE(zebraPos, std::string::npos);
    EXPECT_LT(alphaPos, zebraPos);
}

TEST(Formatter, CommentPreservation) {
    std::string source =
        "// File header\n"
        "namespace app {\n"
        "  public:\n"
        "    void render(); // inline\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    EXPECT_NE(result.find("// File header"), std::string::npos);
    EXPECT_NE(result.find("// inline"), std::string::npos);
}

TEST(Formatter, CommentSpaceNormalization) {
    std::string source =
        "//no space\n"
        "namespace app {\n"
        "  public:\n"
        "    void run();\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    // Should have space after //
    EXPECT_NE(result.find("// no space"), std::string::npos);
}

TEST(Formatter, EndOfLineCommentSpacing) {
    std::string source =
        "namespace app {\n"
        "  public:\n"
        "    void run();// tight comment\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    // Should have at least 2 spaces before //
    EXPECT_NE(result.find("void run();  // tight comment"), std::string::npos);
}

TEST(Formatter, TrailingNewline) {
    std::string source =
        "namespace app {\n"
        "  public:\n"
        "    void run();\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    // Should end with exactly one newline
    EXPECT_EQ(result.back(), '\n');
    EXPECT_NE(result[result.size() - 2], '\n');
}

TEST(Formatter, Idempotent) {
    std::string source =
        "// Header comment\n"
        "\n"
        "import utils;\n"
        "\n"
        "using IntPtr = std::cpp17::intptr_t;\n"
        "\n"
        "namespace engine {\n"
        "\n"
        "    public:\n"
        "        void render();\n"
        "\n"
        "    protected:\n"
        "        void init();\n"
        "}\n";

    std::string first = formatSource(source);
    ASSERT_FALSE(first.empty());
    std::string second = formatSource(first);
    ASSERT_FALSE(second.empty());
    EXPECT_EQ(first, second) << "Formatter should be idempotent";
}

// Regression — fuzz artifact crash-c3df25a4...: the formatter used to
// emit the literal placeholder `comptime if ...` for an IfDecl member,
// which the parser could not re-parse, breaking the round-trip. The
// formatter must reproduce the real `comptime if (cond) { ... }` shape
// so format -> re-parse -> format is a fixed point.
TEST(Formatter, ComptimeIfRoundTrips) {
    std::string source =
        "using Int = std::cpp17::int;\n"
        "namespace ct {\n"
        "  public:\n"
        "    comptime if (sizeof(Int) <= 8) {\n"
        "      void small_type();\n"
        "    }\n"
        "}\n";

    std::string first = formatSource(source);
    ASSERT_FALSE(first.empty());
    // The real construct is emitted, never the old placeholder.
    EXPECT_EQ(first.find("comptime if ..."), std::string::npos);
    EXPECT_NE(first.find("comptime if (sizeof(Int) <= 8) {"), std::string::npos);
    EXPECT_NE(first.find("void small_type();"), std::string::npos);

    std::string second = formatSource(first);
    ASSERT_FALSE(second.empty()) << "comptime-if output must re-parse diag-clean";
    EXPECT_EQ(first, second) << "comptime-if formatting must be idempotent";
}

// Regression — fuzz artifacts crash-a2147b7b... / crash-1236bdf9...: a
// `//` line comment inside a multi-line parameter list lexed on the
// signature line in pass one but on the closing `);` line in pass two,
// so an end-of-line-comment attachment keyed off the function's start
// line was applied unevenly across passes. The round-trip must be a
// fixed point regardless of where such comments land.
TEST(Formatter, CommentInsideMultiLineParamListIsIdempotent) {
    // A long signature forces the multi-line param rendering; the inline
    // `//` comments sit between parameters.
    std::string source =
        "namespace n {\n"
        "  public:\n"
        "    void wide(int aaaaa, int bbbbb, int ccccc, int ddddd, int eeeee, //x\n"
        "              int fffff, int ggggg, int hhhhh, int iiiii, int jjjjj, int kkkkk);\n"
        "}\n"
        "//tail\n";

    std::string first = formatSource(source);
    ASSERT_FALSE(first.empty());
    std::string second = formatSource(first);
    ASSERT_FALSE(second.empty()) << "param-comment output must re-parse diag-clean";
    EXPECT_EQ(first, second) << "comment-in-paren-list formatting must be idempotent";
}

TEST(Formatter, PipelineFormatting) {
    std::string source =
        "namespace pipeline {\n"
        "  public:\n"
        "    void run();\n"
        "    fn run {\n"
        "      fetch -> decode;\n"
        "      decode -> execute;\n"
        "      execute -> void;\n"
        "    }\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    EXPECT_NE(result.find("fetch -> decode;"), std::string::npos);
    EXPECT_NE(result.find("execute -> void;"), std::string::npos);
}

TEST(Formatter, RustStyleDecl) {
    std::string source =
        "namespace sig {\n"
        "  public:\n"
        "    compute(int x) -> int;\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    EXPECT_NE(result.find("compute(int x) -> int;"), std::string::npos);
}

TEST(Formatter, MultiReturnDecl) {
    std::string source =
        "using int = std::cpp17::int;\n"
        "using bool = std::cpp17::bool;\n"
        "\n"
        "namespace sig {\n"
        "  public:\n"
        "    compute(int x, int y) -> (int sum, int product);\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    EXPECT_NE(result.find("-> (int sum, int product)"), std::string::npos);
}

TEST(Formatter, StdImportFormatting) {
    std::string source =
        "std::import(\"cstdint\", uint64_t);\n"
        "\n"
        "namespace io {\n"
        "  public:\n"
        "    void write(uint64_t size);\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    EXPECT_NE(result.find("std::import(\"cstdint\", uint64_t);"), std::string::npos);
}

TEST(Formatter, TypeAliasSorting) {
    std::string source =
        "using SizeT = std::cpp17::size_t;\n"
        "using IntPtr = std::cpp17::intptr_t;\n"
        "\n"
        "namespace app {\n"
        "  public:\n"
        "    void run();\n"
        "}\n";

    std::string result = formatSource(source);
    ASSERT_FALSE(result.empty());
    // IntPtr should come before SizeT alphabetically
    auto intPos = result.find("using IntPtr");
    auto sizePos = result.find("using SizeT");
    ASSERT_NE(intPos, std::string::npos);
    ASSERT_NE(sizePos, std::string::npos);
    EXPECT_LT(intPos, sizePos);
}

TEST(Formatter, EmptySourceReturnsEmpty) {
    std::string result = formatSource("");
    // Empty input parses to empty file — should produce minimal output
    // (just a newline)
    EXPECT_FALSE(result.empty());
}

TEST(Formatter, ParseErrorReturnsEmpty) {
    // Unterminated block comment triggers a quick, clean parse error
    std::string result = formatSource("/* unterminated");
    // Parse error: formatter returns empty string
    EXPECT_TRUE(result.empty());
}
