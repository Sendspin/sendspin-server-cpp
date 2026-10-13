// SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
// Copyright (c) 2025-2026 Kevin Ahrendt
// Kaiser table code: Copyright (c) 2007-2008 Jean-Marc Valin, 2008 Thorvald Natvig (BSD-3-Clause)
//   from Speex DSP library resample.c

/// @file kaiser_tables.h
/// @brief Precomputed Kaiser window tables and cubic interpolation lookup
///
/// Stores Kaiser window sample tables for beta = 6, 8, 10, and 12 in double,
/// float, and Q30 fixed-point formats (ported from the Speex DSP resampler),
/// plus interpolate(), interpolate_f(), and interpolate_kaiser_q30_cubic_*()
/// functions that evaluate the window at an arbitrary position via cubic
/// interpolation.

#ifndef KAISER_TABLES_H
#define KAISER_TABLES_H

#include <cstdint>

// ============================================================================
// Pre-computed Kaiser Window Tables
// ============================================================================
// These tables store pre-computed Kaiser window values for common beta values.
// Using table lookup with cubic interpolation is faster than computing Bessel I0
// directly, and is especially useful for embedded platforms.

namespace micro_resampler {
namespace kaiser_tables {

// Kaiser window table for beta=6.0 from Speex DSP (kaiser6_table)
// 36 values with 32x oversampling for cubic interpolation
static const double KAISER_BETA6_TABLE[36] = {
    0.99733006, 1.00000000, 0.99733006, 0.98935595, 0.97618418, 0.95799003, 0.93501423, 0.90755855,
    0.87598009, 0.84068475, 0.80211977, 0.76076565, 0.71712752, 0.67172623, 0.62508937, 0.57774224,
    0.53019925, 0.48295561, 0.43647969, 0.39120616, 0.34752997, 0.30580127, 0.26632152, 0.22934058,
    0.19505503, 0.16360756, 0.13508755, 0.10953262, 0.08693120, 0.06722600, 0.05031820, 0.03607231,
    0.02432151, 0.01487334, 0.00752000, 0.00000000};

// Kaiser window table for beta=8.0 from Speex DSP (kaiser8_table)
// 36 values with 32x oversampling for cubic interpolation
static const double KAISER_BETA8_TABLE[36] = {
    0.99635258, 1.00000000, 0.99635258, 0.98548012, 0.96759014, 0.94302200, 0.91223751, 0.87580811,
    0.83439927, 0.78875245, 0.73966538, 0.68797126, 0.63451750, 0.58014482, 0.52566725, 0.47185369,
    0.41941150, 0.36897272, 0.32108304, 0.27619388, 0.23465776, 0.19672670, 0.16255380, 0.13219758,
    0.10562887, 0.08273982, 0.06335451, 0.04724088, 0.03412321, 0.02369490, 0.01563093, 0.00959968,
    0.00527363, 0.00233883, 0.00050000, 0.00000000};

// Kaiser window table for beta=10.0 from Speex DSP (kaiser10_table)
// 36 values with 32x oversampling for cubic interpolation
static const double KAISER_BETA10_TABLE[36] = {
    0.99537781, 1.00000000, 0.99537781, 0.98162644, 0.95908712, 0.92831446, 0.89005583, 0.84522401,
    0.79486424, 0.74011713, 0.68217934, 0.62226347, 0.56155915, 0.50119680, 0.44221549, 0.38553619,
    0.33194107, 0.28205962, 0.23636152, 0.19515633, 0.15859932, 0.12670280, 0.09935205, 0.07632451,
    0.05731132, 0.04193980, 0.02979584, 0.02044510, 0.01345224, 0.00839739, 0.00488951, 0.00257636,
    0.00115101, 0.00035515, 0.00000000, 0.00000000};

// Kaiser window table for beta=12.0 from Speex DSP (kaiser12_table)
// 68 values with 64x oversampling for cubic interpolation
static const double KAISER_BETA12_TABLE[68] = {
    0.99859849, 1.00000000, 0.99859849,   0.99440475, 0.98745105, 0.97779076, 0.96549770,
    0.95066529, 0.93340547, 0.91384741,   0.89213598, 0.86843014, 0.84290116, 0.81573067,
    0.78710866, 0.75723148, 0.72629970,   0.69451601, 0.66208321, 0.62920216, 0.59606986,
    0.56287762, 0.52980938, 0.49704014,   0.46473455, 0.43304576, 0.40211431, 0.37206735,
    0.34301800, 0.31506490, 0.28829195,   0.26276832, 0.23854851, 0.21567274, 0.19416736,
    0.17404546, 0.15530766, 0.13794294,   0.12192957, 0.10723616, 0.09382272, 0.08164178,
    0.07063950, 0.06075685, 0.05193064,   0.04409466, 0.03718069, 0.03111947, 0.02584161,
    0.02127838, 0.01736250, 0.01402878,   0.01121463, 0.00886058, 0.00691064, 0.00531256,
    0.00401805, 0.00298291, 0.00216702,   0.00153438, 0.00105297, 0.00069463, 0.00043489,
    0.00025272, 0.00013031, 0.0000527734, 0.00001000, 0.00000000};

constexpr int OVERSAMPLE_BETA6 = 32;
constexpr int TABLE_LENGTH_BETA6 = 36;

constexpr int OVERSAMPLE_BETA8 = 32;
constexpr int TABLE_LENGTH_BETA8 = 36;

constexpr int OVERSAMPLE_BETA10 = 32;
constexpr int TABLE_LENGTH_BETA10 = 36;

constexpr int OVERSAMPLE_BETA12 = 64;
constexpr int TABLE_LENGTH_BETA12 = 68;

// Float versions of Kaiser tables for memory-constrained platforms
// Same values as double tables but stored as float (50% memory savings)
static const float KAISER_BETA6_TABLE_F[36] = {
    0.99733006F, 1.00000000F, 0.99733006F, 0.98935595F, 0.97618418F, 0.95799003F,
    0.93501423F, 0.90755855F, 0.87598009F, 0.84068475F, 0.80211977F, 0.76076565F,
    0.71712752F, 0.67172623F, 0.62508937F, 0.57774224F, 0.53019925F, 0.48295561F,
    0.43647969F, 0.39120616F, 0.34752997F, 0.30580127F, 0.26632152F, 0.22934058F,
    0.19505503F, 0.16360756F, 0.13508755F, 0.10953262F, 0.08693120F, 0.06722600F,
    0.05031820F, 0.03607231F, 0.02432151F, 0.01487334F, 0.00752000F, 0.00000000F};

static const float KAISER_BETA8_TABLE_F[36] = {
    0.99635258F, 1.00000000F, 0.99635258F, 0.98548012F, 0.96759014F, 0.94302200F,
    0.91223751F, 0.87580811F, 0.83439927F, 0.78875245F, 0.73966538F, 0.68797126F,
    0.63451750F, 0.58014482F, 0.52566725F, 0.47185369F, 0.41941150F, 0.36897272F,
    0.32108304F, 0.27619388F, 0.23465776F, 0.19672670F, 0.16255380F, 0.13219758F,
    0.10562887F, 0.08273982F, 0.06335451F, 0.04724088F, 0.03412321F, 0.02369490F,
    0.01563093F, 0.00959968F, 0.00527363F, 0.00233883F, 0.00050000F, 0.00000000F};

static const float KAISER_BETA10_TABLE_F[36] = {
    0.99537781F, 1.00000000F, 0.99537781F, 0.98162644F, 0.95908712F, 0.92831446F,
    0.89005583F, 0.84522401F, 0.79486424F, 0.74011713F, 0.68217934F, 0.62226347F,
    0.56155915F, 0.50119680F, 0.44221549F, 0.38553619F, 0.33194107F, 0.28205962F,
    0.23636152F, 0.19515633F, 0.15859932F, 0.12670280F, 0.09935205F, 0.07632451F,
    0.05731132F, 0.04193980F, 0.02979584F, 0.02044510F, 0.01345224F, 0.00839739F,
    0.00488951F, 0.00257636F, 0.00115101F, 0.00035515F, 0.00000000F, 0.00000000F};

static const float KAISER_BETA12_TABLE_F[68] = {
    0.99859849F, 1.00000000F, 0.99859849F,   0.99440475F, 0.98745105F, 0.97779076F, 0.96549770F,
    0.95066529F, 0.93340547F, 0.91384741F,   0.89213598F, 0.86843014F, 0.84290116F, 0.81573067F,
    0.78710866F, 0.75723148F, 0.72629970F,   0.69451601F, 0.66208321F, 0.62920216F, 0.59606986F,
    0.56287762F, 0.52980938F, 0.49704014F,   0.46473455F, 0.43304576F, 0.40211431F, 0.37206735F,
    0.34301800F, 0.31506490F, 0.28829195F,   0.26276832F, 0.23854851F, 0.21567274F, 0.19416736F,
    0.17404546F, 0.15530766F, 0.13794294F,   0.12192957F, 0.10723616F, 0.09382272F, 0.08164178F,
    0.07063950F, 0.06075685F, 0.05193064F,   0.04409466F, 0.03718069F, 0.03111947F, 0.02584161F,
    0.02127838F, 0.01736250F, 0.01402878F,   0.01121463F, 0.00886058F, 0.00691064F, 0.00531256F,
    0.00401805F, 0.00298291F, 0.00216702F,   0.00153438F, 0.00105297F, 0.00069463F, 0.00043489F,
    0.00025272F, 0.00013031F, 0.0000527734F, 0.00001000F, 0.00000000F};

// ============================================================================
// Q30 Fixed-Point Kaiser Tables
// ============================================================================
// Pre-computed Kaiser window values in Q30 format for pure fixed-point
// coefficient generation. Q30 allows exact representation of 1.0 (= 2^30).
// Values in [0.0, 1.0] map to [0, 1073741824].

// Kaiser window table for beta=6.0 in Q30 format
// Generated from KAISER_BETA6_TABLE: value_q30 = round(value_double * 2^30)
static const int32_t KAISER_BETA6_TABLE_Q30[36] = {
    1070874998, 1073741824, 1070874998, 1062312862, 1048169782, 1028633962, 1003963885, 974483573,
    940576460,  902678377,  861269545,  816865897,  770009811,  721260547,  671184600,  620346007,
    569297110,  518569638,  468666498,  420054416,  373157464,  328351613,  285960555,  246252573,
    209438744,  175672280,  145049152,  117609755,  93341665,   72183368,   54028756,   38732348,
    26115023,   15970127,   8074539,    0};

// Kaiser window table for beta=8.0 in Q30 format
// Generated from KAISER_BETA8_TABLE: value_q30 = round(value_double * 2^30)
static const int32_t KAISER_BETA8_TABLE_Q30[36] = {
    1069825437, 1073741824, 1069825437, 1058151222, 1038942002, 1012562162, 979507568, 940391798,
    895929394,  846916494,  794209654,  738703516,  681307978,  622925757,  564430912, 506649042,
    450339669,  396181441,  344760289,  296560920,  251961851,  211233686,  174540814, 141946071,
    113418136,  88841205,   68026387,   50724509,   36639518,   25442205,   16783583,  10307578,
    5662517,    2511300,    536871,     0};

// Kaiser window table for beta=10.0 in Q30 format
// Generated from KAISER_BETA10_TABLE: value_q30 = round(value_double * 2^30)
static const int32_t KAISER_BETA10_TABLE_Q30[36] = {
    1068778785, 1073741824, 1068778785, 1054013364, 1029811954, 996770062, 955690170, 907552370,
    853478979,  794694717,  732484489,  668150313,  602969546,  538155966, 474825267, 413966332,
    356419010,  302859211,  253791250,  209547514,  170294723,  136046096, 106678451, 81952819,
    61537561,   45032517,   31993040,   21952759,   14444233,   9016629,   5250071,   2766345,
    1235888,    381339,     0,          0};

// Kaiser window table for beta=12.0 in Q30 format
// Generated from KAISER_BETA12_TABLE: value_q30 = round(value_double * 2^30)
static const int32_t KAISER_BETA12_TABLE_Q30[68] = {
    1072236964, 1073741824, 1072236964, 1067733970, 1060267492, 1049894834, 1036695261, 1020769082,
    1002236492, 981236185,  957923714,  932469763,  905058229,  875884137,  845151488,  813071111,
    779858365,  745730887,  710906434,  675600675,  640025139,  604385242,  568878490,  533692787,
    499004923,  464979344,  431766953,  399504275,  368312773,  338298360,  309551124,  282145335,
    256139512,  231576841,  208485615,  186879890,  166760330,  148115104,  130920879,  115143950,
    100741379,  87662194,   75848586,   65237171,   55760100,   47346281,   39922462,   33414276,
    27747217,   22847487,   18642842,   15063288,   12041617,   9513975,    7420243,    5704318,
    4314348,    3202875,    2326820,    1647528,    1130618,    745853,     466960,     271356,
    139919,     56665,      10737,      0};

// Cubic interpolation for Q30 Kaiser table (beta=6, 8, 10 with 32x oversampling)
// Input: abs_alpha_q31 in Q31 format (alpha ∈ [0, 1] maps to [0, 2^31])
// Output: Kaiser window value in Q30 format
// Uses MMSE-optimal cubic interpolation with 32x oversampling
//
// Note: The table is centered at index 1 (where value = 1.0, at alpha = 0).
// Index 0 corresponds to alpha = -1/32, index 1 to alpha = 0, index 33 to alpha = 1.
// Cubic interpolation requires 4 samples: table[ind], table[ind+1], table[ind+2], table[ind+3]
// When frac=0, interp[1]=1 and others=0, so result = table[ind+1].
inline int32_t interpolate_kaiser_q30_cubic_32x(int32_t abs_alpha_q31, const int32_t* table,
                                                int table_length) {
    // Clamp to valid range [0, 2^31 - 1]
    if (abs_alpha_q31 < 0) {
        abs_alpha_q31 = 0;
    }

    // Scale alpha by oversample factor: y = alpha * 32
    // alpha is in Q31, so (alpha >> 26) gives 5 integer bits (0-32)
    constexpr int32_t INDEX_SHIFT_32X = 26;  // Q31 -> 5 integer index bits (32x oversampling)
    int32_t ind = abs_alpha_q31 >> INDEX_SHIFT_32X;

    // Clamp index to valid table range (need 4 samples for cubic interpolation)
    // Maximum valid ind is table_length - 4
    if (ind > table_length - 4) {
        // At the edge, return the last value (essentially 0)
        return table[table_length - 1];
    }

    // Fractional part in Q26 (lower 26 bits)
    // This represents frac in [0, 1) with 26 fractional bits
    constexpr int32_t FRAC_MASK_Q26 = 0x03FFFFFF;  // mask for lower 26 fractional bits (Q26)
    int32_t frac_q26 = abs_alpha_q31 & FRAC_MASK_Q26;

    // Compute frac^2 and frac^3 in appropriate Q formats
    // frac_q26 * frac_q26 gives Q52, shift right by 22 to get Q30
    constexpr int32_t FRAC2_Q52_TO_Q30_SHIFT_32X = 22;  // Q52 -> Q30 shift for frac^2 term
    int64_t frac2_q30 = (static_cast<int64_t>(frac_q26) * frac_q26) >> FRAC2_Q52_TO_Q30_SHIFT_32X;
    // frac2_q30 * frac_q26 gives Q56, shift right by 26 to get Q30
    constexpr int32_t FRAC3_Q56_TO_Q30_SHIFT_32X = 26;  // Q56 -> Q30 shift for frac^3 term
    int64_t frac3_q30 = (frac2_q30 * frac_q26) >> FRAC3_Q56_TO_Q30_SHIFT_32X;

    // Cubic interpolation coefficients in Q30
    // Constants: 1/6 = 178956971 (Q30), 1/3 = 357913941 (Q30), 1/2 = 536870912 (Q30)
    constexpr int64_t ONE_SIXTH_Q30 = 178956971;  // round(2^30 / 6)
    constexpr int64_t ONE_THIRD_Q30 = 357913941;  // round(2^30 / 3)
    constexpr int64_t ONE_HALF_Q30 = 536870912;   // 2^30 / 2
    constexpr int64_t ONE_Q30 = 1073741824;       // 2^30
    constexpr int Q30_FRAC_BITS = 30;  // fractional bit width of Q30 (Q60<->Q30 conversions)

    // Compute interpolation coefficients in Q30:
    // interp[3] = -1/6 * frac + 1/6 * frac^3
    // interp[2] = frac + 1/2 * frac^2 - 1/2 * frac^3
    // interp[0] = -1/3 * frac + 1/2 * frac^2 - 1/6 * frac^3
    // interp[1] = 1 - interp[3] - interp[2] - interp[0]

    // Convert frac_q26 to Q30 for consistent arithmetic
    int64_t frac_q30 = static_cast<int64_t>(frac_q26) << 4;

    // Accumulate in Q60 to preserve precision, then do a single shift at the end
    // interp[3] = -1/6 * frac + 1/6 * frac^3
    int64_t interp3_q60 = (-ONE_SIXTH_Q30 * frac_q30) + (ONE_SIXTH_Q30 * frac3_q30);
    int64_t interp3_q30 = interp3_q60 >> Q30_FRAC_BITS;

    // interp[2] = frac + 1/2 * frac^2 - 1/2 * frac^3
    // Scale frac_q30 to Q60 to match the other terms
    int64_t interp2_q60 =
        (frac_q30 << Q30_FRAC_BITS) + (ONE_HALF_Q30 * frac2_q30) - (ONE_HALF_Q30 * frac3_q30);
    int64_t interp2_q30 = interp2_q60 >> Q30_FRAC_BITS;

    // interp[0] = -1/3 * frac + 1/2 * frac^2 - 1/6 * frac^3
    int64_t interp0_q60 =
        (-ONE_THIRD_Q30 * frac_q30) + (ONE_HALF_Q30 * frac2_q30) - (ONE_SIXTH_Q30 * frac3_q30);
    int64_t interp0_q30 = interp0_q60 >> Q30_FRAC_BITS;

    // interp[1] = 1 - interp[3] - interp[2] - interp[0]
    int64_t interp1_q30 = ONE_Q30 - interp3_q30 - interp2_q30 - interp0_q30;

    // Get table values (Q30)
    int64_t y0 = table[ind];
    int64_t y1 = table[ind + 1];
    int64_t y2 = table[ind + 2];
    int64_t y3 = table[ind + 3];

    // Compute interpolated result
    // interp * table gives Q60, shift by 30 to get Q30
    int64_t result = (interp0_q30 * y0 + interp1_q30 * y1 + interp2_q30 * y2 + interp3_q30 * y3) >>
                     Q30_FRAC_BITS;

    // Clamp result to valid Q30 range [0, 2^30]
    if (result < 0) {
        result = 0;
    }
    if (result > ONE_Q30) {
        result = ONE_Q30;
    }

    return static_cast<int32_t>(result);
}

// Cubic interpolation for Q30 Kaiser table (beta=12 with 64x oversampling)
// Input: abs_alpha_q31 in Q31 format (alpha ∈ [0, 1] maps to [0, 2^31])
// Output: Kaiser window value in Q30 format
// Uses MMSE-optimal cubic interpolation with 64x oversampling
inline int32_t interpolate_kaiser_q30_cubic_64x(int32_t abs_alpha_q31, const int32_t* table,
                                                int table_length) {
    // Clamp to valid range [0, 2^31 - 1]
    if (abs_alpha_q31 < 0) {
        abs_alpha_q31 = 0;
    }

    // Scale alpha by oversample factor: y = alpha * 64
    // alpha is in Q31, so (alpha >> 25) gives 6 integer bits (0-64)
    constexpr int32_t INDEX_SHIFT_64X = 25;  // Q31 -> 6 integer index bits (64x oversampling)
    int32_t ind = abs_alpha_q31 >> INDEX_SHIFT_64X;

    // Clamp index to valid table range (need 4 samples for cubic interpolation)
    // Maximum valid ind is table_length - 4
    if (ind > table_length - 4) {
        // At the edge, return the last value (essentially 0)
        return table[table_length - 1];
    }

    // Fractional part in Q25 (lower 25 bits)
    // This represents frac in [0, 1) with 25 fractional bits
    constexpr int32_t FRAC_MASK_Q25 = 0x01FFFFFF;  // mask for lower 25 fractional bits (Q25)
    int32_t frac_q25 = abs_alpha_q31 & FRAC_MASK_Q25;

    // Compute frac^2 and frac^3 in appropriate Q formats
    // frac_q25 * frac_q25 gives Q50, shift right by 20 to get Q30
    constexpr int32_t FRAC2_Q50_TO_Q30_SHIFT_64X = 20;  // Q50 -> Q30 shift for frac^2 term
    int64_t frac2_q30 = (static_cast<int64_t>(frac_q25) * frac_q25) >> FRAC2_Q50_TO_Q30_SHIFT_64X;
    // frac2_q30 * frac_q25 gives Q55, shift right by 25 to get Q30
    constexpr int32_t FRAC3_Q55_TO_Q30_SHIFT_64X = 25;  // Q55 -> Q30 shift for frac^3 term
    int64_t frac3_q30 = (frac2_q30 * frac_q25) >> FRAC3_Q55_TO_Q30_SHIFT_64X;

    // Cubic interpolation coefficients in Q30
    // Constants: 1/6 = 178956971 (Q30), 1/3 = 357913941 (Q30), 1/2 = 536870912 (Q30)
    constexpr int64_t ONE_SIXTH_Q30 = 178956971;  // round(2^30 / 6)
    constexpr int64_t ONE_THIRD_Q30 = 357913941;  // round(2^30 / 3)
    constexpr int64_t ONE_HALF_Q30 = 536870912;   // 2^30 / 2
    constexpr int64_t ONE_Q30 = 1073741824;       // 2^30
    constexpr int Q30_FRAC_BITS = 30;  // fractional bit width of Q30 (Q60<->Q30 conversions)

    // Convert frac_q25 to Q30 for consistent arithmetic
    int64_t frac_q30 = static_cast<int64_t>(frac_q25) << 5;

    // Accumulate in Q60 to preserve precision, then do a single shift at the end
    // interp[3] = -1/6 * frac + 1/6 * frac^3
    int64_t interp3_q60 = (-ONE_SIXTH_Q30 * frac_q30) + (ONE_SIXTH_Q30 * frac3_q30);
    int64_t interp3_q30 = interp3_q60 >> Q30_FRAC_BITS;

    // interp[2] = frac + 1/2 * frac^2 - 1/2 * frac^3
    // Scale frac_q30 to Q60 to match the other terms
    int64_t interp2_q60 =
        (frac_q30 << Q30_FRAC_BITS) + (ONE_HALF_Q30 * frac2_q30) - (ONE_HALF_Q30 * frac3_q30);
    int64_t interp2_q30 = interp2_q60 >> Q30_FRAC_BITS;

    // interp[0] = -1/3 * frac + 1/2 * frac^2 - 1/6 * frac^3
    int64_t interp0_q60 =
        (-ONE_THIRD_Q30 * frac_q30) + (ONE_HALF_Q30 * frac2_q30) - (ONE_SIXTH_Q30 * frac3_q30);
    int64_t interp0_q30 = interp0_q60 >> Q30_FRAC_BITS;

    // interp[1] = 1 - interp[3] - interp[2] - interp[0]
    int64_t interp1_q30 = ONE_Q30 - interp3_q30 - interp2_q30 - interp0_q30;

    // Get table values (Q30)
    int64_t y0 = table[ind];
    int64_t y1 = table[ind + 1];
    int64_t y2 = table[ind + 2];
    int64_t y3 = table[ind + 3];

    // Compute interpolated result
    // interp * table gives Q60, shift by 30 to get Q30
    int64_t result = (interp0_q30 * y0 + interp1_q30 * y1 + interp2_q30 * y2 + interp3_q30 * y3) >>
                     Q30_FRAC_BITS;

    // Clamp result to valid Q30 range [0, 2^30]
    if (result < 0) {
        result = 0;
    }
    if (result > ONE_Q30) {
        result = ONE_Q30;
    }

    return static_cast<int32_t>(result);
}

// ============================================================================
// Cubic Interpolation Functions
// ============================================================================
// These functions evaluate the Kaiser window at arbitrary positions using
// cubic interpolation from the pre-computed tables.

// Cubic interpolation to evaluate table at position x ∈ [0, 1]
// Matches Speex DSP compute_func() implementation
inline double interpolate(double x, const double* table, int oversample, int table_length) {
    if (x < 0.0) {
        x = 0.0;
    }
    if (x > 1.0) {
        x = 1.0;
    }

    double y = x * oversample;
    int ind = static_cast<int>(y);
    double frac = y - ind;

    // Clamp to valid table range (need 4 samples for cubic interpolation)
    if (ind > table_length - 4) {
        ind = table_length - 4;
        frac = 1.0;
    }

    // Cubic interpolation coefficients (MMSE-optimal)
    double interp[4];
    interp[3] = -0.1666666667 * frac + 0.1666666667 * (frac * frac * frac);
    interp[2] = frac + 0.5 * (frac * frac) - 0.5 * (frac * frac * frac);
    interp[0] = -0.3333333333 * frac + 0.5 * (frac * frac) - 0.1666666667 * (frac * frac * frac);
    interp[1] = 1.0 - interp[3] - interp[2] - interp[0];

    return interp[0] * table[ind] + interp[1] * table[ind + 1] + interp[2] * table[ind + 2] +
           interp[3] * table[ind + 3];
}

// Pure float interpolation using float tables (most memory efficient)
inline float interpolate_f(float x, const float* table, int oversample, int table_length) {
    if (x < 0.0F) {
        x = 0.0F;
    }
    if (x > 1.0F) {
        x = 1.0F;
    }

    float y = x * static_cast<float>(oversample);
    int ind = static_cast<int>(y);
    float frac = y - static_cast<float>(ind);

    if (ind > table_length - 4) {
        ind = table_length - 4;
        frac = 1.0F;
    }

    // Cubic interpolation coefficients (MMSE-optimal) in float
    float frac2 = frac * frac;
    float frac3 = frac2 * frac;
    float interp[4];
    interp[3] = -0.1666666667F * frac + 0.1666666667F * frac3;
    interp[2] = frac + 0.5F * frac2 - 0.5F * frac3;
    interp[0] = -0.3333333333F * frac + 0.5F * frac2 - 0.1666666667F * frac3;
    interp[1] = 1.0F - interp[3] - interp[2] - interp[0];

    return interp[0] * table[ind] + interp[1] * table[ind + 1] + interp[2] * table[ind + 2] +
           interp[3] * table[ind + 3];
}

}  // namespace kaiser_tables
}  // namespace micro_resampler

#endif  // KAISER_TABLES_H
