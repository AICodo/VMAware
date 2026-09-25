#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include "types.hpp"

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

struct sha256 {
    u8 buf[64] = {};
    u32 len = 0;
    u64 bits = 0;
    u32 s[8] = {};

    sha256() noexcept;

    static constexpr u32 rotr(const u32 x, const u32 n) noexcept;
    static constexpr u32 ch(const u32 x, const u32 y, const u32 z) noexcept;
    static constexpr u32 maj(const u32 x, const u32 y, const u32 z) noexcept;
    constexpr static u32 ep0(const u32 x) noexcept;
    constexpr static u32 ep1(const u32 x) noexcept;
    constexpr static u32 sig0(const u32 x) noexcept;
    constexpr static u32 sig1(const u32 x) noexcept;

    void transform() noexcept;

    void update(const u8* data, const size_t n);

    void final(u8 out[32]);
};

std::string exe_path();
std::string compute_self_sha256();