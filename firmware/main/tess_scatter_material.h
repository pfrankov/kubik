// Exact predecoded material stays in flash; each burst stores eight byte indices per dot.
// Preserve the original compiler arithmetic: native FMA, separate binary32 operations on C6.
#pragma once
#include <math.h>
typedef struct { float leaves, out_s, curve, letgo, strength, rings, mass, grip; } scatter_material_t;
#if defined(FP_FAST_FMAF)
#include "tess_scatter_material_fma.h"
#else
#include "tess_scatter_material_plain.h"
#endif
