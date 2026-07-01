// Copyright (c) 2026 Niko Klishchenko. All rights reserved.
// BrawPath.h — tiny path helpers shared by the GPU and AE-compat translation units.
#pragma once
#include <string>
#include <cctype>

// Case-insensitive ".braw" substring test.
inline bool LooksLikeBraw(const std::string& s)
{
    std::string low = s;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    return low.find(".braw") != std::string::npos;
}

// File name portion of a path ("/a/b/c.braw" -> "c.braw").
inline std::string BaseName(const std::string& p)
{
    size_t s = p.find_last_of("/\\");
    return (s == std::string::npos) ? p : p.substr(s + 1);
}
