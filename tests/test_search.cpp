// 正则搜索逻辑单元测试
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QCoreApplication>

#include <cstdio>

static int test_count = 0;
static int test_passed = 0;

#define TEST(name) fprintf(stderr, "  [RUN] %s\n", name)

#define CHECK(cond, msg) \
    do { \
        ++test_count; \
        if (cond) { ++test_passed; fprintf(stderr, "    [PASS] %s\n", msg); } \
        else { fprintf(stderr, "    [FAIL] %s\n", msg); } \
    } while (0)

// 复制 EditorTab.cpp 中的 buildPattern 逻辑
static QRegularExpression buildPattern(const QString &pattern, bool regex,
                                       bool caseSensitive, bool wholeWord)
{
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (!caseSensitive)
        options |= QRegularExpression::CaseInsensitiveOption;

    QString pat;
    if (regex) {
        pat = pattern;
    } else {
        pat = QRegularExpression::escape(pattern);
    }
    if (wholeWord) {
        pat = QStringLiteral("(?:^|\\W)") + pat + QStringLiteral("(?:$|\\W)");
    }
    QRegularExpression re(pat, options);
    return re;
}

struct SearchMatch {
    int line;
    int index;
    int length;
    QString text;
    QString context;
};

// 模拟 EditorTab::findAll 的搜索逻辑（含空模式检查）
static QVector<SearchMatch> findAllInText(const QString &text, const QString &pattern,
                                          bool regex, bool caseSensitive, bool wholeWord)
{
    QVector<SearchMatch> matches;
    if (pattern.isEmpty())
        return matches;

    QRegularExpression re = buildPattern(pattern, regex, caseSensitive, wholeWord);
    if (!re.isValid())
        return matches;

    QStringList lines = text.split('\n');
    for (int lineNum = 0; lineNum < lines.size(); ++lineNum) {
        QString lineText = lines[lineNum];
        auto it = re.globalMatch(lineText);
        while (it.hasNext()) {
            auto m = it.next();
            SearchMatch match;
            match.line = lineNum;
            match.index = m.capturedStart();
            match.length = m.capturedLength();
            match.text = m.captured(0);
            match.context = lineText;
            matches.append(match);
        }
    }
    return matches;
}

void test_plain_search()
{
    TEST("普通文本搜索");
    QString text = "hello world\nhello again\nworld hello";
    auto matches = findAllInText(text, "hello", false, false, false);
    CHECK(matches.size() == 3, "应找到 3 处 'hello'");
    if (matches.size() >= 3) {
        CHECK(matches[0].line == 0, "第1处: 行 0");
        CHECK(matches[0].index == 0, "第1处: 列 0");
        CHECK(matches[1].line == 1, "第2处: 行 1");
        CHECK(matches[2].line == 2, "第3处: 行 2");
        CHECK(matches[2].index == 6, "第3处: 列 6");
    }
}

void test_case_sensitive()
{
    TEST("区分大小写搜索");
    QString text = "Hello hello HELLO HeLLo";
    auto matches = findAllInText(text, "hello", false, true, false);
    CHECK(matches.size() == 1, "应找到 1 处 'hello'");
    if (!matches.isEmpty()) {
        CHECK(matches[0].index == 6, "应在第 6 列");
    }
}

void test_case_insensitive()
{
    TEST("不区分大小写搜索");
    QString text = "Hello hello HELLO";
    auto matches = findAllInText(text, "hello", false, false, false);
    CHECK(matches.size() == 3, "应找到 3 处");
}

void test_regex_search()
{
    TEST("正则表达式搜索");
    QString text = "abc123def456ghi789";
    auto matches = findAllInText(text, "\\d+", true, false, false);
    CHECK(matches.size() == 3, "应找到 3 处数字");
    if (matches.size() >= 3) {
        CHECK(matches[0].text == "123", "第1处: '123'");
        CHECK(matches[1].text == "456", "第2处: '456'");
        CHECK(matches[2].text == "789", "第3处: '789'");
    }
}

void test_regex_email()
{
    TEST("正则表达式: 邮箱");
    QString text = "contact: alice@example.com or bob@test.org";
    auto matches = findAllInText(text, "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}",
                                 true, false, false);
    CHECK(matches.size() == 2, "应找到 2 个邮箱");
    if (matches.size() >= 2) {
        CHECK(matches[0].text == "alice@example.com", "第1个邮箱");
        CHECK(matches[1].text == "bob@test.org", "第2个邮箱");
    }
}

void test_whole_word()
{
    TEST("整词匹配");
    QString text = "cat category cat concatenate cat";
    auto matches = findAllInText(text, "cat", false, false, true);
    CHECK(matches.size() >= 2, "至少应找到 2 处整词 'cat'");
}

void test_empty_pattern()
{
    TEST("空模式搜索");
    QString text = "hello world";
    auto matches = findAllInText(text, "", false, false, false);
    CHECK(matches.isEmpty(), "空模式应返回空结果");
}

void test_no_match()
{
    TEST("无匹配");
    QString text = "hello world";
    auto matches = findAllInText(text, "xyz", false, false, false);
    CHECK(matches.isEmpty(), "应无匹配");
}

void test_multiline_search()
{
    TEST("多行搜索");
    QString text = "line1: foo\nline2: bar\nline3: foo\nline4: baz";
    auto matches = findAllInText(text, "foo", false, false, false);
    CHECK(matches.size() == 2, "应找到 2 处 'foo'");
    if (matches.size() >= 2) {
        CHECK(matches[0].line == 0, "第1处: 行 0");
        CHECK(matches[1].line == 2, "第2处: 行 2");
    }
}

void test_unicode_search()
{
    TEST("Unicode 搜索");
    QString text = "hello world\nhello china\nbye";
    auto matches = findAllInText(text, "hello", false, false, false);
    CHECK(matches.size() == 2, "应找到 2 处 'hello'");
    if (matches.size() >= 2) {
        CHECK(matches[0].line == 0, "第1处: 行 0");
        CHECK(matches[1].line == 1, "第2处: 行 1");
    }
}

void test_regex_special_chars()
{
    TEST("正则特殊字符转义（非正则模式）");
    QString text = "price: $100.00 (tax)";
    auto matches = findAllInText(text, "$100.00", false, false, false);
    CHECK(matches.size() == 1, "应找到 1 处 '$100.00'");
    if (!matches.isEmpty()) {
        CHECK(matches[0].text == "$100.00", "匹配文本应一致");
    }
}

void test_regex_capture_groups()
{
    TEST("正则捕获组");
    QString text = "2026-08-12 and 2026-12-25";
    auto matches = findAllInText(text, "(\\d{4})-(\\d{2})-(\\d{2})", true, false, false);
    CHECK(matches.size() == 2, "应找到 2 个日期");
    if (!matches.isEmpty()) {
        CHECK(matches[0].text == "2026-08-12", "第1个日期");
        CHECK(matches[0].length == 10, "第1个日期长度应为 10");
    }
}

void test_pattern_validity()
{
    TEST("正则模式有效性");
    QRegularExpression valid = buildPattern("test", true, false, false);
    CHECK(valid.isValid(), "简单模式应有效");

    QRegularExpression invalid = buildPattern("[invalid", true, false, false);
    CHECK(!invalid.isValid(), "未闭合括号应无效");
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    fprintf(stderr, "\n=== Search Logic Tests ===\n\n");

    test_plain_search();
    test_case_sensitive();
    test_case_insensitive();
    test_regex_search();
    test_regex_email();
    test_whole_word();
    test_empty_pattern();
    test_no_match();
    test_multiline_search();
    test_unicode_search();
    test_regex_special_chars();
    test_regex_capture_groups();
    test_pattern_validity();

    fprintf(stderr, "\n=== Result: %d/%d passed ===\n\n", test_passed, test_count);
    return (test_passed == test_count) ? 0 : 1;
}
