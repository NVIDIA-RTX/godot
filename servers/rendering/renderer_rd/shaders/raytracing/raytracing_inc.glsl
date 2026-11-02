// Shared definitions for raytracing shaders (raygen, miss, closest_hit, any_hit).
// Must be included before scene_data_inc.glsl since it defines MAX_VIEWS.
#include "../oct_inc.glsl"

#include "../oct_inc.glsl"

#define MAX_VIEWS 2

// Per-geometry entry (geometries, materials, motion) of a hit. A TLAS instance's custom index is its
// first entry; multi-geometry BLASes store one entry per geometry, in geometry order.
#define RT_HIT_GEOMETRY_IDX (gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT)
#define RT_RQ_GEOMETRY_IDX(m_rq, m_committed) uint(rayQueryGetIntersectionInstanceCustomIndexEXT(m_rq, m_committed) + rayQueryGetIntersectionGeometryIndexEXT(m_rq, m_committed))

#ifndef PI
#define PI 3.141592653589f
#endif

// ============================================================================
// RT_PARAMS INDICES - Must match RT_PARAM_* in scene_shader_raytracing.h
// ============================================================================
// rt_params is a vec4 uniform array of RT_PARAM_COUNT floats, injected from SceneShaderRaytracing
// Access: rt_params[idx >> 2][idx & 3] or get_rt_param(idx)
#define RT_PARAM_VIS_MODE 0 // rt_params[0].x - Debug visualization mode (0 = disabled)
#define RT_PARAM_SAMPLE_COUNT 1 // rt_params[0].y - Samples per pixel
#define RT_PARAM_MAX_BOUNCES 2 // rt_params[0].z - Maximum ray bounces
#define RT_PARAM_DENOISER 3 // rt_params[0].w - Denoiser selection (0=none, 1=DLSS RR)
#define RT_PARAM_MAX_TRANSPARENCY_LAYERS 4 // rt_params[1].x - primary-segment peel budget for all blend modes (0 = transparency off)
#define RT_PARAM_TRANSPARENCY_MAX_BOUNCE 5 // rt_params[1].y - last bounce with transparency handling
#define RT_PARAM_TRANSPARENT_COUNT 6 // rt_params[1].z - instances in the unified TLAS transparent camera lane
#define RT_PARAM_TRANSPARENCY_INDIRECT_RANGE 7 // rt_params[1].w - max peel distance on non-singular bounces (0 = unlimited)
#define RT_PARAM_TRANSPARENCY_INDIRECT_FADE 8 // rt_params[2].x - fade-out length before that range (0 = hard cutoff)
#define RT_PARAM_SKY_SCREEN_ENABLED 9 // rt_params[2].y - primary-ray misses read the screen-space sky instead of the octmap
#define RT_PARAM_VOLUMETRIC_FOG_INV_LENGTH 10 // rt_params[2].z - 1 / fog length, 0 = volumetric fog disabled
#define RT_PARAM_VOLUMETRIC_FOG_DETAIL_SPREAD 11 // rt_params[2].w - 1 / detail spread
#define RT_PARAM_VOLUMETRIC_FOG_SKY_AFFECT 12 // rt_params[3].x
#define RT_PARAM_FOG_USE_LEGACY_BLENDING 13 // rt_params[3].y
#define RT_PARAM_LIGHT_COUNT 14 // rt_params[3].z - Number of active lights in light buffer
#define RT_PARAM_FRAME_INDEX 15 // rt_params[3].w - Frame counter for temporal variation
#define RT_PARAM_DECAL_COUNT 16 // rt_params[4].x - decals in the decal volume TLAS
#define RT_PARAM_DECAL_MODE 17 // rt_params[4].y - RT_DECAL_MODE_*: which path vertices receive decals
#define RT_PARAM_TRANSPARENCY_DOMINANT_LAYER_ALPHA 18 // rt_params[4].z - min alpha of the first peel layer to own depth/velocity (0 = never)
#define RT_PARAM_MAX_TRANSPARENCY_LAYERS_REFLECTION 19 // rt_params[4].w - peel budget on singular (mirror) bounce segments
#define RT_PARAM_MAX_TRANSPARENCY_LAYERS_INDIRECT 20 // rt_params[5].x - peel budget on other indirect segments
#define RT_PARAM_SELF_SHADOW_MASK 21 // rt_params[5].y - TLAS shadow group bit of SELF_SHADOWING casters (0 = none)
#define RT_PARAM_VOLUMETRIC_FOG_SAMPLING 22 // rt_params[5].z - VOLUMETRIC_FOG_SAMPLING_* froxel lookup filter
// RT_PARAM_VIS_MODE values. Must match Environment::PathtracingDebugMode.
#define RT_VIS_TRANSPARENCY_LAYERS 23
#define RT_VIS_DYNAMIC_GEOMETRY 29

// True when p_vis_mode traces transparency peels: normal shading, and the debug views that show transparent layers.
bool rt_debug_keeps_transparency(int p_vis_mode) {
	return p_vis_mode == 0 || p_vis_mode == RT_VIS_TRANSPARENCY_LAYERS || p_vis_mode == RT_VIS_DYNAMIC_GEOMETRY;
}
// ============================================================================
// PATHTRACING PAYLOAD (32 bytes, fp16/unorm-packed)
// ============================================================================
// Radiance (r) and throughput (T) interleaved as fp16 into 3 uints:
//   [0]=rg, [1]=bR, [2]=GB  (packHalf2x16 pairs).
// The next-bounce origin is not stored: raygen reconstructs it from the
// current ray and hit_t as `offset_ray_origin(ray_origin + ray_dir * hit_t,
// offset_normal)`. The offset normal and next direction are stored as
// 16-bit unorm octahedral pairs (effective angular error <~0.001 deg,
// below the 8-bit normal quantization baked into offset_ray_origin).
// Use PathState + path_pack/path_unpack for fp32 working copies.

struct PathPayload {
	uint packed_rt[3]; // 12 bytes - radiance+throughput interleaved as fp16
	uint packed_bounces_flags; //  4 bytes - [flags:8][peel_alpha:8][non_singular:1][reserved:7][peel_scene_depth:1][diffuse:3][total:4]
	uint rng_state; //  4 bytes - RNG state for PCG
	float hit_t; //  4 bytes - surface distance, even on termination; -1 on a scene miss
	uint oct_offset_nrm; //  4 bytes - packUnorm2x16(vec3_to_oct(offset normal))
	uint oct_next_dir; //  4 bytes - packUnorm2x16(vec3_to_oct(next direction)); peel-ray input: SCENE_DEPTH, see path_pack_peel()
};

// Set by path_pack_peel(), cleared by path_pack(): oct_next_dir holds the peel ray's SCENE_DEPTH, not a direction.
const uint PEEL_SCENE_DEPTH_FLAG = (1u << 7);
const uint PEEL_RAY_FLAG = (1u << 28);
const uint PEEL_FIRST_LAYER_FLAG = (1u << 29);
const uint PEEL_HAS_CONTINUATION_FLAG = (1u << 30);
const uint PEEL_DEPTH_BARRIER_HIT_FLAG = (1u << 31);

// Unpacked fp32 working copy of the payload.
struct PathState {
	vec3 radiance;
	vec3 throughput;
	uint packed_bounces_flags;
	uint rng_state;
	float hit_t;
	vec3 offset_normal;
	vec3 next_ray_dir;
};

PathState path_unpack(PathPayload p) {
	PathState s;
	vec2 rg = unpackHalf2x16(p.packed_rt[0]);
	vec2 bR = unpackHalf2x16(p.packed_rt[1]);
	vec2 GB = unpackHalf2x16(p.packed_rt[2]);
	s.radiance = max(vec3(rg.x, rg.y, bR.x), vec3(0.0));
	s.throughput = max(vec3(bR.y, GB.x, GB.y), vec3(0.0));
	s.packed_bounces_flags = p.packed_bounces_flags;
	s.rng_state = p.rng_state;
	s.hit_t = p.hit_t;
	s.offset_normal = oct_to_vec3(unpackUnorm2x16(p.oct_offset_nrm) * 2.0 - 1.0);
	s.next_ray_dir = oct_to_vec3(unpackUnorm2x16(p.oct_next_dir) * 2.0 - 1.0);
	return s;
}

void path_pack(inout PathPayload p, PathState s) {
	const float FP16_MAX = 65504.0;
	vec3 r = clamp(s.radiance, vec3(0.0), vec3(FP16_MAX));
	vec3 t = clamp(s.throughput, vec3(0.0), vec3(FP16_MAX));
	p.packed_rt[0] = packHalf2x16(vec2(r.r, r.g));
	p.packed_rt[1] = packHalf2x16(vec2(r.b, t.r));
	p.packed_rt[2] = packHalf2x16(vec2(t.g, t.b));
	p.packed_bounces_flags = s.packed_bounces_flags & ~PEEL_SCENE_DEPTH_FLAG;
	p.rng_state = s.rng_state;
	p.hit_t = s.hit_t;
	p.oct_offset_nrm = packUnorm2x16(vec3_to_oct(s.offset_normal));
	p.oct_next_dir = packUnorm2x16(vec3_to_oct(s.next_ray_dir));
}

// Peel ray setup. p_scene_depth: NDC depth of the segment's opaque hit, 0.0 = far.
void path_pack_peel(inout PathPayload p, PathState s, float p_scene_depth) {
	path_pack(p, s);
	p.packed_bounces_flags |= PEEL_SCENE_DEPTH_FLAG;
	p.oct_next_dir = floatBitsToUint(p_scene_depth);
}

#ifdef RT_DEBUG_ENABLED
bool rt_peel_scene_depth_lost = false; // A peel hit read SCENE_DEPTH after its payload was repacked.
#endif

// SCENE_DEPTH of a peel ray, 0.0 (far, no proximity fade) otherwise. Hits must read it before their first path_pack().
float payload_peel_scene_depth(PathPayload p) {
	if ((p.packed_bounces_flags & PEEL_SCENE_DEPTH_FLAG) != 0u) {
		return uintBitsToFloat(p.oct_next_dir);
	}
#ifdef RT_DEBUG_ENABLED
	rt_peel_scene_depth_lost = rt_peel_scene_depth_lost || (p.packed_bounces_flags & PEEL_RAY_FLAG) != 0u;
#endif
	return 0.0;
}

// Bounce count helpers. Bits 0-3: total, must hold RT_MAX_BOUNCES_LIMIT; bits 4-6: diffuse, must hold MAX_DIFFUSE_BOUNCES.
// Bit 7: PEEL_SCENE_DEPTH_FLAG. Bit 8: LAST_BOUNCE_DIFFUSE_FLAG. Bits 9-13: DLSS-RR guide rays and replacement. Bit 14 is reserved.
uint get_total_bounces(uint packed) {
	return packed & 0xFu;
}
uint get_diffuse_bounces(uint packed) {
	return (packed >> 4u) & 0x7u;
}

// Set once the path has scattered through anything but a (near-)singular lobe.
const uint NON_SINGULAR_PATH_FLAG = (1u << 15);
#define SINGULAR_ROUGHNESS_MAX 0.05
bool is_singular_path(uint packed) {
	return (packed & NON_SINGULAR_PATH_FLAG) == 0u;
}
uint pack_bounces(uint total, uint diffuse) {
	return total | (diffuse << 4u);
}
uint inc_total_bounce(uint packed) {
	return packed + 1u;
}
uint inc_diffuse_bounce(uint packed) {
	return packed + 0x11u;
} // +1 to both total and diffuse

// Set when the most recent scatter sampled the diffuse lobe, cleared when it sampled a specular one.
const uint LAST_BOUNCE_DIFFUSE_FLAG = (1u << 8);

// DLSS-RR primary surface replacement. A guide ray walks mirrors deterministically for the RR guides
// and carries no radiance; on return, raw payload fields hold the result (see dlss_rr_guide_hit()).
const uint GUIDE_RAY_FLAG = (1u << 9);
const uint GUIDE_LAST_FLAG = (1u << 10); //!< Raygen input: the hit must report itself, not continue.
const uint GUIDE_RESULT_FLAG = (1u << 11); //!< Hit output: the payload holds a guide result.
const uint GUIDE_MIRROR_FLAG = (1u << 12); //!< Hit output: replaceable mirror, continue along next_ray_dir.
// Set by a replaceable primary mirror; the path's next singular hit writes the replacement and clears it.
// Still set after that bounce, raygen falls back to a guide ray walk.
const uint DLSS_RR_REPLACE_PENDING_FLAG = (1u << 13);

// Sample 0 flag (bit 24) - only write DLSS RR outputs on first sample
const uint SAMPLE_ZERO_FLAG = (1u << 24);
uint set_sample_zero(uint packed) {
	return packed | SAMPLE_ZERO_FLAG;
}
bool is_sample_zero(uint packed) {
	return (packed & SAMPLE_ZERO_FLAG) != 0u;
}

// Shadow ray flag (bit 25) - indicates this ray is a shadow/occlusion test
const uint SHADOW_RAY_FLAG = (1u << 25);
uint set_shadow_ray(uint packed) {
	return packed | SHADOW_RAY_FLAG;
}
bool is_shadow_ray(uint packed) {
	return (packed & SHADOW_RAY_FLAG) != 0u;
}

const uint PRIMARY_MISS_FLAG = (1u << 27);
uint set_primary_miss(uint packed) {
	return packed | PRIMARY_MISS_FLAG;
}
bool is_primary_miss(uint packed) {
	return (packed & PRIMARY_MISS_FLAG) != 0u;
}

// Peel traces store per-layer alpha in the spare byte (bits 16-23).
float get_peel_alpha(uint packed) {
	return float((packed >> 16u) & 0xFFu) / 255.0;
}
uint set_peel_alpha(uint packed, float a) {
	return (packed & ~0x00FF0000u) | (uint(clamp(a, 0.0, 1.0) * 255.0 + 0.5) << 16u);
}

const uint PATH_TERMINATED_FLAG = (1u << 26);
uint set_path_terminated(uint packed) {
	return packed | PATH_TERMINATED_FLAG;
}
uint clear_path_terminated(uint packed) {
	return packed & ~PATH_TERMINATED_FLAG;
}
bool is_path_terminated(uint packed) {
	return (packed & PATH_TERMINATED_FLAG) != 0u;
}

// RAY_* shader built-ins. Shadow rays start from zeroed bounce state, so every query but RAY_IS_SHADOW excludes them.
// Peel rays report the segment they peel: transparent layers seen by the camera are primary.
bool rt_ray_is_primary(uint packed) {
	return !is_shadow_ray(packed) && get_total_bounces(packed) == 0u;
}
bool rt_ray_is_diffuse(uint packed) {
	return (packed & LAST_BOUNCE_DIFFUSE_FLAG) != 0u;
}
bool rt_ray_is_specular(uint packed) {
	return get_total_bounces(packed) > 0u && !rt_ray_is_diffuse(packed);
}
bool rt_ray_is_singular(uint packed) {
	return !is_shadow_ray(packed) && is_singular_path(packed);
}

// Bounce limits
#define MAX_DIFFUSE_BOUNCES 2u
#define MAX_DENOISER_SPECULAR_HIT_THRESHOLD 0.25

// DLSS-RR primary surface replacement only follows mirrors up to this roughness; rougher ones keep
// their own guides plus a spec MV.
#define DLSS_RR_REPLACEMENT_ROUGHNESS_MAX 0.01

// ============================================================================
// CONSTANTS
// ============================================================================
const uint OFFSET_NONE = 0xFFFFFFFFu;
const uint FLAG_COMPRESSED = 1u;
const uint FLAG_PROCEDURAL = 2u;
// Set when the BLAS uses a per-frame-deformed vertex buffer.
const uint FLAG_DEFORMED = 4u;
// Positions and TBN come from the vertex capture pass.
const uint FLAG_CAPTURED = 8u;
// UVs in the attribute buffer are unorm16, independent of FLAG_COMPRESSED.
const uint FLAG_COMPRESSED_UV = 16u;
// A triangle hit reported as front facing is a raster back face.
const uint FLAG_FACING_INVERTED = 32u;
// Back faces flip the normal, like raster DO_SIDE_CHECK.
const uint FLAG_SIDE_CHECK = 64u;
// SELF_SHADOWING receiver: its shadow rays keep RT_PARAM_SELF_SHADOW_MASK.
const uint FLAG_SELF_SHADOWING = 128u;

// ============================================================================
// RANDOM NUMBER GENERATION - PCG (Permuted Congruential Generator)
// ============================================================================

// PCG random number generator - improved XSH-RR variant with better avalanche
uint pcg_hash(uint seed) {
	uint state = seed * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

// Initialize RNG state with improved mixing to eliminate patterns
uint init_rng(uvec2 pixel, uint frame, uint sample_idx) {
	// Use Wang hash for better mixing - eliminates linear patterns
	uint seed = pixel.x + pixel.y * 65536u + frame * 1000000u + sample_idx * 100000000u;
	return pcg_hash(seed);
}

// Random float in [0, 1)
float rand(inout uint state) {
	state = pcg_hash(state);
	// Keep 24 bits, float(state) would round the top values up to exactly 1.0.
	return float(state >> 8u) * (1.0 / 16777216.0);
}

// Random vec2 in [0, 1)
vec2 rand2(inout uint state) {
	return vec2(rand(state), rand(state));
}

// ============================================================================
// RAY ORIGIN OFFSET (prevents self-intersection)
// Wachter-Binder method: offsets in integer float representation so the
// displacement scales naturally with position magnitude.
// ============================================================================
float _offset_component(float p, float n_comp, int of_comp) {
	const float origin = 1.0 / 32.0;
	const float float_scale = 1.0 / 65536.0;
	int shifted = floatBitsToInt(p) + ((p >= 0.0) ? of_comp : -of_comp);
	float p_i = intBitsToFloat(shifted);
	return (abs(p) < origin) ? (p + float_scale * n_comp) : p_i;
}

vec3 offset_ray_origin(vec3 p, vec3 n) {
	const float int_scale = 256.0;
	ivec3 of = ivec3(int_scale * n);
	return vec3(
			_offset_component(p.x, n.x, of.x),
			_offset_component(p.y, n.y, of.y),
			_offset_component(p.z, n.z, of.z));
}
