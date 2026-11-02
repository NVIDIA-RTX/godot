/**************************************************************************/
/*  shader_compiler.cpp                                                   */
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

#include "shader_compiler.h"

#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/shader_types.h"

#define SL ShaderLanguage

static String _mktab(int p_level) {
	return String("\t").repeat(p_level);
}

static String _typestr(SL::DataType p_type) {
	String type = ShaderLanguage::get_datatype_name(p_type);
	if (!RS::get_singleton()->is_low_end() && ShaderLanguage::is_sampler_type(p_type)) {
		type = type.replace("sampler", "texture"); //we use textures instead of samplers in Vulkan GLSL
	}
	return type;
}

static int _get_datatype_alignment(SL::DataType p_type) {
	switch (p_type) {
		case SL::TYPE_VOID:
			return 0;
		case SL::TYPE_BOOL:
			return 4;
		case SL::TYPE_BVEC2:
			return 8;
		case SL::TYPE_BVEC3:
			return 16;
		case SL::TYPE_BVEC4:
			return 16;
		case SL::TYPE_INT:
			return 4;
		case SL::TYPE_IVEC2:
			return 8;
		case SL::TYPE_IVEC3:
			return 16;
		case SL::TYPE_IVEC4:
			return 16;
		case SL::TYPE_UINT:
			return 4;
		case SL::TYPE_UVEC2:
			return 8;
		case SL::TYPE_UVEC3:
			return 16;
		case SL::TYPE_UVEC4:
			return 16;
		case SL::TYPE_FLOAT:
			return 4;
		case SL::TYPE_VEC2:
			return 8;
		case SL::TYPE_VEC3:
			return 16;
		case SL::TYPE_VEC4:
			return 16;
		case SL::TYPE_MAT2:
			return 16;
		case SL::TYPE_MAT3:
			return 16;
		case SL::TYPE_MAT4:
			return 16;
		case SL::TYPE_SAMPLER2D:
			return 16;
		case SL::TYPE_ISAMPLER2D:
			return 16;
		case SL::TYPE_USAMPLER2D:
			return 16;
		case SL::TYPE_SAMPLER2DARRAY:
			return 16;
		case SL::TYPE_ISAMPLER2DARRAY:
			return 16;
		case SL::TYPE_USAMPLER2DARRAY:
			return 16;
		case SL::TYPE_SAMPLER3D:
			return 16;
		case SL::TYPE_ISAMPLER3D:
			return 16;
		case SL::TYPE_USAMPLER3D:
			return 16;
		case SL::TYPE_SAMPLERCUBE:
			return 16;
		case SL::TYPE_SAMPLERCUBEARRAY:
			return 16;
		case SL::TYPE_SAMPLEREXT:
			return 16;
		case SL::TYPE_STRUCT:
			return 0;
		case SL::TYPE_MAX: {
			ERR_FAIL_V(0);
		}
	}

	ERR_FAIL_V(0);
}

static String _interpstr(SL::DataInterpolation p_interp) {
	switch (p_interp) {
		case SL::INTERPOLATION_FLAT:
			return "flat ";
		case SL::INTERPOLATION_SMOOTH:
			return "";
		case SL::INTERPOLATION_DEFAULT:
			return "";
	}
	return "";
}

static String _prestr(SL::DataPrecision p_pres, bool p_force_highp = false) {
	switch (p_pres) {
		case SL::PRECISION_LOWP:
			return "lowp ";
		case SL::PRECISION_MEDIUMP:
			return "mediump ";
		case SL::PRECISION_HIGHP:
			return "highp ";
		case SL::PRECISION_DEFAULT:
			return p_force_highp ? "highp " : "";
	}
	return "";
}

static String _constr(bool p_is_const) {
	if (p_is_const) {
		return "const ";
	}
	return "";
}

static String _qualstr(SL::ArgumentQualifier p_qual) {
	switch (p_qual) {
		case SL::ARGUMENT_QUALIFIER_IN:
			return "";
		case SL::ARGUMENT_QUALIFIER_OUT:
			return "out ";
		case SL::ARGUMENT_QUALIFIER_INOUT:
			return "inout ";
	}
	return "";
}

static String _opstr(SL::Operator p_op) {
	return SL::get_operator_text(p_op);
}

static String _mkid(const String &p_id) {
	String id = "m_" + p_id.replace("__", "_dus_");
	return id.replace("__", "_dus_"); //doubleunderscore is reserved in glsl
}

// True if p_node reads any of p_names (variables, arrays, or user function results).
static bool _node_reads(const SL::Node *p_node, const HashSet<StringName> &p_names) {
	if (p_node == nullptr) {
		return false;
	}
	switch (p_node->type) {
		case SL::Node::NODE_TYPE_VARIABLE:
			return p_names.has(static_cast<const SL::VariableNode *>(p_node)->name);
		case SL::Node::NODE_TYPE_OPERATOR: {
			for (const SL::Node *arg : static_cast<const SL::OperatorNode *>(p_node)->arguments) {
				if (_node_reads(arg, p_names)) {
					return true;
				}
			}
			return false;
		}
		case SL::Node::NODE_TYPE_MEMBER: {
			const SL::MemberNode *mnode = static_cast<const SL::MemberNode *>(p_node);
			return _node_reads(mnode->owner, p_names) || _node_reads(mnode->index_expression, p_names) || _node_reads(mnode->call_expression, p_names);
		}
		case SL::Node::NODE_TYPE_ARRAY: {
			const SL::ArrayNode *anode = static_cast<const SL::ArrayNode *>(p_node);
			return p_names.has(anode->name) || _node_reads(anode->index_expression, p_names) || _node_reads(anode->call_expression, p_names);
		}
		case SL::Node::NODE_TYPE_ARRAY_CONSTRUCT: {
			for (const SL::Node *init : static_cast<const SL::ArrayConstructNode *>(p_node)->initializer) {
				if (_node_reads(init, p_names)) {
					return true;
				}
			}
			return false;
		}
		default:
			return false;
	}
}

// Variable written by an assignment target (`v`, `v.x`, `v[i]`).
static StringName _assigned_root(const SL::Node *p_node) {
	switch (p_node->type) {
		case SL::Node::NODE_TYPE_VARIABLE:
			return static_cast<const SL::VariableNode *>(p_node)->name;
		case SL::Node::NODE_TYPE_ARRAY:
			return static_cast<const SL::ArrayNode *>(p_node)->name;
		case SL::Node::NODE_TYPE_MEMBER:
			return _assigned_root(static_cast<const SL::MemberNode *>(p_node)->owner);
		case SL::Node::NODE_TYPE_OPERATOR: {
			const SL::OperatorNode *onode = static_cast<const SL::OperatorNode *>(p_node);
			return onode->op == SL::OP_INDEX ? _assigned_root(onode->arguments[0]) : StringName();
		}
		default:
			return StringName();
	}
}

struct ShaderDataflowEdge {
	StringName target;
	const SL::Node *source = nullptr;
};

// Records "target is computed from source" for declarations, assignments, user call arguments and returns.
static void _collect_dataflow(const SL::ShaderNode *p_shader, const SL::Node *p_node, const StringName &p_function, LocalVector<ShaderDataflowEdge> &r_edges) {
	if (p_node == nullptr) {
		return;
	}
	switch (p_node->type) {
		case SL::Node::NODE_TYPE_BLOCK: {
			for (const SL::Node *statement : static_cast<const SL::BlockNode *>(p_node)->statements) {
				_collect_dataflow(p_shader, statement, p_function, r_edges);
			}
		} break;
		case SL::Node::NODE_TYPE_VARIABLE_DECLARATION: {
			for (const SL::VariableDeclarationNode::Declaration &decl : static_cast<const SL::VariableDeclarationNode *>(p_node)->declarations) {
				for (const SL::Node *init : decl.initializer) {
					r_edges.push_back({ decl.name, init });
					_collect_dataflow(p_shader, init, p_function, r_edges);
				}
			}
		} break;
		case SL::Node::NODE_TYPE_CONTROL_FLOW: {
			const SL::ControlFlowNode *cfnode = static_cast<const SL::ControlFlowNode *>(p_node);
			for (const SL::Node *expression : cfnode->expressions) {
				if (cfnode->flow_op == SL::FLOW_OP_RETURN) {
					r_edges.push_back({ p_function, expression });
				}
				_collect_dataflow(p_shader, expression, p_function, r_edges);
			}
			for (const SL::BlockNode *block : cfnode->blocks) {
				_collect_dataflow(p_shader, block, p_function, r_edges);
			}
		} break;
		case SL::Node::NODE_TYPE_OPERATOR: {
			const SL::OperatorNode *onode = static_cast<const SL::OperatorNode *>(p_node);
			if (onode->op >= SL::OP_ASSIGN && onode->op <= SL::OP_ASSIGN_BIT_XOR) {
				r_edges.push_back({ _assigned_root(onode->arguments[0]), onode->arguments[1] });
			} else if (onode->op == SL::OP_CALL && onode->arguments[0]->type == SL::Node::NODE_TYPE_VARIABLE) {
				const StringName &callee = static_cast<const SL::VariableNode *>(onode->arguments[0])->name;
				for (const SL::ShaderNode::Function &func : p_shader->vfunctions) {
					if (func.name != callee) {
						continue;
					}
					for (int i = 1; i < onode->arguments.size() && i - 1 < func.function->arguments.size(); i++) {
						r_edges.push_back({ func.function->arguments[i - 1].name, onode->arguments[i] });
					}
					break;
				}
			}
			for (const SL::Node *arg : onode->arguments) {
				_collect_dataflow(p_shader, arg, p_function, r_edges);
			}
		} break;
		case SL::Node::NODE_TYPE_ARRAY: {
			const SL::ArrayNode *anode = static_cast<const SL::ArrayNode *>(p_node);
			if (anode->assign_expression) {
				r_edges.push_back({ anode->name, anode->assign_expression });
			}
			_collect_dataflow(p_shader, anode->index_expression, p_function, r_edges);
			_collect_dataflow(p_shader, anode->assign_expression, p_function, r_edges);
		} break;
		default:
			break;
	}
}

// Every name computed from p_source anywhere in the shader. Flow-insensitive and keyed by name, so
// shadowed locals and overwritten values are over-approximated.
static HashSet<StringName> _dataflow_dependents(const LocalVector<ShaderDataflowEdge> &p_edges, const StringName &p_source) {
	HashSet<StringName> dependents;
	dependents.insert(p_source);
	bool changed = true;
	while (changed) {
		changed = false;
		for (const ShaderDataflowEdge &edge : p_edges) {
			if (edge.target != StringName() && !dependents.has(edge.target) && _node_reads(edge.source, dependents)) {
				dependents.insert(edge.target);
				changed = true;
			}
		}
	}
	return dependents;
}

static bool _texture_grad_takes_uv(SL::DataType p_sampler) {
	switch (p_sampler) {
		case SL::TYPE_SAMPLER2D:
		case SL::TYPE_ISAMPLER2D:
		case SL::TYPE_USAMPLER2D:
		case SL::TYPE_SAMPLER2DARRAY:
		case SL::TYPE_ISAMPLER2DARRAY:
		case SL::TYPE_USAMPLER2DARRAY:
		case SL::TYPE_SAMPLEREXT:
			return true;
		default:
			return false;
	}
}

static String f2sp0(float p_float) {
	String num = String::num_scientific(p_float);
	if (!num.contains_char('.') && !num.contains_char('e')) {
		num += ".0";
	}
	return num;
}

static String get_constant_text(SL::DataType p_type, const Vector<SL::Scalar> &p_values) {
	switch (p_type) {
		case SL::TYPE_BOOL:
			return p_values[0].boolean ? "true" : "false";
		case SL::TYPE_BVEC2:
		case SL::TYPE_BVEC3:
		case SL::TYPE_BVEC4: {
			String text = "bvec" + itos(p_type - SL::TYPE_BOOL + 1) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += p_values[i].boolean ? "true" : "false";
			}
			text += ")";
			return text;
		}

		case SL::TYPE_INT:
			return itos(p_values[0].sint);
		case SL::TYPE_IVEC2:
		case SL::TYPE_IVEC3:
		case SL::TYPE_IVEC4: {
			String text = "ivec" + itos(p_type - SL::TYPE_INT + 1) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += itos(p_values[i].sint);
			}
			text += ")";
			return text;

		} break;
		case SL::TYPE_UINT:
			return itos(p_values[0].uint) + "u";
		case SL::TYPE_UVEC2:
		case SL::TYPE_UVEC3:
		case SL::TYPE_UVEC4: {
			String text = "uvec" + itos(p_type - SL::TYPE_UINT + 1) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += itos(p_values[i].uint) + "u";
			}
			text += ")";
			return text;
		} break;
		case SL::TYPE_FLOAT:
			return f2sp0(p_values[0].real);
		case SL::TYPE_VEC2:
		case SL::TYPE_VEC3:
		case SL::TYPE_VEC4: {
			String text = "vec" + itos(p_type - SL::TYPE_FLOAT + 1) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += f2sp0(p_values[i].real);
			}
			text += ")";
			return text;

		} break;
		case SL::TYPE_MAT2:
		case SL::TYPE_MAT3:
		case SL::TYPE_MAT4: {
			String text = "mat" + itos(p_type - SL::TYPE_MAT2 + 2) + "(";
			for (int i = 0; i < p_values.size(); i++) {
				if (i > 0) {
					text += ",";
				}

				text += f2sp0(p_values[i].real);
			}
			text += ")";
			return text;

		} break;
		default:
			ERR_FAIL_V(String());
	}
}

String ShaderCompiler::_get_sampler_name(ShaderLanguage::TextureFilter p_filter, ShaderLanguage::TextureRepeat p_repeat) {
	if (p_filter == ShaderLanguage::FILTER_DEFAULT) {
		ERR_FAIL_COND_V(actions.default_filter == ShaderLanguage::FILTER_DEFAULT, String());
		p_filter = actions.default_filter;
	}
	if (p_repeat == ShaderLanguage::REPEAT_DEFAULT) {
		ERR_FAIL_COND_V(actions.default_repeat == ShaderLanguage::REPEAT_DEFAULT, String());
		p_repeat = actions.default_repeat;
	}
	constexpr const char *name_mapping[] = {
		"SAMPLER_NEAREST_CLAMP",
		"SAMPLER_LINEAR_CLAMP",
		"SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP",
		"SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP",
		"SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP",
		"SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP",
		"SAMPLER_NEAREST_REPEAT",
		"SAMPLER_LINEAR_REPEAT",
		"SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT",
		"SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT",
		"SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT",
		"SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT"
	};
	return String(name_mapping[p_filter + (p_repeat == ShaderLanguage::REPEAT_ENABLE ? ShaderLanguage::FILTER_DEFAULT : 0)]);
}

void ShaderCompiler::_dump_function_deps(const SL::ShaderNode *p_node, const StringName &p_for_func, const HashMap<StringName, String> &p_func_code, String &r_to_add, HashSet<StringName> &added) {
	int fidx = -1;

	for (int i = 0; i < p_node->vfunctions.size(); i++) {
		if (p_node->vfunctions[i].name == p_for_func) {
			fidx = i;
			break;
		}
	}

	ERR_FAIL_COND(fidx == -1);

	Vector<StringName> uses_functions;

	for (const StringName &E : p_node->vfunctions[fidx].uses_function) {
		uses_functions.push_back(E);
	}
	uses_functions.sort_custom<StringName::AlphCompare>(); //ensure order is deterministic so the same shader is always produced

	for (int k = 0; k < uses_functions.size(); k++) {
		if (added.has(uses_functions[k])) {
			continue; //was added already
		}

		_dump_function_deps(p_node, uses_functions[k], p_func_code, r_to_add, added);

		SL::FunctionNode *fnode = nullptr;

		for (int i = 0; i < p_node->vfunctions.size(); i++) {
			if (p_node->vfunctions[i].name == uses_functions[k]) {
				fnode = p_node->vfunctions[i].function;
				break;
			}
		}

		ERR_FAIL_NULL(fnode);

		r_to_add += "\n";

		String header;
		if (fnode->return_type == SL::TYPE_STRUCT) {
			header = _mkid(fnode->return_struct_name);
		} else {
			header = _typestr(fnode->return_type);
		}

		if (fnode->return_array_size > 0) {
			header += "[";
			header += itos(fnode->return_array_size);
			header += "]";
		}

		header += " ";
		header += _mkid(fnode->rname);
		header += "(";

		for (int i = 0; i < fnode->arguments.size(); i++) {
			if (i > 0) {
				header += ", ";
			}
			header += _constr(fnode->arguments[i].is_const);
			if (fnode->arguments[i].type == SL::TYPE_STRUCT) {
				header += _qualstr(fnode->arguments[i].qualifier) + _mkid(fnode->arguments[i].struct_name) + " " + _mkid(fnode->arguments[i].name);
			} else {
				header += _qualstr(fnode->arguments[i].qualifier) + _prestr(fnode->arguments[i].precision) + _typestr(fnode->arguments[i].type) + " " + _mkid(fnode->arguments[i].name);
			}
			if (fnode->arguments[i].array_size > 0) {
				header += "[";
				header += itos(fnode->arguments[i].array_size);
				header += "]";
			}
		}

		header += ")\n";
		r_to_add += header;
		r_to_add += p_func_code[uses_functions[k]];

		added.insert(uses_functions[k]);
	}
}

static String _get_global_shader_uniform_from_type_and_index(const String &p_buffer, const String &p_index, ShaderLanguage::DataType p_type, bool p_function_access) {
	const String open = p_function_access ? "(" : "[";
	const String close = p_function_access ? ")" : "]";

	const String v0 = p_buffer + open + p_index + close;

	switch (p_type) {
		case ShaderLanguage::TYPE_BOOL: {
			return "bool(floatBitsToUint(" + v0 + ".x))";
		}
		case ShaderLanguage::TYPE_BVEC2: {
			return "bvec2(floatBitsToUint(" + v0 + ".xy))";
		}
		case ShaderLanguage::TYPE_BVEC3: {
			return "bvec3(floatBitsToUint(" + v0 + ".xyz))";
		}
		case ShaderLanguage::TYPE_BVEC4: {
			return "bvec4(floatBitsToUint(" + v0 + ".xyzw))";
		}
		case ShaderLanguage::TYPE_INT: {
			return "floatBitsToInt(" + v0 + ".x)";
		}
		case ShaderLanguage::TYPE_IVEC2: {
			return "floatBitsToInt(" + v0 + ".xy)";
		}
		case ShaderLanguage::TYPE_IVEC3: {
			return "floatBitsToInt(" + v0 + ".xyz)";
		}
		case ShaderLanguage::TYPE_IVEC4: {
			return "floatBitsToInt(" + v0 + ".xyzw)";
		}
		case ShaderLanguage::TYPE_UINT: {
			return "floatBitsToUint(" + v0 + ".x)";
		}
		case ShaderLanguage::TYPE_UVEC2: {
			return "floatBitsToUint(" + v0 + ".xy)";
		}
		case ShaderLanguage::TYPE_UVEC3: {
			return "floatBitsToUint(" + v0 + ".xyz)";
		}
		case ShaderLanguage::TYPE_UVEC4: {
			return "floatBitsToUint(" + v0 + ".xyzw)";
		}
		case ShaderLanguage::TYPE_FLOAT: {
			return "(" + v0 + ".x)";
		}
		case ShaderLanguage::TYPE_VEC2: {
			return "(" + v0 + ".xy)";
		}
		case ShaderLanguage::TYPE_VEC3: {
			return "(" + v0 + ".xyz)";
		}
		case ShaderLanguage::TYPE_VEC4: {
			return "(" + v0 + ".xyzw)";
		}
		case ShaderLanguage::TYPE_MAT2: {
			const String v1 = p_buffer + open + p_index + "+1u" + close;
			return "mat2(" + v0 + ".xy," + v1 + ".xy)";
		}
		case ShaderLanguage::TYPE_MAT3: {
			const String v1 = p_buffer + open + p_index + "+1u" + close;
			const String v2 = p_buffer + open + p_index + "+2u" + close;

			return "mat3(" + v0 + ".xyz," + v1 + ".xyz," + v2 + ".xyz)";
		}
		case ShaderLanguage::TYPE_MAT4: {
			const String v1 = p_buffer + open + p_index + "+1u" + close;
			const String v2 = p_buffer + open + p_index + "+2u" + close;
			const String v3 = p_buffer + open + p_index + "+3u" + close;
			return "mat4(" + v0 + ".xyzw," + v1 + ".xyzw," + v2 + ".xyzw," + v3 + ".xyzw)";
		}
		default: {
			ERR_FAIL_V("void");
		}
	}
}

// Stands for the source footprint (ddx or ddy) in a scale template.
static constexpr const char *TEXTURE_FOOTPRINT_TOKEN = "$D";

static bool _is_swizzle_xy(const SL::MemberNode *p_member) {
	const String name = p_member->name;
	return name == "xy" || name == "rg" || name == "st";
}

bool ShaderCompiler::_is_uniform_expression(const SL::Node *p_node) const {
	switch (p_node->type) {
		case SL::Node::NODE_TYPE_CONSTANT:
			return true;
		case SL::Node::NODE_TYPE_VARIABLE: {
			const StringName &name = static_cast<const SL::VariableNode *>(p_node)->name;
			const SL::ShaderNode::Uniform *uniform = shader->uniforms.getptr(name);
			if (uniform) {
				return uniform->scope != SL::ShaderNode::Uniform::SCOPE_INSTANCE && !SL::is_sampler_type(uniform->type);
			}
			return shader->constants.has(name);
		}
		case SL::Node::NODE_TYPE_MEMBER: {
			const SL::MemberNode *mnode = static_cast<const SL::MemberNode *>(p_node);
			return mnode->index_expression == nullptr && mnode->call_expression == nullptr && _is_uniform_expression(mnode->owner);
		}
		case SL::Node::NODE_TYPE_OPERATOR: {
			const SL::OperatorNode *onode = static_cast<const SL::OperatorNode *>(p_node);
			if ((onode->op >= SL::OP_ASSIGN && onode->op <= SL::OP_ASSIGN_BIT_XOR) || onode->op == SL::OP_INCREMENT || onode->op == SL::OP_DECREMENT || onode->op == SL::OP_POST_INCREMENT || onode->op == SL::OP_POST_DECREMENT) {
				return false;
			}
			int first = 0;
			if (onode->op == SL::OP_CALL || onode->op == SL::OP_CONSTRUCT || onode->op == SL::OP_STRUCT) {
				if (onode->op == SL::OP_CALL && !internal_functions.has(static_cast<const SL::VariableNode *>(onode->arguments[0])->name)) {
					return false;
				}
				first = 1;
			}
			for (int i = first; i < onode->arguments.size(); i++) {
				if (!_is_uniform_expression(onode->arguments[i])) {
					return false;
				}
			}
			return true;
		}
		default:
			return false;
	}
}

// Affine scale of p_node with respect to the coord of texture_gradients[p_source], as a template (see COORD_SCALE_AFFINE).
// Factors must be uniform expressions so the template is valid anywhere in the shader, e.g. for feedback.
ShaderCompiler::CoordScale ShaderCompiler::_coord_scale(const SL::Node *p_node, int p_source, HashSet<StringName> &r_expanding, const DumpContext &p_context, String &r_scale) {
	if (!_node_reads(p_node, texture_coord_dependents[p_source])) {
		return COORD_SCALE_NONE;
	}
	const StringName &coord = p_context.actions->texture_gradients[p_source].coord;
	auto factor_code = [&](const SL::Node *p_factor) {
		return _dump_node_code(p_factor, 0, *p_context.gen_code, *p_context.actions, *p_context.default_actions, false);
	};

	switch (p_node->type) {
		case SL::Node::NODE_TYPE_VARIABLE: {
			const StringName &name = static_cast<const SL::VariableNode *>(p_node)->name;
			const LocalVector<const SL::Node *> *defs = dataflow_defs.getptr(name);
			const bool expand = defs && !r_expanding.has(name) && (name != coord || p_context.actions->texture_gradients_pre_vertex);
			if (!expand) {
				// Inside its own definition (`UV = UV * s`), coord is the source value.
				if (name != coord) {
					return COORD_SCALE_UNKNOWN;
				}
				r_scale = TEXTURE_FOOTPRINT_TOKEN;
				return COORD_SCALE_AFFINE;
			}
			if (defs->size() != 1) {
				return COORD_SCALE_UNKNOWN;
			}
			r_expanding.insert(name);
			const CoordScale result = _coord_scale((*defs)[0], p_source, r_expanding, p_context, r_scale);
			r_expanding.erase(name);
			return result;
		}
		case SL::Node::NODE_TYPE_MEMBER: {
			const SL::MemberNode *mnode = static_cast<const SL::MemberNode *>(p_node);
			if (!_is_swizzle_xy(mnode) || mnode->index_expression || mnode->call_expression) {
				return COORD_SCALE_UNKNOWN;
			}
			return _coord_scale(mnode->owner, p_source, r_expanding, p_context, r_scale);
		}
		case SL::Node::NODE_TYPE_OPERATOR: {
			const SL::OperatorNode *onode = static_cast<const SL::OperatorNode *>(p_node);
			switch (onode->op) {
				case SL::OP_ADD:
				case SL::OP_SUB: {
					String scale_a, scale_b;
					const CoordScale a = _coord_scale(onode->arguments[0], p_source, r_expanding, p_context, scale_a);
					const CoordScale b = _coord_scale(onode->arguments[1], p_source, r_expanding, p_context, scale_b);
					if (a == COORD_SCALE_AFFINE && b == COORD_SCALE_NONE) {
						r_scale = scale_a;
						return COORD_SCALE_AFFINE;
					}
					if (a == COORD_SCALE_NONE && b == COORD_SCALE_AFFINE) {
						r_scale = onode->op == SL::OP_SUB ? "(-" + scale_b + ")" : scale_b;
						return COORD_SCALE_AFFINE;
					}
					return COORD_SCALE_UNKNOWN;
				}
				case SL::OP_NEGATE: {
					String scale;
					if (_coord_scale(onode->arguments[0], p_source, r_expanding, p_context, scale) != COORD_SCALE_AFFINE) {
						return COORD_SCALE_UNKNOWN;
					}
					r_scale = "(-" + scale + ")";
					return COORD_SCALE_AFFINE;
				}
				case SL::OP_MUL:
				case SL::OP_DIV: {
					// The coordinate side must stay a vec2 so the scale applies to vec2 footprints.
					const bool coord_left = _node_reads(onode->arguments[0], texture_coord_dependents[p_source]);
					const SL::Node *coord_node = onode->arguments[coord_left ? 0 : 1];
					const SL::Node *factor = onode->arguments[coord_left ? 1 : 0];
					if (coord_node->get_datatype() != SL::TYPE_VEC2 || !_is_uniform_expression(factor) || (onode->op == SL::OP_DIV && !coord_left)) {
						return COORD_SCALE_UNKNOWN;
					}
					String scale;
					if (_coord_scale(coord_node, p_source, r_expanding, p_context, scale) != COORD_SCALE_AFFINE) {
						return COORD_SCALE_UNKNOWN;
					}
					const String factor_str = "(" + factor_code(factor) + ")";
					if (onode->op == SL::OP_DIV) {
						r_scale = "(" + scale + " / " + factor_str + ")";
					} else {
						r_scale = coord_left ? "(" + scale + " * " + factor_str + ")" : "(" + factor_str + " * " + scale + ")";
					}
					return COORD_SCALE_AFFINE;
				}
				case SL::OP_CALL: {
					// fract() and mod() tile without changing the scale.
					const StringName &callee = static_cast<const SL::VariableNode *>(onode->arguments[0])->name;
					const bool tiles = (callee == "fract" && onode->arguments.size() == 2) || (callee == "mod" && onode->arguments.size() == 3 && _is_uniform_expression(onode->arguments[2]));
					if (!tiles || !internal_functions.has(callee)) {
						return COORD_SCALE_UNKNOWN;
					}
					return _coord_scale(onode->arguments[1], p_source, r_expanding, p_context, r_scale);
				}
				case SL::OP_CONSTRUCT: {
					// vec2(c) and vec3(c, layer) keep the vec2 coordinate in their first components.
					const SL::Node *first = onode->arguments.size() > 1 ? onode->arguments[1] : nullptr;
					if (first == nullptr || first->get_datatype() != SL::TYPE_VEC2) {
						return COORD_SCALE_UNKNOWN;
					}
					for (int i = 2; i < onode->arguments.size(); i++) {
						if (_node_reads(onode->arguments[i], texture_coord_dependents[p_source])) {
							return COORD_SCALE_UNKNOWN;
						}
					}
					return _coord_scale(first, p_source, r_expanding, p_context, r_scale);
				}
				default:
					return COORD_SCALE_UNKNOWN;
			}
		}
		default:
			return COORD_SCALE_UNKNOWN;
	}
}

ShaderCompiler::TextureFootprint ShaderCompiler::_texture_footprint(const SL::Node *p_coord, const DumpContext &p_context) {
	TextureFootprint footprint;
	footprint.scale = TEXTURE_FOOTPRINT_TOKEN;
	for (int i = (int)texture_coord_dependents.size() - 1; i > 0; i--) {
		if (_node_reads(p_coord, texture_coord_dependents[i])) {
			footprint.source = i;
			break;
		}
	}
	HashSet<StringName> expanding;
	String scale;
	if (_coord_scale(p_coord, footprint.source, expanding, p_context, scale) == COORD_SCALE_AFFINE) {
		footprint.scale = scale;
	}
	return footprint;
}

String ShaderCompiler::_texture_footprint_code(const IdentifierActions &p_actions) const {
	if (texture_footprints.is_empty() || (texture_footprints.size() == 1 && texture_footprints[0].source == 0 && texture_footprints[0].scale == TEXTURE_FOOTPRINT_TOKEN)) {
		return String(); // Already the unscaled source footprint.
	}
	String code = "{\n";
	for (uint32_t i = 0; i < texture_footprints.size(); i++) {
		const TextureFootprint &footprint = texture_footprints[i];
		const IdentifierActions::TextureGradients &source = p_actions.texture_gradients[footprint.source];
		const String ddx = footprint.scale.replace(TEXTURE_FOOTPRINT_TOKEN, source.ddx);
		const String ddy = footprint.scale.replace(TEXTURE_FOOTPRINT_TOKEN, source.ddy);
		if (i == 0) {
			code += "\tvec2 footprint_ddx = " + ddx + ";\n\tvec2 footprint_ddy = " + ddy + ";\n";
			continue;
		}
		code += "\t{\n";
		code += "\t\tvec2 candidate_ddx = " + ddx + ";\n";
		code += "\t\tvec2 candidate_ddy = " + ddy + ";\n";
		code += "\t\tif (dot(candidate_ddx, candidate_ddx) + dot(candidate_ddy, candidate_ddy) < dot(footprint_ddx, footprint_ddx) + dot(footprint_ddy, footprint_ddy)) {\n";
		code += "\t\t\tfootprint_ddx = candidate_ddx;\n\t\t\tfootprint_ddy = candidate_ddy;\n\t\t}\n\t}\n";
	}
	code += "\t" + p_actions.texture_gradients[0].ddx + " = footprint_ddx;\n";
	code += "\t" + p_actions.texture_gradients[0].ddy + " = footprint_ddy;\n}\n";
	return code;
}

String ShaderCompiler::_dump_node_code(const SL::Node *p_node, int p_level, GeneratedCode &r_gen_code, IdentifierActions &p_actions, const DefaultIdentifierActions &p_default_actions, bool p_assigning, bool p_use_scope) {
	String code;

	switch (p_node->type) {
		case SL::Node::NODE_TYPE_SHADER: {
			SL::ShaderNode *pnode = (SL::ShaderNode *)p_node;

			// Render modes.

			for (int i = 0; i < pnode->render_modes.size(); i++) {
				if (p_default_actions.render_mode_defines.has(pnode->render_modes[i]) && !used_rmode_defines.has(pnode->render_modes[i])) {
					r_gen_code.defines.push_back(p_default_actions.render_mode_defines[pnode->render_modes[i]]);
					used_rmode_defines.insert(pnode->render_modes[i]);
				}

				if (p_actions.render_mode_flags.has(pnode->render_modes[i])) {
					*p_actions.render_mode_flags[pnode->render_modes[i]] = true;
				}

				if (p_actions.render_mode_values.has(pnode->render_modes[i])) {
					Pair<int *, int> &p = p_actions.render_mode_values[pnode->render_modes[i]];
					*p.first = p.second;
				}
			}

			// Stencil modes.

			for (int i = 0; i < pnode->stencil_modes.size(); i++) {
				if (p_default_actions.stencil_mode_defines.has(pnode->stencil_modes[i]) && !used_smode_defines.has(pnode->stencil_modes[i])) {
					r_gen_code.defines.push_back(p_default_actions.stencil_mode_defines[pnode->stencil_modes[i]]);
					used_smode_defines.insert(pnode->stencil_modes[i]);
				}

				if (p_actions.stencil_mode_values.has(pnode->stencil_modes[i])) {
					Pair<int *, int> &p = p_actions.stencil_mode_values[pnode->stencil_modes[i]];
					*p.first = p.second;
				}
			}

			// Stencil reference value.

			if (p_actions.stencil_reference && pnode->stencil_reference != -1) {
				*p_actions.stencil_reference = pnode->stencil_reference;
			}

			// structs

			for (int i = 0; i < pnode->vstructs.size(); i++) {
				SL::StructNode *st = pnode->vstructs[i].shader_struct;
				String struct_code;

				struct_code += "struct ";
				struct_code += _mkid(pnode->vstructs[i].name);
				struct_code += " ";
				struct_code += "{\n";
				for (SL::MemberNode *m : st->members) {
					if (m->datatype == SL::TYPE_STRUCT) {
						struct_code += _mkid(m->struct_name);
					} else {
						struct_code += _prestr(m->precision);
						struct_code += _typestr(m->datatype);
					}
					struct_code += " ";
					struct_code += _mkid(m->name);
					if (m->array_size > 0) {
						struct_code += "[";
						struct_code += itos(m->array_size);
						struct_code += "]";
					}
					struct_code += ";\n";
				}
				struct_code += "}";
				struct_code += ";\n";

				for (int j = 0; j < STAGE_MAX; j++) {
					r_gen_code.stage_globals[j] += struct_code;
				}
			}

			int max_texture_uniforms = 0;
			int max_uniforms = 0;

			for (const KeyValue<StringName, SL::ShaderNode::Uniform> &E : pnode->uniforms) {
				if (SL::is_sampler_type(E.value.type)) {
					if (E.value.hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE0 ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE1 ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE2 ||
							E.value.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
						continue; // Don't create uniforms in the generated code for these.
					}
					max_texture_uniforms++;
				} else {
					if (E.value.scope == SL::ShaderNode::Uniform::SCOPE_INSTANCE) {
						continue; // Instances are indexed directly, don't need index uniforms.
					}

					max_uniforms++;
				}
			}

			r_gen_code.texture_uniforms.resize(max_texture_uniforms);

			Vector<int> uniform_sizes;
			Vector<int> uniform_alignments;
			Vector<StringName> uniform_defines;
			uniform_sizes.resize(max_uniforms);
			uniform_alignments.resize(max_uniforms);
			uniform_defines.resize(max_uniforms);
			bool uses_uniforms = false;

			Vector<StringName> uniform_names;

			for (const KeyValue<StringName, SL::ShaderNode::Uniform> &E : pnode->uniforms) {
				uniform_names.push_back(E.key);
			}

			uniform_names.sort_custom<StringName::AlphCompare>(); //ensure order is deterministic so the same shader is always produced

			for (int k = 0; k < uniform_names.size(); k++) {
				const StringName &uniform_name = uniform_names[k];
				const SL::ShaderNode::Uniform &uniform = pnode->uniforms[uniform_name];

				String ucode;

				if (uniform.scope == SL::ShaderNode::Uniform::SCOPE_INSTANCE) {
					//insert, but don't generate any code.
					p_actions.uniforms->insert(uniform_name, uniform);
					continue; // Instances are indexed directly, don't need index uniforms.
				}

				if (uniform.hint == SL::ShaderNode::Uniform::HINT_SCREEN_TEXTURE ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_DEPTH_TEXTURE ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE0 ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE1 ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE2 ||
						uniform.hint == SL::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
					continue; // Don't create uniforms in the generated code for these.
				}

				if (SL::is_sampler_type(uniform.type)) {
					// Texture layouts are different for OpenGL GLSL and Vulkan GLSL
					if (!RS::get_singleton()->is_low_end()) {
						ucode = "layout(set = " + itos(actions.texture_layout_set) + ", binding = " + itos(actions.base_texture_binding_index + uniform.texture_binding) + ") ";
					}
					ucode += "uniform ";
				}

				bool is_buffer_global = !SL::is_sampler_type(uniform.type) && uniform.scope == SL::ShaderNode::Uniform::SCOPE_GLOBAL;

				if (is_buffer_global) {
					//this is an integer to index the global table
					ucode += _typestr(ShaderLanguage::TYPE_UINT);
				} else {
					ucode += _prestr(uniform.precision, ShaderLanguage::is_float_type(uniform.type));
					ucode += _typestr(uniform.type);
				}

				ucode += " " + _mkid(uniform_name);
				if (uniform.array_size > 0) {
					ucode += "[";
					ucode += itos(uniform.array_size);
					ucode += "]";
				}
				ucode += ";\n";
				if (SL::is_sampler_type(uniform.type)) {
					for (int j = 0; j < STAGE_MAX; j++) {
						r_gen_code.stage_globals[j] += ucode;
					}

					GeneratedCode::Texture texture;
					texture.name = uniform_name;
					texture.hint = uniform.hint;
					texture.type = uniform.type;
					texture.use_color = uniform.use_color;
					texture.filter = uniform.filter;
					texture.repeat = uniform.repeat;
					texture.global = uniform.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL;
					texture.array_size = uniform.array_size;
					if (texture.global) {
						r_gen_code.uses_global_textures = true;
					}

					r_gen_code.texture_uniforms.write[uniform.texture_order] = texture;
				} else {
					if (!uses_uniforms) {
						uses_uniforms = true;
					}
					uniform_defines.write[uniform.order] = ucode;
					if (is_buffer_global) {
						//globals are indices into the global table
						uniform_sizes.write[uniform.order] = ShaderLanguage::get_datatype_size(ShaderLanguage::TYPE_UINT);
						uniform_alignments.write[uniform.order] = _get_datatype_alignment(ShaderLanguage::TYPE_UINT);
					} else {
						// The following code enforces a 16-byte alignment of uniform arrays.
						if (uniform.array_size > 0) {
							int size = ShaderLanguage::get_datatype_size(uniform.type) * uniform.array_size;
							int m = (16 * uniform.array_size);
							if ((size % m) != 0) {
								size += m - (size % m);
							}
							uniform_sizes.write[uniform.order] = size;
							uniform_alignments.write[uniform.order] = 16;
						} else {
							uniform_sizes.write[uniform.order] = ShaderLanguage::get_datatype_size(uniform.type);
							uniform_alignments.write[uniform.order] = _get_datatype_alignment(uniform.type);
						}
					}
				}

				p_actions.uniforms->insert(uniform_name, uniform);
			}

			for (int i = 0; i < max_uniforms; i++) {
				r_gen_code.uniforms += uniform_defines[i];
			}

			// add up
			int offset = 0;
			for (int i = 0; i < uniform_sizes.size(); i++) {
				int align = offset % uniform_alignments[i];

				if (align != 0) {
					offset += uniform_alignments[i] - align;
				}

				r_gen_code.uniform_offsets.push_back(offset);

				offset += uniform_sizes[i];
			}

			r_gen_code.uniform_total_size = offset;

			if (r_gen_code.uniform_total_size % 16 != 0) { //UBO sizes must be multiples of 16
				r_gen_code.uniform_total_size += 16 - (r_gen_code.uniform_total_size % 16);
			}

			uint32_t index = p_default_actions.base_varying_index;

			List<Pair<StringName, SL::ShaderNode::Varying>> var_frag_to_light;

			Vector<StringName> varying_names;

			for (const KeyValue<StringName, SL::ShaderNode::Varying> &E : pnode->varyings) {
				varying_names.push_back(E.key);
			}

			varying_names.sort_custom<StringName::AlphCompare>(); //ensure order is deterministic so the same shader is always produced

			for (int k = 0; k < varying_names.size(); k++) {
				const StringName &varying_name = varying_names[k];
				const SL::ShaderNode::Varying &varying = pnode->varyings[varying_name];

				if (varying.stage == SL::ShaderNode::Varying::STAGE_FRAGMENT && !p_default_actions.suppress_varying_io) {
					var_frag_to_light.push_back(Pair<StringName, SL::ShaderNode::Varying>(varying_name, varying));
					fragment_varyings.insert(varying_name);
					continue;
				}
				if (varying.type < SL::TYPE_INT) {
					continue; // Ignore boolean types to prevent crashing (if varying is just declared).
				}

				String type_str = _prestr(varying.precision, ShaderLanguage::is_float_type(varying.type)) + _typestr(varying.type);
				String name_str = _mkid(varying_name);
				uint32_t inc = varying.get_size();

				if (p_default_actions.suppress_varying_io) {
					// No vertex stage (e.g. RT shaders): emit zero-initialized
					// globals instead of in/out IO declarations.
					String decl = type_str + " " + name_str;
					if (varying.array_size > 0) {
						decl += "[" + itos(varying.array_size) + "];\n";
					} else {
						decl += " = " + _typestr(varying.type) + "(0);\n";
					}
					r_gen_code.stage_globals[STAGE_FRAGMENT] += decl;
				} else {
					String vcode;
					String interp_mode = _interpstr(varying.interpolation);
					vcode += type_str;
					vcode += " " + name_str;

					if (varying.array_size > 0) {
						vcode += "[";
						vcode += itos(varying.array_size);
						vcode += "]";
					}

					vcode += ";\n";
					// GLSL ES 3.0 does not allow layout qualifiers for varyings
					if (!RS::get_singleton()->is_low_end()) {
						r_gen_code.stage_globals[STAGE_VERTEX] += "layout(location=" + itos(index) + ") ";
						r_gen_code.stage_globals[STAGE_FRAGMENT] += "layout(location=" + itos(index) + ") ";
					}
					r_gen_code.stage_globals[STAGE_VERTEX] += interp_mode + "out " + vcode;
					r_gen_code.stage_globals[STAGE_FRAGMENT] += interp_mode + "in " + vcode;
				}

				index += inc;
			}

			if (!p_default_actions.suppress_varying_io && var_frag_to_light.size() > 0) {
				String gcode = "\n\nstruct {\n";
				for (const Pair<StringName, SL::ShaderNode::Varying> &E : var_frag_to_light) {
					gcode += "\t" + _prestr(E.second.precision) + _typestr(E.second.type) + " " + _mkid(E.first);
					if (E.second.array_size > 0) {
						gcode += "[";
						gcode += itos(E.second.array_size);
						gcode += "]";
					}
					gcode += ";\n";
				}
				gcode += "} frag_to_light;\n";
				r_gen_code.stage_globals[STAGE_FRAGMENT] += gcode;
			}

			for (int i = 0; i < pnode->vconstants.size(); i++) {
				const SL::ShaderNode::Constant &cnode = pnode->vconstants[i];
				String gcode;
				gcode += _constr(true);
				gcode += _prestr(cnode.precision, ShaderLanguage::is_float_type(cnode.type));
				if (cnode.type == SL::TYPE_STRUCT) {
					gcode += _mkid(cnode.struct_name);
				} else {
					gcode += _typestr(cnode.type);
				}
				gcode += " " + _mkid(String(cnode.name));
				if (cnode.array_size > 0) {
					gcode += "[";
					gcode += itos(cnode.array_size);
					gcode += "]";
				}
				gcode += "=";
				gcode += _dump_node_code(cnode.initializer, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				gcode += ";\n";
				for (int j = 0; j < STAGE_MAX; j++) {
					r_gen_code.stage_globals[j] += gcode;
				}
			}

			HashMap<StringName, String> function_code;

			//code for functions
			for (int i = 0; i < pnode->vfunctions.size(); i++) {
				SL::FunctionNode *fnode = pnode->vfunctions[i].function;
				function = fnode;
				current_func_name = fnode->name;
				function_code[fnode->name] = _dump_node_code(fnode->body, p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				function = nullptr;
			}

			//place functions in actual code

			HashSet<StringName> added_funcs_per_stage[STAGE_MAX];

			for (int i = 0; i < pnode->vfunctions.size(); i++) {
				SL::FunctionNode *fnode = pnode->vfunctions[i].function;

				function = fnode;

				current_func_name = fnode->name;

				if (p_actions.entry_point_stages.has(fnode->name)) {
					Stage stage = p_actions.entry_point_stages[fnode->name];
					Stage dep_stage = (p_default_actions.suppress_varying_io && stage == STAGE_VERTEX) ? STAGE_FRAGMENT : stage;
					_dump_function_deps(pnode, fnode->name, function_code, r_gen_code.stage_globals[dep_stage], added_funcs_per_stage[dep_stage]);
					r_gen_code.code[fnode->name] = function_code[fnode->name];
				}

				function = nullptr;
			}

			//code+=dump_node_code(pnode->body,p_level);
		} break;
		case SL::Node::NODE_TYPE_STRUCT: {
		} break;
		case SL::Node::NODE_TYPE_FUNCTION: {
		} break;
		case SL::Node::NODE_TYPE_BLOCK: {
			SL::BlockNode *bnode = (SL::BlockNode *)p_node;

			//variables
			if (!bnode->single_statement) {
				code += _mktab(p_level - 1) + "{\n";
			}

			int i = 0;
			for (List<ShaderLanguage::Node *>::ConstIterator itr = bnode->statements.begin(); itr != bnode->statements.end(); ++itr, ++i) {
				String scode = _dump_node_code(*itr, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);

				if ((*itr)->type == SL::Node::NODE_TYPE_CONTROL_FLOW || bnode->single_statement) {
					code += scode; //use directly
					if (bnode->use_comma_between_statements && i + 1 < bnode->statements.size()) {
						code += ",";
					}
				} else {
					code += _mktab(p_level) + scode + ";\n";
				}
			}
			if (!bnode->single_statement) {
				code += _mktab(p_level - 1) + "}\n";
			}

		} break;
		case SL::Node::NODE_TYPE_VARIABLE_DECLARATION: {
			SL::VariableDeclarationNode *vdnode = (SL::VariableDeclarationNode *)p_node;

			String declaration;
			declaration += _constr(vdnode->is_const);
			if (vdnode->datatype == SL::TYPE_STRUCT) {
				declaration += _mkid(vdnode->struct_name);
			} else {
				declaration += _prestr(vdnode->precision) + _typestr(vdnode->datatype);
			}
			declaration += " ";
			for (int i = 0; i < vdnode->declarations.size(); i++) {
				bool is_array = vdnode->declarations[i].size > 0;
				if (i > 0) {
					declaration += ",";
				}
				declaration += _mkid(vdnode->declarations[i].name);
				if (is_array) {
					declaration += "[";
					if (vdnode->declarations[i].size_expression != nullptr) {
						declaration += _dump_node_code(vdnode->declarations[i].size_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					} else {
						declaration += itos(vdnode->declarations[i].size);
					}
					declaration += "]";
				}

				if (!is_array || vdnode->declarations[i].single_expression) {
					if (!vdnode->declarations[i].initializer.is_empty()) {
						declaration += "=";
						declaration += _dump_node_code(vdnode->declarations[i].initializer[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					}
				} else {
					int size = vdnode->declarations[i].initializer.size();
					if (size > 0) {
						declaration += "=";
						if (vdnode->datatype == SL::TYPE_STRUCT) {
							declaration += _mkid(vdnode->struct_name);
						} else {
							declaration += _typestr(vdnode->datatype);
						}
						declaration += "[";
						declaration += itos(size);
						declaration += "]";
						declaration += "(";
						for (int j = 0; j < size; j++) {
							if (j > 0) {
								declaration += ",";
							}
							declaration += _dump_node_code(vdnode->declarations[i].initializer[j], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
						}
						declaration += ")";
					}
				}
			}

			code += declaration;
		} break;
		case SL::Node::NODE_TYPE_VARIABLE: {
			SL::VariableNode *vnode = (SL::VariableNode *)p_node;
			bool use_fragment_varying = false;

			if (!p_default_actions.suppress_varying_io && !vnode->is_local && !(p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX)) {
				if (p_assigning) {
					if (shader->varyings.has(vnode->name)) {
						use_fragment_varying = true;
					}
				} else {
					if (fragment_varyings.has(vnode->name)) {
						use_fragment_varying = true;
					}
				}
			}

			if (p_assigning && p_actions.write_flag_pointers.has(vnode->name)) {
				*p_actions.write_flag_pointers[vnode->name] = true;
			}

			if (p_default_actions.usage_defines.has(vnode->name) && !used_name_defines.has(vnode->name)) {
				String define = p_default_actions.usage_defines[vnode->name];
				if (define.begins_with("@")) {
					define = p_default_actions.usage_defines[define.substr(1)];
				}
				r_gen_code.defines.push_back(define);
				used_name_defines.insert(vnode->name);
			}

			if (p_actions.usage_flag_pointers.has(vnode->name) && !used_flag_pointers.has(vnode->name)) {
				*p_actions.usage_flag_pointers[vnode->name] = true;
				used_flag_pointers.insert(vnode->name);
			}

			if (p_default_actions.renames.has(vnode->name)) {
				code = p_default_actions.renames[vnode->name];
			} else {
				bool param_found = false;
				if (function) {
					for (const SL::FunctionNode::Argument &argument : function->arguments) {
						if (argument.name == vnode->name) {
							param_found = true;
							break;
						}
					}
				}
				if (!param_found && shader->uniforms.has(vnode->name)) {
					//its a uniform!
					const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[vnode->name];
					if (u.is_texture()) {
						StringName name;
						if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
							name = "color_buffer";
							if (u.filter >= ShaderLanguage::FILTER_NEAREST_MIPMAP) {
								r_gen_code.uses_screen_texture_mipmaps = true;
							}
							r_gen_code.uses_screen_texture = true;
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
							name = "normal_roughness_buffer";
							r_gen_code.uses_normal_roughness_texture = true;
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
							name = "depth_buffer";
							r_gen_code.uses_depth_texture = true;
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE0) {
							name = "source0";
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE1) {
							name = "source1";
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE2) {
							name = "source2";
						} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE3) {
							name = "source3";
						} else {
							name = _mkid(vnode->name); //texture, use as is
						}

						code = name;
					} else {
						//a scalar or vector
						if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
							code = actions.base_uniform_string + _mkid(vnode->name); //texture, use as is
							//global variable, this means the code points to an index to the global table
							code = _get_global_shader_uniform_from_type_and_index(p_default_actions.global_buffer_array_variable, code, u.type, p_default_actions.global_buffer_array_is_texture);
						} else if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
							//instance variable, index it as such
							code = "(" + p_default_actions.instance_uniform_index_variable + "+" + itos(u.instance_index) + "u)";
							code = _get_global_shader_uniform_from_type_and_index(p_default_actions.global_buffer_array_variable, code, u.type, p_default_actions.global_buffer_array_is_texture);
						} else {
							//regular uniform, index from UBO
							code = actions.base_uniform_string + _mkid(vnode->name);
						}
					}

				} else {
					if (use_fragment_varying) {
						code = "frag_to_light.";
					}
					code += _mkid(vnode->name); //its something else (local var most likely) use as is
				}
			}

			if (vnode->name == time_name) {
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX) {
					r_gen_code.uses_vertex_time = true;
				}
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_FRAGMENT) {
					r_gen_code.uses_fragment_time = true;
				}
			}

		} break;
		case SL::Node::NODE_TYPE_ARRAY_CONSTRUCT: {
			SL::ArrayConstructNode *acnode = (SL::ArrayConstructNode *)p_node;
			int sz = acnode->initializer.size();
			if (acnode->datatype == SL::TYPE_STRUCT) {
				code += _mkid(acnode->struct_name);
			} else {
				code += _typestr(acnode->datatype);
			}
			code += "[";
			code += itos(acnode->initializer.size());
			code += "]";
			code += "(";
			for (int i = 0; i < sz; i++) {
				code += _dump_node_code(acnode->initializer[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				if (i != sz - 1) {
					code += ", ";
				}
			}
			code += ")";
		} break;
		case SL::Node::NODE_TYPE_ARRAY: {
			SL::ArrayNode *anode = (SL::ArrayNode *)p_node;
			bool use_fragment_varying = false;

			if (!p_default_actions.suppress_varying_io && !anode->is_local && !(p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX)) {
				if (anode->assign_expression != nullptr && shader->varyings.has(anode->name)) {
					use_fragment_varying = true;
				} else {
					if (p_assigning) {
						if (shader->varyings.has(anode->name)) {
							use_fragment_varying = true;
						}
					} else {
						if (fragment_varyings.has(anode->name)) {
							use_fragment_varying = true;
						}
					}
				}
			}

			if (p_assigning && p_actions.write_flag_pointers.has(anode->name)) {
				*p_actions.write_flag_pointers[anode->name] = true;
			}

			if (p_default_actions.usage_defines.has(anode->name) && !used_name_defines.has(anode->name)) {
				String define = p_default_actions.usage_defines[anode->name];
				if (define.begins_with("@")) {
					define = p_default_actions.usage_defines[define.substr(1)];
				}
				r_gen_code.defines.push_back(define);
				used_name_defines.insert(anode->name);
			}

			if (p_actions.usage_flag_pointers.has(anode->name) && !used_flag_pointers.has(anode->name)) {
				*p_actions.usage_flag_pointers[anode->name] = true;
				used_flag_pointers.insert(anode->name);
			}

			if (p_default_actions.renames.has(anode->name)) {
				code = p_default_actions.renames[anode->name];
			} else {
				bool param_found = false;
				if (function) {
					for (const SL::FunctionNode::Argument &argument : function->arguments) {
						if (argument.name == anode->name) {
							param_found = true;
							break;
						}
					}
				}
				if (!param_found && shader->uniforms.has(anode->name)) {
					//its a uniform!
					const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[anode->name];
					if (u.is_texture()) {
						code = _mkid(anode->name); //texture, use as is
					} else {
						//a scalar or vector
						if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
							code = actions.base_uniform_string + _mkid(anode->name); //texture, use as is
							//global variable, this means the code points to an index to the global table
							code = _get_global_shader_uniform_from_type_and_index(p_default_actions.global_buffer_array_variable, code, u.type, p_default_actions.global_buffer_array_is_texture);
						} else if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
							//instance variable, index it as such
							code = "(" + p_default_actions.instance_uniform_index_variable + "+" + itos(u.instance_index) + "u)";
							code = _get_global_shader_uniform_from_type_and_index(p_default_actions.global_buffer_array_variable, code, u.type, p_default_actions.global_buffer_array_is_texture);
						} else {
							//regular uniform, index from UBO
							code = actions.base_uniform_string + _mkid(anode->name);
						}
					}
				} else {
					if (use_fragment_varying) {
						code = "frag_to_light.";
					}
					code += _mkid(anode->name);
				}
			}

			if (anode->call_expression != nullptr) {
				code += ".";
				code += _dump_node_code(anode->call_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning, false);
			} else if (anode->index_expression != nullptr) {
				code += "[";
				code += _dump_node_code(anode->index_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += "]";
			} else if (anode->assign_expression != nullptr) {
				code += "=";
				code += _dump_node_code(anode->assign_expression, p_level, r_gen_code, p_actions, p_default_actions, true, false);
			}

			if (anode->name == time_name) {
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_VERTEX) {
					r_gen_code.uses_vertex_time = true;
				}
				if (p_actions.entry_point_stages.has(current_func_name) && p_actions.entry_point_stages[current_func_name] == STAGE_FRAGMENT) {
					r_gen_code.uses_fragment_time = true;
				}
			}

		} break;
		case SL::Node::NODE_TYPE_CONSTANT: {
			SL::ConstantNode *cnode = (SL::ConstantNode *)p_node;

			if (cnode->array_size == 0) {
				return get_constant_text(cnode->datatype, cnode->values);
			} else {
				if (cnode->get_datatype() == SL::TYPE_STRUCT) {
					code += _mkid(cnode->struct_name);
				} else {
					code += _typestr(cnode->datatype);
				}
				code += "[";
				code += itos(cnode->array_size);
				code += "]";
				code += "(";
				for (int i = 0; i < cnode->array_size; i++) {
					if (i > 0) {
						code += ",";
					} else {
						code += "";
					}
					code += _dump_node_code(cnode->array_declarations[0].initializer[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				}
				code += ")";
			}

		} break;
		case SL::Node::NODE_TYPE_OPERATOR: {
			SL::OperatorNode *onode = (SL::OperatorNode *)p_node;

			switch (onode->op) {
				case SL::OP_ASSIGN:
				case SL::OP_ASSIGN_ADD:
				case SL::OP_ASSIGN_SUB:
				case SL::OP_ASSIGN_MUL:
				case SL::OP_ASSIGN_DIV:
				case SL::OP_ASSIGN_SHIFT_LEFT:
				case SL::OP_ASSIGN_SHIFT_RIGHT:
				case SL::OP_ASSIGN_MOD:
				case SL::OP_ASSIGN_BIT_AND:
				case SL::OP_ASSIGN_BIT_OR:
				case SL::OP_ASSIGN_BIT_XOR:
					code = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, true) + _opstr(onode->op) + _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					break;
				case SL::OP_BIT_INVERT:
				case SL::OP_NEGATE:
				case SL::OP_NOT:
				case SL::OP_DECREMENT:
				case SL::OP_INCREMENT: {
					const String node_code = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);

					if (onode->op == SL::OP_NEGATE && node_code.begins_with("-")) { // To prevent writing unary minus twice.
						code = node_code;
					} else {
						code = _opstr(onode->op) + node_code;
					}

				} break;
				case SL::OP_POST_DECREMENT:
				case SL::OP_POST_INCREMENT:
					code = _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + _opstr(onode->op);
					break;
				case SL::OP_CALL:
				case SL::OP_STRUCT:
				case SL::OP_CONSTRUCT: {
					ERR_FAIL_COND_V(onode->arguments[0]->type != SL::Node::NODE_TYPE_VARIABLE, String());
					const SL::VariableNode *vnode = static_cast<const SL::VariableNode *>(onode->arguments[0]);
					const SL::FunctionNode *func = nullptr;
					const bool is_internal_func = internal_functions.has(vnode->name);

					if (!is_internal_func) {
						for (int i = 0; i < shader->vfunctions.size(); i++) {
							if (shader->vfunctions[i].name == vnode->name) {
								func = shader->vfunctions[i].function;
								break;
							}
						}
					}

					bool is_texture_func = false;
					bool is_screen_texture = false;
					bool is_radiance_texture = false;
					bool texture_func_no_uv = false;
					bool texture_func_returns_data = false;
					bool texture_func_simple = false;
					bool texture_explicit = false; // texture() rewritten to textureGrad/textureLod.
					bool texture_grad = false;
					TextureFootprint texture_footprint;
					String texture_bias;
					int texture_call_start = 0;
					int texture_args_start = 0;
					int texture_sampler_end = 0;
					String texture_debug_prefix;

					if (onode->op == SL::OP_STRUCT) {
						code += _mkid(vnode->name);
					} else if (onode->op == SL::OP_CONSTRUCT) {
						code += String(vnode->name);
					} else {
						if (p_actions.usage_flag_pointers.has(vnode->name) && !used_flag_pointers.has(vnode->name)) {
							*p_actions.usage_flag_pointers[vnode->name] = true;
							used_flag_pointers.insert(vnode->name);
						}

						if (is_internal_func) {
							is_texture_func = texture_functions.has(vnode->name);
							texture_func_no_uv = (vnode->name == "textureSize" || vnode->name == "textureQueryLevels");
							texture_func_returns_data = texture_func_no_uv || vnode->name == "textureQueryLod";
							texture_func_simple = vnode->name == "texture";
							if (texture_func_simple && !p_actions.texture_gradients.is_empty() && onode->arguments.size() > 2) {
								texture_grad = _texture_grad_takes_uv(onode->arguments[1]->get_datatype());
								if (texture_grad) {
									texture_footprint = _texture_footprint(onode->arguments[2], { &r_gen_code, &p_actions, &p_default_actions });
									if (!texture_footprints.has(texture_footprint)) {
										texture_footprints.push_back(texture_footprint);
									}
								}
								texture_explicit = p_actions.texture_explicit_gradients;
							}
							if (texture_explicit) {
								texture_grad = _texture_grad_takes_uv(onode->arguments[1]->get_datatype());
								texture_call_start = code.length();
								code += texture_grad ? "textureGrad" : "textureLod";
							} else {
								const String *renamed_function = p_actions.function_renames.getptr(vnode->name);
								code += renamed_function ? *renamed_function : String(vnode->name);
							}
						} else if (p_default_actions.renames.has(vnode->name)) {
							code += p_default_actions.renames[vnode->name];
						} else {
							code += _mkid(vnode->rname);
						}
					}

					code += "(";
					texture_args_start = code.length();

					// if color backbuffer, depth backbuffer or normal roughness texture is used,
					// we will add logic to automatically switch between
					// sampler2D and sampler2D array and vec2 UV and vec3 UV.
					bool multiview_uv_needed = false;
					bool is_normal_roughness_texture = false;

					for (int i = 1; i < onode->arguments.size(); i++) {
						if (texture_explicit && i == 3) {
							texture_bias = _dump_node_code(onode->arguments[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
							continue;
						}
						if (i == 2) {
							texture_sampler_end = code.length();
						}
						if (i > 1) {
							code += ", ";
						}

						bool is_out_qualifier = false;
						if (is_internal_func) {
							is_out_qualifier = SL::is_builtin_func_out_parameter(vnode->name, i - 1);
						} else if (func != nullptr) {
							const SL::ArgumentQualifier qualifier = func->arguments[i - 1].qualifier;
							is_out_qualifier = qualifier == SL::ARGUMENT_QUALIFIER_OUT || qualifier == SL::ARGUMENT_QUALIFIER_INOUT;
						}

						if (is_out_qualifier) {
							StringName name;
							bool found = false;
							{
								const SL::Node *node = onode->arguments[i];

								bool done = false;
								do {
									switch (node->type) {
										case SL::Node::NODE_TYPE_VARIABLE: {
											name = static_cast<const SL::VariableNode *>(node)->name;
											done = true;
											found = true;
										} break;
										case SL::Node::NODE_TYPE_MEMBER: {
											node = static_cast<const SL::MemberNode *>(node)->owner;
										} break;
										default: {
											done = true;
										} break;
									}
								} while (!done);
							}

							if (found && p_actions.write_flag_pointers.has(name)) {
								*p_actions.write_flag_pointers[name] = true;
							}
						}

						String node_code = _dump_node_code(onode->arguments[i], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
						if (is_texture_func && i == 1) {
							// If we're doing a texture lookup we need to check our texture argument
							StringName texture_uniform;
							bool correct_texture_uniform = false;

							switch (onode->arguments[i]->type) {
								case SL::Node::NODE_TYPE_VARIABLE: {
									const SL::VariableNode *varnode = static_cast<const SL::VariableNode *>(onode->arguments[i]);
									texture_uniform = varnode->name;
									correct_texture_uniform = true;
								} break;
								case SL::Node::NODE_TYPE_ARRAY: {
									const SL::ArrayNode *anode = static_cast<const SL::ArrayNode *>(onode->arguments[i]);
									texture_uniform = anode->name;
									correct_texture_uniform = true;
								} break;
								default:
									break;
							}

							if (correct_texture_uniform && !RS::get_singleton()->is_low_end()) {
								// Need to map from texture to sampler in order to sample when using Vulkan GLSL.
								String sampler_name;
								bool is_depth_texture = false;

								if (actions.custom_samplers.has(texture_uniform)) {
									sampler_name = actions.custom_samplers[texture_uniform];
								} else {
									if (shader->uniforms.has(texture_uniform)) {
										const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[texture_uniform];
										if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
											is_screen_texture = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
											is_depth_texture = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
											is_normal_roughness_texture = true;
										}
										sampler_name = _get_sampler_name(u.filter, u.repeat);
									} else {
										bool found = false;

										for (int j = 0; j < function->arguments.size(); j++) {
											if (function->arguments[j].name == texture_uniform) {
												if (function->arguments[j].tex_builtin_check) {
													ERR_CONTINUE(!actions.custom_samplers.has(function->arguments[j].tex_builtin));
													sampler_name = actions.custom_samplers[function->arguments[j].tex_builtin];
													found = true;
													break;
												}
												if (function->arguments[j].tex_argument_check) {
													if (function->arguments[j].tex_hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
														is_screen_texture = true;
													} else if (function->arguments[j].tex_hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
														is_depth_texture = true;
													} else if (function->arguments[j].tex_hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
														is_normal_roughness_texture = true;
													}
													sampler_name = _get_sampler_name(function->arguments[j].tex_argument_filter, function->arguments[j].tex_argument_repeat);
													found = true;
													break;
												}
											}
										}
										if (!found) {
											//function was most likely unused, so use anything (compiler will remove it anyway)
											sampler_name = _get_sampler_name(ShaderLanguage::FILTER_DEFAULT, ShaderLanguage::REPEAT_DEFAULT);
										}
									}
								}

								if (texture_uniform == SNAME("RADIANCE")) {
									is_radiance_texture = true;
								}

								String data_type_name = "";
								if (actions.check_multiview_samplers && (is_screen_texture || is_depth_texture || is_normal_roughness_texture)) {
									data_type_name = "multiviewSampler";
									multiview_uv_needed = true;
								} else if (is_radiance_texture) {
									// We need to use an explicit level of detail to avoid mip mapping artifacts caused by the octahedral discontinuity.
									if (texture_func_simple) {
										code = code.replace("texture(", "textureLod(");
									}
									data_type_name = "sampler2D";
								} else {
									data_type_name = ShaderLanguage::get_datatype_name(onode->arguments[i]->get_datatype());
								}

								code += data_type_name + "(" + node_code + ", " + sampler_name + ")";
							} else if (correct_texture_uniform && RS::get_singleton()->is_low_end()) {
								// Texture function on low end hardware (i.e. OpenGL).

								if (shader->uniforms.has(texture_uniform)) {
									const ShaderLanguage::ShaderNode::Uniform &u = shader->uniforms[texture_uniform];
									if (actions.check_multiview_samplers) {
										if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
											multiview_uv_needed = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE) {
											multiview_uv_needed = true;
										} else if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE) {
											multiview_uv_needed = true;
										}
									}
									if (u.hint == ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE) {
										is_screen_texture = true;
									}
								}

								code += node_code;
							} else {
								code += node_code;
							}
						} else if (multiview_uv_needed && !texture_func_no_uv && i == 2) {
							// UV coordinate after using color, depth or normal roughness texture.
							node_code = "multiview_uv(" + node_code + ".xy)";

							code += node_code;
						} else if (is_radiance_texture && !texture_func_no_uv && i == 2) {
							node_code = "vec3_to_oct_with_border(" + node_code + ", params.border_size)";

							// Need an explicit level of detail if one isn't provided by the user.
							if (texture_func_simple && onode->arguments.size() == 3) {
								node_code += ", 0.0";
							}

							code += node_code;
						} else {
							code += node_code;
						}
					}
					if (texture_explicit && texture_grad) {
						// Bias scales the footprint by 2^bias, like a raster LOD bias.
						const IdentifierActions::TextureGradients &source = p_actions.texture_gradients[texture_footprint.source];
						const String bias_scale = texture_bias.is_empty() ? String() : " * exp2(" + texture_bias + ")";
						const String grads = ", " + texture_footprint.scale.replace(TEXTURE_FOOTPRINT_TOKEN, source.ddx) + bias_scale +
								", " + texture_footprint.scale.replace(TEXTURE_FOOTPRINT_TOKEN, source.ddy) + bias_scale;
						if (!p_actions.texture_debug_macro.is_empty()) {
							const SL::Node *sampler = onode->arguments[1];
							const SL::ShaderNode::Uniform *uniform = sampler->type == SL::Node::NODE_TYPE_VARIABLE ? shader->uniforms.getptr(static_cast<const SL::VariableNode *>(sampler)->name) : nullptr;
							const bool albedo = uniform && uniform->use_color; // source_color on a sampler.
							texture_debug_prefix = p_actions.texture_debug_macro + "(" + code.substr(texture_args_start, texture_sampler_end - texture_args_start) + grads + (albedo ? ", 2) " : ", 1) ");
						}
						if (texture_bias.is_empty()) {
							code += grads;
						} else {
							// Zero gradients (no ray cone) would drop the bias; the closing ")" below ends the ternary.
							const String call = code.substr(texture_call_start);
							code = code.substr(0, texture_call_start) + "(" + source.ddx + " != vec2(0.0) ? " + call + grads + ") : textureLod(" +
									code.substr(texture_args_start) + ", " + texture_bias + ")";
						}
					} else if (texture_explicit) {
						code += ", " + (texture_bias.is_empty() ? String("0.0") : texture_bias);
					}
					code += ")";
					if (!texture_debug_prefix.is_empty()) {
						code = code.substr(0, texture_call_start) + "(" + texture_debug_prefix + code.substr(texture_call_start) + ")";
					}
					if (is_screen_texture && !texture_func_returns_data && actions.apply_luminance_multiplier) {
						if (RS::get_singleton()->is_low_end()) {
							code = "(" + code + " / vec4(vec3(scene_data_block.data.luminance_multiplier), 1.0))";
						} else {
							code = "(" + code + " * vec4(vec3(sc_luminance_multiplier()), 1.0))";
						}
					}
					if (is_normal_roughness_texture && !texture_func_returns_data) {
						code = "normal_roughness_compatibility(" + code + ")";
					}
				} break;
				case SL::OP_INDEX: {
					code += _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += "[";
					code += _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += "]";

				} break;
				case SL::OP_SELECT_IF: {
					code += "(";
					code += _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += "?";
					code += _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += ":";
					code += _dump_node_code(onode->arguments[2], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					code += ")";

				} break;
				case SL::OP_EMPTY: {
					// Semicolon (or empty statement) - ignored.
				} break;

				default: {
					if (p_use_scope) {
						code += "(";
					}
					code += _dump_node_code(onode->arguments[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + " " + _opstr(onode->op) + " " + _dump_node_code(onode->arguments[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
					if (p_use_scope) {
						code += ")";
					}
					break;
				}
			}

		} break;
		case SL::Node::NODE_TYPE_CONTROL_FLOW: {
			SL::ControlFlowNode *cfnode = (SL::ControlFlowNode *)p_node;
			if (cfnode->flow_op == SL::FLOW_OP_IF) {
				code += _mktab(p_level) + "if (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ")\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				if (cfnode->blocks.size() == 2) {
					code += _mktab(p_level) + "else\n";
					code += _dump_node_code(cfnode->blocks[1], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				}
			} else if (cfnode->flow_op == SL::FLOW_OP_SWITCH) {
				code += _mktab(p_level) + "switch (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ")\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_CASE) {
				code += _mktab(p_level) + "case " + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ":\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_DEFAULT) {
				code += _mktab(p_level) + "default:\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_DO) {
				code += _mktab(p_level) + "do";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += _mktab(p_level) + "while (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ");";
			} else if (cfnode->flow_op == SL::FLOW_OP_WHILE) {
				code += _mktab(p_level) + "while (" + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ")\n";
				code += _dump_node_code(cfnode->blocks[0], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);
			} else if (cfnode->flow_op == SL::FLOW_OP_FOR) {
				String left = _dump_node_code(cfnode->blocks[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				String middle = _dump_node_code(cfnode->blocks[1], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				String right = _dump_node_code(cfnode->blocks[2], p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += _mktab(p_level) + "for (" + left + ";" + middle + ";" + right + ")\n";
				code += _dump_node_code(cfnode->blocks[3], p_level + 1, r_gen_code, p_actions, p_default_actions, p_assigning);

			} else if (cfnode->flow_op == SL::FLOW_OP_RETURN) {
				if (cfnode->expressions.size()) {
					code = "return " + _dump_node_code(cfnode->expressions[0], p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + ";";
				} else {
					code = "return;";
				}
			} else if (cfnode->flow_op == SL::FLOW_OP_DISCARD) {
				if (p_default_actions.usage_defines.has("DISCARD") && !used_name_defines.has("DISCARD")) {
					String define = p_default_actions.usage_defines["DISCARD"];
					if (define.begins_with("@")) {
						define = p_default_actions.usage_defines[define.substr(1)];
					}
					r_gen_code.defines.push_back(define);
					used_name_defines.insert("DISCARD");
				}

				if (p_actions.usage_flag_pointers.has("DISCARD") && !used_flag_pointers.has("DISCARD")) {
					*p_actions.usage_flag_pointers["DISCARD"] = true;
					used_flag_pointers.insert("DISCARD");
				}

				code = p_actions.discard_replacement.is_empty() ? "discard;" : p_actions.discard_replacement;
			} else if (cfnode->flow_op == SL::FLOW_OP_CONTINUE) {
				code = "continue;";
			} else if (cfnode->flow_op == SL::FLOW_OP_BREAK) {
				code = "break;";
			}

		} break;
		case SL::Node::NODE_TYPE_MEMBER: {
			SL::MemberNode *mnode = (SL::MemberNode *)p_node;
			String name;
			if (mnode->basetype == SL::TYPE_STRUCT) {
				name = _mkid(mnode->name);
			} else {
				name = mnode->name;
			}
			code = _dump_node_code(mnode->owner, p_level, r_gen_code, p_actions, p_default_actions, p_assigning) + "." + name;
			if (mnode->index_expression != nullptr) {
				code += "[";
				code += _dump_node_code(mnode->index_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning);
				code += "]";
			} else if (mnode->assign_expression != nullptr) {
				code += "=";
				code += _dump_node_code(mnode->assign_expression, p_level, r_gen_code, p_actions, p_default_actions, true, false);
			} else if (mnode->call_expression != nullptr) {
				code += ".";
				code += _dump_node_code(mnode->call_expression, p_level, r_gen_code, p_actions, p_default_actions, p_assigning, false);
			}
		} break;
	}

	return code;
}

ShaderLanguage::DataType ShaderCompiler::_get_global_shader_uniform_type(const StringName &p_name) {
	RSE::GlobalShaderParameterType gvt = RSG::material_storage->global_shader_parameter_get_type(p_name);
	return (ShaderLanguage::DataType)RS::global_shader_uniform_type_get_shader_datatype(gvt);
}

Error ShaderCompiler::compile(RSE::ShaderMode p_mode, const String &p_code, IdentifierActions *p_actions, const String &p_path, GeneratedCode &r_gen_code) {
	SL::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(p_mode);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(p_mode);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(p_mode);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	info.global_shader_uniform_type_func = _get_global_shader_uniform_type;
	info.base_varying_index = actions.base_varying_index;

	Error err = parser.compile(p_code, info);

	if (err != OK) {
		Vector<ShaderLanguage::FilePosition> include_positions = parser.get_include_positions();

		String current;
		HashMap<String, Vector<String>> includes;
		includes[""] = Vector<String>();
		Vector<String> include_stack;
		Vector<String> shader_lines = p_code.split("\n");

		// Reconstruct the files.
		for (int i = 0; i < shader_lines.size(); i++) {
			String l = shader_lines[i];
			if (l.begins_with("@@>")) {
				String inc_path = l.replace_first("@@>", "");

				l = "#include \"" + inc_path + "\"";
				includes[current].append("#include \"" + inc_path + "\""); // Restore the include directive
				include_stack.push_back(current);
				current = inc_path;
				includes[inc_path] = Vector<String>();

			} else if (l.begins_with("@@<")) {
				if (include_stack.size()) {
					current = include_stack[include_stack.size() - 1];
					include_stack.resize(include_stack.size() - 1);
				}
			} else {
				includes[current].push_back(l);
			}
		}

		// Print the files.
		for (const KeyValue<String, Vector<String>> &E : includes) {
			int err_line = -1;
			for (const ShaderLanguage::FilePosition &include_position : include_positions) {
				if (include_position.file == E.key) {
					err_line = include_position.line;
				}
			}
			if (err_line < 0) {
				// Skip files that don't contain errors.
				continue;
			}

			if (E.key.is_empty()) {
				if (p_path == "") {
					print_line("--Main Shader--");
				} else {
					print_line("--" + p_path + "--");
				}
			} else {
				print_line("--" + E.key + "--");
			}
			const Vector<String> &V = E.value;
			for (int i = 0; i < V.size(); i++) {
				if (i == err_line - 1) {
					// Mark the error line to be visible without having to look at
					// the trace at the end.
					print_line(vformat("E%4d-> %s", i + 1, V[i]));
				} else if ((i == err_line - 3) || (i == err_line - 2) || (i == err_line) || (i == err_line + 1)) {
					// Print 4 lines around the error line.
					print_line(vformat("%5d | %s", i + 1, V[i]));
				}
			}
		}

		String file;
		int line;
		if (include_positions.size() > 1) {
			file = include_positions[include_positions.size() - 1].file;
			line = include_positions[include_positions.size() - 1].line;
		} else {
			file = p_path;
			line = parser.get_error_line();
		}

		_err_print_error(nullptr, file.utf8().get_data(), line, parser.get_error_text().utf8().get_data(), false, ERR_HANDLER_SHADER);
		return err;
	}

	r_gen_code.defines.clear();
	r_gen_code.code.clear();
	for (int i = 0; i < STAGE_MAX; i++) {
		r_gen_code.stage_globals[i] = String();
	}
	r_gen_code.uses_fragment_time = false;
	r_gen_code.uses_vertex_time = false;
	r_gen_code.uses_global_textures = false;
	r_gen_code.uses_screen_texture_mipmaps = false;
	r_gen_code.uses_screen_texture = false;
	r_gen_code.uses_depth_texture = false;
	r_gen_code.uses_normal_roughness_texture = false;

	used_name_defines.clear();
	used_rmode_defines.clear();
	used_smode_defines.clear();
	used_flag_pointers.clear();
	fragment_varyings.clear();

	shader = parser.get_shader();
	function = nullptr;

	dataflow_defs.clear();
	texture_coord_dependents.clear();
	texture_footprints.clear();
	if (!p_actions->texture_gradients.is_empty()) {
		LocalVector<ShaderDataflowEdge> edges;
		for (const SL::ShaderNode::Function &func : shader->vfunctions) {
			_collect_dataflow(shader, func.function->body, func.name, edges);
		}
		for (const ShaderDataflowEdge &edge : edges) {
			if (edge.target != StringName()) {
				dataflow_defs[edge.target].push_back(edge.source);
			}
		}
		for (const IdentifierActions::TextureGradients &source : p_actions->texture_gradients) {
			texture_coord_dependents.push_back(_dataflow_dependents(edges, source.coord));
		}
	}

	// Return value only relevant within nested calls.
	_ALLOW_DISCARD_ _dump_node_code(shader, 1, r_gen_code, *p_actions, actions, false);

	if (!p_actions->texture_footprint_code.is_empty()) {
		const String footprint_code = _texture_footprint_code(*p_actions);
		if (!footprint_code.is_empty()) {
			r_gen_code.code[p_actions->texture_footprint_code] = footprint_code;
		}
	}

	return OK;
}

void ShaderCompiler::initialize(DefaultIdentifierActions p_actions) {
	actions = p_actions;

	time_name = "TIME";

	List<String> func_list;

	ShaderLanguage::get_builtin_funcs(&func_list);

	for (const String &E : func_list) {
		internal_functions.insert(E);
	}
	texture_functions.insert("texture");
	texture_functions.insert("textureProj");
	texture_functions.insert("textureLod");
	texture_functions.insert("textureProjLod");
	texture_functions.insert("textureGrad");
	texture_functions.insert("textureProjGrad");
	texture_functions.insert("textureGather");
	texture_functions.insert("textureSize");
	texture_functions.insert("textureQueryLod");
	texture_functions.insert("textureQueryLevels");
	texture_functions.insert("texelFetch");
}

ShaderCompiler::ShaderCompiler() {
}
