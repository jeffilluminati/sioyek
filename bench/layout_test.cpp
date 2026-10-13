// Integration tests for page rows using the real Document and DocumentView.
// PDFs and databases are generated in a temporary directory, with no GUI needed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "checksum.h"
#include "database.h"
#include "document.h"
#include "document_view.h"

extern int PAGES_PER_ROW;
extern int FIRST_PAGE_COLUMN;
extern bool RECTO_VERSO_ADJUSTMENT;
extern bool SCROLL_PAST_DOCUMENT_ENDS;
extern bool SAME_WIDTH;
extern float PAGE_SPACE_X;
extern float PAGE_SPACE_Y;
extern float FIT_TO_PAGE_WIDTH_RATIO;
extern bool TOUCH_MODE;
fz_locks_context get_mupdf_locks();

static int assertions = 0;

static void check(bool condition, const char* message) {
    ++assertions;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static void near(float actual, float expected, const char* message) {
    if (std::abs(actual - expected) > 0.02f) {
        std::fprintf(stderr, "actual %.4f, expected %.4f: ", actual, expected);
    }
    check(std::abs(actual - expected) < 0.02f, message);
}

// Small, valid PDF writer: each page has a visible rectangle and a chosen MediaBox.
static void write_pdf(const QString& path, const std::vector<std::pair<int, int>>& sizes) {
    std::vector<std::string> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids;
    for (size_t i = 0; i < sizes.size(); ++i) kids += std::to_string(3 + 2 * i) + " 0 R ";
    objects.push_back("<< /Type /Pages /Count " + std::to_string(sizes.size()) + " /Kids [" + kids + "] >>");
    for (size_t i = 0; i < sizes.size(); ++i) {
        auto [width, height] = sizes[i];
        objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(width) +
            " " + std::to_string(height) + "] /Resources << >> /Contents " + std::to_string(4 + 2 * i) + " 0 R >>");
        std::string drawing = "50 50 " + std::to_string(width - 100) + " " + std::to_string(height - 100) + " re f\n";
        objects.push_back("<< /Length " + std::to_string(drawing.size()) + " >>\nstream\n" + drawing + "endstream");
    }
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> positions;
    for (size_t i = 0; i < objects.size(); ++i) {
        positions.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t position : positions) {
        char entry[40];
        std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", position);
        pdf += entry;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
        std::to_string(xref) + "\n%%EOF\n";
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "create temporary PDF");
    check(file.write(pdf.data(), pdf.size()) == static_cast<qint64>(pdf.size()), "write temporary PDF");
}

class LayoutView : public DocumentView {
public:
    using DocumentView::DocumentView;
    void attach(Document* document) {
        current_document = document;
        zoom_level = 1;
        view_width = 1200;
        view_height = 400;
        offset = {0, 100};
        two_page_mode = true;
        fill_cached_virtual_rects(true);
    }
    const VirtualRect& rect(int page) { return cached_virtual_rects.at(page); }
    void focus(int page, float y = 100) {
        auto pos = document_to_virtual_pos(DocumentPos{page, current_document->get_page_width(page) / 2, y});
        offset = {-pos.x, pos.y};
    }
};

static void test_uniform(LayoutView& view) {
    PAGES_PER_ROW = 2;
    FIRST_PAGE_COLUMN = 0;
    RECTO_VERSO_ADJUSTMENT = false;
    view.refresh_page_layout();
    near(view.rect(0).x0, -610, "two columns: first page starts left of center");
    near(view.rect(1).x0, 10, "two columns: second page starts right of center");
    near(view.rect(2).y0, 830, "next row includes vertical gap");

    RECTO_VERSO_ADJUSTMENT = true;
    view.refresh_page_layout();
    check(view.get_page_row_range(0) == std::pair<int, int>{0, 1}, "legacy recto cover occupies its own row");
    near(view.rect(0).x0, 10, "legacy recto cover occupies second column");
    near(view.rect(1).x0, -610, "legacy recto: next row starts left");
    near(view.rect(1).y0, 830, "legacy recto: next row below cover");

    PAGES_PER_ROW = 3;
    FIRST_PAGE_COLUMN = 1;
    view.refresh_page_layout();
    check(view.get_pages_per_row() == 3, "three configured columns");
    near(view.rect(0).x0, -920, "three columns: left");
    near(view.rect(1).x0, -300, "three columns: center");
    near(view.rect(2).x0, 320, "three columns: right");
    near(view.rect(3).y0, 830, "fourth page begins next row");
    check(view.get_page_row_range(4) == std::pair<int, int>{3, 6}, "presentation row range includes all three pages");

    for (int first = 1; first <= 3; ++first) {
        FIRST_PAGE_COLUMN = first;
        view.refresh_page_layout();
        for (int page = 0; page < view.get_document()->num_pages(); ++page) {
            auto [begin, end] = view.get_page_row_range(page);
            check(begin <= page && page < end && end <= 8, "every page belongs to a valid row");
            near(view.rect(page).y0, view.rect(begin).y0, "row members share their top edge");
        }
    }
    FIRST_PAGE_COLUMN = 1;
    view.refresh_page_layout();

    for (int page = 0; page < view.get_document()->num_pages(); ++page) {
        DocumentPos source{page, 123, 234};
        auto absolute = view.virtual_to_absolute_pos(view.document_to_virtual_pos(source));
        auto restored = absolute.to_document(view.get_document());
        check(restored.page == source.page, "coordinate conversion retains page");
        near(restored.x, source.x, "coordinate conversion retains x");
        near(restored.y, source.y, "coordinate conversion retains y");
    }

    FIRST_PAGE_COLUMN = 3;
    view.refresh_page_layout();
    near(view.rect(0).x0, 320, "explicit first column puts cover on right");
    near(view.rect(1).y0, 830, "new row after right-column cover");
    check(view.get_page_row_range(7) == std::pair<int, int>{7, 8}, "incomplete final row has valid page range");
    view.focus(0);
    view.fit_to_page_width();
    near(view.get_zoom_level(), 1200.0f / 1840, "cover keeps full-row fit scale");
    view.focus(7);
    view.fit_to_page_width();
    near(view.get_zoom_level(), 1200.0f / 1840, "incomplete final row keeps full-row fit scale");

    FIRST_PAGE_COLUMN = 1;
    view.refresh_page_layout();
    view.focus(0);
    view.move_pages(1);
    near(view.get_virtual_offset().y, 930, "next_page moves one three-page row");
    view.move_pages(1);
    near(view.get_virtual_offset().y, 1760, "next_page reaches incomplete last row");
    view.move_pages(1000);
    check(view.get_center_page_number() < 8, "next_page at end stays in document");
    view.move_pages(-1000);
    near(view.get_virtual_offset().y, 100, "previous_page clamps to first row");

    view.focus(4);
    auto anchor = view.get_offsets();
    PAGES_PER_ROW = 4;
    view.refresh_page_layout();
    near(view.get_offsets().y, anchor.y, "changing column count preserves vertical reading position");

    FIRST_PAGE_COLUMN = 999;
    view.refresh_page_layout();
    check(view.get_page_column(0) == 3, "first column beyond row width is clamped");
    PAGES_PER_ROW = 0;
    view.refresh_page_layout();
    check(view.get_pages_per_row() == 1, "zero columns is clamped to one");
    PAGES_PER_ROW = std::numeric_limits<int>::max();
    view.refresh_page_layout();
    check(view.get_pages_per_row() == 100, "column count has a bounded maximum");

    PAGES_PER_ROW = 3;
    FIRST_PAGE_COLUMN = 1;
    view.refresh_page_layout();
    view.focus(3);
    view.fit_to_page_width(true);
    check(std::isfinite(view.get_zoom_level()) && view.get_zoom_level() > 0, "smart row fit gives a finite positive zoom");
    check(view.get_page_space_x() < 0, "smart row fit crops interior margins");
    view.fit_to_page_width();
    near(view.get_page_space_x(), PAGE_SPACE_X, "normal fit restores configured gap");
    near(view.get_page_space_y(), PAGE_SPACE_Y, "vertical gap getter returns vertical gap");

    view.toggle_two_page();
    check(!view.is_two_page_mode() && view.get_pages_per_row() == 1, "toggle returns to single-page mode");
    view.toggle_two_page();
    check(view.is_two_page_mode() && view.get_pages_per_row() == 3, "toggle restores configured column count");
}

static void test_mixed(LayoutView& view) {
    PAGES_PER_ROW = 3;
    FIRST_PAGE_COLUMN = 1;
    RECTO_VERSO_ADJUSTMENT = false;
    view.refresh_page_layout();
    near(view.rect(0).y1, 800, "mixed pages: first height");
    near(view.rect(1).y1, 1000, "mixed pages: tallest height");
    near(view.rect(3).y0, 1030, "next row starts below tallest page");
    near(view.rect(1).x0 - view.rect(0).x1, 20, "mixed widths retain first horizontal gap");
    near(view.rect(2).x0 - view.rect(1).x1, 20, "mixed widths retain second horizontal gap");
    view.focus(0);
    view.move_pages(1);
    near(view.get_virtual_offset().y, 1130, "row navigation uses tallest mixed page");
    view.move_pages(-1);
    near(view.get_virtual_offset().y, 100, "mixed row navigation is reversible");
    view.fit_to_page_width();
    near(view.get_zoom_level(), 1200.0f / 2040, "width fit uses all actual page widths");
    view.fit_to_page_height();
    near(view.get_zoom_level(), 0.38f, "height fit uses tallest page in row");
    near(view.get_virtual_offset().y, 500, "height fit centers the whole row");
    view.fit_to_page_height_and_width_smart();
    check(std::isfinite(view.get_zoom_level()) && view.get_zoom_level() > 0, "smart width and height fit stays finite");
    view.fit_to_page_width();
    view.fit_to_page_height_width_minimum(40);
    near(view.get_zoom_level(), 0.36f, "presentation fits row height and status bar");
}

static void test_single_document(LayoutView& view) {
    PAGES_PER_ROW = 3;
    FIRST_PAGE_COLUMN = 3;
    view.refresh_page_layout();
    view.focus(0);
    view.fit_to_page_width();
    near(view.get_zoom_level(), 1200.0f / 1840, "single-page document keeps placeholders in multi-page width fit");
    view.move_pages(1);
    check(view.get_center_page_number() == 0, "single-page document cannot navigate past end");
    view.move_pages(-1);
    check(view.get_center_page_number() == 0, "single-page document cannot navigate before beginning");
    view.fit_to_page_height_width_minimum(40);
    near(view.get_zoom_level(), 0.45f, "single-page document fills presentation height");
    SCROLL_PAST_DOCUMENT_ENDS = false;
    view.move_pages(1000);
    check(view.get_center_page_number() == 0, "single-page document respects scroll bounds");
    SCROLL_PAST_DOCUMENT_ENDS = true;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    check(tmp.isValid(), "create temporary directory");
    auto locks = get_mupdf_locks();
    fz_context* ctx = fz_new_context(nullptr, &locks, FZ_STORE_DEFAULT);
    check(ctx != nullptr, "create MuPDF context");
    fz_register_document_handlers(ctx);
    SCROLL_PAST_DOCUMENT_ENDS = true;
    TOUCH_MODE = false;
    SAME_WIDTH = false;
    PAGE_SPACE_X = 20;
    PAGE_SPACE_Y = 30;
    FIT_TO_PAGE_WIDTH_RATIO = 1;
    DatabaseManager db;
    auto local = QDir(tmp.path()).filePath("local.db").toStdWString();
    auto shared = QDir(tmp.path()).filePath("shared.db").toStdWString();
    check(db.open(local, shared), "open temporary databases");
    db.ensure_database_compatibility(local, shared);
    CachedChecksummer checksummer(nullptr);
    {
        DocumentManager manager(ctx, &db, &checksummer);
        QString uniform_path = QDir(tmp.path()).filePath("uniform.pdf");
        write_pdf(uniform_path, std::vector<std::pair<int, int>>(8, {600, 800}));
        auto uniform_file = uniform_path.toStdWString();
        checksummer.get_checksum(uniform_file);
        Document* uniform = manager.get_document(uniform_file);
        bool invalid = false;
        check(uniform->open(&invalid, true), "open uniform PDF with synchronous dimensions");
        LayoutView view(&db, &manager, &checksummer);
        view.attach(uniform);
        test_uniform(view);

        QString mixed_path = QDir(tmp.path()).filePath("mixed.pdf");
        write_pdf(mixed_path, {{600, 800}, {900, 1000}, {500, 700}, {650, 750}, {550, 600}, {800, 900}});
        auto mixed_file = mixed_path.toStdWString();
        checksummer.get_checksum(mixed_file);
        Document* mixed = manager.get_document(mixed_file);
        check(mixed->open(&invalid, true), "open mixed-size PDF with synchronous dimensions");
        view.attach(mixed);
        test_mixed(view);

        QString single_path = QDir(tmp.path()).filePath("single.pdf");
        write_pdf(single_path, {{600, 800}});
        auto single_file = single_path.toStdWString();
        checksummer.get_checksum(single_file);
        Document* single = manager.get_document(single_file);
        check(single->open(&invalid, true), "open single-page PDF");
        view.attach(single);
        test_single_document(view);
    }
    fz_drop_context(ctx);
    std::printf("PASS: %d page-layout assertions\n", assertions);
    return 0;
}
