#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include "lowpoly.h"
#include <cstring>

static const char* kSrc = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Params { uint w, h; int low2, high2; uint g, gw, gh, npoly; };

kernel void luma_blur_h(device const uchar* rgb [[buffer(0)]], device ushort* tmp [[buffer(1)]],
                        constant Params& P [[buffer(2)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= P.w || id.y >= P.h) return;
    const int K[5] = {1, 4, 6, 4, 1};
    uint row = id.y * P.w;
    int acc = 0;
    for (int t = 0; t < 5; t++) {
        int x = clamp((int)id.x + t - 2, 0, (int)P.w - 1);
        uint i = (row + x) * 3;
        acc += K[t] * ((77 * rgb[i] + 150 * rgb[i + 1] + 29 * rgb[i + 2]) >> 8);
    }
    tmp[row + id.x] = (ushort)acc;
}

kernel void blur_v(device const ushort* tmp [[buffer(0)]], device uchar* blur [[buffer(1)]],
                   constant Params& P [[buffer(2)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= P.w || id.y >= P.h) return;
    const int K[5] = {1, 4, 6, 4, 1};
    int acc = 0;
    for (int t = 0; t < 5; t++) {
        int y = clamp((int)id.y + t - 2, 0, (int)P.h - 1);
        acc += K[t] * tmp[y * P.w + id.x];
    }
    blur[id.y * P.w + id.x] = (uchar)(acc >> 8);
}

kernel void sobel(device const uchar* blur [[buffer(0)]], device int* mag [[buffer(1)]], device uchar* dir [[buffer(2)]],
                  constant Params& P [[buffer(3)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= P.w || id.y >= P.h) return;
    int x = id.x, y = id.y, w = P.w, h = P.h;
    int xm = max(x - 1, 0), xp = min(x + 1, w - 1), ym = max(y - 1, 0), yp = min(y + 1, h - 1);
    device const uchar* r0 = blur + ym * w;
    device const uchar* r1 = blur + y * w;
    device const uchar* r2 = blur + yp * w;
    int dx = (r0[xp] + 2 * r1[xp] + r2[xp]) - (r0[xm] + 2 * r1[xm] + r2[xm]);
    int dy = (r2[xm] + 2 * r2[x] + r2[xp]) - (r0[xm] + 2 * r0[x] + r0[xp]);
    int adx = abs(dx), ady = abs(dy);
    mag[y * w + x] = dx * dx + dy * dy;
    dir[y * w + x] = ady * 1000 <= adx * 414 ? 0 : ady * 1000 >= adx * 2414 ? 2 : ((dx ^ dy) >= 0 ? 1 : 3);
}

kernel void nms_threshold(device const int* mag [[buffer(0)]], device const uchar* dir [[buffer(1)]], device uchar* cls [[buffer(2)]],
                          constant Params& P [[buffer(3)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= P.w || id.y >= P.h) return;
    int x = id.x, y = id.y, w = P.w, h = P.h;
    uint i = y * w + x;
    int v = 0;
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

kernel void voronoi(device const int2* seeds [[buffer(0)]], device const uint* off [[buffer(1)]], device const uint* idx [[buffer(2)]],
                    device uint* owner [[buffer(3)]], constant Params& P [[buffer(4)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= P.w || id.y >= P.h) return;
    int x = id.x, y = id.y, g = P.g, gw = P.gw, gh = P.gh;
    int cx = x / g, cy = y / g;
    uint best = 0xFFFFFFFFu;
    int bd = 0x7FFFFFFF;
    int rmax = max(gw, gh);
    for (int r = 0; r <= rmax; r++) {
        if (r > 0) {
            int lim = (r - 1) * g + 1;
            if (best != 0xFFFFFFFFu && bd < lim * lim) break;
        }
        for (int by = cy - r; by <= cy + r; by++) {
            if (by < 0 || by >= gh) continue;
            bool edge_row = by == cy - r || by == cy + r;
            int step = edge_row ? 1 : 2 * r;
            for (int bx = cx - r; bx <= cx + r; bx += step) {
                if (bx >= 0 && bx < gw) {
                    uint b = by * gw + bx;
                    for (uint k = off[b]; k < off[b + 1]; k++) {
                        uint i = idx[k];
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

struct Poly { int2 v[4]; uint n; uint pad[3]; };

kernel void raster_find(device const uchar* rgb [[buffer(0)]], device const uint* owner [[buffer(1)]], device const uint* off [[buffer(2)]],
                        device const uint* idx [[buffer(3)]], device const Poly* polys [[buffer(4)]], device uint* pid [[buffer(5)]],
                        device atomic_uint* sums [[buffer(6)]], constant Params& P [[buffer(7)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= P.w || id.y >= P.h) return;
    int x = id.x, y = id.y;
    uint k = y * P.w + x, s = owner[k], found = 0xFFFFFFFFu;
    for (uint j = off[s]; j < off[s + 1] && found == 0xFFFFFFFFu; j++) {
        Poly q = polys[idx[j]];
        bool in = true;
        for (uint i = 0; i < q.n; i++) {
            int2 u = q.v[i], v = q.v[(i + 1) % q.n];
            if ((v.x - u.x) * (y - u.y) - (v.y - u.y) * (x - u.x) < 0) { in = false; break; }
        }
        if (in) found = idx[j];
    }
    pid[k] = found;
    if (found == 0xFFFFFFFFu) return;
    atomic_fetch_add_explicit(&sums[found * 4 + 0], (uint)rgb[k * 3], memory_order_relaxed);
    atomic_fetch_add_explicit(&sums[found * 4 + 1], (uint)rgb[k * 3 + 1], memory_order_relaxed);
    atomic_fetch_add_explicit(&sums[found * 4 + 2], (uint)rgb[k * 3 + 2], memory_order_relaxed);
    atomic_fetch_add_explicit(&sums[found * 4 + 3], 1u, memory_order_relaxed);
}

kernel void poly_colors(device const atomic_uint* sums [[buffer(0)]], device const Poly* polys [[buffer(1)]], device const uchar* rgb [[buffer(2)]],
                        device uchar* colors [[buffer(3)]], constant Params& P [[buffer(4)]], uint i [[thread_position_in_grid]]) {
    if (i >= P.npoly) return;
    uint n = atomic_load_explicit(&sums[i * 4 + 3], memory_order_relaxed);
    if (n) {
        for (uint c = 0; c < 3; c++) colors[i * 3 + c] = (uchar)(atomic_load_explicit(&sums[i * 4 + c], memory_order_relaxed) / n);
    } else {
        uint k = (polys[i].v[0].y * P.w + polys[i].v[0].x) * 3;
        for (uint c = 0; c < 3; c++) colors[i * 3 + c] = rgb[k + c];
    }
}

kernel void raster_paint(device const uint* pid [[buffer(0)]], device const uchar* colors [[buffer(1)]], device uchar* out [[buffer(2)]],
                         constant Params& P [[buffer(3)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= P.w || id.y >= P.h) return;
    uint k = id.y * P.w + id.x, p = pid[k];
    uint c = p == 0xFFFFFFFFu ? P.npoly : p;
    out[k * 3] = colors[c * 3]; out[k * 3 + 1] = colors[c * 3 + 1]; out[k * 3 + 2] = colors[c * 3 + 2];
}
)MSL";

struct Params32 { uint32_t w, h; int32_t low2, high2; uint32_t g, gw, gh, npoly; };
struct PolyGpu { int32_t v[8]; uint32_t n, pad[3]; };

namespace {
struct MetalGpu : Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> luma_blur_h, blur_v, sobel, nms_threshold, vor, raster_find, poly_colors, raster_paint;
    id<MTLBuffer> rgb, tmp, blur, mag, dir, cls, owner, pid, seeds, off, idx, polys, sums, colors, out;
    std::string dev_name;
    int w = 0, h = 0;

    bool init() {
        dev = MTLCreateSystemDefaultDevice();
        if (!dev) return false;
        NSError* err = nil;
        MTLCompileOptions* opts = [MTLCompileOptions new];
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:opts error:&err];
        if (!lib) { fprintf(stderr, "metal: %s\n", err.localizedDescription.UTF8String); return false; }
        auto pso = [&](const char* name, id<MTLComputePipelineState>& p) {
            id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:name]];
            p = [dev newComputePipelineStateWithFunction:f error:&err];
            return p != nil;
        };
        if (!pso("luma_blur_h", luma_blur_h) || !pso("blur_v", blur_v) || !pso("sobel", sobel) ||
            !pso("nms_threshold", nms_threshold) || !pso("voronoi", vor) || !pso("raster_find", raster_find) ||
            !pso("poly_colors", poly_colors) || !pso("raster_paint", raster_paint)) return false;
        queue = [dev newCommandQueue];
        dev_name = dev.name.UTF8String;
        return true;
    }
    const char* name() override { return dev_name.c_str(); }

    id<MTLBuffer> buf(size_t bytes) { return [dev newBufferWithLength:std::max<size_t>(bytes, 16) options:MTLResourceStorageModeShared]; }
    id<MTLBuffer> buf(const void* data, size_t bytes) {
        id<MTLBuffer> b = buf(bytes);
        if (bytes) memcpy(b.contents, data, bytes);
        return b;
    }

    void dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> p, std::initializer_list<id<MTLBuffer>> bufs, const Params32& P) {
        [enc setComputePipelineState:p];
        int i = 0;
        for (auto b : bufs) [enc setBuffer:b offset:0 atIndex:i++];
        [enc setBytes:&P length:sizeof P atIndex:i];
        [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    }

    void upload(const Image& img) override {
        w = img.w; h = img.h;
        size_t n = img.size();
        rgb = buf(img.px.data(), n * 3);
        tmp = buf(n * 2); blur = buf(n); mag = buf(n * 4); dir = buf(n); cls = buf(n);
        owner = buf(n * 4); pid = buf(n * 4); out = buf(n * 3);
    }

    void canny_front(int low2, int high2, std::vector<uint8_t>& c) override {
        Params32 P{(uint32_t)w, (uint32_t)h, low2, high2, 0, 0, 0, 0};
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        dispatch(enc, luma_blur_h, {rgb, tmp}, P);
        dispatch(enc, blur_v, {tmp, blur}, P);
        dispatch(enc, sobel, {blur, mag, dir}, P);
        dispatch(enc, nms_threshold, {mag, dir, cls}, P);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        c.resize((size_t)w * h);
        memcpy(c.data(), cls.contents, c.size());
    }

    void voronoi(const std::vector<Pt>& s, const SeedGrid& gr, std::vector<uint32_t>& o) override {
        Params32 P{(uint32_t)w, (uint32_t)h, 0, 0, (uint32_t)gr.g, (uint32_t)gr.gw, (uint32_t)gr.gh, 0};
        seeds = buf(s.data(), s.size() * sizeof(Pt));
        off = buf(gr.off.data(), gr.off.size() * 4);
        idx = buf(gr.idx.data(), gr.idx.size() * 4);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        dispatch(enc, vor, {seeds, off, idx, owner}, P);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        o.resize((size_t)w * h);
        memcpy(o.data(), owner.contents, o.size() * 4);
    }

    void raster(std::vector<Polygon>& ps, const std::vector<uint32_t>& csr_off, const std::vector<uint32_t>& csr_idx,
                Rgb background, Image& img) override {
        size_t np = ps.size();
        std::vector<PolyGpu> pg(np);
        for (size_t i = 0; i < np; i++) {
            pg[i].n = ps[i].n;
            for (int j = 0; j < 4; j++) { pg[i].v[2 * j] = ps[i].v[j].x; pg[i].v[2 * j + 1] = ps[i].v[j].y; }
        }
        polys = buf(pg.data(), np * sizeof(PolyGpu));
        id<MTLBuffer> coff = buf(csr_off.data(), csr_off.size() * 4);
        id<MTLBuffer> cidx = buf(csr_idx.data(), csr_idx.size() * 4);
        sums = buf(np * 16);
        memset(sums.contents, 0, np * 16);   // 32-bit channel sums; fine below 16M px per polygon
        Params32 P{(uint32_t)w, (uint32_t)h, 0, 0, 0, 0, 0, (uint32_t)np};
        colors = buf((np + 1) * 3);
        uint8_t* col = (uint8_t*)colors.contents;
        col[np * 3] = background.r; col[np * 3 + 1] = background.g; col[np * 3 + 2] = background.b;

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        dispatch(enc, raster_find, {rgb, owner, coff, cidx, polys, pid, sums}, P);
        [enc setComputePipelineState:poly_colors];
        id<MTLBuffer> cbufs[4] = {sums, polys, rgb, colors};
        for (int i = 0; i < 4; i++) [enc setBuffer:cbufs[i] offset:0 atIndex:i];
        [enc setBytes:&P length:sizeof P atIndex:4];
        [enc dispatchThreads:MTLSizeMake(np, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        dispatch(enc, raster_paint, {pid, colors, out}, P);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        for (size_t i = 0; i < np; i++) ps[i].color = {col[i * 3], col[i * 3 + 1], col[i * 3 + 2]};
        img = Image(w, h);
        memcpy(img.px.data(), out.contents, img.size() * 3);
    }
};
}

Gpu* gpu_create() {
    @autoreleasepool {
        MetalGpu* g = new MetalGpu;
        if (g->init()) return g;
        delete g;
        return nullptr;
    }
}

bool platform_decode(const std::string& path, Image& img) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        if (!src) return false;
        CGImageRef cg = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
        CFRelease(src);
        if (!cg) return false;
        int w = (int)CGImageGetWidth(cg), h = (int)CGImageGetHeight(cg);
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, (size_t)w * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        CGImageRelease(cg);
        img = Image(w, h);
        for (size_t i = 0; i < img.size(); i++) img.px[i] = {rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]};
        return true;
    }
}

bool platform_encode(const std::string& path, const Image& img) {
    size_t d = path.rfind('.');
    std::string e = d == std::string::npos ? "" : path.substr(d + 1);
    for (auto& c : e) c = (char)tolower(c);
    CFStringRef uti;
    if (e == "png") uti = CFSTR("public.png");
    else if (e == "jpg" || e == "jpeg") uti = CFSTR("public.jpeg");
    else if (e == "heic") uti = CFSTR("public.heic");
    else if (e == "tif" || e == "tiff") uti = CFSTR("public.tiff");
    else if (e == "webp") uti = CFSTR("org.webmproject.webp");
    else return false;
    @autoreleasepool {
        CGDataProviderRef prov = CGDataProviderCreateWithData(nullptr, img.px.data(), img.size() * 3, nullptr);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef cg = CGImageCreate(img.w, img.h, 8, 24, (size_t)img.w * 3, cs, kCGImageAlphaNone, prov, nullptr, false, kCGRenderingIntentDefault);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, uti, 1, nullptr);
        bool ok = false;
        if (dst) {
            NSDictionary* props = @{(__bridge NSString*)kCGImageDestinationLossyCompressionQuality: @0.9};
            CGImageDestinationAddImage(dst, cg, (__bridge CFDictionaryRef)props);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        CGImageRelease(cg);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(prov);
        return ok;
    }
}
