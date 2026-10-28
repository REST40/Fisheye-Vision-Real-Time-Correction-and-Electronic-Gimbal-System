#ifndef FPTZ_CAMERA_H
#define FPTZ_CAMERA_H
#include "fptz_geometry.h"
/* Generated from the supplied parameter JSON; D is k1,k2,p1,p2,k3.
 * Keep all coefficients in binary64 until final float32 map rounding. */
typedef struct {
    double xi;
    double K[3][3];
    double D[5];
} FptzLensCalibration;
extern const FptzLensCalibration fptz_camera_calibration[FPTZ_LENS_COUNT];
#define FPTZ_CALIBRATION_SHA256 "3526f0a857b64c7c2fc263c6695272694f6a061e277d41b73fe3492ef2f0f44b"
#endif
