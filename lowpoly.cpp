#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI // wingdi declares Polygon()
#include <windows.h>
#include <shellapi.h>
#undef near // legacy macros clash with identifiers
#undef far
#endif
#include "lowpoly.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <thread>
#if defined(__linux__)
#include <unistd.h>
#endif
#include <unordered_map>
#include <cerrno>
#include <filesystem>

#define STBI_WINDOWS_UTF8
#define STBIW_WINDOWS_UTF8
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
bool platform_can_encode(const std::string&) { return false; }
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
// busy pool (other thread or nested call) runs inline
void parallel_for(int n, const std::function<void(int, int)>& fn) {
    if (!g_pool) pool_init(0);
    static std::mutex busy;
    std::unique_lock<std::mutex> lk(busy, std::try_to_lock);
    if (!lk.owns_lock()) { if (n > 0) fn(0, n); return; }
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

// exif orientation, APP1 "Exif" -> IFD0 tag 0x0112
int jpeg_orientation(const uint8_t* d, size_t n) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
    size_t p = 2;
    while (p + 4 <= n) {
        if (d[p] != 0xFF) return 1;
        int m = d[p + 1];
        if (m == 0xFF) { p++; continue; }
        if (m == 0xD9 || m == 0xDA) return 1;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { p += 2; continue; }
        size_t len = (size_t)d[p + 2] << 8 | d[p + 3];
        if (len < 2) return 1;
        if (m == 0xE1 && len >= 16 && p + 2 + len <= n && !memcmp(d + p + 4, "Exif\0\0", 6)) {
            const uint8_t* t = d + p + 10;
            size_t tn = len - 8;
            bool le = t[0] == 'I' && t[1] == 'I';
            if (!le && !(t[0] == 'M' && t[1] == 'M')) return 1;
            auto u16 = [&](size_t o) { return le ? t[o] | t[o + 1] << 8 : t[o] << 8 | t[o + 1]; };
            if (u16(2) != 42) return 1;
            size_t o = le ? (size_t)u16(4) | (size_t)u16(6) << 16 : (size_t)u16(4) << 16 | (size_t)u16(6);
            if (o + 2 > tn) return 1;
            size_t cnt = u16(o);
            for (size_t i = 0; i < cnt && o + 2 + (i + 1) * 12 <= tn; i++) {
                size_t e = o + 2 + i * 12;
                if (u16(e) != 0x0112) continue;
                int v = u16(e + 2) == 3 ? u16(e + 8) : 1;
                return v >= 1 && v <= 8 ? v : 1;
            }
            return 1;
        }
        p += 2 + len;
    }
    return 1;
}

// exif orientation 1..8 -> display order
void apply_orientation(Image& img, int exif) {
    if (exif < 2 || exif > 8) return;
    int w = img.w, h = img.h;
    bool sw = exif >= 5;
    Image o(sw ? h : w, sw ? w : h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int dx, dy;
        switch (exif) {
            case 2: dx = w - 1 - x; dy = y; break;
            case 3: dx = w - 1 - x; dy = h - 1 - y; break;
            case 4: dx = x; dy = h - 1 - y; break;
            case 5: dx = y; dy = x; break;
            case 6: dx = h - 1 - y; dy = x; break;
            case 7: dx = h - 1 - y; dy = w - 1 - x; break;
            default: dx = y; dy = w - 1 - x; break;
        }
        o.at(dx, dy) = img.at(x, y);
    }
    img = std::move(o);
}

// utf-8 paths, utf-16 on windows
static FILE* open_file(const std::string& p, const char* mode) {
#ifdef _WIN32
    return _wfopen(std::filesystem::u8path(p).wstring().c_str(), std::filesystem::u8path(mode).wstring().c_str());
#else
    return fopen(p.c_str(), mode);
#endif
}

bool stb_decode(const std::string& path, Image& img, std::string& err) {
    int w, h, n;
    uint8_t* d = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!d) { err = "cannot decode " + path + ": " + stbi_failure_reason(); return false; }
    img = Image(w, h);
    memcpy(img.px.data(), d, img.size() * 3);
    stbi_image_free(d);
    if (FILE* f = open_file(path, "rb")) {
        uint8_t hd[65536];
        size_t k = fread(hd, 1, sizeof hd, f);
        fclose(f);
        apply_orientation(img, jpeg_orientation(hd, k));
    }
    return true;
}

bool load_image(const std::string& path, Image& img, std::string& err) {
    err.clear();   // empty err means "not a ppm"
    FILE* f = open_file(path, "rb");
    if (!f) { err = "cannot open " + path; return false; }
    bool ok = load_ppm(f, img, err);
    fclose(f);
    if (ok) return true;
    if (!err.empty()) return false;
    if (platform_decode(path, img)) return true;
    return stb_decode(path, img, err);
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
        FILE* f = open_file(path, "wb");
        if (!f) { err = "cannot write " + path; return false; }
        fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
        fwrite(img.px.data(), 3, img.size(), f);
        fclose(f);
        return true;
    }
    if (e == "png") {
        if (write_png(path, img)) return true;
        err = "cannot write " + path;
        return false;
    }
    if (platform_encode(path, img)) return true;
    int ok;
    if (e == "jpg" || e == "jpeg") ok = stbi_write_jpg(path.c_str(), img.w, img.h, 3, img.px.data(), 90);
    else if (e == "bmp") ok = stbi_write_bmp(path.c_str(), img.w, img.h, 3, img.px.data());
    else if (e == "tga") ok = stbi_write_tga(path.c_str(), img.w, img.h, 3, img.px.data());
    else ok = 0;
    if (!ok) err = "cannot write " + path;
    return ok != 0;
}

// ---- png ---------------------------------------------------------------------------

// lz77 on previous pixel and pixel above, fixed huffman, bands in parallel

static uint32_t crc32_bytes(uint32_t c, const uint8_t* p, size_t n) {
    static uint32_t t[8][256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; t[0][i] = x; }
        for (uint32_t i = 0; i < 256; i++) for (int s = 1; s < 8; s++) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 255];
    });
    c = ~c;
    for (; n >= 8; p += 8, n -= 8) {
        uint32_t a = (p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) ^ c, b = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
        c = t[7][a & 255] ^ t[6][a >> 8 & 255] ^ t[5][a >> 16 & 255] ^ t[4][a >> 24] ^ t[3][b & 255] ^ t[2][b >> 8 & 255] ^ t[1][b >> 16 & 255] ^ t[0][b >> 24];
    }
    while (n--) c = t[0][(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}

struct Adler { uint32_t a = 1, b = 0; };
static Adler adler_bytes(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = std::min<size_t>(n, 5552);
        n -= k;
        for (; k; k--) { a += *p++; b += a; }
        a %= 65521; b %= 65521;
    }
    return {a, b};
}
static Adler adler_join(Adler x, Adler y, size_t len_y) { // x then y
    uint32_t rem = (uint32_t)(len_y % 65521);
    return {(x.a + y.a + 65521 - 1) % 65521, (uint32_t)(((uint64_t)rem * x.a + x.b + y.b + 65521 - rem) % 65521)};
}

struct FixedCodes { // lsb-first fixed huffman codes {bits, count}
    uint16_t lit[256][2];
    uint16_t len[259][2];
    uint16_t dcode[30][2]; // 5-bit code + extra bits are added per distance
    FixedCodes() {
        auto rev = [](uint32_t v, int n) { uint32_t r = 0; for (int i = 0; i < n; i++) r |= (v >> i & 1) << (n - 1 - i); return r; };
        auto sym = [&](int s, uint16_t* o) {
            if (s < 144) { o[0] = (uint16_t)rev(0x30 + s, 8); o[1] = 8; }
            else if (s < 256) { o[0] = (uint16_t)rev(0x190 + s - 144, 9); o[1] = 9; }
            else if (s < 280) { o[0] = (uint16_t)rev(s - 256, 7); o[1] = 7; }
            else { o[0] = (uint16_t)rev(0xC0 + s - 280, 8); o[1] = 8; }
        };
        for (int i = 0; i < 256; i++) sym(i, lit[i]);
        static const int lb[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
        for (int l = 3; l <= 258; l++) {
            int c = 28; while (lb[c] > l) c--;
            int eb = c < 8 || c == 28 ? 0 : (c - 4) / 4, ex = l - lb[c];
            uint16_t s[2]; sym(257 + c, s);
            len[l][0] = (uint16_t)(s[0] | ex << s[1]); len[l][1] = (uint16_t)(s[1] + eb);
        }
    }
    // distance symbol + extra bits, packed
    void dist(int d, uint32_t& bits, int& n) const {
        static const int db[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
        int c = 29; while (db[c] > d) c--;
        int eb = c < 4 ? 0 : (c - 2) / 2;
        uint32_t r = 0; for (int i = 0; i < 5; i++) r |= (c >> i & 1) << (4 - i);
        bits = r | (uint32_t)(d - db[c]) << 5; n = 5 + eb;
    }
};

struct BitOut {
    uint8_t* p; uint64_t acc = 0; int n = 0;
    void put(uint32_t v, int k) {
        acc |= (uint64_t)v << n; n += k;
        if (n >= 32) { uint32_t w = (uint32_t)acc; memcpy(p, &w, 4); p += 4; acc >>= 32; n -= 32; } // little-endian hosts only
    }
    void align() { while (n > 0) { *p++ = (uint8_t)acc; acc >>= 8; n -= 8; } acc = 0; n = 0; }
};

static size_t match_len(const uint8_t* p, size_t d, size_t max) {
    size_t l = 0;
    while (l + 8 <= max) { uint64_t a, b; memcpy(&a, p + l, 8); memcpy(&b, p + l - d, 8); if (a != b) break; l += 8; }
    while (l < max && p[l] == p[l - d]) l++;
    return l;
}

// s[b, e) -> one fixed block + sync flush (final block if last)
static size_t deflate_band(const uint8_t* s, size_t b, size_t e, size_t D, bool last, const FixedCodes& fc, const uint32_t dbits[2], const int dn[2], uint8_t* out) {
    BitOut o{out};
    o.put(last ? 3 : 2, 3); // bfinal, btype = 01
    for (size_t i = b; i < e;) {
        size_t max = std::min<size_t>(258, e - i), l3 = i >= 3 ? match_len(s + i, 3, max) : 0, lD = D && i >= D ? match_len(s + i, D, max) : 0;
        int k = lD > l3;
        size_t l = k ? lD : l3;
        if (l >= 3) { o.put(fc.len[l][0], fc.len[l][1]); o.put(dbits[k], dn[k]); i += l; }
        else { o.put(fc.lit[s[i]][0], fc.lit[s[i]][1]); i++; }
    }
    o.put(0, 7); // end of block
    if (last) o.align();
    else { o.put(0, 3); o.align(); o.put(0, 16); o.put(0xFFFF, 16); } // empty stored block = sync flush
    return (size_t)(o.p - out);
}

static size_t png_max_dist = 32768; // deflate window

bool write_png(const std::string& path, const Image& img) {
    if (img.w <= 0 || img.h <= 0 || img.px.size() < img.size()) return false;
    size_t w = img.w, h = img.h, stride = w * 3, sw = stride + 1, N = sw * h, bh = 32; // rows per band, fixed
    size_t nb = (h + bh - 1) / bh;
    size_t D = sw <= png_max_dist ? sw : 0;
    static const FixedCodes fc;
    uint32_t dbits[2]; int dn[2] = {0, 0};
    fc.dist(3, dbits[0], dn[0]);
    if (D) fc.dist((int)D, dbits[1], dn[1]);
    std::unique_ptr<uint8_t[]> raw(new uint8_t[N]);
    const uint8_t* px = (const uint8_t*)img.px.data();
    struct Band { std::unique_ptr<uint8_t[]> buf; size_t len = 0; Adler ad; };
    vector<Band> bands(nb);
    // filter 0 rows, then deflate each band
    parallel_for((int)nb, [&](int b0, int b1) {
        for (int b = b0; b < b1; b++) {
            size_t y0 = b * bh, y1 = std::min(h, y0 + bh);
            for (size_t y = y0; y < y1; y++) {
                uint8_t* r = raw.get() + y * sw; const uint8_t* c = px + y * stride;
                r[0] = 0;
                memcpy(r + 1, c, stride);
            }
        }
    });
    parallel_for((int)nb, [&](int b0, int b1) {
        for (int b = b0; b < b1; b++) {
            size_t s0 = b * bh * sw, s1 = std::min(N, s0 + bh * sw);
            Band& B = bands[b];
            size_t cap = (s1 - s0) * 9 / 8 + 64;
            B.buf.reset(new uint8_t[cap]);
            B.len = deflate_band(raw.get(), s0, s1, D, b == (int)nb - 1, fc, dbits, dn, B.buf.get());
            B.ad = adler_bytes(raw.get() + s0, s1 - s0);
        }
    });
    size_t z = 2 + 4;
    Adler ad;
    for (size_t b = 0; b < nb; b++) { z += bands[b].len; ad = b ? adler_join(ad, bands[b].ad, std::min(N, (b + 1) * bh * sw) - b * bh * sw) : bands[b].ad; }
    // sig, ihdr, idat*, iend
    size_t nidat = (z + 0x3fffffff) / 0x40000000;
    vector<uint8_t> out(8 + 25 + z + nidat * 12 + 12);
    uint8_t* p = out.data();
    auto be32 = [](uint8_t* q, uint32_t v) { q[0] = v >> 24; q[1] = v >> 16; q[2] = v >> 8; q[3] = (uint8_t)v; };
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(p, sig, 8); p += 8;
    be32(p, 13); memcpy(p + 4, "IHDR", 4); be32(p + 8, (uint32_t)w); be32(p + 12, (uint32_t)h);
    p[16] = 8; p[17] = 2; p[18] = p[19] = p[20] = 0;
    be32(p + 21, crc32_bytes(0, p + 4, 17)); p += 25;
    // zlib stream, then idat chunks (split at 1 gib)
    vector<uint8_t> zs; zs.reserve(z);
    zs.push_back(0x78); zs.push_back(0x01);
    for (auto& B : bands) zs.insert(zs.end(), B.buf.get(), B.buf.get() + B.len);
    uint8_t t4[4]; be32(t4, (ad.b << 16) | ad.a); zs.insert(zs.end(), t4, t4 + 4);
    for (size_t o = 0; o < z; o += 0x40000000) {
        size_t n = std::min<size_t>(z - o, 0x40000000);
        be32(p, (uint32_t)n); memcpy(p + 4, "IDAT", 4); memcpy(p + 8, zs.data() + o, n);
        be32(p + 8 + n, crc32_bytes(0, p + 4, n + 4)); p += n + 12;
    }
    be32(p, 0); memcpy(p + 4, "IEND", 4); be32(p + 8, 0xAE426082u); p += 12;
    FILE* f = open_file(path, "wb");
    if (!f) return false;
    bool ok = fwrite(out.data(), 1, p - out.data(), f) == (size_t)(p - out.data());
    return fclose(f) == 0 && ok;
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
    out.reserve(p.points + 4);
    auto push = [&](Pt q) {
        size_t i = (size_t)q.y * w + q.x;
        if (!taken[i]) { taken[i] = 1; out.push_back(q); }
    };
    for (Pt q : cand) push(q);

    Rng r(mix64(p.seed ^ 0xA5A5A5A5ull));
    for (int k = 0; k < uniform_budget; k++) push({(int)r.below(w), (int)r.below(h)});
    for (int tries = 0; (int)out.size() < p.points && tries < p.points * 8; tries++)
        push({(int)r.below(w), (int)r.below(h)});

    // corners are always vertices
    push({0, 0}); push({0, h - 1}); push({w - 1, 0}); push({w - 1, h - 1});
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

// exact nearest seed by rings of bins, ties to the lowest index
static inline uint32_t nearest_seed(int x, int y, const vector<Pt>& seeds, const SeedGrid& gr) {
    const int g = gr.g, gw = gr.gw, gh = gr.gh, rmax = std::max(gw, gh), cx = x / g, cy = y / g;
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
    return best;
}

// 8x8 tiles, candidates within d0 + 2r of the centre
static constexpr int VOR_TILE = 8;   // namespace scope for msvc

// avx2 clone picked at load time on x86 linux
#if defined(__x86_64__) && defined(__linux__) && defined(__GNUC__)
#define SIMD_CLONES __attribute__((target_clones("avx2", "default")))
#else
#define SIMD_CLONES
#endif

// one tile, candidate-major, 8 wide
SIMD_CLONES static void tile_sweep(const Pt* seeds, const uint32_t* cand, size_t nc, int x0, int y0, int rows,
                                   int32_t* bd, int32_t* bi) {
    for (size_t j = 0; j < nc; j++) {
        uint32_t c = cand[j];
        Pt sp = seeds[c];
        for (int yy = 0; yy < rows; yy++) {
            int32_t dy = y0 + yy - sp.y, dy2 = dy * dy;
            int32_t* rd = bd + yy * VOR_TILE;
            int32_t* ri = bi + yy * VOR_TILE;
            for (int xx = 0; xx < VOR_TILE; xx++) {
                int32_t dx = x0 + xx - sp.x, d = dx * dx + dy2;
                bool lt = d < rd[xx];
                rd[xx] = lt ? d : rd[xx];
                ri[xx] = lt ? (int32_t)c : ri[xx];
            }
        }
    }
}
void voronoi_grid(int w, int h, const vector<Pt>& seeds, const SeedGrid& gr, vector<uint32_t>& owner) {
    owner.resize((size_t)w * h);
    constexpr int T = VOR_TILE;
    const int tw = (w + T - 1) / T, th = (h + T - 1) / T, g = gr.g;
    parallel_for(th, [&](int t0, int t1) {
        vector<uint32_t> cand;
        for (int ty = t0; ty < t1; ty++)
            for (int tx = 0; tx < tw; tx++) {
                int x0 = tx * T, y0 = ty * T, x1 = std::min(x0 + T, w), y1 = std::min(y0 + T, h);
                int cx = (x0 + x1 - 1) / 2, cy = (y0 + y1 - 1) / 2;
                int rx = std::max(cx - x0, x1 - 1 - cx), ry = std::max(cy - y0, y1 - 1 - cy);
                double d0 = std::sqrt((double)dist2(cx, cy, seeds[nearest_seed(cx, cy, seeds, gr)]));
                double lim = d0 + 2 * std::sqrt((double)(rx * rx + ry * ry)) + 1;
                int64_t lim2 = (int64_t)(lim * lim) + 1;
                int br = (int)(lim / g) + 1, bcx = cx / g, bcy = cy / g;
                cand.clear();
                for (int by = std::max(bcy - br, 0); by <= std::min(bcy + br, gr.gh - 1); by++)
                    for (int bx = std::max(bcx - br, 0); bx <= std::min(bcx + br, gr.gw - 1); bx++) {
                        size_t b = (size_t)by * gr.gw + bx;
                        for (uint32_t k = gr.off[b]; k < gr.off[b + 1]; k++)
                            if (dist2(cx, cy, seeds[gr.idx[k]]) <= lim2) cand.push_back(gr.idx[k]);
                    }
                std::sort(cand.begin(), cand.end());
                // candidates in index order, strict < keeps lowest-index ties
                alignas(32) int32_t bd[VOR_TILE * VOR_TILE], bi[VOR_TILE * VOR_TILE];
                for (int k = 0; k < T * T; k++) { bd[k] = INT32_MAX; bi[k] = 0; }
                tile_sweep(seeds.data(), cand.data(), cand.size(), x0, y0, y1 - y0, bd, bi);
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) owner[(size_t)y * w + x] = (uint32_t)bi[(y - y0) * T + (x - x0)];
            }
    });
}

// project seeds whose cells touch a side onto it
void add_side_seeds(vector<Pt>& seeds, int w, int h) {
    SeedGrid gr = build_seed_grid(w, h, seeds);
    vector<uint8_t> taken((size_t)w * h, 0);
    for (Pt q : seeds) taken[(size_t)q.y * w + q.x] = 1;
    auto project = [&](int x, int y, bool horizontal) {
        Pt s = seeds[nearest_seed(x, y, seeds, gr)];
        Pt q = horizontal ? Pt{s.x, y} : Pt{x, s.y};
        size_t k = (size_t)q.y * w + q.x;
        if (!taken[k]) { taken[k] = 1; seeds.push_back(q); }
    };
    for (int x = 0; x < w; x++) { project(x, 0, true); project(x, h - 1, true); }
    for (int y = 0; y < h; y++) { project(0, y, false); project(w - 1, y, false); }
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

// triangles with circumcentre outside the image, found by sweeping each side
static vector<Key> border_keys(const vector<uint32_t>& owner, int w, int h, const vector<Pt>& seeds) {
    vector<Key> out;
    for (int side = 0; side < 4; side++) {
        int n = side < 2 ? w : h;
        // u along the side, v inward
        auto uv = [&](Pt p) -> std::pair<double, double> {
            switch (side) {
                case 0: return {(double)p.x, (double)p.y};
                case 1: return {(double)p.x, (double)(h - 1 - p.y)};
                case 2: return {(double)p.y, (double)p.x};
                default: return {(double)p.y, (double)(w - 1 - p.x)};
            }
        };
        vector<uint32_t> seq;
        for (int i = 0; i < n; i++) {
            size_t k = side == 0 ? i : side == 1 ? (size_t)(h - 1) * w + i : side == 2 ? (size_t)i * w : (size_t)i * w + w - 1;
            if (seq.empty() || seq.back() != owner[k]) seq.push_back(owner[k]);
        }
        int m = (int)seq.size();
        vector<int> prev(m), next(m), stamp(m, 0);
        vector<uint8_t> alive(m, 1);
        for (int i = 0; i < m; i++) { prev[i] = i - 1; next[i] = i + 1 < m ? i + 1 : -1; }
        struct Ev { double v; int mid, stamp; bool operator<(const Ev& o) const { return v != o.v ? v < o.v : mid > o.mid; } };
        std::priority_queue<Ev> pq;
        auto push = [&](int b) {
            if (b < 0 || prev[b] < 0 || next[b] < 0) return;
            uint32_t ia = seq[prev[b]], ib = seq[b], ic = seq[next[b]];
            if (ia == ic) return;
            auto [ax, ay] = uv(seeds[ia]); auto [bx, by] = uv(seeds[ib]); auto [cx, cy] = uv(seeds[ic]);
            double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
            if (d == 0) return;
            double cv = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
            if (cv < 0.5) pq.push({cv, b, stamp[b]});
        };
        for (int i = 0; i < m; i++) push(i);
        while (!pq.empty()) {
            Ev e = pq.top(); pq.pop();
            int b = e.mid;
            if (!alive[b] || e.stamp != stamp[b]) continue;
            int a = prev[b], c = next[b];
            Key k{{seq[a], seq[b], seq[c], UINT32_MAX}};
            std::sort(k.k, k.k + 3);
            out.push_back(k);
            alive[b] = 0;
            next[a] = c; prev[c] = a;
            stamp[a]++; stamp[c]++;
            push(a); push(c);
        }
    }
    return out;
}

// keys of every 2x2 window where three or four cells meet, per band
static vector<vector<Key>> junction_keys(const vector<uint32_t>& owner, int w, int h, const vector<Pt>& seeds) {
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
                    // skip 8 uniform windows at once
                    if ((x & 7) == 0 && x + 9 <= w) {
                        uint32_t diff = 0;
                        for (int k = 0; k < 8; k++) diff |= (r0[x + k] ^ r0[x + k + 1]) | (r0[x + k] ^ r1[x + k]) | (r1[x + k] ^ r1[x + k + 1]);
                        if (!diff) { x += 7; continue; }
                    }
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
    local.push_back(border_keys(owner, w, h, seeds));
    return local;
}

// ---- overlap repair ----------------------------------------------------------------

// > 0 if d inside circumcircle of ccw a, b, c (exact for coords < 2^14)
static int64_t incircle(Pt a, Pt b, Pt c, Pt d) {
    int64_t ax = a.x - d.x, ay = a.y - d.y, bx = b.x - d.x, by = b.y - d.y, cx = c.x - d.x, cy = c.y - d.y;
    return (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) +
           (cx * cx + cy * cy) * (ax * by - bx * ay);
}

// no separating edge
static bool interiors_meet(const Polygon& p, const Polygon& q) {
    auto separated = [](const Polygon& a, const Polygon& b) {
        for (int i = 0; i < a.n; i++) {
            Pt u = a.v[i], v = a.v[(i + 1) % a.n];
            bool all_out = true;
            for (int j = 0; j < b.n && all_out; j++) all_out = cross(u, v, b.v[j]) <= 0;
            if (all_out) return true;
        }
        return false;
    };
    return !separated(p, q) && !separated(q, p);
}

// near-cocircular seeds overlap, so retriangulate each group exactly
static vector<Polygon> resolve_overlaps(vector<Polygon> polys, const vector<Pt>& seeds) {
    size_t np = polys.size();
    vector<uint32_t> off, idx;
    build_csr(polys, seeds.size(), off, idx);
    vector<uint32_t> parent(np);
    for (size_t i = 0; i < np; i++) parent[i] = (uint32_t)i;
    auto find = [&](uint32_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
    // overlapping polygons share a seed
    int chunks = pool_size() * 4;
    vector<vector<std::pair<uint32_t, uint32_t>>> hits(chunks);
    parallel_for(chunks, [&](int c0, int c1) {
        for (int c = c0; c < c1; c++)
            for (size_t s = seeds.size() * c / chunks, e = seeds.size() * (c + 1) / chunks; s < e; s++)
                for (uint32_t a = off[s]; a < off[s + 1]; a++)
                    for (uint32_t b = a + 1; b < off[s + 1]; b++) {
                        uint32_t i = idx[a], j = idx[b], lo = UINT32_MAX;
                        for (int u = 0; u < polys[i].n; u++)
                            for (int v = 0; v < polys[j].n; v++) if (polys[i].s[u] == polys[j].s[v]) lo = std::min(lo, polys[i].s[u]);
                        if (lo == s && interiors_meet(polys[i], polys[j])) hits[c].push_back({std::min(i, j), std::max(i, j)});
                    }
    });
    // group only the overlapping ones
    vector<uint32_t> involved;
    for (auto& v : hits) for (auto [i, j] : v) { involved.push_back(i); involved.push_back(j); parent[find(i)] = find(j); }
    if (involved.empty()) return polys;
    std::sort(involved.begin(), involved.end());
    involved.erase(std::unique(involved.begin(), involved.end()), involved.end());
    vector<uint8_t> skip(np, 0);
    for (uint32_t i : involved) skip[i] = 1;
    vector<Polygon> out;
    out.reserve(np);
    for (size_t i = 0; i < np; i++) if (!skip[i]) out.push_back(polys[i]);
    std::sort(involved.begin(), involved.end(), [&](uint32_t a, uint32_t b) {
        uint32_t ra = find(a), rb = find(b);
        return ra != rb ? ra < rb : a < b;
    });
    for (size_t g0 = 0, g1; g0 < involved.size(); g0 = g1) {
        for (g1 = g0 + 1; g1 < involved.size() && find(involved[g1]) == find(involved[g0]); g1++) {}
        vector<uint32_t> g(involved.begin() + g0, involved.begin() + g1);
        vector<uint32_t> ids;
        for (uint32_t i : g) for (int k = 0; k < polys[i].n; k++) ids.push_back(polys[i].s[k]);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        if (ids.size() > 12) { for (uint32_t i : g) out.push_back(polys[i]); continue; }   // never seen, keep as is
        int m = (int)ids.size();
        vector<vector<uint32_t>> done;
        for (int a = 0; a < m; a++) for (int b = a + 1; b < m; b++) for (int c = b + 1; c < m; c++) {
            Pt pa = seeds[ids[a]], pb = seeds[ids[b]], pc = seeds[ids[c]];
            int64_t o = cross(pa, pb, pc);
            if (o == 0) continue;
            if (o < 0) std::swap(pb, pc);
            vector<uint32_t> on = {ids[a], ids[b], ids[c]};
            bool empty = true;
            for (int d = 0; d < m && empty; d++) {
                if (d == a || d == b || d == c) continue;
                int64_t t = incircle(pa, pb, pc, seeds[ids[d]]);
                if (t > 0) empty = false;
                else if (t == 0) on.push_back(ids[d]);
            }
            if (!empty) continue;
            std::sort(on.begin(), on.end());
            if (std::find(done.begin(), done.end(), on) != done.end()) continue;
            done.push_back(on);
            // cocircular seeds become one polygon or a fan
            vector<Pt> pts;
            for (uint32_t id : on) pts.push_back(seeds[id]);
            vector<Pt> hull = hull_monotone(pts);
            auto emit = [&](std::initializer_list<Pt> vs) {
                Polygon q{};
                for (Pt v : vs) {
                    q.v[q.n] = v;
                    for (uint32_t id : on) if (seeds[id] == v) q.s[q.n] = id;
                    q.n++;
                }
                out.push_back(q);
            };
            if (hull.size() == 4) emit({hull[0], hull[1], hull[2], hull[3]});
            else for (size_t f = 1; f + 1 < hull.size(); f++) emit({hull[0], hull[f], hull[f + 1]});
        }
    }
    return out;
}

static vector<Polygon> keys_to_polygons(const vector<Key>& keys, const vector<Pt>& seeds, bool exact = true) {
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
    return exact ? resolve_overlaps(std::move(out), seeds) : out;
}

std::vector<Polygon> extract_polygons(const vector<uint32_t>& owner, int w, int h, const vector<Pt>& seeds, bool exact) {
    if (w < 2 || h < 2) return {};
    auto local = junction_keys(owner, w, h, seeds);
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
    return keys_to_polygons(keys, seeds, exact);
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

struct Edges { int32_t a[4], b[4], c[4]; int n; int x0, y0, x1, y1, w, h; };

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
    e.w = w; e.h = h;
    return e;
}

static inline int32_t floor_div32(int32_t a, int32_t b) { int32_t q = a / b; return q - ((a % b != 0) && ((a < 0) != (b < 0))); }
static inline int64_t floor_div(int64_t a, int64_t b) { int64_t q = a / b; return q - ((a % b != 0) && ((a < 0) != (b < 0))); }

// convex polygon, row y -> span [xl, xr]
// fill rule samples at (x + e, y + e^2), inward on the last column and row
template <class F> static inline void spans(const Edges& e, int ylo, int yhi, F&& f) {
    int y0 = std::max(e.y0, ylo), y1 = std::min(e.y1, yhi);
    for (int y = y0; y < y1; y++) {
        int sy = y == e.h - 1 ? -1 : 1;
        // columns before the last, nudged right
        int64_t xl = e.x0, xr = std::min(e.x1, e.w - 1) - 1;
        for (int i = 0; i < e.n; i++) {
            // fits in 32 bits
            int32_t a = e.a[i], k = e.b[i] * y + e.c[i];
            if (a > 0) xl = std::max<int64_t>(xl, -floor_div32(k, a));            // a*x + k >= 0
            else if (a < 0) xr = std::min<int64_t>(xr, -floor_div32(-k, -a) - 1);  // x < k / -a
            else if (k < 0 || (k == 0 && e.b[i] * sy <= 0)) xr = xl - 1;
        }
        // the last column, nudged left
        bool last = e.x1 == e.w;
        for (int i = 0; i < e.n && last; i++) {
            int64_t v = (int64_t)e.a[i] * (e.w - 1) + (int64_t)e.b[i] * y + e.c[i];
            last = v > 0 || (v == 0 && (e.a[i] < 0 || (e.a[i] == 0 && e.b[i] * sy > 0)));
        }
        if (xl <= xr) f(y, (int)xl, last ? e.w - 1 : (int)xr);
        else if (last) f(y, e.w - 1, e.w - 1);
    }
}

template <class F> static inline void walk(const Edges& e, int w, int ylo, int yhi, F&& f) {
    spans(e, ylo, yhi, [&](int y, int xl, int xr) {
        size_t row = (size_t)y * w;
        for (int x = xl; x <= xr; x++) f(row + x);
    });
}

// per-row prefix sums of r, g, b, r^2 + g^2 + b^2
struct PolySums {
    uint64_t s[3] = {0, 0, 0}, s2 = 0, n = 0;
    double sse() const { return n ? s2 - ((double)s[0] * s[0] + (double)s[1] * s[1] + (double)s[2] * s[2]) / n : 0; }
};
struct RowSums {
    int w, h;
    size_t pw;
    vector<uint32_t> pre;
    explicit RowSums(const Image& src) : w(src.w), h(src.h), pw((size_t)src.w + 1), pre(pw * src.h * 4) {
        parallel_for(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                uint32_t* o = &pre[(size_t)y * pw * 4];
                const Rgb* row = &src.px[(size_t)y * w];
                o[0] = o[1] = o[2] = o[3] = 0;
                for (int x = 0; x < w; x++) {
                    Rgb p = row[x];
                    uint32_t* a = o + (size_t)x * 4;
                    a[4] = a[0] + p.r; a[5] = a[1] + p.g; a[6] = a[2] + p.b;
                    a[7] = a[3] + p.r * p.r + p.g * p.g + p.b * p.b;
                }
            }
        });
    }
    PolySums of(const Polygon& q) const {
        PolySums r;
        spans(edges_of(q, w, h), 0, h, [&](int y, int xl, int xr) {
            const uint32_t* a = &pre[((size_t)y * pw + xl) * 4];
            const uint32_t* b = &pre[((size_t)y * pw + xr + 1) * 4];
            r.s[0] += b[0] - a[0]; r.s[1] += b[1] - a[1]; r.s[2] += b[2] - a[2]; r.s2 += b[3] - a[3];
            r.n += xr - xl + 1;
        });
        return r;
    }
};

// open-addressing map, edge key -> u32, with tombstones
struct EdgeMap {
    static constexpr uint64_t EMPTY = ~0ull, GONE = ~0ull - 1;
    vector<uint64_t> keys;
    vector<uint32_t> vals;
    size_t mask;
    explicit EdgeMap(size_t n) {
        size_t cap = 16;
        while (cap < n * 2) cap *= 2;
        keys.assign(cap, EMPTY); vals.resize(cap); mask = cap - 1;
    }
    static size_t hash(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (size_t)k; }
    uint32_t* find(uint64_t k) {
        for (size_t i = hash(k) & mask;; i = (i + 1) & mask) {
            if (keys[i] == k) return &vals[i];
            if (keys[i] == EMPTY) return nullptr;
        }
    }
    // (slot, inserted)
    std::pair<uint32_t*, bool> insert(uint64_t k, uint32_t v) {
        size_t tomb = SIZE_MAX;
        for (size_t i = hash(k) & mask;; i = (i + 1) & mask) {
            if (keys[i] == k) return {&vals[i], false};
            if (keys[i] == GONE && tomb == SIZE_MAX) tomb = i;
            if (keys[i] == EMPTY) {
                size_t j = tomb != SIZE_MAX ? tomb : i;
                keys[j] = k; vals[j] = v;
                return {&vals[j], true};
            }
        }
    }
    void erase(uint64_t k) {
        for (size_t i = hash(k) & mask;; i = (i + 1) & mask) {
            if (keys[i] == k) { keys[i] = GONE; return; }
            if (keys[i] == EMPTY) return;
        }
    }
};

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

// merge triangle pairs into convex quads, cost = n1 n2 / (n1 + n2) |m1 - m2|^2
static void merge_flat_pairs(const RowSums& rs, vector<Polygon>& polys, double rel);
void merge_flat_pairs(const Image& src, vector<Polygon>& polys, double rel) { merge_flat_pairs(RowSums(src), polys, rel); }
static void merge_flat_pairs(const RowSums& rs, vector<Polygon>& polys, double rel) {
    size_t np = polys.size();
    vector<std::array<double, 3>> sum(np);
    vector<int64_t> cnt(np);
    vector<double> sse(np);
    parallel_for((int)np, [&](int b, int e) {
        for (int i = b; i < e; i++) {
            PolySums ps = rs.of(polys[i]);
            sum[i] = {(double)ps.s[0], (double)ps.s[1], (double)ps.s[2]}; cnt[i] = (int64_t)ps.n; sse[i] = ps.sse();
        }
    });
    double total = 0;
    for (double e : sse) total += e;
    double max_cost = rel * total / std::max<size_t>(np, 1);
    struct Cand { double cost; uint32_t a, b; };
    vector<Cand> cands;
    EdgeMap edge(np * 2);
    for (uint32_t i = 0; i < np; i++) {
        const Polygon& q = polys[i];
        if (q.n != 3 || !cnt[i]) continue;
        for (int t = 0; t < 3; t++) {
            uint32_t u = q.s[t], v = q.s[(t + 1) % 3];
            uint64_t key = ((uint64_t)std::min(u, v) << 32) | std::max(u, v);
            auto [val, fresh] = edge.insert(key, i);
            if (fresh) continue;
            uint32_t j = *val;
            if (!cnt[j]) continue;
            double n1 = cnt[i], n2 = cnt[j], d = 0;
            for (int c = 0; c < 3; c++) { double x = sum[i][c] / n1 - sum[j][c] / n2; d += x * x; }
            double cost = n1 * n2 / (n1 + n2) * d;
            if (cost <= max_cost) cands.push_back({cost, j, i});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
        return x.cost != y.cost ? x.cost < y.cost : x.a != y.a ? x.a < y.a : x.b < y.b;
    });
    vector<uint8_t> used(np, 0), drop(np, 0);
    for (auto& c : cands) {
        if (used[c.a] || used[c.b]) continue;
        const Polygon &p = polys[c.a], &q = polys[c.b];
        Pt pts[4]; uint32_t ids[4]; int n = 0;
        for (int t = 0; t < 3; t++) { pts[n] = p.v[t]; ids[n++] = p.s[t]; }
        for (int t = 0; t < 3 && n < 4; t++) {
            bool dup = false;
            for (int k = 0; k < 3; k++) dup |= q.s[t] == p.s[k];
            if (!dup) { pts[n] = q.v[t]; ids[n++] = q.s[t]; }
        }
        Pt o[4];
        if (n != 4 || hull4(pts, 4, o) != 4) continue;   // union not a convex quad
        Polygon m{};
        m.n = 4;
        for (int k = 0; k < 4; k++) {
            m.v[k] = o[k];
            for (int t = 0; t < 4; t++) if (pts[t] == o[k]) m.s[k] = ids[t];
        }
        for (int ch = 0; ch < 3; ch++) sum[c.a][ch] += sum[c.b][ch];
        cnt[c.a] += cnt[c.b];
        polys[c.a] = m;
        used[c.a] = used[c.b] = 1;
        drop[c.b] = 1;
    }
    vector<Polygon> out;
    out.reserve(np);
    for (size_t i = 0; i < np; i++) {
        if (drop[i]) continue;
        Polygon q = polys[i];
        if (cnt[i]) q.color = {uint8_t(sum[i][0] / cnt[i]), uint8_t(sum[i][1] / cnt[i]), uint8_t(sum[i][2] / cnt[i])};
        out.push_back(q);
    }
    polys.swap(out);
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

// ---- scaled / svg output ---------------------------------------------------------------

// pixel centre in [0,n-1] -> [0,N-1], corners snap
static int map_px(int x, int n, int N) {
    if (x <= 0) return 0;
    if (x >= n - 1) return N - 1;
    return std::clamp((int)std::lround((x + 0.5) * N / n - 0.5), 0, N - 1);
}

// box-average down to W x H
static Image box_down(const Image& src, int W, int H) {
    Image o(W, H);
    parallel_for(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            int sy0 = (int)((int64_t)y * src.h / H), sy1 = std::max(sy0 + 1, (int)((int64_t)(y + 1) * src.h / H));
            for (int x = 0; x < W; x++) {
                int sx0 = (int)((int64_t)x * src.w / W), sx1 = std::max(sx0 + 1, (int)((int64_t)(x + 1) * src.w / W));
                uint32_t r = 0, g = 0, b = 0, n = (uint32_t)((sy1 - sy0) * (sx1 - sx0));
                for (int sy = sy0; sy < sy1; sy++)
                    for (int sx = sx0; sx < sx1; sx++) { Rgb p = src.at(sx, sy); r += p.r; g += p.g; b += p.b; }
                o.at(x, y) = {uint8_t((r + n / 2) / n), uint8_t((g + n / 2) / n), uint8_t((b + n / 2) / n)};
            }
        }
    });
    return o;
}

// render at W x H with aa x aa supersampling
Image render_scaled(const vector<Polygon>& polys, int w, int h, int W, int H, int aa, Rgb bg) {
    if (W < w || H < h) { Image o(w, h, bg); rasterize(o, polys); return box_down(o, W, H); }
    int Ws = W * aa, Hs = H * aa;
    vector<Polygon> q = polys;
    for (auto& p : q) for (int k = 0; k < p.n; k++) p.v[k] = {map_px(p.v[k].x, w, Ws), map_px(p.v[k].y, h, Hs)};
    Image o(Ws, Hs, bg);
    rasterize(o, q);
    return aa > 1 ? box_down(o, W, H) : o;
}

// svg in half-pixel units (all integer), one color per path
std::string svg_string(const vector<Polygon>& polys, int w, int h, int W, int H) {
    auto hv = [](int x, int n) { return x <= 0 ? 0 : x >= n - 1 ? 2 * n : 2 * x + 1; };
    char b[256];
    std::string s;
    s.reserve(polys.size() * 64 + 256);
    snprintf(b, sizeof b, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\" "
             "preserveAspectRatio=\"none\">\n<g fill=\"currentColor\" stroke=\"currentColor\" stroke-width=\"%.4g\" "
             "stroke-linejoin=\"round\">\n", W, H, 2 * w, 2 * h, 2.0 * w / W);
    s += b;
    static const char* hex = "0123456789abcdef";
    for (auto& p : polys) {
        // first vertex absolute, the rest relative
        int x0 = hv(p.v[0].x, w), y0 = hv(p.v[0].y, h);
        int o = snprintf(b, sizeof b, "<path d=\"M%d %d", x0, y0);
        b[o++] = 'l';
        for (int k = 1; k < p.n; k++) {
            int x = hv(p.v[k].x, w), y = hv(p.v[k].y, h);
            // no space before a minus
            for (int d : {x - x0, y - y0}) o += snprintf(b + o, sizeof b - o, d < 0 || b[o - 1] == 'l' ? "%d" : " %d", d);
            x0 = x; y0 = y;
        }
        b[o++] = 'z';
        uint8_t c[3] = {p.color.r, p.color.g, p.color.b};
        bool shrt = c[0] % 17 == 0 && c[1] % 17 == 0 && c[2] % 17 == 0;
        o += snprintf(b + o, sizeof b - o, "\" color=\"#");
        for (int i = 0; i < 3; i++) {
            if (!shrt) b[o++] = hex[c[i] >> 4];
            b[o++] = hex[c[i] & 15];
        }
        o += snprintf(b + o, sizeof b - o, "\"/>\n");
        s.append(b, o);
    }
    s += "</g>\n</svg>\n";
    return s;
}

bool write_svg(const std::string& path, const vector<Polygon>& polys, int w, int h, int W, int H) {
    std::string s = svg_string(polys, w, h, W, H);
    FILE* f = open_file(path, "wb");
    if (!f) return false;
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    return fclose(f) == 0 && ok;
}

// own polygons, then neighbours', then any own one
static uint32_t owner_poly(const vector<Polygon>& polys, const vector<uint32_t>& off, const vector<uint32_t>& idx,
                           uint32_t s, int x, int y) {
    for (uint32_t j = off[s]; j < off[s + 1]; j++) if (inside(polys[idx[j]], x, y)) return idx[j];
    for (uint32_t j = off[s]; j < off[s + 1]; j++) {
        const Polygon& q = polys[idx[j]];
        for (int v = 0; v < q.n; v++)
            for (uint32_t m = off[q.s[v]]; m < off[q.s[v] + 1]; m++) if (inside(polys[idx[m]], x, y)) return idx[m];
    }
    return off[s] < off[s + 1] ? idx[off[s]] : UINT32_MAX;
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
            uint32_t found = owner_poly(polys, off, idx, owner[k], x, y);
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

// flip shared edges while that lowers the colour error
Polygon make_tri(const vector<Pt>& seeds, uint32_t a, uint32_t b, uint32_t c) {
    if (cross(seeds[a], seeds[b], seeds[c]) < 0) std::swap(b, c);
    Polygon q{};
    q.n = 3;
    uint32_t id[3] = {a, b, c};
    for (int k = 0; k < 3; k++) { q.s[k] = id[k]; q.v[k] = seeds[id[k]]; }
    return q;
}

static int flip_edges(const RowSums& rs, const vector<Pt>& seeds, vector<Polygon>& polys);
int flip_edges(const Image& src, const vector<Pt>& seeds, vector<Polygon>& polys) { return flip_edges(RowSums(src), seeds, polys); }
static int flip_edges(const RowSums& rs, const vector<Pt>& seeds, vector<Polygon>& polys) {
    size_t np = polys.size();
    vector<double> err(np);
    parallel_for((int)np, [&](int b, int e) { for (int i = b; i < e; i++) err[i] = rs.of(polys[i]).sse(); });
    // shared edges and their two triangles
    auto key = [](uint32_t u, uint32_t v) { return ((uint64_t)std::min(u, v) << 32) | std::max(u, v); };
    struct Edge { uint32_t a, b; int32_t t[2]; double gain; Polygon alt[2]; };
    vector<Edge> edges;
    EdgeMap slot(np * 2);
    for (uint32_t i = 0; i < np; i++) {
        if (polys[i].n != 3) continue;
        for (int t = 0; t < 3; t++) {
            uint32_t u = polys[i].s[t], v = polys[i].s[(t + 1) % 3];
            auto [val, fresh] = slot.insert(key(u, v), (uint32_t)edges.size());
            if (fresh) edges.push_back({std::min(u, v), std::max(u, v), {(int32_t)i, -1}, 0, {}});
            else edges[*val].t[1] = (int32_t)i;
        }
    }
    auto other = [](const Polygon& q, uint32_t u, uint32_t v) {
        for (int t = 0; t < 3; t++) if (q.s[t] != u && q.s[t] != v) return q.s[t];
        return UINT32_MAX;
    };
    // gain of the other diagonal, quad must be convex
    auto score = [&](Edge& E) {
        E.gain = 0;
        if (E.t[1] < 0) return;
        uint32_t c = other(polys[E.t[0]], E.a, E.b), d = other(polys[E.t[1]], E.a, E.b);
        Pt A = seeds[E.a], B = seeds[E.b], C = seeds[c], D = seeds[d];
        int64_t s1 = cross(C, D, A), s2 = cross(C, D, B), s3 = cross(A, B, C), s4 = cross(A, B, D);
        if (!((s1 > 0 && s2 < 0) || (s1 < 0 && s2 > 0)) || !((s3 > 0 && s4 < 0) || (s3 < 0 && s4 > 0))) return;
        E.alt[0] = make_tri(seeds, c, d, E.a);
        E.alt[1] = make_tri(seeds, d, c, E.b);
        E.gain = err[E.t[0]] + err[E.t[1]] - rs.of(E.alt[0]).sse() - rs.of(E.alt[1]).sse();
    };
    vector<uint32_t> dirty(edges.size());
    for (uint32_t i = 0; i < edges.size(); i++) dirty[i] = i;
    const double EPS = 1e-6;
    int flips = 0;
    vector<uint8_t> busy(np, 0), mark(edges.size(), 0);
    for (int round = 0; round < 64 && !dirty.empty(); round++) {
        parallel_for((int)dirty.size(), [&](int b, int e) { for (int i = b; i < e; i++) score(edges[dirty[i]]); });
        // best first, one flip per triangle per round
        vector<uint32_t> cand;
        for (uint32_t i = 0; i < edges.size(); i++) if (edges[i].gain > EPS) cand.push_back(i);
        std::sort(cand.begin(), cand.end(), [&](uint32_t x, uint32_t y) { return edges[x].gain != edges[y].gain ? edges[x].gain > edges[y].gain : x < y; });
        vector<uint32_t> apply;
        for (uint32_t i : cand) {
            Edge& E = edges[i];
            if (busy[E.t[0]] || busy[E.t[1]]) continue;
            busy[E.t[0]] = busy[E.t[1]] = 1;
            apply.push_back(i);
        }
        if (apply.empty()) break;
        dirty.clear();
        auto touch = [&](uint32_t e) { if (!mark[e]) { mark[e] = 1; dirty.push_back(e); } };
        for (uint32_t i : apply) {
            Edge& E = edges[i];
            int32_t t0 = E.t[0], t1 = E.t[1];
            uint32_t c = other(polys[t0], E.a, E.b), d = other(polys[t1], E.a, E.b);
            // t0 = (c, d, a), t1 = (d, c, b)
            auto retarget = [&](uint32_t u, uint32_t v, int32_t from, int32_t to) {
                Edge& N = edges[*slot.find(key(u, v))];
                if (N.t[0] == from) N.t[0] = to; else if (N.t[1] == from) N.t[1] = to;
            };
            retarget(c, E.b, t0, t1);
            retarget(d, E.a, t1, t0);
            uint32_t a = E.a, b = E.b;
            polys[t0] = E.alt[0]; polys[t1] = E.alt[1];
            slot.erase(key(a, b));
            *slot.insert(key(c, d), i).first = i;
            E.a = std::min(c, d); E.b = std::max(c, d); E.gain = 0;
            for (uint64_t k2 : {key(a, c), key(c, b), key(b, d), key(d, a)}) touch(*slot.find(k2));
            touch(i);
            flips++;
        }
        parallel_for((int)apply.size(), [&](int b0, int b1) {
            for (int k = b0; k < b1; k++) for (int32_t t : edges[apply[k]].t) err[t] = rs.of(polys[t]).sse();
        });
        for (uint32_t i : apply) busy[edges[i].t[0]] = busy[edges[i].t[1]] = 0;
        for (uint32_t e : dirty) mark[e] = 0;
    }
    return flips;
}

static int move_vertices(const RowSums& rs, vector<Pt>& seeds, vector<Polygon>& polys, int rounds);
int relax_vertices(const Image& src, vector<Pt>& seeds, vector<Polygon>& polys, int rounds) {
    return move_vertices(RowSums(src), seeds, polys, rounds);
}

// move vertices 1-2 px while the error drops, polygons stay convex, star area fixed
static int move_vertices(const RowSums& rs, vector<Pt>& seeds, vector<Polygon>& polys, int rounds) {
    int w = rs.w, h = rs.h;
    size_t np = polys.size(), ns = seeds.size();
    vector<uint32_t> off, idx;
    build_csr(polys, ns, off, idx);
    vector<double> err(np);
    parallel_for((int)np, [&](int b, int e) { for (int i = b; i < e; i++) err[i] = rs.of(polys[i]).sse(); });
    static const Pt STEPS[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
                               {2, 0}, {-2, 0}, {0, 2}, {0, -2}};
    auto valid = [](const Polygon& q) {
        for (int k = 0; k < q.n; k++) if (cross(q.v[k], q.v[(k + 1) % q.n], q.v[(k + 2) % q.n]) <= 0) return false;
        return true;
    };
    struct Move { double gain; uint32_t v; Pt to; };
    int moved = 0;
    // colour vertices so no two share a polygon, one class moves at once
    vector<int> colour(ns, -1);
    int ncol = 0;
    for (uint32_t v = 0; v < ns; v++) {
        uint64_t used = 0;
        for (uint32_t j = off[v]; j < off[v + 1]; j++) {
            const Polygon& q = polys[idx[j]];
            for (int k = 0; k < q.n; k++) if (colour[q.s[k]] >= 0 && colour[q.s[k]] < 64) used |= 1ull << colour[q.s[k]];
        }
        int c = 0;
        while (c < 64 && (used >> c & 1)) c++;
        colour[v] = c;
        ncol = std::max(ncol, c + 1);
    }
    vector<vector<uint32_t>> cls(ncol);
    for (uint32_t v = 0; v < ns; v++) cls[colour[v]].push_back(v);
    vector<uint8_t> dirty(ns, 1);
    vector<Move> best(ns);
    for (int r = 0; r < rounds; r++) {
        int round_moves = 0;
        for (auto& group : cls) {
            // only vertices near a move
            vector<uint32_t> todo;
            for (uint32_t v : group) if (dirty[v]) { todo.push_back(v); dirty[v] = 0; }
            parallel_for((int)todo.size(), [&](int b, int e) {
                Polygon tmp[16];
                for (int ti = b; ti < e; ti++) {
                    uint32_t v = todo[ti];
                    best[v] = Move{0, v, seeds[v]};
                    Pt p = seeds[v];
                    bool sx = p.x == 0 || p.x == w - 1, sy = p.y == 0 || p.y == h - 1;
                    if ((sx && sy) || off[v + 1] - off[v] > 16 || off[v] == off[v + 1]) continue;
                    double before = 0;
                    int64_t area = 0;
                    for (uint32_t j = off[v]; j < off[v + 1]; j++) { before += err[idx[j]]; area += signed_area2(polys[idx[j]].v, polys[idx[j]].n); }
                    auto tryat = [&](Pt q) {
                        if (q.x < 0 || q.y < 0 || q.x >= w || q.y >= h) return false;
                        double after = 0;
                        bool ok = true;
                        int64_t area2 = 0;
                        uint32_t t = 0;
                        for (uint32_t j = off[v]; j < off[v + 1] && ok; j++, t++) {
                            tmp[t] = polys[idx[j]];
                            for (int k = 0; k < tmp[t].n; k++) if (tmp[t].s[k] == v) tmp[t].v[k] = q;
                            ok = valid(tmp[t]);
                            area2 += signed_area2(tmp[t].v, tmp[t].n);
                        }
                        // star area changes if v leaves its star
                        ok = ok && area2 == area;
                        for (uint32_t k = 0; ok && k < t; k++) after += rs.of(tmp[k]).sse();
                        if (!ok || before - after <= best[v].gain) return false;
                        best[v] = {before - after, v, q};
                        return true;
                    };
                    for (Pt d : STEPS) {
                        if ((sx && d.x) || (sy && d.y)) continue;   // a side vertex stays on its side
                        tryat({p.x + d.x, p.y + d.y});
                    }
                    // line search, up to 6 px
                    if (best[v].gain > 0) {
                        Pt d{best[v].to.x - p.x, best[v].to.y - p.y};
                        if (std::abs(d.x) == 2 || std::abs(d.y) == 2) d = {d.x / 2, d.y / 2};
                        for (int k = 0; k < 6 && tryat({best[v].to.x + d.x, best[v].to.y + d.y}); k++) {}
                    }
                }
            });
            vector<uint32_t> apply;
            for (uint32_t v : todo) if (best[v].gain > 1e-6) apply.push_back(v);
            for (uint32_t v : apply) {
                seeds[v] = best[v].to;
                for (uint32_t j = off[v]; j < off[v + 1]; j++) {
                    Polygon& q = polys[idx[j]];
                    for (int k = 0; k < q.n; k++) if (q.s[k] == v) q.v[k] = best[v].to;
                }
            }
            parallel_for((int)apply.size(), [&](int b, int e) {
                for (int a = b; a < e; a++) for (uint32_t j = off[apply[a]]; j < off[apply[a] + 1]; j++) err[idx[j]] = rs.of(polys[idx[j]]).sse();
            });
            for (uint32_t v : apply)
                for (uint32_t j = off[v]; j < off[v + 1]; j++) {
                    const Polygon& q = polys[idx[j]];
                    for (int k = 0; k < q.n; k++) dirty[q.s[k]] = 1;
                }
            round_moves += (int)apply.size();
        }
        moved += round_moves;
        if (!round_moves) break;
    }
    return moved;
}

// ---- refinement -------------------------------------------------------------------

// split the worst polygons, growing the set by GROWTH per round
static const double GROWTH = 0.3;

static void refine_seeds(const Image& src, const RowSums& rs, vector<Pt>& seeds, size_t target, Gpu* gpu);
void refine_seeds(const Image& src, vector<Pt>& seeds, size_t target, Gpu* gpu) { refine_seeds(src, RowSums(src), seeds, target, gpu); }
static void refine_seeds(const Image& src, const RowSums& rs, vector<Pt>& seeds, size_t target, Gpu* gpu) {
    int w = src.w, h = src.h;
    vector<uint8_t> taken((size_t)w * h, 0);
    for (Pt s : seeds) taken[(size_t)s.y * w + s.x] = 1;
    vector<uint32_t> owner;
    while (seeds.size() < target) {
        SeedGrid grid = build_seed_grid(w, h, seeds);
        if (gpu) gpu->voronoi(seeds, grid, owner);
        else voronoi_grid(w, h, seeds, grid, owner);
        // no overlap repair needed for scoring
        vector<Polygon> polys = extract_polygons(owner, w, h, seeds, false);
        // sse = sum(x^2) - sum(x)^2 / n
        vector<double> err(polys.size());
        vector<std::array<float, 3>> mean(polys.size());
        parallel_for((int)polys.size(), [&](int b, int e) {
            for (int i = b; i < e; i++) {
                PolySums ps = rs.of(polys[i]);
                err[i] = ps.sse();
                for (int c = 0; c < 3; c++) mean[i][c] = ps.n ? (float)((double)ps.s[c] / ps.n) : 0.f;
            }
        });
        size_t add = std::min({target - seeds.size(), std::max<size_t>(1, (size_t)(seeds.size() * GROWTH)), polys.size()});
        vector<uint32_t> order(polys.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = (uint32_t)i;
        // partial sort of the worst
        auto worse = [&](uint32_t a, uint32_t b) { return err[a] != err[b] ? err[a] > err[b] : a < b; };
        size_t sorted = std::min(order.size(), add + add / 2 + 64);
        std::partial_sort(order.begin(), order.begin() + sorted, order.end(), worse);
        // split at the centroid weighted by squared colour deviation
        auto split_at = [&](uint32_t i) -> std::array<Pt, 2> {
            const Polygon& q = polys[i];
            int cx = 0, cy = 0;
            for (int t = 0; t < q.n; t++) { cx += q.v[t].x; cy += q.v[t].y; }
            Pt c{cx / q.n, cy / q.n};
            float m0 = mean[i][0], m1 = mean[i][1], m2 = mean[i][2];
            double sw = 0, sx = 0, sy = 0;
            spans(edges_of(q, w, h), 0, h, [&](int y, int xl, int xr) {
                const Rgb* row = &src.px[(size_t)y * w];
                double rw = 0;
                for (int x = xl; x <= xr; x++) {
                    float dr = row[x].r - m0, dg = row[x].g - m1, db = row[x].b - m2;
                    float d = dr * dr + dg * dg + db * db;
                    rw += d; sx += (double)d * x;
                }
                sw += rw; sy += rw * y;
            });
            return {sw > 0 ? Pt{(int)std::lround(sx / sw), (int)std::lround(sy / sw)} : c, c};
        };
        vector<std::array<Pt, 2>> at(sorted);
        parallel_for((int)sorted, [&](int b, int e) { for (int j = b; j < e; j++) at[j] = split_at(order[j]); });
        size_t before = seeds.size();
        // skip taken points
        for (size_t j = 0; j < order.size() && seeds.size() - before < add && err[order[j]] > 0; j++) {
            if (j == at.size()) {
                std::sort(order.begin() + j, order.end(), worse);
                at.resize(order.size());
                parallel_for((int)(order.size() - j), [&](int b, int e) { for (int t = b; t < e; t++) at[j + t] = split_at(order[j + t]); });
            }
            for (Pt q : at[j]) {
                size_t k = (size_t)q.y * w + q.x;
                if (taken[k]) continue;
                taken[k] = 1;
                seeds.push_back(q);
                break;
            }
        }
        if (seeds.size() == before) break;
    }
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
"  -o FILE            output, repeatable. png jpg bmp tga ppm svg, heic tiff on macos\n"
"                     (default <input>-lowpoly.png)\n"
"  --scale F          output size multiplier, 0.01 to 100 (1)\n"
"  --aa N             supersample N x N then average down, 1 to 8 (1)\n"
"  --points N         interior sample points (10000)\n"
"  --levels N         grid levels for density weighting (150)\n"
"  --uniform F        fraction of points spread uniformly (0.08)\n"
"  --refine F         fraction of points placed by error refinement (0.95)\n"
"  --flip MODE        on | off, flip edges to lower the colour error (on)\n"
"  --relax N          rounds of nudging points to lower the colour error (2)\n"
"  --merge F          join close-coloured triangles into quads, 0 keeps them all (1)\n"
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
"  --stages DIR       also write the six stage images to DIR\n"
"  --hull             overlay the seed hull on the output\n"
"  --text FILE        also write the Tri/Qua shape list\n"
"  --bench            time each stage against its alternatives\n"
"  -q                 quiet\n";

static bool can_write(const std::string& path) {
    std::string e = ext_of(path);
    return e == "png" || e == "ppm" || e == "jpg" || e == "jpeg" || e == "bmp" || e == "tga" || e == "svg" ||
           platform_can_encode(e);
}

static bool parse_args(int argc, char** argv, Params& p, std::string& err) {
    auto need = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) { err = std::string(flag) + " needs a value"; return nullptr; }
        return argv[++i];
    };
    // whole string, in range
    auto num = [&](int& i, const char* flag, double lo, double hi, double& out) {
        const char* v = need(i, flag);
        if (!v) return false;
        char* end = nullptr;
        errno = 0;
        double d = strtod(v, &end);
        if (end == v || *end || errno || !std::isfinite(d) || d < lo || d > hi) {
            char buf[160];
            snprintf(buf, sizeof buf, "%s must be a number in %g..%g, got '%s'", flag, lo, hi, v);
            err = buf;
            return false;
        }
        out = d;
        return true;
    };
    auto integer = [&](int& i, const char* flag, int lo, int hi, int& out) {
        double d;
        if (!num(i, flag, lo, hi, d)) return false;
        if (d != std::floor(d)) { err = std::string(flag) + " must be an integer, got '" + argv[i] + "'"; return false; }
        out = (int)d;
        return true;
    };
    auto choice = [&](int& i, const char* flag, std::initializer_list<const char*> opts, int& out) {
        const char* v = need(i, flag);
        if (!v) return false;
        int k = 0;
        for (const char* o : opts) { if (!strcmp(v, o)) { out = k; return true; } k++; }
        err = std::string("unknown ") + flag + " '" + v + "'";
        return false;
    };
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char* v;
        double d;
        int k;
        bool ok = true;
        if (a == "-h" || a == "--help") { fputs(USAGE, stdout); exit(0); }
        else if (a == "-o") { if ((ok = (v = need(i, "-o")) != nullptr)) p.outs.push_back(v); }
        else if (a == "--points") ok = integer(i, "--points", 0, 5000000, p.points);
        else if (a == "--levels") ok = integer(i, "--levels", 1, 10000, p.levels);
        else if (a == "--uniform") ok = num(i, "--uniform", 0, 1, p.uniform);
        else if (a == "--refine") ok = num(i, "--refine", 0, 1, p.refine);
        else if (a == "--merge") ok = num(i, "--merge", 0, 1000, p.merge);
        else if (a == "--relax") ok = integer(i, "--relax", 0, 100, p.relax);
        else if (a == "--flip") { if ((ok = choice(i, "--flip", {"off", "on"}, k))) p.flip = k == 1; }
        else if (a == "--seed") {
            if ((ok = (v = need(i, "--seed")) != nullptr)) {
                char* end = nullptr;
                errno = 0;
                p.seed = strtoull(v, &end, 10);
                if (end == v || *end || errno || *v == '-') { err = std::string("--seed must be a non-negative integer, got '") + v + "'"; ok = false; }
            }
        }
        else if (a == "--canny-low") { if ((ok = num(i, "--canny-low", 0, 2000, d))) p.canny_low = (float)d; }
        else if (a == "--canny-high") { if ((ok = num(i, "--canny-high", 0, 2000, d))) p.canny_high = (float)d; }
        else if (a == "--auto-canny") p.auto_canny = true;
        else if (a == "--scale") ok = num(i, "--scale", 0.01, 100, p.scale);
        else if (a == "--aa") ok = integer(i, "--aa", 1, 8, p.aa);
        else if (a == "--threads") ok = integer(i, "--threads", 0, 1024, p.threads);
        else if (a == "--stages") { if ((ok = (v = need(i, "--stages")) != nullptr)) p.stages = v; }
        else if (a == "--no-stages") p.stages.clear();   // kept for old scripts
        else if (a == "--hull") p.hull = true;
        else if (a == "--text") { if ((ok = (v = need(i, "--text")) != nullptr)) p.text = v; }
        else if (a == "--bench") p.bench = true;
        else if (a == "-q") p.quiet = true;
        else if (a == "--color") { if ((ok = choice(i, "--color", {"mean", "corner"}, k))) p.color = (ColorMode)k; }
        else if (a == "--voronoi") { if ((ok = choice(i, "--voronoi", {"grid", "jfa", "brute"}, k))) p.voronoi = (VoronoiMode)k; }
        else if (a == "--canny") { if ((ok = choice(i, "--canny", {"int", "float"}, k))) p.canny_float = k == 1; }
        else if (a == "--extract") { if ((ok = choice(i, "--extract", {"hash", "sort"}, k))) p.extract_sort = k == 1; }
        else if (a == "--raster") { if ((ok = choice(i, "--raster", {"bbox", "owner"}, k))) p.raster_owner = k == 1; }
        else if (a == "--backend") { if ((ok = choice(i, "--backend", {"auto", "cpu", "gpu"}, k))) p.backend = (Backend)k; }
        else if (a[0] == '-' && a.size() > 1) { err = "unknown option " + a; return false; }
        else if (p.in.empty()) p.in = a;
        else { err = "unexpected argument " + a; return false; }
        if (!ok) return false;
    }
    if (p.in.empty()) { err = "no input file"; return false; }
    if (p.canny_low > p.canny_high) { err = "--canny-low is above --canny-high"; return false; }
    if (p.outs.empty()) {
        std::string stem = std::filesystem::u8path(p.in).stem().u8string();
        p.outs.push_back(stem + "-lowpoly.png");
    }
    auto same = [](const std::string& x, const std::string& y) {
        std::error_code ec;
        auto nx = std::filesystem::absolute(std::filesystem::u8path(x), ec).lexically_normal(), ny = std::filesystem::absolute(std::filesystem::u8path(y), ec).lexically_normal();
        return nx == ny;
    };
    for (auto& o : p.outs) {
        if (!can_write(o)) { err = "cannot write ." + ext_of(o) + " files (" + o + ")"; return false; }
        if (same(o, p.in)) { err = "refusing to overwrite the input " + o; return false; }
    }
    if (!p.text.empty() && same(p.text, p.in)) { err = "refusing to overwrite the input " + p.text; return false; }
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
    FILE* f = open_file(path, "wb");
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

// auto never picks the gpu, the cpu path is faster end to end
static const size_t GPU_MIN_PIXELS = SIZE_MAX;

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
    auto local = junction_keys(owner, w, h, seeds);
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
#if defined(__linux__) && defined(__GLIBC__)
    // glibc keeps freed blocks and uses huge pages
    if (!getenv("GLIBC_TUNABLES")) {
        setenv("GLIBC_TUNABLES", "glibc.malloc.hugetlb=1:glibc.malloc.mmap_threshold=1073741824:"
                                 "glibc.malloc.trim_threshold=4294967295", 1);
        execv("/proc/self/exe", argv);
        unsetenv("GLIBC_TUNABLES");
    }
#endif
#ifdef _WIN32
    // argv as utf-8
    std::vector<std::string> args8;
    std::vector<char*> argv8;
    int wn = 0;
    if (LPWSTR* wa = CommandLineToArgvW(GetCommandLineW(), &wn)) {
        for (int i = 0; i < wn; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wa[i], -1, nullptr, 0, nullptr, nullptr);
            std::string a(n > 0 ? n - 1 : 0, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wa[i], -1, &a[0], n, nullptr, nullptr);
            args8.push_back(std::move(a));
        }
        LocalFree(wa);
        for (auto& a : args8) argv8.push_back(&a[0]);
        argv8.push_back(nullptr);
        argc = wn;
        argv = argv8.data();
    }
#endif
    Params p;
    std::string err;
    if (!parse_args(argc, argv, p, err)) { fprintf(stderr, "error: %s\n%s", err.c_str(), USAGE); return 1; }
    stbi_write_png_compression_level = 2;
    stbi_write_force_png_filter = 0; // filter none
    pool_init(p.threads);

    // gpu init overlaps decode
    Gpu* gpu = nullptr;
    double gpu_ms = 0;
    std::thread gpu_th;
    // only if the image will use it
    int iw = 0, ih = 0, ic = 0;
    bool large = GPU_MIN_PIXELS != SIZE_MAX && (!stbi_info(p.in.c_str(), &iw, &ih, &ic) || (size_t)iw * ih >= GPU_MIN_PIXELS);
    if (p.backend == Backend::Gpu || (p.backend == Backend::Auto && (large || p.bench))) gpu_th = std::thread([&] { auto tg = Clock::now(); gpu = gpu_create(); gpu_ms = ms_since(tg); });
    auto join_gpu = [&] { if (gpu_th.joinable()) gpu_th.join(); };

    Image img;
    auto t0 = Clock::now();
    if (!load_image(p.in, img, err)) { join_gpu(); delete gpu; fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    double decode_ms = ms_since(t0);
    join_gpu();
    auto fail = [&](const char* fmt, auto... args) { delete gpu; fprintf(stderr, fmt, args...); return 1; };
    if (img.w > MAX_DIM || img.h > MAX_DIM) return fail("error: image larger than %d px per side\n", MAX_DIM);
    if (img.w < 2 || img.h < 2) return fail("%s", "error: image too small\n");
    int OW = std::max(1, (int)std::lround(img.w * p.scale)), OH = std::max(1, (int)std::lround(img.h * p.scale));
    int aa = (OW < img.w || OH < img.h) ? 1 : p.aa;   // shrinking renders 1x and box-downsamples
    bool scaled = OW != img.w || OH != img.h || aa > 1;
    if ((int64_t)OW * aa > MAX_DIM || (int64_t)OH * aa > MAX_DIM || (int64_t)OW * aa * OH * aa > (1 << 27))
        return fail("error: output %dx%d (x%d supersampled) is too large (max %d px per side, %d Mpx); lower --scale or --aa\n",
                    OW, OH, aa, MAX_DIM, (1 << 27) >> 20);
    if (!p.quiet) printf("%s  %dx%d  %d threads\n", p.in.c_str(), img.w, img.h, pool_size());

    bool want = p.backend == Backend::Gpu || (p.backend == Backend::Auto && (img.size() >= GPU_MIN_PIXELS || p.bench));
    if (!want) { delete gpu; gpu = nullptr; }
    else {
        if (!gpu && p.backend == Backend::Gpu) fprintf(stderr, "warning: no gpu, using cpu\n");
        if (gpu && !p.quiet) printf("  gpu: %s (init %.2f ms)\n", gpu->name(), gpu_ms);
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

    auto seeds = t.run("sample", [&] {
        Params ps = p;
        ps.points = p.points - (int)(p.points * p.refine);
        return sample_points(mask, img.w, img.h, ps);
    });
    t.run("sides", [&] { add_side_seeds(seeds, img.w, img.h); });
    // cpu voronoi, faster than the gpu round trip
    bool merge = p.merge > 0 && p.color == ColorMode::Mean;
    std::unique_ptr<RowSums> sums;
    if (p.refine > 0 || p.flip || p.relax > 0 || merge) t.run("sums", [&] { sums.reset(new RowSums(img)); });
    if (p.refine > 0) t.run("refine", [&] { refine_seeds(img, *sums, seeds, seeds.size() + (size_t)(p.points * p.refine), nullptr); });

    // sides follow the refined interior
    if (p.refine > 0) t.run("sides", [&] { add_side_seeds(seeds, img.w, img.h); });

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
    if (polys.empty()) return fail("%s", "error: no polygons; try more --points\n");
    vector<Pt> seed_hull;
    if (p.hull || !p.stages.empty()) seed_hull = t.run("hull", [&] { return hull_monotone(seeds); });

    Rgb bg = image_mean(img);
    Image out;
    if (p.flip) t.run("flip", [&] { flip_edges(*sums, seeds, polys); });
    // alternate moves and flips
    for (int c = 0; c < p.relax; c++) {
        t.run("move", [&] { move_vertices(*sums, seeds, polys, 2); });
        if (p.flip) t.run("flip", [&] { flip_edges(*sums, seeds, polys); });
    }
    // owner lookup needs the unmodified delaunay mesh
    bool owner_ok = p.voronoi == VoronoiMode::Grid && p.color == ColorMode::Mean && !p.flip && p.relax == 0;
    if (owner_ok && (use_gpu || p.raster_owner)) {
        if (merge) t.run("merge", [&] { merge_flat_pairs(*sums, polys, p.merge); });
        t.run("raster", [&] {
            vector<uint32_t> off, idx;
            build_csr(polys, seeds.size(), off, idx);
            if (use_gpu) gpu->raster(polys, off, idx, bg, out);
            else rasterize_by_owner(img, owner, off, idx, polys, bg, out);
        });
    } else {
        if (merge) t.run("merge", [&] { merge_flat_pairs(*sums, polys, p.merge); });   // colours them too
        // fit the merged quads
        if (merge && p.relax > 0) t.run("move", [&] { move_vertices(*sums, seeds, polys, 2); color_polygons(img, polys, ColorMode::Mean); });
        else t.run("colour", [&] { color_polygons(img, polys, p.color); });
        t.run("raster", [&] { out = Image(img.w, img.h, bg); rasterize(out, polys); });
    }
    if (p.hull) draw_polyline(out, seed_hull, QUAD, true);
    Image big;
    bool want_raster = false;
    for (auto& o : p.outs) want_raster |= ext_of(o) != "svg";
    if (scaled && want_raster) {
        t.run("scale", [&] {
            big = render_scaled(polys, img.w, img.h, OW, OH, aa, bg);
            if (p.hull) {
                vector<Pt> hv = seed_hull;
                for (auto& q : hv) q = {map_px(q.x, img.w, OW), map_px(q.y, img.h, OH)};
                draw_polyline(big, hv, QUAD, true);
            }
        });
    }
    if (!p.quiet) printf("  %-12s %8.2f ms\n", "total", t.total);

    t0 = Clock::now();
    for (auto& o : p.outs) {
        if (ext_of(o) == "svg") { if (!write_svg(o, polys, img.w, img.h, OW, OH)) return fail("error: cannot write %s\n", o.c_str()); }
        else if (!save_image(o, scaled ? big : out, err)) return fail("error: %s\n", err.c_str());
    }
    double encode_ms = ms_since(t0);
    if (!p.text.empty() && !write_text(p.text, img, polys)) return fail("error: cannot write %s\n", p.text.c_str());

    double stage_ms = 0;
    if (!p.stages.empty()) {
        t0 = Clock::now();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(p.stages), ec);
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
               edge_px, seeds.size(), polys.size(), tris, polys.size() - tris, p.outs[0].c_str());
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
    std::error_code tec;
    std::string base = (std::filesystem::temp_directory_path(tec) / "lowpoly-test").string();
    CHECK(save_image(base + ".ppm", a, err));
    CHECK(load_image(base + ".ppm", b, err));
    CHECK(b.w == 7 && b.h == 5 && b.px == a.px);
    // non-ascii file name round trip
    {
        std::string u = (std::filesystem::temp_directory_path(tec) / std::filesystem::u8path("lowpoly-\xC3\xBCn\xC3\xAF-\xE2\x82\xAC.png")).u8string();
        CHECK(save_image(u, a, err));
        CHECK(load_image(u, b, err) && b.w == 7 && b.h == 5);
        std::filesystem::remove(std::filesystem::u8path(u), tec);
    }
    CHECK(save_image(base + ".png", a, err));
    CHECK(load_image(base + ".png", b, err));
    CHECK(b.px == a.px);
}

// jpeg with an exif orientation tag
static vector<uint8_t> with_exif(const vector<uint8_t>& jpg, bool le, int ori) {
    auto w16 = [&](vector<uint8_t>& v, int x) { if (le) { v.push_back(x & 255); v.push_back(x >> 8); } else { v.push_back(x >> 8); v.push_back(x & 255); } };
    vector<uint8_t> t = {uint8_t(le ? 'I' : 'M'), uint8_t(le ? 'I' : 'M')};
    w16(t, 42);
    if (le) { w16(t, 8); w16(t, 0); } else { w16(t, 0); w16(t, 8); }   // ifd0 offset 8 (32-bit)
    auto cnt1 = [&] { if (le) { w16(t, 1); w16(t, 0); } else { w16(t, 0); w16(t, 1); } };
    w16(t, 2);                           // a decoy tag, then orientation
    w16(t, 0x0100); w16(t, 3); cnt1(); w16(t, 99); w16(t, 0);
    w16(t, 0x0112); w16(t, 3); cnt1(); w16(t, ori); w16(t, 0);
    for (int i = 0; i < 4; i++) t.push_back(0);
    vector<uint8_t> seg = {0xFF, 0xE1};
    int len = (int)t.size() + 8;
    seg.push_back(len >> 8); seg.push_back(len & 255);
    for (char c : std::string("Exif\0\0", 6)) seg.push_back((uint8_t)c);
    seg.insert(seg.end(), t.begin(), t.end());
    vector<uint8_t> out(jpg.begin(), jpg.begin() + 2);
    out.insert(out.end(), seg.begin(), seg.end());
    out.insert(out.end(), jpg.begin() + 2, jpg.end());
    return out;
}

static void test_orientation() {
    // 3x2 source a b c / d e f
    auto src = [] { Image a(3, 2); for (int i = 0; i < 6; i++) a.px[i] = {uint8_t(i * 40 + 10), uint8_t(i), 0}; return a; };
    // expected, as source indices
    const int exp[9][6] = {{}, {0, 1, 2, 3, 4, 5}, {2, 1, 0, 5, 4, 3}, {5, 4, 3, 2, 1, 0}, {3, 4, 5, 0, 1, 2},
                           {0, 3, 1, 4, 2, 5}, {3, 0, 4, 1, 5, 2}, {5, 2, 4, 1, 3, 0}, {2, 5, 1, 4, 0, 3}};
    for (int o = 1; o <= 8; o++) {
        Image a = src();
        apply_orientation(a, o);
        CHECK(a.w == (o >= 5 ? 2 : 3) && a.h == (o >= 5 ? 3 : 2));
        bool ok = a.px.size() == 6;
        for (int i = 0; ok && i < 6; i++) ok = a.px[i].g == exp[o][i];
        CHECK(ok);
    }
    Image z = src();
    apply_orientation(z, 0); apply_orientation(z, 9);
    CHECK(z.w == 3 && z.px == src().px);
    // parser is bounds-safe on truncated/garbage input
    uint8_t junk[] = {0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x20, 'E', 'x', 'i', 'f'};
    CHECK(jpeg_orientation(junk, sizeof junk) == 1 && jpeg_orientation(junk, 3) == 1 && jpeg_orientation(nullptr, 0) == 1);
    // both decoders rotate
    Image base(16, 8);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) base.at(x, y) = x < 8 ? Rgb{230, 20, 20} : Rgb{20, 20, 230};
    vector<uint8_t> jpg;
    stbi_write_jpg_to_func([](void* c, void* d, int n) { auto* v = (vector<uint8_t>*)c; v->insert(v->end(), (uint8_t*)d, (uint8_t*)d + n); }, &jpg, 16, 8, 3, base.px.data(), 100);
    std::string dir = "/Volumes/RAMDisk/claude-tmp/claude-501/-Users-gunalanr-Desktop-projects-lowpoly-cpp/ea545b02-7ef7-4188-8d26-2fb61149679d/scratchpad/";
    if (!std::filesystem::exists(dir)) dir = std::filesystem::temp_directory_path().string() + "/";
    std::string path = dir + "lowpoly-test-exif.jpg", err;
    for (int le = 0; le < 2; le++) for (int o : {1, 6, 8}) {
        auto b = with_exif(jpg, le, o);
        CHECK(jpeg_orientation(b.data(), b.size()) == o);
        FILE* f = open_file(path, "wb");
        CHECK(f != nullptr);
        if (!f) continue;
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
        Image s, l;
        CHECK(stb_decode(path, s, err) && load_image(path, l, err));
        for (Image* m : {&s, &l}) {
            CHECK(m->w == (o == 1 ? 16 : 8) && m->h == (o == 1 ? 8 : 16));
            if (m->w != 8 && m->w != 16) continue;
            // red left half goes on top for cw, bottom for ccw
            Rgb top = m->at(0, 0), bot = m->at(m->w - 1, m->h - 1);
            bool red_first = o == 1 || o == 6;
            CHECK((top.r > 150 && top.b < 100 && bot.b > 150) == red_first);
            CHECK((top.b > 150 && top.r < 100 && bot.r > 150) == !red_first);
        }
    }
    std::remove(path.c_str());
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

    Params c; c.points = 2000; c.levels = 30; c.uniform = 0;
    auto cp = sample_points(mask_with(400, 400, [](int x, int) { return x < 100; }), 400, 400, c);
    int left = 0;
    for (auto q : cp) left += q.x < 100;
    CHECK(left > (int)(cp.size() * 0.6));

    Params e; e.points = 100;
    CHECK(sample_points(mask_with(64, 64, [](int, int) { return false; }), 64, 64, e).size() >= 100);

    Params d; d.points = 300;
    auto m = mask_with(120, 90, [](int x, int y) { return (x * y) % 7 == 0; });
    CHECK(sample_points(m, 120, 90, d) == sample_points(m, 120, 90, d));

    // side seeds
    {
        int W = 130, H = 97;
        Params a; a.points = 300; a.levels = 20;
        auto m = mask_with(W, H, [](int x, int y) { return (x * 7 + y * 3) % 23 == 0; });
        auto s1 = sample_points(m, W, H, a);
        vector<Pt> t = s1; add_side_seeds(t, W, H);
        vector<Pt> t2 = s1; add_side_seeds(t2, W, H);
        CHECK(t == t2 && t.size() > s1.size());
        CHECK(std::equal(s1.begin(), s1.end(), t.begin()));
        for (Pt c : {Pt{0, 0}, Pt{0, H - 1}, Pt{W - 1, 0}, Pt{W - 1, H - 1}}) CHECK(std::find(t.begin(), t.end(), c) != t.end());
        bool proj = true;
        for (size_t i = s1.size(); i < t.size(); i++) {
            Pt q = t[i];
            proj &= q.x == 0 || q.y == 0 || q.x == W - 1 || q.y == H - 1;
            proj &= std::any_of(s1.begin(), s1.end(), [&](Pt s) { return (q.y == 0 || q.y == H - 1) ? s.x == q.x : s.y == q.y; });
        }
        CHECK(proj);
        vector<Pt> u = t; std::sort(u.begin(), u.end());
        CHECK(std::adjacent_find(u.begin(), u.end()) == u.end());
        vector<uint32_t> own;
        voronoi_grid(W, H, t, build_seed_grid(W, H, t), own);
        int64_t area = 0;
        for (auto& q : extract_polygons(own, W, H, t)) area += signed_area2(q.v, q.n);
        CHECK(area == 2ll * (W - 1) * (H - 1));
    }
}

static void test_refine() {
    // disc, new seeds near the rim, error below uniform
    int w = 160, h = 120;
    Image img(w, h, {20, 40, 200});
    auto in_disc = [](int x, int y) { return (x - 80) * (x - 80) + (y - 60) * (y - 60) < 35 * 35; };
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (in_disc(x, y)) img.at(x, y) = {230, 180, 20};
    auto rim = mask_with(w, h, [&](int x, int y) { return in_disc(x, y) != in_disc(x + 1, y) || in_disc(x, y) != in_disc(x, y + 1); });
    Params p; p.points = 40; p.uniform = 1;
    auto base = sample_points(rim, w, h, p);
    add_side_seeds(base, w, h);
    auto seeds = base;
    refine_seeds(img, seeds, base.size() + 200);
    // two flat colours can run out of splittable error
    size_t added = seeds.size() - base.size();
    CHECK(added >= 100 && added <= 200);
    CHECK(std::equal(base.begin(), base.end(), seeds.begin()));
    vector<Pt> s = seeds; std::sort(s.begin(), s.end());
    CHECK(std::adjacent_find(s.begin(), s.end()) == s.end());
    int near = 0;
    for (size_t i = base.size(); i < seeds.size(); i++) {
        double r = std::hypot(seeds[i].x - 80.0, seeds[i].y - 60.0);
        near += std::fabs(r - 35) <= 4;
    }
    CHECK(near * 10 > (int)added * 8);
    auto again = base;
    refine_seeds(img, again, base.size() + 200);
    CHECK(again == seeds);

    auto sse = [&](const vector<Pt>& sd) {
        vector<uint32_t> owner;
        voronoi_grid(w, h, sd, build_seed_grid(w, h, sd), owner);
        auto polys = extract_polygons(owner, w, h, sd);
        color_polygons(img, polys, ColorMode::Mean);
        Image out(w, h);
        rasterize(out, polys);
        uint64_t e = 0;
        for (size_t i = 0; i < img.size(); i++) {
            int dr = img.px[i].r - out.px[i].r, dg = img.px[i].g - out.px[i].g, db = img.px[i].b - out.px[i].b;
            e += dr * dr + dg * dg + db * db;
        }
        return e;
    };
    Params u = p; u.points = 240;
    CHECK(sse(seeds) * 2 < sse(sample_points(rim, w, h, u)));
}

static void test_flip() {
    // diagonal edge, flips lower the error, tiling stays exact
    int w = 120, h = 90;
    Image img(w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) img.at(x, y) = 3 * x + 2 * y < 240 ? Rgb{250, 40, 30} : Rgb{20, 60, 220};
    vector<Pt> s = random_seeds(150, w, h);
    for (Pt c : {Pt{0, 0}, Pt{w - 1, 0}, Pt{0, h - 1}, Pt{w - 1, h - 1}}) s.push_back(c);
    std::sort(s.begin(), s.end()); s.erase(std::unique(s.begin(), s.end()), s.end());
    add_side_seeds(s, w, h);
    vector<uint32_t> owner;
    voronoi_grid(w, h, s, build_seed_grid(w, h, s), owner);
    auto base = extract_polygons(owner, w, h, s);
    auto sse = [&](vector<Polygon> ps) {
        color_polygons(img, ps, ColorMode::Mean);
        Image out(w, h);
        rasterize(out, ps);
        uint64_t e = 0;
        for (size_t i = 0; i < img.size(); i++) {
            int dr = img.px[i].r - out.px[i].r, dg = img.px[i].g - out.px[i].g, db = img.px[i].b - out.px[i].b;
            e += dr * dr + dg * dg + db * db;
        }
        return e;
    };
    auto f = base;
    int n = flip_edges(img, s, f);
    CHECK(n > 0 && f.size() == base.size());
    CHECK(sse(f) < sse(base));
    int64_t area = 0;
    vector<int> own(w * h, 0);
    for (auto& q : f) { area += signed_area2(q.v, q.n); walk(edges_of(q, w, h), w, 0, h, [&](size_t k) { own[k]++; }); }
    CHECK(area == 2ll * (w - 1) * (h - 1));
    CHECK(std::all_of(own.begin(), own.end(), [](int c) { return c == 1; }));
    auto g = base;
    pool_init(1);
    flip_edges(img, s, g);
    pool_init(0);
    bool same = g.size() == f.size();
    for (size_t i = 0; same && i < f.size(); i++) same = memcmp(f[i].s, g[i].s, sizeof f[i].s) == 0 && f[i].n == g[i].n;
    CHECK(same);

    // relaxation, lower error, exact tiling, thread-independent
    auto rs1 = s; auto rf = f;
    CHECK(relax_vertices(img, rs1, rf, 3) > 0);
    CHECK(sse(rf) < sse(f));
    area = 0;
    std::fill(own.begin(), own.end(), 0);
    bool vs = true;
    for (auto& q : rf) {
        area += signed_area2(q.v, q.n);
        walk(edges_of(q, w, h), w, 0, h, [&](size_t k) { own[k]++; });
        for (int k = 0; k < q.n; k++) vs &= q.v[k] == rs1[q.s[k]];
    }
    CHECK(vs && area == 2ll * (w - 1) * (h - 1));
    CHECK(std::all_of(own.begin(), own.end(), [](int c) { return c == 1; }));
    auto rs2 = s; auto rg = f;
    pool_init(1);
    relax_vertices(img, rs2, rg, 3);
    pool_init(0);
    CHECK(rs1 == rs2);
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

    // edge-hugging seeds still tile exactly
    for (int trial = 0; trial < 4; trial++) {
        vector<Pt> r = random_seeds(150, w, h);
        for (int i = 0; i < 60; i++) {
            int t = (int)trand(4), u = (int)trand(t < 2 ? w : h), d = 1 + (int)trand(3);
            r.push_back(t == 0 ? Pt{u, d} : t == 1 ? Pt{u, h - 1 - d} : t == 2 ? Pt{d, u} : Pt{w - 1 - d, u});
        }
        for (int x = 0; x < w; x += 16) { r.push_back({x, 0}); r.push_back({x, h - 1}); }
        for (int y = 0; y < h; y += 16) { r.push_back({0, y}); r.push_back({w - 1, y}); }
        r.push_back({w - 1, h - 1});
        std::sort(r.begin(), r.end()); r.erase(std::unique(r.begin(), r.end()), r.end());
        voronoi_grid(w, h, r, build_seed_grid(w, h, r), owner);
        auto ps = extract_polygons(owner, w, h, r);
        for (auto& q : ps) q.color = {255, 255, 255};
        Image cov(w, h);
        rasterize(cov, ps);
        int holes = 0;
        for (auto p : cov.px) holes += p.r == 0;
        CHECK(holes == 0);
        // with the fill rule every pixel has exactly one owner
        vector<int> own(w * h, 0);
        for (auto& q : ps) walk(edges_of(q, w, h), w, 0, h, [&](size_t k) { own[k]++; });
        CHECK(std::all_of(own.begin(), own.end(), [](int c) { return c == 1; }));
        // areas add up to the rectangle
        int64_t area = 0;
        for (auto& q : ps) area += signed_area2(q.v, q.n);
        CHECK(area == 2ll * (w - 1) * (h - 1));
    }

    // square lattice, every junction cocircular
    for (int step : {3, 7, 10}) {
        vector<Pt> r;
        for (int y = 0; y < h; y += step) for (int x = 0; x < w; x += step) r.push_back({x, y});
        for (int x = 0; x < w; x++) { r.push_back({x, 0}); r.push_back({x, h - 1}); }
        for (int y = 0; y < h; y++) { r.push_back({0, y}); r.push_back({w - 1, y}); }
        std::sort(r.begin(), r.end()); r.erase(std::unique(r.begin(), r.end()), r.end());
        voronoi_grid(w, h, r, build_seed_grid(w, h, r), owner);
        int64_t area = 0;
        for (auto& q : extract_polygons(owner, w, h, r)) area += signed_area2(q.v, q.n);
        CHECK(area == 2ll * (w - 1) * (h - 1));
    }
}

static void test_raster() {
    Image img(10, 10);
    rasterize(img, {mkpoly({{2,2},{6,2},{6,6},{2,6}}, {255,255,255})});
    int n = 0; bool inside_ok = true;
    for (int y = 0; y < 10; y++) for (int x = 0; x < 10; x++) if (img.at(x, y).r == 255) { n++; inside_ok &= x >= 2 && x <= 6 && y >= 2 && y <= 6; }
    // fill rule keeps left and top edges only
    CHECK(n == 16 && inside_ok);

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
    CHECK(painted == w * h);
}

static vector<uint8_t> slurp(const std::string& p) {
    vector<uint8_t> v;
    if (FILE* f = open_file(p, "rb")) { uint8_t b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f))) v.insert(v.end(), b, b + n); fclose(f); }
    return v;
}

static void test_png() {
    std::error_code tec;
    std::string p = (std::filesystem::temp_directory_path(tec) / "lowpoly-test-png.png").string();
    auto rt = [&](const Image& a) { // encode, stb decode, compare
        if (!write_png(p, a)) return false;
        int w, h, c;
        uint8_t* d = stbi_load(p.c_str(), &w, &h, &c, 3);
        bool ok = d && w == a.w && h == a.h && !memcmp(d, a.px.data(), a.size() * 3);
        stbi_image_free(d);
        return ok;
    };
    Image noise(101, 97), flat(64, 70, {12, 34, 56}), mesh(300, 211, {200, 200, 200});
    for (auto& q : noise.px) q = {uint8_t(trand(256)), uint8_t(trand(256)), uint8_t(trand(256))};
    vector<Polygon> ps;
    for (int i = 0; i < 200; i++) {
        auto q = mkpoly({{(int)trand(300),(int)trand(211)},{(int)trand(300),(int)trand(211)},{(int)trand(300),(int)trand(211)}}, {uint8_t(trand(256)), uint8_t(i), uint8_t(trand(256))});
        if (q.n >= 3) ps.push_back(q);
    }
    rasterize(mesh, ps);
    CHECK(rt(noise) && rt(flat) && rt(mesh));
    for (int s : {1, 2, 3, 31, 32, 33, 64, 65, 257, 1000}) { // odd sizes, band edges, long runs
        Image a(s, 1, {5, 5, 5}), b(1, s, {9, 9, 9}), c(s, 33);
        for (auto& q : c.px) q = trand(3) ? Rgb{1, 2, 3} : Rgb{uint8_t(trand(256)), 0, 7};
        CHECK(rt(a) && rt(b) && rt(c));
    }
    Image odd(1237, 71);
    for (int y = 0; y < odd.h; y++) for (int x = 0; x < odd.w; x++) odd.at(x, y) = {uint8_t(x / 40), uint8_t(y / 9), uint8_t(x % 3 ? 7 : 8)};
    CHECK(rt(odd));
    // stride + 1 > window
    size_t keep = png_max_dist;
    png_max_dist = 1000;
    CHECK(rt(odd) && rt(mesh));
    png_max_dist = keep;
    Image wide(10923, 40, {3, 3, 3}); // stride + 1 > 32768
    for (int y = 0; y < wide.h; y++) for (int x = 0; x < wide.w; x++) if ((x ^ y) % 97 == 0) wide.at(x, y) = {uint8_t(x), uint8_t(y), 1};
    CHECK(rt(wide));
    // bytes must not depend on the thread count
    CHECK(write_png(p, mesh));
    auto b0 = slurp(p);
    pool_init(1);
    CHECK(write_png(p, mesh));
    auto b1 = slurp(p);
    pool_init(0);
    CHECK(!b0.empty() && b0 == b1);
    CHECK(!write_png(p, Image()));
    std::filesystem::remove(p, tec);
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
    // thin edge tiles, few seeds
    for (auto [tw, th, n] : {std::tuple{329, 241, 30}, std::tuple{241, 329, 7}, std::tuple{17, 9, 3}, std::tuple{331, 247, 5000}}) {
        g->upload(Image(tw, th));
        auto ts = random_seeds(n, tw, th);
        auto tg = build_seed_grid(tw, th, ts);
        vector<uint32_t> tc, tgo;
        voronoi_grid(tw, th, ts, tg, tc);
        g->voronoi(ts, tg, tgo);
        CHECK(tc == tgo);
    }
    g->upload(img);
    g->voronoi(s, grid, og);   // the raster reads the owner map left on the gpu
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

static void test_scaled() {
    int w = 90, h = 60;
    vector<Pt> seeds;
    for (int i = 0; i < 60; i++) seeds.push_back({(int)trand(w), (int)trand(h)});
    for (int x = 0; x < w; x += 15) { seeds.push_back({x, 0}); seeds.push_back({x, h - 1}); }
    for (int y = 0; y < h; y += 15) { seeds.push_back({0, y}); seeds.push_back({w - 1, y}); }
    seeds.push_back({w - 1, 0}); seeds.push_back({w - 1, h - 1});
    std::sort(seeds.begin(), seeds.end());
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());
    vector<uint32_t> owner;
    voronoi_grid(w, h, seeds, build_seed_grid(w, h, seeds), owner);
    auto ps = extract_polygons(owner, w, h, seeds);
    for (size_t i = 0; i < ps.size(); i++) ps[i].color = {uint8_t(i % 200), uint8_t(i / 200 + 1), 7};
    Rgb bg{250, 0, 250};
    for (int aa = 1; aa <= 2; aa++) {
        Image s = render_scaled(ps, w, h, 3 * w, 3 * h, aa, bg);
        bool full = s.w == 3 * w && s.h == 3 * h;
        for (auto p : s.px) full &= p != bg;
        CHECK(full);
    }
    Image half = render_scaled(ps, w, h, w / 2, h / 2, 1, bg);
    bool hp = half.w == w / 2 && half.h == h / 2;
    for (auto p : half.px) hp &= p != bg;
    CHECK(hp);
    Image plain(w, h, bg);
    rasterize(plain, ps);
    CHECK(render_scaled(ps, w, h, w, h, 1, bg).px == plain.px);
    std::string sv = svg_string(ps, w, h, 3 * w, 3 * h);
    size_t n = 0;
    for (size_t at = sv.find("<path"); at != std::string::npos; at = sv.find("<path", at + 1)) n++;
    CHECK(n == ps.size());
    CHECK(sv.find("width=\"270\" height=\"180\"") != std::string::npos && sv.find("viewBox=\"0 0 180 120\"") != std::string::npos);
    // integer coordinates
    CHECK(sv.find('.', sv.find("<path")) == std::string::npos);
}

int main() {
    pool_init(0);
    test_pool();
    test_ppm_roundtrip();
    test_orientation();
    test_canny();
    test_sampling();
    test_refine();
    test_voronoi();
    test_hull();
    test_extract();
    test_flip();
    test_raster();
    test_png();
    test_scaled();
    test_gpu();
    if (g_fail) { fprintf(stderr, "%d failures\n", g_fail); return 1; }
    puts("ok");
    return 0;
}
#endif
