// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// WarpMap.h
// Per-frame warp data passed from GyroIntegrator → MetalWarpEngine.
// Stores one 3×3 homography per scanline row (rolling-shutter correction).
// For a non-RS warp all rows hold the same matrix.
#pragma once

#include <vector>
#include <array>
#include <cstring>

struct WarpMap {
    int  width  = 0;
    int  height = 0;
    bool valid  = false;

    // Row-major storage: rowHomographies[row * 9 + col]
    // Each row holds 9 floats = one 3×3 homography.
    std::vector<float> rowHomographies;

    bool IsValid() const { return valid && !rowHomographies.empty(); }

    void FillUniform(const std::array<float,9>& H, int w, int h)
    {
        width  = w;
        height = h;
        rowHomographies.resize(static_cast<size_t>(h) * 9);
        for (int r = 0; r < h; ++r)
            std::memcpy(&rowHomographies[static_cast<size_t>(r) * 9],
                        H.data(), 9 * sizeof(float));
        valid = true;
    }

    void SetRow(int row, const std::array<float,9>& H)
    {
        if (row < 0 || row >= height) return;
        std::memcpy(&rowHomographies[static_cast<size_t>(row) * 9],
                    H.data(), 9 * sizeof(float));
    }

    static WarpMap Identity(int w, int h)
    {
        WarpMap m;
        m.FillUniform({1,0,0, 0,1,0, 0,0,1}, w, h);
        return m;
    }
};
