// SPDX-License-Identifier: Apache-2.0
// Automatic exposure (phases.md P4.4): a 256-bin histogram of log2 luminance of the
// scene colour, then a percentile average adapted over time.
// f3d::render::raster::ExposureConstants.
#ifndef F3D_EXPOSURE_HLSLI
#define F3D_EXPOSURE_HLSLI

cbuffer ExposureConstants
{
    float4 g_ExposureParams; // x: log2 luminance at bin 1, y: log2 range of bins 1..255, z: adaptation when EV rises, w: when it falls
    float4 g_ExposureLimits; // x: min EV100, y: max EV100, z: manual EV100, w: 1 = automatic, 0 = manual
    float4 g_ExposureExtra;  // x: compensation (EV), y: low percentile, z: high percentile, w: 1 = snap (no adaptation)
};

#define F3D_HISTOGRAM_BINS 256

#endif // F3D_EXPOSURE_HLSLI
