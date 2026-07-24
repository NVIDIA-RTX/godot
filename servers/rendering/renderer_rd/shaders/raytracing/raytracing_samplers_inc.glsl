// 12 material samplers (filter/repeat combinations, matching samplers_inc.glsl).
layout(set = 0, binding = 16) uniform sampler SAMPLER_NEAREST_CLAMP;
layout(set = 0, binding = 17) uniform sampler SAMPLER_LINEAR_CLAMP;
layout(set = 0, binding = 18) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
layout(set = 0, binding = 19) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 0, binding = 20) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 0, binding = 21) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 0, binding = 22) uniform sampler SAMPLER_NEAREST_REPEAT;
layout(set = 0, binding = 23) uniform sampler SAMPLER_LINEAR_REPEAT;
layout(set = 0, binding = 24) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
layout(set = 0, binding = 25) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
layout(set = 0, binding = 26) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
layout(set = 0, binding = 27) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
