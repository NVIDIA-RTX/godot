/**************************************************************************/
/*  test_shader_compiler_texture_footprint.cpp                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_shader_compiler_texture_footprint)

#include "servers/rendering/shader_compiler.h"

namespace TestShaderCompilerTextureFootprint {

struct Compiled {
	String code; // vertex() + fragment() bodies.
	String footprint; // Footprint code section, empty when unscaled.
};

// Mirrors the ray tracing setup (explicit gradients from the mesh UV) unless p_raster.
static Compiled compile_spatial(const String &p_code, bool p_raster = false, const String &p_debug_macro = String()) {
	ShaderCompiler::DefaultIdentifierActions defaults;
	defaults.renames["UV"] = "uv_interp";
	defaults.renames["UV2"] = "uv2_interp";
	defaults.base_uniform_string = "material.";
	ShaderCompiler compiler;
	compiler.initialize(defaults);

	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.uniforms = &uniforms;
	actions.texture_gradients = { { "UV", "uv_ddx", "uv_ddy" }, { "UV2", "uv2_ddx", "uv2_ddy" } };
	actions.texture_explicit_gradients = !p_raster;
	actions.texture_gradients_pre_vertex = !p_raster;
	actions.texture_footprint_code = "texture_footprint";
	actions.texture_debug_macro = p_debug_macro;

	ShaderCompiler::GeneratedCode gen;
	Compiled compiled;
	if (compiler.compile(RSE::SHADER_SPATIAL, "shader_type spatial;\n" + p_code, &actions, String(), gen) != OK) {
		return compiled;
	}
	compiled.code = (gen.code.has("vertex") ? gen.code["vertex"] : String()) + (gen.code.has("fragment") ? gen.code["fragment"] : String());
	compiled.footprint = gen.code.has("texture_footprint") ? gen.code["texture_footprint"] : String();
	return compiled;
}

// ShaderCompiler reads ShaderTypes, which only a live RenderingServer keeps valid.
TEST_CASE("[SceneTree][ShaderCompiler] Plain UV lookups use the source footprint and need no footprint code") {
	const Compiled c = compile_spatial("uniform sampler2D t;\nvoid fragment() { ALBEDO = texture(t, UV).rgb; }\n");
	CHECK(c.code.contains("textureGrad("));
	CHECK(c.code.contains(", uv_ddx, uv_ddy)"));
	CHECK(c.footprint.is_empty());
}

TEST_CASE("[SceneTree][ShaderCompiler] Uniform and constant tiling scale the footprint") {
	const Compiled c = compile_spatial("uniform sampler2D t;\nuniform vec2 tiling;\n"
									   "void fragment() { ALBEDO = texture(t, fract(UV * tiling + vec2(TIME)) * 2.0).rgb; }\n");
	CHECK(c.code.contains("((uv_ddx * (material.m_tiling)) * (2.0))"));
	CHECK(c.footprint.contains("uv_ddx = footprint_ddx;"));
}

TEST_CASE("[SceneTree][ShaderCompiler] Matrix factors keep their multiplication order") {
	const Compiled c = compile_spatial("uniform sampler2D t;\nuniform mat2 rot;\nvoid fragment() { ALBEDO = texture(t, rot * UV).rgb; }\n");
	CHECK(c.code.contains("((material.m_rot) * uv_ddx)"));
}

TEST_CASE("[SceneTree][ShaderCompiler] UV2 is followed through locals and selects the UV2 footprint") {
	const Compiled c = compile_spatial("uniform sampler2D t;\nuniform sampler2D u;\n"
									   "void fragment() { vec2 uv = UV2 * 4.0; ALBEDO = texture(t, uv).rgb + texture(u, UV).rgb; }\n");
	CHECK(c.code.contains("(uv2_ddx * (4.0))"));
	CHECK(c.code.contains(", uv_ddx, uv_ddy)"));
	// Finest of both lookups.
	CHECK(c.footprint.contains("vec2 candidate_ddx = "));
}

TEST_CASE("[SceneTree][ShaderCompiler] vertex() writes to UV scale the footprint only before vertex()") {
	const String code = "uniform sampler2D t;\nuniform vec2 scale;\nuniform vec2 offset;\n"
						"void vertex() { UV = UV * scale + offset; }\nvoid fragment() { ALBEDO = texture(t, UV).rgb; }\n";
	CHECK(compile_spatial(code).code.contains("(uv_ddx * (material.m_scale))"));
	CHECK(compile_spatial(code, true).footprint.is_empty());
}

TEST_CASE("[SceneTree][ShaderCompiler] Non-affine or per-pixel coordinate math falls back to the source footprint") {
	const Compiled c = compile_spatial("uniform sampler2D t;\n"
									   "void fragment() { vec2 a = UV * UV.x; vec2 b = UV; b = UV * 2.0; ALBEDO = texture(t, a).rgb + texture(t, b).rgb + texture(t, sin(UV)).rgb; }\n");
	CHECK_FALSE(c.code.contains("uv_ddx *"));
	CHECK(c.footprint.is_empty());
}

TEST_CASE("[SceneTree][ShaderCompiler] Bias scales the footprint, falls back to LOD bias without one, and 3D lookups use LOD 0") {
	const Compiled c = compile_spatial("uniform sampler2D t;\nuniform sampler3D v;\n"
									   "void fragment() { ALBEDO = texture(t, UV, 1.0).rgb + texture(v, vec3(UV, 0.0)).rgb; }\n");
	CHECK(c.code.contains("(uv_ddx != vec2(0.0) ? textureGrad("));
	CHECK(c.code.contains(", uv_ddx * exp2(1.0), uv_ddy * exp2(1.0)) : textureLod("));
	CHECK(c.code.contains("uv_interp, 1.0))"));
	CHECK(c.code.contains("textureLod("));
	CHECK(c.code.contains(", 0.0)"));
}

TEST_CASE("[SceneTree][ShaderCompiler] The debug macro records each explicit lookup's sampler, footprint and albedo rank") {
	const Compiled c = compile_spatial("uniform sampler2D t : source_color;\nuniform sampler2D u;\n"
									   "void fragment() { ALBEDO = texture(t, UV * 2.0).rgb + texture(u, UV).rgb; }\n",
			false, "DBG");
	CHECK(c.code.contains("(DBG(sampler2D(m_t, SAMPLER_NEAREST_CLAMP), (uv_ddx * (2.0)), (uv_ddy * (2.0)), 2) textureGrad(sampler2D(m_t, "));
	CHECK(c.code.contains("(DBG(sampler2D(m_u, SAMPLER_NEAREST_CLAMP), uv_ddx, uv_ddy, 1) textureGrad(sampler2D(m_u, "));
	CHECK_FALSE(compile_spatial("uniform sampler2D t;\nvoid fragment() { ALBEDO = texture(t, UV).rgb; }\n").code.contains("DBG("));
}

TEST_CASE("[SceneTree][ShaderCompiler] Raster keeps implicit texture() but still reports the scaled footprint") {
	const Compiled c = compile_spatial("uniform sampler2D t;\nuniform float tiling;\nvoid fragment() { ALBEDO = texture(t, UV * tiling).rgb; }\n", true);
	CHECK(c.code.contains("texture("));
	CHECK_FALSE(c.code.contains("textureGrad("));
	CHECK(c.footprint.contains("vec2 footprint_ddx = (uv_ddx * (material.m_tiling));"));
}

} // namespace TestShaderCompilerTextureFootprint
