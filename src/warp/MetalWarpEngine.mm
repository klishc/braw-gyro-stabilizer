// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// MetalWarpEngine.mm
// Compiles WarpKernel.metal at runtime using MTLDevice newLibraryWithSource:
// This avoids needing the Metal toolchain at build time entirely.

#import "MetalWarpEngine.h"
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <cstring>
#include <vector>

// ── Warp kernel source embedded as a string ───────────────────────────────────
// Identical to shaders/WarpKernel.metal — kept in sync manually.
static const char* kWarpKernelSrc = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct WarpUniforms {
    uint width;
    uint height;
    uint pad0;
    uint pad1;
};

static float4 SampleBilinear(
    texture2d<float, access::read> tex,
    float u, float v,
    uint W, uint H)
{
    float px = u * float(W) - 0.5f;
    float py = v * float(H) - 0.5f;
    int x0 = clamp(int(floor(px)),     0, int(W)-1);
    int y0 = clamp(int(floor(py)),     0, int(H)-1);
    int x1 = clamp(int(floor(px))+1,  0, int(W)-1);
    int y1 = clamp(int(floor(py))+1,  0, int(H)-1);
    float fx = px - floor(px);
    float fy = py - floor(py);
    float4 c00 = tex.read(uint2(x0, y0));
    float4 c10 = tex.read(uint2(x1, y0));
    float4 c01 = tex.read(uint2(x0, y1));
    float4 c11 = tex.read(uint2(x1, y1));
    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy);
}

kernel void warp_kernel(
    texture2d<float, access::read>   srcTex  [[texture(0)]],
    texture2d<float, access::write>  dstTex  [[texture(1)]],
    device const float*              homo    [[buffer(0)]],
    device const WarpUniforms&       uni     [[buffer(1)]],
    uint2                            gid     [[thread_position_in_grid]])
{
    uint W = uni.width;
    uint H = uni.height;
    if (gid.x >= W || gid.y >= H) return;

    uint base = gid.y * 9u;
    float h00 = homo[base+0u], h01 = homo[base+1u], h02 = homo[base+2u];
    float h10 = homo[base+3u], h11 = homo[base+4u], h12 = homo[base+5u];
    float h20 = homo[base+6u], h21 = homo[base+7u], h22 = homo[base+8u];

    float ox = float(gid.x) + 0.5f;
    float oy = float(gid.y) + 0.5f;

    float sx_h = h00*ox + h01*oy + h02;
    float sy_h = h10*ox + h11*oy + h12;
    float sw   = h20*ox + h21*oy + h22;

    float srcX = sx_h / sw;
    float srcY = sy_h / sw;
    float u = srcX / float(W);
    float v = srcY / float(H);

    float4 color;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    else
        color = SampleBilinear(srcTex, u, v, W, H);

    dstTex.write(color, gid);
}
)METAL";

// ── GPU uniform block ─────────────────────────────────────────────────────────
struct WarpUniforms {
    uint32_t width, height, pad0, pad1;
};

// ── Texture helper ────────────────────────────────────────────────────────────
static id<MTLTexture> MakeTexture(id<MTLDevice> dev, int w, int h, bool write)
{
    auto* d = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                     width:(NSUInteger)w
                                    height:(NSUInteger)h
                                 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = write
        ? (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)
        :  MTLTextureUsageShaderRead;
    return [dev newTextureWithDescriptor:d];
}

// ─────────────────────────────────────────────────────────────────────────────

MetalWarpEngine::MetalWarpEngine() = default;

MetalWarpEngine::~MetalWarpEngine()
{
    mDevice = nil;
    mQueue = nil;
    mPipeline = nil;
    mHomoBuf = nil;
    mSrcTex = nil;
    mDstTex = nil;
}

bool MetalWarpEngine::Init()
{
    mDevice = MTLCreateSystemDefaultDevice();
    if (!mDevice) { NSLog(@"[BRAWGyro] No Metal device"); return false; }

    mQueue = [mDevice newCommandQueue];
    if (!mQueue) return false;

    return LoadMetalLib();
}

bool MetalWarpEngine::LoadMetalLib()
{
    // Compile shader source at runtime — no .metallib file needed
    NSString* src = [NSString stringWithUTF8String:kWarpKernelSrc];
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.fastMathEnabled = YES;

    NSError* err = nil;
    id<MTLLibrary> lib = [mDevice newLibraryWithSource:src
                                               options:opts
                                                 error:&err];
    if (!lib) {
        NSLog(@"[BRAWGyro] Shader compile failed: %@", err.localizedDescription);
        return false;
    }

    id<MTLFunction> fn = [lib newFunctionWithName:@"warp_kernel"];
    if (!fn) { NSLog(@"[BRAWGyro] warp_kernel not found"); return false; }

    mPipeline = [mDevice newComputePipelineStateWithFunction:fn error:&err];
    if (!mPipeline) {
        NSLog(@"[BRAWGyro] Pipeline error: %@", err.localizedDescription);
        return false;
    }

    mReady = true;
    NSLog(@"[BRAWGyro] Metal pipeline ready (runtime compiled)");
    return true;
}

void MetalWarpEngine::EnsureTextures(int w, int h)
{
    if (w == mTexWidth && h == mTexHeight) return;
    mSrcTex    = MakeTexture(mDevice, w, h, false);
    mDstTex    = MakeTexture(mDevice, w, h, true);
    mTexWidth  = w;
    mTexHeight = h;
}

// Copy one image, handling arbitrary (possibly negative / padded) row strides.
// Reading row r from `srcStride`-strided memory, writing tightly packed top-down.
static void PackRows(const uint8_t* src, int srcStride,
                     uint8_t* dst, int dstStride,
                     int height, size_t bytesPerRow)
{
    for (int r = 0; r < height; ++r)
        std::memcpy(dst + (ptrdiff_t)r * dstStride,
                    src + (ptrdiff_t)r * srcStride,
                    bytesPerRow);
}

void MetalWarpEngine::ApplyWarp(
    const uint8_t* src, int srcRowBytes,
          uint8_t* dst, int dstRowBytes,
    int width, int height,
    const WarpMap& warpMap)
{
    if (!src || !dst || width <= 0 || height <= 0) return;

    // RGBA/BGRA float32 = 16 bytes/pixel. The tight (unpadded) row size.
    const int      kBpp        = 16;
    const size_t   tightRB     = (size_t)width * kBpp;

    // Passthrough if the GPU isn't ready or there's no valid warp — done with
    // sign-correct strides so a bottom-up world copies cleanly (no glitch).
    if (!mReady || !warpMap.IsValid()) {
        PackRows(src, srcRowBytes, dst, dstRowBytes, height, tightRB);
        return;
    }

    // Serialize: shared textures/command queue are not safe across Premiere's
    // concurrent render threads.
    std::lock_guard<std::mutex> lock(mMutex);

    const size_t frameBytes = (size_t)height * tightRB;
    bool gpuError = false;

    // CRITICAL: drain per-frame autoreleased Metal objects (command buffer, encoder,
    // textures, etc.) every call. Without this pool, Premiere's render thread never
    // drains them and RAM grows without bound (the multi-GB leak you saw).
    @autoreleasepool {
        EnsureTextures(width, height);
        MTLRegion region = MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height);

        // ── Upload source (tight, top-down; reused staging buffer) ──────────
        if (mScratchUp.size() < frameBytes) mScratchUp.resize(frameBytes);
        PackRows(src, srcRowBytes, mScratchUp.data(), (int)tightRB, height, tightRB);
        [mSrcTex replaceRegion:region mipmapLevel:0
                     withBytes:mScratchUp.data() bytesPerRow:tightRB];

        // ── Homographies (reused GPU buffer) ────────────────────────────────
        size_t homoBytes = warpMap.rowHomographies.size() * sizeof(float);
        if (!mHomoBuf || mHomoBuf.length < homoBytes)
            mHomoBuf = [mDevice newBufferWithLength:homoBytes
                                            options:MTLResourceStorageModeShared];
        std::memcpy(mHomoBuf.contents, warpMap.rowHomographies.data(), homoBytes);

        // ── Dispatch (uniforms via setBytes — no per-frame buffer alloc) ────
        WarpUniforms uni = {(uint32_t)width, (uint32_t)height, 0, 0};
        id<MTLCommandBuffer>         cmd = [mQueue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:mPipeline];
        [enc setTexture:mSrcTex atIndex:0];
        [enc setTexture:mDstTex atIndex:1];
        [enc setBuffer:mHomoBuf offset:0 atIndex:0];
        [enc setBytes:&uni length:sizeof(uni) atIndex:1];

        MTLSize tg   = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(((NSUInteger)width+15)/16,
                                   ((NSUInteger)height+15)/16, 1);
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        if (cmd.error) {
            NSLog(@"[BRAWGyro] GPU error: %@", cmd.error.localizedDescription);
            gpuError = true;
        } else {
            // ── Read back (single bulk getBytes into reused buffer) ─────────
            if (mScratchDown.size() < frameBytes) mScratchDown.resize(frameBytes);
            [mDstTex getBytes:mScratchDown.data() bytesPerRow:tightRB
                   fromRegion:region mipmapLevel:0];
            PackRows(mScratchDown.data(), (int)tightRB, dst, dstRowBytes, height, tightRB);
        }
    }

    if (gpuError)
        PackRows(src, srcRowBytes, dst, dstRowBytes, height, tightRB);  // fail safe
}
