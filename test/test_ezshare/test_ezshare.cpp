#include <unity.h>

#include <cctype>
#include <functional>
#include <set>

#include "ezshare.h"

using namespace ezshare;

void setUp() {}
void tearDown() {}

// A fake card folder: answers a probe the way /download?fname=<pattern> does, matching
// `*` and `?` case-insensitively against the folder's names.
struct FakeFolder {
    std::set<std::string> names;
    int probes = 0;
    std::string failOn;  // a pattern that gets neither 206 nor 404

    FakeFolder(std::set<std::string> n = {}) : names(std::move(n)) {}

    static bool match(const char* p, const char* s) {
        if (*p == '\0') return *s == '\0';
        if (*p == '*') return match(p + 1, s) || (*s != '\0' && match(p, s + 1));
        if (*s == '\0') return false;
        if (*p != '?' && std::tolower(static_cast<unsigned char>(*p)) != std::tolower(static_cast<unsigned char>(*s)))
            return false;
        return match(p + 1, s + 1);
    }

    ProbeStatus operator()(const std::string& pattern) {
        ++probes;
        if (pattern == failOn) return ProbeStatus::Failed;
        for (const auto& n : names) {
            if (match(pattern.c_str(), n.c_str())) return ProbeStatus::Hit;
        }
        return ProbeStatus::Miss;
    }
};

static const std::vector<std::string_view> kDigits6(6, "0123456789");

static void test_fake_card_matches_wildcards_ignoring_case() {
    FakeFolder card{{"20260221_194046_CSL.crc"}};
    TEST_ASSERT_TRUE(card("20260221_1*") == ProbeStatus::Hit);
    TEST_ASSERT_TRUE(card("20260221_194046_csl.CRC") == ProbeStatus::Hit);
    TEST_ASSERT_TRUE(card("20260221_19404?_CSL.crc") == ProbeStatus::Hit);
    TEST_ASSERT_TRUE(card("20260221_2*") == ProbeStatus::Miss);
}

static void test_expand_finds_every_timestamp() {
    FakeFolder card{{"20260221_194046_CSL.crc", "20260221_194046_EVE.edf", "20260221_194047_BRP.edf",
                     "20260221_231502_PLD.edf", "20260222_031502_SAD.edf", "Journal.dat"}};
    std::vector<std::string> found;
    TEST_ASSERT_TRUE(expand("20260221_", kDigits6, std::ref(card), found));
    TEST_ASSERT_EQUAL_size_t(3, found.size());
    TEST_ASSERT_EQUAL_STRING("20260221_194046", found[0].c_str());
    TEST_ASSERT_EQUAL_STRING("20260221_194047", found[1].c_str());
    TEST_ASSERT_EQUAL_STRING("20260221_231502", found[2].c_str());
}

static void test_expand_prunes_misses() {
    FakeFolder card{{"20260221_194046_CSL.crc"}};
    std::vector<std::string> found;
    TEST_ASSERT_TRUE(expand("20260221_", kDigits6, std::ref(card), found));
    TEST_ASSERT_EQUAL_size_t(1, found.size());
    // One hit per position, ten digits tried at each: no subtree under a miss is probed.
    TEST_ASSERT_EQUAL_INT(60, card.probes);
}

static void test_expand_empty_folder_finds_nothing() {
    FakeFolder card;
    std::vector<std::string> found;
    TEST_ASSERT_TRUE(expand("20260221_", kDigits6, std::ref(card), found));
    TEST_ASSERT_EQUAL_size_t(0, found.size());
    TEST_ASSERT_EQUAL_INT(10, card.probes);
}

static void test_expand_stops_on_probe_failure() {
    FakeFolder card{{"20260221_194046_CSL.crc"}};
    card.failOn = "20260221_19*";
    std::vector<std::string> found;
    TEST_ASSERT_FALSE(expand("20260221_", kDigits6, std::ref(card), found));
}

static void test_expand_settings_stems() {
    FakeFolder card{{"AGL.tgt", "AGL.crc", "ABR.log", "XYZ.tgt"}};
    std::vector<std::string> found;
    TEST_ASSERT_TRUE(expand("", std::vector<std::string_view>(3, kLetters), std::ref(card), found));
    TEST_ASSERT_EQUAL_size_t(3, found.size());
    TEST_ASSERT_EQUAL_STRING("ABR", found[0].c_str());
    TEST_ASSERT_EQUAL_STRING("AGL", found[1].c_str());
    TEST_ASSERT_EQUAL_STRING("XYZ", found[2].c_str());
}

static void test_time_alphabets_split_at_noon() {
    // Sessions dated with the folder's date start after noon; those with the next date, before.
    FakeFolder card{{"20260221_120000_EVE.edf", "20260221_235959_EVE.edf", "20260222_000000_EVE.edf",
                     "20260222_115959_EVE.edf"}};
    std::vector<std::string> found;
    TEST_ASSERT_TRUE(expand("20260221_", timeAlphabets(false), std::ref(card), found));
    TEST_ASSERT_TRUE(expand("20260222_", timeAlphabets(true), std::ref(card), found));
    TEST_ASSERT_EQUAL_size_t(4, found.size());
}

static void test_add_days_crosses_month_year_and_leap_day() {
    TEST_ASSERT_EQUAL_STRING("20270101", folderName(addDays(Date{2026, 12, 31}, 1)).c_str());
    TEST_ASSERT_EQUAL_STRING("20240229", folderName(addDays(Date{2024, 2, 28}, 1)).c_str());
    TEST_ASSERT_EQUAL_STRING("20240301", folderName(addDays(Date{2024, 2, 29}, 1)).c_str());
    TEST_ASSERT_EQUAL_STRING("20260131", folderName(addDays(Date{2026, 2, 1}, -1)).c_str());
}

static void test_days_between() {
    TEST_ASSERT_EQUAL_INT(7, daysBetween(Date{2026, 11, 12}, Date{2026, 11, 19}));
    TEST_ASSERT_EQUAL_INT(-1, daysBetween(Date{2027, 1, 1}, Date{2026, 12, 31}));
}

static void test_edf_start_date() {
    auto d = parseEdfStartDate("21.02.26");
    TEST_ASSERT_TRUE(d.has_value());
    TEST_ASSERT_EQUAL_STRING("20260221", folderName(*d).c_str());
    TEST_ASSERT_FALSE(parseEdfStartDate("21.13.26").has_value());
    TEST_ASSERT_FALSE(parseEdfStartDate("21-02-26").has_value());
    TEST_ASSERT_FALSE(parseEdfStartDate("21.02.2").has_value());
}

static void test_content_range_total() {
    TEST_ASSERT_EQUAL_UINT64(4190420, contentRangeTotal("bytes 236-243/4190420").value());
    TEST_ASSERT_EQUAL_UINT64(4, contentRangeTotal("bytes 0-3/4").value());
    TEST_ASSERT_FALSE(contentRangeTotal("bytes 0-3/*").has_value());
    TEST_ASSERT_FALSE(contentRangeTotal("").has_value());
    TEST_ASSERT_FALSE(contentRangeTotal("bytes 0-3/12x").has_value());
}

static void test_signature_ranges() {
    TEST_ASSERT_EQUAL_UINT32(236, signatureRange("STR.edf").first);
    TEST_ASSERT_EQUAL_UINT32(243, signatureRange("20260221_194046_BRP.edf").last);
    TEST_ASSERT_EQUAL_UINT32(0, signatureRange("AGL.crc").first);
    TEST_ASSERT_EQUAL_UINT32(7, signatureRange("AGL.crc").last);
    TEST_ASSERT_EQUAL_UINT32(0, signatureRange("AGL.tgt").last);
    TEST_ASSERT_EQUAL_UINT32(0, signatureRange("ABR.log").last);
    TEST_ASSERT_TRUE(keepsSignatureBytes("STR.edf"));
    TEST_ASSERT_TRUE(keepsSignatureBytes("STR.crc"));
    TEST_ASSERT_FALSE(keepsSignatureBytes("Identification.tgt"));
    TEST_ASSERT_FALSE(keepsSignatureBytes("ABR.log"));
}

static void test_crc_companions() {
    TEST_ASSERT_EQUAL_STRING("STR.crc", crcCompanion("STR.edf").c_str());
    TEST_ASSERT_EQUAL_STRING("AGL.crc", crcCompanion("AGL.tgt").c_str());
    TEST_ASSERT_EQUAL_STRING("", crcCompanion("ABR.log").c_str());
    TEST_ASSERT_EQUAL_STRING("", crcCompanion("AGL.crc").c_str());
}

static void test_downloads_missing_or_changed_files() {
    const Signature edf{4190420, "     123"};
    TEST_ASSERT_TRUE(needsDownload(std::nullopt, edf, false));
    TEST_ASSERT_FALSE(needsDownload(edf, edf, false));
    TEST_ASSERT_TRUE(needsDownload(Signature{4190420, "     122"}, edf, false));
    TEST_ASSERT_TRUE(needsDownload(Signature{4190000, "     123"}, edf, false));
}

static void test_downloads_when_companion_changed() {
    const Signature tgt{812, ""};
    TEST_ASSERT_TRUE(needsDownload(tgt, tgt, true));
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

static void test_parse_folder_name() {
    const auto d = parseFolderName("20261119");
    TEST_ASSERT_TRUE(d.has_value());
    TEST_ASSERT_EQUAL_INT(2026, d->year);
    TEST_ASSERT_EQUAL_INT(11, d->month);
    TEST_ASSERT_EQUAL_INT(19, d->day);
    TEST_ASSERT_FALSE(parseFolderName("2026111").has_value());
    TEST_ASSERT_FALSE(parseFolderName("20261340").has_value());
    TEST_ASSERT_FALSE(parseFolderName("").has_value());
}

static void test_backfill_floor() {
    const Date lookbackStart{2026, 11, 13};
    // No card start date: only the lookback window.
    TEST_ASSERT_EQUAL_STRING("20261113", folderName(backfillFloor(lookbackStart, std::nullopt, std::nullopt)).c_str());
    // First pass: back to the card's first day.
    TEST_ASSERT_EQUAL_STRING("20250911",
                             folderName(backfillFloor(lookbackStart, Date{2025, 9, 11}, std::nullopt)).c_str());
    // Everything through 2026-11-11 already checked: start the day after.
    TEST_ASSERT_EQUAL_STRING(
        "20261112", folderName(backfillFloor(lookbackStart, Date{2025, 9, 11}, Date{2026, 11, 11})).c_str());
    // Checked past the window's start: never later than the window.
    TEST_ASSERT_EQUAL_STRING(
        "20261113", folderName(backfillFloor(lookbackStart, Date{2025, 9, 11}, Date{2026, 11, 18})).c_str());
    // A card that starts after the checked date: the card's start wins.
    TEST_ASSERT_EQUAL_STRING(
        "20261101", folderName(backfillFloor(lookbackStart, Date{2026, 11, 1}, Date{2026, 10, 1})).c_str());
    // A card that starts inside the window: the window's start.
    TEST_ASSERT_EQUAL_STRING("20261113",
                             folderName(backfillFloor(lookbackStart, Date{2026, 11, 15}, std::nullopt)).c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fake_card_matches_wildcards_ignoring_case);
    RUN_TEST(test_expand_finds_every_timestamp);
    RUN_TEST(test_expand_prunes_misses);
    RUN_TEST(test_expand_empty_folder_finds_nothing);
    RUN_TEST(test_expand_stops_on_probe_failure);
    RUN_TEST(test_expand_settings_stems);
    RUN_TEST(test_time_alphabets_split_at_noon);
    RUN_TEST(test_add_days_crosses_month_year_and_leap_day);
    RUN_TEST(test_days_between);
    RUN_TEST(test_edf_start_date);
    RUN_TEST(test_content_range_total);
    RUN_TEST(test_signature_ranges);
    RUN_TEST(test_crc_companions);
    RUN_TEST(test_downloads_missing_or_changed_files);
    RUN_TEST(test_downloads_when_companion_changed);
    RUN_TEST(test_lists_folders_inside_lookback);
    RUN_TEST(test_lookback_crosses_month_and_year);
    RUN_TEST(test_lists_old_folders_never_copied);
    RUN_TEST(test_never_lists_non_date_folders);
    RUN_TEST(test_safe_names);
    RUN_TEST(test_json_escape);
    RUN_TEST(test_window);
    RUN_TEST(test_parse_folder_name);
    RUN_TEST(test_backfill_floor);
    return UNITY_END();
}
