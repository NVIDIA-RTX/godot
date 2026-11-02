// Mesh UV / UV2 footprints for custom shader texture() calls, which ShaderCompiler emits as textureGrad with
// these scaled by the call's coordinate math. Set by the closest-hit from the ray cone; zero samples mip 0.
vec2 rt_uv_ddx = vec2(0.0);
vec2 rt_uv_ddy = vec2(0.0);
vec2 rt_uv2_ddx = vec2(0.0);
vec2 rt_uv2_ddy = vec2(0.0);

// ShaderCompiler's texture_debug_macro; records the texture mip debug view in the closest hit only.
#ifdef RT_TEXTURE_MIP_DEBUG
#define RT_TEXTURE_DEBUG(p_sampler, p_ddx, p_ddy, p_rank) rt_debug_record_texture(textureSize(p_sampler, 0), textureQueryLevels(p_sampler), p_ddx, p_ddy, p_rank),
#else
#define RT_TEXTURE_DEBUG(p_sampler, p_ddx, p_ddy, p_rank)
#endif
