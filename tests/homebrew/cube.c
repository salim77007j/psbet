// ============================================================================
//  PSBET test homebrew — cube.c
//  A rotating, depth-tested 3D cube rendered through PSBET's emulated GPU
//  pipeline: the guest builds PBFF command buffers each frame (clear,
//  viewport, depth state, cull state, 12 shaded triangles) and submits them
//  via sceGnmSubmitCommandBuffers — the same submission shape a real PS5
//  title uses. PSBET executes them on its software rasterizer and captures
//  screenshots host-side.
//
//  This is the Phase 1 "proof of life" workload. Runs until the host frame
//  limit or sceKernelExitProcess. Exit code 0.
// ============================================================================
#include <math.h>
#include <string.h>
#include "psbet/psb_guest_api.h"

#define FRAME_W 1280
#define FRAME_H 720
#define DCB_SIZE (256 * 1024)

// Per-frame static ring storage: reused every frame so nothing leaks even
// when the host frame limit longjmps out mid-frame.
static __attribute__((aligned(16))) uint8_t dcb[DCB_SIZE];

static int32_t mutex_id = 0;
static uint64_t frame_count = 0;

static void push_bytes(size_t* off, const void* p, size_t n) {
    memcpy(dcb + *off, p, n);
    *off += n;
}

typedef struct { float m[4][4]; } Mat4;

static void mat_identity(Mat4* m) {
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.f;
}

static void mat_mul(Mat4* out, const Mat4* a, const Mat4* b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a->m[i][k] * b->m[k][j];
            r.m[i][j] = s;
        }
    *out = r;
}

static void mat_rotate(Mat4* m, float ax, float ay) {
    Mat4 rx, ry;
    mat_identity(m);
    const float cx = cosf(ax), sx = sinf(ax);
    const float cy = cosf(ay), sy = sinf(ay);
    mat_identity(&rx);
    rx.m[1][1] = cx; rx.m[1][2] = -sx;
    rx.m[2][1] = sx; rx.m[2][2] = cx;
    mat_identity(&ry);
    ry.m[0][0] = cy;  ry.m[0][2] = sy;
    ry.m[2][0] = -sy; ry.m[2][2] = cy;
    mat_mul(m, &ry, &rx);
}

// Cube: 6 faces x 2 triangles, CCW winding (front = CCW in screen space).
static const float cube_verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};
static const int cube_faces[6][4] = {
    {4, 5, 6, 7}, // +z front
    {1, 0, 3, 2}, // -z back
    {5, 1, 2, 6}, // +x right
    {0, 4, 7, 3}, // -x left
    {7, 6, 2, 3}, // +y top
    {0, 1, 5, 4}, // -y bottom
};
static const float face_colors[6][4] = {
    {0.90f, 0.32f, 0.24f, 1.f},
    {0.95f, 0.61f, 0.16f, 1.f},
    {0.36f, 0.72f, 0.28f, 1.f},
    {0.24f, 0.55f, 0.90f, 1.f},
    {0.62f, 0.34f, 0.84f, 1.f},
    {0.94f, 0.83f, 0.30f, 1.f},
};

// Perspective-project a world point to NDC (z in [0,1], 1 = far).
static void project(const Mat4* rot, const float* p, float* out4) {
    float wx = p[0], wy = p[1], wz = p[2];
    float rx = rot->m[0][0] * wx + rot->m[0][1] * wy + rot->m[0][2] * wz;
    float ry = rot->m[1][0] * wx + rot->m[1][1] * wy + rot->m[1][2] * wz;
    float rz = rot->m[2][0] * wx + rot->m[2][1] * wy + rot->m[2][2] * wz;

    const float camDist = 4.2f;           // camera at z = -camDist
    const float zc = rz + camDist;        // view depth (positive in front)
    const float focal = 1.9f;
    const float x = (rx * focal) / zc;
    const float y = (ry * focal) / zc;

    // Depth: map view depth [camDist - sqrt3, camDist + sqrt3] -> [0, 1]
    const float zn = camDist - 1.8f, zf = camDist + 1.8f;
    const float depth = (zc - zn) / (zf - zn);

    out4[0] = x; out4[1] = y; out4[2] = depth; out4[3] = 1.f;
}

void psb_entry(void) {
    if (sceKernelCreateMutex("frame", 0, 0, &mutex_id) != 0)
        sceKernelExitProcess(1);

    const uint64_t t0 = sceKernelGetProcessTime();

    for (;;) {
        const uint64_t t = sceKernelGetProcessTime() - t0;
        const float angle = (float)(t % 4000000ull) * (6.28318f / 4000000.f) * 2.f;
        const float ax = angle * 0.7f, ay = angle;

        Mat4 rot;
        mat_rotate(&rot, ax, ay);

        // ---- build the display command buffer ------------------------------
        size_t off = 0;

        // Clear (deep navy)
        {
            struct { uint32_t op, size; float c[4]; } pkt = {
                1, sizeof(uint32_t) * 2 + sizeof(float) * 4,
                {0.03f, 0.045f, 0.10f, 1.f}};
            push_bytes(&off, &pkt, sizeof(pkt));
        }
        // Viewport
        {
            struct { uint32_t op, size; float x, y, w, h; } pkt = {
                2, sizeof(uint32_t) * 2 + sizeof(float) * 4,
                0.f, 0.f, (float)FRAME_W, (float)FRAME_H};
            push_bytes(&off, &pkt, sizeof(pkt));
        }
        // Depth test LESS
        {
            struct { uint32_t op, size; uint32_t enable, func; } pkt = {
                3, sizeof(uint32_t) * 4, 1, 1};
            push_bytes(&off, &pkt, sizeof(pkt));
        }
        // Backface cull
        {
            struct { uint32_t op, size; uint32_t mode; } pkt = {
                4, sizeof(uint32_t) * 3, 1};
            push_bytes(&off, &pkt, sizeof(pkt));
        }
        // Draw: 12 triangles, interleaved position(NDC)+color
        {
            const uint32_t count = 36;
            const uint32_t payload = sizeof(uint32_t) * 3 + count * (sizeof(float) * 8);
            struct { uint32_t op, size, vertexCount; } hdr = {5, payload, count};
            push_bytes(&off, &hdr, sizeof(hdr));

            static float verts[36 * 8]; // static: no stack pressure
            size_t vi = 0;
            for (int f = 0; f < 6; ++f) {
                const int quad[4] = { cube_faces[f][0], cube_faces[f][1],
                                      cube_faces[f][2], cube_faces[f][3] };
                const int tris[2][3] = {
                    {quad[0], quad[1], quad[2]},
                    {quad[0], quad[2], quad[3]},
                };
                for (int tt = 0; tt < 2; ++tt) {
                    for (int k = 0; k < 3; ++k) {
                        float ndc[4];
                        project(&rot, cube_verts[tris[tt][k]], ndc);
                        verts[vi++] = ndc[0]; verts[vi++] = ndc[1];
                        verts[vi++] = ndc[2]; verts[vi++] = ndc[3];
                        // face shading: slight gradient by vertex index
                        verts[vi++] = face_colors[f][0];
                        verts[vi++] = face_colors[f][1];
                        verts[vi++] = face_colors[f][2];
                        verts[vi++] = face_colors[f][3];
                    }
                }
            }
            push_bytes(&off, verts, sizeof(float) * 8 * count);
        }

        // ---- submit to the emulated GPU ------------------------------------
        void* dcbAddrs[1] = { dcb };
        uint32_t dcbSizes[1] = { (uint32_t)off };
        sceGnmSubmitCommandBuffers(1, dcbAddrs, dcbSizes, 0, 0, 0);
        frame_count++;

        if ((frame_count % 300) == 0) {
            const char* msg = "[cube] frame milestone reached\n";
            sceKernelWrite(1, msg, strlen(msg));
        }
    }
}
