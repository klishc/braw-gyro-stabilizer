// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// WarpKernel.metal
// Per-row homography inverse warp for gyro stabilization.
//
// For each output pixel (x, y):
//   1. Load the 3×3 homography H for scanline y
//   2. Apply H to (x, y, 1) → source homogeneous coordinate
//   3. Perspective-divide → (u, v) normalised source UV
//   4. Bilinear-sample source texture
//   5. Write to output texture
//
// Buffer layout:
//   buffer(0) — float[height × 9], one row-major 3×3 homography per scanline
//   buffer(1) — WarpUniforms { width, height, pad, pad }

#include <metal_stdlib>
using namespace metal;

struct WarpUniforms {
    uint width;
    uint height;
    uint pad0;
    uint pad1;
};

// ── Bilinear sampling (clamp-to-edge) ────────────────────────────────────────
static float4 SampleBilinear(
    texture2d<float, access::read> tex,
    float u, float v,
    uint W, uint H)
{
    // Convert UV [0,1] → texel-center pixel coordinates
    float px = u * float(W) - 0.5f;
    float py = v * float(H) - 0.5f;

    int x0 = int(floor(px));
    int y0 = int(floor(py));
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    // Clamp to [0, W-1] / [0, H-1]
    x0 = clamp(x0, 0, int(W) - 1);
    y0 = clamp(y0, 0, int(H) - 1);
    x1 = clamp(x1, 0, int(W) - 1);
    y1 = clamp(y1, 0, int(H) - 1);

    float fx = px - floor(px);
    float fy = py - floor(py);

    float4 c00 = tex.read(uint2(x0, y0));
    float4 c10 = tex.read(uint2(x1, y0));
    float4 c01 = tex.read(uint2(x0, y1));
    float4 c11 = tex.read(uint2(x1, y1));

    // Bilinear interpolation
    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy);
}

// ── Main kernel ───────────────────────────────────────────────────────────────
kernel void warp_kernel(
    texture2d<float, access::read>   srcTex   [[texture(0)]],
    texture2d<float, access::write>  dstTex   [[texture(1)]],
    device const float*              homo     [[buffer(0)]],
    device const WarpUniforms&       uni      [[buffer(1)]],
    uint2                            gid      [[thread_position_in_grid]])
{
    uint W = uni.width;
    uint H = uni.height;

    if (gid.x >= W || gid.y >= H) return;

    // Load the 3×3 homography for this row (row-major storage, 9 floats).
    // homo is laid out as: homo[row * 9 + element]
    // Element layout (row-major): [h00, h01, h02, h10, h11, h12, h20, h21, h22]
    uint base = gid.y * 9u;
    float h00 = homo[base + 0u];
    float h01 = homo[base + 1u];
    float h02 = homo[base + 2u];
    float h10 = homo[base + 3u];
    float h11 = homo[base + 4u];
    float h12 = homo[base + 5u];
    float h20 = homo[base + 6u];
    float h21 = homo[base + 7u];
    float h22 = homo[base + 8u];

    // Output pixel centre (pixel coordinates, not normalised)
    float ox = float(gid.x) + 0.5f;
    float oy = float(gid.y) + 0.5f;

    // Apply H to (ox, oy, 1) — maps output → source pixel coordinates
    float sx_h = h00 * ox + h01 * oy + h02;
    float sy_h = h10 * ox + h11 * oy + h12;
    float sw   = h20 * ox + h21 * oy + h22;

    // Perspective divide
    float srcX = sx_h / sw;
    float srcY = sy_h / sw;

    // Convert pixel coordinates → [0, 1] UV
    float u = srcX / float(W);
    float v = srcY / float(H);

    float4 color;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        // Out-of-bounds → black (stabilization border)
        color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    } else {
        color = SampleBilinear(srcTex, u, v, W, H);
    }

    dstTex.write(color, gid);
}
