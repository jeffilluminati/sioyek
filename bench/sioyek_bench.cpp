// Headless benchmark for sioyek's large-document code paths.
//
// It is linked against the application's own object files (see build_bench.sh) and uses
// `bench_main` as the entry point, so every phase below runs the real sioyek implementation:
//
//   checksum    CachedChecksummer::get_checksum (MD5 of the whole file)
//   open        Document::open with synchronous page dimension loading
//   index       the background indexing thread started by Document::open
//   firstpages  rendering the first pages while the document is being indexed (not in "all")
//   breakdown   the per-page steps of the indexing thread, timed individually
//   search      Document::search_text / search_regex over the super fast index
//   fullsearch  PdfRenderer's search thread (used when super_fast_search is off)
//   render      page rasterization with the same mupdf calls as the render worker threads
//   upload      OpenGL texture upload of rendered pages (main thread work)
//   session     scrolling through the document with the real PdfRenderer (latency and memory),
//               not included in "all"
//   banded      render_request_pixmap with 1, 2 and 4 bands (threads) per page: speed and how the
//               output compares with the single threaded render, not included in "all"
//   db          DatabaseManager queries against a large annotation database (no PDF needed)
//
// usage: sioyek_bench [--phases a,b,c] [--render-pages N] [--scale S] [--threads T] [--json out.jsonl] file.pdf
//        sioyek_bench --phases db [--db-docs N]
//
// options:     --session-pages N --session-dwell MS  length of the session scroll and time per page
//              --store-mb N                          size of mupdf's store (default: what sioyek uses)
// environment: BENCH_STD_MUTEX=1        use plain std::mutex MuPDF locks (sioyek's previous implementation)
//              BENCH_UPLOAD_VARIANTS=1  also time RGBA and BGRA texture uploads of the same pages
//              BENCH_PIXMAP_HASHES=1    print a hash of every rendered page and of the text index, to
//                                       check that two builds produce the same output (see
//                                       bench/compare_output.sh)
//              BENCH_VMMAP=1            print `vmmap --summary` after the session phase
//              BENCH_DUMP_DIR=dir       write the raw samples of every page rendered by the render phase
//
// build with bench/build_bench.sh, create test documents with bench/gen_corpus.py (and, to check that
// rendering changes leave the output identical, bench/gen_render_tests.py), profile with
// `xcrun xctrace record --template 'Time Profiler' --launch -- bench/sioyek_bench ...` and summarize
// the trace with bench/xctrace_summary.py.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <set>
#include <fstream>
#include <sstream>
#include <random>
#include <algorithm>
#include <cmath>
#include <map>

#include <mach/mach.h>
#include <unistd.h>

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QDir>
#include <QTemporaryDir>
#include <QFileInfo>

#include <mupdf/fitz.h>

// The index fingerprint (BENCH_PIXMAP_HASHES) reads Document's private index data. document.h's
// own includes come first so that only Document itself is affected. (Test harness only: clang
// doesn't change the layout of a class because of access specifiers.)
#include <vector>
#include <string>
#include <optional>
#include <thread>
#include <mutex>
#include <map>
#include <unordered_map>
#include <deque>
#include <regex>
#include <qstandarditemmodel.h>
#include <qdatetime.h>
#include <qobject.h>
#include <qnetworkreply.h>
#include <qjsondocument.h>
#include <qurlquery.h>
#include "book.h"
#include "coordinates.h"
#define private public
#include "document.h"
#undef private

#include "checksum.h"
#include "database.h"
#include "pdf_renderer.h"
#include "utils.h"
#include "sqlite3.h"

fz_locks_context get_mupdf_locks(); // main.cpp
size_t sioyek_mupdf_store_size(); // main.cpp

extern bool SUPER_FAST_SEARCH;
extern bool CREATE_TABLE_OF_CONTENTS_IF_NOT_EXISTS;
extern int MAX_CREATED_TABLE_OF_CONTENTS_SIZE;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// the lock implementation sioyek used before, for comparison (BENCH_STD_MUTEX=1)
std::mutex std_mupdf_mutexes[FZ_LOCK_MAX];
void std_lock(void*, int lock) { std_mupdf_mutexes[lock].lock(); }
void std_unlock(void*, int lock) { std_mupdf_mutexes[lock].unlock(); }

double footprint_mb(bool peak = false) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return -1;
    return (peak ? info.ledger_phys_footprint_peak : info.phys_footprint) / (1024.0 * 1024.0);
}


struct Options {
    std::set<std::string> phases;
    int render_pages = 48;
    float scale = 3.0f; // zoom 1.5 on a 2x retina display
    int threads = 4;    // sioyek uses 4 render threads
    int search_reps = 5;
    std::string json_path;
    std::string label = "run";
    int db_docs = 2000;
    int session_pages = 40;
    int session_dwell_ms = 50;
    int store_mb = 0; // 0: the size the application uses
    std::wstring file;
};

std::ofstream json_out;
std::vector<int> kept_pages; // page numbers of the pixmaps kept by bench_render
std::string current_file;

void report(const std::string& phase, const std::string& metric, double value, const std::string& unit) {
    printf("  %-11s %-34s %12.2f %s\n", phase.c_str(), metric.c_str(), value, unit.c_str());
    fflush(stdout);
    if (json_out.is_open()) {
        json_out << "{\"file\":\"" << current_file << "\",\"phase\":\"" << phase << "\",\"metric\":\"" << metric
                 << "\",\"value\":" << value << ",\"unit\":\"" << unit << "\"}\n";
        json_out.flush();
    }
}

std::vector<int> sample_pages(int num_pages, int count) {
    std::vector<int> pages;
    if (count >= num_pages) {
        for (int i = 0; i < num_pages; i++) pages.push_back(i);
        return pages;
    }
    for (int i = 0; i < count; i++) {
        pages.push_back(static_cast<int>((static_cast<long long>(i) * num_pages) / count));
    }
    return pages;
}

// Renders `pages` with `threads` workers, each with its own cloned context and document,
// mirroring PdfRenderer::run (non sliced path, annotations enabled).
void bench_render(fz_context* ctx, const std::wstring& path, const std::vector<int>& pages, const Options& opt,
                  std::vector<fz_pixmap*>* keep, fz_context** keep_ctx) {
    std::atomic<int> next{ 0 };
    std::atomic<long long> total_pixels{ 0 };
    std::vector<std::thread> workers;
    std::vector<fz_context*> contexts(opt.threads);
    std::vector<fz_document*> docs(opt.threads);
    for (int t = 0; t < opt.threads; t++) {
        contexts[t] = fz_clone_context(ctx);
        docs[t] = open_document_with_file_name(contexts[t], path);
    }
    std::mutex keep_mutex;

    auto t0 = Clock::now();
    for (int t = 0; t < opt.threads; t++) {
        workers.emplace_back([&, t]() {
            fz_context* c = contexts[t];
            while (true) {
                int i = next.fetch_add(1);
                if (i >= (int)pages.size()) break;
                fz_try(c) {
                    RenderRequest req;
                    req.path = path;
                    req.page = pages[i];
                    req.zoom_level = opt.scale;
                    req.display_scale = 1.0f;
                    req.slice_index = -1;
                    req.should_render_annotations = true;
                    fz_pixmap* pixmap = render_request_pixmap(c, docs[t], req);
                    total_pixels += (long long)pixmap->w * pixmap->h;
                    if (const char* dump_dir = getenv("BENCH_DUMP_DIR")) {
                        // raw samples, to compare the output of different builds pixel by pixel
                        std::string name = std::string(dump_dir) + "/page" + std::to_string(pages[i]) + "_" +
                            std::to_string(pixmap->w) + "x" + std::to_string(pixmap->h) + "x" + std::to_string(pixmap->n) + ".raw";
                        if (FILE* f = fopen(name.c_str(), "wb")) {
                            for (int row = 0; row < pixmap->h; row++) fwrite(pixmap->samples + (size_t)row * pixmap->stride, 1, (size_t)pixmap->w * pixmap->n, f);
                            fclose(f);
                        }
                    }
                    if (getenv("BENCH_PIXMAP_HASHES")) {
                        // FNV-1a of the rendered samples, to compare the output of different builds
                        uint64_t hash = 1469598103934665603ull;
                        for (size_t k = 0; k < (size_t)pixmap->stride * pixmap->h; k++) {
                            hash = (hash ^ pixmap->samples[k]) * 1099511628211ull;
                        }
                        printf("pixmap page %d %dx%d %016llx\n", pages[i], pixmap->w, pixmap->h, (unsigned long long)hash);
                    }
                    if (keep && t == 0 && keep->size() < 8) {
                        std::lock_guard<std::mutex> lock(keep_mutex);
                        keep->push_back(fz_keep_pixmap(c, pixmap));
                        kept_pages.push_back(pages[i]);
                    }
                    fz_drop_pixmap(c, pixmap);
                }
                fz_catch(c) {
                    fprintf(stderr, "render failed on page %d\n", pages[i]);
                }
            }
            });
    }
    for (auto& w : workers) w.join();
    double elapsed = ms_since(t0);

    report("render", "wall time (" + std::to_string(pages.size()) + " pages)", elapsed, "ms");
    report("render", "throughput", pages.size() / (elapsed / 1000.0), "pages/s");
    report("render", "avg page pixels", (double)total_pixels / pages.size() / 1e6, "MP");

    for (int t = 0; t < opt.threads; t++) {
        fz_drop_document(contexts[t], docs[t]);
        if (keep && t == 0) {
            *keep_ctx = contexts[t];
        }
        else {
            fz_drop_context(contexts[t]);
        }
    }
}

void bench_upload(fz_context* ctx, const std::vector<fz_pixmap*>& pixmaps, fz_pixmap* reference_rgb) {
    if (pixmaps.empty()) return;

    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    QOpenGLContext gl;
    gl.setFormat(format);
    if (!gl.create() || !gl.makeCurrent(&surface)) {
        fprintf(stderr, "could not create an OpenGL context, skipping upload benchmark\n");
        return;
    }

    const int reps = 5;
    double total_ms = 0;
    double total_mb = 0;
    // warm up the driver
    create_texture_from_pixmap(pixmaps[0]);
    glFinish();

    for (int r = 0; r < reps; r++) {
        for (fz_pixmap* pixmap : pixmaps) {
            auto t0 = Clock::now();
            GLuint texture = create_texture_from_pixmap(pixmap);
            glFinish();
            total_ms += ms_since(t0);
            total_mb += (double)pixmap->stride * pixmap->h / (1024.0 * 1024.0);
            glDeleteTextures(1, &texture);
        }
    }
    int n = reps * (int)pixmaps.size();
    report("upload", "avg texture upload (main thread)", total_ms / n, "ms");

    // Read the texture of the first page back as RGB and compare it with an RGB rendering of the same
    // page, to make sure the texture has the right colors whatever format the renderer produces.
    if (reference_rgb) {
        fz_pixmap* pixmap = pixmaps[0];
        GLuint texture = create_texture_from_pixmap(pixmap);
        std::vector<unsigned char> readback((size_t)pixmap->w * pixmap->h * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, readback.data());
        glDeleteTextures(1, &texture);
        int max_diff = 0;
        long long differing = 0;
        if (reference_rgb->w == pixmap->w && reference_rgb->h == pixmap->h) {
            for (int y = 0; y < pixmap->h; y++) {
                for (int x = 0; x < pixmap->w * 3; x++) {
                    int d = std::abs((int)readback[(size_t)y * pixmap->w * 3 + x] - (int)reference_rgb->samples[(size_t)y * reference_rgb->stride + x]);
                    max_diff = std::max(max_diff, d);
                    if (d > 2) differing++;
                }
            }
            report("upload", "texture vs RGB render: max channel diff", max_diff, "levels");
            report("upload", "texture vs RGB render: samples off by >2", (double)differing, "samples");
        }
        else {
            report("upload", "texture vs RGB render: size mismatch", 1, "");
        }
    }

    if (getenv("BENCH_UPLOAD_VARIANTS")) {
        struct Variant { const char* name; GLenum internal; GLenum format; GLenum type; };
        Variant variants[] = {
            { "RGBA8 from RGBA/UNSIGNED_BYTE", GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE },
            { "RGBA8 from BGRA/8888_REV", GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV },
        };
        for (auto& v : variants) {
            double t = 0;
            for (fz_pixmap* pixmap : pixmaps) {
                std::vector<unsigned char> rgba((size_t)pixmap->w * pixmap->h * 4, 255);
                for (int i = 0; i < pixmap->w * pixmap->h; i++) memcpy(&rgba[i * 4], pixmap->samples + i * 3, 3);
                for (int r = 0; r < reps; r++) {
                    auto t0 = Clock::now();
                    GLuint tex;
                    glGenTextures(1, &tex);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, v.internal, pixmap->w, pixmap->h, 0, v.format, v.type, rgba.data());
                    glFinish();
                    t += ms_since(t0);
                    glDeleteTextures(1, &tex);
                }
            }
            report("upload", v.name, t / n, "ms");
        }
    }
    report("upload", "pixmap bytes per page", total_mb / n, "MB");
    gl.doneCurrent();
}

void bench_index_breakdown(fz_context* ctx, Document* doc, const std::wstring& path) {
    fz_context* c = fz_clone_context(ctx);
    fz_document* d = open_document_with_file_name(c, path);
    int n = fz_count_pages(c, d);

    double t_stext = 0, t_flat = 0, t_prism = 0, t_refs = 0, t_eqs = 0, t_generic = 0, t_toc = 0;
    std::wstring index;
    std::vector<int> page_begin_indices;
    std::map<std::wstring, IndexedData> refs;
    std::map<std::wstring, std::vector<IndexedData>> eqs;
    std::vector<IndexedData> generic;
    std::vector<TocNode*> toc_stack, toc_nodes;
    int toc_entries = 0;

    for (int i = 0; i < n; i++) {
        auto t0 = Clock::now();
        fz_stext_page* stext_page = fz_new_stext_page_from_page_number(c, d, i, nullptr);
        t_stext += ms_since(t0);

        t0 = Clock::now();
        std::vector<fz_stext_char*> flat_chars;
        get_flat_chars_from_stext_page(stext_page, flat_chars);
        t_flat += ms_since(t0);

        t0 = Clock::now();
        flat_char_prism2(flat_chars, i, index, page_begin_indices);
        t_prism += ms_since(t0);

        t0 = Clock::now();
        index_references(stext_page, i, refs);
        t_refs += ms_since(t0);

        t0 = Clock::now();
        index_equations(flat_chars, i, eqs);
        t_eqs += ms_since(t0);

        t0 = Clock::now();
        index_generic(flat_chars, i, generic);
        t_generic += ms_since(t0);

        t0 = Clock::now();
        if (toc_entries < MAX_CREATED_TABLE_OF_CONTENTS_SIZE) {
            toc_entries += doc->add_stext_page_to_created_toc(stext_page, i, toc_stack, toc_nodes);
        }
        t_toc += ms_since(t0);

        fz_drop_stext_page(c, stext_page);
    }
    double total = t_stext + t_flat + t_prism + t_refs + t_eqs + t_generic + t_toc;
    report("breakdown", "stext extraction (mupdf)", t_stext, "ms");
    report("breakdown", "get_flat_chars_from_stext_page", t_flat, "ms");
    report("breakdown", "flat_char_prism2", t_prism, "ms");
    report("breakdown", "index_references", t_refs, "ms");
    report("breakdown", "index_equations", t_eqs, "ms");
    report("breakdown", "index_generic", t_generic, "ms");
    report("breakdown", "add_stext_page_to_created_toc", t_toc, "ms");
    report("breakdown", "sioyek share of indexing", 100.0 * (total - t_stext) / total, "%");
    report("breakdown", "indexed chars/refs/eqs/generic",
           (double)index.size(), "chars");
    printf("              (%zu refs, %zu eqs, %zu generic, %d toc)\n", refs.size(), eqs.size(), generic.size(), toc_entries);

    fz_drop_document(c, d);
    fz_drop_context(c);
}

void bench_search(Document* doc) {
    struct Query {
        std::wstring text;
        SearchCaseSensitivity cs;
        bool regex;
        const char* name;
    };
    std::vector<Query> queries = {
        { L"optimal", SearchCaseSensitivity::CaseSensitive, false, "word, case sensitive" },
        { L"optimal", SearchCaseSensitivity::CaseInsensitive, false, "word, case insensitive" },
        { L"Section", SearchCaseSensitivity::CaseInsensitive, false, "capitalized, case insensitive" },
        { L"the", SearchCaseSensitivity::CaseInsensitive, false, "very common, case insensitive" },
        { L"convex optimal bound", SearchCaseSensitivity::CaseInsensitive, false, "phrase, case insensitive" },
        { L"zqxjv", SearchCaseSensitivity::CaseSensitive, false, "absent, case sensitive" },
        { L"zqxjv", SearchCaseSensitivity::CaseInsensitive, false, "absent, case insensitive" },
        { L"[0-9]+\\.[0-9]+", SearchCaseSensitivity::CaseSensitive, true, "regex numbers" },
        { L"(convex|linear) (optimal|space)", SearchCaseSensitivity::CaseInsensitive, true, "regex alternation, case insensitive" },
        { L"\\b[A-Z][a-z]+ing\\b", SearchCaseSensitivity::CaseSensitive, true, "regex word pattern" },
    };
    int n = doc->num_pages();
    for (const auto& q : queries) {
        const int reps = q.regex ? 1 : 5;
        size_t results = 0;
        auto t0 = Clock::now();
        for (int r = 0; r < reps; r++) {
            auto res = q.regex ? doc->search_regex(q.text, q.cs, 0, 0, n - 1) : doc->search_text(q.text, q.cs, 0, 0, n - 1);
            results = res.size();
        }
        double avg = ms_since(t0) / reps;
        report("search", std::string(q.name), avg, "ms");
        printf("              (%zu results)\n", results);
    }
    // a search restricted to a small page range should not scan the whole document
    auto t0 = Clock::now();
    auto res = doc->search_text(L"zqxjv", SearchCaseSensitivity::CaseSensitive, 0, 0, std::min(n - 1, 9));
    report("search", "absent, first 10 pages only", ms_since(t0), "ms");
}

void bench_fullsearch(fz_context* ctx, const std::wstring& path) {
    bool quit = false;
    PdfRenderer renderer(1, &quit, ctx);
    renderer.start_threads();

    std::vector<SearchResult> results;
    std::mutex results_mutex;
    float percent = 0;
    bool searching = true;
    double mem_before = footprint_mb();
    auto t0 = Clock::now();
    renderer.add_request(path, 0, L"zqxjv", false, &results, &percent, &searching, &results_mutex);
    // wait for the search thread to pick up and then finish the request
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        results_mutex.lock();
        bool done = !searching;
        results_mutex.unlock();
        if (done && !renderer.is_search_busy()) break;
        if (done && ms_since(t0) > 50) break;
    }
    double elapsed = ms_since(t0);
    report("fullsearch", "full document search (non indexed)", elapsed, "ms");
    report("fullsearch", "memory growth during search", footprint_mb() - mem_before, "MB");
    quit = true;
    renderer.join_threads();
}


std::string fake_checksum(int doc) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%032x", (unsigned)(doc * 2654435761u));
    return buf;
}

std::string fake_uuid(int doc, int kind, int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{%08x-%04x-%04x-0000-000000000000}", (unsigned)doc, (unsigned)kind, (unsigned)i);
    return buf;
}

// Times the database work sioyek does when opening a document, editing annotations and periodically
// persisting the reading position, against a database of a heavy user (many annotated documents).
void bench_db(int num_docs) {
    const int highlights_per_doc = 100, bookmarks_per_doc = 25, marks_per_doc = 5, links_per_doc = 5;

    QTemporaryDir tmp;
    std::wstring local_path = QDir(tmp.path()).filePath("local.db").toStdWString();
    std::wstring shared_path = QDir(tmp.path()).filePath("shared.db").toStdWString();

    DatabaseManager db;
    db.open(local_path, shared_path);
    db.ensure_database_compatibility(local_path, shared_path);
    db.ensure_schema_compatibility();

    // bulk fill through a separate connection in a single transaction
    auto t0 = Clock::now();
    {
        sqlite3* g = nullptr;
        sqlite3_open(QString::fromStdWString(shared_path).toUtf8().constData(), &g);
        sqlite3_exec(g, "BEGIN", nullptr, nullptr, nullptr);
        sqlite3_stmt *book, *mark, *bookmark, *highlight, *link;
        sqlite3_prepare_v2(g, "INSERT INTO opened_books(path, zoom_level, offset_x, offset_y, last_access_time, document_name) VALUES (?, 1.5, 0, ?, datetime('now'), 'paper')", -1, &book, nullptr);
        sqlite3_prepare_v2(g, "INSERT INTO marks(document_path, symbol, offset_y, uuid, creation_time, modification_time) VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)", -1, &mark, nullptr);
        sqlite3_prepare_v2(g, "INSERT INTO bookmarks(document_path, desc, offset_y, uuid, creation_time, modification_time) VALUES (?, 'a bookmark with some text', ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)", -1, &bookmark, nullptr);
        sqlite3_prepare_v2(g, "INSERT INTO highlights(document_path, desc, type, begin_x, begin_y, end_x, end_y, uuid, creation_time, modification_time) VALUES (?, 'some highlighted sentence from the paper that is being read', 'a', 10, ?, 200, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)", -1, &highlight, nullptr);
        sqlite3_prepare_v2(g, "INSERT INTO links(src_document, dst_document, src_offset_y, src_offset_x, dst_offset_x, dst_offset_y, dst_zoom_level, uuid, creation_time, modification_time) VALUES (?, ?, ?, 0, 0, 100, 1, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP)", -1, &link, nullptr);
        auto run = [](sqlite3_stmt* st) { sqlite3_step(st); sqlite3_reset(st); };
        for (int d = 0; d < num_docs; d++) {
            std::string cs = fake_checksum(d);
            sqlite3_bind_text(book, 1, cs.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_double(book, 2, d); run(book);
            for (int i = 0; i < marks_per_doc; i++) {
                std::string u = fake_uuid(d, 1, i);
                char sym[2] = { (char)('a' + i), 0 };
                sqlite3_bind_text(mark, 1, cs.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(mark, 2, sym, -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(mark, 3, i * 100); sqlite3_bind_text(mark, 4, u.c_str(), -1, SQLITE_TRANSIENT); run(mark);
            }
            for (int i = 0; i < bookmarks_per_doc; i++) {
                std::string u = fake_uuid(d, 2, i);
                sqlite3_bind_text(bookmark, 1, cs.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_double(bookmark, 2, i * 50);
                sqlite3_bind_text(bookmark, 3, u.c_str(), -1, SQLITE_TRANSIENT); run(bookmark);
            }
            for (int i = 0; i < highlights_per_doc; i++) {
                std::string u = fake_uuid(d, 3, i);
                sqlite3_bind_text(highlight, 1, cs.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_double(highlight, 2, i * 20);
                sqlite3_bind_double(highlight, 3, i * 20 + 12); sqlite3_bind_text(highlight, 4, u.c_str(), -1, SQLITE_TRANSIENT); run(highlight);
            }
            for (int i = 0; i < links_per_doc; i++) {
                std::string u = fake_uuid(d, 4, i);
                std::string dst = fake_checksum((d + i + 1) % num_docs);
                sqlite3_bind_text(link, 1, cs.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(link, 2, dst.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(link, 3, i * 100); sqlite3_bind_text(link, 4, u.c_str(), -1, SQLITE_TRANSIENT); run(link);
            }
        }
        for (auto* st : { book, mark, bookmark, highlight, link }) sqlite3_finalize(st);
        sqlite3_exec(g, "COMMIT", nullptr, nullptr, nullptr);
        sqlite3_close(g);

        sqlite3* l = nullptr;
        sqlite3_open(QString::fromStdWString(local_path).toUtf8().constData(), &l);
        sqlite3_exec(l, "BEGIN", nullptr, nullptr, nullptr);
        sqlite3_stmt* hash;
        sqlite3_prepare_v2(l, "INSERT INTO document_hash(path, hash) VALUES (?, ?)", -1, &hash, nullptr);
        for (int d = 0; d < num_docs; d++) {
            std::string cs = fake_checksum(d);
            std::string path = "/Users/someone/papers/paper_" + std::to_string(d) + ".pdf";
            sqlite3_bind_text(hash, 1, path.c_str(), -1, SQLITE_TRANSIENT); sqlite3_bind_text(hash, 2, cs.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(hash); sqlite3_reset(hash);
        }
        sqlite3_finalize(hash);
        sqlite3_exec(l, "COMMIT", nullptr, nullptr, nullptr);
        sqlite3_close(l);
    }
    int rows = num_docs * (1 + highlights_per_doc + bookmarks_per_doc + marks_per_doc + links_per_doc + 1);
    report("db", "fill (" + std::to_string(rows) + " rows)", ms_since(t0), "ms");

    // Simulate a database created by an older sioyek: drop any secondary indices, so the first
    // startup below includes building them (the one time cost of upgrading).
    {
        auto drop_indices = [](const std::wstring& path) {
            sqlite3* c = nullptr;
            sqlite3_open(QString::fromStdWString(path).toUtf8().constData(), &c);
            std::vector<std::string> names;
            sqlite3_exec(c, "SELECT name FROM sqlite_master WHERE type='index' AND sql IS NOT NULL", [](void* out, int, char** argv, char**) {
                ((std::vector<std::string>*)out)->push_back(argv[0]);
                return 0;
                }, &names, nullptr);
            for (const auto& name : names) sqlite3_exec(c, ("DROP INDEX " + name).c_str(), nullptr, nullptr, nullptr);
            sqlite3_exec(c, "VACUUM", nullptr, nullptr, nullptr);
            sqlite3_close(c);
        };
        drop_indices(shared_path);
        drop_indices(local_path);
    }
    double size_before = QFileInfo(QString::fromStdWString(shared_path)).size() / (1024.0 * 1024.0);

    // what sioyek does at startup after opening the database, on a second DatabaseManager so it runs
    // against the filled database
    t0 = Clock::now();
    DatabaseManager db2;
    db2.open(local_path, shared_path);
    db2.ensure_database_compatibility(local_path, shared_path);
    db2.ensure_schema_compatibility();
    report("db", "first startup: open + compatibility checks", ms_since(t0), "ms");

    t0 = Clock::now();
    DatabaseManager db3;
    db3.open(local_path, shared_path);
    db3.ensure_database_compatibility(local_path, shared_path);
    db3.ensure_schema_compatibility();
    report("db", "next startups: open + compatibility checks", ms_since(t0), "ms");

    report("db", "shared.db size (without indices)", size_before, "MB");
    report("db", "shared.db size (after startup)", QFileInfo(QString::fromStdWString(shared_path)).size() / (1024.0 * 1024.0), "MB");

    std::mt19937 rng(1);
    const int reps = 200;

    // Document::load_document_metadata_from_db
    double t_open = 0;
    size_t loaded = 0;
    for (int r = 0; r < reps; r++) {
        std::string cs = fake_checksum(rng() % num_docs);
        std::vector<Mark> marks;
        std::vector<BookMark> bookmarks;
        std::vector<Highlight> highlights;
        std::vector<Portal> portals;
        auto t = Clock::now();
        db2.select_mark(cs, marks);
        db2.select_bookmark(cs, bookmarks);
        db2.select_highlight(cs, highlights);
        db2.select_links(cs, portals);
        t_open += ms_since(t);
        loaded += marks.size() + bookmarks.size() + highlights.size() + portals.size();
    }
    report("db", "document open: load annotations", t_open / reps, "ms");
    printf("              (%zu annotations per document)\n", loaded / reps);

    double t_hash = 0;
    for (int r = 0; r < reps; r++) {
        std::vector<std::wstring> paths;
        auto t = Clock::now();
        db2.get_path_from_hash(fake_checksum(rng() % num_docs), paths);
        t_hash += ms_since(t);
    }
    report("db", "path lookup by checksum", t_hash / reps, "ms");

    const int write_reps = 50;
    double t_insert = 0, t_delete = 0, t_update_type = 0, t_persist = 0;
    for (int r = 0; r < write_reps; r++) {
        std::string cs = fake_checksum(rng() % num_docs);
        auto t = Clock::now();
        db2.insert_highlight(cs, L"new highlight", 1, 2, 3, 4, 'a', QString::fromStdString(fake_uuid(9999999, 3, r)).toStdWString());
        t_insert += ms_since(t);

        t = Clock::now();
        db2.update_highlight_type(fake_uuid(rng() % num_docs, 3, rng() % highlights_per_doc), 'b');
        t_update_type += ms_since(t);

        t = Clock::now();
        db2.delete_highlight(fake_uuid(rng() % num_docs, 3, rng() % highlights_per_doc));
        t_delete += ms_since(t);

        t = Clock::now();
        db2.update_book(cs, 1.5f, 0, (float)r, L"paper");
        t_persist += ms_since(t);
    }
    report("db", "add highlight", t_insert / write_reps, "ms");
    report("db", "change highlight type (by uuid)", t_update_type / write_reps, "ms");
    report("db", "delete highlight (by uuid)", t_delete / write_reps, "ms");
    report("db", "persist reading position (update_book)", t_persist / write_reps, "ms");
}


// Scrolls through the document with the real PdfRenderer (its worker threads, response cache and
// garbage collection) and uploads textures like the main thread does, one page per step: pages
// p and p+1 are visible and p+2 is prefetched. Measures how long each step waits for its pages and
// how much memory the rendered pages (pixmaps and textures) keep alive.
void bench_session(fz_context* ctx, const std::wstring& path, int num_pages, const Options& opt) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    QOpenGLContext gl;
    gl.setFormat(format);
    if (!gl.create() || !gl.makeCurrent(&surface)) {
        fprintf(stderr, "could not create an OpenGL context, skipping session benchmark\n");
        return;
    }

    bool quit = false;
    PdfRenderer renderer(opt.threads, &quit, ctx);
    renderer.set_num_cached_pages(5); // sioyek's desktop default
    renderer.start_threads();

    const float zoom = 1.5f;
    const float display_scale = opt.scale / zoom;
    int steps = std::min(opt.session_pages, num_pages - 2);
    double before = footprint_mb();
    double max_footprint = before;
    std::vector<double> waits;
    auto last_gc = Clock::now();
    auto t_start = Clock::now();

    for (int p = 0; p < steps; p++) {
        auto t_step = Clock::now();
        while (true) {
            bool all_visible = true;
            for (int page : { p, p + 1, p + 2 }) {
                int w, h;
                GLuint texture = renderer.find_rendered_page(path, page, true, -1, 1, 1, zoom, display_scale, &w, &h);
                if (page <= p + 1 && texture == 0) all_visible = false;
            }
            if (ms_since(last_gc) >= 1000) {
                renderer.delete_old_pages();
                last_gc = Clock::now();
            }
            max_footprint = std::max(max_footprint, footprint_mb());
            if (all_visible) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        glFinish();
        waits.push_back(ms_since(t_step));
        // the page stays on screen for a moment before scrolling on
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.session_dwell_ms));
    }
    double total = ms_since(t_start);
    double max_scroll_footprint = max_footprint;

    // keep showing the last pages for a while, like a user who stopped scrolling, so that the cache
    // garbage collection (which runs every second) catches up
    auto t_settle = Clock::now();
    while (ms_since(t_settle) < 3000) {
        for (int page : { steps - 1, steps }) {
            int w, h;
            renderer.find_rendered_page(path, page, true, -1, 1, 1, zoom, display_scale, &w, &h);
        }
        if (ms_since(last_gc) >= 1000) {
            renderer.delete_old_pages();
            last_gc = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    glFinish();
    double footprint_after_scroll = footprint_mb();

    // zoom in on the last pages: they have to be rendered again, which is where caching decoded
    // images and fonts in mupdf's store helps
    double zoom_wait = 0;
    {
        auto t_zoom = Clock::now();
        const float new_zoom = zoom * 1.25f;
        while (true) {
            bool done = true;
            for (int page : { steps - 1, steps }) {
                int w, h;
                bool exact = false;
                // a texture of the old zoom level is returned until the new one is ready
                renderer.find_rendered_page(path, page, true, -1, 1, 1, new_zoom, display_scale, &w, &h, &exact);
                if (!exact) done = false;
            }
            if (done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        zoom_wait = ms_since(t_zoom);
    }

    // What a user sees when zooming: sioyek draws every frame (~16 ms) with whatever texture is closest
    // until the one for the current zoom level is rendered. Time from the last zoom change until both
    // visible pages are sharp, for a single 2x step and for a pinch-like burst of zoom levels. (Whole
    // pages: at zoom levels this high sioyek draws the visible part in tiles, which this doesn't do.)
    auto frames_until_sharp = [&](float z) {
        auto t0 = Clock::now();
        while (true) {
            bool done = true;
            for (int page : { steps - 1, steps }) {
                int w, h;
                bool exact = false;
                renderer.find_rendered_page(path, page, true, -1, 1, 1, z, display_scale, &w, &h, &exact);
                if (!exact) done = false;
            }
            glFinish();
            if (done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        return ms_since(t0);
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // workers are idle again
    double zoom_step_wait = frames_until_sharp(zoom * 1.25f * 2.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    double pinch_wait = 0;
    {
        float z = zoom * 2.5f;
        for (int frame = 0; frame < 12; frame++) {
            z *= 1.06f;
            for (int page : { steps - 1, steps }) {
                int w, h;
                renderer.find_rendered_page(path, page, true, -1, 1, 1, z, display_scale, &w, &h);
            }
            glFinish();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        pinch_wait = frames_until_sharp(z);
    }

    std::vector<double> sorted = waits;
    std::sort(sorted.begin(), sorted.end());
    report("session", "scroll " + std::to_string(steps) + " pages: total", total, "ms");
    report("session", "wait for visible pages: median", sorted[sorted.size() / 2], "ms");
    report("session", "wait for visible pages: p90", sorted[(sorted.size() * 9) / 10], "ms");
    report("session", "footprint growth during scroll (max)", max_scroll_footprint - before, "MB");
    report("session", "footprint growth after scrolling stops", footprint_after_scroll - before, "MB");
    report("session", "re-render 2 pages after zooming", zoom_wait, "ms");
    report("session", "sharp after a 2x zoom step", zoom_step_wait, "ms");
    report("session", "sharp after a pinch zoom burst", pinch_wait, "ms");

    if (getenv("BENCH_VMMAP")) {
        // memory regions by type, to see what the growth consists of
        std::string cmd = "vmmap --summary " + std::to_string(getpid()) + " 2>/dev/null";
        FILE* f = popen(cmd.c_str(), "r");
        char line[512];
        bool in_table = false;
        while (f && fgets(line, sizeof(line), f)) {
            std::string l(line);
            if (l.find("REGION TYPE") != std::string::npos) in_table = true;
            if (in_table) fputs(line, stdout);
            if (in_table && l.find("TOTAL") == 0) break;
        }
        if (f) pclose(f);
    }

    quit = true;
    renderer.join_threads();
    gl.doneCurrent();
}


uint64_t pixmap_hash(fz_pixmap* p) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t k = 0; k < (size_t)p->stride * p->h; k++) hash = (hash ^ p->samples[k]) * 1099511628211ull;
    return hash;
}

void bench_banded(fz_context* ctx, const std::wstring& path, const std::vector<int>& pages, float scale) {
    fz_context* c = fz_clone_context(ctx);
    auto reference_request = [&](int page) {
        RenderRequest req;
        req.path = path;
        req.page = page;
        req.zoom_level = scale;
        req.display_scale = 1.0f;
        req.slice_index = -1;
        req.should_render_annotations = true;
        return req;
    };

    // reference output and timing of the current single threaded path
    std::map<int, uint64_t> reference;
    fz_document* d = open_document_with_file_name(c, path);
    auto t0 = Clock::now();
    for (int page : pages) {
        fz_pixmap* p = render_request_pixmap(c, d, reference_request(page));
        reference[page] = pixmap_hash(p);
        fz_drop_pixmap(c, p);
    }
    double reference_ms = ms_since(t0) / pages.size();
    fz_drop_document(c, d);

    for (int bands : { 1, 2, 4 }) {
        // fresh document so every configuration starts from the same state
        d = open_document_with_file_name(c, path);
        int mismatches = 0;
        double total = 0;
        std::vector<double> page_times;
        for (int page : pages) {
            auto t = Clock::now();
            fz_pixmap* p = render_request_pixmap(c, d, reference_request(page), bands - 1);
            page_times.push_back(ms_since(t));
            total += page_times.back();
            if (pixmap_hash(p) != reference[page]) mismatches++;
            fz_drop_pixmap(c, p);
        }
        fz_drop_document(c, d);
        report("banded", std::to_string(bands) + " band(s): ms per page", total / pages.size(), "ms");
        if (!page_times.empty()) {
            std::sort(page_times.begin(), page_times.end());
            report("banded", std::to_string(bands) + " band(s): p50 page", page_times[page_times.size() / 2], "ms");
            report("banded", std::to_string(bands) + " band(s): p90 page", page_times[(page_times.size() * 9) / 10], "ms");
            report("banded", std::to_string(bands) + " band(s): max page", page_times.back(), "ms");
        }
        report("banded", std::to_string(bands) + " band(s): pages differing", mismatches, "pages");
    }
    report("banded", "render_request_pixmap: ms per page", reference_ms, "ms");

    // how different are the banded renders
    d = open_document_with_file_name(c, path);
    int max_diff = 0;
    long long differing = 0, near_boundary = 0, total_samples = 0;
    for (int page : pages) {
        fz_pixmap* a = render_request_pixmap(c, d, reference_request(page));
        fz_pixmap* b = render_request_pixmap(c, d, reference_request(page), 3);
        int h = a->h;
        for (int y = 0; y < h; y++) {
            int band_edge_distance = h;
            for (int band = 1; band < 4; band++) band_edge_distance = std::min(band_edge_distance, std::abs(y - (h * band) / 4));
            for (int x = 0; x < a->w * a->n; x++) {
                int diff = std::abs((int)a->samples[(size_t)y * a->stride + x] - (int)b->samples[(size_t)y * b->stride + x]);
                total_samples++;
                if (diff) {
                    differing++;
                    if (band_edge_distance <= 2) near_boundary++;
                    max_diff = std::max(max_diff, diff);
                }
            }
        }
        fz_drop_pixmap(c, a);
        fz_drop_pixmap(c, b);
    }
    fz_drop_document(c, d);
    report("banded", "4 bands: max channel difference", max_diff, "levels");
    report("banded", "4 bands: differing samples", 100.0 * differing / total_samples, "%");
    report("banded", "4 bands: of those within 2 rows of a band edge", differing ? 100.0 * near_boundary / differing : 0, "%");
    fz_drop_context(c);
}


// Hashes of everything the indexing thread produces, to check that two builds index identically.
struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void byte(uint8_t b) { h = (h ^ b) * 1099511628211ull; }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) byte((v >> (8 * i)) & 0xFF); }
    void f(float v) { uint32_t u; memcpy(&u, &v, 4); u32(u); }
    void str(const std::wstring& s) { u32(s.size()); for (wchar_t c : s) u32((uint32_t)c); }
    void data(const IndexedData& d) { u32(d.page); f(d.y_offset); str(d.text); }
};

void hash_toc(Fnv& fnv, const std::vector<TocNode*>& nodes) {
    fnv.u32(nodes.size());
    for (const TocNode* node : nodes) {
        fnv.str(node->title);
        fnv.u32(node->page);
        fnv.f(node->x);
        fnv.f(node->y);
        hash_toc(fnv, node->children);
    }
}

void print_index_fingerprint(Document* doc) {
    Fnv text, pages, refs, eqs, generic, toc;
    for (wchar_t c : doc->super_fast_search_index) text.u32((uint32_t)c);
    for (int b : doc->super_fast_page_begin_indices) pages.u32(b);
    for (const auto& [key, d] : doc->reference_indices) { refs.str(key); refs.data(d); }
    for (const auto& [key, list] : doc->equation_indices) { eqs.str(key); for (const auto& d : list) eqs.data(d); }
    for (const auto& d : doc->generic_indices) generic.data(d);
    hash_toc(toc, doc->created_top_level_toc_nodes);
    printf("index text %016llx pages %016llx (%zu) refs %016llx (%zu) eqs %016llx (%zu) generic %016llx (%zu) toc %016llx (%zu)\n",
           (unsigned long long)text.h, (unsigned long long)pages.h, doc->super_fast_page_begin_indices.size(),
           (unsigned long long)refs.h, doc->reference_indices.size(), (unsigned long long)eqs.h, doc->equation_indices.size(),
           (unsigned long long)generic.h, doc->generic_indices.size(), (unsigned long long)toc.h, doc->created_top_level_toc_nodes.size());
}

void run(const Options& opt) {
    current_file = QString::fromStdWString(opt.file).toStdString();
    printf("\n== %s [%s]\n", current_file.c_str(), opt.label.c_str());

    // the same lock implementation as the application
    fz_locks_context locks = get_mupdf_locks();
    if (getenv("BENCH_STD_MUTEX")) {
        locks.user = nullptr;
        locks.lock = std_lock;
        locks.unlock = std_unlock;
    }
    fz_context* ctx = fz_new_context(nullptr, &locks, opt.store_mb > 0 ? (size_t)opt.store_mb << 20 : sioyek_mupdf_store_size());
    fz_register_document_handlers(ctx);

    QTemporaryDir tmp;
    DatabaseManager db;
    std::wstring local_db_path = QDir(tmp.path()).filePath("local.db").toStdWString();
    std::wstring shared_db_path = QDir(tmp.path()).filePath("shared.db").toStdWString();
    db.open(local_db_path, shared_db_path);
    db.ensure_database_compatibility(local_db_path, shared_db_path);
    CachedChecksummer checksummer(nullptr);

    auto has = [&](const char* p) { return opt.phases.count(p) > 0 || opt.phases.count("all") > 0; };

    report("memory", "baseline footprint", footprint_mb(), "MB");

    // Computing the checksum up front also means Document::open finds it in the cache and doesn't
    // start a concurrent hashing thread that would pollute the timings below.
    auto t0 = Clock::now();
    std::string checksum = checksummer.get_checksum(opt.file);
    if (has("checksum")) report("checksum", "md5 of file", ms_since(t0), "ms");

    DocumentManager document_manager(ctx, &db, &checksummer);
    Document* doc = document_manager.get_document(opt.file);
    bool invalid = false;
    t0 = Clock::now();
    doc->open(&invalid, true);
    double open_ms = ms_since(t0);
    auto index_start = Clock::now();
    int n = doc->num_pages();

    if (opt.phases.count("firstpages")) {
        // what the user waits for right after opening: the first pages, rendered while the
        // document is being indexed in the background
        fz_context* c = fz_clone_context(ctx);
        fz_document* d = open_document_with_file_name(c, opt.file);
        for (int page = 0; page < std::min(n, 3); page++) {
            RenderRequest req;
            req.path = opt.file;
            req.page = page;
            req.zoom_level = opt.scale;
            req.display_scale = 1.0f;
            req.slice_index = -1;
            req.should_render_annotations = true;
            auto t = Clock::now();
            fz_pixmap* p = render_request_pixmap(c, d, req);
            report("firstpages", "render page " + std::to_string(page) + " while indexing", ms_since(t), "ms");
            fz_drop_pixmap(c, p);
        }
        report("firstpages", "indexing still running", doc->get_is_indexing() ? 1 : 0, "");
        fz_drop_document(c, d);
        fz_drop_context(c);
    }
    if (has("open")) {
        report("open", "open + page dimensions (" + std::to_string(n) + " pages)", open_ms, "ms");
        report("memory", "footprint after open", footprint_mb(), "MB");
    }

    while (doc->get_is_indexing()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (has("index")) {
        report("index", "background indexing thread", ms_since(index_start), "ms");
        report("index", "super fast index chars", (double)doc->get_super_fast_search_index().size(), "chars");
        if (getenv("BENCH_PIXMAP_HASHES")) {
            print_index_fingerprint(doc);
        }
        report("memory", "footprint after index", footprint_mb(), "MB");
    }

    if (has("breakdown")) bench_index_breakdown(ctx, doc, opt.file);
    if (has("search")) bench_search(doc);
    if (has("fullsearch")) bench_fullsearch(ctx, opt.file);

    if (opt.phases.count("session")) bench_session(ctx, opt.file, n, opt);
    if (opt.phases.count("banded")) bench_banded(ctx, opt.file, sample_pages(n, opt.render_pages), opt.scale);

    if (has("render") || has("upload")) {
        std::vector<fz_pixmap*> keep;
        fz_context* keep_ctx = nullptr;
        bench_render(ctx, opt.file, sample_pages(n, opt.render_pages), opt, has("upload") ? &keep : nullptr, &keep_ctx);
        if (has("upload")) {
            // reference rendering of the first kept page, straight from mupdf in RGB
            fz_pixmap* reference_rgb = nullptr;
            if (!keep.empty()) {
                fz_try(ctx) {
                    fz_document* d = open_document_with_file_name(ctx, opt.file);
                    int first_page = kept_pages[0];
                    reference_rgb = fz_new_pixmap_from_page_number(ctx, d, first_page, fz_scale(opt.scale, opt.scale), fz_device_rgb(ctx), 0);
                    fz_drop_document(ctx, d);
                }
                fz_catch(ctx) {
                    reference_rgb = nullptr;
                }
            }
            bench_upload(keep_ctx, keep, reference_rgb);
            if (reference_rgb) fz_drop_pixmap(ctx, reference_rgb);
            for (auto* p : keep) fz_drop_pixmap(keep_ctx, p);
            fz_drop_context(keep_ctx);
        }
    }
    report("memory", "final footprint", footprint_mb(), "MB");
    report("memory", "peak footprint", footprint_mb(true), "MB");
}

} // namespace

extern "C" int bench_main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    Options opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--phases") {
            std::stringstream ss(next());
            std::string p;
            while (std::getline(ss, p, ',')) opt.phases.insert(p);
        }
        else if (a == "--render-pages") opt.render_pages = std::stoi(next());
        else if (a == "--scale") opt.scale = std::stof(next());
        else if (a == "--threads") opt.threads = std::stoi(next());
        else if (a == "--json") opt.json_path = next();
        else if (a == "--label") opt.label = next();
        else if (a == "--db-docs") opt.db_docs = std::stoi(next());
        else if (a == "--session-pages") opt.session_pages = std::stoi(next());
        else if (a == "--session-dwell") opt.session_dwell_ms = std::stoi(next());
        else if (a == "--store-mb") opt.store_mb = std::stoi(next());
        else opt.file = QString::fromLocal8Bit(a.c_str()).toStdWString();
    }
    if (opt.phases.empty()) opt.phases.insert("all");
    if (opt.phases.size() == 1 && opt.phases.count("db")) {
        if (!opt.json_path.empty()) json_out.open(opt.json_path, std::ios::app);
        current_file = "database";
        printf("\n== database [%s]\n", opt.label.c_str());
        bench_db(opt.db_docs);
        fflush(stdout);
        _exit(0);
    }
    if (opt.file.empty()) {
        fprintf(stderr, "usage: sioyek_bench [--phases ...] file.pdf\n");
        return 1;
    }
    if (!opt.json_path.empty()) json_out.open(opt.json_path, std::ios::app);
    run(opt);
    fflush(stdout);
    // skip global destructors, documents and threads are intentionally left alive
    _exit(0);
}
