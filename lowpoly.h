#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

constexpr int MAX_DIM = 16384;

struct Rgb { uint8_t r, g, b; };
inline bool operator==(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
inline bool operator!=(Rgb a, Rgb b) { return !(a == b); }

struct Pt { int32_t x, y; };
inline bool operator==(Pt a, Pt b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(Pt a, Pt b) { return !(a == b); }
inline bool operator<(Pt a, Pt b) { return a.x != b.x ? a.x < b.x : a.y < b.y; }

struct Image {
    int w = 0, h = 0;
    std::vector<Rgb> px;
    Image() = default;
    Image(int w, int h, Rgb c = {0, 0, 0}) : w(w), h(h), px((size_t)w * h, c) {}
    Rgb& at(int x, int y) { return px[(size_t)y * w + x]; }
    Rgb at(int x, int y) const { return px[(size_t)y * w + x]; }
    size_t size() const { return (size_t)w * h; }
};

// vertices counter-clockwise (signed_area2 > 0); s[i] is the seed index of v[i]
struct Polygon {
    Pt v[4];
    uint32_t s[4];
    uint8_t n;
    Rgb color;
};

enum class ColorMode { Mean, Corner };
enum class VoronoiMode { Grid, Jfa, Brute };
enum class Backend { Auto, Cpu, Gpu };

struct Params {
    std::string in, out, text, stages = "out";
    int points = 10000, levels = 150, border = 16, threads = 0;
    double uniform = 0.08;
    uint64_t seed = 24301;
    float canny_low = 5.f, canny_high = 25.f;
    bool auto_canny = false, no_stages = false, hull = false, bench = false, quiet = false;
    bool canny_float = false, extract_sort = false, raster_owner = false;
    ColorMode color = ColorMode::Mean;
    VoronoiMode voronoi = VoronoiMode::Grid;
    Backend backend = Backend::Auto;
};

void pool_init(int threads);
int pool_size();
void parallel_for(int n, const std::function<void(int, int)>& fn);

bool load_image(const std::string& path, Image& img, std::string& err);
bool save_image(const std::string& path, const Image& img, std::string& err);
bool platform_decode(const std::string& path, Image& img);
bool platform_encode(const std::string& path, const Image& img);

std::vector<uint8_t> to_luma(const Image& img);
// cls: 0 none, 1 weak, 2 strong; low2/high2 are squared thresholds
void canny_classify(const std::vector<uint8_t>& luma, int w, int h, int low2, int high2, bool auto_thr,
                    std::vector<uint8_t>& cls);
void hysteresis(std::vector<uint8_t>& cls, int w, int h);   // in place, cls becomes a 0/255 mask
std::vector<Pt> sample_points(const std::vector<uint8_t>& mask, int w, int h, const Params& p);

struct SeedGrid {
    int g = 0, gw = 0, gh = 0;
    std::vector<uint32_t> off;   // gw*gh+1
    std::vector<uint32_t> idx;   // seed indices, bin-major, ascending within a bin
};
SeedGrid build_seed_grid(int w, int h, const std::vector<Pt>& seeds);
void voronoi_grid(int w, int h, const std::vector<Pt>& seeds, const SeedGrid& grid, std::vector<uint32_t>& owner);
void voronoi_jfa(int w, int h, const std::vector<Pt>& seeds, std::vector<uint32_t>& owner);
void voronoi_brute(int w, int h, const std::vector<Pt>& seeds, std::vector<uint32_t>& owner);

int64_t cross(Pt o, Pt a, Pt b);
int64_t signed_area2(const Pt* v, int n);
std::vector<Pt> hull_monotone(std::vector<Pt> pts);
std::vector<Pt> hull_quick(const std::vector<Pt>& pts);

std::vector<Polygon> extract_polygons(const std::vector<uint32_t>& owner, int w, int h, const std::vector<Pt>& seeds);
void build_csr(const std::vector<Polygon>& polys, size_t nseeds, std::vector<uint32_t>& off, std::vector<uint32_t>& idx);

void color_polygons(const Image& src, std::vector<Polygon>& polys, ColorMode mode);
void rasterize(Image& dst, const std::vector<Polygon>& polys);
void rasterize_by_owner(const Image& src, const std::vector<uint32_t>& owner, const std::vector<uint32_t>& csr_off,
                        const std::vector<uint32_t>& csr_idx, std::vector<Polygon>& polys, Rgb background, Image& dst);

void draw_line(Image& img, int x0, int y0, int x1, int y1, Rgb c);
void draw_polyline(Image& img, const std::vector<Pt>& pts, Rgb c, bool closed);

struct Gpu {
    virtual ~Gpu() = default;
    virtual const char* name() = 0;
    virtual void upload(const Image& img) = 0;
    virtual void canny_front(int low2, int high2, std::vector<uint8_t>& cls) = 0;
    virtual void voronoi(const std::vector<Pt>& seeds, const SeedGrid& grid, std::vector<uint32_t>& owner) = 0;
    virtual void raster(std::vector<Polygon>& polys, const std::vector<uint32_t>& csr_off,
                        const std::vector<uint32_t>& csr_idx, Rgb background, Image& out) = 0;
};
Gpu* gpu_create();
