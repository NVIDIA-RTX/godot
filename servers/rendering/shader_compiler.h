/**************************************************************************/
/*  shader_compiler.h                                                     */
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

#pragma once

#include "core/templates/pair.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/shader_language.h"

class ShaderCompiler {
public:
	enum Stage {
		STAGE_VERTEX,
		STAGE_FRAGMENT,
		STAGE_COMPUTE,
		STAGE_RAYGEN,
		STAGE_ANY_HIT,
		STAGE_CLOSEST_HIT,
		STAGE_MISS,
		STAGE_INTERSECTION,
		STAGE_MAX
	};

	struct IdentifierActions {
		struct TextureGradients {
			StringName coord;
			String ddx;
			String ddy;
		};

		HashMap<StringName, Stage> entry_point_stages;

		HashMap<StringName, Pair<int *, int>> render_mode_values;
		HashMap<StringName, bool *> render_mode_flags;
		HashMap<StringName, bool *> usage_flag_pointers;
		HashMap<StringName, String> function_renames;

		// texture() footprints. Entry 0 is the fallback; later ones apply to values derived from their coord.
		Vector<TextureGradients> texture_gradients;
		bool texture_explicit_gradients = false; // Emit texture() as textureGrad().
		bool texture_gradients_pre_vertex = false; // Also scale by coord writes in vertex().
		String texture_footprint_code; // Writes the finest footprint to texture_gradients[0].
		String texture_debug_macro; // MACRO(sampler, ddx, ddy, rank), rank 2 for source_color.

		HashMap<StringName, bool *> write_flag_pointers;
		HashMap<StringName, Pair<int *, int>> stencil_mode_values;
		int *stencil_reference = nullptr;

		HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> *uniforms = nullptr;
		String discard_replacement;
	};

	struct GeneratedCode {
		Vector<String> defines;
		struct Texture {
			StringName name;
			ShaderLanguage::DataType type = ShaderLanguage::DataType::TYPE_VOID;
			ShaderLanguage::ShaderNode::Uniform::Hint hint = ShaderLanguage::ShaderNode::Uniform::Hint::HINT_NONE;
			bool use_color = false;
			ShaderLanguage::TextureFilter filter = ShaderLanguage::TextureFilter::FILTER_DEFAULT;
			ShaderLanguage::TextureRepeat repeat = ShaderLanguage::TextureRepeat::REPEAT_DEFAULT;
			bool global = false;
			int array_size = 0;
		};

		Vector<Texture> texture_uniforms;

		Vector<uint32_t> uniform_offsets;
		uint32_t uniform_total_size = 0;
		String uniforms;
		String stage_globals[STAGE_MAX];

		HashMap<String, String> code;

		bool uses_global_textures = false;
		bool uses_fragment_time = false;
		bool uses_vertex_time = false;
		bool uses_screen_texture_mipmaps = false;
		bool uses_screen_texture = false;
		bool uses_depth_texture = false;
		bool uses_normal_roughness_texture = false;
	};

	struct DefaultIdentifierActions {
		HashMap<StringName, String> renames;
		HashMap<StringName, String> render_mode_defines;
		HashMap<StringName, String> stencil_mode_defines;
		HashMap<StringName, String> usage_defines;
		HashMap<StringName, String> custom_samplers;
		ShaderLanguage::TextureFilter default_filter = ShaderLanguage::TextureFilter::FILTER_NEAREST;
		ShaderLanguage::TextureRepeat default_repeat = ShaderLanguage::TextureRepeat::REPEAT_DISABLE;
		int base_texture_binding_index = 0;
		int texture_layout_set = 0;
		String base_uniform_string;
		String global_buffer_array_variable;
		bool global_buffer_array_is_texture = false;
		String instance_uniform_index_variable;
		uint32_t base_varying_index = 0;
		bool suppress_varying_io = false;
		bool apply_luminance_multiplier = false;
		bool check_multiview_samplers = false;
	};

private:
	ShaderLanguage parser;

	String _get_sampler_name(ShaderLanguage::TextureFilter p_filter, ShaderLanguage::TextureRepeat p_repeat);

	void _dump_function_deps(const ShaderLanguage::ShaderNode *p_node, const StringName &p_for_func, const HashMap<StringName, String> &p_func_code, String &r_to_add, HashSet<StringName> &added);
	String _dump_node_code(const ShaderLanguage::Node *p_node, int p_level, GeneratedCode &r_gen_code, IdentifierActions &p_actions, const DefaultIdentifierActions &p_default_actions, bool p_assigning, bool p_scope = true);

	const ShaderLanguage::ShaderNode *shader = nullptr;
	const ShaderLanguage::FunctionNode *function = nullptr;
	StringName current_func_name;
	StringName time_name;
	HashSet<StringName> texture_functions;

	HashSet<StringName> used_name_defines;
	HashSet<StringName> used_flag_pointers;
	HashSet<StringName> used_rmode_defines;
	HashSet<StringName> used_smode_defines;
	HashSet<StringName> internal_functions;
	HashSet<StringName> fragment_varyings;

	enum CoordScale {
		COORD_SCALE_NONE,
		COORD_SCALE_AFFINE,
		COORD_SCALE_UNKNOWN,
	};

	struct TextureFootprint {
		int source = 0;
		String scale;
		bool operator==(const TextureFootprint &p_other) const { return source == p_other.source && scale == p_other.scale; }
	};

	struct DumpContext {
		GeneratedCode *gen_code = nullptr;
		IdentifierActions *actions = nullptr;
		const DefaultIdentifierActions *default_actions = nullptr;
	};

	HashMap<StringName, LocalVector<const ShaderLanguage::Node *>> dataflow_defs;
	LocalVector<HashSet<StringName>> texture_coord_dependents;
	LocalVector<TextureFootprint> texture_footprints;

	bool _is_uniform_expression(const ShaderLanguage::Node *p_node) const;
	CoordScale _coord_scale(const ShaderLanguage::Node *p_node, int p_source, HashSet<StringName> &r_expanding, const DumpContext &p_context, String &r_scale);
	TextureFootprint _texture_footprint(const ShaderLanguage::Node *p_coord, const DumpContext &p_context);
	String _texture_footprint_code(const IdentifierActions &p_actions) const;

	DefaultIdentifierActions actions;

	static ShaderLanguage::DataType _get_global_shader_uniform_type(const StringName &p_name);

public:
	Error compile(RSE::ShaderMode p_mode, const String &p_code, IdentifierActions *p_actions, const String &p_path, GeneratedCode &r_gen_code);

	void initialize(DefaultIdentifierActions p_actions);
	ShaderCompiler();
};
