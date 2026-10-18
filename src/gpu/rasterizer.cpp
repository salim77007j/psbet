// ============================================================================
//  PSBET — rasterizer.cpp (Phase 5)
//  Edge-function rasterization with watertight fill (E >= 0), depth test and
//  barycentric color interpolation. Precision: double for edge accumulation.
//
//  Phase 5: row-band parallel execution on a persistent worker pool plus an
//  optional AVX2 path (explicit intrinsics, no FMA contraction). Both are
//  BYTE-IDENTICAL to the original scalar path by construction:
//    * bands own disjoint row ranges — per-pixel math never reorders;
//    * the AVX path performs the exact same IEEE double operations per
//      pixel (same grouping, no incremental reformulation, no FMA), four
//      pixels at a time, with a scalar tail for span remainders.
//  Identity is locked by raster_parallel / avx2_compare in the test suite
//  (finite vertex data — the PBFF contract; NaN inputs are guests' UB).
//
//  Fill-rule note (v1, unchanged): we accept E >= 0 for all triangles rather
//  than a strict top-left rule. Shared-edge pixels are double-drawn; with
//  opaque geometry and LESS depth testing the result is visually identical
//  and the implementation is trivially watertight.
// ============================================================================
#include "rasterizer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"

namespace psb::gpu {

namespace {

// ---------------------------------------------------------------------------
// Per-triangle preparation — hoisted out of the pixel loops. The arithmetic
// is EXACTLY the Phase-1 scalar math (same ops, same order), so results are
// bit-identical to the historical reference output.
// ---------------------------------------------------------------------------
struct TriPrep {
    double ax, ay, bx, by, cx, cy;    // screen space (double, as before)
    double invArea;
    i32    minX, maxX, minY, maxY;    // clamped bounding box (empty if culled)
    float  az, bz, cz;                // depths (float, promoted at use)
    float  col[3][4];                 // a/b/c colors
    bool   depthTest;
    DepthFunc depthFunc;
    bool   live() const { return minX <= maxX && minY <= maxY; }
};

static void prepTriangle(const Rasterizer& r, const GpuVertex* va,
                         const GpuVertex* vb, const GpuVertex* vc,
                         CullMode cull, bool depthTest, DepthFunc depthFunc,
                         TriPrep* out) {
    const u32 width = r.width(), height = r.height();
    const float* vp = r.viewport();

    // NDC -> screen (y-up NDC, y-down screen). z maps directly to depth.
    const double ax = vp[0] + (va->pos[0] * 0.5 + 0.5) * vp[2];
    const double ay = vp[1] + (1.0 - (va->pos[1] * 0.5 + 0.5)) * vp[3];
    const double bx = vp[0] + (vb->pos[0] * 0.5 + 0.5) * vp[2];
    const double by = vp[1] + (1.0 - (vb->pos[1] * 0.5 + 0.5)) * vp[3];
    const double cx = vp[0] + (vc->pos[0] * 0.5 + 0.5) * vp[2];
    const double cy = vp[1] + (1.0 - (vc->pos[1] * 0.5 + 0.5)) * vp[3];

    out->ax = ax; out->ay = ay; out->bx = bx; out->by = by;
    out->cx = cx; out->cy = cy;
    out->az = va->pos[2]; out->bz = vb->pos[2]; out->cz = vc->pos[2];
    std::memcpy(out->col[0], va->col, sizeof(out->col[0]));
    std::memcpy(out->col[1], vb->col, sizeof(out->col[1]));
    std::memcpy(out->col[2], vc->col, sizeof(out->col[2]));
    out->depthTest = depthTest;
    out->depthFunc = depthFunc;
    out->minX = 1; out->maxX = 0;     // empty by default
    out->minY = 1; out->maxY = 0;

    const double area2 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area2 == 0.0) { out->invArea = 0.0; return; }   // degenerate
    if (cull == CullMode::Back && area2 <= 0.0) { out->invArea = 0.0; return; }
    out->invArea = 1.0 / area2;

    // Bounding box clamped to viewport and framebuffer (identical math).
    const i32 minX = std::max<i32>(static_cast<i32>(vp[0]),
        std::max<i32>(0, static_cast<i32>(std::floor(std::min({ax, bx, cx})))));
    const i32 maxX = std::min<i32>(static_cast<i32>(vp[0] + vp[2]) - 1,
        std::min<i32>(static_cast<i32>(width) - 1,
                      static_cast<i32>(std::ceil(std::max({ax, bx, cx})))));
    const i32 minY = std::max<i32>(static_cast<i32>(vp[1]),
        std::max<i32>(0, static_cast<i32>(std::floor(std::min({ay, by, cy})))));
    const i32 maxY = std::min<i32>(static_cast<i32>(vp[1] + vp[3]) - 1,
        std::min<i32>(static_cast<i32>(height) - 1,
                      static_cast<i32>(std::ceil(std::max({ay, by, cy})))));
    if (minX > maxX || minY > maxY) return;
    out->minX = minX; out->maxX = maxX;
    out->minY = minY; out->maxY = maxY;
}

// ---------------------------------------------------------------------------
// Scalar per-pixel core — arithmetic identical to the Phase-1 path.
// ---------------------------------------------------------------------------
static inline void scalarPixel(const Rasterizer& r, const TriPrep& t,
                               i32 py, i32 px) {
    const double sx = px + 0.5;
    const double sy = py + 0.5;

    const double w0 = ((t.bx - t.ax) * (sy - t.ay)
                     - (t.by - t.ay) * (sx - t.ax)) * t.invArea;
    const double w1 = ((t.cx - t.bx) * (sy - t.by)
                     - (t.cy - t.by) * (sx - t.bx)) * t.invArea;
    const double w2 = 1.0 - w0 - w1;

    if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) return;

    const double depthV = t.az * w0 + t.bz * w1 + t.cz * w2;
    const size_t idx = static_cast<size_t>(py) * r.width() + px;

    if (t.depthTest) {
        bool pass = false;
        switch (t.depthFunc) {
            case DepthFunc::Never: pass = false; break;
            case DepthFunc::Less:  pass = depthV <  r.depthData()[idx]; break;
            case DepthFunc::Equal: pass = depthV == r.depthData()[idx]; break;
            case DepthFunc::LEqual:pass = depthV <= r.depthData()[idx]; break;
        }
        if (!pass) return;
    }

    r.depthData()[idx] = static_cast<float>(depthV);

    const float rr = static_cast<float>(t.col[0][0] * w0 + t.col[1][0] * w1 + t.col[2][0] * w2);
    const float gg = static_cast<float>(t.col[0][1] * w0 + t.col[1][1] * w1 + t.col[2][1] * w2);
    const float bb = static_cast<float>(t.col[0][2] * w0 + t.col[1][2] * w1 + t.col[2][2] * w2);
    const float aa = static_cast<float>(t.col[0][3] * w0 + t.col[1][3] * w1 + t.col[2][3] * w2);

    u8* out = r.colorData() + idx * 4;
    out[0] = static_cast<u8>(std::clamp(rr, 0.f, 1.f) * 255.99f);
    out[1] = static_cast<u8>(std::clamp(gg, 0.f, 1.f) * 255.99f);
    out[2] = static_cast<u8>(std::clamp(bb, 0.f, 1.f) * 255.99f);
    out[3] = static_cast<u8>(std::clamp(aa, 0.f, 1.f) * 255.99f);
}

// Scalar band core — rows [y0, y1) of one prepared triangle.
static void rasterBandScalar(const Rasterizer& r, const TriPrep& t,
                             i32 y0, i32 y1) {
    for (i32 py = y0; py < y1; ++py)
        for (i32 px = t.minX; px <= t.maxX; ++px)
            scalarPixel(r, t, py, px);
}

// ---------------------------------------------------------------------------
// AVX2 band core — four pixels per vector. Same IEEE double operations as
// the scalar core (identical grouping; explicit intrinsics cannot contract
// into FMA), plus a scalar tail for span remainders.
// ---------------------------------------------------------------------------
#if (defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)) || \
    (defined(__x86_64__) && defined(__clang__))
#define PSBET_AVX_PATH 1
#endif

#ifdef PSBET_AVX_PATH
#include <immintrin.h>

__attribute__((target("avx2")))
static void rasterBandAvx(const Rasterizer& r, const TriPrep& t,
                          i32 y0, i32 y1) {
    const u32 width = r.width();
    u8*    color = r.colorData();
    float* depth = r.depthData();

    const __m256d vAx = _mm256_set1_pd(t.ax), vAy = _mm256_set1_pd(t.ay);
    const __m256d vBx = _mm256_set1_pd(t.bx), vBy = _mm256_set1_pd(t.by);
    const __m256d dBxAx = _mm256_set1_pd(t.bx - t.ax);
    const __m256d dByAy = _mm256_set1_pd(t.by - t.ay);
    const __m256d dCxBx = _mm256_set1_pd(t.cx - t.bx);
    const __m256d dCyBy = _mm256_set1_pd(t.cy - t.by);
    const __m256d vInvArea = _mm256_set1_pd(t.invArea);
    const __m256d vZero = _mm256_setzero_pd();
    const __m256d vOne  = _mm256_set1_pd(1.0);
    const __m256d vAz = _mm256_set1_pd(static_cast<double>(t.az));
    const __m256d vBz = _mm256_set1_pd(static_cast<double>(t.bz));
    const __m256d vCz = _mm256_set1_pd(static_cast<double>(t.cz));
    const __m256d vAr = _mm256_set1_pd(static_cast<double>(t.col[0][0]));
    const __m256d vAg = _mm256_set1_pd(static_cast<double>(t.col[0][1]));
    const __m256d vAb = _mm256_set1_pd(static_cast<double>(t.col[0][2]));
    const __m256d vAa = _mm256_set1_pd(static_cast<double>(t.col[0][3]));
    const __m256d vBr = _mm256_set1_pd(static_cast<double>(t.col[1][0]));
    const __m256d vBg = _mm256_set1_pd(static_cast<double>(t.col[1][1]));
    const __m256d vBb = _mm256_set1_pd(static_cast<double>(t.col[1][2]));
    const __m256d vBa = _mm256_set1_pd(static_cast<double>(t.col[1][3]));
    const __m256d vCr = _mm256_set1_pd(static_cast<double>(t.col[2][0]));
    const __m256d vCg = _mm256_set1_pd(static_cast<double>(t.col[2][1]));
    const __m256d vCb = _mm256_set1_pd(static_cast<double>(t.col[2][2]));
    const __m256d vCa = _mm256_set1_pd(static_cast<double>(t.col[2][3]));
    const __m128 v255d99 = _mm_set1_ps(255.99f);
    const __m128 vZeroF  = _mm_setzero_ps();
    const __m128 vOneF   = _mm_set1_ps(1.f);

    for (i32 py = y0; py < y1; ++py) {
        const double sy = py + 0.5;
        const __m256d vSyAy = _mm256_sub_pd(_mm256_set1_pd(sy), vAy);
        const __m256d vSyBy = _mm256_sub_pd(_mm256_set1_pd(sy), vBy);

        i32 px = t.minX;
        const i32 spanEnd = t.maxX - 3;   // last x with px+3 <= maxX
        for (; px <= spanEnd; px += 4) {
            // sx lanes: px, px+1, px+2, px+3 (each +0.5). set_pd is high..low.
            const __m256d vSx = _mm256_set_pd(px + 3 + 0.5, px + 2 + 0.5,
                                              px + 1 + 0.5, px + 0.5);
            const __m256d vSxAx = _mm256_sub_pd(vSx, vAx);
            const __m256d vSxBx = _mm256_sub_pd(vSx, vBx);

            // w0 = ((bx-ax)*(sy-ay) - (by-ay)*(sx-ax)) * invArea
            const __m256d w0 = _mm256_mul_pd(
                _mm256_sub_pd(_mm256_mul_pd(dBxAx, vSyAy),
                              _mm256_mul_pd(dByAy, vSxAx)), vInvArea);
            // w1 = ((cx-bx)*(sy-by) - (cy-by)*(sx-bx)) * invArea
            const __m256d w1 = _mm256_mul_pd(
                _mm256_sub_pd(_mm256_mul_pd(dCxBx, vSyBy),
                              _mm256_mul_pd(dCyBy, vSxBx)), vInvArea);
            const __m256d w2 = _mm256_sub_pd(_mm256_sub_pd(vOne, w0), w1);

            // Inside test: !(w0<0 || w1<0 || w2<0)
            const __m256d inside = _mm256_and_pd(_mm256_and_pd(
                _mm256_cmp_pd(w0, vZero, _CMP_GE_OQ),
                _mm256_cmp_pd(w1, vZero, _CMP_GE_OQ)),
                _mm256_cmp_pd(w2, vZero, _CMP_GE_OQ));
            int pm = _mm256_movemask_pd(inside);
            if (pm == 0) continue;

            // depth = az*w0 + bz*w1 + cz*w2 (left-assoc, as the scalar core)
            const __m256d depthV = _mm256_add_pd(
                _mm256_add_pd(_mm256_mul_pd(vAz, w0), _mm256_mul_pd(vBz, w1)),
                _mm256_mul_pd(vCz, w2));

            const size_t idx = static_cast<size_t>(py) * width + px;
            if (t.depthTest) {
                const __m128 oldF = _mm_loadu_ps(depth + idx);
                const __m256d oldD = _mm256_cvtps_pd(oldF);
                __m256d cmp;
                switch (t.depthFunc) {
                    case DepthFunc::Never:  cmp = _mm256_setzero_pd(); break;
                    case DepthFunc::Less:   cmp = _mm256_cmp_pd(depthV, oldD, _CMP_LT_OQ); break;
                    case DepthFunc::Equal:  cmp = _mm256_cmp_pd(depthV, oldD, _CMP_EQ_OQ); break;
                    case DepthFunc::LEqual: cmp = _mm256_cmp_pd(depthV, oldD, _CMP_LE_OQ); break;
                    default:                cmp = _mm256_setzero_pd(); break;
                }
                pm = _mm256_movemask_pd(_mm256_and_pd(inside, cmp));
                if (pm == 0) continue;
            }

            // Depth writes: pass lanes take the new depth (bands own disjoint
            // pixels, so per-lane stores are race-free).
            const __m128 newDepthF = _mm256_cvtpd_ps(depthV);
            alignas(16) float dOut[4];
            _mm_store_ps(dOut, newDepthF);
            for (int lane = 0; lane < 4; ++lane)
                if (pm & (1 << lane)) depth[idx + lane] = dOut[lane];

            // Colors: double interp -> float (same rounding as the scalar
            // cast) -> clamp -> *255.99f -> truncate to u8.
            const __m128 rf = _mm256_cvtpd_ps(_mm256_add_pd(_mm256_add_pd(
                _mm256_mul_pd(vAr, w0), _mm256_mul_pd(vBr, w1)), _mm256_mul_pd(vCr, w2)));
            const __m128 gf = _mm256_cvtpd_ps(_mm256_add_pd(_mm256_add_pd(
                _mm256_mul_pd(vAg, w0), _mm256_mul_pd(vBg, w1)), _mm256_mul_pd(vCg, w2)));
            const __m128 bf = _mm256_cvtpd_ps(_mm256_add_pd(_mm256_add_pd(
                _mm256_mul_pd(vAb, w0), _mm256_mul_pd(vBb, w1)), _mm256_mul_pd(vCb, w2)));
            const __m128 af = _mm256_cvtpd_ps(_mm256_add_pd(_mm256_add_pd(
                _mm256_mul_pd(vAa, w0), _mm256_mul_pd(vBa, w1)), _mm256_mul_pd(vCa, w2)));

            // std::clamp(v,0,1) for finite v == max(v,0) then min(.,1).
            const __m128 rc = _mm_min_ps(_mm_max_ps(rf, vZeroF), vOneF);
            const __m128 gc = _mm_min_ps(_mm_max_ps(gf, vZeroF), vOneF);
            const __m128 bc = _mm_min_ps(_mm_max_ps(bf, vZeroF), vOneF);
            const __m128 ac = _mm_min_ps(_mm_max_ps(af, vZeroF), vOneF);

            const __m128i ri = _mm_cvttps_epi32(_mm_mul_ps(rc, v255d99));
            const __m128i gi = _mm_cvttps_epi32(_mm_mul_ps(gc, v255d99));
            const __m128i bi = _mm_cvttps_epi32(_mm_mul_ps(bc, v255d99));
            const __m128i ai = _mm_cvttps_epi32(_mm_mul_ps(ac, v255d99));

            const __m128i pix = _mm_or_si128(_mm_or_si128(
                ri, _mm_slli_epi32(gi, 8)),
                _mm_or_si128(_mm_slli_epi32(bi, 16), _mm_slli_epi32(ai, 24)));

            if (t.depthTest) {
                _mm_maskstore_epi32(reinterpret_cast<int*>(color + idx * 4),
                    _mm_set_epi32(-(pm >> 3 & 1), -(pm >> 2 & 1),
                                  -(pm >> 1 & 1), -(pm & 1)), pix);
            } else {
                _mm_storeu_si128(reinterpret_cast<__m128i*>(color + idx * 4), pix);
            }
        }
        // Scalar tail (identical arithmetic).
        for (; px <= t.maxX; ++px) scalarPixel(r, t, py, px);
    }
}
#endif // PSBET_AVX_PATH

// ---------------------------------------------------------------------------
// Tuning state + dispatch
// ---------------------------------------------------------------------------
std::atomic<u32> g_bandThreads{0};    // 0 = auto
std::atomic<bool> g_simdEnabled{true};

u32 autoBandThreads() {
    if (const char* e = std::getenv("PSBET_RASTER_THREADS")) {
        const long v = std::strtol(e, nullptr, 10);
        if (v >= 1 && v <= 64) return static_cast<u32>(v);
    }
    const u32 hw = std::thread::hardware_concurrency();
    u32 n = hw ? hw - 1 : 1;
    return std::clamp(n, 1u, 8u);
}

u32 effectiveBandThreads() {
    const u32 t = g_bandThreads.load(std::memory_order_relaxed);
    return t ? t : autoBandThreads();
}

bool useAvxPath() {
#ifdef PSBET_AVX_PATH
    if (!g_simdEnabled.load(std::memory_order_relaxed)) return false;
    // Tuning/measurement override: PSBET_RASTER_SIMD=off forces the scalar
    // path (A/B benchmarking; identical output either way).
    if (const char* e = std::getenv("PSBET_RASTER_SIMD"))
        if (std::strcmp(e, "off") == 0) return false;
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Task pool — persistent workers + the calling thread all pull from one FIFO.
// Tasks own disjoint work, so completion order never affects results.
// ---------------------------------------------------------------------------
class TaskPool {
public:
    static TaskPool& get() {
        static TaskPool p(effectiveBandThreads());
        return p;
    }

    explicit TaskPool(u32 workers) {
        const u32 n = std::clamp(workers, 1u, 64u);
        // The caller participates too: n-1 dedicated threads + main = n.
        for (u32 i = 1; i < n; ++i)
            th_.emplace_back([this] { workerLoop(); });
    }

    ~TaskPool() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cvWork_.notify_all();
        for (auto& t : th_) t.join();
    }

    void run(std::vector<std::function<void()>> tasks) {
        if (tasks.empty()) return;
        if (tasks.size() == 1 || th_.empty()) {
            for (auto& f : tasks) f();
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& f : tasks) tasks_.push_back(std::move(f));
            active_ = static_cast<u32>(tasks_.size());
        }
        cvWork_.notify_all();

        // Main thread pulls too.
        for (;;) {
            std::function<void()> f;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (tasks_.empty()) break;
                f = std::move(tasks_.front());
                tasks_.pop_front();
            }
            f();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (--active_ == 0) break;
            }
        }
        // Wait for the rest.
        std::unique_lock<std::mutex> lk(mtx_);
        cvDone_.wait(lk, [this] { return active_ == 0; });
    }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cvWork_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                f = std::move(tasks_.front());
                tasks_.pop_front();
            }
            f();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (--active_ == 0) cvDone_.notify_all();
            }
        }
    }

    std::mutex mtx_;
    std::condition_variable cvWork_, cvDone_;
    std::deque<std::function<void()>> tasks_;
    u32 active_ = 0;
    bool stop_ = false;
    std::vector<std::thread> th_;
};

static void rasterBand(const Rasterizer& r, const TriPrep& t, i32 y0, i32 y1) {
#ifdef PSBET_AVX_PATH
    if (useAvxPath()) {
        rasterBandAvx(r, t, y0, y1);
        return;
    }
#endif
    rasterBandScalar(r, t, y0, y1);
}

constexpr i64 kBandMinPixels = 4096;   // below this, banding overhead wins

} // namespace

// ---------------------------------------------------------------------------
// Rasterizer public API
// ---------------------------------------------------------------------------
Rasterizer::Rasterizer(u8* colorBuf, float* depthBuf, u32 width, u32 height)
    : color_(colorBuf), depth_(depthBuf), width_(width), height_(height) {
    vpW_ = static_cast<float>(width);
    vpH_ = static_cast<float>(height);
}

void Rasterizer::setViewport(float x, float y, float w, float h) {
    vpX_ = x; vpY_ = y; vpW_ = w; vpH_ = h;
}

void Rasterizer::setDepthTest(bool enable, DepthFunc func) {
    depthTest_ = enable;
    depthFunc_ = func;
}

void Rasterizer::setCullMode(CullMode mode) { cull_ = mode; }

void Rasterizer::clear(float r, float g, float b, float a) {
    const u32 packed = static_cast<u32>(std::clamp(r, 0.f, 1.f) * 255.99f)
                     | (static_cast<u32>(std::clamp(g, 0.f, 1.f) * 255.99f) << 8)
                     | (static_cast<u32>(std::clamp(b, 0.f, 1.f) * 255.99f) << 16)
                     | (static_cast<u32>(std::clamp(a, 0.f, 1.f) * 255.99f) << 24);
    u32* px = reinterpret_cast<u32*>(color_);
    float* dp = depth_;
    const size_t total = static_cast<size_t>(width_) * height_;

    const u32 threads = effectiveBandThreads();
    if (threads <= 1 || height_ < threads) {
        std::fill(px, px + total, packed);
        std::fill(dp, dp + total, 1.0f);
        return;
    }

    // Row-split clear: disjoint row ranges, trivially deterministic.
    const u32 nBands = std::min<u32>(threads, static_cast<u32>(height_));
    const size_t rows = height_ / nBands, rem = height_ % nBands;
    std::vector<std::function<void()>> tasks;
    tasks.reserve(nBands);
    size_t y0 = 0;
    for (u32 b = 0; b < nBands; ++b) {
        const size_t cnt = rows + (b < rem ? 1 : 0);
        tasks.emplace_back([px, dp, y0, cnt, packed, width = width_] {
            std::fill(px + y0 * width, px + (y0 + cnt) * width, packed);
            std::fill(dp + y0 * width, dp + (y0 + cnt) * width, 1.0f);
        });
        y0 += cnt;
    }
    TaskPool::get().run(std::move(tasks));
}

void Rasterizer::drawTriangles(const GpuVertex* v, u32 count) {
    const u32 nTris = count / 3;
    if (nTris == 0) return;

    const u32 threads = effectiveBandThreads();

    // Prepare all triangles first (per-triangle math hoisted out of bands).
    std::vector<TriPrep> preps(nTris);
    for (u32 t = 0; t < nTris; ++t)
        prepTriangle(*this, &v[t * 3 + 0], &v[t * 3 + 1], &v[t * 3 + 2],
                     cull_, depthTest_, depthFunc_, &preps[t]);

    // Build band tasks. Pointers into `preps` are stable from here on.
    std::vector<std::function<void()>> tasks;
    const bool canBand = threads > 1;
    for (const TriPrep& p : preps) {
        if (!p.live()) continue;
        const i32 rows = p.maxY - p.minY + 1;
        const i64 pixels = static_cast<i64>(rows) * (p.maxX - p.minX + 1);
        if (!canBand || pixels < kBandMinPixels) {
            tasks.emplace_back([&r = *this, &p] { rasterBand(r, p, p.minY, p.maxY + 1); });
            continue;
        }
        const i32 nBands = static_cast<i32>(std::min<u32>(threads, static_cast<u32>(rows)));
        const i32 bandRows = (rows + nBands - 1) / nBands;
        for (i32 b = 0, y = p.minY; b < nBands; ++b, y += bandRows) {
            const i32 y1 = std::min<i32>(y + bandRows, p.maxY + 1);
            tasks.emplace_back([&r = *this, &p, y, y1] { rasterBand(r, p, y, y1); });
        }
    }
    if (tasks.empty()) return;

    if (tasks.size() > 1 && canBand)
        TaskPool::get().run(std::move(tasks));
    else
        for (auto& f : tasks) f();
}

// ---- Phase 5 tuning --------------------------------------------------------
void Rasterizer::setBandThreads(u32 n) { g_bandThreads.store(n); }
u32  Rasterizer::bandThreads()         { return effectiveBandThreads(); }
void Rasterizer::setSimdEnabled(bool on) { g_simdEnabled.store(on); }
bool Rasterizer::simdEnabled()         { return g_simdEnabled.load(); }
bool Rasterizer::simdCompiled() {
#ifdef PSBET_AVX_PATH
    return true;
#else
    return false;
#endif
}
bool Rasterizer::hostHasAvx2() {
#ifdef PSBET_AVX_PATH
    return __builtin_cpu_supports("avx2") != 0;
#else
    return false;
#endif
}

} // namespace psb::gpu
