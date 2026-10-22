#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Reversible string obfuscation, NOT cryptography or a place to store secrets.
// Release builds supply a fresh seed; standalone tests may override it.
#ifndef AIM_OBF_SEED
#define AIM_OBF_SEED 0x629f1473ba508de1ULL
#endif
#if defined(_MSC_VER)
#define AIM_OBF_NOINLINE __declspec(noinline)
#else
#define AIM_OBF_NOINLINE __attribute__((noinline))
#endif

namespace AimText {
constexpr std::uint64_t mix(std::uint64_t v) noexcept {
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    return v ^ (v >> 31);
}
constexpr std::uint64_t site(const char* file, std::uint64_t counter) noexcept {
    std::uint64_t value = AIM_OBF_SEED;
    while (*file) value = (value ^ static_cast<unsigned char>(*file++)) * 1099511628211ULL;
    return mix(value ^ counter);
}
constexpr std::uint32_t mask(std::uint64_t key, std::size_t index) noexcept {
    return static_cast<std::uint32_t>(mix(key + (index + 1) * 0x9e3779b97f4a7c15ULL));
}

template<class Char, std::size_t N, std::uint64_t Key>
struct Encoded {
    using Unit = std::make_unsigned_t<Char>;
    std::array<Unit, N> data{};
    consteval explicit Encoded(const Char (&literal)[N]) {
        for (std::size_t i = 0; i < N; ++i)
            data[i] = static_cast<Unit>(static_cast<Unit>(literal[i]) ^ mask(Key, i));
    }
};

template<class Char, std::size_t N>
class Plain {
    std::array<Char, N> data_{};
public:
    template<std::uint64_t Key>
    AIM_OBF_NOINLINE explicit Plain(const Encoded<Char, N, Key>& encoded) noexcept {
        // Volatile reads prevent /O2 + /GL from replacing this with plaintext.
        const volatile typename Encoded<Char, N, Key>::Unit* source = encoded.data.data();
        for (std::size_t i = 0; i < N; ++i)
            data_[i] = static_cast<Char>(source[i] ^ mask(Key, i));
    }
    Plain(const Plain&) = delete;
    Plain& operator=(const Plain&) = delete;
    AIM_OBF_NOINLINE ~Plain() noexcept {
        volatile Char* p = data_.data();
        for (std::size_t i = 0; i < N; ++i) p[i] = Char{};
    }
    const Char* c_str() const noexcept { return data_.data(); }
    constexpr std::size_t size() const noexcept { return N - 1; }
};

template<std::uint64_t Key, class Char, std::size_t N>
consteval auto encode(const Char (&literal)[N]) { return Encoded<Char, N, Key>(literal); }
template<class Char, std::size_t N, std::uint64_t Key>
auto decode(const Encoded<Char, N, Key>& encoded) noexcept { return Plain<Char, N>(encoded); }
} // namespace AimText

// Scoped owner. Its pointer must not escape the full expression / owner lifetime.
#define AIM_TEXT_OWNER(literal) ([] { \
    static constexpr auto encoded = ::AimText::encode<::AimText::site(__FILE__, __COUNTER__)>(literal); \
    return ::AimText::decode(encoded); \
}())
#define AIM_TEXT_TEMP(literal) (AIM_TEXT_OWNER(literal).c_str())

// Explicit persistent form for APIs retaining pointers (ImGui labels, arrays).
// Thread-safe C++ local-static initialization; plaintext persists until shutdown.
#define AIM_TEXT(literal) ([] { \
    static constexpr auto encoded = ::AimText::encode<::AimText::site(__FILE__, __COUNTER__)>(literal); \
    static const auto plain = ::AimText::decode(encoded); \
    return plain.c_str(); \
}())
