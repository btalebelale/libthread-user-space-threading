Le formatage peut être différent de la source
#pragma GCC target("avx2")
#pragma GCC optimize("Ofast,unroll-loops")
#include <bits/extc++.h>
#include <immintrin.h>
#include <sys/mman.h>

#ifdef NDEBUG
#define toy_assert(expr) [[assume(expr)]]
#else
#define toy_assert(expr) assert(expr)
#endif

namespace toy {

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using i128 = __int128;
using isize = intptr_t;

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using u128 = unsigned __int128;
using usize = uintptr_t;

using f16 = _Float16;
using f32 = float;
using f64 = double;
using f80 = long double;
using f128 = __float128;

} // namespace toy

using namespace toy;

// LUT1: 最高有效 4 位数字组，前导零替换为空格（空格兼作数值间分隔符）
// 例：42 → "  42" (0x20,0x20,'4','2')，0 → "    " (全空格)
static constexpr auto LUT1 = [] {
    std::array<int, 10000> res;
    for (int i = 0; i < 10000; ++i) {
        char ch[4];
        ch[0] = '0' + i / 1000;
        ch[1] = '0' + i / 100 % 10;
        ch[2] = '0' + i / 10 % 10;
        ch[3] = '0' + i % 10;
        if (i < 1000) ch[0] = ' ';
        if (i < 100) ch[1] = ' ';
        if (i < 10) ch[2] = ' ';
        if (i < 1) ch[3] = ' ';
        res[i] = ch[0] | ch[1] << 8 | ch[2] << 16 | ch[3] << 24;
    }
    return res;
}();

// LUT2: 后续 4 位数字组，前导补零
// 例：42 → "0042"，0 → "0000"
static constexpr auto LUT2 = [] {
    std::array<int, 10000> res;
    for (int i = 0; i < 10000; ++i) {
        char ch[4];
        ch[0] = '0' + i / 1000;
        ch[1] = '0' + i / 100 % 10;
        ch[2] = '0' + i / 10 % 10;
        ch[3] = '0' + i % 10;
        res[i] = ch[0] | ch[1] << 8 | ch[2] << 16 | ch[3] << 24;
    }
    return res;
}();

// pshufb 掩码表：将低 N 字节左对齐到 128 位寄存器最高位
// 替代 u128 移位 (store → __int128 shift → reload)，用单条 pshufb 实现
// mask[n][i] = (i < 16-n) ? 0x80/*输出0*/ : (i-16+n)/*源字节索引*/
static constexpr auto SHUFFLE_MASKS = [] {
    std::array<std::array<char, 16>, 16> res{};
    for (int n = 0; n < 16; ++n)
        for (int i = 0; i < 16; ++i)
            res[n][i] = (i < 16 - n) ? (char)0x80 : (char)(i - 16 + n);
    return res;
}();

int main() {
#ifdef LOCAL
  assert(freopen("test.in", "r", stdin));
  assert(freopen("test.out", "w", stdout));
#endif
    // mmap stdin (fd=0) 到内存，允许指针直接遍历输入
    char *p = (char *)mmap(nullptr, UINT_MAX, PROT_READ, MAP_PRIVATE, 0, 0);

    // ── SIMD 常量 ──
    const __m128i ascii_zero = _mm_set1_epi8('0'); // 每字节 '0'(0x30)
    // const __m128i hi_nibble_mask =
    //     _mm_set1_epi8((char)0xF0); // 高 4 位掩码，用于数字检测
    // 逐级合并权重（标准 SIMD 十进制解析链）：
    //   w1: 相邻字节 → 两位数   d0*10 + d1
    //   w2: 相邻 u16 → 四位数   dd0*100 + dd1
    //   w3: 相邻 u32 → 八位数   dddd0*10000 + dddd1
    const __m128i w1 = _mm_set1_epi16(0x010A);     // {10, 1} per byte pair
    const __m128i w2 = _mm_set1_epi32(0x00010064); // {100, 1} per u16 pair
    const __m128i w3 = _mm_set_epi32(1, 10000, 1, 10000); // 偶 u32 × 10000

    // 将 16 字节数字值 (已减'0') 解析为 u64
    // 输入 chunk = [d0,d1,...,d15]（高位在前），输出 = d0d1...d15 的整数值
    const auto parse_16_digits = [&](__m128i chunk) -> u64 {
        __m128i t1 = _mm_maddubs_epi16(chunk, w1);
        __m128i t2 = _mm_madd_epi16(t1, w2);
        __m128i prod = _mm_mul_epu32(t2, w3); // [A*10000, C*10000] (偶 u32→u64)
        __m128i odd = _mm_srli_epi64(t2, 32); // [B, D] (奇 u32 移到低位)
        __m128i t3 = _mm_add_epi64(prod, odd); // [A*10000+B, C*10000+D]
        // 第 4 级：2×u64 → 1×u64（标量合并）
        u64 r0 = _mm_cvtsi128_si64(t3);    // 前 8 位数字
        u64 r1 = _mm_extract_epi64(t3, 1); // 后 8 位数字
        return r0 * 100000000 + r1;        // 合成完整 16 位数值
    };

    // const __m128i vzero = _mm_setzero_si128();

    const auto ii = [&]() -> u64 {
        // 1) load16
        // __m128i raw = _mm_loadu_si128((const __m128i*)p);

        // 2) diff = raw - '0'
        //    digit: 0..9  => sign bit 0
        //    space/newline: negative => sign bit 1 (在你“只有数字和空格”的假设下成立)
        __m128i diff = _mm_sub_epi8(_mm_loadu_si128((const __m128i*)p), ascii_zero);

        // 3) 非数字位置：直接看 sign bit
        int nd = _mm_movemask_epi8(diff); // bit=1 => non-digit

        u64 x;
        if (nd == 0) {
            // 全 16 字节都是数字
            x = parse_16_digits(diff);
            p += 16;
            for (; *p >= 48; ++p) x = x * 10 + (*p & 15);
            ++p;
        } else {
            // 第一个 non-digit 的位置就是数字长度
            int num_digits = __builtin_ctz((u32)nd);

            // pshufb：把前 num_digits 个 digit 左对齐到高位（保持你原来的对齐方式）
            diff = _mm_shuffle_epi8(
                diff,
                _mm_loadu_si128((const __m128i*)&SHUFFLE_MASKS[num_digits])
            );

            x = parse_16_digits(diff);
            p += num_digits + 1; // 跳过数字和分隔符
        }
        return x;
    };

    // ── 输出部分：4 字节 LUT 批量写入 ──
    int bufO[1 << 17], *ptrO = bufO, *endO = bufO + sizeof(bufO) / sizeof(int);
    auto flush = [&]() {
        write(1, bufO, (ptrO - bufO) << 2);
        ptrO = bufO;
    };
    const auto copy1 = [&](u64 x) {
        *ptrO = LUT1[x], ++ptrO;
    }; // 最高组（前导空格作分隔）
    const auto copy2 = [&](u64 x) {
        *ptrO = LUT2[x], ++ptrO;
    }; // 后续组（前导补零）
    const auto oo = [&](u64 x) {
        if (endO - ptrO < 8) flush();
        if (x >= 1000000000000000) {
            u64 a = x / 100000000, b = x % 100000000;
            u64 c = a / 10000, d = a % 10000, e = b / 10000, f = b % 10000;
            u64 g = c / 10000, h = c % 10000;
            copy1(g), copy2(h), copy2(d), copy2(e), copy2(f);
        } else if (x >= 100000000000) {
            u64 a = x / 100000000, b = x % 100000000;
            u64 c = a / 10000, d = a % 10000, e = b / 10000, f = b % 10000;
            copy1(c), copy2(d), copy2(e), copy2(f);
        } else if (x >= 10000000) {
            u64 a = x / 100000000, b = x % 100000000;
            u64 c = b / 10000, d = b % 10000;
            copy1(a), copy2(c), copy2(d);
        } else if (x >= 1000) {
            u64 a = x / 10000, b = x % 10000;
            copy1(a), copy2(b);
        } else if (x > 0) {
            copy1(x);
        } else {
            *ptrO = 0x30202020, ++ptrO; // "   0"
        }
    };
    auto n = ii();
    constexpr u64 BLOCK = 1 << 14;
    u64 sums[BLOCK];
    for (u64 base = 0; base < n; base += BLOCK) {
        u64 cnt = std::min(BLOCK, n - base);
        for (u64 i = 0; i < cnt; ++i) sums[i] = ii() + ii();
        for (u64 i = 0; i < cnt; ++i) oo(sums[i]);
    }
    flush();
}
