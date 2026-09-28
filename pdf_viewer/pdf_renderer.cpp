#include "pdf_renderer.h"
#include "utils.h"
#include <qdatetime.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <atomic>
#include <thread>

extern bool LINEAR_TEXTURE_FILTERING;
extern int NUM_V_SLICES;
extern int NUM_H_SLICES;
extern bool TOUCH_MODE;
//extern bool AUTO_EMBED_ANNOTATIONS;
extern bool CASE_SENSITIVE_SEARCH;
extern bool SMARTCASE_SEARCH;
extern float GAMMA;
extern float DARK_MODE_CONTRAST;
extern float CUSTOM_BACKGROUND_COLOR[3];
extern float CUSTOM_TEXT_COLOR[3];
extern float CUSTOM_COLOR_CONTRAST;
extern int MAX_PENDING_REQUESTS;

// what every OpenGL implementation sioyek runs on supports, until the actual limit is known
std::atomic<int> max_texture_size{ 16384 };
extern unsigned int CACHE_INVALID_MILIES;

// Rendered pages are only ever uploaded to OpenGL textures. On desktop OpenGL we render straight into
// BGRA, which is the drivers' native texture layout, so the upload in the main thread is a plain copy
// instead of a per pixel RGB -> BGRA conversion (about 3x faster for a full page on macOS).
#if !defined(SIOYEK_ANDROID) && defined(GL_BGRA) && defined(GL_UNSIGNED_INT_8_8_8_8_REV)
#define SIOYEK_BGRA_TEXTURES
#endif

// A device that only decodes the images a draw device would need (and so puts them in mupdf's
// store). When a page is drawn in bands, each band asks for just the part of an image it covers.
// Those parts don't come from a common decode, and decoding JPEG or JBIG2 data up to a band's last
// row means decoding everything above it too, so the bands of a scanned page would decode the
// image over and over. mupdf reuses a cached decode of a whole image for any part of it though, so
// decoding the whole images once before drawing the bands avoids that.
struct ImagePrefetchDevice {
    fz_device super;
    fz_rect area;
    // total area of `area` that images are drawn on
    float image_area;
};

static void prefetch_image(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm) {
    ImagePrefetchDevice* prefetch_dev = (ImagePrefetchDevice*)dev;
    fz_rect visible = fz_intersect_rect(fz_transform_rect(fz_unit_rect, ctm), prefetch_dev->area);
    if (fz_is_empty_rect(visible)) {
        return;
    }
    prefetch_dev->image_area += (visible.x1 - visible.x0) * (visible.y1 - visible.y0);
    // the draw device decides the resolution to decode at from this matrix, and so does the cache key
    fz_matrix local_ctm = fz_gridfit_matrix(0, ctm);
    int dw, dh;
    fz_pixmap* pixmap = fz_get_pixmap_from_image(ctx, image, nullptr, &local_ctm, &dw, &dh);
    fz_drop_pixmap(ctx, pixmap);
}

static void prefetch_fill_image(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha, fz_color_params color_params) {
    if (alpha != 0) prefetch_image(ctx, dev, image, ctm);
}

static void prefetch_fill_image_mask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, fz_colorspace* colorspace, const float* color, float alpha, fz_color_params color_params) {
    if (alpha != 0) prefetch_image(ctx, dev, image, ctm);
}

static void prefetch_clip_image_mask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, fz_rect scissor) {
    prefetch_image(ctx, dev, image, ctm);
}

// Decodes the images of `list` that are drawn on `area` and returns the fraction of `area` they cover.
static float prefetch_display_list_images(fz_context* ctx, fz_display_list* list, fz_matrix transform_matrix, fz_rect area) {
    ImagePrefetchDevice* dev = fz_new_derived_device(ctx, ImagePrefetchDevice);
    dev->super.fill_image = prefetch_fill_image;
    dev->super.fill_image_mask = prefetch_fill_image_mask;
    dev->super.clip_image_mask = prefetch_clip_image_mask;
    dev->area = area;
    dev->image_area = 0;
    float coverage = 0;
    fz_try(ctx) {
        fz_run_display_list(ctx, list, &dev->super, transform_matrix, area, nullptr);
        fz_close_device(ctx, &dev->super);
        float total = (area.x1 - area.x0) * (area.y1 - area.y0);
        coverage = total > 0 ? dev->image_area / total : 0;
    }
    fz_always(ctx) {
        fz_drop_device(ctx, &dev->super);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
    return coverage;
}

// Draws rows [y0, y1) of `pixmap` from `list`, through a view of just those rows so that threads
// drawing different bands never touch the same memory.
static void draw_display_list_band(fz_context* ctx, fz_display_list* list, fz_matrix transform_matrix, fz_pixmap* pixmap, int y0, int y1) {
    fz_irect band = { pixmap->x, y0, pixmap->x + pixmap->w, y1 };
    fz_pixmap* view = fz_new_pixmap_from_pixmap(ctx, pixmap, &band);
    fz_device* draw_device = nullptr;
    fz_var(draw_device);
    fz_try(ctx) {
        // like mutool draw's banding: the device works in pixels and the page transform is applied
        // when running the list, so that the band rectangle culls in the same (pixel) space
        draw_device = fz_new_draw_device(ctx, fz_identity, view);
        fz_run_display_list(ctx, list, draw_device, transform_matrix, fz_rect_from_irect(band), nullptr);
        fz_close_device(ctx, draw_device);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, draw_device);
        fz_drop_pixmap(ctx, view);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

fz_pixmap* render_request_pixmap(fz_context* mupdf_context, fz_document* doc, const RenderRequest& req, int helper_threads, int* bands_used, fz_display_list* page_list) {
    fz_matrix transform_matrix = fz_pre_scale(fz_identity, req.zoom_level * req.display_scale, req.zoom_level * req.display_scale);

#ifdef SIOYEK_BGRA_TEXTURES
    fz_colorspace* colorspace = fz_device_bgr(mupdf_context);
    int alpha = 1;
#else
    fz_colorspace* colorspace = fz_device_rgb(mupdf_context);
    int alpha = 0;
#endif

    fz_page* page = fz_load_page(mupdf_context, doc, req.page);
    fz_pixmap* rendered_pixmap = nullptr;
    fz_device* draw_device = nullptr;
    fz_display_list* list = nullptr;

    int final_num_bands = 1;

    fz_var(rendered_pixmap);
    fz_var(draw_device);
    fz_var(list);
    fz_var(final_num_bands);

    fz_try(mupdf_context) {
        fz_rect rect = fz_bound_page(mupdf_context, page);
        fz_irect bbox;
        if (req.slice_index == -1) {
            bbox = fz_round_rect(fz_transform_rect(rect, transform_matrix));
        }
        else {
            bbox = get_index_irect(rect, req.slice_index, transform_matrix, req.num_h_slices, req.num_v_slices);
        }

        if ((bbox.x1 - bbox.x0 > max_texture_size) || (bbox.y1 - bbox.y0 > max_texture_size)) {
            fz_throw(mupdf_context, FZ_ERROR_LIMIT, "page is too big for a texture");
        }

        rendered_pixmap = fz_new_pixmap_with_bbox(mupdf_context, colorspace, bbox, nullptr, alpha);
        // opaque white background (this also sets the alpha channel to 255)
        fz_clear_pixmap_with_value(mupdf_context, rendered_pixmap, 0xFF);

        int num_bands = std::max(1, std::min(helper_threads + 1, rendered_pixmap->h / 64));
        if ((num_bands == 1) && page_list) {
            draw_display_list_band(mupdf_context, page_list, transform_matrix, rendered_pixmap, rendered_pixmap->y, rendered_pixmap->y + rendered_pixmap->h);
        }
        else if (num_bands == 1) {
            draw_device = fz_new_draw_device(mupdf_context, transform_matrix, rendered_pixmap);

            if (req.should_render_annotations) {
                fz_run_page(mupdf_context, page, draw_device, fz_identity, nullptr); // todo: use cookie to report progress
            }
            else {
                fz_run_page_contents(mupdf_context, page, draw_device, fz_identity, nullptr); // todo: use cookie to report progress
            }
            fz_close_device(mupdf_context, draw_device);
        }
        else {
            // Heavy page: interpret it once into a display list and rasterize horizontal bands of
            // it in parallel. Rasterizing (filling paths, decoding and scaling images) is where the
            // time goes on such pages, so this divides most of the render time by the number of bands.
            if (page_list) {
                list = fz_keep_display_list(mupdf_context, page_list);
            }
            else if (req.should_render_annotations) {
                list = fz_new_display_list_from_page(mupdf_context, page);
            }
            else {
                list = fz_new_display_list_from_page_contents(mupdf_context, page);
            }

            float image_coverage = prefetch_display_list_images(mupdf_context, list, transform_matrix, fz_rect_from_irect(bbox));
            if (image_coverage > 0.5f) {
                // Mostly an image (e.g. a scanned page): decoding it can't be split, and while
                // scrolling, bands scaling a big image in parallel slowed down the other pages being
                // rendered more than they sped up this one. Draw it in one piece.
                num_bands = 1;
            }

            final_num_bands = num_bands;
            int y0 = rendered_pixmap->y;
            int h = rendered_pixmap->h;
            auto band_y = [&](int band) { return y0 + (int)(((long long)h * band) / num_bands); };

            std::vector<fz_context*> helper_contexts;
            std::vector<std::thread> helpers;
            std::atomic<bool> helper_failed{ false };
            for (int band = 1; band < num_bands; band++) {
                helper_contexts.push_back(fz_clone_context(mupdf_context));
            }
            for (int band = 1; band < num_bands; band++) {
                fz_context* ctx = helper_contexts[band - 1];
                helpers.emplace_back([&, ctx, band]() {
                    fz_try(ctx) {
                        draw_display_list_band(ctx, list, transform_matrix, rendered_pixmap, band_y(band), band_y(band + 1));
                    }
                    fz_catch(ctx) {
                        helper_failed = true;
                    }
                    });
            }
            bool failed = false;
            fz_try(mupdf_context) {
                draw_display_list_band(mupdf_context, list, transform_matrix, rendered_pixmap, band_y(0), band_y(1));
            }
            fz_catch(mupdf_context) {
                failed = true;
            }
            for (auto& helper : helpers) {
                helper.join();
            }
            for (fz_context* ctx : helper_contexts) {
                fz_drop_context(ctx);
            }
            if (failed || helper_failed) {
                fz_throw(mupdf_context, FZ_ERROR_GENERIC, "could not render page band");
            }
        }
    }
    fz_always(mupdf_context) {
        fz_drop_device(mupdf_context, draw_device);
        fz_drop_display_list(mupdf_context, list);
        fz_drop_page(mupdf_context, page);
    }
    fz_catch(mupdf_context) {
        fz_drop_pixmap(mupdf_context, rendered_pixmap);
        fz_rethrow(mupdf_context);
    }

    if (GAMMA != 1.0f) {
        fz_gamma_pixmap(mupdf_context, rendered_pixmap, GAMMA);
    }
    if (bands_used) {
        *bands_used = final_num_bands;
    }
    return rendered_pixmap;
}

// Estimates the color of the page's paper from a render of the page (or of a slice or tile of it): the
// median of the pixels along the edges of the page, a little inside them (scans often have dark edges).
// Returns false if the pixmap has none of the page's edges.
static bool estimate_paper_color(fz_pixmap* pixmap, const RenderRequest& req, uint32_t* color) {
    int w = pixmap->w;
    int h = pixmap->h;
    int n = pixmap->n;
    if ((w <= 0) || (h <= 0) || (n < 3)) return false;

    int nh = 1, nv = 1, h_index = 0, v_index = 0;
    if (req.slice_index >= 0) {
        nh = std::max(1, req.num_h_slices);
        nv = std::max(1, req.num_v_slices);
        h_index = req.slice_index % nh;
        v_index = req.slice_index / nh;
    }
    // 2% of the page's width and height in
    int inset_x = std::max(1, w * nh / 50);
    int inset_y = std::max(1, h * nv / 50);

    std::vector<unsigned char> channels[3];
    auto sample = [&](int x, int y) {
        const unsigned char* pixel = pixmap->samples + static_cast<size_t>(y) * pixmap->stride + static_cast<size_t>(x) * n;
#ifdef SIOYEK_BGRA_TEXTURES
        channels[0].push_back(pixel[2]);
        channels[1].push_back(pixel[1]);
        channels[2].push_back(pixel[0]);
#else
        channels[0].push_back(pixel[0]);
        channels[1].push_back(pixel[1]);
        channels[2].push_back(pixel[2]);
#endif
    };
    int step_x = std::max(1, w / 256);
    int step_y = std::max(1, h / 256);
    if ((inset_x < w / 2) && (h_index == 0)) for (int y = 0; y < h; y += step_y) sample(inset_x, y);
    if ((inset_x < w / 2) && (h_index == nh - 1)) for (int y = 0; y < h; y += step_y) sample(w - 1 - inset_x, y);
    if ((inset_y < h / 2) && (v_index == 0)) for (int x = 0; x < w; x += step_x) sample(x, inset_y);
    if ((inset_y < h / 2) && (v_index == nv - 1)) for (int x = 0; x < w; x += step_x) sample(x, h - 1 - inset_y);
    if (channels[0].empty()) return false;

    uint32_t result = 0;
    for (auto& channel : channels) {
        std::nth_element(channel.begin(), channel.begin() + channel.size() / 2, channel.end());
        result = (result << 8) | channel[channel.size() / 2];
    }
    *color = result;
    return true;
}

GLuint create_texture_from_pixmap(fz_pixmap* pixmap) {
    GLuint result = 0;
    glGenTextures(1, &result);
    glBindTexture(GL_TEXTURE_2D, result);

    if (LINEAR_TEXTURE_FILTERING) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

#ifdef GL_CLAMP
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#endif

    // rows of a pixmap are tightly packed, so unless each pixel is 4 bytes they are not necessarily
    // aligned to 4 bytes
    int alignment = (pixmap->n == 4) ? 4 : 1;
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
#ifdef SIOYEK_BGRA_TEXTURES
    if (pixmap->n == 4) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pixmap->w, pixmap->h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixmap->samples);
    }
    else
#endif
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pixmap->w, pixmap->h, 0, GL_RGB, GL_UNSIGNED_BYTE, pixmap->samples);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return result;
}

PdfRenderer::PdfRenderer(int num_threads, bool* should_quit_pointer, fz_context* context_to_clone) : context_to_clone(context_to_clone),
pixmaps_to_drop(num_threads),
pixmap_drop_mutex(num_threads),
thread_rendering_mutex(num_threads),
thread_contexts(num_threads),
should_quit_pointer(should_quit_pointer),
num_threads(num_threads)
{

    // this interval must be less than cache invalidation time
    garbage_collect_timer.setInterval(1000);
    garbage_collect_timer.start();

    for (int i = 0; i < num_threads; i++) {
        thread_busy_status.push_back(false);
    }

    // Cores that aren't busy rendering can help rendering heavy pages (see render_request_pixmap).
    // One is left for the main thread. Only performance cores count: a page rendered in bands is only
    // done when its slowest band is, so a band on an efficiency core would delay the whole page.
    band_cores = std::max(0, get_num_performance_cores() - 1);
    QObject::connect(&garbage_collect_timer, &QTimer::timeout, [&]() {
        delete_old_pages();
        });
}

fz_context* PdfRenderer::init_context() {
    return fz_clone_context(context_to_clone);
}


void PdfRenderer::start_threads() {

    for (int i = 0; i < num_threads; i++) {
        worker_threads.push_back(std::thread([&, i]() {
            run(i);
            }));
    }
    search_thread = std::thread([&]() {
        run_search(num_threads);
        });
}

void PdfRenderer::join_threads()
{
    for (auto& worker : worker_threads) {
        worker.join();
    }
    search_thread.join();
}


void PdfRenderer::add_request(std::wstring document_path, int page, bool should_render_annotations, float zoom_level, float display_scale, int index, int num_h_slices, int num_v_slices) {
    //fz_document* doc = get_document_with_path(document_path);
    if (document_path.size() > 0) {
        RenderRequest req;
        req.path = document_path;
        req.page = page;
        req.zoom_level = zoom_level;
        req.slice_index = index;
        req.num_h_slices = num_h_slices;
        req.num_v_slices = num_v_slices;
        req.display_scale = display_scale;
        req.should_render_annotations = should_render_annotations;

        pending_requests_mutex.lock();
        // if the zoom level has changed, there is no point in previous requests with a different zoom level
        // (a page drawn in tiles also needs the whole page at another zoom level, so requests for tiles
        // only replace requests for tiles and requests for whole pages only those for whole pages)
        for (int i = pending_render_requests.size() - 1; i >= 0; i--) {
            const RenderRequest& pending = pending_render_requests[i];
            if (pending.path == req.path && pending.page == req.page && (pending.zoom_level != zoom_level) &&
                ((pending.slice_index == -1) == (req.slice_index == -1))) {
                pending_render_requests.erase(pending_render_requests.begin() + i);
            }
        }
        bool should_add = true;
        for (size_t i = 0; i < pending_render_requests.size(); i++) {
            if (pending_render_requests[i] == req) {
                should_add = false;
            }
        }
        if (should_add) {
            pending_render_requests.push_back(req);
        }
        if (pending_render_requests.size() > (size_t) MAX_PENDING_REQUESTS) {
            pending_render_requests.erase(pending_render_requests.begin());
        }
        pending_requests_mutex.unlock();
        if (should_add) {
            pending_requests_cv.notify_one();
        }
    }
    else {
        std::wcout << "Error: could not find documnet" << std::endl;
    }
}
void PdfRenderer::add_request(std::wstring document_path,
    int page,
    std::wstring term,
    bool is_regex,
    std::vector<SearchResult>* out,
    float* percent_done,
    bool* is_searching,
    std::mutex* mut,
    std::optional<std::pair<int, int>> range) {

    //fz_document* doc = get_document_with_path(document_path);
    if (document_path.size() > 0) {

        SearchRequest req;
        req.path = document_path;
        req.start_page = page;
        req.search_term = term;
        req.search_results_mutex = mut;
        req.search_results = out;
        req.percent_done = percent_done;
        req.is_searching = is_searching;
        req.range = range;
        req.is_regex = is_regex;

        search_request_mutex.lock();
        pending_search_request = req;
        search_request_mutex.unlock();
        search_request_cv.notify_one();
    }
    else {
        std::wcout << "Error: could not find document" << std::endl;
    }
}

//should only be called from the main thread

GLuint PdfRenderer::find_rendered_page(std::wstring path, int page, bool should_render_annotations, int index, int num_h_slices, int num_v_slices, float zoom_level, float display_scale, int* page_width, int* page_height, bool* exact) {
    if (exact) *exact = false;
    //fz_document* doc = get_document_with_path(path);
    if (path.size() > 0) {
        RenderRequest req;
        req.path = path;
        req.page = page;
        req.zoom_level = zoom_level;
        req.slice_index = index;
        req.num_h_slices = num_h_slices;
        req.num_v_slices = num_v_slices;
        req.display_scale = display_scale;
        req.should_render_annotations = should_render_annotations;
        cached_response_mutex.lock();
        GLuint result = 0;
        for (auto& cached_resp : cached_responses) {
            if (cached_resp.pending || cached_resp.failed) continue;

            if ((cached_resp.request == req) && (cached_resp.invalid == false)) {
                cached_resp.last_access_time = QDateTime::currentMSecsSinceEpoch();

                if (page_width) *page_width = cached_resp.width;
                if (page_height) *page_height = cached_resp.height;

                // We can only use OpenGL in the main thread, so we can not upload the rendered
                // pixmap into a texture in the worker thread, so whenever we get a rendered page
                // in the main thread, we initialize its OpenGL texture if it is not initialized already
                if (cached_resp.texture != 0) {
                    result = cached_resp.texture;
                }
                else if ((frame_upload_budget_ms > 0) && (frame_upload_ms >= frame_upload_budget_ms)) {
                    // this frame has spent its time on uploads, it's uploaded in a later one
                    uploads_deferred = true;
                }
                else {
                    auto upload_begin = std::chrono::steady_clock::now();
                    result = create_texture_from_pixmap(cached_resp.pixmap);
                    frame_upload_ms += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - upload_begin).count();

                    // don't need the pixmap anymore
                    pixmap_drop_mutex[cached_resp.thread].lock();
                    pixmaps_to_drop[cached_resp.thread].push_back(cached_resp.pixmap);
                    cached_resp.texture = result;
                    pixmap_drop_mutex[cached_resp.thread].unlock();

                }
                break;
            }
        }
        cached_response_mutex.unlock();
        if (exact) *exact = result != 0;
        if (result == 0) {
            if (TOUCH_MODE) {
                if (!no_rerender) {
                    add_request(path, page, should_render_annotations, zoom_level, display_scale, index, num_h_slices, num_v_slices);
                }
            }
            else {
                add_request(path, page, should_render_annotations, zoom_level, display_scale, index, num_h_slices, num_v_slices);
            }
            return try_closest_rendered_page(
                path,
                page,
                should_render_annotations,
                index,
                num_h_slices,
                num_v_slices,
                zoom_level,
                display_scale,
                page_width,
                page_height
            );
        }
        return result;
    }
    return 0;
}

void PdfRenderer::begin_frame(float upload_budget_ms) {
    frame_upload_budget_ms = upload_budget_ms;
    frame_upload_ms = 0;
    uploads_deferred = false;
}

bool PdfRenderer::were_uploads_deferred() {
    return uploads_deferred;
}

// Returns (a reference to) the display list of the page of `req`, from the cache if it's there. If it
// isn't, it is made and cached if make_if_missing (otherwise, or if it can't be made, nullptr is returned). Several threads
// rendering tiles of a page at the same time wait for the one that makes it. The lists are made with
// the thread's own document, but only hold references to fonts and images, so any worker can draw them.
fz_display_list* PdfRenderer::get_page_display_list(fz_context* ctx, fz_document* doc, const RenderRequest& req, bool make_if_missing) {
    const int max_cached_lists = 4;

    std::unique_lock<std::mutex> lock(display_lists_mutex);
    while (true) {
        auto it = std::find_if(display_lists.begin(), display_lists.end(), [&](const CachedDisplayList& cached) {
            return (cached.path == req.path) && (cached.page == req.page) && (cached.annotations == req.should_render_annotations);
            });
        if (it == display_lists.end()) break;
        if (it->list) {
            it->last_use = ++display_list_uses;
            return fz_keep_display_list(ctx, it->list);
        }
        // another thread is making it
        display_lists_cv.wait(lock);
    }
    if (!make_if_missing) {
        return nullptr;
    }

    CachedDisplayList placeholder;
    placeholder.path = req.path;
    placeholder.page = req.page;
    placeholder.annotations = req.should_render_annotations;
    placeholder.last_use = ++display_list_uses;
    display_lists.push_back(placeholder);
    unsigned long long generation = display_list_generation;
    lock.unlock();

    fz_display_list* list = nullptr;
    fz_page* page = nullptr;
    bool failed = false;
    fz_var(list);
    fz_var(page);
    fz_try(ctx) {
        page = fz_load_page(ctx, doc, req.page);
        if (req.should_render_annotations) {
            list = fz_new_display_list_from_page(ctx, page);
        }
        else {
            list = fz_new_display_list_from_page_contents(ctx, page);
        }
    }
    fz_always(ctx) {
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        failed = true;
    }

    lock.lock();
    auto it = std::find_if(display_lists.begin(), display_lists.end(), [&](const CachedDisplayList& cached) {
        return (cached.path == req.path) && (cached.page == req.page) && (cached.annotations == req.should_render_annotations) && (cached.list == nullptr);
        });
    if (failed || (generation != display_list_generation)) {
        // not cached: it failed, or the cache was cleared meanwhile (the document may have changed)
        if (it != display_lists.end()) display_lists.erase(it);
    }
    else {
        if (it != display_lists.end()) it->list = fz_keep_display_list(ctx, list);
        // evict the least recently used lists (not ones being made)
        while (display_lists.size() > max_cached_lists) {
            auto oldest = display_lists.end();
            for (auto candidate = display_lists.begin(); candidate != display_lists.end(); candidate++) {
                if (candidate->list && ((oldest == display_lists.end()) || (candidate->last_use < oldest->last_use))) {
                    oldest = candidate;
                }
            }
            if (oldest == display_lists.end()) break;
            fz_drop_display_list(ctx, oldest->list);
            display_lists.erase(oldest);
        }
    }
    display_lists_cv.notify_all();
    lock.unlock();

    // (not thrown: that would skip the destructors of this function's locals; without a list the page
    // is drawn directly, which reports the error)
    return failed ? nullptr : list;
}

void PdfRenderer::drop_display_lists(const std::wstring* path) {
    std::lock_guard<std::mutex> lock(display_lists_mutex);
    display_list_generation++;
    for (int i = static_cast<int>(display_lists.size()) - 1; i >= 0; i--) {
        if (path && (display_lists[i].path != *path)) continue;
        // lists being made are erased by the thread making them (the generation changed)
        if (display_lists[i].list == nullptr) continue;
        fz_drop_display_list(context_to_clone, display_lists[i].list);
        display_lists.erase(display_lists.begin() + i);
    }
}

GLuint PdfRenderer::find_closest_rendered_page(std::wstring path, int page, bool should_render_annotations, int index, int num_h_slices, int num_v_slices, float zoom_level, float display_scale, int* page_width, int* page_height) {
    return try_closest_rendered_page(path, page, should_render_annotations, index, num_h_slices, num_v_slices, zoom_level, display_scale, page_width, page_height);
}

GLuint PdfRenderer::try_closest_rendered_page(std::wstring doc_path, int page, bool should_render_annotations, int index, int num_h_slices, int num_v_slices, float zoom_level, float display_scale, int* page_width, int* page_height) {
    /*
    If the requested page is not available, we try to find the rendered page with the closest
    possible zoom level to our request and return that instead
    */
    cached_response_mutex.lock();

    // the one whose zoom level is closest (by ratio: 1.0 is as far from 2.0 as 4.0 is)
    float min_diff = std::numeric_limits<float>::infinity();
    GLuint best_texture = 0;
    RenderResponse* best_response = nullptr;

    for (auto& cached_resp : cached_responses) {
        if (cached_resp.pending) continue;
        if ((cached_resp.request.slice_index == index) &&
            (cached_resp.request.num_h_slices == num_h_slices) &&
            (cached_resp.request.num_v_slices == num_v_slices) &&
            (cached_resp.request.path == doc_path) &&
            (cached_resp.request.display_scale == display_scale) &&
            (cached_resp.request.should_render_annotations == should_render_annotations) &&
            (cached_resp.request.page == page) &&
            (cached_resp.texture != 0)) {
            float diff = std::abs(std::log(cached_resp.request.zoom_level / zoom_level));
            if (diff <= min_diff) {
                min_diff = diff;
                best_texture = cached_resp.texture;
                best_response = &cached_resp;
                if (page_width) *page_width = static_cast<int>(cached_resp.width * zoom_level / cached_resp.request.zoom_level);
                if (page_height) *page_height = static_cast<int>(cached_resp.height * zoom_level / cached_resp.request.zoom_level);
            }
        }
    }
    if (best_response) {
        // the texture is going to be drawn until the requested one is rendered, so it counts as used
        // and delete_old_pages won't delete it from under us
        best_response->last_access_time = QDateTime::currentMSecsSinceEpoch();
    }
    cached_response_mutex.unlock();
    return best_texture;
}

uint32_t PdfRenderer::get_paper_color(const std::wstring& path, int page) {
    std::lock_guard<std::mutex> lock(cached_response_mutex);
    auto document_colors = paper_colors.find(path);
    if ((document_colors == paper_colors.end()) || document_colors->second.empty()) {
        return 0xFFFFFF;
    }
    // the page's own, or that of the closest page before or after it
    const std::map<int, uint32_t>& colors = document_colors->second;
    auto after = colors.lower_bound(page);
    if (after == colors.end()) return std::prev(after)->second;
    if ((after->first == page) || (after == colors.begin())) return after->second;
    auto before = std::prev(after);
    return ((page - before->first) <= (after->first - page)) ? before->second : after->second;
}

void PdfRenderer::delete_old_pages(bool force_all, bool invalidate_all) {
    /*
    Deletes old cached pages. This function should only be called from the main thread.
    OpenGL textures are released immediately but pixmaps should be freed from the thread
    that created them, so we add old pixmaps to pixmaps_to_delete and delete them later from the worker thread
    */

    if (no_rerender) {
        // don't release while resizing, can cause a blank screen
        return;
    }

    cached_response_mutex.lock();
    std::vector<int> indices_to_delete;
    unsigned int now = QDateTime::currentMSecsSinceEpoch();

    if (invalidate_all) {
        for (size_t i = 0; i < cached_responses.size(); i++) {
            cached_responses[i].invalid = true;
        }
        are_documents_invalidated = true;
    }

    if (force_all) {
        for (size_t i = 0; i < cached_responses.size(); i++) {
            indices_to_delete.push_back(i);
        }
        are_documents_invalidated = true;
    }

    if (force_all || invalidate_all) {
        // the document may have changed (or is being closed)
        drop_display_lists();
        paper_colors.clear();
    }
    else {
        // We never delete the most recently used responses that add up to num_cached_pages pages. A
        // response for a slice counts as the corresponding fraction of a page. (This used to keep the
        // num_cached_pages * NUM_V_SLICES * NUM_H_SLICES most recent responses, but pages are only
        // sliced in special cases, so it kept 25 whole pages alive, ~17MB of texture each on a retina
        // display.) We also never delete what was drawn in the latest frame, even when that is more
        // than num_cached_pages pages (e.g. when zoomed out), nor requests that are still being
        // rendered, since the worker thread still needs their entry to store the result.
        std::vector<int> by_recency;
        for (size_t i = 0; i < cached_responses.size(); i++) {
            if (!cached_responses[i].pending) {
                by_recency.push_back(i);
            }
        }
        std::sort(by_recency.begin(), by_recency.end(), [&](int lhs, int rhs) {
            return cached_responses[lhs].last_access_time > cached_responses[rhs].last_access_time;
            });

        // textures used within this long of the most recent use were drawn in the same frame
        const unsigned int frame_window = 50;
        unsigned int latest_access = by_recency.size() > 0 ? cached_responses[by_recency[0]].last_access_time : now;

        float num_pages = 0;
        for (int i : by_recency) {
            const RenderResponse& resp = cached_responses[i];
            num_pages += (resp.request.slice_index == -1) ? 1.0f : 1.0f / (resp.request.num_h_slices * resp.request.num_v_slices);
            bool in_latest_frame = (latest_access - resp.last_access_time) <= frame_window;
            if (!in_latest_frame && (num_pages > num_cached_pages + 0.001f) && ((now - resp.last_access_time) > CACHE_INVALID_MILIES)) {
                indices_to_delete.push_back(i);
            }
        }
        // the deletion loop below expects increasing indices
        std::sort(indices_to_delete.begin(), indices_to_delete.end());
    }

    // We erase from back to front so that erasing one element does not change
    // the index of other elements
    for (int j = indices_to_delete.size() - 1; j >= 0; j--) {
        int index_to_delete = indices_to_delete[j];
        RenderResponse resp = cached_responses[index_to_delete];

        pixmap_drop_mutex[resp.thread].lock();
        if (resp.texture == 0) {
            pixmaps_to_drop[resp.thread].push_back(resp.pixmap);
        }
        pixmap_drop_mutex[resp.thread].unlock();

        if (resp.texture != 0) {
            glDeleteTextures(1, &resp.texture);
        }

        cached_responses.erase(cached_responses.begin() + index_to_delete);
    }
    cached_response_mutex.unlock();
}

void PdfRenderer::run_search(int thread_index)
{
    fz_context* mupdf_context = init_context();

    while (!(*should_quit_pointer)) {
        if (pending_search_request.has_value()) {

            search_request_mutex.lock();
            SearchRequest req = pending_search_request.value();
            pending_search_request = {};
            search_request_mutex.unlock();

            SearchCaseSensitivity search_case_sensitivity = SearchCaseSensitivity::CaseInsensitive;
            if (CASE_SENSITIVE_SEARCH) search_case_sensitivity = SearchCaseSensitivity::CaseSensitive;
            if (SMARTCASE_SEARCH) search_case_sensitivity = SearchCaseSensitivity::SmartCase;

            search_is_busy = true;
            searching_mutex.lock();
            fz_document* doc = get_document_with_path(thread_index, mupdf_context, req.path);

            int num_pages_in_document = fz_count_pages(mupdf_context, doc);

            int page_begin = 0;
            int page_end = num_pages_in_document - 1;

            if (req.range) {
                page_begin = req.range.value().first - 1;
                page_end = req.range.value().second - 1;
            }
            // make sure page range is valid {
            if (page_begin < 0) page_begin = 0;
            if (page_begin > num_pages_in_document - 1) page_begin = num_pages_in_document - 1;
            if (page_end < 0) page_end = 0;
            if (page_end > num_pages_in_document - 1) page_end = num_pages_in_document - 1;
            //}

            int num_pages = page_end - page_begin + 1;

            req.search_results_mutex->lock();
            req.search_results->clear();
            *req.is_searching = true;
            req.search_results_mutex->unlock();

            if (req.start_page > page_end || req.start_page < page_begin) {
                req.start_page = page_begin;
            }

            int num_handled_pages = 0;
            int i = req.start_page;
            while (num_handled_pages < num_pages && (!pending_search_request.has_value()) && (!(*should_quit_pointer))) {
                num_handled_pages++;

                fz_stext_page* stext_page = fz_new_stext_page_from_page_number(mupdf_context, doc, i, nullptr);

                std::vector<fz_stext_char*> flat_chars;
                get_flat_chars_from_stext_page(stext_page, flat_chars, false);
                std::wstring page_text;
                std::vector<int> page_begin_indices;
                flat_char_prism2(flat_chars, i, page_text, page_begin_indices);
                req.search_results_mutex->lock();

                std::vector<SearchResult> page_results;
                if (req.is_regex == false) {
                    page_results = search_text_with_index(page_text, page_begin_indices, req.search_term, search_case_sensitivity, 0, 0, 0);
                }
                else {
                    page_results = search_regex_with_index(page_text, page_begin_indices, req.search_term, search_case_sensitivity, 0, 0, 0);
                }

                for (auto& pr : page_results) {
                    pr.page = i;
                }

                req.search_results->insert(req.search_results->end(), page_results.begin(), page_results.end());
                req.search_results_mutex->unlock();

                if (num_handled_pages % 16 == 0) {
                    *req.percent_done = (float)num_handled_pages / num_pages;
                    //*invalidate_pointer = true;
                    emit search_advance();
                }

                fz_drop_stext_page(mupdf_context, stext_page);


                i++;
                if (i > page_end) {
                    i = page_begin;
                }
            }
            searching_mutex.unlock();
            req.search_results_mutex->lock();
            *req.is_searching = false;
            //*invalidate_pointer = true;
            //if (on_search_invalidate) {
            //	on_search_invalidate();
            //}
            emit search_advance();
            req.search_results_mutex->unlock();
        }
        else {
            search_is_busy = false;
            std::unique_lock<std::mutex> lock(search_request_mutex);
            search_request_cv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                return pending_search_request.has_value() || *should_quit_pointer;
                });
        }
    }
}

PdfRenderer::~PdfRenderer() {
}

fz_document* PdfRenderer::get_document_with_path(int thread_index, fz_context* mupdf_context, std::wstring path) {

    std::pair<int, std::wstring> document_id = std::make_pair(thread_index, path);

    {
        std::lock_guard<std::mutex> lock(opened_documents_mutex);
        if (opened_documents.find(document_id) != opened_documents.end()) {
            return opened_documents.at(document_id);
        }
    }

    fz_document* ret_val = nullptr;
    fz_try(mupdf_context) {
        //		ret_val = fz_open_document(mupdf_context, utf8_encode(path).c_str());
        ret_val = open_document_with_file_name(mupdf_context, path);

        if (fz_needs_password(mupdf_context, ret_val)) {
            if (document_passwords.find(path) != document_passwords.end()) {
                fz_authenticate_password(mupdf_context, ret_val, document_passwords[path].c_str());
            }
        }

        {

            std::lock_guard<std::mutex> lock(opened_documents_mutex);
            opened_documents[make_pair(thread_index, path)] = ret_val;
        }
    }
    fz_catch(mupdf_context) {
        std::wcout << "Error: could not open document" << std::endl;
    }

    return ret_val;
}

void PdfRenderer::delete_old_pixmaps(int thread_index, fz_context* mupdf_context) {
    // this function should only be called from the worker thread
    pixmap_drop_mutex[thread_index].lock();
    for (size_t i = 0; i < pixmaps_to_drop[thread_index].size(); i++) {
        fz_try(mupdf_context) {
            fz_drop_pixmap(mupdf_context, pixmaps_to_drop[thread_index][i]);
        }
        fz_catch(mupdf_context) {
            std::wcout << "Error: could not drop pixmap" << std::endl;
        }
    }
    pixmaps_to_drop[thread_index].clear();
    pixmap_drop_mutex[thread_index].unlock();
}
void PdfRenderer::clear_cache() {
    delete_old_pages(false, true);
}

void PdfRenderer::run(int thread_index) {
    fz_context* mupdf_context = init_context();
    thread_contexts[thread_index] = mupdf_context;

    while (!(*should_quit_pointer)) {
        // pixmaps that were uploaded to textures (or evicted) since the last iteration. If we only
        // did this when idle, they would pile up (~17MB each) while the user keeps scrolling.
        delete_old_pixmaps(thread_index, mupdf_context);

        pending_requests_mutex.lock();

        bool quitting = false;
        while (pending_render_requests.size() == 0) {
            pending_requests_mutex.unlock();
            cached_response_mutex.lock();
            for (int i = 0; i < cached_responses.size(); i++) {
                if ((cached_responses[i].thread == thread_index) && (cached_responses[i].texture == 0) && (cached_responses[i].pixmap == nullptr)) {
                    cached_responses[i].invalid = true;
                }
            }
            cached_response_mutex.unlock();
            delete_old_pixmaps(thread_index, mupdf_context);
            if (*should_quit_pointer) {
                // pending_requests_mutex is not locked here
                quitting = true;
                break;
            }

            thread_busy_status[thread_index] = false;
            {
                // Wait until add_request wakes us up (polling instead would delay every render that is
                // requested while the workers are idle, e.g. after zooming, by up to the polling interval),
                // looking at should_quit_pointer now and then.
                std::unique_lock<std::mutex> lock(pending_requests_mutex);
                pending_requests_cv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                    return pending_render_requests.size() > 0 || *should_quit_pointer;
                    });
                lock.release(); // the loop goes on with pending_requests_mutex locked
            }
        }
        if (quitting) break;
        if (*should_quit_pointer) {
            // There are still pending requests and we hold pending_requests_mutex. Release it, otherwise
            // the other worker threads wait for it forever and joining them on exit hangs.
            pending_requests_mutex.unlock();
            break;
        }
        //cout << "worker thread running ... pending requests: " << pending_render_requests.size() << endl;

        RenderRequest req = pending_render_requests[pending_render_requests.size() - 1];

        // if the request is already rendered, just return the previous result
        cached_response_mutex.lock();

        if (are_documents_invalidated) {
            std::lock_guard<std::mutex> lock(opened_documents_mutex);
            for (auto [_, document] : opened_documents) {
                fz_drop_document(mupdf_context, document);
            }
            opened_documents.clear();
            are_documents_invalidated = false;
        }

        bool is_already_rendered = false;
        for (const auto& cached_rep : cached_responses) {
            if ((cached_rep.request == req) && (cached_rep.invalid == false)) is_already_rendered = true;
        }

        if (!is_already_rendered) {
            RenderResponse resp;
            resp.thread = thread_index;
            resp.request = req;
            resp.last_access_time = QDateTime::currentMSecsSinceEpoch();
            resp.texture = 0;
            resp.invalid = false;
            resp.pending = true;
            cached_responses.push_back(resp);
        }

        cached_response_mutex.unlock();
        pending_render_requests.pop_back();
        pending_requests_mutex.unlock();
        thread_rendering_mutex[thread_index].lock();
        thread_busy_status[thread_index] = true;

        if (!is_already_rendered) {

            // -1 until this worker counts as rendering
            int helpers = -1;
            fz_display_list* page_list = nullptr;
            fz_var(helpers);
            fz_var(page_list);
            fz_try(mupdf_context) {
                fz_document* doc = get_document_with_path(thread_index, mupdf_context, req.path);

                // Tiles and slices of a page are drawn from its display list, so that its contents are
                // interpreted once rather than for every tile (on pages with many paths that is most of
                // the time). A whole page uses the list if there is one.
                page_list = get_page_display_list(mupdf_context, doc, req, req.slice_index != -1);

                rendering_workers++;
                helpers = acquire_band_helpers(req);
                auto render_begin = std::chrono::steady_clock::now();
                int bands_used = 1;
                fz_pixmap* rendered_pixmap = render_request_pixmap(mupdf_context, doc, req, helpers, &bands_used, page_list);
                float elapsed_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - render_begin).count();
                update_render_cost_estimate(req.path, rendered_pixmap, elapsed_ms, bands_used);
                release_band_helpers(helpers);
                helpers = -1;
                rendering_workers--;
                uint32_t paper_color = 0;
                bool has_paper_color = estimate_paper_color(rendered_pixmap, req, &paper_color);

                cached_response_mutex.lock();
                if (has_paper_color) {
                    paper_colors[req.path][req.page] = paper_color;
                }
                int index = get_pending_response_index_with_thread_index(req, thread_index);
                if (index >= 0) {
                    cached_responses[index].last_access_time = QDateTime::currentMSecsSinceEpoch();
                    cached_responses[index].pixmap = rendered_pixmap;
                    cached_responses[index].width = rendered_pixmap->w;
                    cached_responses[index].height = rendered_pixmap->h;
                    cached_responses[index].pending = false;
                }
                else {
                    // the entry was deleted while we were rendering (e.g. all pages were invalidated)
                    fz_drop_pixmap(mupdf_context, rendered_pixmap);
                }

                cached_response_mutex.unlock();

                emit render_advance();

            }
            fz_always(mupdf_context) {
                fz_drop_display_list(mupdf_context, page_list);
            }
            fz_catch(mupdf_context) {
                // helpers is -1 when the render itself succeeded and the counters were already updated
                if (helpers >= 0) {
                    release_band_helpers(helpers);
                    rendering_workers--;
                }
                // Keep the entry as failed: while it is cached, the page is drawn from the closest texture
                // there is and not rendered again (removing it would render it again every frame).
                cached_response_mutex.lock();
                int index = get_pending_response_index_with_thread_index(req, thread_index);
                if (index >= 0) {
                    cached_responses[index].pending = false;
                    cached_responses[index].failed = true;
                    cached_responses[index].last_access_time = QDateTime::currentMSecsSinceEpoch();
                }
                cached_response_mutex.unlock();
                emit render_advance();
                std::cerr << "Error: could not render page: " << fz_caught_message(mupdf_context) << std::endl;
            }
        }
        thread_rendering_mutex[thread_index].unlock();

    }
}

void PdfRenderer::add_password(std::wstring path, std::string password) {
    document_passwords[path] = password;
    delete_old_pages(true, false);
}

bool operator==(const RenderRequest& lhs, const RenderRequest& rhs) {
    if (rhs.path != lhs.path) {
        return false;
    }
    if (rhs.slice_index != lhs.slice_index) {
        return false;
    }
    if (rhs.num_h_slices != lhs.num_h_slices) {
        return false;
    }
    if (rhs.num_v_slices != lhs.num_v_slices) {
        return false;
    }
    if (rhs.page != lhs.page) {
        return false;
    }
    if (rhs.zoom_level != lhs.zoom_level) {
        return false;
    }
    if (rhs.display_scale != lhs.display_scale) {
        return false;
    }
    if (rhs.should_render_annotations != lhs.should_render_annotations) {
        return false;
    }
    return true;
}


bool PdfRenderer::is_search_busy() {
    return pending_search_request.has_value() || search_is_busy;

}
bool PdfRenderer::is_busy() {
    for (int i = 0; i < num_threads; i++) {
        if (thread_busy_status[i]) {
            return true;
        }
    }
    return pending_render_requests.size() > 0;
}

void PdfRenderer::free_all_resources_for_document(std::wstring doc_path) {
    searching_mutex.lock();
    for (int i = 0; i < num_threads; i++) {
        thread_rendering_mutex[i].lock();
    }
    
    delete_old_pages(true, true); // todo: this is overkill, just delete the pixmaps for the document

    for (int i = 0; i < num_threads; i++) {
        auto index = std::make_pair(i, doc_path);
        if (opened_documents.find(index) != opened_documents.end()) {
            fz_document* doc_to_delete = opened_documents[index];
            fz_drop_document(thread_contexts[i], doc_to_delete);
            opened_documents.erase(index);
        }
    }

    for (int i = 0; i < num_threads; i++) {
        thread_rendering_mutex[i].unlock();
    }
    searching_mutex.unlock();
}

void PdfRenderer::debug() {
    cached_response_mutex.lock();
    for (auto resp : cached_responses) {
        std::wcout << resp.request.path << L" " << resp.request.page << " " << resp.request.slice_index << L" " << resp.request.zoom_level << "(" << resp.width << "*" << resp.height << ")" << std::endl;
    }
    std::wcout << "________________________________________\n";
    cached_response_mutex.unlock();
}

int PdfRenderer::get_pending_response_index_with_thread_index(const RenderRequest& req, int thread_index){
    // assumes we hold a lock on cached_response_mutex

    for (int i = 0; i < cached_responses.size(); i++) {
        if (cached_responses[i].pending && (cached_responses[i].request == req) && (cached_responses[i].thread == thread_index)) {
            return i;
        }
    }
    return -1;
}

void PdfRenderer::set_num_cached_pages(int n_cached_pages) {
    num_cached_pages = n_cached_pages;
}

float PdfRenderer::estimate_render_ms(const RenderRequest& req) {
    std::lock_guard<std::mutex> lock(render_cost_mutex);
    auto it = render_ms_per_megapixel.find(req.path);
    if (it == render_ms_per_megapixel.end()) {
        return -1;
    }
    // the page dimensions aren't known here, estimate the pixel count from a letter sized page
    float scale = req.zoom_level * req.display_scale;
    float megapixels = (612.0f * scale) * (792.0f * scale) / 1e6f;
    if (req.slice_index != -1) {
        megapixels /= req.num_h_slices * req.num_v_slices;
    }
    return it->second * megapixels;
}

int PdfRenderer::acquire_band_helpers(const RenderRequest& req) {
    // only pages that take long enough for the extra threads to pay off
    const float min_ms_for_bands = 25.0f;
    float estimate = estimate_render_ms(req);
    if (estimate < min_ms_for_bands) {
        return 0;
    }
    const int max_helpers = 3;
    std::lock_guard<std::mutex> lock(band_helpers_mutex);
    // workers that are rendering (including this one) and helpers of other renders occupy cores
    int available = band_cores - rendering_workers.load() - band_helpers_in_use;
    int take = std::max(0, std::min(available, max_helpers));
    band_helpers_in_use += take;
    return take;
}

void PdfRenderer::release_band_helpers(int helpers) {
    if (helpers > 0) {
        std::lock_guard<std::mutex> lock(band_helpers_mutex);
        band_helpers_in_use -= helpers;
    }
}

void PdfRenderer::update_render_cost_estimate(const std::wstring& path, fz_pixmap* pixmap, float elapsed_ms, int bands) {
    float megapixels = (float)pixmap->w * pixmap->h / 1e6f;
    if (megapixels <= 0) return;
    // what the render would have taken on one thread (a banded render doesn't scale perfectly, so
    // this overestimates a little, which is fine for deciding whether to use bands)
    float single_thread_ms = elapsed_ms * bands;
    float ms_per_megapixel = single_thread_ms / megapixels;

    std::lock_guard<std::mutex> lock(render_cost_mutex);
    auto it = render_ms_per_megapixel.find(path);
    if (it == render_ms_per_megapixel.end()) {
        render_ms_per_megapixel[path] = ms_per_megapixel;
    }
    else {
        // follow the document's recent pages
        it->second = 0.5f * it->second + 0.5f * ms_per_megapixel;
    }
}
