#include <unity.h>

#include "ezshare.h"

using namespace ezshare;

void setUp() {}
void tearDown() {}

// Lines in the format community clients document for the card's /dir listing.
static const char* kFileLine =
    "2024- 6- 7   20:50:24          64KB  <a href=\"http://192.168.4.1/download?file=JOURNAL.DAT\"> Journal.dat</a>";
static const char* kDatalogFileLine =
    "2026-11-18   12:00:04         808KB  <a href=\"http://192.168.4.1/download?file=DATALOG%5C20261118%5C20261~1.EDF\"> "
    "20261118_120004_EVE.edf</a>";
static const char* kDirLine =
    "2026-11-18   12:00:04   &lt;DIR&gt;   <a href=\"dir?dir=A:%5CDATALOG%5C20261118\"> 20261118</a>";

static void test_parses_file_line() {
    auto parsed = parseLine(kFileLine);
    auto* file = std::get_if<File>(&parsed);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_STRING("Journal.dat", file->name.c_str());
    TEST_ASSERT_EQUAL_STRING("file=JOURNAL.DAT", file->query.c_str());
    TEST_ASSERT_EQUAL_UINT32(64, file->sizeKb);
    TEST_ASSERT_EQUAL_INT(2024, file->modified.year);
    TEST_ASSERT_EQUAL_INT(6, file->modified.month);
    TEST_ASSERT_EQUAL_INT(7, file->modified.day);
    TEST_ASSERT_EQUAL_INT(20, file->modified.hour);
    TEST_ASSERT_EQUAL_INT(50, file->modified.minute);
    TEST_ASSERT_EQUAL_INT(24, file->modified.second);
}

static void test_keeps_long_name_and_encoded_query() {
    auto parsed = parseLine(kDatalogFileLine);
    auto* file = std::get_if<File>(&parsed);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_STRING("20261118_120004_EVE.edf", file->name.c_str());
    TEST_ASSERT_EQUAL_STRING("file=DATALOG%5C20261118%5C20261~1.EDF", file->query.c_str());
    TEST_ASSERT_EQUAL_UINT32(808, file->sizeKb);
}

static void test_parses_directory_line() {
    auto parsed = parseLine(kDirLine);
    auto* dir = std::get_if<Directory>(&parsed);
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_EQUAL_STRING("20261118", dir->name.c_str());
    TEST_ASSERT_EQUAL_STRING("dir=A:%5CDATALOG%5C20261118", dir->query.c_str());
    TEST_ASSERT_EQUAL_INT(2026, dir->modified.year);
}

static void test_tolerates_trailing_carriage_return() {
    std::string line = std::string(kFileLine) + "\r";
    TEST_ASSERT_TRUE(std::holds_alternative<File>(parseLine(line)));
}

static void test_lines_without_links_are_not_entries() {
    TEST_ASSERT_TRUE(std::holds_alternative<NotAnEntry>(parseLine("")));
    TEST_ASSERT_TRUE(std::holds_alternative<NotAnEntry>(parseLine("<html><body><pre>")));
    TEST_ASSERT_TRUE(std::holds_alternative<NotAnEntry>(parseLine("   ")));
}

static void test_unknown_size_unit_is_malformed() {
    TEST_ASSERT_TRUE(std::holds_alternative<Malformed>(parseLine(
        "2026-11-18   12:00:04           4MB  <a href=\"http://192.168.4.1/download?file=BIG.EDF\"> Big.edf</a>")));
}

static void test_missing_timestamp_is_malformed() {
    TEST_ASSERT_TRUE(std::holds_alternative<Malformed>(
        parseLine("          64KB  <a href=\"http://192.168.4.1/download?file=JOURNAL.DAT\"> Journal.dat</a>")));
}

static void test_unknown_link_kind_is_malformed() {
    TEST_ASSERT_TRUE(std::holds_alternative<Malformed>(
        parseLine("2026-11-18   12:00:04   64KB  <a href=\"http://192.168.4.1/other?x=1\"> x</a>")));
}

static void test_out_of_range_timestamp_is_malformed() {
    TEST_ASSERT_TRUE(std::holds_alternative<Malformed>(
        parseLine("2026-13-18   12:00:04   64KB  <a href=\"http://192.168.4.1/download?file=A.EDF\"> a.edf</a>")));
}

static void test_lists_folders_inside_lookback() {
    Date today{2026, 11, 19};
    TEST_ASSERT_TRUE(shouldListDatalogFolder("20261119", today, 7, true));
    TEST_ASSERT_TRUE(shouldListDatalogFolder("20261112", today, 7, true));
    TEST_ASSERT_FALSE(shouldListDatalogFolder("20261111", today, 7, true));
}

static void test_lookback_crosses_month_and_year() {
    TEST_ASSERT_TRUE(shouldListDatalogFolder("20261228", Date{2027, 1, 3}, 7, true));
    TEST_ASSERT_FALSE(shouldListDatalogFolder("20261226", Date{2027, 1, 3}, 7, true));
}

static void test_lists_old_folders_never_copied() {
    TEST_ASSERT_TRUE(shouldListDatalogFolder("20260301", Date{2026, 11, 19}, 7, false));
}

static void test_never_lists_non_date_folders() {
    TEST_ASSERT_FALSE(shouldListDatalogFolder("..", Date{2026, 11, 19}, 7, false));
    TEST_ASSERT_FALSE(shouldListDatalogFolder("2026111", Date{2026, 11, 19}, 7, false));
    TEST_ASSERT_FALSE(shouldListDatalogFolder("2026111X", Date{2026, 11, 19}, 7, false));
    TEST_ASSERT_FALSE(shouldListDatalogFolder("20261340", Date{2026, 11, 19}, 7, false));
}

static void test_size_matches_within_kb_rounding() {
    TEST_ASSERT_TRUE(sizeMatchesKb(808 * 1024, 808));
    TEST_ASSERT_TRUE(sizeMatchesKb(808 * 1024 - 1023, 808));
    TEST_ASSERT_TRUE(sizeMatchesKb(808 * 1024 + 1023, 808));
    TEST_ASSERT_FALSE(sizeMatchesKb(808 * 1024 + 1024, 808));
    TEST_ASSERT_FALSE(sizeMatchesKb(806 * 1024, 808));
    TEST_ASSERT_TRUE(sizeMatchesKb(100, 0));
}

static void test_downloads_missing_or_changed_files() {
    const int64_t when = 1795000000;
    TEST_ASSERT_TRUE(needsDownload(std::nullopt, 808, when));
    TEST_ASSERT_FALSE(needsDownload(LocalFile{808 * 1024, when}, 808, when));
    TEST_ASSERT_TRUE(needsDownload(LocalFile{808 * 1024, when}, 808, when + 60));
    TEST_ASSERT_TRUE(needsDownload(LocalFile{400 * 1024, when}, 808, when));
}

static void test_tolerates_fat_two_second_times() {
    const int64_t odd = 1795000001;
    TEST_ASSERT_FALSE(needsDownload(LocalFile{808 * 1024, odd - 1}, 808, odd));
    TEST_ASSERT_TRUE(needsDownload(LocalFile{808 * 1024, odd - 3}, 808, odd));
}

static void test_safe_names() {
    TEST_ASSERT_TRUE(isSafeName("20261118_120004_EVE.edf"));
    TEST_ASSERT_TRUE(isSafeName("STR.edf"));
    TEST_ASSERT_FALSE(isSafeName(""));
    TEST_ASSERT_FALSE(isSafeName("."));
    TEST_ASSERT_FALSE(isSafeName(".."));
    TEST_ASSERT_FALSE(isSafeName("a/b"));
    TEST_ASSERT_FALSE(isSafeName("a\\b"));
    TEST_ASSERT_FALSE(isSafeName("x.part"));
    TEST_ASSERT_FALSE(isSafeName(std::string("a\nb")));
}

static void test_json_escape() {
    TEST_ASSERT_EQUAL_STRING("plain", jsonEscape("plain").c_str());
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c", jsonEscape("a\"b\\c").c_str());
    TEST_ASSERT_EQUAL_STRING("x\\ny\\rz\\t", jsonEscape("x\ny\rz\t").c_str());
    TEST_ASSERT_EQUAL_STRING("\\u0001", jsonEscape(std::string(1, '\x01')).c_str());
    TEST_ASSERT_EQUAL_STRING("<a href=\\\"x\\\">", jsonEscape("<a href=\"x\">").c_str());
}

static void test_window() {
    TEST_ASSERT_TRUE(inWindow(9, 9, 18));
    TEST_ASSERT_TRUE(inWindow(17, 9, 18));
    TEST_ASSERT_FALSE(inWindow(18, 9, 18));
    TEST_ASSERT_FALSE(inWindow(8, 9, 18));
    TEST_ASSERT_TRUE(inWindow(23, 22, 6));
    TEST_ASSERT_TRUE(inWindow(2, 22, 6));
    TEST_ASSERT_FALSE(inWindow(12, 22, 6));
    TEST_ASSERT_TRUE(inWindow(3, 0, 0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parses_file_line);
    RUN_TEST(test_keeps_long_name_and_encoded_query);
    RUN_TEST(test_parses_directory_line);
    RUN_TEST(test_tolerates_trailing_carriage_return);
    RUN_TEST(test_lines_without_links_are_not_entries);
    RUN_TEST(test_unknown_size_unit_is_malformed);
    RUN_TEST(test_missing_timestamp_is_malformed);
    RUN_TEST(test_unknown_link_kind_is_malformed);
    RUN_TEST(test_out_of_range_timestamp_is_malformed);
    RUN_TEST(test_lists_folders_inside_lookback);
    RUN_TEST(test_lookback_crosses_month_and_year);
    RUN_TEST(test_lists_old_folders_never_copied);
    RUN_TEST(test_never_lists_non_date_folders);
    RUN_TEST(test_size_matches_within_kb_rounding);
    RUN_TEST(test_downloads_missing_or_changed_files);
    RUN_TEST(test_tolerates_fat_two_second_times);
    RUN_TEST(test_safe_names);
    RUN_TEST(test_json_escape);
    RUN_TEST(test_window);
    return UNITY_END();
}
