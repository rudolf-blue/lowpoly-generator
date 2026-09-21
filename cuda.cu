#include "lowpoly.h"
#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>

struct Params32 { uint32_t w, h; int32_t low2, high2; uint32_t g, gw, gh, npoly; };
struct PolyGpu { int2 v[4]; uint32_t n, pad[3]; };

#define PIX \
    int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y; \
    if (x >= (int)P.w || y >= (int)P.h) return;

__global__ void k_luma_blur_h(const uint8_t* rgb, uint16_t* tmp, Params32 P) {
    PIX
    const int K[5] = {1, 4, 6, 4, 1};
    int row = y * P.w, acc = 0;
    for (int t = 0; t < 5; t++) {
        int xx = min(max(x + t - 2, 0), (int)P.w - 1);
        int i = (row + xx) * 3;
        acc += K[t] * ((77 * rgb[i] + 150 * rgb[i + 1] + 29 * rgb[i + 2]) >> 8);
    }
    tmp[row + x] = (uint16_t)acc;
}

__global__ void k_blur_v(const uint16_t* tmp, uint8_t* blur, Params32 P) {
    PIX
    const int K[5] = {1, 4, 6, 4, 1};
    int acc = 0;
    for (int t = 0; t < 5; t++) {
        int yy = min(max(y + t - 2, 0), (int)P.h - 1);
        acc += K[t] * tmp[yy * P.w + x];
    }
    blur[y * P.w + x] = (uint8_t)(acc >> 8);
}

__global__ void k_sobel(const uint8_t* blur, int* mag, uint8_t* dir, Params32 P) {
    PIX
    int w = P.w, h = P.h;
    int xm = max(x - 1, 0), xp = min(x + 1, w - 1), ym = max(y - 1, 0), yp = min(y + 1, h - 1);
    const uint8_t* r0 = blur + ym * w;
    const uint8_t* r1 = blur + y * w;
    const uint8_t* r2 = blur + yp * w;
    int dx = (r0[xp] + 2 * r1[xp] + r2[xp]) - (r0[xm] + 2 * r1[xm] + r2[xm]);
    int dy = (r2[xm] + 2 * r2[x] + r2[xp]) - (r0[xm] + 2 * r0[x] + r0[xp]);
    int adx = abs(dx), ady = abs(dy);
    mag[y * w + x] = dx * dx + dy * dy;
    dir[y * w + x] = ady * 1000 <= adx * 414 ? 0 : ady * 1000 >= adx * 2414 ? 2 : ((dx ^ dy) >= 0 ? 1 : 3);
}

__global__ void k_nms_threshold(const int* mag, const uint8_t* dir, uint8_t* cls, Params32 P) {
    PIX
    int w = P.w, h = P.h, i = y * w + x, v = 0;
    if (x > 0 && y > 0 && x < w - 1 && y < h - 1) {
        int m = mag[i], a, b;
        switch (dir[i]) {
            case 0: a = mag[i - 1]; b = mag[i + 1]; break;
            case 1: a = mag[i - w - 1]; b = mag[i + w + 1]; break;
            case 2: a = mag[i - w]; b = mag[i + w]; break;
            default: a = mag[i - w + 1]; b = mag[i + w - 1]; break;
        }
        if (m >= a && m >= b) v = m;
    }
    cls[i] = v >= P.high2 ? 2 : v >= P.low2 ? 1 : 0;
}

__global__ void k_voronoi(const int2* seeds, const uint32_t* off, const uint32_t* idx, uint32_t* owner, Params32 P) {
    PIX
    int g = P.g, gw = P.gw, gh = P.gh, cx = x / g, cy = y / g;
    uint32_t best = 0xFFFFFFFFu;
    int bd = 0x7FFFFFFF, rmax = max(gw, gh);
    for (int r = 0; r <= rmax; r++) {
        if (r > 0) {
            int lim = (r - 1) * g + 1;
            if (best != 0xFFFFFFFFu && bd < lim * lim) break;
        }
        for (int by = cy - r; by <= cy + r; by++) {
            if (by < 0 || by >= gh) continue;
            int step = (by == cy - r || by == cy + r) ? 1 : 2 * r;
            for (int bx = cx - r; bx <= cx + r; bx += step) {
                if (bx >= 0 && bx < gw) {
                    uint32_t b = by * gw + bx;
                    for (uint32_t k = off[b]; k < off[b + 1]; k++) {
                        uint32_t i = idx[k];
                        int2 s = seeds[i];
                        int dx = x - s.x, dy = y - s.y, d = dx * dx + dy * dy;
                        if (d < bd || (d == bd && i < best)) { bd = d; best = i; }
                    }
                }
                if (r == 0) break;
            }
        }
    }
    owner[y * P.w + x] = best;
}

__global__ void k_raster_find(const uint8_t* rgb, const uint32_t* owner, const uint32_t* off, const uint32_t* idx,
                              const PolyGpu* polys, uint32_t* pid, unsigned int* sums, Params32 P) {
    PIX
    uint32_t k = y * P.w + x, s = owner[k], found = 0xFFFFFFFFu;
    for (uint32_t j = off[s]; j < off[s + 1] && found == 0xFFFFFFFFu; j++) {
        PolyGpu q = polys[idx[j]];
        bool in = true;
        for (uint32_t i = 0; i < q.n; i++) {
            int2 u = q.v[i], v = q.v[(i + 1) % q.n];
            if ((v.x - u.x) * (y - u.y) - (v.y - u.y) * (x - u.x) < 0) { in = false; break; }
        }
        if (in) found = idx[j];
    }
    pid[k] = found;
    if (found == 0xFFFFFFFFu) return;
    atomicAdd(&sums[found * 4 + 0], (unsigned)rgb[k * 3]);
    atomicAdd(&sums[found * 4 + 1], (unsigned)rgb[k * 3 + 1]);
    atomicAdd(&sums[found * 4 + 2], (unsigned)rgb[k * 3 + 2]);
    atomicAdd(&sums[found * 4 + 3], 1u);
}

__global__ void k_poly_colors(const unsigned int* sums, const PolyGpu* polys, const uint8_t* rgb, uint8_t* colors, Params32 P) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= P.npoly) return;
    unsigned n = sums[i * 4 + 3];
    if (n) for (int c = 0; c < 3; c++) colors[i * 3 + c] = (uint8_t)(sums[i * 4 + c] / n);
    else {
        uint32_t k = (polys[i].v[0].y * P.w + polys[i].v[0].x) * 3;
        for (int c = 0; c < 3; c++) colors[i * 3 + c] = rgb[k + c];
    }
}

__global__ void k_raster_paint(const uint32_t* pid, const uint8_t* colors, uint8_t* out, Params32 P) {
    PIX
    uint32_t k = y * P.w + x, p = pid[k], c = p == 0xFFFFFFFFu ? P.npoly : p;
    out[k * 3] = colors[c * 3]; out[k * 3 + 1] = colors[c * 3 + 1]; out[k * 3 + 2] = colors[c * 3 + 2];
}

namespace {
struct CudaGpu : Gpu {
    std::string dev_name;
    int w = 0, h = 0;
    uint8_t *rgb = nullptr, *blur = nullptr, *dir = nullptr, *cls = nullptr, *out = nullptr, *colors = nullptr;
    uint16_t* tmp = nullptr;
    int* mag = nullptr;
    uint32_t *owner = nullptr, *pid = nullptr, *d_off = nullptr, *d_idx = nullptr;
    int2* seeds = nullptr;
    PolyGpu* polys = nullptr;
    unsigned int* sums = nullptr;
    dim3 block{16, 16};

    bool init() {
        int n = 0;
        if (cudaGetDeviceCount(&n) != cudaSuccess || n == 0) return false;
        cudaDeviceProp p;
        cudaGetDeviceProperties(&p, 0);
        dev_name = p.name;
        cudaFree(0);
        return true;
    }
    ~CudaGpu() override {
        for (void* p : {(void*)rgb, (void*)blur, (void*)dir, (void*)cls, (void*)out, (void*)colors, (void*)tmp, (void*)mag,
                        (void*)owner, (void*)pid, (void*)d_off, (void*)d_idx, (void*)seeds, (void*)polys, (void*)sums})
            if (p) cudaFree(p);
    }
    const char* name() override { return dev_name.c_str(); }
    dim3 grid() const { return dim3((w + 15) / 16, (h + 15) / 16); }

    template <class T> static T* dev_alloc(size_t n) { T* p; cudaMalloc(&p, std::max<size_t>(n, 1) * sizeof(T)); return p; }
    template <class T> static T* dev_copy(const T* src, size_t n) { T* p = dev_alloc<T>(n); if (n) cudaMemcpy(p, src, n * sizeof(T), cudaMemcpyHostToDevice); return p; }

    void upload(const Image& img) override {
        w = img.w; h = img.h;
        size_t n = img.size();
        rgb = dev_copy((const uint8_t*)img.px.data(), n * 3);
        tmp = dev_alloc<uint16_t>(n); blur = dev_alloc<uint8_t>(n); mag = dev_alloc<int>(n); dir = dev_alloc<uint8_t>(n);
        cls = dev_alloc<uint8_t>(n); owner = dev_alloc<uint32_t>(n); pid = dev_alloc<uint32_t>(n); out = dev_alloc<uint8_t>(n * 3);
    }

    void canny_front(int low2, int high2, std::vector<uint8_t>& c) override {
        Params32 P{(uint32_t)w, (uint32_t)h, low2, high2, 0, 0, 0, 0};
        k_luma_blur_h<<<grid(), block>>>(rgb, tmp, P);
        k_blur_v<<<grid(), block>>>(tmp, blur, P);
        k_sobel<<<grid(), block>>>(blur, mag, dir, P);
        k_nms_threshold<<<grid(), block>>>(mag, dir, cls, P);
        c.resize((size_t)w * h);
        cudaMemcpy(c.data(), cls, c.size(), cudaMemcpyDeviceToHost);
    }

    void voronoi(const std::vector<Pt>& s, const SeedGrid& gr, std::vector<uint32_t>& o) override {
        Params32 P{(uint32_t)w, (uint32_t)h, 0, 0, (uint32_t)gr.g, (uint32_t)gr.gw, (uint32_t)gr.gh, 0};
        if (seeds) cudaFree(seeds);
        if (d_off) cudaFree(d_off);
        if (d_idx) cudaFree(d_idx);
        seeds = dev_copy((const int2*)s.data(), s.size());
        d_off = dev_copy(gr.off.data(), gr.off.size());
        d_idx = dev_copy(gr.idx.data(), gr.idx.size());
        k_voronoi<<<grid(), block>>>(seeds, d_off, d_idx, owner, P);
        o.resize((size_t)w * h);
        cudaMemcpy(o.data(), owner, o.size() * 4, cudaMemcpyDeviceToHost);
    }

    void raster(std::vector<Polygon>& ps, const std::vector<uint32_t>& csr_off, const std::vector<uint32_t>& csr_idx,
                Rgb background, Image& img) override {
        size_t np = ps.size();
        std::vector<PolyGpu> pg(np);
        for (size_t i = 0; i < np; i++) {
            pg[i].n = ps[i].n;
            for (int j = 0; j < 4; j++) pg[i].v[j] = make_int2(ps[i].v[j].x, ps[i].v[j].y);
        }
        if (polys) cudaFree(polys);
        if (sums) cudaFree(sums);
        if (colors) cudaFree(colors);
        polys = dev_copy(pg.data(), np);
        uint32_t* coff = dev_copy(csr_off.data(), csr_off.size());
        uint32_t* cidx = dev_copy(csr_idx.data(), csr_idx.size());
        sums = dev_alloc<unsigned int>(np * 4);
        cudaMemset(sums, 0, np * 16);   // 32-bit channel sums; fine below 16M px per polygon
        std::vector<uint8_t> col((np + 1) * 3);
        col[np * 3] = background.r; col[np * 3 + 1] = background.g; col[np * 3 + 2] = background.b;
        colors = dev_copy(col.data(), col.size());
        Params32 P{(uint32_t)w, (uint32_t)h, 0, 0, 0, 0, 0, (uint32_t)np};
        k_raster_find<<<grid(), block>>>(rgb, owner, coff, cidx, polys, pid, sums, P);
        k_poly_colors<<<(unsigned)((np + 63) / 64), 64>>>(sums, polys, rgb, colors, P);
        k_raster_paint<<<grid(), block>>>(pid, colors, out, P);
        cudaMemcpy(col.data(), colors, np * 3, cudaMemcpyDeviceToHost);
        for (size_t i = 0; i < np; i++) ps[i].color = {col[i * 3], col[i * 3 + 1], col[i * 3 + 2]};
        img = Image(w, h);
        cudaMemcpy(img.px.data(), out, img.size() * 3, cudaMemcpyDeviceToHost);
        cudaFree(coff);
        cudaFree(cidx);
    }
};
}

Gpu* gpu_create() {
    CudaGpu* g = new CudaGpu;
    if (g->init()) return g;
    delete g;
    return nullptr;
}
