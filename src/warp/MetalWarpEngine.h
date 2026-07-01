// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// MetalWarpEngine.h
// GPU warp engine: applies per-row homographies to a video frame using Metal.
// One instance per plugin instance; textures are lazily allocated and reused.
#pragma once

#include "../gyro/WarpMap.h"
#include <cstdint>
#include <mutex>
#include <vector>

// Metal types forward-declared to keep this header pure C++
// (implementation is Objective-C++ in MetalWarpEngine.mm)
#ifdef __OBJC__
#import <Metal/Metal.h>
#else
using MTLDevice_t              = void;
using MTLCommandQueue_t        = void;
using MTLComputePipelineState_t = void;
using MTLBuffer_t              = void;
using MTLTexture_t             = void;
#endif

class MetalWarpEngine {
public:
    MetalWarpEngine();
    ~MetalWarpEngine();

    // Must be called once before ApplyWarp. Returns false on failure.
    bool Init();

    // Apply the per-row homography warp on the GPU.
    // Pixel format expected: BGRA float32 (PrPixelFormat_BGRA_4444_32f_Linear).
    // srcRowBytes / dstRowBytes may differ (Premiere aligns rows).
    void ApplyWarp(
        const uint8_t* src, int srcRowBytes,
              uint8_t* dst, int dstRowBytes,
        int width, int height,
        const WarpMap& warpMap);

private:
    bool LoadMetalLib();
    void EnsureTextures(int width, int height);

#ifdef __OBJC__
    id<MTLDevice>                mDevice   = nil;
    id<MTLCommandQueue>          mQueue    = nil;
    id<MTLComputePipelineState>  mPipeline = nil;
    id<MTLBuffer>                mHomoBuf  = nil;
    id<MTLTexture>               mSrcTex   = nil;
    id<MTLTexture>               mDstTex   = nil;
#else
    void* mDevice   = nullptr;
    void* mQueue    = nullptr;
    void* mPipeline = nullptr;
    void* mHomoBuf  = nullptr;
    void* mSrcTex   = nullptr;
    void* mDstTex   = nullptr;
#endif

    int  mTexWidth  = 0;
    int  mTexHeight = 0;
    bool mReady     = false;

    // Premiere may call the effect from several render threads at once; the textures
    // and command buffer below are shared, so every ApplyWarp is serialized.
    std::mutex mMutex;

    // Reused CPU staging buffers (avoid allocating/freeing ~hundreds of MB per frame).
    std::vector<uint8_t> mScratchUp;
    std::vector<uint8_t> mScratchDown;
};
