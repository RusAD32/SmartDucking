#pragma once

namespace smart_ducking::parameter_ids
{
inline constexpr auto crossoverHz = "crossoverHz";
inline constexpr auto duckDepthDb = "duckDepthDb";
inline constexpr auto attackMs = "attackMs";
inline constexpr auto holdMs = "holdMs";
inline constexpr auto releaseMs = "releaseMs";
inline constexpr auto lookaheadMs = "lookaheadMs";
inline constexpr auto sidechainPreGainDb = "sidechainPreGainDb";
inline constexpr auto activityThresholdDb = "activityThresholdDb";
inline constexpr auto spectralAmountDb = "spectralAmountDb";
inline constexpr auto spectralLowHz = "spectralLowHz";
inline constexpr auto spectralHighHz = "spectralHighHz";
inline constexpr auto weightFrequencyHz = "weightFrequencyHz";
inline constexpr auto weightGainDb = "weightGainDb";
inline constexpr auto weightQ = "weightQ";
inline constexpr auto fftResolution = "fftResolution";
inline constexpr auto processingMode = "processingMode";
inline constexpr auto mainFormat = "mainFormat";
inline constexpr auto voiceFormat = "voiceFormat";
inline constexpr auto vlcMode = "vlcMode";
inline constexpr auto downmix = "downmix";
inline constexpr auto dropLfe = "dropLfe";

inline constexpr auto matrixLL = "matrixLL";
inline constexpr auto matrixLC = "matrixLC";
inline constexpr auto matrixLR = "matrixLR";
inline constexpr auto matrixLLs = "matrixLLs";
inline constexpr auto matrixLRs = "matrixLRs";
inline constexpr auto matrixCL = "matrixCL";
inline constexpr auto matrixCC = "matrixCC";
inline constexpr auto matrixCR = "matrixCR";
inline constexpr auto matrixCLs = "matrixCLs";
inline constexpr auto matrixCRs = "matrixCRs";
inline constexpr auto matrixRL = "matrixRL";
inline constexpr auto matrixRC = "matrixRC";
inline constexpr auto matrixRR = "matrixRR";
inline constexpr auto matrixRLs = "matrixRLs";
inline constexpr auto matrixRRs = "matrixRRs";
inline constexpr auto matrixLsL = "matrixLsL";
inline constexpr auto matrixLsC = "matrixLsC";
inline constexpr auto matrixLsR = "matrixLsR";
inline constexpr auto matrixLsLs = "matrixLsLs";
inline constexpr auto matrixLsRs = "matrixLsRs";
inline constexpr auto matrixRsL = "matrixRsL";
inline constexpr auto matrixRsC = "matrixRsC";
inline constexpr auto matrixRsR = "matrixRsR";
inline constexpr auto matrixRsLs = "matrixRsLs";
inline constexpr auto matrixRsRs = "matrixRsRs";
} // namespace smart_ducking::parameter_ids
