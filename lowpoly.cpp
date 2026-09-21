#include "lowpoly.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

using std::vector;

#ifdef LOWPOLY_NO_GPU
Gpu* gpu_create() { return nullptr; }
#endif
#if !defined(__APPLE__) || defined(LOWPOLY_NO_GPU)
bool platform_decode(const std::string&, Image&) { return false; }
bool platform_encode(const std::string&, const Image&) { return false; }
#endif

// ---- thread pool -------------------------------------------------------------

namespace {
struct Pool {
    vector<std::thread> threads;
    std::mutex m;
    std::condition_variable cv, done;
    const std::function<void(int, int)>* fn = nullptr;
    int n = 0, chunk = 0, pending = 0;
    uint64_t gen = 0;
    std::atomic<int> next{0};
    bool quit = false;

    void worker() {
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return gen != seen || quit; });
            if (quit) return;
            seen = gen;
            auto f = fn;
            lk.unlock();
            run_chunks(*f);
            lk.lock();
            if (--pending == 0) done.notify_one();
        }
    }
    void run_chunks(const std::function<void(int, int)>& f) {
        for (;;) {
            int b = next.fetch_add(chunk);
            if (b >= n) return;
            f(b, std::min(b + chunk, n));
        }
    }
    void run(int count, const std::function<void(int, int)>& f) {
        if (count <= 0) return;
        int nt = (int)threads.size() + 1;
        if (count < 2 || nt == 1) { f(0, count); return; }
        std::unique_lock<std::mutex> lk(m);
        n = count;
        chunk = std::max(1, count / (nt * 4));
        next = 0;
        pending = (int)threads.size();
        fn = &f;
        gen++;
        cv.notify_all();
        lk.unlock();
        run_chunks(f);
        lk.lock();
        done.wait(lk, [&] { return pending == 0; });
        fn = nullptr;
    }
    ~Pool() {
        { std::lock_guard<std::mutex> lk(m); quit = true; }
        cv.notify_all();
        for (auto& t : threads) t.join();
    }
};
Pool* g_pool = nullptr;
}

void pool_init(int threads) {
    if (threads <= 0) threads = (int)std::thread::hardware_concurrency();
    threads = std::max(1, threads);
    delete g_pool;
    g_pool = new Pool;
    for (int i = 1; i < threads; i++) g_pool->threads.emplace_back([] { g_pool->worker(); });
}
int pool_size() { return g_pool ? (int)g_pool->threads.size() + 1 : 1; }
void parallel_for(int n, const std::function<void(int, int)>& fn) {
    if (!g_pool) pool_init(0);
    g_pool->run(n, fn);
}

// ---- image io ------------------------------------------------------------------

static bool load_ppm(FILE* f, Image& img, std::string& err) {
    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || magic[0] != 'P' || (magic[1] != '6' && magic[1] != '5')) return false;
    int vals[3], got = 0;
    while (got < 3) {
        int c = fgetc(f);
        if (c == EOF) { err = "bad ppm header"; return false; }
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); continue; }
        if (isspace(c)) continue;
        ungetc(c, f);
        if (fscanf(f, "%d", &vals[got]) != 1) { err = "bad ppm header"; return false; }
        got++;
    }
    fgetc(f);
    int w = vals[0], h = vals[1], ch = magic[1] == '6' ? 3 : 1;
    if (w <= 0 || h <= 0 || vals[2] != 255) { err = "unsupported ppm"; return false; }
    vector<uint8_t> buf((size_t)w * h * ch);
    if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { err = "truncated ppm"; return false; }
    img = Image(w, h);
    for (size_t i = 0; i < img.size(); i++)
        img.px[i] = ch == 3 ? Rgb{buf[3 * i], buf[3 * i + 1], buf[3 * i + 2]} : Rgb{buf[i], buf[i], buf[i]};
    return true;
}

bool load_image(const std::string& path, Image& img, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    bool ok = load_ppm(f, img, err);
    fclose(f);
    if (ok) return true;
    if (!err.empty()) return false;
    if (platform_decode(path, img)) return true;
    int w, h, n;
    uint8_t* d = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!d) { err = "cannot decode " + path + ": " + stbi_failure_reason(); return false; }
    img = Image(w, h);
    memcpy(img.px.data(), d, img.size() * 3);
    stbi_image_free(d);
    return true;
}

static std::string ext_of(const std::string& p) {
    size_t d = p.rfind('.');
    std::string e = d == std::string::npos ? "" : p.substr(d + 1);
    for (auto& c : e) c = (char)tolower(c);
    return e;
}

bool save_image(const std::string& path, const Image& img, std::string& err) {
    std::string e = ext_of(path);
    if (e == "ppm") {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { err = "cannot write " + path; return false; }
        fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
        fwrite(img.px.data(), 3, img.size(), f);
        fclose(f);
        return true;
    }
    if (platform_encode(path, img)) return true;
    int ok;
    if (e == "jpg" || e == "jpeg") ok = stbi_write_jpg(path.c_str(), img.w, img.h, 3, img.px.data(), 90);
    else if (e == "bmp") ok = stbi_write_bmp(path.c_str(), img.w, img.h, 3, img.px.data());
    else if (e == "tga") ok = stbi_write_tga(path.c_str(), img.w, img.h, 3, img.px.data());
    else ok = stbi_write_png(path.c_str(), img.w, img.h, 3, img.px.data(), img.w * 3);
    if (!ok) err = "cannot write " + path;
    return ok != 0;
}

// ---- canny ---------------------------------------------------------------------

std::vector<uint8_t> to_luma(const Image& img) {
    vector<uint8_t> out(img.size());
    parallel_for(img.h, [&](int y0, int y1) {
        for (size_t i = (size_t)y0 * img.w, e = (size_t)y1 * img.w; i < e; i++) {
            Rgb p = img.px[i];
            out[i] = (uint8_t)((77 * p.r + 150 * p.g + 29 * p.b) >> 8);
        }
    });
    return out;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// uninitialised storage so first touch happens inside the parallel loops
template <class T> static std::unique_ptr<T[]> raw(size_t n) { return std::unique_ptr<T[]>(new T[n]); }

// blur -> sobel -> nms -> classify; mag2 is only kept for auto thresholds
static void gradient_classify(const vector<uint8_t>& luma, int w, int h, int low2, int high2, vector<uint8_t>& cls,
                              vector<int32_t>* mag2) {
    static const int K[5] = {1, 4, 6, 4, 1};
    size_t n = (size_t)w * h;
    auto tmp = raw<uint16_t>(n);
    auto blur = raw<uint8_t>(n);
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* row = &luma[(size_t)y * w];
            uint16_t* out = &tmp[(size_t)y * w];
            for (int x = 0; x < w; x++) {
                int acc = 0;
                for (int t = 0; t < 5; t++) acc += K[t] * row[clampi(x + t - 2, 0, w - 1)];
                out[x] = (uint16_t)acc;
            }
        }
    });
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* out = &blur[(size_t)y * w];
            const uint16_t* rows[5];
            for (int t = 0; t < 5; t++) rows[t] = &tmp[(size_t)clampi(y + t - 2, 0, h - 1) * w];
            for (int x = 0; x < w; x++) {
                int acc = 0;
                for (int t = 0; t < 5; t++) acc += K[t] * rows[t][x];
                out[x] = (uint8_t)(acc >> 8);
            }
        }
    });

    auto m = raw<int32_t>(n);
    auto dir = raw<uint8_t>(n);
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* r0 = &blur[(size_t)clampi(y - 1, 0, h - 1) * w];
            const uint8_t* r1 = &blur[(size_t)y * w];
            const uint8_t* r2 = &blur[(size_t)clampi(y + 1, 0, h - 1) * w];
            for (int x = 0; x < w; x++) {
                int xm = clampi(x - 1, 0, w - 1), xp = clampi(x + 1, 0, w - 1);
                int dx = (r0[xp] + 2 * r1[xp] + r2[xp]) - (r0[xm] + 2 * r1[xm] + r2[xm]);
                int dy = (r2[xm] + 2 * r2[x] + r2[xp]) - (r0[xm] + 2 * r0[x] + r0[xp]);
                size_t i = (size_t)y * w + x;
                m[i] = dx * dx + dy * dy;
                int adx = abs(dx), ady = abs(dy);
                // sector by tan(22.5) and tan(67.5), no atan2
                dir[i] = ady * 1000 <= adx * 414 ? 0 : ady * 1000 >= adx * 2414 ? 2 : (dx ^ dy) >= 0 ? 1 : 3;
            }
        }
    });

    cls.resize(n);
    if (mag2) mag2->assign(n, 0);
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                int32_t v = 0;
                if (x > 0 && y > 0 && x < w - 1 && y < h - 1) {
                    int32_t a, b;
                    switch (dir[i]) {
                        case 0: a = m[i - 1]; b = m[i + 1]; break;
                        case 1: a = m[i - w - 1]; b = m[i + w + 1]; break;
                        case 2: a = m[i - w]; b = m[i + w]; break;
                        default: a = m[i - w + 1]; b = m[i + w - 1]; break;
                    }
                    if (m[i] >= a && m[i] >= b) v = m[i];
                }
                if (mag2) (*mag2)[i] = v;
                cls[i] = v >= high2 ? 2 : v >= low2 ? 1 : 0;
            }
        }
    });
}

void canny_classify(const vector<uint8_t>& luma, int w, int h, int low2, int high2, bool auto_thr, vector<uint8_t>& cls) {
    if (!auto_thr) { gradient_classify(luma, w, h, low2, high2, cls, nullptr); return; }
    vector<int32_t> mag2;
    gradient_classify(luma, w, h, 0, 0, cls, &mag2);
    vector<int32_t> ridge;
    ridge.reserve(mag2.size() / 8);
    for (int32_t v : mag2) if (v > 0) ridge.push_back(v);
    if (ridge.empty()) high2 = 1;
    else {
        size_t k = (size_t)((ridge.size() - 1) * 0.85);
        std::nth_element(ridge.begin(), ridge.begin() + k, ridge.end());
        high2 = std::max<int32_t>(1, ridge[k]);
    }
    low2 = (int)(high2 * 0.16);
    parallel_for(h, [&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w, e = (size_t)y1 * w; i < e; i++)
            cls[i] = mag2[i] >= high2 ? 2 : mag2[i] >= low2 ? 1 : 0;
    });
}

void hysteresis(vector<uint8_t>& cls, int w, int h) {
    int bands = pool_size() * 4;
    vector<vector<uint32_t>> strong(bands);
    parallel_for(bands, [&](int b0, int b1) {
        for (int b = b0; b < b1; b++) {
            size_t i0 = cls.size() * b / bands, i1 = cls.size() * (b + 1) / bands;
            for (size_t i = i0; i < i1; i++) if (cls[i] == 2) { cls[i] = 255; strong[b].push_back((uint32_t)i); }
        }
    });
    vector<uint32_t> stack;
    for (auto& v : strong) stack.insert(stack.end(), v.begin(), v.end());
    while (!stack.empty()) {
        uint32_t i = stack.back(); stack.pop_back();
        int x = i % w, y = i / w;
        for (int ny = std::max(y - 1, 0); ny <= std::min(y + 1, h - 1); ny++)
            for (int nx = std::max(x - 1, 0); nx <= std::min(x + 1, w - 1); nx++) {
                size_t j = (size_t)ny * w + nx;
                if (cls[j] == 1) { cls[j] = 255; stack.push_back((uint32_t)j); }
            }
    }
    parallel_for(h, [&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w, e = (size_t)y1 * w; i < e; i++) if (cls[i] == 1) cls[i] = 0;
    });
}

// ---- sampling ------------------------------------------------------------------

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
    uint32_t below(uint32_t n) { return n ? (uint32_t)(((next() >> 32) * n) >> 32) : 0; }
};

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return x;
}

std::vector<Pt> sample_points(const vector<uint8_t>& mask, int w, int h, const Params& p) {
    int stride = w + 1;
    vector<uint32_t> sat((size_t)stride * (h + 1), 0);
    for (int y = 0; y < h; y++) {
        uint32_t acc = 0;
        for (int x = 0; x < w; x++) {
            acc += mask[(size_t)y * w + x] != 0;
            sat[(size_t)(y + 1) * stride + x + 1] = sat[(size_t)y * stride + x + 1] + acc;
        }
    }
    auto count = [&](int x0, int y0, int x1, int y1) -> uint32_t {
        return sat[(size_t)y1 * stride + x1] + sat[(size_t)y0 * stride + x0]
             - sat[(size_t)y0 * stride + x1] - sat[(size_t)y1 * stride + x0];
    };
    uint32_t total = count(0, 0, w, h);

    struct Cell { int x0, y0, cw, ch, quota; uint64_t key; };
    vector<Cell> cells;
    int density_budget = (int)(p.points * (1.0 - p.uniform));
    int uniform_budget = p.points - density_budget;
    if (total == 0) { uniform_budget = p.points; density_budget = 0; }

    if (density_budget > 0) {
        double per_level = (double)density_budget / p.levels;
        vector<vector<Cell>> per(p.levels);
        parallel_for(p.levels, [&](int l0, int l1) {
            for (int i = l0 + 1; i <= l1; i++) {
                double carry = 0;
                int cw = std::max(w / i, 1), ch = std::max(h / i, 1);
                for (int gy = 0; gy < i; gy++) {
                    int y0 = gy * ch, y1 = gy + 1 == i ? h : y0 + ch;
                    if (y0 >= h) break;
                    for (int gx = 0; gx < i; gx++) {
                        int x0 = gx * cw, x1 = gx + 1 == i ? w : x0 + cw;
                        if (x0 >= w) break;
                        uint32_t c = count(x0, y0, x1, y1);
                        if (!c) continue;
                        carry += per_level * c / total;
                        int q = (int)carry;
                        carry -= q;
                        q = std::min(q, 1024);
                        if (q) per[i - 1].push_back({x0, y0, x1 - x0, y1 - y0, q, ((uint64_t)i << 32) | ((uint64_t)gy << 16) | (uint64_t)gx});
                    }
                }
            }
        });
        for (auto& v : per) cells.insert(cells.end(), v.begin(), v.end());
    }

    // each cell draws from its own rng stream, so the pass is parallel and order-free
    vector<uint32_t> cell_off(cells.size() + 1, 0);
    for (size_t i = 0; i < cells.size(); i++) cell_off[i + 1] = cell_off[i] + cells[i].quota;
    vector<Pt> cand(cell_off.back());
    parallel_for((int)cells.size(), [&](int b, int e) {
        for (int i = b; i < e; i++) {
            const Cell& c = cells[i];
            Rng r(mix64(p.seed ^ mix64(c.key)));
            for (int k = 0; k < c.quota; k++)
                cand[cell_off[i] + k] = {c.x0 + (int)r.below(c.cw), c.y0 + (int)r.below(c.ch)};
        }
    });

    vector<uint8_t> taken((size_t)w * h, 0);
    vector<Pt> out;
    out.reserve(p.points + 2 * (w + h) / p.border + 8);
    auto push = [&](Pt q) {
        size_t i = (size_t)q.y * w + q.x;
        if (!taken[i]) { taken[i] = 1; out.push_back(q); }
    };
    for (Pt q : cand) push(q);

    Rng r(mix64(p.seed ^ 0xA5A5A5A5ull));
    for (int k = 0; k < uniform_budget; k++) push({(int)r.below(w), (int)r.below(h)});
    for (int tries = 0; (int)out.size() < p.points && tries < p.points * 8; tries++)
        push({(int)r.below(w), (int)r.below(h)});

    push({0, 0}); push({0, h - 1}); push({w - 1, 0}); push({w - 1, h - 1});
    for (int x = 0; x < w; x += p.border) { push({x, 0}); push({x, h - 1}); }
    for (int y = 0; y < h; y += p.border) { push({0, y}); push({w - 1, y}); }
    return out;
}

// ---- voronoi -------------------------------------------------------------------

static inline int32_t dist2(int x, int y, Pt s) { int dx = x - s.x, dy = y - s.y; return dx * dx + dy * dy; }

void voronoi_brute(int w, int h, const vector<Pt>& seeds, vector<uint32_t>& owner) {
    owner.resize((size_t)w * h);
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) for (int x = 0; x < w; x++) {
            uint32_t best = 0; int32_t bd = INT32_MAX;
            for (size_t i = 0; i < seeds.size(); i++) {
                int32_t d = dist2(x, y, seeds[i]);
                if (d < bd) { bd = d; best = (uint32_t)i; }
            }
            owner[(size_t)y * w + x] = best;
        }
    });
}

SeedGrid build_seed_grid(int w, int h, const vector<Pt>& seeds) {
    SeedGrid gr;
    double per = (double)w * h / std::max<size_t>(seeds.size(), 1);
    gr.g = std::clamp((int)std::lround(std::sqrt(2.0 * per)), 4, 64);
    gr.gw = (w + gr.g - 1) / gr.g;
    gr.gh = (h + gr.g - 1) / gr.g;
    gr.off.assign((size_t)gr.gw * gr.gh + 1, 0);
    for (Pt s : seeds) gr.off[(size_t)(s.y / gr.g) * gr.gw + s.x / gr.g + 1]++;
    for (size_t i = 1; i < gr.off.size(); i++) gr.off[i] += gr.off[i - 1];
    gr.idx.resize(seeds.size());
    vector<uint32_t> fill(gr.off.begin(), gr.off.end() - 1);
    for (size_t i = 0; i < seeds.size(); i++) {
        size_t b = (size_t)(seeds[i].y / gr.g) * gr.gw + seeds[i].x / gr.g;
        gr.idx[fill[b]++] = (uint32_t)i;
    }
    return gr;
}

// exact nearest seed: rings of bins outward, stop once no unvisited bin can be closer
void voronoi_grid(int w, int h, const vector<Pt>& seeds, const SeedGrid& gr, vector<uint32_t>& owner) {
    owner.resize((size_t)w * h);
    const int g = gr.g, gw = gr.gw, gh = gr.gh, rmax = std::max(gw, gh);
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            int cy = y / g;
            for (int x = 0; x < w; x++) {
                int cx = x / g;
                uint32_t best = UINT32_MAX; int32_t bd = INT32_MAX;
                auto scan = [&](int bx, int by) {
                    if (bx < 0 || by < 0 || bx >= gw || by >= gh) return;
                    size_t b = (size_t)by * gw + bx;
                    for (uint32_t k = gr.off[b]; k < gr.off[b + 1]; k++) {
                        uint32_t i = gr.idx[k];
                        int32_t d = dist2(x, y, seeds[i]);
                        if (d < bd || (d == bd && i < best)) { bd = d; best = i; }
                    }
                };
                scan(cx, cy);
                for (int r = 1; r <= rmax; r++) {
                    int64_t lim = (int64_t)(r - 1) * g + 1;
                    if (best != UINT32_MAX && bd < lim * lim) break;
                    for (int bx = cx - r; bx <= cx + r; bx++) { scan(bx, cy - r); scan(bx, cy + r); }
                    for (int by = cy - r + 1; by <= cy + r - 1; by++) { scan(cx - r, by); scan(cx + r, by); }
                }
                owner[(size_t)y * w + x] = best;
            }
        }
    });
}

void voronoi_jfa(int w, int h, const vector<Pt>& seeds, vector<uint32_t>& owner) {
    const uint32_t EMPTY = UINT32_MAX;
    size_t n = (size_t)w * h;
    vector<uint32_t> seed_at(n, EMPTY), cur(n, EMPTY), nxt(n);
    for (size_t i = 0; i < seeds.size(); i++) {
        size_t k = (size_t)seeds[i].y * w + seeds[i].x;
        if (seed_at[k] == EMPTY) { seed_at[k] = (uint32_t)i; cur[k] = ((uint32_t)seeds[i].x << 16) | (uint32_t)seeds[i].y; }
    }
    auto d2 = [](int x, int y, uint32_t p) { int dx = x - (int)(p >> 16), dy = y - (int)(p & 0xFFFF); return dx * dx + dy * dy; };
    vector<int> steps;
    for (int k = 1; k < std::max(w, h); k *= 2) steps.push_back(k);
    std::reverse(steps.begin(), steps.end());
    steps.push_back(2); steps.push_back(1);
    for (int k : steps) {
        parallel_for(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                uint32_t best = cur[i];
                int bd = best == EMPTY ? INT32_MAX : d2(x, y, best);
                for (int dy = -k; dy <= k; dy += k) {
                    int ny = y + dy;
                    if (ny < 0 || ny >= h) continue;
                    for (int dx = -k; dx <= k; dx += k) {
                        int nx = x + dx;
                        if (nx < 0 || nx >= w) continue;
                        uint32_t c = cur[(size_t)ny * w + nx];
                        if (c == EMPTY) continue;
                        int d = d2(x, y, c);
                        if (d < bd) { bd = d; best = c; }
                    }
                }
                nxt[i] = best;
            }
        });
        std::swap(cur, nxt);
    }
    owner.resize(n);
    parallel_for(h, [&](int y0, int y1) {
        for (size_t i = (size_t)y0 * w, e = (size_t)y1 * w; i < e; i++) {
            uint32_t p = cur[i];
            owner[i] = p == EMPTY ? 0 : seed_at[(size_t)(p & 0xFFFF) * w + (p >> 16)];
        }
    });
}

// ---- hull ------------------------------------------------------------------------

int64_t cross(Pt o, Pt a, Pt b) {
    return (int64_t)(a.x - o.x) * (b.y - o.y) - (int64_t)(a.y - o.y) * (b.x - o.x);
}

int64_t signed_area2(const Pt* v, int n) {
    int64_t a = 0;
    for (int i = 0; i < n; i++) { Pt p = v[i], q = v[(i + 1) % n]; a += (int64_t)p.x * q.y - (int64_t)q.x * p.y; }
    return a;
}

std::vector<Pt> hull_monotone(vector<Pt> pts) {
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    size_t n = pts.size();
    if (n < 3) return pts;
    vector<Pt> h(2 * n);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) k--;
        h[k++] = pts[i];
    }
    for (size_t i = n - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 0) k--;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    return h;
}

// hull of at most four points, no allocation; returns count
static int hull4(const Pt* in, int n, Pt* out) {
    Pt p[4];
    std::copy(in, in + n, p);
    std::sort(p, p + n);
    n = (int)(std::unique(p, p + n) - p);
    if (n < 3) { std::copy(p, p + n, out); return n; }
    Pt h[8];
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) k--;
        h[k++] = p[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && cross(h[k - 2], h[k - 1], p[i]) <= 0) k--;
        h[k++] = p[i];
    }
    std::copy(h, h + k - 1, out);
    return k - 1;
}

static void hull_side(const vector<Pt>& pts, Pt a, Pt b, vector<Pt>& out) {
    if (pts.empty()) { out.push_back(b); return; }
    Pt far = pts[0]; int64_t fd = std::abs(cross(a, b, pts[0]));
    for (Pt q : pts) { int64_t d = std::abs(cross(a, b, q)); if (d > fd) { fd = d; far = q; } }
    vector<Pt> l, r;
    for (Pt q : pts) { if (cross(a, far, q) > 0) l.push_back(q); else if (cross(far, b, q) > 0) r.push_back(q); }
    if (l.size() + r.size() > 4096) {
        vector<Pt> lo, ro;
        std::thread th([&] { hull_side(l, a, far, lo); });
        hull_side(r, far, b, ro);
        th.join();
        out.insert(out.end(), lo.begin(), lo.end());
        out.insert(out.end(), ro.begin(), ro.end());
    } else {
        hull_side(l, a, far, out);
        hull_side(r, far, b, out);
    }
}

std::vector<Pt> hull_quick(const vector<Pt>& in) {
    vector<Pt> pts = in;
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return pts;
    Pt l = pts.front(), r = pts.back();
    vector<Pt> above, below;
    for (Pt q : pts) { int64_t c = cross(l, r, q); if (c > 0) above.push_back(q); else if (c < 0) below.push_back(q); }
    vector<Pt> a{l}, b;
    std::thread th([&] { hull_side(above, l, r, a); });
    hull_side(below, r, l, b);
    th.join();
    b.pop_back();
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// ---- polygons ----------------------------------------------------------------------

struct Key { uint32_t k[4]; };
static inline bool operator==(const Key& a, const Key& b) { return memcmp(a.k, b.k, 16) == 0; }
static inline bool operator<(const Key& a, const Key& b) { return memcmp(a.k, b.k, 16) < 0; }
static inline uint64_t key_hash(const Key& a) {
    uint64_t h = 0;
    for (int i = 0; i < 4; i++) { h = (h ^ a.k[i]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    return h;
}

// keys of every 2x2 window where three or four cells meet, per band
static vector<vector<Key>> junction_keys(const vector<uint32_t>& owner, int w, int h) {
    int bands = std::min(h - 1, pool_size() * 4);
    vector<vector<Key>> local(bands);
    parallel_for(bands, [&](int b0, int b1) {
        for (int b = b0; b < b1; b++) {
            int y0 = (int)((int64_t)(h - 1) * b / bands), y1 = (int)((int64_t)(h - 1) * (b + 1) / bands);
            vector<Key>& out = local[b];
            for (int y = y0; y < y1; y++) {
                const uint32_t* r0 = &owner[(size_t)y * w];
                const uint32_t* r1 = r0 + w;
                for (int x = 0; x + 1 < w; x++) {
                    uint32_t o[4] = {r0[x], r0[x + 1], r1[x], r1[x + 1]};
                    if (o[0] == o[1] && o[0] == o[2] && o[0] == o[3]) continue;
                    Key k{{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}};
                    int n = 0;
                    for (uint32_t v : o) {
                        bool dup = false;
                        for (int i = 0; i < n; i++) dup |= k.k[i] == v;
                        if (!dup) k.k[n++] = v;
                    }
                    if (n < 3) continue;
                    std::sort(k.k, k.k + n);
                    out.push_back(k);
                }
            }
        }
    });
    return local;
}

static vector<Polygon> keys_to_polygons(const vector<Key>& keys, const vector<Pt>& seeds) {
    vector<Polygon> polys(keys.size());
    vector<uint8_t> keep(keys.size(), 0);
    parallel_for((int)keys.size(), [&](int b, int e) {
        for (int i = b; i < e; i++) {
            const Key& k = keys[i];
            int n = k.k[3] == UINT32_MAX ? 3 : 4;
            Pt pts[4], o[4];
            for (int j = 0; j < n; j++) pts[j] = seeds[k.k[j]];
            int m = hull4(pts, n, o);
            if (m < 3) continue;
            Polygon& q = polys[i];
            q.n = (uint8_t)m;
            for (int j = 0; j < m; j++) {
                q.v[j] = o[j];
                for (int t = 0; t < n; t++) if (pts[t] == o[j]) q.s[j] = k.k[t];
            }
            q.color = {0, 0, 0};
            keep[i] = 1;
        }
    });
    vector<Polygon> out;
    out.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); i++) if (keep[i]) out.push_back(polys[i]);
    return out;
}

std::vector<Polygon> extract_polygons(const vector<uint32_t>& owner, int w, int h, const vector<Pt>& seeds) {
    if (w < 2 || h < 2) return {};
    auto local = junction_keys(owner, w, h);
    size_t total = 0;
    for (auto& v : local) total += v.size();
    size_t cap = 16;
    while (cap < total * 2) cap *= 2;
    vector<Key> table(cap);
    vector<uint8_t> used(cap, 0);
    vector<Key> keys;
    keys.reserve(total / 4);
    for (auto& v : local) for (const Key& k : v) {
        size_t i = key_hash(k) & (cap - 1);
        while (used[i] && !(table[i] == k)) i = (i + 1) & (cap - 1);
        if (!used[i]) { used[i] = 1; table[i] = k; keys.push_back(k); }
    }
    return keys_to_polygons(keys, seeds);
}

void build_csr(const vector<Polygon>& polys, size_t nseeds, vector<uint32_t>& off, vector<uint32_t>& idx) {
    off.assign(nseeds + 1, 0);
    for (auto& q : polys) for (int i = 0; i < q.n; i++) off[q.s[i] + 1]++;
    for (size_t i = 1; i < off.size(); i++) off[i] += off[i - 1];
    idx.resize(off.back());
    vector<uint32_t> fill(off.begin(), off.end() - 1);
    for (size_t p = 0; p < polys.size(); p++) for (int i = 0; i < polys[p].n; i++) idx[fill[polys[p].s[i]]++] = (uint32_t)p;
}

void draw_line(Image& img, int x0, int y0, int x1, int y1, Rgb c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < img.w && y0 < img.h) img.at(x0, y0) = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void draw_polyline(Image& img, const vector<Pt>& pts, Rgb c, bool closed) {
    size_t n = pts.size();
    if (n < 2) return;
    for (size_t i = 0; i + 1 < n + closed; i++)
        draw_line(img, pts[i].x, pts[i].y, pts[(i + 1) % n].x, pts[(i + 1) % n].y, c);
}

// ---- raster ----------------------------------------------------------------------

struct Edges { int32_t a[4], b[4], c[4]; int n; int x0, y0, x1, y1; };

// edge functions are >= 0 inside for ccw vertices; terms stay under 2^30 for coords < 16384
static Edges edges_of(const Polygon& q, int w, int h) {
    Edges e; e.n = q.n;
    e.x0 = e.y0 = INT32_MAX; e.x1 = e.y1 = INT32_MIN;
    for (int i = 0; i < q.n; i++) {
        Pt u = q.v[i], v = q.v[(i + 1) % q.n];
        e.a[i] = -(v.y - u.y); e.b[i] = v.x - u.x; e.c[i] = -(e.a[i] * u.x + e.b[i] * u.y);
        e.x0 = std::min(e.x0, u.x); e.y0 = std::min(e.y0, u.y); e.x1 = std::max(e.x1, u.x); e.y1 = std::max(e.y1, u.y);
    }
    e.x0 = std::max(e.x0, 0); e.y0 = std::max(e.y0, 0); e.x1 = std::min(e.x1 + 1, w); e.y1 = std::min(e.y1 + 1, h);
    return e;
}

template <class F> static inline void walk(const Edges& e, int w, int ylo, int yhi, F&& f) {
    int y0 = std::max(e.y0, ylo), y1 = std::min(e.y1, yhi);
    for (int y = y0; y < y1; y++) {
        int32_t acc[4];
        for (int i = 0; i < e.n; i++) acc[i] = e.a[i] * e.x0 + e.b[i] * y + e.c[i];
        size_t row = (size_t)y * w;
        for (int x = e.x0; x < e.x1; x++) {
            bool in = true;
            for (int i = 0; i < e.n; i++) in &= acc[i] >= 0;
            if (in) f(row + x);
            for (int i = 0; i < e.n; i++) acc[i] += e.a[i];
        }
    }
}

static inline bool inside(const Polygon& q, int x, int y) {
    for (int i = 0; i < q.n; i++) {
        Pt u = q.v[i], v = q.v[(i + 1) % q.n];
        if ((int64_t)(v.x - u.x) * (y - u.y) - (int64_t)(v.y - u.y) * (x - u.x) < 0) return false;
    }
    return true;
}

void color_polygons(const Image& src, vector<Polygon>& polys, ColorMode mode) {
    parallel_for((int)polys.size(), [&](int b, int e) {
        for (int i = b; i < e; i++) {
            Polygon& q = polys[i];
            if (mode == ColorMode::Corner) { q.color = src.at(q.v[0].x, q.v[0].y); continue; }
            uint64_t r = 0, g = 0, bl = 0, n = 0;
            walk(edges_of(q, src.w, src.h), src.w, 0, src.h, [&](size_t k) { Rgb p = src.px[k]; r += p.r; g += p.g; bl += p.b; n++; });
            q.color = n ? Rgb{uint8_t(r / n), uint8_t(g / n), uint8_t(bl / n)} : src.at(q.v[0].x, q.v[0].y);
        }
    });
}

void rasterize(Image& dst, const vector<Polygon>& polys) {
    int w = dst.w, h = dst.h;
    if (polys.empty() || h == 0) return;
    int band_h = std::max(16, (h + pool_size() * 4 - 1) / (pool_size() * 4));
    int bands = (h + band_h - 1) / band_h;
    vector<vector<uint32_t>> bucket(bands);
    for (size_t i = 0; i < polys.size(); i++) {
        int y0 = INT32_MAX, y1 = INT32_MIN;
        for (int k = 0; k < polys[i].n; k++) { y0 = std::min(y0, polys[i].v[k].y); y1 = std::max(y1, polys[i].v[k].y); }
        y0 = std::max(y0, 0); y1 = std::min(y1, h - 1);
        for (int b = y0 / band_h; b <= y1 / band_h && b < bands; b++) bucket[b].push_back((uint32_t)i);
    }
    parallel_for(bands, [&](int b0, int b1) {
        for (int b = b0; b < b1; b++) {
            int ylo = b * band_h, yhi = std::min(ylo + band_h, h);
            for (uint32_t i : bucket[b]) {
                const Polygon& q = polys[i];
                walk(edges_of(q, w, h), w, ylo, yhi, [&](size_t k) { dst.px[k] = q.color; });
            }
        }
    });
}

void rasterize_by_owner(const Image& src, const vector<uint32_t>& owner, const vector<uint32_t>& off,
                        const vector<uint32_t>& idx, vector<Polygon>& polys, Rgb background, Image& dst) {
    int w = src.w, h = src.h;
    vector<uint32_t> pid(src.size());
    vector<std::atomic<uint64_t>> sum(polys.size() * 4);
    for (auto& s : sum) s.store(0, std::memory_order_relaxed);
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) for (int x = 0; x < w; x++) {
            size_t k = (size_t)y * w + x;
            uint32_t s = owner[k], found = UINT32_MAX;
            for (uint32_t j = off[s]; j < off[s + 1]; j++) if (inside(polys[idx[j]], x, y)) { found = idx[j]; break; }
            pid[k] = found;
            if (found == UINT32_MAX) continue;
            Rgb p = src.px[k];
            sum[found * 4 + 0].fetch_add(p.r, std::memory_order_relaxed);
            sum[found * 4 + 1].fetch_add(p.g, std::memory_order_relaxed);
            sum[found * 4 + 2].fetch_add(p.b, std::memory_order_relaxed);
            sum[found * 4 + 3].fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (size_t i = 0; i < polys.size(); i++) {
        uint64_t n = sum[i * 4 + 3];
        polys[i].color = n ? Rgb{uint8_t(sum[i * 4] / n), uint8_t(sum[i * 4 + 1] / n), uint8_t(sum[i * 4 + 2] / n)}
                           : src.at(polys[i].v[0].x, polys[i].v[0].y);
    }
    dst = Image(w, h, background);
    parallel_for(h, [&](int y0, int y1) {
        for (size_t k = (size_t)y0 * w, e = (size_t)y1 * w; k < e; k++) if (pid[k] != UINT32_MAX) dst.px[k] = polys[pid[k]].color;
    });
}

static Rgb image_mean(const Image& img) {
    uint64_t r = 0, g = 0, b = 0;
    for (Rgb p : img.px) { r += p.r; g += p.g; b += p.b; }
    uint64_t n = std::max<uint64_t>(img.size(), 1);
    return {uint8_t(r / n), uint8_t(g / n), uint8_t(b / n)};
}

// ---- cli ---------------------------------------------------------------------------

static const char* USAGE =
"lowpoly <input> [options]\n"
"  -o FILE            output image (default <input>-lowpoly.png; extension picks the format)\n"
"  --points N         interior sample points (10000)\n"
"  --levels N         grid levels for density weighting (150)\n"
"  --border N         border seed every N px (16)\n"
"  --uniform F        fraction of points spread uniformly (0.08)\n"
"  --seed N           rng seed (24301)\n"
"  --canny-low F      canny low threshold (5)\n"
"  --canny-high F     canny high threshold (25)\n"
"  --auto-canny       thresholds from the gradient distribution\n"
"  --color MODE       mean | corner (mean)\n"
"  --voronoi MODE     grid | jfa | brute (grid)\n"
"  --canny MODE       int | float (int)\n"
"  --extract MODE     hash | sort (hash)\n"
"  --raster MODE      bbox | owner (bbox)\n"
"  --backend MODE     auto | cpu | gpu (auto)\n"
"  --threads N        worker threads (all cores)\n"
"  --stages DIR       where stage images go (out)\n"
"  --no-stages        skip stage images\n"
"  --hull             overlay the seed hull on the output\n"
"  --text FILE        also write the Tri/Qua shape list\n"
"  --bench            time each stage against its alternatives\n"
"  -q                 quiet\n";

static bool parse_args(int argc, char** argv, Params& p, std::string& err) {
    auto need = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) { err = std::string(flag) + " needs a value"; return nullptr; }
        return argv[++i];
    };
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char* v;
        if (a == "-h" || a == "--help") { fputs(USAGE, stdout); exit(0); }
        else if (a == "-o") { if (!(v = need(i, "-o"))) return false; p.out = v; }
        else if (a == "--points") { if (!(v = need(i, "--points"))) return false; p.points = atoi(v); }
        else if (a == "--levels") { if (!(v = need(i, "--levels"))) return false; p.levels = std::max(1, atoi(v)); }
        else if (a == "--border") { if (!(v = need(i, "--border"))) return false; p.border = std::max(1, atoi(v)); }
        else if (a == "--uniform") { if (!(v = need(i, "--uniform"))) return false; p.uniform = std::clamp(atof(v), 0.0, 1.0); }
        else if (a == "--seed") { if (!(v = need(i, "--seed"))) return false; p.seed = strtoull(v, nullptr, 10); }
        else if (a == "--canny-low") { if (!(v = need(i, "--canny-low"))) return false; p.canny_low = (float)atof(v); }
        else if (a == "--canny-high") { if (!(v = need(i, "--canny-high"))) return false; p.canny_high = (float)atof(v); }
        else if (a == "--auto-canny") p.auto_canny = true;
        else if (a == "--threads") { if (!(v = need(i, "--threads"))) return false; p.threads = atoi(v); }
        else if (a == "--stages") { if (!(v = need(i, "--stages"))) return false; p.stages = v; }
        else if (a == "--no-stages") p.no_stages = true;
        else if (a == "--hull") p.hull = true;
        else if (a == "--text") { if (!(v = need(i, "--text"))) return false; p.text = v; }
        else if (a == "--bench") p.bench = true;
        else if (a == "-q") p.quiet = true;
        else if (a == "--color") {
            if (!(v = need(i, "--color"))) return false;
            if (!strcmp(v, "mean")) p.color = ColorMode::Mean;
            else if (!strcmp(v, "corner")) p.color = ColorMode::Corner;
            else { err = "unknown --color"; return false; }
        } else if (a == "--voronoi") {
            if (!(v = need(i, "--voronoi"))) return false;
            if (!strcmp(v, "grid")) p.voronoi = VoronoiMode::Grid;
            else if (!strcmp(v, "jfa")) p.voronoi = VoronoiMode::Jfa;
            else if (!strcmp(v, "brute")) p.voronoi = VoronoiMode::Brute;
            else { err = "unknown --voronoi"; return false; }
        } else if (a == "--canny") {
            if (!(v = need(i, "--canny"))) return false;
            if (!strcmp(v, "int")) p.canny_float = false;
            else if (!strcmp(v, "float")) p.canny_float = true;
            else { err = "unknown --canny"; return false; }
        } else if (a == "--extract") {
            if (!(v = need(i, "--extract"))) return false;
            if (!strcmp(v, "hash")) p.extract_sort = false;
            else if (!strcmp(v, "sort")) p.extract_sort = true;
            else { err = "unknown --extract"; return false; }
        } else if (a == "--raster") {
            if (!(v = need(i, "--raster"))) return false;
            if (!strcmp(v, "bbox")) p.raster_owner = false;
            else if (!strcmp(v, "owner")) p.raster_owner = true;
            else { err = "unknown --raster"; return false; }
        } else if (a == "--backend") {
            if (!(v = need(i, "--backend"))) return false;
            if (!strcmp(v, "auto")) p.backend = Backend::Auto;
            else if (!strcmp(v, "cpu")) p.backend = Backend::Cpu;
            else if (!strcmp(v, "gpu")) p.backend = Backend::Gpu;
            else { err = "unknown --backend"; return false; }
        } else if (a[0] == '-') { err = "unknown option " + a; return false; }
        else if (p.in.empty()) p.in = a;
        else { err = "unexpected argument " + a; return false; }
    }
    if (p.in.empty()) { err = "no input file"; return false; }
    if (p.out.empty()) {
        std::string stem = p.in;
        size_t s = stem.find_last_of("/\\");
        if (s != std::string::npos) stem = stem.substr(s + 1);
        size_t d = stem.rfind('.');
        if (d != std::string::npos) stem = stem.substr(0, d);
        p.out = stem + "-lowpoly.png";
    }
    return true;
}

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct Timer {
    bool quiet;
    double total = 0;
    template <class F> auto run(const char* name, F&& f) {
        auto t = Clock::now();
        if constexpr (std::is_void_v<decltype(f())>) {
            f();
            report(name, ms_since(t));
        } else {
            auto r = f();
            report(name, ms_since(t));
            return r;
        }
    }
    void report(const char* name, double ms) {
        total += ms;
        if (!quiet) printf("  %-12s %8.2f ms\n", name, ms);
    }
};

static bool write_text(const std::string& path, const Image& img, const vector<Polygon>& polys) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n%zu\n", img.w, img.h, polys.size());
    for (auto& q : polys) {
        fputs(q.n == 3 ? "Tri\n" : "Qua\n", f);
        for (int i = 0; i < q.n; i++) fprintf(f, "%d %d\n", q.v[i].x, q.v[i].y);
        fprintf(f, "%d %d %d\n", q.color.r, q.color.g, q.color.b);
    }
    fclose(f);
    return true;
}

static const Rgb TRI = {0, 229, 255}, QUAD = {255, 138, 0};

// gpu wins from about here; measured on an m5 pro against its own cpu path
static const size_t GPU_MIN_PIXELS = 4000000;

// ---- bench ---------------------------------------------------------------------------

// the rust port's float canny, for the table
static void canny_float(const Image& img, int w, int h, float low, float high, vector<uint8_t>& cls) {
    static const float K[5] = {1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f};
    vector<float> luma(img.size()), tmp(img.size()), blur(img.size()), mag(img.size()), sup(img.size(), 0);
    vector<uint8_t> dir(img.size());
    for (size_t i = 0; i < img.size(); i++) luma[i] = 0.299f * img.px[i].r + 0.587f * img.px[i].g + 0.114f * img.px[i].b;
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) for (int x = 0; x < w; x++) {
            float acc = 0;
            for (int t = 0; t < 5; t++) acc += K[t] * luma[(size_t)y * w + clampi(x + t - 2, 0, w - 1)];
            tmp[(size_t)y * w + x] = acc;
        }
    });
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) for (int x = 0; x < w; x++) {
            float acc = 0;
            for (int t = 0; t < 5; t++) acc += K[t] * tmp[(size_t)clampi(y + t - 2, 0, h - 1) * w + x];
            blur[(size_t)y * w + x] = acc;
        }
    });
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) for (int x = 0; x < w; x++) {
            int xm = clampi(x - 1, 0, w - 1), xp = clampi(x + 1, 0, w - 1), ym = clampi(y - 1, 0, h - 1), yp = clampi(y + 1, 0, h - 1);
            const float *r0 = &blur[(size_t)ym * w], *r1 = &blur[(size_t)y * w], *r2 = &blur[(size_t)yp * w];
            float dx = (r0[xp] + 2 * r1[xp] + r2[xp]) - (r0[xm] + 2 * r1[xm] + r2[xm]);
            float dy = (r2[xm] + 2 * r2[x] + r2[xp]) - (r0[xm] + 2 * r0[x] + r0[xp]);
            size_t i = (size_t)y * w + x;
            mag[i] = std::sqrt(dx * dx + dy * dy);
            float adx = std::fabs(dx), ady = std::fabs(dy);
            dir[i] = ady <= adx * 0.41421357f ? 0 : ady >= adx * 2.4142136f ? 2 : dx * dy > 0 ? 1 : 3;
        }
    });
    parallel_for(h, [&](int y0, int y1) {
        for (int y = std::max(y0, 1); y < std::min(y1, h - 1); y++) for (int x = 1; x < w - 1; x++) {
            size_t i = (size_t)y * w + x;
            float m = mag[i], a, b;
            switch (dir[i]) {
                case 0: a = mag[i - 1]; b = mag[i + 1]; break;
                case 1: a = mag[i - w - 1]; b = mag[i + w + 1]; break;
                case 2: a = mag[i - w]; b = mag[i + w]; break;
                default: a = mag[i - w + 1]; b = mag[i + w - 1]; break;
            }
            if (m >= a && m >= b) sup[i] = m;
        }
    });
    cls.resize(img.size());
    for (size_t i = 0; i < img.size(); i++) cls[i] = sup[i] >= high ? 2 : sup[i] >= low ? 1 : 0;
    hysteresis(cls, w, h);
}

static vector<Polygon> extract_sort_unique(const vector<uint32_t>& owner, int w, int h, const vector<Pt>& seeds) {
    auto local = junction_keys(owner, w, h);
    vector<Key> all;
    for (auto& v : local) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    return keys_to_polygons(all, seeds);
}

// the 2023 driver: every pixel of the image against one shape, barycentric fan test
static void naive_fill_one(Image& img, const Polygon& q) {
    for (int y = 0; y < img.h; y++) for (int x = 0; x < img.w; x++) {
        bool in = false;
        for (int i = 1; i + 1 < q.n && !in; i++) {
            Pt a = q.v[0], b = q.v[i], c = q.v[i + 1];
            float det = (float)(b.x - a.x) * (c.y - a.y) - (float)(c.x - a.x) * (b.y - a.y);
            if (det == 0) continue;
            float al = ((b.y - c.y) * (float)(x - c.x) + (c.x - b.x) * (float)(y - c.y)) / det;
            float be = ((c.y - a.y) * (float)(x - c.x) + (a.x - c.x) * (float)(y - c.y)) / det;
            in = al >= 0 && be >= 0 && 1 - al - be >= 0;
        }
        if (in) img.at(x, y) = q.color;
    }
}

template <class F> static double best_of(int reps, F&& f) {
    double best = 1e30;
    for (int i = 0; i < reps; i++) { auto t = Clock::now(); f(); best = std::min(best, ms_since(t)); }
    return best;
}

static void run_bench(const Params& p, const Image& img, const vector<Pt>& seeds, const vector<Polygon>& polys, Gpu* gpu) {
    int w = img.w, h = img.h;
    printf("\n=== bench: %s %dx%d, %zu seeds, %zu polygons (best of 3) ===\n", p.in.c_str(), w, h, seeds.size(), polys.size());
    auto row = [](const char* stage, const char* what, double ms, const char* note = "") {
        printf("%-9s %-38s %9.2f ms  %s\n", stage, what, ms, note);
    };
    char note[128];
    auto luma = to_luma(img);
    vector<uint8_t> cls;
    int low2 = (int)(p.canny_low * p.canny_low), high2 = (int)(p.canny_high * p.canny_high);
    double cf = best_of(3, [&] { canny_float(img, w, h, p.canny_low, p.canny_high, cls); });
    double ci = best_of(3, [&] { canny_classify(luma, w, h, low2, high2, false, cls); hysteresis(cls, w, h); });
    row("canny", "float, sqrt magnitude", cf);
    snprintf(note, sizeof note, "%.1fx", cf / ci);
    row("canny", "integer, squared magnitude", ci, note);
    if (gpu) {
        gpu->upload(img);
        double cg = best_of(3, [&] { gpu->canny_front(low2, high2, cls); hysteresis(cls, w, h); });
        snprintf(note, sizeof note, "%.1fx vs integer cpu", ci / cg);
        row("canny", "gpu front half + cpu hysteresis", cg, note);
    }

    vector<uint32_t> owner;
    SeedGrid grid = build_seed_grid(w, h, seeds);
    double vb = seeds.size() * img.size() > 4e10 ? 0 : best_of(1, [&] { voronoi_brute(w, h, seeds, owner); });
    double vj = best_of(3, [&] { voronoi_jfa(w, h, seeds, owner); });
    double vg = best_of(3, [&] { grid = build_seed_grid(w, h, seeds); voronoi_grid(w, h, seeds, grid, owner); });
    if (vb > 0) row("voronoi", "brute force (original)", vb);
    else row("voronoi", "brute force (original)", 0, "skipped, too large");
    snprintf(note, sizeof note, "%.1fx vs brute", vb / vj);
    row("voronoi", "jump flooding +2 (rust)", vj, vb > 0 ? note : "");
    snprintf(note, sizeof note, "%.1fx vs jfa%s", vj / vg, "");
    row("voronoi", "grid ring search, exact", vg, note);
    if (gpu) {
        double vgg = best_of(3, [&] { gpu->voronoi(seeds, grid, owner); });
        snprintf(note, sizeof note, "%.1fx vs cpu grid", vg / vgg);
        row("voronoi", "grid ring search, gpu", vgg, note);
        voronoi_grid(w, h, seeds, grid, owner);
    }

    vector<Polygon> ps;
    double es = best_of(3, [&] { ps = extract_sort_unique(owner, w, h, seeds); });
    double eh = best_of(3, [&] { ps = extract_polygons(owner, w, h, seeds); });
    row("extract", "sort + unique (original)", es);
    snprintf(note, sizeof note, "%.1fx", es / eh);
    row("extract", "flat hash dedup", eh, note);

    double hm = best_of(3, [&] { hull_monotone(seeds); });
    double hq = best_of(3, [&] { hull_quick(seeds); });
    row("hull", "monotone chain", hm);
    snprintf(note, sizeof note, "%.1fx", hm / hq);
    row("hull", "quickhull", hq, note);

    Rgb bg = image_mean(img);
    size_t sample = std::min<size_t>(64, polys.size());
    double rn = best_of(1, [&] { Image o(w, h); for (size_t i = 0; i < sample; i++) naive_fill_one(o, polys[i]); });
    rn = rn / sample * polys.size();
    double rb = best_of(3, [&] { auto q = polys; color_polygons(img, q, ColorMode::Mean); Image o(w, h, bg); rasterize(o, q); });
    vector<uint32_t> off, idx;
    build_csr(polys, seeds.size(), off, idx);
    double ro = best_of(3, [&] { auto q = polys; Image o; rasterize_by_owner(img, owner, off, idx, q, bg, o); });
    snprintf(note, sizeof note, "extrapolated from %zu shapes", sample);
    row("raster", "full-image scan per shape (original)", rn, note);
    snprintf(note, sizeof note, "%.0fx", rn / rb);
    row("raster", "bbox edge functions + colour, banded", rb, note);
    snprintf(note, sizeof note, "%.1fx vs bbox", rb / ro);
    row("raster", "owner lookup + colour", ro, note);
    if (gpu) {
        double rg = best_of(3, [&] { auto q = polys; Image o; gpu->raster(q, off, idx, bg, o); });
        snprintf(note, sizeof note, "%.1fx vs bbox cpu", rb / rg);
        row("raster", "owner lookup + colour, gpu", rg, note);
    }
}

// ---- main ------------------------------------------------------------------------------

#ifndef LOWPOLY_TESTS
int main(int argc, char** argv) {
    Params p;
    std::string err;
    if (!parse_args(argc, argv, p, err)) { fprintf(stderr, "error: %s\n%s", err.c_str(), USAGE); return 1; }
    stbi_write_png_compression_level = 2;
    pool_init(p.threads);

    Image img;
    auto t0 = Clock::now();
    if (!load_image(p.in, img, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    double decode_ms = ms_since(t0);
    if (img.w > MAX_DIM || img.h > MAX_DIM) { fprintf(stderr, "error: image larger than %d px per side\n", MAX_DIM); return 1; }
    if (img.w < 2 || img.h < 2) { fprintf(stderr, "error: image too small\n"); return 1; }
    if (!p.quiet) printf("%s  %dx%d  %d threads\n", p.in.c_str(), img.w, img.h, pool_size());

    Gpu* gpu = nullptr;
    bool want = p.backend == Backend::Gpu || (p.backend == Backend::Auto && (img.size() >= GPU_MIN_PIXELS || p.bench));
    if (want) {
        auto tg = Clock::now();
        gpu = gpu_create();
        if (!gpu && p.backend == Backend::Gpu) fprintf(stderr, "warning: no gpu, using cpu\n");
        if (gpu && !p.quiet) printf("  gpu: %s (init %.2f ms)\n", gpu->name(), ms_since(tg));
    }
    bool use_gpu = gpu && (p.backend == Backend::Gpu || img.size() >= GPU_MIN_PIXELS);

    Timer t{p.quiet};
    int low2 = (int)(p.canny_low * p.canny_low), high2 = (int)(p.canny_high * p.canny_high);
    vector<uint8_t> mask;
    if (use_gpu) t.run("upload", [&] { gpu->upload(img); });
    t.run("canny", [&] {
        if (p.canny_float) { canny_float(img, img.w, img.h, p.canny_low, p.canny_high, mask); return; }
        if (use_gpu && !p.auto_canny) gpu->canny_front(low2, high2, mask);
        else canny_classify(to_luma(img), img.w, img.h, low2, high2, p.auto_canny, mask);
        hysteresis(mask, img.w, img.h);
    });
    size_t edge_px = std::count(mask.begin(), mask.end(), 255);

    auto seeds = t.run("sample", [&] { return sample_points(mask, img.w, img.h, p); });

    vector<uint32_t> owner;
    SeedGrid grid;
    t.run("voronoi", [&] {
        if (p.voronoi == VoronoiMode::Brute) voronoi_brute(img.w, img.h, seeds, owner);
        else if (p.voronoi == VoronoiMode::Jfa) voronoi_jfa(img.w, img.h, seeds, owner);
        else {
            grid = build_seed_grid(img.w, img.h, seeds);
            if (use_gpu) gpu->voronoi(seeds, grid, owner);
            else voronoi_grid(img.w, img.h, seeds, grid, owner);
        }
    });

    auto polys = t.run("extract", [&] {
        return p.extract_sort ? extract_sort_unique(owner, img.w, img.h, seeds) : extract_polygons(owner, img.w, img.h, seeds);
    });
    if (polys.empty()) { fprintf(stderr, "error: no polygons; try more --points\n"); return 1; }
    vector<Pt> seed_hull;
    if (p.hull || !p.no_stages) seed_hull = t.run("hull", [&] { return hull_monotone(seeds); });

    Rgb bg = image_mean(img);
    Image out;
    bool owner_ok = p.voronoi == VoronoiMode::Grid && p.color == ColorMode::Mean;
    if (owner_ok && (use_gpu || p.raster_owner)) {
        t.run("raster", [&] {
            vector<uint32_t> off, idx;
            build_csr(polys, seeds.size(), off, idx);
            if (use_gpu) gpu->raster(polys, off, idx, bg, out);
            else rasterize_by_owner(img, owner, off, idx, polys, bg, out);
        });
    } else {
        t.run("colour", [&] { color_polygons(img, polys, p.color); });
        t.run("raster", [&] { out = Image(img.w, img.h, bg); rasterize(out, polys); });
    }
    if (p.hull) draw_polyline(out, seed_hull, QUAD, true);
    if (!p.quiet) printf("  %-12s %8.2f ms\n", "total", t.total);

    t0 = Clock::now();
    if (!save_image(p.out, out, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    double encode_ms = ms_since(t0);
    if (!p.text.empty() && !write_text(p.text, img, polys)) { fprintf(stderr, "error: cannot write %s\n", p.text.c_str()); return 1; }

    double stage_ms = 0;
    if (!p.no_stages) {
        t0 = Clock::now();
        mkdir(p.stages.c_str(), 0755);
        Image edges(img.w, img.h), pts(img.w, img.h), vor(img.w, img.h), wire(img.w, img.h);
        for (size_t i = 0; i < img.size(); i++) if (mask[i]) edges.px[i] = {255, 255, 255};
        for (Pt s : seeds) pts.at(s.x, s.y) = {0, 255, 0};
        draw_polyline(pts, seed_hull, QUAD, true);
        for (size_t i = 0; i < img.size(); i++) {
            uint32_t v = owner[i] * 0x9E3779B9u; v ^= v >> 15; v *= 0x85EBCA6Bu; v ^= v >> 13;
            vor.px[i] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16)};
        }
        for (auto& q : polys)
            for (int i = 0; i < q.n; i++) { Pt a = q.v[i], b = q.v[(i + 1) % q.n]; draw_line(wire, a.x, a.y, b.x, b.y, q.n == 3 ? TRI : QUAD); }
        const Image* imgs[6] = {&img, &edges, &pts, &vor, &wire, &out};
        const char* names[6] = {"raw", "edges", "points", "voronoi", "polygons", "final"};
        vector<std::thread> th;
        for (int i = 0; i < 6; i++) th.emplace_back([&, i] { std::string e; save_image(p.stages + "/" + names[i] + ".png", *imgs[i], e); });
        for (auto& x : th) x.join();
        stage_ms = ms_since(t0);
    }

    if (!p.quiet) {
        size_t tris = 0;
        for (auto& q : polys) tris += q.n == 3;
        printf("  decode %.2f ms, encode %.2f ms, stages %.2f ms\n", decode_ms, encode_ms, stage_ms);
        printf("  %zu edge px -> %zu seeds -> %zu polygons (%zu tri, %zu quad) -> %s\n",
               edge_px, seeds.size(), polys.size(), tris, polys.size() - tris, p.out.c_str());
    }
    if (p.bench) run_bench(p, img, seeds, polys, gpu);
    delete gpu;
    return 0;
}
#endif

// ---- tests ----------------------------------------------------------------------------

#ifdef LOWPOLY_TESTS
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t test_rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t trand(uint32_t n) {
    uint64_t x = test_rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    test_rng_state = x;
    return (uint32_t)((((x * 0x2545F4914F6CDD1Dull) >> 32) * n) >> 32);
}

static vector<uint8_t> mask_with(int w, int h, const std::function<bool(int, int)>& f) {
    vector<uint8_t> m((size_t)w * h, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (f(x, y)) m[y * w + x] = 255;
    return m;
}

static vector<Pt> random_seeds(int n, int w, int h) {
    vector<uint8_t> taken((size_t)w * h, 0);
    vector<Pt> v;
    while ((int)v.size() < n) {
        Pt q{(int)trand(w), (int)trand(h)};
        if (!taken[q.y * w + q.x]) { taken[q.y * w + q.x] = 1; v.push_back(q); }
    }
    return v;
}

static Polygon mkpoly(vector<Pt> pts, Rgb c) {
    auto o = hull_monotone(pts);
    Polygon q{}; q.n = (uint8_t)o.size(); q.color = c;
    for (int i = 0; i < q.n; i++) { q.v[i] = o[i]; q.s[i] = 0; }
    return q;
}

static void test_pool() {
    vector<int> v(100000, 0);
    parallel_for((int)v.size(), [&](int b, int e) { for (int i = b; i < e; i++) v[i] = i * 2; });
    bool ok = true;
    for (int i = 0; i < (int)v.size(); i++) ok &= v[i] == i * 2;
    CHECK(ok);
    parallel_for((int)v.size(), [&](int b, int e) { for (int i = b; i < e; i++) v[i] += 1; });
    CHECK(v[12345] == 12345 * 2 + 1);
}

static void test_ppm_roundtrip() {
    Image a(7, 5);
    for (size_t i = 0; i < a.size(); i++) a.px[i] = {uint8_t(i), uint8_t(i * 3), uint8_t(i * 7)};
    std::string err;
    Image b;
    CHECK(save_image("/tmp/lowpoly-test.ppm", a, err));
    CHECK(load_image("/tmp/lowpoly-test.ppm", b, err));
    CHECK(b.w == 7 && b.h == 5 && b.px == a.px);
    CHECK(save_image("/tmp/lowpoly-test.png", a, err));
    CHECK(load_image("/tmp/lowpoly-test.png", b, err));
    CHECK(b.px == a.px);
}

static void test_canny() {
    int w = 40, h = 40;
    Image img(w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t v = x < 20 ? 0 : 255; img.at(x, y) = {v, v, v}; }
    vector<uint8_t> cls;
    canny_classify(to_luma(img), w, h, 25, 625, false, cls);
    hysteresis(cls, w, h);
    int n = 0; bool near = true;
    for (int y = 5; y < h - 5; y++) for (int x = 0; x < w; x++) if (cls[y * w + x]) { n++; near &= x >= 17 && x <= 22; }
    CHECK(n >= 30 && near);

    Image flat(32, 32, {70, 70, 70});
    canny_classify(to_luma(flat), 32, 32, 25, 625, false, cls);
    hysteresis(cls, 32, 32);
    CHECK(std::count(cls.begin(), cls.end(), 255) == 0);
    canny_classify(to_luma(flat), 32, 32, 0, 0, true, cls);
    hysteresis(cls, 32, 32);
    CHECK(std::count(cls.begin(), cls.end(), 255) == 0);

    cls = {2, 1, 1, 1, 1, 1, 1, 0, 1, 1};
    hysteresis(cls, 10, 1);
    for (int i = 0; i < 7; i++) CHECK(cls[i] == 255);
    CHECK(cls[7] == 0 && cls[8] == 0);
}

static void test_sampling() {
    Params p; p.points = 500; p.levels = 20;
    auto pts = sample_points(mask_with(200, 150, [](int x, int y) { return x == y; }), 200, 150, p);
    vector<Pt> s = pts; std::sort(s.begin(), s.end());
    CHECK(std::adjacent_find(s.begin(), s.end()) == s.end());
    bool inb = true;
    for (auto q : pts) inb &= q.x >= 0 && q.x < 200 && q.y >= 0 && q.y < 150;
    CHECK(inb && pts.size() >= 500);
    CHECK(std::find(pts.begin(), pts.end(), Pt{199, 149}) != pts.end());

    Params c; c.points = 2000; c.levels = 30; c.border = 1000000; c.uniform = 0;
    auto cp = sample_points(mask_with(400, 400, [](int x, int) { return x < 100; }), 400, 400, c);
    int left = 0;
    for (auto q : cp) left += q.x < 100;
    CHECK(left > (int)(cp.size() * 0.6));

    Params e; e.points = 100;
    CHECK(sample_points(mask_with(64, 64, [](int, int) { return false; }), 64, 64, e).size() >= 100);

    Params d; d.points = 300;
    auto m = mask_with(120, 90, [](int x, int y) { return (x * y) % 7 == 0; });
    CHECK(sample_points(m, 120, 90, d) == sample_points(m, 120, 90, d));
}

static void test_voronoi() {
    struct { int w, h, n; } cases[] = {{64, 64, 20}, {200, 130, 400}, {97, 61, 3}, {300, 200, 6000}, {50, 40, 1}};
    for (auto c : cases) {
        auto s = random_seeds(c.n, c.w, c.h);
        vector<uint32_t> a, b;
        voronoi_grid(c.w, c.h, s, build_seed_grid(c.w, c.h, s), a);
        voronoi_brute(c.w, c.h, s, b);
        CHECK(a == b);
        bool own = true;
        for (size_t i = 0; i < s.size(); i++) own &= a[s[i].y * c.w + s[i].x] == i;
        CHECK(own);
    }
    int w = 200, h = 130;
    auto s = random_seeds(400, w, h);
    vector<uint32_t> a, b;
    voronoi_jfa(w, h, s, a);
    voronoi_brute(w, h, s, b);
    int wrong = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t i = a[y * w + x], j = b[y * w + x];
        if (i == j) continue;
        auto d = [&](uint32_t k) { int64_t dx = x - s[k].x, dy = y - s[k].y; return dx * dx + dy * dy; };
        if (d(i) != d(j)) wrong++;
    }
    CHECK(wrong < w * h / 2000);
}

static void test_hull() {
    vector<Pt> sq = {{0,0},{10,0},{10,10},{0,10},{5,5},{3,7},{9,1}};
    auto hm = hull_monotone(sq);
    CHECK(hm.size() == 4 && signed_area2(hm.data(), 4) == 200);
    vector<Pt> line; for (int i = 0; i < 10; i++) line.push_back({i, 2 * i});
    CHECK(hull_monotone(line).size() == 2);
    CHECK(hull_monotone({}).empty());
    CHECK(hull_monotone({{1,1},{1,1},{1,1}}).size() == 1);
    for (int trial = 0; trial < 40; trial++) {
        int n = 4 + trial * 7;
        vector<Pt> pts; for (int i = 0; i < n; i++) pts.push_back({(int)trand(500), (int)trand(500)});
        auto h = hull_monotone(pts);
        bool convex = true, contains = true;
        for (size_t i = 0; i < h.size(); i++) {
            convex &= cross(h[i], h[(i + 1) % h.size()], h[(i + 2) % h.size()]) > 0;
            for (auto q : pts) contains &= cross(h[i], h[(i + 1) % h.size()], q) >= 0;
        }
        CHECK(convex && contains);
        auto q = hull_quick(pts);
        std::sort(h.begin(), h.end()); std::sort(q.begin(), q.end());
        CHECK(h == q);
    }
    vector<Pt> big(20000); for (auto& q : big) q = {(int)trand(4000), (int)trand(3000)};
    auto a = hull_monotone(big), b = hull_quick(big);
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    CHECK(a == b);

    vector<Pt> lex = {{0,0},{0,10},{10,0},{10,10}};
    CHECK(signed_area2(lex.data(), 4) == 0);
    auto o = hull_monotone(lex);
    CHECK(o.size() == 4 && signed_area2(o.data(), 4) == 200);
    CHECK(hull_monotone({{0,0},{10,0},{5,10},{5,3}}).size() == 3);
    Pt o4[4];
    CHECK(hull4(lex.data(), 4, o4) == 4 && signed_area2(o4, 4) == 200);
}

static void test_extract() {
    int w = 200, h = 150;
    auto s = random_seeds(120, w, h);
    vector<uint32_t> owner;
    voronoi_grid(w, h, s, build_seed_grid(w, h, s), owner);
    auto polys = extract_polygons(owner, w, h, s);
    CHECK(polys.size() > 50);
    bool ok = true;
    for (auto& q : polys) {
        int64_t area = signed_area2(q.v, q.n);
        ok &= area > 0;
        int64_t fan = 0;
        for (int i = 1; i + 1 < q.n; i++) fan += std::abs(cross(q.v[0], q.v[i], q.v[i + 1]));
        ok &= fan == area;
        for (int i = 0; i < q.n; i++) ok &= s[q.s[i]] == q.v[i];
    }
    CHECK(ok);
    vector<uint32_t> off, idx;
    build_csr(polys, s.size(), off, idx);
    size_t verts = 0;
    for (auto& q : polys) verts += q.n;
    CHECK(off.size() == s.size() + 1 && idx.size() == verts);
    CHECK(extract_polygons(owner, w, h, s).size() == polys.size());
    CHECK(extract_sort_unique(owner, w, h, s).size() == polys.size());
    vector<uint32_t> one(1, 0);
    CHECK(extract_polygons(one, 1, 1, {{0,0}}).empty());
}

static void test_raster() {
    Image img(10, 10);
    rasterize(img, {mkpoly({{2,2},{6,2},{6,6},{2,6}}, {255,255,255})});
    int n = 0; bool inside_ok = true;
    for (int y = 0; y < 10; y++) for (int x = 0; x < 10; x++) if (img.at(x, y).r == 255) { n++; inside_ok &= x >= 2 && x <= 6 && y >= 2 && y <= 6; }
    CHECK(n == 25 && inside_ok);

    Image seam(21, 21);
    rasterize(seam, {mkpoly({{0,0},{20,0},{20,20}}, {1,0,0}), mkpoly({{0,0},{20,20},{0,20}}, {2,0,0})});
    bool ok = true;
    for (auto p : seam.px) ok &= p.r != 0;
    CHECK(ok);

    vector<Polygon> ps;
    for (int i = 0; i < 300; i++) {
        auto q = mkpoly({{(int)trand(200),(int)trand(200)},{(int)trand(200),(int)trand(200)},{(int)trand(200),(int)trand(200)}}, {uint8_t(i % 251), 7, 9});
        if (q.n >= 3) ps.push_back(q);
    }
    Image a(200, 200), b(200, 200);
    rasterize(a, ps);
    pool_init(1);
    rasterize(b, ps);
    pool_init(0);
    CHECK(a.px == b.px);

    Image src(10, 10);
    for (int y = 0; y < 10; y++) for (int x = 0; x < 10; x++) src.at(x, y) = {uint8_t(x < 5 ? 0 : 100), 50, 50};
    vector<Polygon> one = {mkpoly({{5,0},{9,0},{9,9},{5,9}}, {0,0,0})};
    color_polygons(src, one, ColorMode::Mean);
    CHECK(one[0].color == (Rgb{100, 50, 50}));

    int w = 200, h = 150;
    Image rnd(w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) rnd.at(x, y) = {uint8_t(x), uint8_t(y), uint8_t((x + y) / 2)};
    auto s = random_seeds(300, w, h);
    for (int x = 0; x < w; x += 10) { s.push_back({x, 0}); s.push_back({x, h - 1}); }
    for (int y = 0; y < h; y += 10) { s.push_back({0, y}); s.push_back({w - 1, y}); }
    s.push_back({w - 1, h - 1});
    std::sort(s.begin(), s.end()); s.erase(std::unique(s.begin(), s.end()), s.end());
    vector<uint32_t> owner;
    voronoi_grid(w, h, s, build_seed_grid(w, h, s), owner);
    auto polys = extract_polygons(owner, w, h, s);
    vector<uint32_t> off, idx;
    build_csr(polys, s.size(), off, idx);
    Image o;
    auto pa = polys, pb = polys;
    rasterize_by_owner(rnd, owner, off, idx, pa, {9, 9, 9}, o);
    color_polygons(rnd, pb, ColorMode::Mean);
    int same = 0, painted = 0;
    // shared-edge pixels belong to both polygons in the bbox walk and to one in the owner walk
    for (size_t i = 0; i < pa.size(); i++)
        same += abs(pa[i].color.r - pb[i].color.r) <= 4 && abs(pa[i].color.g - pb[i].color.g) <= 4 && abs(pa[i].color.b - pb[i].color.b) <= 4;
    for (auto p : o.px) painted += p != (Rgb{9, 9, 9});
    CHECK(same > (int)(pa.size() * 0.9));
    CHECK(painted > w * h * 0.93);
}

static void test_gpu() {
    Gpu* g = gpu_create();
    if (!g) { puts("no gpu, skipping gpu tests"); return; }
    printf("gpu: %s\n", g->name());
    int w = 331, h = 247;
    Image img(w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) img.at(x, y) = {uint8_t((x * 7 + y * 3) & 255), uint8_t(x < w / 2 ? 30 : 200), uint8_t(trand(256))};
    g->upload(img);
    vector<uint8_t> cc, cg;
    canny_classify(to_luma(img), w, h, 25, 625, false, cc);
    g->canny_front(25, 625, cg);
    CHECK(cc == cg);
    auto s = random_seeds(900, w, h);
    auto grid = build_seed_grid(w, h, s);
    vector<uint32_t> oc, og;
    voronoi_grid(w, h, s, grid, oc);
    g->voronoi(s, grid, og);
    CHECK(oc == og);
    auto polys = extract_polygons(oc, w, h, s);
    vector<uint32_t> off, idx;
    build_csr(polys, s.size(), off, idx);
    Image a, b;
    auto pa = polys, pb = polys;
    rasterize_by_owner(img, oc, off, idx, pa, {1, 2, 3}, a);
    g->raster(pb, off, idx, {1, 2, 3}, b);
    CHECK(a.px == b.px);
    bool same = true;
    for (size_t i = 0; i < pa.size(); i++) same &= pa[i].color == pb[i].color;
    CHECK(same);
    delete g;
}

int main() {
    pool_init(0);
    test_pool();
    test_ppm_roundtrip();
    test_canny();
    test_sampling();
    test_voronoi();
    test_hull();
    test_extract();
    test_raster();
    test_gpu();
    if (g_fail) { fprintf(stderr, "%d failures\n", g_fail); return 1; }
    puts("ok");
    return 0;
}
#endif
