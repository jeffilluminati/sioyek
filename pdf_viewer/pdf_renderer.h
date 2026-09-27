#pragma once

#include <vector>
#include <string>
#include <mupdf/fitz.h>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <variant>
#include <unordered_map>
#include <map>
#include <optional>
#include <iostream>
#include <functional>
#include <thread>
#include <unordered_map>
#include <map>

#include <qobject.h>
#include <qtimer.h>

#include "book.h"


struct RenderRequest {
    std::wstring path;
    bool should_render_annotations = true;
    int page;
    float zoom_level;
    float display_scale;
    int slice_index = -1;
    int num_h_slices = 1;
    int num_v_slices = 1;
};

struct SearchRequest {
    std::wstring path;
    int start_page;
    std::wstring search_term;
    std::vector<SearchResult>* search_results;
    std::mutex* search_results_mutex;
    float* percent_done = nullptr;
    bool* is_searching = nullptr;
    std::optional<std::pair<int, int>> range;
    bool is_regex = false;
};

struct RenderResponse {
    RenderRequest request;
    unsigned int last_access_time;
    int thread;
    fz_pixmap* pixmap = nullptr;
    int width = -1;
    int height = -1;
    GLuint texture = 0;
    bool invalid = false;
    bool pending = true;
    // rendering failed (e.g. the page is too big for a texture); the closest texture there is is drawn
    // instead, and the request isn't rendered again until this is evicted
    bool failed = false;
};

// Largest texture width or height the GPU supports (set when OpenGL is initialized). Pages and tiles
// bigger than this can't be drawn (a texture that big can't be created, OpenGL draws it black), so they
// aren't rendered.
extern std::atomic<int> max_texture_size;

bool operator==(const RenderRequest& lhs, const RenderRequest& rhs);

// Rasterizes the page (or page slice) described by `req`. Called from the render worker threads.
// With helper_threads > 0 the page may be rasterized in horizontal bands by that many extra threads
// (not if it's mostly an image), `bands_used` is set to the number of bands it was drawn in. When `list`
// (the page's display list) is given, it's drawn from that rather than from the page.
fz_pixmap* render_request_pixmap(fz_context* ctx, fz_document* doc, const RenderRequest& req, int helper_threads = 0, int* bands_used = nullptr, fz_display_list* list = nullptr);

// Uploads a rendered pixmap into a new OpenGL texture. Must be called from the thread that owns the GL context.
GLuint create_texture_from_pixmap(fz_pixmap* pixmap);

class PdfRenderer : public QObject {
    Q_OBJECT
        // A pointer to the mupdf context to clone.
        // Since the context should only be used from the thread that initialized it,
        // we can not simply clone the context in the initializer because the initializer
        // is called from the main thread. Instead, we just save a pointer to the context
        // in the initializer and then clone the context when run() is called in the worker
        //thread.
        fz_context* context_to_clone;

    std::vector<std::vector<fz_pixmap*>> pixmaps_to_drop;
    std::map<std::pair<int, std::wstring>, fz_document*> opened_documents;

    std::vector<RenderRequest> pending_render_requests;
    std::optional<SearchRequest> pending_search_request;
    std::vector<RenderResponse> cached_responses;
    std::vector<std::thread> worker_threads;
    std::thread search_thread;
    std::vector<bool> thread_busy_status;
    bool search_is_busy = false;

    std::mutex opened_documents_mutex;
    std::mutex pending_requests_mutex;
    std::mutex search_request_mutex;
    // idle worker threads wait on these for requests
    std::condition_variable pending_requests_cv;
    std::condition_variable search_request_cv;
    std::mutex cached_response_mutex;
    std::vector<std::mutex> pixmap_drop_mutex;
    std::vector<fz_context*> thread_contexts;
    int num_cached_pages = 5;

    std::mutex searching_mutex;
    std::vector<std::mutex> thread_rendering_mutex;

    QTimer garbage_collect_timer;

    bool* should_quit_pointer = nullptr;
    bool are_documents_invalidated = false;

    int num_threads = 0;

    // cores that can be used to render (worker threads and helpers rendering bands of heavy pages)
    int band_cores = 0;
    std::atomic<int> rendering_workers{ 0 };
    std::mutex band_helpers_mutex;
    int band_helpers_in_use = 0;
    // running estimate of how long rendering a megapixel takes in each document
    std::mutex render_cost_mutex;
    std::map<std::wstring, float> render_ms_per_megapixel;
    float estimate_render_ms(const RenderRequest& req);
    int acquire_band_helpers(const RenderRequest& req);
    void release_band_helpers(int helpers);
    void update_render_cost_estimate(const std::wstring& path, fz_pixmap* pixmap, float elapsed_ms, int bands);

    std::map<std::wstring, std::string> document_passwords;

    // Display lists of the pages that are drawn in slices or tiles (see get_page_display_list).
    struct CachedDisplayList {
        std::wstring path;
        int page;
        bool annotations;
        fz_display_list* list = nullptr; // nullptr while a thread is making it
        unsigned long long last_use = 0;
    };
    std::mutex display_lists_mutex;
    std::condition_variable display_lists_cv;
    std::vector<CachedDisplayList> display_lists;
    unsigned long long display_list_uses = 0;
    // incremented when the cached lists are dropped, so that a list made meanwhile isn't cached
    unsigned long long display_list_generation = 0;
    fz_display_list* get_page_display_list(fz_context* ctx, fz_document* doc, const RenderRequest& req, bool make_if_missing);
    void drop_display_lists(const std::wstring* path = nullptr);

    // uploads to textures in the frame being drawn (see begin_frame)
    float frame_upload_budget_ms = 0;
    float frame_upload_ms = 0;
    bool uploads_deferred = false;

    fz_context* init_context();
    fz_document* get_document_with_path(int thread_index, fz_context* mupdf_context, std::wstring path);
    GLuint try_closest_rendered_page(std::wstring doc_path, int page, bool should_render_annotations, int index, int num_h_slices, int num_v_slices, float zoom_level, float display_scale, int* page_width, int* page_height);
    void delete_old_pixmaps(int thread_index, fz_context* mupdf_context);
    void run(int thread_index);
    void run_search(int thread_index);
    int get_pending_response_index_with_thread_index(const RenderRequest& req, int thread_index);

public:
    bool no_rerender = false;

    PdfRenderer(int num_threads, bool* should_quit_pointer, fz_context* context_to_clone);
    ~PdfRenderer();
    void clear_cache();

    void start_threads();
    void join_threads();
    void free_all_resources_for_document(std::wstring doc_path);

    bool is_busy();
    bool is_search_busy();
    //should only be called from the main thread
    void add_request(std::wstring document_path, int page, bool should_render_annotations, float zoom_level, float display_scale, int index, int num_h_slices, int num_v_slices);
    void add_request(std::wstring document_path,
        int page,
        std::wstring term,
        bool is_regex,
        std::vector<SearchResult>* out,
        float* percent_done,
        bool* is_searching,
        std::mutex* mut,
        std::optional<std::pair<int,
        int>> range = {});

    // Returns the texture of the page at the given zoom level, or while that is being rendered, the one
    // closest to it (*exact, if given, tells which).
    GLuint find_rendered_page(std::wstring path, int page, bool should_render_annotations, int index, int num_h_slices, int num_v_slices, float zoom_level, float display_scale, int* page_width, int* page_height, bool* exact = nullptr);
    // Limits the time spent uploading rendered pages to textures in the frame that is about to be drawn
    // (0: no limit). Pages that would go over it are returned as not rendered yet, and uploaded in a later
    // frame: were_uploads_deferred() says whether one should be drawn.
    void begin_frame(float upload_budget_ms);
    bool were_uploads_deferred();

    // The rendered page (or slice) with the zoom level closest to the given one, without requesting
    // anything to be rendered. 0 if there is none.
    GLuint find_closest_rendered_page(std::wstring path, int page, bool should_render_annotations, int index, int num_h_slices, int num_v_slices, float zoom_level, float display_scale, int* page_width, int* page_height);
    void delete_old_pages(bool force_all = false, bool invalidate_all = false);
    void add_password(std::wstring path, std::string password);
    void debug();
    void set_num_cached_pages(int n_cached_pages);

signals:
    void render_advance();
    void search_advance();

};
