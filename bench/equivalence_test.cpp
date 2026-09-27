// Differential test for the optimized text processing and search functions.
//
// The `reference` namespace contains the previous implementations (std::wregex based indexing,
// Boyer-Moore search, 10 KB checksum reads) verbatim. The test runs them and the current ones on
// every page of the given PDFs and on randomly generated inputs, and reports any difference.
//
// usage: sioyek_equivalence_test file.pdf...

#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <regex>
#include <string>
#include <vector>
#include <unistd.h>

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <mupdf/fitz.h>

#include "book.h"
#include "checksum.h"
#include "utils.h"

bool are_stext_chars_far_enough_for_equation(fz_stext_char* first, fz_stext_char* second);
void find_regex_matches_in_stext_page(const std::vector<fz_stext_char*>& flat_chars,
    const std::wregex& regex,
    std::vector<std::pair<int, int>>& match_ranges, std::vector<std::wstring>& match_texts);
bool is_string_titlish(const std::wstring& str);
bool is_delimeter(int c);
std::function<bool(const wchar_t&, const wchar_t&)> get_pred(SearchCaseSensitivity cs, const std::wstring& query);
std::function<wchar_t(const wchar_t&)> get_hash(SearchCaseSensitivity cs, const std::wstring& query);

namespace reference {

bool is_stext_line_rtl(fz_stext_line* line) {
    float rtl_count = 0.0f;
    float total_count = 0.0f;
    LL_ITER(ch, line->first_char) {
        if (is_rtl(ch->c)) {
            rtl_count += 1.0f;
        }
        total_count += 1.0f;
    }
    return ((rtl_count / total_count) > 0.5f);
}

std::vector<fz_stext_char*> reorder_stext_line(fz_stext_line* line) {

    std::vector<fz_stext_char*> reordered_chars;

    bool rtl = reference::is_stext_line_rtl(line);

    LL_ITER(ch, line->first_char) {
        reordered_chars.push_back(ch);
    }

    if (rtl) {
        std::stable_sort(reordered_chars.begin(), reordered_chars.end(), [](fz_stext_char* lhs, fz_stext_char* rhs) {
            return lhs->quad.lr.x >= rhs->quad.lr.x;
            });
    }
    else {
        std::stable_sort(reordered_chars.begin(), reordered_chars.end(), [](fz_stext_char* lhs, fz_stext_char* rhs) {
            return (lhs->quad.lr.x <= rhs->quad.lr.x) && (lhs->quad.ll.x < rhs->quad.ll.x);
            });
    }
    return reordered_chars;
}

void get_flat_chars_from_block(fz_stext_block* block, std::vector<fz_stext_char*>& flat_chars, bool dehyphenate) {
    if (block->type == FZ_STEXT_BLOCK_TEXT) {
        LL_ITER(line, block->u.t.first_line) {
            std::vector<fz_stext_char*> reordered_chars = reference::reorder_stext_line(line);
            for (auto ch : reordered_chars) {
                if (ch->c == 65533) {
                    ch->c = ' ';
                }

                if (dehyphenate) {
                    if (ch->c == '-' && (ch->next == nullptr)) {
                        continue;
                    }
                }

                flat_chars.push_back(ch);
            }
        }
    }
}

void get_flat_chars_from_stext_page(fz_stext_page* stext_page, std::vector<fz_stext_char*>& flat_chars, bool dehyphenate) {
    LL_ITER(block, stext_page->first_block) {
        reference::get_flat_chars_from_block(block, flat_chars, dehyphenate);
    }
}

bool is_delimeter(int c) {
    std::vector<char> delimeters = { ' ', '\n', ';', ',' };
    return std::find(delimeters.begin(), delimeters.end(), c) != delimeters.end();
}

std::vector<std::wstring> find_all_regex_matches(std::wstring haystack,
    const std::wstring& regex_string,
    std::vector<std::pair<int, int>>* match_ranges) {

    std::wregex regex(regex_string);
    std::wsmatch match;
    std::vector<std::wstring> res;
    int skipped_length = 0;

    while (std::regex_search(haystack, match, regex)) {
        for (size_t i = 0; i < match.size(); i++) {
            if (match[i].matched) {
                res.push_back(match[i].str());
                if (match_ranges) {
                    int begin_index = match[i].first - haystack.begin();
                    int match_length = match[i].length();
                    match_ranges->push_back(std::make_pair(skipped_length + begin_index, skipped_length + begin_index + match_length-1));
                }
            }
        }
        skipped_length += match.prefix().length() + match.length();
        haystack = match.suffix();
    }
    return res;

}

void find_regex_matches_in_stext_page(const std::vector<fz_stext_char*>& flat_chars,
    const std::wregex& regex,
    std::vector<std::pair<int, int>>& match_ranges, std::vector<std::wstring>& match_texts) {

    std::wstring page_string;
    std::vector<int> indices;

    get_text_from_flat_chars(flat_chars, page_string, indices);

    std::wsmatch match;

    int offset = 0;
    while (std::regex_search(page_string, match, regex)) {
        int start_index = offset + match.position();
        int end_index = start_index + match.length() - 1;
        match_ranges.push_back(std::make_pair(indices[start_index], indices[end_index]));
        match_texts.push_back(match.str());

        int old_length = page_string.size();
        page_string = match.suffix();
        int new_length = page_string.size();

        offset += (old_length - new_length);
    }
}

void index_generic(const std::vector<fz_stext_char*>& flat_chars, int page_number, std::vector<IndexedData>& indices) {

    std::wstring page_string;
    std::vector<std::optional<fz_rect>> page_rects;

    for (auto ch : flat_chars) {
        page_string.push_back(ch->c);
        page_rects.push_back(fz_rect_from_quad(ch->quad));
        if (ch->next == nullptr) {
            page_string.push_back('\n');
            page_rects.push_back({});
        }
    }

    std::wregex index_dst_regex(L"(^|\n)[A-Z][a-zA-Z]{2,}\\.?[ \t]+[0-9]+(\\.[0-9]+)*");
    std::wsmatch match;


    int offset = 0;
    while (std::regex_search(page_string, match, index_dst_regex)) {

        IndexedData new_data;
        new_data.page = page_number;
        std::wstring match_string = match.str();
        new_data.text = strip_string(match_string);
        new_data.y_offset = 0.0f;

        int match_start_index = match.position();
        int match_size = match_string.size();
        for (int i = 0; i < match_size; i++) {
            int index = offset + match_start_index + i;
            if (page_rects[index]) {
                new_data.y_offset = page_rects[index].value().y0;
                break;
            }
        }
        offset += match_start_index + match_size;
        page_string = match.suffix();

        indices.push_back(new_data);
    }
}

void index_equations(const std::vector<fz_stext_char*>& flat_chars, int page_number, std::map<std::wstring, std::vector<IndexedData>>& indices) {
    std::wregex regex(L"\\([0-9]+(\\.[0-9]+)*\\)");
    std::vector<std::pair<int, int>> match_ranges;
    std::vector<std::wstring> match_texts;

    reference::find_regex_matches_in_stext_page(flat_chars, regex, match_ranges, match_texts);

    for (size_t i = 0; i < match_ranges.size(); i++) {
        auto [start_index, end_index] = match_ranges[i];
        if (start_index == -1 || end_index == -1) {
            break;
        }


        if (((start_index > 0) && are_stext_chars_far_enough_for_equation(flat_chars[start_index - 1], flat_chars[start_index]))) {

            std::wstring match_text = match_texts[i].substr(1, match_texts[i].size() - 2);
            IndexedData indexed_equation;
            indexed_equation.page = page_number;
            indexed_equation.text = match_text;
            indexed_equation.y_offset = flat_chars[start_index]->quad.ll.y;
            if (indices.find(match_text) == indices.end()) {
                indices[match_text] = std::vector<IndexedData>();
                indices[match_text].push_back(indexed_equation);
            }
            else {
                indices[match_text].push_back(indexed_equation);
            }
        }
    }

}

bool is_string_titlish(const std::wstring& str) {
    if (str.size() <= 5 || str.size() >= 60) {
        return false;
    }
    std::wregex regex(L"([0-9IVXC]+\\.)+([0-9IVXC]+)*");
    std::wsmatch match;

    std::regex_search(str, match, regex);
    int pos = match.position();
    int size = match.length();
    return (size > 0) && (pos == 0);
}

std::vector<SearchResult> search_text_with_index(const std::wstring& super_fast_search_index,
    const std::vector<int>& page_begin_indices,
    const std::wstring& query,
    SearchCaseSensitivity case_sensitive,
    int begin_page,
    int min_page,
    int max_page) {

    std::vector<SearchResult> output;
    std::vector<SearchResult> before_results;

    if (min_page < 0) {
        min_page = 0;
    }

    if (max_page > page_begin_indices.size() - 1) {
        max_page = page_begin_indices.size() - 1;
    }

    int begin_index = page_begin_indices[min_page];
    int end_index = max_page == page_begin_indices.size()-1? super_fast_search_index.size() : page_begin_indices[max_page+1];
    bool is_before = true;

    auto pred = get_pred(case_sensitive, query);
    auto hash = get_hash(case_sensitive, query);
    auto searcher = std::boyer_moore_searcher(query.begin(), query.end(), hash, pred);
    auto it = std::search(
        super_fast_search_index.begin() + begin_index,
        super_fast_search_index.begin() + end_index,
        searcher);

    int match_page = min_page;

    for (; it != super_fast_search_index.end(); it = std::search(it + 1, super_fast_search_index.end(), searcher)) {
        int start_index = it - super_fast_search_index.begin();

        while ((match_page < page_begin_indices.size() - 1) && page_begin_indices[match_page + 1] <= start_index) match_page++;

        if (match_page >= begin_page) {
            is_before = false;
        }

        int end_index = start_index + query.size();

        SearchResult res;
        res.page = match_page;
        res.begin_index_in_page = start_index - page_begin_indices[match_page];
        res.end_index_in_page = end_index - page_begin_indices[match_page];

        if (!((match_page < min_page) || (match_page > max_page))) {
            if (is_before) {
                before_results.push_back(res);
            }
            else {
                output.push_back(res);
            }
        }
    }

    output.insert(output.end(), before_results.begin(), before_results.end());
    return output;

}

std::string compute_checksum(const QString& file_name, QCryptographicHash::Algorithm hash_algorithm)
{
    QFile infile(file_name);
    qint64 file_size = infile.size();
    const qint64 buffer_size = 10240;

    if (infile.open(QIODevice::ReadOnly))
    {
        char buffer[buffer_size];
        int bytes_read;
        int read_size = qMin(file_size, buffer_size);

        QCryptographicHash hash(hash_algorithm);
        while (read_size > 0 && (bytes_read = infile.read(buffer, read_size)) > 0)
        {
            file_size -= bytes_read;
            hash.addData(buffer, bytes_read);
            read_size = qMin(file_size, buffer_size);
        }

        infile.close();
        return QString(hash.result().toHex()).toStdString();
    }
    return "";
}

} // namespace reference

namespace {

int failures = 0;
long long checks = 0;

void check(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        failures++;
        if (failures < 30) fprintf(stderr, "MISMATCH: %s\n", what.c_str());
    }
}

bool same(const std::vector<IndexedData>& a, const std::vector<IndexedData>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].page != b[i].page || a[i].text != b[i].text || a[i].y_offset != b[i].y_offset) return false;
    }
    return true;
}

bool same(const std::map<std::wstring, std::vector<IndexedData>>& a, const std::map<std::wstring, std::vector<IndexedData>>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [k, v] : a) {
        auto it = b.find(k);
        if (it == b.end() || !same(v, it->second)) return false;
    }
    return true;
}

bool same(const std::vector<SearchResult>& a, const std::vector<SearchResult>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].page != b[i].page || a[i].begin_index_in_page != b[i].begin_index_in_page || a[i].end_index_in_page != b[i].end_index_in_page) return false;
    }
    return true;
}

std::string narrow(const std::wstring& s) {
    return QString::fromStdWString(s).toStdString();
}

const std::vector<std::wstring> regexes = {
    L"[a-zA-Z]{2,}\\.?[ \t]+[0-9]+(\\.[0-9]+)*",
    L"\\[[a-zA-Z0-9, ]+\\]",
    L"\\([0-9]+(\\.[0-9]+)*\\)",
    L"^[A-Z][a-z]+",
    L"(^|\n)[A-Z][a-zA-Z]{2,}\\.?[ \t]+[0-9]+",
    L"\\b[a-z]{3}\\b",
};

void compare_flat_chars_processing(const std::vector<fz_stext_char*>& flat_chars, int page, const std::string& where) {
    {
        std::vector<IndexedData> a, b;
        reference::index_generic(flat_chars, page, a);
        index_generic(flat_chars, page, b);
        check(same(a, b), "index_generic " + where);
    }
    {
        std::map<std::wstring, std::vector<IndexedData>> a, b;
        reference::index_equations(flat_chars, page, a);
        index_equations(flat_chars, page, b);
        check(same(a, b), "index_equations " + where);
    }
    for (const auto& r : regexes) {
        std::wregex regex(r);
        std::vector<std::pair<int, int>> ra, rb;
        std::vector<std::wstring> ta, tb;
        reference::find_regex_matches_in_stext_page(flat_chars, regex, ra, ta);
        find_regex_matches_in_stext_page(flat_chars, regex, rb, tb);
        check(ra == rb && ta == tb, "find_regex_matches_in_stext_page " + narrow(r) + " " + where);

        std::wstring text;
        std::vector<int> indices;
        get_text_from_flat_chars(flat_chars, text, indices);
        std::vector<std::pair<int, int>> ma, mb;
        auto xa = reference::find_all_regex_matches(text, r, &ma);
        auto xb = find_all_regex_matches(text, r, &mb);
        check(xa == xb && ma == mb, "find_all_regex_matches " + narrow(r) + " " + where);
    }
}

void compare_search(const std::wstring& index, const std::vector<int>& page_begin_indices, const std::wstring& query,
                    SearchCaseSensitivity cs, int begin_page, int min_page, int max_page, const std::string& where) {
    auto a = reference::search_text_with_index(index, page_begin_indices, query, cs, begin_page, min_page, max_page);
    auto b = search_text_with_index(index, page_begin_indices, query, cs, begin_page, min_page, max_page);
    if (same(a, b)) {
        check(true, "");
        return;
    }
    // The previous implementation bounded only its first search to the page range, so a match that
    // starts on max_page but ends after it was found only if it wasn't the first match. The new one
    // always includes it. Accept exactly that difference.
    int end_index = (max_page >= (int)page_begin_indices.size() - 1) ? (int)index.size() : page_begin_indices[max_page + 1];
    std::vector<SearchResult> b_without;
    for (const auto& r : b) {
        int start = page_begin_indices[r.page] + r.begin_index_in_page;
        bool straddles = start < end_index && start + (int)query.size() > end_index;
        if (!(straddles && a.empty())) b_without.push_back(r);
    }
    bool ok = same(a, b_without);
    if (ok) {
        printf("  note: range-end straddling match now reported (%s)\n", where.c_str());
    }
    check(ok, "search_text_with_index '" + narrow(query) + "' " + where +
                  " ref=" + std::to_string(a.size()) + " new=" + std::to_string(b.size()));
}

std::wstring random_case(std::wstring s, std::mt19937& rng) {
    for (auto& c : s) {
        if (rng() % 2) c = std::towupper(c);
        else c = std::towlower(c);
    }
    return s;
}

void search_tests(const std::wstring& index, const std::vector<int>& page_begin_indices, std::mt19937& rng, int n_queries, const std::string& where) {
    if (index.empty() || page_begin_indices.empty()) return;
    int num_pages = page_begin_indices.size();
    SearchCaseSensitivity modes[] = { SearchCaseSensitivity::CaseSensitive, SearchCaseSensitivity::CaseInsensitive, SearchCaseSensitivity::SmartCase };
    for (int q = 0; q < n_queries; q++) {
        size_t len = 1 + rng() % 12;
        size_t pos = rng() % index.size();
        std::wstring query = index.substr(pos, len);
        if (rng() % 3 == 0) query = random_case(query, rng);
        if (rng() % 10 == 0) query += L"zq";
        auto cs = modes[rng() % 3];
        int min_page = 0, max_page = num_pages - 1, begin_page = 0;
        if (rng() % 2) {
            min_page = rng() % num_pages;
            max_page = min_page + rng() % std::max(1, num_pages - min_page);
            begin_page = min_page + rng() % std::max(1, max_page - min_page + 1);
        }
        compare_search(index, page_begin_indices, query, cs, begin_page, min_page, max_page,
                       where + " cs=" + std::to_string((int)cs) + " pages=" + std::to_string(min_page) + ".." + std::to_string(max_page));
    }
}

// Pages of random characters drawn mostly from the characters the regexes care about.
void fuzz_text_processing(std::mt19937& rng) {
    const std::wstring alphabet = L"AAABFSTXCVIabcdefghxyz0123456789012345....((()))   \t\t\n[],-;";
    for (int page = 0; page < 3000; page++) {
        int n = rng() % 300;
        std::vector<fz_stext_char> storage(n);
        std::vector<fz_stext_char*> chars(n);
        for (int i = 0; i < n; i++) {
            fz_stext_char& ch = storage[i];
            memset(&ch, 0, sizeof(ch));
            ch.c = alphabet[rng() % alphabet.size()];
            float x = (float)(i % 40) * 6.0f;
            float y = (float)(i / 40) * 12.0f + (float)(rng() % 3);
            if (rng() % 7 == 0) x += 60.0f; // gaps for the equation distance heuristic
            ch.quad.ll = fz_make_point(x, y + 10);
            ch.quad.lr = fz_make_point(x + 5, y + 10);
            ch.quad.ul = fz_make_point(x, y);
            ch.quad.ur = fz_make_point(x + 5, y);
            ch.origin = ch.quad.ll;
            chars[i] = &ch;
        }
        for (int i = 0; i < n; i++) {
            storage[i].next = (i + 1 < n && rng() % 12 != 0) ? &storage[i + 1] : nullptr;
        }
        compare_flat_chars_processing(chars, page, "fuzz page " + std::to_string(page));
    }

    const std::wstring titlish_alphabet = L"0123IVXC.. aBx";
    for (int i = 0; i < 200000; i++) {
        std::wstring s;
        int n = rng() % 70;
        for (int j = 0; j < n; j++) s.push_back(titlish_alphabet[rng() % titlish_alphabet.size()]);
        check(reference::is_string_titlish(s) == is_string_titlish(s), "is_string_titlish '" + narrow(s) + "'");
    }
    for (int c = -1; c < 70000; c++) {
        check(reference::is_delimeter(c) == is_delimeter(c), "is_delimeter " + std::to_string(c));
    }
}

// An index with non ASCII text, including characters whose case mapping crosses the ASCII boundary.
void fuzz_unicode_search(std::mt19937& rng) {
    const std::wstring alphabet = L"aAbBkKsSiIeE \x212A\x017F\x0131\x0130\x00E9\x00C9\x03B1\x0391\x0436\x0416\x4E2D\x00DF\x1E9E";
    for (int round = 0; round < 40; round++) {
        std::wstring index;
        std::vector<int> page_begin_indices;
        int pages = 1 + rng() % 40;
        for (int p = 0; p < pages; p++) {
            page_begin_indices.push_back(index.size());
            int n = rng() % 400;
            for (int i = 0; i < n; i++) index.push_back(alphabet[rng() % alphabet.size()]);
        }
        search_tests(index, page_begin_indices, rng, 200, "unicode round " + std::to_string(round));
    }
}

void checksum_tests(const std::vector<QString>& files) {
    QTemporaryDir dir;
    std::vector<QString> all = files;
    std::mt19937 rng(7);
    for (qint64 size : { 0LL, 1LL, 10239LL, 10240LL, 10241LL, (1LL << 20) - 1, 1LL << 20, (1LL << 20) + 1, 3LL * (1 << 20) + 17 }) {
        QString path = dir.filePath(QString("f%1").arg(size));
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        QByteArray data(size, 0);
        for (auto& c : data) c = (char)(rng() & 0xFF);
        f.write(data);
        f.close();
        all.push_back(path);
    }
    all.push_back(dir.filePath("does_not_exist"));
    for (const auto& path : all) {
        check(reference::compute_checksum(path, QCryptographicHash::Md5) == compute_checksum(path, QCryptographicHash::Md5),
              "checksum " + path.toStdString());
    }
}

} // namespace

extern "C" int test_main(int argc, char** argv) {
    QCoreApplication app(argc, argv); // sets the locale from the environment, like sioyek
    std::mt19937 rng(12345);

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);

    std::vector<QString> files;
    for (int i = 1; i < argc; i++) files.push_back(QString::fromLocal8Bit(argv[i]));

    for (const auto& file : files) {
        printf("%s\n", file.toStdString().c_str());
        fz_document* doc = open_document_with_file_name(ctx, file.toStdWString());
        int n = fz_count_pages(ctx, doc);
        std::wstring index;
        std::vector<int> page_begin_indices;
        for (int page = 0; page < n; page++) {
            fz_stext_page* stext_page = fz_new_stext_page_from_page_number(ctx, doc, page, nullptr);
            for (bool dehyphenate : { false, true }) {
                std::vector<fz_stext_char*> a, b;
                reference::get_flat_chars_from_stext_page(stext_page, a, dehyphenate);
                get_flat_chars_from_stext_page(stext_page, b, dehyphenate);
                check(a == b, "get_flat_chars_from_stext_page page " + std::to_string(page));
            }
            std::vector<fz_stext_char*> flat_chars;
            get_flat_chars_from_stext_page(stext_page, flat_chars);
            compare_flat_chars_processing(flat_chars, page, "page " + std::to_string(page));
            flat_char_prism2(flat_chars, page, index, page_begin_indices);

            LL_ITER(block, stext_page->first_block) {
                std::vector<fz_stext_char*> chars;
                get_flat_chars_from_block(block, chars);
                std::wstring block_string;
                std::vector<int> indices;
                get_text_from_flat_chars(chars, block_string, indices);
                check(reference::is_string_titlish(block_string) == is_string_titlish(block_string), "is_string_titlish block");
            }
            fz_drop_stext_page(ctx, stext_page);
        }
        search_tests(index, page_begin_indices, rng, 400, "file");
        fz_drop_document(ctx, doc);
    }

    printf("fuzzing text processing\n");
    fuzz_text_processing(rng);
    printf("fuzzing unicode search\n");
    fuzz_unicode_search(rng);
    printf("checksums\n");
    checksum_tests(files);

    printf("%lld checks, %d mismatches\n", checks, failures);
    fflush(stdout);
    _exit(failures == 0 ? 0 : 1);
}
