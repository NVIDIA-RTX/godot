// Froxel volume lookups shared by the scene, sky and path tracer fog passes.
// Bicubic only filters across the screen (XY); the noise modes also jitter across depth slices.

// Must match RSE::EnvironmentVolumetricFogSampling.
#define VOLUMETRIC_FOG_SAMPLING_TRILINEAR 0u
#define VOLUMETRIC_FOG_SAMPLING_BICUBIC 1u
#define VOLUMETRIC_FOG_SAMPLING_DITHER 2u
#define VOLUMETRIC_FOG_SAMPLING_FAST_NOISE 3u

// Cubic B-spline built from 4 trilinear taps; smooths the linear creases at froxel borders.
vec4 volumetric_fog_sample_bicubic(texture3D p_fog, sampler p_sampler, vec3 p_uvw) {
	vec2 size = vec2(textureSize(sampler3D(p_fog, p_sampler), 0).xy);
	vec2 texel = p_uvw.xy * size - 0.5;
	vec2 base = floor(texel);
	vec2 f = texel - base;
	vec2 f2 = f * f;
	vec2 f3 = f2 * f;

	vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
	vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
	vec2 w3 = f3 / 6.0;
	vec2 g0 = w0 + w1;
	vec2 g1 = w2 + w3;

	// Each pair of texels collapses into one bilinear tap at its weighted center.
	vec2 uv0 = (base - 0.5 + w1 / g0) / size;
	vec2 uv1 = (base + 1.5 + w3 / g1) / size;

	vec4 s00 = textureLod(sampler3D(p_fog, p_sampler), vec3(uv0.x, uv0.y, p_uvw.z), 0.0);
	vec4 s10 = textureLod(sampler3D(p_fog, p_sampler), vec3(uv1.x, uv0.y, p_uvw.z), 0.0);
	vec4 s01 = textureLod(sampler3D(p_fog, p_sampler), vec3(uv0.x, uv1.y, p_uvw.z), 0.0);
	vec4 s11 = textureLod(sampler3D(p_fog, p_sampler), vec3(uv1.x, uv1.y, p_uvw.z), 0.0);
	return g0.y * (g0.x * s00 + g1.x * s10) + g1.y * (g0.x * s01 + g1.x * s11);
}

// Each pixel of a 4x4 tile gets its own cell of a 4x4x4 grid, built from the Bayer matrix bits (pixel parity,
// 2x2 block parity). Every 2x2 neighborhood spans the XY range, and the high bits fill all 8 octants.
vec3 volumetric_fog_bayer_3d(uvec2 p_pixel) {
	uvec2 f = p_pixel & 1u;
	uvec2 c = (p_pixel >> 1u) & 1u;
	uvec3 cell = uvec3(((f.x ^ f.y) << 1u) | (c.x ^ c.y), (f.y << 1u) | c.x, (c.y << 1u) | f.x);
	return (vec3(cell) + 0.5) * 0.25;
}

// R3 low-discrepancy sequence (Roberts 2018) in 0.32 fixed point, exact for any frame number. It never repeats,
// so it cannot lock onto the TAA/upscaler jitter cycle, which decides what each output pixel accumulates.
vec3 volumetric_fog_r3(uint p_frame) {
	return vec3(p_frame * uvec3(3518319154u, 2882110344u, 2360945575u)) * (1.0 / 4294967296.0);
}

// FAST noise (thirdparty/fast_noise), tuned for a 3x3 spatial and an alpha 0.1 temporal filter. Layers play in
// order; the tile moves by R3 once per loop so the loop cannot line up with a jitter cycle of the same length.
// Depth reads the tile half a period away, which FAST leaves uncorrelated with the XY read.
vec3 volumetric_fog_fast_noise(texture2DArray p_noise, sampler p_sampler, uvec2 p_pixel, uint p_frame) {
	uvec3 size = uvec3(textureSize(sampler2DArray(p_noise, p_sampler), 0));
	uint loop = p_frame / size.z;
	uvec2 pixel = p_pixel + uvec2(volumetric_fog_r3(loop).xy * vec2(size.xy));
	int layer = int(p_frame % size.z);
	vec2 xy = texelFetch(sampler2DArray(p_noise, p_sampler), ivec3(pixel % size.xy, layer), 0).rg;
	float z = texelFetch(sampler2DArray(p_noise, p_sampler), ivec3((pixel + size.xy / 2u) % size.xy, layer), 0).r;
	return vec3(xy, z);
}

// p_noise in [0, 1) jitters up to half a froxel per axis, depth included; a temporal resolve must average it out.
// The second tap mirrors the first around p_uvw, canceling the linear part of the trilinear error.
vec4 volumetric_fog_sample_jittered(texture3D p_fog, sampler p_sampler, vec3 p_uvw, vec3 p_noise) {
	vec3 offset = (p_noise - 0.5) / vec3(textureSize(sampler3D(p_fog, p_sampler), 0));
	vec4 a = textureLod(sampler3D(p_fog, p_sampler), p_uvw + offset, 0.0);
	vec4 b = textureLod(sampler3D(p_fog, p_sampler), p_uvw - offset, 0.0);
	return (a + b) * 0.5;
}

// p_pixel is the render (pre-upscale) pixel: the upscaler's neighborhood clamp sees that grid, so it must hold the
// full noise range. p_frame must be monotonic; the per-frame rotation is what averages out at output resolution.
vec4 volumetric_fog_sample(texture3D p_fog, texture2DArray p_noise, sampler p_sampler, vec3 p_uvw, uint p_mode, uvec2 p_pixel, uint p_frame) {
	switch (p_mode) {
		case VOLUMETRIC_FOG_SAMPLING_BICUBIC:
			return volumetric_fog_sample_bicubic(p_fog, p_sampler, p_uvw);
		case VOLUMETRIC_FOG_SAMPLING_DITHER:
			return volumetric_fog_sample_jittered(p_fog, p_sampler, p_uvw, fract(volumetric_fog_bayer_3d(p_pixel) + volumetric_fog_r3(p_frame)));
		case VOLUMETRIC_FOG_SAMPLING_FAST_NOISE:
			return volumetric_fog_sample_jittered(p_fog, p_sampler, p_uvw, volumetric_fog_fast_noise(p_noise, p_sampler, p_pixel, p_frame));
		default:
			return textureLod(sampler3D(p_fog, p_sampler), p_uvw, 0.0);
	}
}
