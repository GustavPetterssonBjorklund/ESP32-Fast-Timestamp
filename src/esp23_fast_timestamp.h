#pragma once
#include <stdint.h>

/**
 * @file fast_timestamp.h
 * @brief Ultra-low-overhead cycle-based timing for ESP32 (Xtensa & RISC‑V) with wrap-safe comparisons.
 *
 * @details
 * This header provides a tiny API to read the CPU cycle counter and compute elapsed time with
 * minimal overhead. It hides architecture differences between ESP32 Xtensa (ESP32/S2/S3) and
 * ESP32 RISC‑V (C2/C3/C6/H2), and supplies wrap-safe comparison utilities.
 *
 * ### Timing sources (rules of thumb, single-call overhead)
 * - `millis()` .................. ~0.7–0.9 µs
 * - `micros()` .................. ~0.4–0.6 µs
 * - `esp_timer_get_time()` ...... ~0.5–0.7 µs
 * - FreeRTOS `xTaskGetTickCount` ~0.3–0.5 µs
 * - `ESP.getCycleCount()` ....... ~20–25 ns (Xtensa)
 * - Inline `CCOUNT/MCYCLE` ...... ~4–15 ns (fastest; this header)
 *
 * @note Numbers vary by board/toolchain/optimization. Use a quick micro-benchmark on your target
 *       if you need exact figures.
 *
 * @par Wrap behavior
 * - Xtensa `CCOUNT` is 32-bit and wraps every ~17.9 s @ 240 MHz.
 * - RISC‑V `mcycle` is exposed here as a tear-free 64-bit value (effectively no wrap).
 *
 * @par Frequency assumptions
 * Conversions from cycles → time assume a fixed CPU frequency. If DVFS or clock changes are
 * enabled in your firmware, prefer `esp_timer_get_time()` for real time, or rebase your measures
 * on cycle counts without converting to wall time.
 */

// ============================================================================
//  Low-level cycle counter read (architecture-specific)
// ============================================================================

#if defined(ARDUINO_ARCH_ESP32) &&                                      \
    !defined(ARDUINO_ARCH_ESP32C2) && !defined(ARDUINO_ARCH_ESP32C3) && \
    !defined(ARDUINO_ARCH_ESP32C6) && !defined(ARDUINO_ARCH_ESP32H2)

/**
 * @brief ESP32 Xtensa variant (ESP32 / S2 / S3).
 */
using fast_counter_t = uint32_t;

/**
 * @brief Read the Xtensa CCOUNT register.
 * @return Current cycle count (wraps modulo 2^32).
 *
 * @remarks Typical overhead: ~4–8 ns when inlined with -O2/-O3.
 */
static inline fast_counter_t fast_rdcycle()
{
    uint32_t c;
    asm volatile("rsr.ccount %0" : "=a"(c));
    return c;
}

#elif defined(ARDUINO_ARCH_ESP32C2) || defined(ARDUINO_ARCH_ESP32C3) || \
    defined(ARDUINO_ARCH_ESP32C6) || defined(ARDUINO_ARCH_ESP32H2)

/**
 * @brief ESP32 RISC‑V variants (C2/C3/C6/H2).
 */
using fast_counter_t = uint64_t;

/**
 * @brief Tear-free read of the 64-bit RISC‑V cycle counter.
 * @return Current 64-bit cycle count.
 *
 * @remarks Reads MCYCLEH/MCYCLE/MCYCLEH and retries if rollover detected.
 *          Typical overhead: ~12–15 ns when inlined.
 */
static inline fast_counter_t fast_rdcycle()
{
    uint32_t hi1, lo, hi2;
    do
    {
        asm volatile("csrr %0, mcycleh" : "=r"(hi1));
        asm volatile("csrr %0, mcycle" : "=r"(lo));
        asm volatile("csrr %0, mcycleh" : "=r"(hi2));
    } while (hi1 != hi2);
    return (uint64_t(hi2) << 32) | lo;
}

#else
#error "Unsupported ESP32 target. Add your arch guards here."
#endif

// ============================================================================
//  Public API
// ============================================================================

namespace fasttime
{

    /**
     * @brief Opaque timestamp backed by the CPU cycle counter.
     *
     * @note On Xtensa, @c ticks is 32-bit and wraps approximately every 17.9 s at 240 MHz.
     *       On RISC‑V variants, @c ticks is 64-bit.
     */
    struct Timestamp
    {
        fast_counter_t ticks; ///< Raw cycle count (modulo 2^N).

        /**
         * @brief Read a timestamp (single cycle-counter read).
         * @return Timestamp captured “now”.
         *
         * @remarks Overhead is the same as @ref fast_rdcycle.
         */
        static inline Timestamp now() { return Timestamp{fast_rdcycle()}; }
    };

    /**
     * @brief Wrap-safe “a before b” comparison.
     *
     * @param a Earlier candidate.
     * @param b Later candidate.
     * @return true if @p a is before @p b.
     *
     * @details
     * - Xtensa (32-bit): Uses modulo arithmetic to stay correct across wrap.
     * - RISC‑V (64-bit): Plain integer comparison.
     */
    static inline bool before(const Timestamp a, const Timestamp b)
    {
        if constexpr (sizeof(fast_counter_t) == 4)
        {
            return (int32_t)(a.ticks - b.ticks) < 0;
        }
        else
        {
            return a.ticks < b.ticks;
        }
    }

    /**
     * @brief Wrap-safe difference in cycles: @p b - @p a.
     *
     * @param a Start timestamp.
     * @param b End timestamp.
     * @return Elapsed cycles as a non-negative value.
     */
    static inline uint64_t cycles_between(const Timestamp a, const Timestamp b)
    {
        if constexpr (sizeof(fast_counter_t) == 4)
        {
            return (uint32_t)(b.ticks - a.ticks);
        }
        else
        {
            return (uint64_t)(b.ticks - a.ticks);
        }
    }

// ----------------------------------------------------------------------------
//  Frequency configuration
// ----------------------------------------------------------------------------

/**
 * @def FASTTIME_FREQ_HZ
 * @brief CPU frequency used for cycle→time conversion.
 *
 * @details
 * Defaults to @c F_CPU if defined; otherwise falls back to 240 MHz.
 * You can override with -DFASTTIME_FREQ_HZ=... in platformio.ini.
 */
#ifndef FASTTIME_FREQ_HZ
#ifdef F_CPU
#define FASTTIME_FREQ_HZ ((uint64_t)F_CPU)
#else
#define FASTTIME_FREQ_HZ (240000000ULL)
#endif
#endif

    // ----------------------------------------------------------------------------
    //  Conversions (with explicit caveats)
    // ----------------------------------------------------------------------------

    /**
     * @brief Convert cycles to microseconds.
     *
     * @param cycles Cycle count (e.g., from @ref cycles_between).
     * @return Microseconds as 64-bit integer.
     *
     * @warning This performs a 64-bit division at runtime. On microcontrollers,
     *          integer division is relatively slow (dozens of cycles). If you call
     *          this in a hot path or tight loop, prefer:
     *          - keeping results in *cycles* and only converting at the boundary, or
     *          - using a fixed-point reciprocal (multiply+shift) precomputed at init.
     *
     * @warning The conversion assumes a fixed CPU frequency (FASTTIME_FREQ_HZ). If
     *          DVFS/clock scaling is enabled, results in µs can drift. For stable
     *          wall time, use @c esp_timer_get_time() or @c micros().
     */
    static inline uint64_t cycles_to_us(uint64_t cycles)
    {
        return cycles / (FASTTIME_FREQ_HZ / 1000000ULL);
    }

    /**
     * @brief Convert cycles to milliseconds.
     *
     * @param cycles Cycle count (e.g., from @ref cycles_between).
     * @return Milliseconds as 64-bit integer.
     *
     * @warning Contains a 64-bit division. Avoid calling per-iteration in hot loops—
     *          convert once per report or use cycles directly for threshold checks.
     *
     * @warning Assumes fixed CPU frequency (see @ref cycles_to_us for DVFS caveat).
     */
    static inline uint64_t cycles_to_ms(uint64_t cycles)
    {
        return cycles / (FASTTIME_FREQ_HZ / 1000ULL);
    }

    /**
     * @brief Elapsed microseconds since @p start.
     *
     * @param start Timestamp captured earlier.
     * @return Elapsed microseconds.
     *
     * @warning Calls @ref fast_rdcycle plus a 64-bit division. For ultra‑tight loops,
     *          prefer @ref cycles_between and aggregate/convert outside the loop.
     *
     * @warning Subject to the same fixed‑frequency assumption as @ref cycles_to_us.
     */
    static inline uint64_t elapsed_us(const Timestamp start)
    {
        return cycles_to_us(cycles_between(start, Timestamp::now()));
    }

    /**
     * @brief Elapsed milliseconds since @p start.
     *
     * @param start Timestamp captured earlier.
     * @return Elapsed milliseconds.
     *
     * @warning Calls @ref fast_rdcycle plus a 64-bit division. Avoid per‑iteration use
     *          in performance‑critical sections.
     *
     * @warning Subject to the fixed‑frequency assumption (see @ref cycles_to_ms).
     */
    static inline uint64_t elapsed_ms(const Timestamp start)
    {
        return cycles_to_ms(cycles_between(start, Timestamp::now()));
    }

    // ----------------------------------------------------------------------------
    //  Fixed-point conversion helper (no division in hot path)
    // ----------------------------------------------------------------------------

    /**
     * @brief Helper holding a fixed-point reciprocal to convert cycles→µs without division.
     *
     * @details
     * Precompute once at init, then use @ref to_us to convert using multiply+shift.
     * Default scaling uses Q32.32 fixed point (scale = 2^32).
     *
     * @code
     * fasttime::UsConverter cvt = fasttime::UsConverter::make();
     * // later, in a hot loop:
     * uint64_t us = cvt.to_us(cycles_between(t0, fasttime::Timestamp::now()));
     * @endcode
     */
    struct UsConverter
    {
        uint64_t k;     ///< Fixed-point reciprocal: k ≈ (1e6 / F_HZ) * 2^32
        uint32_t shift; ///< Right shift to undo the fixed-point scale (default 32).

        /**
         * @brief Build a converter for the configured FASTTIME_FREQ_HZ.
         */
        static inline UsConverter make(uint64_t freq_hz = FASTTIME_FREQ_HZ,
                                       uint32_t q = 32)
        {
            // k = round((1e6 << q) / freq_hz)
            UsConverter c;
            c.k = ((1000000ULL << q) + (freq_hz / 2ULL)) / freq_hz;
            c.shift = q;
            return c;
        }

        /**
         * @brief Convert cycles→µs using a single 64-bit multiply + shift.
         *
         * @warning Accuracy depends on @ref FASTTIME_FREQ_HZ being correct and constant.
         * @warning Minor quantization error vs integer division is usually <1 µs.
         */
        inline uint64_t to_us(uint64_t cycles) const
        {
            return (cycles * k) >> shift;
        }
    };

} // namespace fasttime
