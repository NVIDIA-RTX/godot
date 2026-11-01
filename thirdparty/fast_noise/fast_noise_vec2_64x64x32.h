// Filter-adapted Spatiotemporal Sampling (FAST) noise, Electronic Arts SEED.
// Copyright (c) 2023 Electronic Arts Inc. All rights reserved. BSD-3-Clause, see LICENSE.txt.
// Generated with: FastNoise.exe vector2 uniform binomial 2 exponential 0.1 0.1 product 64 64 32 fast -split -seed 1337 -numsteps 100000
// Layout: [layer][y][x][rg], uniform vec2 per texel, layers are consecutive frames.

#pragma once

#include <cstdint>

static constexpr uint32_t FAST_NOISE_VEC2_SIZE = 64;
static constexpr uint32_t FAST_NOISE_VEC2_LAYERS = 32;
static constexpr uint32_t FAST_NOISE_VEC2_BYTES = FAST_NOISE_VEC2_LAYERS * FAST_NOISE_VEC2_SIZE * FAST_NOISE_VEC2_SIZE * 2;

/// FAST_NOISE_VEC2_BYTES of RG8 texels in [layer][y][x][rg] order.
const uint8_t *fast_noise_vec2_get_data();
