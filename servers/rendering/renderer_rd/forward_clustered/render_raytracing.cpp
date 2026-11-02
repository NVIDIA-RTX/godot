/**************************************************************************/
/*  render_raytracing.cpp                                                 */
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

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/templates/radix_sort.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_raytracing.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_billboard.h"
#include "servers/rendering/rendering_rt_material_state.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/environment_storage.h"

using namespace RendererSceneRenderImplementation;

// Device addresses written into the geometry and material tables must follow the RT validation rules in
// render_raytracing_validation.h. Strict validation reports what was missed.

// Grouping variants kept per mesh. Variants appear when material state changes which surfaces merge.
static constexpr uint32_t RT_MESH_GROUP_MAX_VARIANTS = 4;

RTBlasTransformPool::Slot RTBlasTransformPool::allocate(const float *p_matrix) {
	RD *rd = RD::get_singleton();
	if (free_slots.is_empty()) {
		RID page = rd->storage_buffer_create(SLOTS_PER_PAGE * MATRIX_SIZE, Vector<uint8_t>(), 0,
				RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		ERR_FAIL_COND_V(!page.is_valid(), Slot());
		rd->set_resource_name(page, "RT BLAS transform page " + itos(pages.size()));
		pages.push_back(page);
		for (uint32_t i = SLOTS_PER_PAGE; i > 0; i--) {
			free_slots.push_back({ page, (i - 1) * MATRIX_SIZE });
		}
	}
	Slot slot = free_slots[free_slots.size() - 1];
	free_slots.resize(free_slots.size() - 1);
	rd->buffer_update(slot.buffer, slot.offset, MATRIX_SIZE, p_matrix);
	return slot;
}

void RTBlasTransformPool::release(Slot &r_slot) {
	if (r_slot.is_valid()) {
		free_slots.push_back(r_slot);
		r_slot = Slot();
	}
}

void RTBlasTransformPool::clear() {
	RD *rd = RD::get_singleton();
	for (const RID &page : pages) {
		rd->free_rid(page);
	}
	pages.clear();
	free_slots.clear();
}

// RD frees a BLAS together with any mesh buffer it was built from, so the BLAS may already be gone.
static void _free_blas_resources(RTBlasTransformPool &r_pool, RID &r_blas, RTBlasTransformPool::Slot *r_transforms, uint32_t p_count) {
	RD *rd = RD::get_singleton();
	if (rd->acceleration_structure_is_valid(r_blas)) {
		rd->free_rid(r_blas);
	}
	r_blas = RID();
	for (uint32_t i = 0; i < p_count; i++) {
		r_pool.release(r_transforms[i]);
	}
}

static void _free_rid_if_valid(RID &r_rid) {
	if (r_rid.is_valid()) {
		RD::get_singleton()->free_rid(r_rid);
		r_rid = RID();
	}
}

// Device address of p_buffer for an RT_GeometryData field. r_source records the buffer for strict validation.
static uint64_t _rt_buffer_address(RID p_buffer, RID &r_source) {
	r_source = p_buffer;
	return RD::get_singleton()->buffer_get_device_address(p_buffer);
}

// As above, for an address of p_buffer cached when it was allocated; the cache must be refreshed with the buffer.
static uint64_t _rt_buffer_address(RID p_buffer, uint64_t p_cached_address, RID &r_source) {
	r_source = p_buffer;
	return p_cached_address;
}

// The results must be handled: a BLAS that failed to build stays invalidated, and so does a TLAS built from it.
[[nodiscard]] static Error _rt_blas_build(RID p_blas) {
	return RD::get_singleton()->blas_build(p_blas);
}

[[nodiscard]] static Error _rt_tlas_build(RID p_tlas, Span<RD::AccelerationStructureInstance> p_instances) {
	return RD::get_singleton()->tlas_build(p_tlas, p_instances);
}

// The entry owns every buffer its BLAS reads, so the BLAS is freed here exactly once, before its inputs.
static void _release_deformed_entry(RTDeformedCacheEntry &r_entry) {
	if (r_entry.ptr) {
		_free_rid_if_valid(r_entry.ptr->blas);
		memdelete(r_entry.ptr);
		r_entry.ptr = nullptr;
	}
	_free_rid_if_valid(r_entry.owned_ib);
	_free_rid_if_valid(r_entry.owned_vb_full);
	_free_rid_if_valid(r_entry.prev_pos_vb);
}

static void _reset_deformed_entry(RTDeformedCacheEntry &r_entry) {
	_release_deformed_entry(r_entry);
	r_entry = RTDeformedCacheEntry();
}

// prepare_frame evicts expired entries; entries never used (last_used_frame 0) don't expire.
static bool _rt_deformed_entry_expired(const RTDeformedCacheEntry &p_entry, uint32_t p_frame) {
	static const uint32_t DEFORMED_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracing/deformed_mesh_cache_ttl_frames");
	return p_entry.last_used_frame != 0 && p_frame - p_entry.last_used_frame > DEFORMED_CACHE_TTL;
}

static void _release_merged_mm_entry(RTMergedMMEntry &r_entry) {
	for (RID *resource : { &r_entry.blas, &r_entry.merged_vtx_buffer, &r_entry.merged_attr_buffer, &r_entry.replicated_idx_buffer, &r_entry.prev_pos_buffer }) {
		_free_rid_if_valid(*resource);
	}
}

static void _reset_merged_mm_entry(RTMergedMMEntry &r_entry) {
	_release_merged_mm_entry(r_entry);
	r_entry = RTMergedMMEntry();
}

// Grows the entry's previous-position buffer to hold p_bytes; false when it can't be allocated.
static bool _rt_ensure_prev_pos_buffer(RTMergedMMEntry &r_entry, uint32_t p_bytes) {
	if (r_entry.prev_pos_buffer.is_valid() && r_entry.prev_pos_capacity_bytes >= p_bytes) {
		return true;
	}
	_free_rid_if_valid(r_entry.prev_pos_buffer);
	r_entry.prev_pos_capacity_bytes = p_bytes;
	r_entry.prev_pos_buffer = RD::get_singleton()->storage_buffer_create(p_bytes, {}, 0, RD::BUFFER_CREATION_AS_STORAGE_BIT | RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	ERR_FAIL_COND_V(!r_entry.prev_pos_buffer.is_valid(), false);
	r_entry.prev_pos_address = RD::get_singleton()->buffer_get_device_address(r_entry.prev_pos_buffer);
	RD::get_singleton()->set_resource_name(r_entry.prev_pos_buffer, "RT MM merged prev pos");
	return true;
}

// prepare_frame evicts expired entries; entries never used (last_used_frame 0) don't expire.
static bool _rt_merged_mm_entry_expired(const RTMergedMMEntry &p_entry, uint32_t p_frame) {
	static const uint32_t MM_BLAS_CACHE_TTL = (uint32_t)GLOBAL_GET("rendering/pathtracing/multimesh_blas_cache_ttl_frames");
	return p_entry.last_used_frame != 0 && p_frame - p_entry.last_used_frame > MM_BLAS_CACHE_TTL;
}

// Refreshes the entry's private index copy; call only after its BLAS was freed.
static void _copy_deformed_indices(void *p_mesh_surface, uint32_t p_vertex_count, uint64_t p_cache_key, RTDeformedCacheEntry &r_entry) {
	RD *rd = RD::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const RID source = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface);
	const uint32_t index_count = source.is_valid() ? mesh_storage->mesh_surface_get_index_count(p_mesh_surface) : 0;
	const bool is_16bit = p_vertex_count <= 65536;
	const RD::IndexBufferFormat format = is_16bit ? RD::INDEX_BUFFER_FORMAT_UINT16 : RD::INDEX_BUFFER_FORMAT_UINT32;

	if (index_count != r_entry.owned_ib_index_count || format != r_entry.owned_ib_format) {
		_free_rid_if_valid(r_entry.owned_ib);
		r_entry.owned_ib_index_count = 0;
		if (index_count == 0) {
			return;
		}
		r_entry.owned_ib = rd->index_buffer_create(index_count, format, {}, false,
				RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		ERR_FAIL_COND(!r_entry.owned_ib.is_valid());
		rd->set_resource_name(r_entry.owned_ib, "RT deformed owned IB [" + itos(p_cache_key) + "]");
		r_entry.owned_ib_index_count = index_count;
		r_entry.owned_ib_format = format;
	}
	if (r_entry.owned_ib.is_valid()) {
		rd->buffer_copy(source, r_entry.owned_ib, 0, 0, index_count * (is_16bit ? 2u : 4u));
	}
}

// Points each compressed geometry at a row-major 3x4 matrix mapping its positions from unit-AABB space to mesh space.
static void _assign_compression_transforms(RTBlasTransformPool &r_pool, const RT_GeometryData *p_geometries,
		RD::AccelerationStructureGeometry *r_as_geometries, RTBlasTransformPool::Slot *r_transforms, uint32_t p_count) {
	for (uint32_t i = 0; i < p_count; i++) {
		const RT_GeometryData &g = p_geometries[i];
		if ((g.flags & RT_GEOM_FLAG_COMPRESSED) == 0) {
			continue;
		}
		const float matrix[12] = {
			g.aabb_size_x, 0.0f, 0.0f, g.aabb_pos_x,
			0.0f, g.aabb_size_y, 0.0f, g.aabb_pos_y,
			0.0f, 0.0f, g.aabb_size_z, g.aabb_pos_z
		};
		r_transforms[i] = r_pool.allocate(matrix);
		r_as_geometries[i].geometry.triangles.transform_buffer = r_transforms[i].buffer;
		r_as_geometries[i].geometry.triangles.transform_offset = r_transforms[i].offset;
	}
}

static RenderingBillboard::Camera _rt_billboard_camera(const Transform3D &p_transform, const Projection &p_projection) {
	RenderingBillboard::Camera camera;
	camera.transform = p_transform;
	camera.orthogonal = p_projection.is_orthogonal();
	camera.projection_y_scale = p_projection.columns[1][1];
	return camera;
}

static void _rt_store_basis_columns(const Basis &p_basis, float (&r_columns)[3][4]) {
	for (int i = 0; i < 3; i++) {
		const Vector3 column = p_basis.get_column(i);
		r_columns[i][0] = column.x;
		r_columns[i][1] = column.y;
		r_columns[i][2] = column.z;
		r_columns[i][3] = 0.0f;
	}
}

// Billboard inputs of multimesh_merge.glsl for a MultiMesh node at p_node seen from p_camera.
// Mirrors RenderingBillboard::compute_world_transform, mapped into node space.
static RTMergeBillboardParams _rt_merge_billboard_params(const RenderingBillboard::Settings &p_settings, uint32_t p_custom_offset, const Transform3D &p_node, const RenderingBillboard::Camera &p_camera) {
	RTMergeBillboardParams params;
	params.mode = p_settings.mode;
	params.keep_scale = p_settings.keep_scale;
	params.fixed_size_enabled = p_settings.fixed_size;
	params.custom_offset = p_settings.mode == RenderingBillboard::MODE_PARTICLES ? p_custom_offset : UINT32_MAX;
	_rt_store_basis_columns(p_node.basis.inverse() * RenderingBillboard::facing_basis(p_settings.mode, p_camera.transform.basis), params.facing);
	_rt_store_basis_columns(p_node.basis, params.node_basis);
	if (p_settings.fixed_size) {
		const RenderingBillboard::FixedSizeScale scale = RenderingBillboard::fixed_size_scale(p_camera);
		const Vector3 gradient = p_node.basis.xform_inv(scale.gradient);
		params.fixed_size[0] = gradient.x;
		params.fixed_size[1] = gradient.y;
		params.fixed_size[2] = gradient.z;
		params.fixed_size[3] = scale.at(p_node.origin);
	}
	return params;
}

// Transform of the MultiMesh instance record at p_record (row-major 3x4).
static Transform3D _rt_mm_record_transform(const float *p_record) {
	Transform3D xform;
	for (int row = 0; row < 3; row++) {
		xform.basis.rows[row][0] = p_record[row * 4 + 0];
		xform.basis.rows[row][1] = p_record[row * 4 + 1];
		xform.basis.rows[row][2] = p_record[row * 4 + 2];
		xform.origin[row] = p_record[row * 4 + 3];
	}
	return xform;
}

static bool _rt_multimesh_source(RID p_multimesh, RTInstanceSource &r_source) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	if (mesh_storage->multimesh_get_transform_format(p_multimesh) != RSE::MULTIMESH_TRANSFORM_3D) {
		return false;
	}
	r_source.base = p_multimesh;
	r_source.count = mesh_storage->multimesh_get_instances_to_draw(p_multimesh);
	r_source.gpu_buffer = mesh_storage->multimesh_get_gpu_buffer(p_multimesh);
	r_source.stride = mesh_storage->multimesh_get_stride(p_multimesh);
	mesh_storage->_multimesh_get_motion_vectors_offsets(p_multimesh, r_source.current_offset, r_source.prev_offset);
	r_source.color_offset = mesh_storage->multimesh_uses_colors(p_multimesh) ? mesh_storage->multimesh_get_color_offset(p_multimesh) : UINT32_MAX;
	r_source.custom_offset = mesh_storage->multimesh_uses_custom_data(p_multimesh) ? mesh_storage->multimesh_get_custom_data_offset(p_multimesh) : UINT32_MAX;
	r_source.last_change = mesh_storage->multimesh_get_last_change(p_multimesh);
	return r_source.count > 0;
}

static bool _rt_particles_source(RID p_particles, RTInstanceSource &r_source) {
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	if (particles_storage->particles_get_mode(p_particles) != RSE::PARTICLES_MODE_3D) {
		return false;
	}
	r_source.count = particles_storage->particles_get_amount(p_particles, r_source.trail_steps);
	r_source.base = p_particles;
	r_source.gpu_buffer = particles_storage->particles_get_instance_buffer(p_particles);
	r_source.stride = 20; // 3x4 transform, color, custom.
	r_source.color_offset = 12;
	r_source.custom_offset = 16;
	particles_storage->particles_get_instance_buffer_motion_vectors_offsets(p_particles, r_source.current_offset, r_source.prev_offset);
	r_source.last_change = RSG::rasterizer->get_frame_number(); // Simulated every frame the system is visible to rays.
	r_source.particles = true;
	r_source.world_space = !particles_storage->particles_is_using_local_coords(p_particles);
	if (r_source.world_space) {
		r_source.dead_position = particles_storage->particles_get_emission_transform(p_particles).origin;
	}
	return r_source.count > 0 && r_source.gpu_buffer.is_valid();
}

// Instance records of a traced MultiMesh or particle system; false when the path tracer can't trace it.
static bool _rt_instance_source(RSE::InstanceType p_type, RID p_base, RTInstanceSource &r_source) {
	switch (p_type) {
		case RSE::INSTANCE_MULTIMESH:
			return _rt_multimesh_source(p_base, r_source);
		case RSE::INSTANCE_PARTICLES:
			return _rt_particles_source(p_base, r_source);
		default:
			return false;
	}
}

// p_source as drawn by a shader that does (p_uses_trails) or doesn't use particle_trails: see RTVertexCapture::drawn_instances.
static RTInstanceSource _rt_drawn_instances(const RTInstanceSource &p_source, bool p_uses_trails) {
	const RTVertexCapture::Instances drawn = RTVertexCapture::drawn_instances(p_source.count, p_source.stride, p_source.trail_steps, p_uses_trails);
	RTInstanceSource source = p_source;
	source.count = drawn.count;
	source.stride = drawn.stride;
	source.current_offset /= drawn.records_per_instance;
	source.prev_offset /= drawn.records_per_instance;
	source.trail_steps = 1;
	if (source.stride > 0xffu && (source.color_offset != UINT32_MAX || source.custom_offset != UINT32_MAX)) {
		WARN_PRINT_ONCE("Path tracing ignores the instance COLOR and INSTANCE_CUSTOM of particle trails with more than 12 trail bind poses.");
		source.color_offset = UINT32_MAX;
		source.custom_offset = UINT32_MAX;
	}
	return source;
}

// Triangles of a merged (source, surface) BLAS, or 0 when the merge bake can't handle it. Captured surfaces don't read
// mesh positions nor instance records, and have no other way to be traced, so neither limits them.
static uint64_t _rt_merged_triangle_count(const RTInstanceSource &p_source, void *p_mesh_surface, bool p_captured) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	if (!p_captured && p_source.stride > 0xffu) {
		return 0;
	}
	// Compressed positions are UNORM16x4, not float3.
	if (!p_captured && (mesh_storage->mesh_surface_get_format(p_mesh_surface) & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES)) {
		return 0;
	}
	const uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	const uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);
	const bool indexed = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0).is_valid() && index_count > 0;
	if (vertex_count == 0) {
		return 0;
	}
	const uint64_t triangles = uint64_t(p_source.count) * (indexed ? index_count / 3 : vertex_count / 3);
	// MultiMeshes above the cap use the expanded TLAS fallback; particles and captured surfaces have none.
	static const uint32_t MM_MERGED_BLAS_MAX_TRIANGLES = (uint32_t)GLOBAL_GET("rendering/pathtracing/multimesh_merged_blas_max_triangles");
	if (!p_source.particles && !p_captured && triangles > MM_MERGED_BLAS_MAX_TRIANGLES) {
		return 0;
	}
	return triangles;
}

// Scheduler generation of a pool entry: the RID validator's low byte, never 0.
static uint8_t _rt_entry_generation(RID p_handle) {
	const uint8_t gen = uint8_t(p_handle.get_id() >> 32);
	return gen != 0 ? gen : 1;
}

// Pools whose entries are scheduled; each gets its own scheduler slot range.
enum RTSchedulerPool : uint32_t {
	RT_SCHEDULER_POOL_MERGED_MM,
	RT_SCHEDULER_POOL_CAPTURE,
	RT_SCHEDULER_POOL_CAPTURED_MM,
	RT_SCHEDULER_POOL_COUNT,
};

static uint32_t _rt_scheduler_slot(RID p_handle, RTSchedulerPool p_pool) {
	return p_handle.get_local_index() * RT_SCHEDULER_POOL_COUNT + p_pool;
}

// View frustum for scheduler visibility; contains everything when the view has no scene data.
struct RTViewFrustum {
	Vector<Plane> planes;
	Vector3 points[8];

	explicit RTViewFrustum(const RenderDataRD *p_render_data) {
		if (p_render_data->scene_data) {
			const RenderSceneDataRD *scene_data = p_render_data->scene_data;
			planes = scene_data->cam_projection.get_projection_planes(scene_data->cam_transform);
			scene_data->cam_projection.get_endpoints(scene_data->cam_transform, points);
		}
	}

	bool intersects(const AABB &p_aabb) const {
		return planes.is_empty() || p_aabb.intersects_convex_shape(planes.ptr(), planes.size(), points, 8);
	}
};

_FORCE_INLINE_ static uint32_t _rt_indices_to_primitives(RSE::PrimitiveType p_primitive, uint32_t p_indices) {
	static const uint32_t divisor[RSE::PRIMITIVE_MAX] = { 1, 2, 1, 3, 1 };
	static const uint32_t subtractor[RSE::PRIMITIVE_MAX] = { 0, 0, 1, 0, 2 };
	return (p_indices - subtractor[p_primitive]) / divisor[p_primitive];
}

static void _rt_update_or_grow_storage(RID &r_buffer, uint32_t &r_capacity, const void *p_data, uint32_t p_size, const char *p_name) {
	if (p_size == 0) {
		return;
	}
	if (p_size > r_capacity) {
		if (r_buffer.is_valid()) {
			RD::get_singleton()->free_rid(r_buffer);
		}
		r_capacity = p_size;
		Vector<uint8_t> init;
		init.resize(p_size);
		memcpy(init.ptrw(), p_data, p_size);
		r_buffer = RD::get_singleton()->storage_buffer_create(p_size, init);
		RD::get_singleton()->set_resource_name(r_buffer, p_name);
	} else {
		RD::get_singleton()->buffer_update(r_buffer, 0, p_size, p_data);
	}
}

static void _rt_set_entry_material(RT_GeometryData &r_geometry, const RTMaterialData *p_mat_data, uint32_t p_material_flags, float p_instance_alpha) {
	r_geometry.material_index = rt_pack_material_index(p_mat_data->table_index, rt_instance_transparency(p_instance_alpha));
	r_geometry.material_flags = p_material_flags;
}

static uint32_t _rt_tlas_flags(const RTSurfaceGroupKey &p_key, const RTTriangleFacing &p_facing) {
	return p_facing.instance_flags | (p_key.opaque ? uint32_t(RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT) : 0u);
}

static RID _create_blas(Span<RD::AccelerationStructureGeometry> p_geometries, bool p_prefer_fast_build, bool p_allow_update, const String &p_name, LocalVector<RID> &r_dirty_blas_list) {
	BitField<RD::AccelerationStructureFlagBits> as_flags = p_prefer_fast_build
			? RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT
			: RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT;
	if (p_allow_update) {
		as_flags.set_flag(RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
	}
	RID blas = RD::get_singleton()->blas_create(p_geometries, as_flags);
	if (!blas.is_valid()) {
		return RID();
	}
	RD::get_singleton()->set_resource_name(blas, p_name);
	r_dirty_blas_list.push_back(blas);
	return blas;
}

void RendererSceneRenderImplementation::rt_assign_surface_groups(Span<RTSurfaceGroupKey> p_keys, LocalVector<uint32_t> &r_leaders) {
	r_leaders.resize(p_keys.size());
	for (uint32_t i = 0; i < p_keys.size(); i++) {
		r_leaders[i] = i;
		for (uint32_t j = 0; j < i; j++) {
			if (p_keys[j] == p_keys[i]) {
				r_leaders[i] = j;
				break;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RenderRaytracing::initialize(RenderForwardClustered *p_owner) {
	owner = p_owner;
	// Initialized eagerly: vertex captures resolve materials before build_tlas() runs prepare_frame().
	bindless_block = memnew(BindlessBlock);
	bindless_block->initialize(RD::get_singleton());
#ifdef RT_VALIDATION_ENABLED
	GLOBAL_DEF(PropertyInfo(Variant::BOOL, "rendering/pathtracing/debug/strict_validation"), false);
#endif

	// Initialize merged MultiMesh BLAS compute shader.
	Vector<String> merge_modes;
	merge_modes.push_back("\n");
	merge_modes.push_back("\n#define MODE_INDEXED\n");
	mm_merge_shader.shader.initialize(merge_modes);
	mm_merge_shader.version = mm_merge_shader.shader.version_create();
	for (int i = 0; i < MergeShader::MODE_MAX; i++) {
		mm_merge_shader.version_shader[i] = mm_merge_shader.shader.version_get_shader(mm_merge_shader.version, i);
		mm_merge_shader.pipeline[i] = RD::get_singleton()->compute_pipeline_create(mm_merge_shader.version_shader[i]);
		RD::get_singleton()->set_resource_name(mm_merge_shader.pipeline[i], "RT MM merge pipeline [mode=" + itos(i) + "]");
	}
}

RenderRaytracing::~RenderRaytracing() {
	for (KeyValue<RTViewportStateKey, RTViewportState *> &kv : viewport_states) {
		_free_viewport_state_internal(kv.value);
	}
	viewport_states.clear();

	cleanup_caches();

	if (mat_ubo_pool_buffer.is_valid()) {
		_free_rid_if_valid(mat_ubo_pool_buffer);
	}

	if (bindless_block) {
		memdelete(bindless_block);
		bindless_block = nullptr;
	}
	if (shader) {
		memdelete(shader);
		shader = nullptr;
	}

	mm_merge_shader.shader.version_free(mm_merge_shader.version);
}

// ---------------------------------------------------------------------------
// Per-viewport state lifecycle
// ---------------------------------------------------------------------------

static RTViewportStateKey _viewport_state_key(const RenderDataRD *p_render_data) {
	return RTViewportStateKey(
			p_render_data->render_buffers.ptr(), p_render_data->viewport, p_render_data->environment);
}

static Ref<RendererRD::Fog::VolumetricFog> _rt_get_volumetric_fog(const RenderDataRD *p_render_data) {
	Ref<RendererRD::Fog::VolumetricFog> fog = RendererRD::Fog::get_active(p_render_data->render_buffers.ptr(), p_render_data->environment);
	return fog.is_valid() && fog->fog_map.is_valid() ? fog : Ref<RendererRD::Fog::VolumetricFog>();
}

RTViewportState *RenderRaytracing::_get_or_create_viewport_state(const RenderDataRD *p_render_data) {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RTViewportStateKey key = _viewport_state_key(p_render_data);
	HashMap<RTViewportStateKey, RTViewportState *>::Iterator it = viewport_states.find(key);
	if (it != viewport_states.end()) {
		return it->value;
	}
	RTViewportState *state = memnew(RTViewportState);
	state->render_buffers = p_render_data->render_buffers.ptr();
	viewport_states.insert(key, state);
	return state;
}

RTViewportState *RenderRaytracing::_get_viewport_state(const RenderDataRD *p_render_data) const {
	if (!p_render_data || p_render_data->render_buffers.is_null()) {
		return nullptr;
	}
	RTViewportStateKey key = _viewport_state_key(p_render_data);
	HashMap<RTViewportStateKey, RTViewportState *>::ConstIterator it = viewport_states.find(key);
	return (it != viewport_states.end()) ? it->value : nullptr;
}

void RenderRaytracing::_free_viewport_state_internal(RTViewportState *p_state) {
	if (!p_state) {
		return;
	}
	if (p_state->tlas.is_valid()) {
		RD::get_singleton()->free_rid(p_state->tlas);
	}
	if (p_state->tlas_transparent.is_valid()) {
		RD::get_singleton()->free_rid(p_state->tlas_transparent);
	}
	if (p_state->geometry_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->geometry_buffer);
	}
	if (p_state->motion_index_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_index_buffer);
	}
	if (p_state->motion_transform_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->motion_transform_buffer);
	}
	for (const KeyValue<RTMMSurfaceKey, RID> &kv : p_state->billboard_mm_entries) {
		_free_merged_mm_entry(kv.value);
	}
	_free_capture_entry_map(p_state->capture_entries, false);
	_free_capture_entry_map(p_state->captured_mm_entries, true);
	if (p_state->merge_billboard_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->merge_billboard_buffer);
	}
	if (p_state->light_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->light_buffer);
	}
	if (p_state->params_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->params_buffer);
	}
	if (p_state->scene_ubo.is_valid()) {
		RD::get_singleton()->free_rid(p_state->scene_ubo);
	}
	if (p_state->scene_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_state->scene_uniform_set)) {
		RD::get_singleton()->free_rid(p_state->scene_uniform_set);
	}
	if (p_state->fog_shadows_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_state->fog_shadows_uniform_set)) {
		RD::get_singleton()->free_rid(p_state->fog_shadows_uniform_set);
	}
	if (p_state->fog_shadows_ubo.is_valid()) {
		RD::get_singleton()->free_rid(p_state->fog_shadows_ubo);
	}
	if (p_state->decal_tlas.is_valid()) {
		RD::get_singleton()->free_rid(p_state->decal_tlas);
	}
	if (p_state->decal_buffer.is_valid()) {
		RD::get_singleton()->free_rid(p_state->decal_buffer);
	}
	memdelete(p_state);
}

void RenderRaytracing::free_viewport_state(RenderSceneBuffersRD *p_render_buffers) {
	LocalVector<RTViewportStateKey> to_remove;
	for (KeyValue<RTViewportStateKey, RTViewportState *> &kv : viewport_states) {
		if (kv.value->render_buffers == p_render_buffers) {
			_free_viewport_state_internal(kv.value);
			to_remove.push_back(kv.key);
		}
	}
	for (const RTViewportStateKey &key : to_remove) {
		viewport_states.erase(key);
	}
}

// ---------------------------------------------------------------------------
// Raytracing / DLSS-RR output textures (stored on the render buffers via named
// scopes) and the per-viewport DLSS upscaler context (in RTViewportState).
// ---------------------------------------------------------------------------

void RenderRaytracing::rt_ensure_textures(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);

	uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	if (!p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RAYTRACING)) {
		p_render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RAYTRACING, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_DEPTH)) {
		p_render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_DEPTH, RD::DATA_FORMAT_R32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
}

bool RenderRaytracing::rt_has_texture(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RAYTRACING);
}

RID RenderRaytracing::rt_get_texture(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RAYTRACING);
}

bool RenderRaytracing::rt_has_depth_texture(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_DEPTH);
}

RID RenderRaytracing::rt_get_depth_texture(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_DEPTH);
}

void RenderRaytracing::rt_ensure_sky_texture(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);

	if (!p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_SKY)) {
		uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
		p_render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_SKY, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	}
}

bool RenderRaytracing::rt_has_sky_texture(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_SKY);
}

RID RenderRaytracing::rt_get_sky_texture(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_SKY);
}

RID RenderRaytracing::rt_get_sky_framebuffer(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	if (!p_render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_SKY)) {
		return RID();
	}
	RID texture = p_render_buffers->get_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_RT_SKY);
	return FramebufferCacheRD::get_singleton()->get_cache_multiview(p_render_buffers->get_view_count(), texture);
}

void RenderRaytracing::dlss_rr_ensure_buffers(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);

	if (p_render_buffers->has_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO)) {
		return;
	}

	uint32_t usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT |
			RD::TEXTURE_USAGE_SAMPLING_BIT |
			RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;

	// Diffuse Albedo: linear RGB surface color for non-metals (RGBA16F; DLSS-RR expects
	// linear albedo, and float precision avoids 8-bit dark-value banding without gamma packing).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	// Specular Albedo: RGB specular reflection color (RGBA16F; needs the accuracy, fixes banding artifacts).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_ALBEDO, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
	// Normal + Roughness: World space normals (RGB) + roughness (A).
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_NORMAL_ROUGHNESS, RD::DATA_FORMAT_R8G8B8A8_SNORM, usage_bits, RD::TEXTURE_SAMPLES_1);
	// Specular Motion Vectors: same RG16F UV-delta convention as the main velocity buffer.
	p_render_buffers->create_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_MOTION_VECTORS, RD::DATA_FORMAT_R16G16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);
}

void RenderRaytracing::dlss_rr_free_buffers(RenderSceneBuffersRD *p_render_buffers) {
	ERR_FAIL_NULL(p_render_buffers);
	p_render_buffers->clear_context(RB_SCOPE_DLSS_RR);
}

bool RenderRaytracing::dlss_rr_has_buffers(RenderSceneBuffersRD *p_render_buffers) const {
	return p_render_buffers && p_render_buffers->has_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO);
}

RID RenderRaytracing::dlss_rr_get_diffuse_albedo(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_DIFFUSE_ALBEDO);
}

RID RenderRaytracing::dlss_rr_get_specular_albedo(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_ALBEDO);
}

RID RenderRaytracing::dlss_rr_get_normal_roughness(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_NORMAL_ROUGHNESS);
}

RID RenderRaytracing::dlss_rr_get_specular_motion_vectors(RenderSceneBuffersRD *p_render_buffers) const {
	ERR_FAIL_NULL_V(p_render_buffers, RID());
	return p_render_buffers->get_texture(RB_SCOPE_DLSS_RR, RB_TEX_DLSS_RR_SPECULAR_MOTION_VECTORS);
}

// ---------------------------------------------------------------------------
// Material UBO sub-allocation pool
//
// Single device-address storage buffer of MAT_UBO_POOL_TOTAL_BYTES, divided
// into MAT_UBO_POOL_CAPACITY fixed-size slots. Allocate/release just bump a
// next-slot counter and a free-list. Per-material UBO writes are
// buffer_update at slot offset, no allocation. mat.uniform_address is
// pool_bda + slot * slot_size, so the closest-hit shader's
// CustomMaterialUniforms(addr) cast lands on the correct slot.
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t MAT_UBO_POOL_SLOT_SIZE = 512;
constexpr uint32_t MAT_UBO_POOL_CAPACITY = 100000;
constexpr uint64_t MAT_UBO_POOL_TOTAL_BYTES = uint64_t(MAT_UBO_POOL_SLOT_SIZE) * MAT_UBO_POOL_CAPACITY;
} // namespace

void RenderRaytracing::mat_ubo_pool_ensure_initialized() {
	if (mat_ubo_pool_buffer.is_valid()) {
		return;
	}
	Vector<uint8_t> init;
	init.resize(MAT_UBO_POOL_TOTAL_BYTES);
	memset(init.ptrw(), 0, MAT_UBO_POOL_TOTAL_BYTES);
	mat_ubo_pool_buffer = RD::get_singleton()->storage_buffer_create(
			MAT_UBO_POOL_TOTAL_BYTES, init, 0,
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
	if (mat_ubo_pool_buffer.is_valid()) {
		RD::get_singleton()->set_resource_name(mat_ubo_pool_buffer, "RT Material UBO Pool");
		mat_ubo_pool_bda = RD::get_singleton()->buffer_get_device_address(mat_ubo_pool_buffer);
	}
}

uint32_t RenderRaytracing::mat_ubo_pool_allocate() {
	mat_ubo_pool_ensure_initialized();
	if (!mat_ubo_pool_buffer.is_valid()) {
		return UINT32_MAX;
	}
	if (mat_ubo_pool_free_count > 0) {
		--mat_ubo_pool_free_count;
		return mat_ubo_pool_free_slots[mat_ubo_pool_free_count];
	}
	if (mat_ubo_pool_next_slot >= MAT_UBO_POOL_CAPACITY) {
		ERR_PRINT_ONCE("RT Material UBO Pool exhausted; falling back to dedicated buffer.");
		return UINT32_MAX;
	}
	return mat_ubo_pool_next_slot++;
}

void RenderRaytracing::mat_ubo_pool_release(uint32_t p_slot) {
	if (p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	if (mat_ubo_pool_free_count < mat_ubo_pool_free_slots.size()) {
		mat_ubo_pool_free_slots[mat_ubo_pool_free_count] = p_slot;
	} else {
		mat_ubo_pool_free_slots.push_back(p_slot);
	}
	++mat_ubo_pool_free_count;
}

void RenderRaytracing::_mat_free_dedicated_buffer(RTMaterialData *p_mat_data) {
	_free_rid_if_valid(p_mat_data->uniform_buffer);
}

void RenderRaytracing::mat_ubo_pool_update(uint32_t p_slot, const void *p_data, uint32_t p_size) {
	if (!mat_ubo_pool_buffer.is_valid() || p_slot >= MAT_UBO_POOL_CAPACITY) {
		return;
	}
	uint32_t size = MIN(p_size, MAT_UBO_POOL_SLOT_SIZE);
	RD::get_singleton()->buffer_update(mat_ubo_pool_buffer, uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE, size, p_data);
}

uint64_t RenderRaytracing::mat_ubo_pool_get_address(uint32_t p_slot) const {
	return mat_ubo_pool_bda + uint64_t(p_slot) * MAT_UBO_POOL_SLOT_SIZE;
}

// ---------------------------------------------------------------------------
// Cache management
// ---------------------------------------------------------------------------

void RenderRaytracing::cleanup_caches() {
	for (uint32_t i = 0; i < surface_chunks.size(); i++) {
		if (surface_chunks[i]) {
			for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
				RTCacheEntry *entry = &surface_chunks[i][j];
				if (entry->ptr) {
					_free_blas_resources(blas_transform_pool, entry->ptr->blas, &entry->ptr->blas_transform, 1);
					memdelete(entry->ptr);
					entry->ptr = nullptr;
				}
			}
			memdelete_arr(surface_chunks[i]);
		}
	}
	surface_chunks.clear();

	for (KeyValue<RTMeshGroupKey, RTMeshGroupEntry> &kv : mesh_groups) {
		_free_blas_resources(blas_transform_pool, kv.value.blas, kv.value.transforms.ptr(), kv.value.transforms.size());
	}
	mesh_groups.clear();

	// Free all cached deformed surface data, including every vertex capture.
	for (const RID &handle : deformed_pool.get_owned_list()) {
		_free_deformed_entry(handle);
	}
	shared_capture_entries.clear();
	shared_captured_mm_entries.clear();
	for (KeyValue<RTViewportStateKey, RTViewportState *> &kv : viewport_states) {
		kv.value->capture_entries.clear();
		kv.value->captured_mm_entries.clear();
		// Plans name material cache slots, which are freed below.
		kv.value->plan_lookup.clear();
	}
	_free_rid_if_valid(capture_arena);
	_free_rid_if_valid(capture_params_buffer);
	capture_arena_capacity = 0;
	capture_arena_used = 0;
	capture_arena_window_peak = 0;
	capture_params_capacity = 0;
	capture_params.reset();
	_clear_capture_bindings();
	capture_candidates.reset();

	// Free all merged MultiMesh BLAS data.
	{
		LocalVector<RID> live = merged_mm_pool.get_owned_list();
		for (const RID &handle : live) {
			_free_merged_mm_entry(handle);
		}
	}
	mm_handles.clear();
	particle_mm_entries.clear();

	// Free all cached material data
	for (uint32_t i = 0; i < material_chunks.size(); i++) {
		if (material_chunks[i]) {
			for (uint32_t j = 0; j < RT_CACHE_CHUNK_SIZE; j++) {
				RTMaterialCacheEntry *entry = &material_chunks[i][j];
				if (entry->ptr) {
					_mat_free_dedicated_buffer(entry->ptr);
					if (entry->ptr->uniform_pool_slot != UINT32_MAX) {
						mat_ubo_pool_release(entry->ptr->uniform_pool_slot);
					}
					memdelete(entry->ptr);
					entry->ptr = nullptr;
				}
			}
			memdelete_arr(material_chunks[i]);
		}
	}
	material_chunks.clear();
	material_table.reset();
	custom_material_slots.reset();
	custom_material_slots_prev.reset();

	_free_decal_box_blas();
	decal_cache.reset();
	dynamic_geometry_debug.clear();
	decal_infos.reset();
	decal_candidates.reset();
	decal_sort_scratch.reset();
	decal_data.reset();
	decal_instances.reset();
	tlas_instance_scratch.reset();
	tlas_transparent_instance_scratch.reset();
	blas_transform_pool.clear();

	// Reset counters
	material_free_slots.clear();
	next_material_slot = 0;
	vram_used = 0;
	cache_hits = 0;
	cache_misses = 0;
}

// ---------------------------------------------------------------------------
// RID helpers
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::get_rid_index(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() & 0xFFFFFFFFULL);
}

uint32_t RenderRaytracing::get_rid_version(RID p_rid) {
	return static_cast<uint32_t>(p_rid.get_id() >> 32);
}

RTCacheEntry *RenderRaytracing::get_surface_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;

	// Grow vector if needed, initializing new slots to nullptr
	while (chunk_idx >= surface_chunks.size()) {
		surface_chunks.push_back(nullptr);
	}

	if (!surface_chunks[chunk_idx]) {
		surface_chunks.set(chunk_idx, memnew_arr(RTCacheEntry, RT_CACHE_CHUNK_SIZE));
		for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
			surface_chunks[chunk_idx][i] = RTCacheEntry();
		}
	}

	return &surface_chunks[chunk_idx][entry_idx];
}

RTMaterialCacheEntry *RenderRaytracing::get_material_cache_entry(uint32_t p_index) {
	uint32_t chunk_idx = p_index >> RT_CACHE_CHUNK_SHIFT;
	uint32_t entry_idx = p_index & RT_CACHE_CHUNK_MASK;

	// Grow vector if needed, initializing new slots to nullptr
	while (chunk_idx >= material_chunks.size()) {
		material_chunks.push_back(nullptr);
	}

	if (!material_chunks[chunk_idx]) {
		material_chunks.set(chunk_idx, memnew_arr(RTMaterialCacheEntry, RT_CACHE_CHUNK_SIZE));
		for (uint32_t i = 0; i < RT_CACHE_CHUNK_SIZE; i++) {
			material_chunks[chunk_idx][i] = RTMaterialCacheEntry();
		}
	}

	return &material_chunks[chunk_idx][entry_idx];
}

uint32_t RenderRaytracing::allocate_material_slot() {
	if (!material_free_slots.is_empty()) {
		uint32_t slot = material_free_slots[material_free_slots.size() - 1];
		material_free_slots.resize(material_free_slots.size() - 1);
		return slot;
	}
	return next_material_slot++;
}

// ---------------------------------------------------------------------------
// Slot-pool access helpers
// ---------------------------------------------------------------------------

RTDeformedCacheEntry *RenderRaytracing::_access_deformed_slot(RID &r_handle) {
	RTDeformedCacheEntry *entry = deformed_pool.get_or_null(r_handle);
	if (!entry) {
		// Stale or never-allocated handle -> fresh slot. RID_Owner default-
		// constructs the entry, so all members start at their cleared values.
		r_handle = deformed_pool.make_rid();
		entry = deformed_pool.get_or_null(r_handle);
	}
	return entry;
}

// Like _access_deformed_slot, but leaves the entry unmarked for this frame (process_deformed_surface marks it once
// it's traced). An expired entry is reset in place so prepare_frame can't evict it before it's traced.
RTDeformedCacheEntry *RenderRaytracing::_access_capture_entry(RID &r_handle) {
	RTDeformedCacheEntry *entry = deformed_pool.get_or_null(r_handle);
	if (!entry) {
		r_handle = deformed_pool.make_rid();
		return deformed_pool.get_or_null(r_handle);
	}
	if (_rt_deformed_entry_expired(*entry, RSG::rasterizer->get_frame_number())) {
		_reset_deformed_entry(*entry);
	}
	return entry;
}

void RenderRaytracing::_free_deformed_entry(RID p_handle) {
	RTDeformedCacheEntry *entry = deformed_pool.get_or_null(p_handle);
	if (!entry) {
		return;
	}
	_release_deformed_entry(*entry);
	deformed_pool.free(p_handle);
}

RTMergedMMEntry *RenderRaytracing::_access_merged_mm_slot(RID &r_handle) {
	RTMergedMMEntry *entry = merged_mm_pool.get_or_null(r_handle);
	if (!entry) {
		r_handle = merged_mm_pool.make_rid();
		entry = merged_mm_pool.get_or_null(r_handle);
	}
	return entry;
}

// _access_capture_entry for MultiMesh and particle captures; build_tlas marks the entry once it's traced.
RTMergedMMEntry *RenderRaytracing::_access_captured_mm_entry(RID &r_handle) {
	RTMergedMMEntry *entry = merged_mm_pool.get_or_null(r_handle);
	if (!entry) {
		r_handle = merged_mm_pool.make_rid();
		return merged_mm_pool.get_or_null(r_handle);
	}
	if (_rt_merged_mm_entry_expired(*entry, RSG::rasterizer->get_frame_number())) {
		_reset_merged_mm_entry(*entry);
	}
	return entry;
}

void RenderRaytracing::_free_merged_mm_entry(RID p_handle) {
	RTMergedMMEntry *entry = merged_mm_pool.get_or_null(p_handle);
	if (!entry) {
		return;
	}
	_release_merged_mm_entry(*entry);
	merged_mm_pool.free(p_handle);
}

void RenderRaytracing::_free_capture_entry_map(HashMap<RTMMSurfaceKey, RID> &r_map, bool p_merged) {
	for (const KeyValue<RTMMSurfaceKey, RID> &kv : r_map) {
		if (p_merged) {
			_free_merged_mm_entry(kv.value);
		} else {
			_free_deformed_entry(kv.value);
		}
	}
	r_map.clear();
}

RID &RenderRaytracing::_shared_merged_mm_handle(const RTInstanceSource &p_source, uint32_t p_surface_key) {
	if (p_source.particles) {
		return particle_mm_entries[RTMMSurfaceKey{ p_source.base.get_id(), 0, p_surface_key }];
	}
	// Dense side table keyed by RID local index; the validator detects a local index recycled for another MultiMesh.
	const uint32_t li = p_source.base.get_local_index();
	const uint32_t mm_validator = static_cast<uint32_t>(p_source.base.get_id() >> 32);
	if (li >= mm_handles.size()) {
		mm_handles.resize(li + 1);
	}
	MMSurfaceHandles &mm_entry = mm_handles[li];
	if (mm_entry.mm_validator != mm_validator) {
		for (const RID &handle : mm_entry.per_surface) {
			_free_merged_mm_entry(handle);
		}
		mm_entry.per_surface.clear();
		mm_entry.mm_validator = mm_validator;
	}
	return mm_entry.surface_handle(p_surface_key);
}

void RenderRaytracing::_begin_dynamic_scheduler_frame() {
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	if (frame == dynamic_scheduler_frame) {
		return;
	}
	dynamic_scheduler_frame = frame;
	RTSchedulerBudgets budgets;
	budgets.max_operations = GLOBAL_GET_CACHED(uint32_t, "rendering/pathtracing/dynamic_blas_max_operations");
	budgets.build_triangles = GLOBAL_GET_CACHED(uint64_t, "rendering/pathtracing/dynamic_blas_build_triangle_budget");
	budgets.update_triangles = GLOBAL_GET_CACHED(uint64_t, "rendering/pathtracing/dynamic_blas_update_triangle_budget");
	budgets.rebuild_interval_frames = GLOBAL_GET_CACHED(uint32_t, "rendering/pathtracing/dynamic_rebuild_interval_frames");
	budgets.max_stale_frames = GLOBAL_GET_CACHED(uint32_t, "rendering/pathtracing/dynamic_max_stale_frames");
	dynamic_scheduler.set_budgets(budgets);
	cpu_visibility_estimator.set_offscreen_weight(GLOBAL_GET_CACHED(float, "rendering/pathtracing/dynamic_offscreen_size_weight"));
	dynamic_scheduler.begin_frame(frame);
}

// Fills dynamic_actions for dynamic_items and accounts for them in the RT_DYNAMIC_* counters.
void RenderRaytracing::_schedule_dynamic_items(const RenderDataRD *p_render_data, const RTViewportState *p_state) {
	dynamic_actions.clear();
	if (dynamic_items.is_empty()) {
		return;
	}
	_begin_dynamic_scheduler_frame();
	const RTSchedulerStats stats_before = dynamic_scheduler.get_stats();
	RTSchedulerFrameContext frame_context;
	frame_context.frame = dynamic_scheduler_frame;
	frame_context.viewport_id = uint32_t(uintptr_t(p_state));
	if (p_render_data->scene_data) {
		frame_context.camera = p_render_data->scene_data->cam_transform;
		frame_context.prev_camera = p_render_data->scene_data->prev_cam_transform;
	}
	dynamic_scheduler.schedule(cpu_visibility_estimator, frame_context, dynamic_items, dynamic_actions);
	const RTSchedulerStats &stats = dynamic_scheduler.get_stats();
	if (stats.held > stats_before.held) {
		_flag_not_ready(RSE::RENDERING_NOT_READY_RT_STALE_GEOMETRY);
	}
	if (p_render_data->render_info) {
		int *info = p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE];
		info[RSE::VIEWPORT_RENDER_INFO_RT_DYNAMIC_UPDATED] += stats.get_updated() - stats_before.get_updated();
		info[RSE::VIEWPORT_RENDER_INFO_RT_DYNAMIC_HELD] += stats.held - stats_before.held;
		info[RSE::VIEWPORT_RENDER_INFO_RT_DYNAMIC_BUDGET_LIMITED] += stats.budget_limited - stats_before.budget_limited;
	}
}

// Pool eviction and handle map pruning only bound memory (expired entries are reset or reused on access, freed
// handles reallocated), so each call checks a bounded slice and resumes there next frame: no frame scans everything.
static constexpr uint32_t RT_SWEEP_BUDGET = 64;

// Calls p_visit(handle, entry) for up to RT_SWEEP_BUDGET live entries of p_pool. p_visit may free the entry it gets.
template <typename T, typename F>
static void _rt_sweep_pool(RTPoolSweep &r_sweep, RID_Owner<T> &p_pool, const F &p_visit) {
	if (r_sweep.cursor >= r_sweep.pending.size()) {
		r_sweep.pending = p_pool.get_owned_list();
		r_sweep.cursor = 0;
	}
	const uint32_t end = MIN(r_sweep.cursor + RT_SWEEP_BUDGET, r_sweep.pending.size());
	for (; r_sweep.cursor < end; r_sweep.cursor++) {
		const RID handle = r_sweep.pending[r_sweep.cursor];
		T *entry = p_pool.get_or_null(handle);
		if (entry) {
			p_visit(handle, *entry);
		}
	}
}

// Drops up to RT_SWEEP_BUDGET checked map entries whose pooled entry was evicted or freed.
template <typename T>
static void _rt_prune_handle_map(HashMap<RTMMSurfaceKey, RID> &r_map, RTHandleMapCursor &r_cursor, const RID_Owner<T> &p_pool) {
	HashMap<RTMMSurfaceKey, RID>::Iterator it = r_cursor.valid ? r_map.find(r_cursor.key) : r_map.begin();
	if (!it) {
		it = r_map.begin();
	}
	for (uint32_t i = 0; i < RT_SWEEP_BUDGET && it; i++) {
		// Map nodes don't move on erase, so the next iterator stays valid.
		HashMap<RTMMSurfaceKey, RID>::Iterator next = it;
		++next;
		if (!p_pool.owns(it->value)) {
			r_map.remove(it);
		}
		it = next;
	}
	r_cursor.valid = bool(it);
	if (it) {
		r_cursor.key = it->key;
	}
}

// ---------------------------------------------------------------------------
// Per-frame preparation
// ---------------------------------------------------------------------------

void RenderRaytracing::prepare_frame() {
	tlas_instances.clear();
	tlas_transparent_instances.clear();
	geometry_data.clear();
	motion_indices.clear();
	motion_transforms.clear();

	const uint32_t current_frame = RSG::rasterizer->get_frame_number();

	_rt_sweep_pool(deformed_sweep, deformed_pool, [&](RID p_handle, RTDeformedCacheEntry &p_entry) {
		if (_rt_deformed_entry_expired(p_entry, current_frame)) {
			_free_deformed_entry(p_handle);
		}
	});
	_rt_prune_handle_map(shared_capture_entries, shared_capture_prune, deformed_pool);

	_rt_sweep_pool(merged_mm_sweep, merged_mm_pool, [&](RID p_handle, RTMergedMMEntry &p_entry) {
		if (_rt_merged_mm_entry_expired(p_entry, current_frame)) {
			_free_merged_mm_entry(p_handle);
		}
	});
	_rt_prune_handle_map(particle_mm_entries, particle_mm_prune, merged_mm_pool);
	_rt_prune_handle_map(shared_captured_mm_entries, shared_captured_mm_prune, merged_mm_pool);

	_evict_dead_mesh_groups();

	// Finish async HG compiles so live_ready_mask matches this frame (sync path fills at build_tlas end).
	SceneShaderRaytracing::get_singleton()->drain_completed_compiles();

	// Reset per-frame metrics
	cache_hits = 0;
	cache_misses = 0;

	bindless_block->begin_frame();
}

// ---------------------------------------------------------------------------
// Surface processing
// ---------------------------------------------------------------------------

void RenderRaytracing::_surface_cache_key(const void *p_surf, uint32_t &r_cache_key, uint32_t &r_mesh_rid_version) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);
	RID mesh_rid = surf->owner->data->base;
	if (surf->owner->data->base_type == RSE::INSTANCE_MULTIMESH) {
		RID underlying = RendererRD::MeshStorage::get_singleton()->multimesh_get_mesh(mesh_rid);
		if (underlying.is_valid()) {
			mesh_rid = underlying;
		}
	}
	r_cache_key = (mesh_rid.get_local_index() << 8) | (surf->surface_index & 0xFF);
	r_mesh_rid_version = get_rid_version(mesh_rid);
}

RTSurfaceData *RenderRaytracing::process_surface(
		const void *p_surf,
		uint32_t p_surface_invalidation_counter,
		const Transform3D &p_transform,
		LocalVector<RID> &r_dirty_blas_list) {
	uint32_t cache_key = 0;
	uint32_t mesh_rid_version = 0;
	_surface_cache_key(p_surf, cache_key, mesh_rid_version);
	return process_surface_cached(p_surf, cache_key, mesh_rid_version, p_surface_invalidation_counter, p_transform, r_dirty_blas_list);
}

RTSurfaceData *RenderRaytracing::process_surface_cached(
		const void *p_surf,
		uint32_t p_cache_key,
		uint32_t p_mesh_rid_version,
		uint32_t p_surface_invalidation_counter,
		const Transform3D &p_transform,
		LocalVector<RID> &r_dirty_blas_list) {
	const uint32_t cache_key = p_cache_key;
	const uint32_t mesh_version = p_mesh_rid_version;
	RTCacheEntry *entry = get_surface_cache_entry(cache_key);

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	bool needs_refresh = !entry->ptr ||
			entry->cached_rid_version != mesh_version ||
			entry->cached_counter != p_surface_invalidation_counter;

	if (!needs_refresh && entry->ptr->blas.is_valid()) {
		cache_hits++;
		entry->last_used_frame = current_frame;
		return entry->ptr;
	}

	cache_misses++;

	// Allocate or reuse entry
	if (!entry->ptr) {
		entry->ptr = memnew(RTSurfaceData);
	} else {
		_free_blas_resources(blas_transform_pool, entry->ptr->blas, &entry->ptr->blas_transform, 1);
	}

	RTSurfaceData *surf_data = entry->ptr;

	void *mesh_surface = static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf)->surface;
	_populate_surface_blas(mesh_surface, RID(), RID(), false, false, false, cache_key, surf_data, r_dirty_blas_list);

	if (!surf_data->blas.is_valid()) {
		return surf_data;
	}

	entry->cached_counter = p_surface_invalidation_counter;
	entry->cached_rid_version = mesh_version;
	entry->last_used_frame = current_frame;

	return surf_data;
}

// ---------------------------------------------------------------------------
// Deformed surface processing
// ---------------------------------------------------------------------------

RTSurfaceData *RenderRaytracing::process_deformed_surface(
		void *p_mesh_surface,
		const RTDeformedGeometrySource &p_source,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list) {
	ERR_FAIL_NULL_V(p_source.cache_handle, nullptr);
	if (!p_source.is_deformed()) {
		return nullptr;
	}

	RD *rd = RD::get_singleton();
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	// Compute the deformed VB layout.
	uint64_t fmt = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	bool is_2d = fmt & RSE::ARRAY_FLAG_USE_2D_VERTICES;
	bool has_n = fmt & RSE::ARRAY_FORMAT_NORMAL;
	bool has_t = fmt & RSE::ARRAY_FORMAT_TANGENT;
	uint32_t position_stride = is_2d ? (uint32_t)sizeof(float) * 2u : (uint32_t)sizeof(float) * 3u;
	uint32_t tbn_section_per_vertex = has_n ? (has_t ? 8u : 4u) : 0u;
	uint32_t per_vertex_bytes = position_stride + tbn_section_per_vertex;
	uint32_t full_size = per_vertex_bytes * vertex_count;
	uint32_t pos_size = position_stride * vertex_count;

	if (vertex_count == 0 || full_size == 0) {
		return nullptr;
	}

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	const uint64_t buffer_id = p_source.captured ? 0 : p_source.current_vb.get_id();

	RTDeformedCacheEntry *entry_ptr = _access_deformed_slot(*p_source.cache_handle);
	ERR_FAIL_NULL_V(entry_ptr, nullptr);
	RTDeformedCacheEntry &entry = *entry_ptr;

	bool layout_changed = entry.cached_vertex_count != vertex_count || entry.cached_full_size != full_size;
	bool data_changed = layout_changed ||
			entry.cached_change_stamp != p_source.change_stamp ||
			entry.cached_buffer_id != buffer_id;
	bool needs_new_blas = !entry.ptr || !entry.blas_built_once || layout_changed ||
			entry.cached_key_version != p_source.cache_version ||
			entry.cached_surface_counter != p_source.surface_counter;

	BitField<RD::BufferCreationBits> owned_full_flags = RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT |
			RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;
	BitField<RD::BufferCreationBits> prev_flags = RD::BUFFER_CREATION_AS_STORAGE_BIT |
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT;

	// Reallocate owned storage if the surface grew.
	if (entry.owned_vb_full_capacity < full_size) {
		if (entry.ptr && entry.ptr->blas.is_valid()) {
			_free_rid_if_valid(entry.ptr->blas);
			entry.blas_built_once = false;
		}
		_free_rid_if_valid(entry.owned_vb_full);
		entry.owned_vb_full = rd->vertex_buffer_create(full_size, Span<uint8_t>(), owned_full_flags);
		ERR_FAIL_COND_V(!entry.owned_vb_full.is_valid(), nullptr);
		entry.owned_vb_full_capacity = full_size;
		rd->set_resource_name(entry.owned_vb_full, "RT deformed owned VB [" + itos(p_source.cache_key) + "]");
		entry.prev_pos_seeded = false;
		needs_new_blas = true;
	}
	if (entry.prev_pos_vb_capacity < pos_size) {
		_free_rid_if_valid(entry.prev_pos_vb);
		entry.prev_pos_vb = rd->storage_buffer_create(pos_size, Vector<uint8_t>(), 0, prev_flags);
		ERR_FAIL_COND_V(!entry.prev_pos_vb.is_valid(), nullptr);
		entry.prev_pos_vb_address = rd->buffer_get_device_address(entry.prev_pos_vb);
		entry.prev_pos_vb_capacity = pos_size;
		rd->set_resource_name(entry.prev_pos_vb, "RT deformed prev pos VB [" + itos(p_source.cache_key) + "]");
		entry.prev_pos_seeded = false;
	}

	// The previous-position snapshot runs once per frame, even when the same surface is visible from multiple viewports.
	RTFrameTouch touch(entry.last_used_frame, current_frame);
	if (touch.is_first() && entry.prev_pos_seeded && !entry.prev_pos_current) {
		rd->buffer_copy(entry.owned_vb_full, entry.prev_pos_vb, 0, 0, pos_size);
		entry.prev_pos_current = true;
	}
	// Any touch with new data copies it: the BLAS refit queued below reads owned_vb_full.
	if (data_changed || !entry.prev_pos_seeded) {
		ERR_FAIL_COND_V_MSG(!p_source.current_vb.is_valid(), nullptr, "Captured surface has new data but no capture recorded this view.");
		rd->buffer_copy(p_source.current_vb, entry.owned_vb_full, p_source.current_vb_offset, 0, full_size);
		entry.prev_pos_current = false;
	}
	if (!entry.prev_pos_seeded) {
		// Seed prev = current so motion vectors are zero on the first frame.
		rd->buffer_copy(entry.owned_vb_full, entry.prev_pos_vb, 0, 0, pos_size);
		entry.prev_pos_seeded = true;
		entry.prev_pos_current = true;
	}

	if (needs_new_blas) {
		cache_misses++;
		if (!entry.ptr) {
			entry.ptr = memnew(RTSurfaceData);
		}
		_free_rid_if_valid(entry.ptr->blas);
		_copy_deformed_indices(p_mesh_surface, vertex_count, p_source.cache_key, entry);
		_populate_surface_blas(p_mesh_surface, entry.owned_vb_full, entry.owned_ib, true, true, true,
				static_cast<uint32_t>(p_source.cache_key), entry.ptr, r_dirty_blas_list);
		entry.blas_built_once = entry.ptr->blas.is_valid();
	} else if (data_changed) {
		cache_misses++;
		// Rewriting the BLAS in place is only ordered after earlier traces of the frame by the owned_vb_full copy above.
		LocalVector<RID> &queue = p_source.force_rebuild ? r_dirty_blas_list : r_dirty_blas_update_list;
		queue.push_back(entry.ptr->blas);
	} else {
		cache_hits++;
	}

	if (entry.ptr && entry.ptr->blas.is_valid()) {
		const uint32_t kept_flags = entry.ptr->geometry.flags & ~uint32_t(RT_GEOM_FLAG_CAPTURED);
		entry.ptr->geometry.flags = kept_flags | RT_GEOM_FLAG_DEFORMED | (p_source.captured ? uint32_t(RT_GEOM_FLAG_CAPTURED) : 0u);
		const uint64_t prev_addr = _rt_buffer_address(entry.prev_pos_vb, entry.prev_pos_vb_address, entry.ptr->sources.prev_vertex);
		entry.ptr->geometry.prev_vertex_buffer_address_lo = static_cast<uint32_t>(prev_addr & 0xFFFFFFFFULL);
		entry.ptr->geometry.prev_vertex_buffer_address_hi = static_cast<uint32_t>(prev_addr >> 32);
	}

	entry.cached_change_stamp = p_source.change_stamp;
	entry.cached_key_version = p_source.cache_version;
	entry.cached_surface_counter = p_source.surface_counter;
	entry.cached_buffer_id = buffer_id;
	entry.cached_vertex_count = vertex_count;
	entry.cached_full_size = full_size;
	touch.commit();

	return entry.ptr;
}

// Skinned / blend shape vertex buffer of a mesh instance surface; `current_vb` is invalid when the surface isn't deformed.
static RTDeformedGeometrySource _rt_mesh_instance_source(RID p_mesh_instance, uint32_t p_surface_index) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RTDeformedGeometrySource src;
	src.current_vb = mesh_storage->mesh_instance_get_vertex_buffer(p_mesh_instance, p_surface_index);
	src.prev_vb = mesh_storage->mesh_instance_get_prev_vertex_buffer(p_mesh_instance, p_surface_index);
	src.change_stamp = mesh_storage->mesh_instance_get_last_change(p_mesh_instance, p_surface_index);
	const uint64_t mi_id = p_mesh_instance.get_id();
	const uint32_t mi_index = static_cast<uint32_t>(mi_id & 0xFFFFFFFFULL);
	src.cache_version = static_cast<uint32_t>(mi_id >> 32);
	src.cache_key = (static_cast<uint64_t>(mi_index) << 16) | (p_surface_index & 0xFFFFu);
	return src;
}

// ---------------------------------------------------------------------------
// Vertex capture
// ---------------------------------------------------------------------------

// Capture layout of one instance of a mesh surface; empty (zero vertices) when the surface can't be captured.
static RTVertexCapture::Layout _rt_capture_layout(void *p_mesh_surface) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const uint64_t format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	RTVertexCapture::Layout layout;
	if (format & RSE::ARRAY_FLAG_USE_2D_VERTICES) {
		return layout;
	}
	layout.vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	layout.has_normal = format & RSE::ARRAY_FORMAT_NORMAL;
	layout.has_tangent = format & RSE::ARRAY_FORMAT_TANGENT;
	return layout;
}

static _FORCE_INLINE_ uint64_t _rt_fmix64(uint64_t p_h) {
	p_h ^= p_h >> 33;
	p_h *= 0xFF51AFD7ED558CCDull;
	p_h ^= p_h >> 33;
	p_h *= 0xC4CEB9FE1A85EC53ull;
	return p_h ^ (p_h >> 33);
}

// 64-bit change stamp in one pass over 8-byte words; stamps only compare within a run, never persist.
static uint64_t _rt_hash_bytes(const void *p_data, size_t p_size) {
	constexpr uint64_t K = 0x9E3779B97F4A7C15ull;
	const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
	uint64_t h = K ^ p_size;
	size_t i = 0;
	for (; i + sizeof(uint64_t) <= p_size; i += sizeof(uint64_t)) {
		uint64_t word;
		memcpy(&word, bytes + i, sizeof(word));
		h = (h ^ _rt_fmix64(word)) * K;
	}
	if (i < p_size) {
		uint64_t word = 0;
		memcpy(&word, bytes + i, p_size - i);
		h = (h ^ _rt_fmix64(word)) * K;
	}
	return _rt_fmix64(h);
}

// Stamps of the inputs p_dependencies name; the others stay 0. p_view holds the per-view fields, p_kept the inputs of the
// entry's kept capture, p_instances the change stamp of the instance records vertex() reads (0 for meshes).
// global_uniforms_stamp of p_traits this frame, computed once per shader and scratch.
static uint64_t _rt_capture_global_uniforms(RTPlanScratch &r_scratch, const RTVertexCapture::ShaderTraits &p_traits, uint64_t p_frame) {
	if (r_scratch.global_uniform_stamps_frame != p_frame) {
		r_scratch.global_uniform_stamps_frame = p_frame;
		r_scratch.global_uniform_stamps.clear();
	}
	for (const Pair<const void *, uint64_t> &stamp : r_scratch.global_uniform_stamps) {
		if (stamp.first == &p_traits) {
			return stamp.second;
		}
	}
	const RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	const uint64_t stamp = RTVertexCapture::global_uniforms_stamp(p_traits.dependencies.global_uniforms, [material_storage](const StringName &p_name) {
		return RTVertexCapture::GlobalUniformValue{ material_storage->global_shader_uniform_get_values(p_name), material_storage->global_shader_uniform_get_texture(p_name).get_id() };
	});
	r_scratch.global_uniform_stamps.push_back({ &p_traits, stamp });
	return stamp;
}

RTVertexCapture::Stamps RenderRaytracing::_capture_inputs(RTPlanScratch &r_scratch, const void *p_surface_cache, uint32_t p_dependencies, const RTVertexCapture::Stamps &p_view, const RTVertexCapture::Stamps &p_kept, const RTVertexCapture::Layout &p_layout, uint64_t p_instances) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surface_cache);
	const RenderForwardClustered::GeometryInstanceForwardClustered *inst = surf->owner;
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RTVertexCapture::Stamps stamps;
	stamps.mode_version = p_view.mode_version;
	stamps.frame = p_view.frame;
	stamps.instances = p_instances;
	stamps.instance_uniforms_version = p_view.instance_uniforms_version;
	if (p_dependencies & RTVertexCapture::DEP_GLOBAL_UNIFORMS) {
		stamps.global_uniforms = _rt_capture_global_uniforms(r_scratch, surf->shader->rt_capture, p_view.frame);
	}
	if (p_dependencies & RTVertexCapture::DEP_CAMERA) {
		stamps.camera = p_view.camera;
	}
	if (p_dependencies & RTVertexCapture::DEP_MODEL_TRANSFORM) {
		stamps.model_transform = _rt_hash_bytes(&inst->transform, sizeof(Transform3D));
	}
	if ((p_dependencies & RTVertexCapture::DEP_INSTANCE_UNIFORMS) && inst->shader_uniforms_offset >= 0) {
		const Span<const float> values = material_storage->global_shader_uniforms_get_values();
		const uint64_t first = uint64_t(inst->shader_uniforms_offset) * 4;
		const uint64_t count = ShaderLanguage::MAX_INSTANCE_UNIFORM_INDICES * 4;
		// A kept stamp of 0 was never hashed (the dependency is new), so it can't be reused.
		const bool reuse = p_kept.instance_uniforms != 0 && p_kept.instance_uniforms_version == p_view.instance_uniforms_version;
		if (first + count <= values.size()) {
			stamps.instance_uniforms = reuse ? p_kept.instance_uniforms : _rt_hash_bytes(values.ptr() + first, count * sizeof(float));
		}
	}
	if (p_dependencies & RTVertexCapture::DEP_MATERIAL_UNIFORMS) {
		const uint64_t material[2] = { surf->rt_material.get_id(), material_storage->material_get_rt_invalidation_counter(surf->rt_material) };
		stamps.material = _rt_hash_bytes(material, sizeof(material));
	}
	if (inst->mesh_instance.is_valid()) {
		stamps.deformation = mesh_storage->mesh_instance_get_last_change(inst->mesh_instance, surf->surface_index);
	}
	const uint64_t source[7] = {
		uint64_t(uintptr_t(surf->surface)),
		mesh_storage->mesh_surface_get_rt_invalidation_counter(surf->surface),
		uint64_t(uintptr_t(surf->shader)),
		surf->shader->rt_capture.code_version,
		p_layout.vertex_count,
		p_layout.instance_count,
		inst->mesh_instance.get_id(),
	};
	stamps.source = _rt_hash_bytes(source, sizeof(source));
	return stamps;
}

RTVertexCapture::Mode RenderRaytracing::read_capture_mode() {
	return RTVertexCapture::Mode(GLOBAL_GET_CACHED(int32_t, "rendering/pathtracing/vertex_capture_mode"));
}

bool RenderRaytracing::is_capture_surface(const void *p_surf, RTVertexCapture::Mode p_mode) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);
	const RenderForwardClustered::GeometryInstanceForwardClustered *inst = surf->owner;
	if (!surf->rt_base_pass || !surf->shader || !inst || !inst->data || inst->rt_procedural ||
			!RTVertexCapture::needs_capture(surf->shader->rt_capture, p_mode)) {
		return false;
	}
	switch (inst->data->base_type) {
		case RSE::INSTANCE_MESH:
			return true;
		case RSE::INSTANCE_MULTIMESH:
			return RendererRD::MeshStorage::get_singleton()->multimesh_get_transform_format(inst->data->base) == RSE::MULTIMESH_TRANSFORM_3D;
		case RSE::INSTANCE_PARTICLES:
			return RendererRD::ParticlesStorage::get_singleton()->particles_get_mode(inst->data->base) == RSE::PARTICLES_MODE_3D;
		default:
			return false;
	}
}

// Captures taken under another mode may belong to surfaces that aren't captured anymore, so all are dropped.
void RenderRaytracing::_update_capture_mode(RTVertexCapture::Mode p_mode) {
	if (int32_t(p_mode) == capture_mode) {
		return;
	}
	capture_mode = int32_t(p_mode);
	capture_mode_version++;
	_free_capture_entry_map(shared_capture_entries, false);
	_free_capture_entry_map(shared_captured_mm_entries, true);
	for (KeyValue<RTViewportStateKey, RTViewportState *> &state : viewport_states) {
		_free_capture_entry_map(state.value->capture_entries, false);
		_free_capture_entry_map(state.value->captured_mm_entries, true);
	}
}

RTCaptureState *RenderRaytracing::_capture_state(const CaptureBinding &p_binding) {
	if (p_binding.merged) {
		RTMergedMMEntry *entry = merged_mm_pool.get_or_null(p_binding.entry);
		return entry ? &entry->capture : nullptr;
	}
	RTDeformedCacheEntry *entry = deformed_pool.get_or_null(p_binding.entry);
	return entry ? &entry->capture : nullptr;
}

void RenderRaytracing::_reset_capture_entry(const CaptureBinding &p_binding) {
	if (p_binding.merged) {
		RTMergedMMEntry *entry = merged_mm_pool.get_or_null(p_binding.entry);
		ERR_FAIL_NULL(entry);
		_reset_merged_mm_entry(*entry);
	} else {
		RTDeformedCacheEntry *entry = deformed_pool.get_or_null(p_binding.entry);
		ERR_FAIL_NULL(entry);
		_reset_deformed_entry(*entry);
	}
}

// Bumping the view number orphans every surface's binding index, so stale indices are never followed.
void RenderRaytracing::_clear_capture_bindings() {
	capture_bindings.clear();
	capture_view++;
}

void RenderRaytracing::_bind_capture(const void *p_surf, const CaptureBinding &p_binding) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);
	surf->rt_capture_view = capture_view;
	surf->rt_capture_binding = capture_bindings.size();
	capture_bindings.push_back(p_binding);
}

RenderRaytracing::CaptureBinding *RenderRaytracing::_capture_binding(const void *p_surf) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);
	return surf->rt_capture_view == capture_view ? &capture_bindings[surf->rt_capture_binding] : nullptr;
}

// The material the rasterizer draws the surface with. Its fallback default material is traced as the RT default.
RID RenderRaytracing::_surface_material_rid(const void *p_surf) const {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);
	return surf->rt_material == owner->scene_shader.default_material ? RID() : surf->rt_material;
}

RTMaterialData *RenderRaytracing::_surface_material(const void *p_surf) {
	const RID material = _surface_material_rid(p_surf);
	return process_material(material, RendererRD::MaterialStorage::get_singleton()->material_get_rt_invalidation_counter(material));
}

// The only point where planning may update the material cache: when p_ctx allows mutation. Null when deferred.
const RTMaterialData *RenderRaytracing::_plan_surface_material(const RTPlanContext &p_ctx, const void *p_surf, uint32_t &r_slot, uint16_t &r_counter) {
	const RID material = _surface_material_rid(p_surf);
	r_counter = p_ctx.material_storage->material_get_rt_invalidation_counter(material);
	const RTMaterialData *mat_data = lookup_material(material, r_counter, r_slot);
	if (mat_data || !p_ctx.allow_mutation) {
		return mat_data;
	}
	r_slot = material.is_valid() ? get_rid_index(material) : UINT32_MAX;
	return process_material(material, r_counter);
}

const RTCaptureState *RenderRaytracing::_peek_capture_state(const CaptureBinding &p_binding) const {
	// get_or_null() doesn't modify the pools; RID_Owner only lacks a const overload.
	return const_cast<RenderRaytracing *>(this)->_capture_state(p_binding);
}

bool RenderRaytracing::_peek_captured(const void *p_surf, const LocalVector<RTCaptureRequest> &p_captures) const {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);
	if (surf->rt_capture_view != capture_view) {
		return false;
	}
	const CaptureBinding &binding = capture_bindings[surf->rt_capture_binding];
	const RTCaptureState *capture = _peek_capture_state(binding);
	return capture && ((binding.request != UINT32_MAX && p_captures[binding.request].recorded) || capture->stamp != 0);
}

RTPlanContext RenderRaytracing::_make_plan_context(const RenderDataRD *p_render_data, uint32_t p_rt_flags, const LocalVector<RTCaptureRequest> *p_captures) const {
	RTPlanContext ctx;
	ctx.mesh_storage = RendererRD::MeshStorage::get_singleton();
	ctx.material_storage = RendererRD::MaterialStorage::get_singleton();
	ctx.rt_shader = SceneShaderRaytracing::get_singleton();
	ctx.captures = p_captures;
	ctx.versions.material_version = ctx.material_storage->material_get_rt_version();
	ctx.versions.mesh_version = ctx.mesh_storage->mesh_get_rt_invalidation_version();
	// A changed hit group slot rebuilds its material with an unchanged counter, so it invalidates plans like readiness does.
	ctx.versions.ready_version = ctx.rt_shader->get_ready_version() + uint32_t(material_slot_version);
	ctx.versions.capture_mode_version = capture_mode_version;
	ctx.view_stamps.frame = RSG::rasterizer->get_frame_number();
	ctx.view_stamps.mode_version = capture_mode_version;
	ctx.view_stamps.instance_uniforms_version = ctx.material_storage->global_shader_parameters_instance_get_version();
	if (const RenderSceneDataRD *scene_data = p_render_data->scene_data) {
		const uint64_t camera[2] = {
			_rt_hash_bytes(&scene_data->cam_transform, sizeof(Transform3D)),
			_rt_hash_bytes(&scene_data->cam_projection, sizeof(Projection)),
		};
		ctx.view_stamps.camera = _rt_hash_bytes(camera, sizeof(camera));
		ctx.camera_visible_layers = scene_data->camera_visible_layers;
	}
	ctx.rt_flags = p_rt_flags;
	ctx.capture_mode = read_capture_mode();
	ctx.allow_mutation = true;
	return ctx;
}

// prepare_vertex_captures and build_tlas plan into the same records, so whichever runs first in a frame opens the arena.
RTPlanArena &RenderRaytracing::_open_plan_arena(RTViewportState *p_state, const RenderDataRD *p_render_data, const RTPlanContext &p_ctx) {
	// Plans don't record the pipeline their hit groups were checked against, so a flags change drops them.
	if (p_state->plan_rt_flags != p_ctx.rt_flags) {
		p_state->plan_rt_flags = p_ctx.rt_flags;
		p_state->plan_lookup.clear();
	}
	const PagedArray<RenderGeometryInstance *> &instances = *p_render_data->rt_instances;
	const uint32_t count = uint32_t(instances.size());
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	const bool opened = p_state->plan_frame == frame && p_state->plan_arenas[p_state->plan_current].instances.size() == count;
	if (!opened && p_state->plan_frame != frame) {
		p_state->plan_current ^= 1;
		p_state->plan_frame = frame;
	}
	RTPlanArena &arena = p_state->plan_arenas[p_state->plan_current];
	if (!opened) {
		plan_capacities.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
					static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(instances[i]);
			plan_capacities[i] = inst ? inst->rt_surface_count : 0;
		}
		rt_plan_reserve(arena, plan_capacities);
	}
	// The pass planning now validates against its own context; the TLAS pass runs last, so its versions are kept.
	arena.versions = p_ctx.versions;
	return arena;
}

// Instances per planning task.
static constexpr uint32_t RT_PLAN_CHUNK_SIZE = 64;
static constexpr uint32_t RT_PLAN_PARALLEL_MIN_INSTANCES = 256;

// Strict validation re-plans reused records into one shared arena, so it keeps planning serial.
static bool _rt_plan_parallel(uint32_t p_count) {
#ifdef RT_VALIDATION_ENABLED
	if (rt_validation().strict) {
		return false;
	}
#endif
	return p_count >= RT_PLAN_PARALLEL_MIN_INSTANCES && GLOBAL_GET_CACHED(bool, "rendering/pathtracing/parallel_planning");
}

// Calls p_plan(ctx, scratch, index) for instances [0, p_count): first on the worker pool with mutation off, then serially,
// in index order, for those it deferred. p_plan returns false when deferred, and must only write its own record.
template <typename F>
static void _rt_plan_instances(const RTPlanContext &p_ctx, uint32_t p_count, RTPlanWorkspace &r_workspace, const F &p_plan) {
	const bool parallel = _rt_plan_parallel(p_count);
	if (parallel) {
		struct Job {
			const F *plan;
			RTPlanContext ctx;
			RTPlanWorkspace *workspace;
			uint32_t count;
		};
		Job job = { &p_plan, p_ctx, &r_workspace, p_count };
		job.ctx.allow_mutation = false;
		const uint32_t chunks = (p_count + RT_PLAN_CHUNK_SIZE - 1) / RT_PLAN_CHUNK_SIZE;
		r_workspace.chunks.resize(chunks);
		r_workspace.deferred.resize(p_count);
		const auto run_chunk = [](void *p_job, uint32_t p_chunk) {
			const Job &j = *static_cast<const Job *>(p_job);
			const uint32_t end = MIN((p_chunk + 1) * RT_PLAN_CHUNK_SIZE, j.count);
			for (uint32_t i = p_chunk * RT_PLAN_CHUNK_SIZE; i < end; i++) {
				j.workspace->deferred[i] = !(*j.plan)(j.ctx, j.workspace->chunks[p_chunk], i);
			}
		};
		WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
		pool->wait_for_group_task_completion(pool->add_native_group_task(run_chunk, &job, int(chunks), -1, true));
	}
	for (uint32_t i = 0; i < p_count; i++) {
		if (!parallel || r_workspace.deferred[i]) {
			ERR_CONTINUE_MSG(!p_plan(p_ctx, r_workspace.serial, i), "Serial planning can't be deferred.");
		}
	}
}

static const RTPlanArena &_rt_prev_plan_arena(const RTViewportState &p_state) {
	return p_state.plan_arenas[p_state.plan_current ^ 1];
}

// Last frame's record of p_inst in _rt_prev_plan_arena(p_state), or UINT32_MAX.
static uint32_t _rt_plan_prev_index(const RTViewportState &p_state, uint32_t p_rt_index, uint64_t p_surfaces_version) {
	uint32_t index = UINT32_MAX;
	if (p_rt_index != UINT32_MAX) {
		const LocalVector<uint32_t> &lookup = p_state.plan_lookup;
		rt_plan_lookup(Span<uint32_t>(lookup.ptr(), lookup.size()), _rt_prev_plan_arena(p_state), p_rt_index, p_surfaces_version, index);
	}
	return index;
}

// Flags the capture plan owns; the TLAS plan and its copies keep them.
static constexpr uint8_t RT_PLAN_CAPTURE_PART_FLAGS = RT_PLAN_INSTANCE_NO_CAPTURE_SURFACES;

template <typename T>
static void _rt_copy_plan_range(const LocalVector<T> &p_src, uint32_t p_src_begin, LocalVector<T> &r_dst, uint32_t p_dst_begin, uint32_t p_count) {
	static_assert(std::is_trivially_copyable_v<T>, "Plan ranges are copied with memcpy.");
	if (p_count > 0) {
		memcpy(r_dst.ptr() + p_dst_begin, p_src.ptr() + p_src_begin, size_t(p_count) * sizeof(T));
	}
}

// rt_plan_copy_forward of the TLAS part only: the destination keeps its key and this frame's capture plan.
static bool _rt_copy_tlas_plan(const RTPlanArena &p_src, uint32_t p_src_index, RTPlanArena &r_dst, uint32_t p_dst_index) {
	const RTPlanInstance &src = p_src.instances[p_src_index];
	RTPlanInstance &dst = r_dst.instances[p_dst_index];
	if (src.surface_count > dst.range_capacity) {
		return false;
	}
	dst.group_count = src.group_count;
	dst.surface_count = src.surface_count;
	dst.pending_surfaces = src.pending_surfaces;
	dst.flags = uint8_t((src.flags & ~RT_PLAN_CAPTURE_PART_FLAGS) | (dst.flags & RT_PLAN_CAPTURE_PART_FLAGS));
	_rt_copy_plan_range(p_src.groups, src.range_begin, r_dst.groups, dst.range_begin, src.group_count);
	_rt_copy_plan_range(p_src.surfaces, src.range_begin, r_dst.surfaces, dst.range_begin, src.surface_count);
	return true;
}

// Keeps the entry memo of captures planned again for the same surfaces.
static void _rt_carry_capture_entries(const RTPlanArena &p_prev, uint32_t p_prev_index, RTPlanArena &r_arena, uint32_t p_index) {
	const RTPlanInstance &prev = p_prev.instances[p_prev_index];
	const RTPlanInstance &record = r_arena.instances[p_index];
	const uint32_t count = MIN(prev.capture_count, record.capture_count);
	for (uint32_t k = 0; k < count; k++) {
		const RTPlanCapture &old = p_prev.captures[prev.range_begin + k];
		RTPlanCapture &capture = r_arena.captures[record.range_begin + k];
		if (old.surf == capture.surf && old.surface_key == capture.surface_key &&
				(old.flags & RT_PLAN_CAPTURE_IDENTITY_FLAGS) == (capture.flags & RT_PLAN_CAPTURE_IDENTITY_FLAGS)) {
			capture.entry = old.entry;
		}
	}
}

#ifdef RT_VALIDATION_ENABLED
static bool _rt_plan_element_equal(const RTPlanGroup &p_a, const RTPlanGroup &p_b) {
	const RTSurfaceGroupKey &a = p_a.key;
	const RTSurfaceGroupKey &b = p_b.key;
	return a.cull_mode == b.cull_mode && a.sbt_offset == b.sbt_offset && a.tlas_mask == b.tlas_mask && a.billboard == b.billboard &&
			a.opaque == b.opaque && a.groupable == b.groupable && p_a.surface_mask == p_b.surface_mask && p_a.opaque_mask == p_b.opaque_mask &&
			p_a.surface_begin == p_b.surface_begin && p_a.surface_count == p_b.surface_count && p_a.transparent == p_b.transparent;
}

static bool _rt_plan_element_equal(const RTPlanSurface &p_a, const RTPlanSurface &p_b) {
	return p_a.surf == p_b.surf && p_a.material_slot == p_b.material_slot && p_a.cache_key == p_b.cache_key && p_a.mesh_rid_version == p_b.mesh_rid_version &&
			p_a.surface_counter == p_b.surface_counter && p_a.material_flags == p_b.material_flags && p_a.material_counter == p_b.material_counter &&
			p_a.surface_index == p_b.surface_index && p_a.deformed_kind == p_b.deformed_kind && p_a.flags == p_b.flags;
}

// The entry memo is resolved by apply, so it isn't part of the plan.
static bool _rt_plan_element_equal(const RTPlanCapture &p_a, const RTPlanCapture &p_b) {
	return p_a.surf == p_b.surf && p_a.layout.vertex_count == p_b.layout.vertex_count && p_a.layout.instance_count == p_b.layout.instance_count &&
			p_a.layout.has_normal == p_b.layout.has_normal && p_a.layout.has_tangent == p_b.layout.has_tangent && p_a.dependencies == p_b.dependencies &&
			p_a.triangle_count == p_b.triangle_count && p_a.surface_key == p_b.surface_key &&
			(p_a.flags & RT_PLAN_CAPTURE_IDENTITY_FLAGS) == (p_b.flags & RT_PLAN_CAPTURE_IDENTITY_FLAGS);
}

static bool _rt_plan_element_equal(const AABB &p_a, const AABB &p_b) {
	return p_a == p_b;
}

template <typename T>
static bool _rt_plan_ranges_equal(const LocalVector<T> &p_a, uint32_t p_a_begin, const LocalVector<T> &p_b, uint32_t p_b_begin, uint32_t p_count) {
	for (uint32_t i = 0; i < p_count; i++) {
		if (!_rt_plan_element_equal(p_a[p_a_begin + i], p_b[p_b_begin + i])) {
			return false;
		}
	}
	return true;
}

// True when the TLAS part (p_tlas_part) or the capture part of two records holds the same plan.
static bool _rt_plan_records_equal(const RTPlanArena &p_a, uint32_t p_a_index, const RTPlanArena &p_b, uint32_t p_b_index, bool p_tlas_part) {
	const RTPlanInstance &a = p_a.instances[p_a_index];
	const RTPlanInstance &b = p_b.instances[p_b_index];
	const uint8_t part_flags = p_tlas_part ? uint8_t(~RT_PLAN_CAPTURE_PART_FLAGS) : RT_PLAN_CAPTURE_PART_FLAGS;
	if ((a.flags & part_flags) != (b.flags & part_flags)) {
		return false;
	}
	if (!p_tlas_part) {
		return a.capture_count == b.capture_count && _rt_plan_ranges_equal(p_a.captures, a.range_begin, p_b.captures, b.range_begin, a.capture_count) &&
				_rt_plan_ranges_equal(p_a.capture_aabbs, a.range_begin, p_b.capture_aabbs, b.range_begin, a.capture_count);
	}
	return a.group_count == b.group_count && a.surface_count == b.surface_count && a.pending_surfaces == b.pending_surfaces &&
			_rt_plan_ranges_equal(p_a.groups, a.range_begin, p_b.groups, b.range_begin, a.group_count) &&
			_rt_plan_ranges_equal(p_a.surfaces, a.range_begin, p_b.surfaces, b.range_begin, a.surface_count);
}

RTPlanArena &RenderRaytracing::_rtv_replan_arena(const RTPlanArena &p_arena, uint32_t p_index) {
	uint32_t capacity = p_arena.instances[p_index].range_capacity;
	rt_plan_reserve(rtv_plan_arena, Span<uint32_t>(&capacity, 1));
	rtv_plan_arena.versions = p_arena.versions;
	rtv_plan_arena.instances[0].key = p_arena.instances[p_index].key;
	return rtv_plan_arena;
}

void RenderRaytracing::_rtv_check_reused_plan(const RTPlanArena &p_arena, uint32_t p_index, bool p_tlas_part, bool p_planned) const {
	if (p_planned && _rt_plan_records_equal(p_arena, p_index, rtv_plan_arena, 0, p_tlas_part)) {
		return;
	}
	if (p_tlas_part) {
		ERR_PRINT_ONCE("Raytracing: a reused TLAS plan differs from a fresh one.");
	} else {
		ERR_PRINT_ONCE("Raytracing: a reused capture plan differs from a fresh one.");
	}
}

void RenderRaytracing::_rtv_check_reused_captures(const RTPlanContext &p_ctx, const void *p_inst, const RTPlanArena &p_arena, uint32_t p_index) {
	const bool planned = _plan_capture_instance(p_ctx, p_inst, _rtv_replan_arena(p_arena, p_index), 0);
	_rtv_check_reused_plan(p_arena, p_index, false, planned);
}

static void _rtv_check_capture_memo(const RID *p_mapped, RID p_memo) {
	if (!p_mapped || *p_mapped != p_memo) {
		ERR_PRINT_ONCE("Raytracing: a capture entry memo differs from its handle map entry.");
	}
}
#endif

// Last frame's capture part can be kept only when it had no capture surface: anything else follows per-frame state.
// A degenerate transform skips the surface check in _plan_capture_instance, so it isn't reused either.
static bool _rt_capture_part_reusable(const RTPlanArena &p_prev, uint32_t p_prev_index, const Transform3D &p_transform, const RTPlanContext &p_ctx) {
	return (p_prev.instances[p_prev_index].flags & RT_PLAN_INSTANCE_NO_CAPTURE_SURFACES) && p_prev.versions.capture_mode_version == p_ctx.versions.capture_mode_version &&
			!Math::is_zero_approx(p_transform.basis.determinant());
}

bool RenderRaytracing::_plan_captures(const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, const RTViewportState &p_state, const void *p_inst, RTPlanArena &r_arena, uint32_t p_index) {
	const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
			static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(p_inst);
	const RTPlanArena &prev = _rt_prev_plan_arena(p_state);
	const uint32_t prev_index = inst ? _rt_plan_prev_index(p_state, inst->rt_index, inst->rt_surfaces_version) : UINT32_MAX;
	if (prev_index != UINT32_MAX && _rt_capture_part_reusable(prev, prev_index, inst->transform, p_ctx)) {
		RTPlanInstance &record = r_arena.instances[p_index];
		record.capture_count = 0;
		record.flags |= RT_PLAN_INSTANCE_NO_CAPTURE_SURFACES;
		RT_VALIDATE(_rtv_check_reused_captures(p_ctx, p_inst, r_arena, p_index));
		return true;
	}
	if (!_plan_capture_instance(p_ctx, p_inst, r_arena, p_index)) {
		return false;
	}
	if (prev_index != UINT32_MAX) {
		_rt_carry_capture_entries(prev, prev_index, r_arena, p_index);
	}
	_evaluate_captures(p_ctx, r_scratch, p_inst, r_arena, p_index);
	return true;
}

const RTCaptureState *RenderRaytracing::_peek_live_capture_state(RID p_entry, bool p_merged, uint32_t p_frame) const {
	RenderRaytracing *self = const_cast<RenderRaytracing *>(this); // RID_Owner only lacks a const get_or_null.
	if (p_merged) {
		const RTMergedMMEntry *entry = self->merged_mm_pool.get_or_null(p_entry);
		return entry && !_rt_merged_mm_entry_expired(*entry, p_frame) ? &entry->capture : nullptr;
	}
	const RTDeformedCacheEntry *entry = self->deformed_pool.get_or_null(p_entry);
	return entry && !_rt_deformed_entry_expired(*entry, p_frame) ? &entry->capture : nullptr;
}

void RenderRaytracing::_evaluate_captures(const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, const void *p_inst, RTPlanArena &r_arena, uint32_t p_index) const {
	const RTPlanInstance &record = r_arena.instances[p_index];
	if (record.capture_count == 0 || !p_ctx.frustum) {
		return;
	}
	const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
			static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(p_inst);
	RTInstanceSource source;
	if (inst->data->base_type != RSE::INSTANCE_MESH && !_rt_instance_source(inst->data->base_type, inst->data->base, source)) {
		return;
	}
	const uint32_t frame = uint32_t(p_ctx.view_stamps.frame);
	for (uint32_t index = record.range_begin; index < record.range_begin + record.capture_count; index++) {
		RTPlanCapture &capture = r_arena.captures[index];
		capture.flags &= RT_PLAN_CAPTURE_IDENTITY_FLAGS;
		if (p_ctx.frustum->intersects(r_arena.capture_aabbs[index])) {
			capture.flags |= RT_PLAN_CAPTURE_IN_FRUSTUM;
		}
		const RTCaptureState *kept = _peek_live_capture_state(capture.entry, capture.flags & RT_PLAN_CAPTURE_MERGED, frame);
		if (!kept) {
			continue;
		}
		RTPlanCaptureEval &eval = r_arena.capture_evals[index];
		eval.inputs = _capture_inputs(r_scratch, capture.surf, capture.dependencies, p_ctx.view_stamps, kept->inputs, capture.layout, source.last_change);
		eval.recapture = kept->frame == p_ctx.view_stamps.frame
				? RTVertexCapture::RECAPTURE_NONE
				: RTVertexCapture::needs_recapture(kept->stamp != 0, kept->inputs, eval.inputs, capture.dependencies);
		capture.flags |= RT_PLAN_CAPTURE_EVALUATED;
	}
}

HashMap<RTMMSurfaceKey, RID> &RenderRaytracing::_capture_entry_map(RTViewportState *p_state, uint8_t p_capture_flags) {
	const bool merged = p_capture_flags & RT_PLAN_CAPTURE_MERGED;
	if (p_capture_flags & RT_PLAN_CAPTURE_CAMERA_DEPENDENT) {
		return merged ? p_state->captured_mm_entries : p_state->capture_entries;
	}
	return merged ? shared_captured_mm_entries : shared_capture_entries;
}

// The pool entry of r_capture: its memo while the pool owns it, which then equals the handle map's entry, else the
// handle map's. Null when it can't be allocated.
RID RenderRaytracing::_resolve_capture_entry(RTViewportState *p_state, const RTMMSurfaceKey &p_key, RTPlanCapture &r_capture) {
	const bool merged = r_capture.flags & RT_PLAN_CAPTURE_MERGED;
	HashMap<RTMMSurfaceKey, RID> &entries = _capture_entry_map(p_state, r_capture.flags);
	const bool memo_alive = merged ? merged_mm_pool.owns(r_capture.entry) : deformed_pool.owns(r_capture.entry);
	RID &handle = memo_alive ? r_capture.entry : entries[p_key];
	// Resets an expired entry in place, like a handle map access.
	const bool accessed = merged ? _access_captured_mm_entry(handle) != nullptr : _access_capture_entry(handle) != nullptr;
	if (!accessed) {
		return RID();
	}
	if (memo_alive) {
		RT_VALIDATE(_rtv_check_capture_memo(entries.getptr(p_key), handle));
	}
	r_capture.entry = handle;
	return handle;
}

// The only shader state planning writes: the vertex() analysis on a shader's first capture. False when deferred.
bool RenderRaytracing::_plan_capture_dependencies(const RTPlanContext &p_ctx, const void *p_surf) {
	const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf =
			static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(p_surf);
	RTVertexCapture::ShaderTraits &traits = surf->shader->rt_capture;
	if (traits.dependencies.mask != RTVertexCapture::DEP_UNKNOWN) {
		return true;
	}
	if (!p_ctx.allow_mutation) {
		return false;
	}
	traits.dependencies = SceneShaderRaytracing::compute_vertex_capture_dependencies(surf->shader->code);
	traits.code_version = ++capture_code_version_counter;
	return true;
}

bool RenderRaytracing::_plan_capture_instance(const RTPlanContext &p_ctx, const void *p_inst, RTPlanArena &r_arena, uint32_t p_index) {
	const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
			static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(p_inst);
	RTPlanInstance &record = r_arena.instances[p_index];
	record.capture_count = 0;
	record.flags &= ~RT_PLAN_INSTANCE_NO_CAPTURE_SURFACES;
	if (!inst || !inst->data || Math::is_zero_approx(inst->transform.basis.determinant())) {
		return true;
	}
	// MultiMesh and particle captures are merged into one BLAS per surface, in TLAS instance space.
	const bool merged = inst->data->base_type != RSE::INSTANCE_MESH;
	RTInstanceSource source;
	if (merged && !_rt_instance_source(inst->data->base_type, inst->data->base, source)) {
		return true;
	}

	// Particle surface indices restart for every draw pass mesh, so they are keyed by list position.
	uint32_t surface_ordinal = 0;
	bool has_capture_surface = false;
	for (const RenderForwardClustered::GeometryInstanceSurfaceDataCache *surf = inst->surface_caches; surf; surf = surf->next, surface_ordinal++) {
		if (!is_capture_surface(surf, p_ctx.capture_mode)) {
			continue;
		}
		has_capture_surface = true;
		// Captures can only be committed once the surface's hit group is traced, so they aren't recorded before.
		uint32_t material_slot = UINT32_MAX;
		uint16_t material_counter = 0;
		const RTMaterialData *mat_data = _plan_surface_material(p_ctx, surf, material_slot, material_counter);
		if (!mat_data) {
			return false;
		}
		if (!p_ctx.rt_shader->is_hg_ready_in_bundle(mat_data->rt_sbt_offset, p_ctx.rt_flags)) {
			continue;
		}
		RTVertexCapture::Layout layout = _rt_capture_layout(surf->surface);
		if (merged) {
			layout.instance_count = _rt_drawn_instances(source, surf->flags & RenderForwardClustered::GeometryInstanceSurfaceDataCache::FLAG_USES_PARTICLE_TRAILS).count;
		}
		if (layout.point_count() == 0) {
			continue;
		}
		if (!_plan_capture_dependencies(p_ctx, surf)) {
			return false;
		}
		ERR_BREAK_MSG(record.capture_count >= record.range_capacity, "Instance has more surfaces than its rt_surface_count.");

		const uint32_t index = record.range_begin + record.capture_count++;
		RTPlanCapture &capture = r_arena.captures[index];
		capture = RTPlanCapture();
		capture.surf = surf;
		capture.layout = layout;
		capture.dependencies = RTVertexCapture::surface_dependencies(surf->shader->rt_capture, surf->shader->billboard.is_camera_dependent());
		const uint64_t triangles = uint64_t(_rt_indices_to_primitives(surf->primitive, p_ctx.mesh_storage->mesh_surface_get_vertices_drawn_count(surf->surface))) * layout.instance_count;
		capture.triangle_count = uint32_t(MIN(triangles, uint64_t(UINT32_MAX)));
		capture.surface_key = source.particles ? surface_ordinal : surf->surface_index;
		capture.flags = uint8_t((merged ? RT_PLAN_CAPTURE_MERGED : 0) | ((capture.dependencies & RTVertexCapture::DEP_CAMERA) ? RT_PLAN_CAPTURE_CAMERA_DEPENDENT : 0));
		r_arena.capture_aabbs[index] = inst->transformed_aabb;
	}
	if (!has_capture_surface) {
		record.flags |= RT_PLAN_INSTANCE_NO_CAPTURE_SURFACES;
	}
	return true;
}

void RenderRaytracing::_apply_capture_instance(const RTPlanContext &p_ctx, RTViewportState *p_state, const void *p_inst, RTPlanArena &r_arena, uint32_t p_index) {
	const RTPlanInstance &record = r_arena.instances[p_index];
	if (record.capture_count == 0) {
		return;
	}
	const RenderForwardClustered::GeometryInstanceForwardClustered *inst =
			static_cast<const RenderForwardClustered::GeometryInstanceForwardClustered *>(p_inst);
	RTInstanceSource source;
	if (inst->data->base_type != RSE::INSTANCE_MESH) {
		ERR_FAIL_COND(!_rt_instance_source(inst->data->base_type, inst->data->base, source));
	}
	const Transform3D capture_transform = source.world_space ? Transform3D() : inst->transform;

	for (uint32_t index = record.range_begin; index < record.range_begin + record.capture_count; index++) {
		RTPlanCapture &capture = r_arena.captures[index];
		const bool merged = capture.flags & RT_PLAN_CAPTURE_MERGED;
		const RTMMSurfaceKey key{ inst->data->base.get_id(), uint64_t(uintptr_t(inst)), capture.surface_key };
		const RID evaluated_entry = capture.entry;
		const RID handle = _resolve_capture_entry(p_state, key, capture);
		ERR_CONTINUE(handle.is_null());

		CaptureBinding binding;
		binding.entry = handle;
		binding.merged = merged;
		RTVertexCapture::Recapture recapture;
		if ((capture.flags & RT_PLAN_CAPTURE_EVALUATED) && handle == evaluated_entry) {
			const RTPlanCaptureEval &eval = r_arena.capture_evals[index];
			binding.inputs = eval.inputs;
			recapture = eval.recapture;
		} else {
			const RTCaptureState &kept = *_capture_state(binding);
			binding.inputs = _capture_inputs(plan_workspace.serial, capture.surf, capture.dependencies, p_ctx.view_stamps, kept.inputs, capture.layout, source.last_change);
			recapture = kept.frame == p_ctx.view_stamps.frame
					? RTVertexCapture::RECAPTURE_NONE
					: RTVertexCapture::needs_recapture(kept.stamp != 0, kept.inputs, binding.inputs, capture.dependencies);
		}
		if (recapture == RTVertexCapture::RECAPTURE_NONE) {
			_bind_capture(capture.surf, binding);
			continue;
		}
		if (recapture == RTVertexCapture::RECAPTURE_REBUILD) {
			_reset_capture_entry(binding);
			binding.rebuild = true;
		}

		RTSchedulerItem item;
		item.slot = _rt_scheduler_slot(handle, merged ? RT_SCHEDULER_POOL_CAPTURED_MM : RT_SCHEDULER_POOL_CAPTURE);
		item.gen = _rt_entry_generation(handle);
		item.needs_build = binding.rebuild;
		item.in_frustum = capture.flags & RT_PLAN_CAPTURE_IN_FRUSTUM;
		item.blas_update = rt_resolve_blas_update(inst->rt_blas_update,
				static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(capture.surf)->shader->rt_blas_update);
		item.aabb = r_arena.capture_aabbs[index];
		item.triangle_count = capture.triangle_count;
		dynamic_items.push_back(item);
		capture_candidates.push_back(CaptureCandidate{ capture.surf, binding, capture.layout, capture_transform, source.dead_position });
	}
}

void RenderRaytracing::prepare_vertex_captures(const RenderDataRD *p_render_data, uint32_t p_rt_flags, LocalVector<RTCaptureRequest> &r_requests) {
	r_requests.clear();
	capture_params.clear();
	_clear_capture_bindings();
	capture_candidates.clear();
	capture_arena_used = 0;
	_refresh_custom_shader_slots();
	const RTVertexCapture::Mode mode = read_capture_mode();
	_update_capture_mode(mode);
	if (mode == RTVertexCapture::MODE_DISABLED || !p_render_data->rt_instances || !p_render_data->scene_data ||
			!RD::get_singleton()->has_feature(RD::SUPPORTS_VERTEX_PIPELINE_STORES)) {
		return;
	}
	RTViewportState *state = _get_or_create_viewport_state(p_render_data);
	if (!state) {
		return;
	}
	_rt_prune_handle_map(state->capture_entries, state->capture_prune, deformed_pool);
	_rt_prune_handle_map(state->captured_mm_entries, state->captured_mm_prune, merged_mm_pool);

	const RenderSceneDataRD *scene_data = p_render_data->scene_data;
	const RTViewFrustum frustum(p_render_data);
	RTPlanContext ctx = _make_plan_context(p_render_data, p_rt_flags, nullptr);
	ctx.frustum = &frustum;
	RTPlanArena &arena = _open_plan_arena(state, p_render_data, ctx);
	const PagedArray<RenderGeometryInstance *> &rt_instances = *p_render_data->rt_instances;
	_rt_plan_instances(ctx, arena.instances.size(), plan_workspace, [&](const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, uint32_t p_index) {
		return _plan_captures(p_ctx, r_scratch, *state, rt_instances[p_index], arena, p_index);
	});

	// Bind surfaces whose capture is current; queue the others for the scheduler.
	dynamic_items.clear();
	for (uint32_t i = 0; i < arena.instances.size(); i++) {
		_apply_capture_instance(ctx, state, rt_instances[i], arena, i);
	}

	// Held captures keep their last capture; a held first capture leaves the surface unbound (static fallback).
	_schedule_dynamic_items(p_render_data, state);
	uint64_t arena_size = 0;
	for (uint32_t i = 0; i < capture_candidates.size(); i++) {
		CaptureCandidate &candidate = capture_candidates[i];
		const bool fits = arena_size + candidate.layout.total_bytes() <= UINT32_MAX;
		if (dynamic_actions[i] == RT_SCHEDULER_HOLD || !fits) {
			if (!candidate.binding.rebuild) {
				_bind_capture(candidate.surf, candidate.binding);
			}
			continue;
		}
		RTCaptureRequest request;
		request.surf = candidate.surf;
		request.layout = candidate.layout;
		request.arena_offset = uint32_t(arena_size);
		candidate.binding.request = r_requests.size();
		candidate.binding.rebuild = candidate.binding.rebuild || dynamic_actions[i] == RT_SCHEDULER_REBUILD;
		_bind_capture(candidate.surf, candidate.binding);
		r_requests.push_back(request);
		capture_params.push_back(RTVertexCapture::make_draw_params(candidate.layout, request.arena_offset, candidate.transform, scene_data->cam_transform, candidate.dead_position));
		arena_size += candidate.layout.total_bytes();
	}
	_reserve_capture_arena(arena_size);
	if (r_requests.is_empty()) {
		return;
	}

	_rt_update_or_grow_storage(capture_params_buffer, capture_params_capacity, capture_params.ptr(), capture_params.size() * sizeof(RTVertexCapture::DrawParams), "RT vertex capture params");
}

// Adds 50% headroom past p_size, at most MAX_HEADROOM, so ramping capture sizes don't reallocate every frame. Never
// below the minimum: kept captures reference the arena (without reading it), so it must stay valid once created.
static uint32_t _rt_capture_arena_target(uint64_t p_size) {
	constexpr uint64_t MIN_SIZE = 64 * 1024;
	constexpr uint64_t MAX_HEADROOM = 256 * 1024 * 1024;
	return uint32_t(CLAMP(p_size + MIN(p_size / 2, MAX_HEADROOM), MIN_SIZE, uint64_t(UINT32_MAX)));
}

// Makes the capture arena hold at least p_size bytes for the current view. Shrinks when the largest view of a full
// time window used under a quarter of it, so a spike doesn't hold its memory until cleanup_caches.
void RenderRaytracing::_reserve_capture_arena(uint64_t p_size) {
	constexpr uint64_t SHRINK_WINDOW_MSEC = 5000;
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	capture_arena_used = p_size;
	capture_arena_window_peak = MAX(capture_arena_window_peak, p_size);

	uint32_t capacity = capture_arena_capacity;
	if (p_size > capacity) {
		capacity = _rt_capture_arena_target(p_size);
	} else if (now - capture_arena_window_start >= SHRINK_WINDOW_MSEC) {
		if (capture_arena_window_peak * 4 < capacity) {
			capacity = _rt_capture_arena_target(capture_arena_window_peak);
		}
		capture_arena_window_start = now;
		capture_arena_window_peak = p_size;
	}
	if (capacity == capture_arena_capacity) {
		return;
	}
	_free_rid_if_valid(capture_arena);
	capture_arena_capacity = capacity;
	capture_arena = RD::get_singleton()->storage_buffer_create(capacity);
	RD::get_singleton()->set_resource_name(capture_arena, "RT vertex capture arena");
}

// Commits the capture recorded for p_surf this view to its entry. Returns the surface's binding, or null when the
// surface has no capture to trace. r_recorded is the request recorded this view; null when the entry keeps its last capture.
RenderRaytracing::CaptureBinding *RenderRaytracing::_commit_capture(const void *p_surf, LocalVector<RTCaptureRequest> &r_captures, RTCaptureRequest *&r_recorded) {
	r_recorded = nullptr;
	CaptureBinding *binding = _capture_binding(p_surf);
	if (!binding) {
		return nullptr;
	}
	RTCaptureState *capture = _capture_state(*binding);
	ERR_FAIL_NULL_V(capture, nullptr);
	if (binding->request != UINT32_MAX && r_captures[binding->request].recorded) {
		r_recorded = &r_captures[binding->request];
		r_recorded->committed = true;
		capture->frame = RSG::rasterizer->get_frame_number();
		capture->stamp = ++capture_stamp_counter;
		capture->inputs = binding->inputs;
	}
	return capture->stamp != 0 ? binding : nullptr;
}

// Fills r_source with the mesh surface's captured geometry: the capture recorded for this view, or the one its entry
// keeps. False when the surface has no capture to trace.
bool RenderRaytracing::_resolve_captured_source(const void *p_surf, LocalVector<RTCaptureRequest> &r_captures, RTDeformedGeometrySource &r_source) {
	RTCaptureRequest *recorded = nullptr;
	CaptureBinding *binding = _commit_capture(p_surf, r_captures, recorded);
	if (!binding) {
		return false;
	}
	r_source = RTDeformedGeometrySource();
	r_source.cache_handle = &binding->entry;
	if (recorded) {
		r_source.current_vb = capture_arena;
		r_source.current_vb_offset = recorded->arena_offset;
	}
	r_source.captured = true;
	r_source.force_rebuild = recorded && binding->rebuild;
	r_source.change_stamp = _capture_state(*binding)->stamp;
	r_source.cache_key = uint64_t(uintptr_t(p_surf));
	return true;
}

// Resolves a captured MultiMesh or particle surface to its merged entry: a capture recorded this view rebakes it,
// otherwise the entry keeps its last bake. False when the surface has no capture to trace.
bool RenderRaytracing::_resolve_captured_mm(const void *p_surf, LocalVector<RTCaptureRequest> &r_captures, RID &r_entry, RTSchedulerAction &r_action, RTMergeBakeInputs &r_bake) {
	RTCaptureRequest *recorded = nullptr;
	const CaptureBinding *binding = _commit_capture(p_surf, r_captures, recorded);
	if (!binding) {
		return false;
	}
	r_entry = binding->entry;
	r_action = RT_SCHEDULER_HOLD;
	if (recorded) {
		r_action = binding->rebuild ? RT_SCHEDULER_REBUILD : RT_SCHEDULER_REFIT;
	}
	r_bake.captured = true;
	r_bake.capture_offset = recorded ? recorded->arena_offset : RT_OFFSET_NONE;
	return true;
}

// Keeps the merged positions of r_entry's previous capture as its motion vector source before a new capture is baked.
void RenderRaytracing::_snapshot_captured_positions(RTMergedMMEntry &r_entry, uint32_t p_point_count) {
	const uint32_t bytes = p_point_count * 12;
	if (!r_entry.blas_built_once || !_rt_ensure_prev_pos_buffer(r_entry, bytes)) {
		return;
	}
	RD::get_singleton()->buffer_copy(r_entry.merged_vtx_buffer, r_entry.prev_pos_buffer, 0, 0, bytes);
	r_entry.motion_frame = RSG::rasterizer->get_frame_number();
}

// ---------------------------------------------------------------------------
// Surface BLAS helper (shared between static and deformed paths)
// ---------------------------------------------------------------------------

// Fills RTSurfaceData geometry metadata from the surface format.
// Returns the RIDs needed for BLAS creation so _populate_surface_blas can use them.
static void _fill_surface_geometry_data(
		void *p_mesh_surface,
		bool p_force_uncompressed,
		RTSurfaceData *r_surf_data,
		RID *r_vertex_buffer = nullptr,
		RID *r_attribute_buffer = nullptr,
		RID *r_index_buffer = nullptr) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);
	// Forcing uncompressed only affects the vertex buffer (positions and TBN); the attribute buffer keeps its format.
	bool attributes_compressed = surface_format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
	bool compressed = attributes_compressed && !p_force_uncompressed;
	bool is_2d = surface_format & RSE::ARRAY_FLAG_USE_2D_VERTICES;

	r_surf_data->is_compressed = compressed;

	RT_GeometryData &geom = r_surf_data->geometry;
	memset(&geom, 0, sizeof(geom));
	r_surf_data->sources = RTGeometrySources();

	RID vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	RID attribute_buffer = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0);

	if (r_vertex_buffer) {
		*r_vertex_buffer = vertex_buffer;
	}
	if (r_attribute_buffer) {
		*r_attribute_buffer = attribute_buffer;
	}
	if (r_index_buffer) {
		*r_index_buffer = index_buffer;
	}

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);

	geom.vertex_count = vertex_count;

	// Position stride
	uint32_t position_stride;
	if (is_2d) {
		position_stride = sizeof(float) * 2;
	} else if (compressed) {
		position_stride = sizeof(uint16_t) * 4;
	} else {
		position_stride = sizeof(float) * 3;
	}
	geom.position_stride = position_stride;

	// Normal/tangent layout
	uint32_t normal_stride;
	uint32_t tangent_stride = 0;
	geom.normal_byte_offset = RT_OFFSET_NONE;
	geom.tangent_byte_offset = RT_OFFSET_NONE;
	uint32_t current_offset = position_stride * vertex_count;

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;

	if (compressed) {
		normal_stride = sizeof(uint16_t) * 2;
		if (has_normal) {
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		}
	} else {
		if (has_normal && has_tangent) {
			normal_stride = sizeof(uint16_t) * 4;
			tangent_stride = sizeof(uint16_t) * 4;
			geom.normal_byte_offset = current_offset;
			geom.tangent_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else if (has_normal) {
			normal_stride = sizeof(uint16_t) * 2;
			geom.normal_byte_offset = current_offset;
			current_offset += normal_stride * vertex_count;
		} else {
			normal_stride = 0;
		}
	}
	geom.normal_stride = normal_stride;
	geom.tangent_stride = tangent_stride;
	geom.flags = (compressed ? RT_GEOM_FLAG_COMPRESSED : 0) | (attributes_compressed ? RT_GEOM_FLAG_COMPRESSED_UV : 0);

	if (compressed) {
		AABB surface_aabb = mesh_storage->mesh_surface_get_aabb(p_mesh_surface);
		geom.aabb_size_x = surface_aabb.size.x;
		geom.aabb_size_y = surface_aabb.size.y;
		geom.aabb_size_z = surface_aabb.size.z;
		geom.aabb_pos_x = surface_aabb.position.x;
		geom.aabb_pos_y = surface_aabb.position.y;
		geom.aabb_pos_z = surface_aabb.position.z;
	} else {
		geom.aabb_size_x = 1.0f;
		geom.aabb_size_y = 1.0f;
		geom.aabb_size_z = 1.0f;
		geom.aabb_pos_x = 0.0f;
		geom.aabb_pos_y = 0.0f;
		geom.aabb_pos_z = 0.0f;
	}

	// Attribute buffer layout
	uint32_t attrib_offset = 0;
	geom.uv_byte_offset = RT_OFFSET_NONE;
	geom.uv2_byte_offset = RT_OFFSET_NONE;
	geom.color_byte_offset = RT_OFFSET_NONE;

	if (surface_format & RSE::ARRAY_FORMAT_COLOR) {
		geom.color_byte_offset = attrib_offset;
		attrib_offset += sizeof(uint32_t);
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV) {
		geom.uv_byte_offset = attrib_offset;
		attrib_offset += attributes_compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	if (surface_format & RSE::ARRAY_FORMAT_TEX_UV2) {
		geom.uv2_byte_offset = attrib_offset;
		attrib_offset += attributes_compressed ? sizeof(uint16_t) * 2 : sizeof(float) * 2;
	}
	for (int ci = 0; ci < RSE::ARRAY_CUSTOM_COUNT; ci++) {
		const uint32_t fmt_shift[RSE::ARRAY_CUSTOM_COUNT] = { RSE::ARRAY_FORMAT_CUSTOM0_SHIFT, RSE::ARRAY_FORMAT_CUSTOM1_SHIFT, RSE::ARRAY_FORMAT_CUSTOM2_SHIFT, RSE::ARRAY_FORMAT_CUSTOM3_SHIFT };
		if (surface_format & (1ULL << (RSE::ARRAY_CUSTOM0 + ci))) {
			uint32_t fmt = (surface_format >> fmt_shift[ci]) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
			const uint32_t fmtsize[RSE::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
			attrib_offset += fmtsize[fmt];
		}
	}
	geom.attribute_stride = attrib_offset;

	// UV scale (fp16 packed, matches GLSL unpackHalf2x16)
	Vector4 uv_scale = mesh_storage->mesh_surface_get_uv_scale(p_mesh_surface);
	geom.uv_scale_packed = (uint32_t(Math::make_half_float(uv_scale.y)) << 16) | Math::make_half_float(uv_scale.x);
	geom.uv2_scale_packed = (uint32_t(Math::make_half_float(uv_scale.w)) << 16) | Math::make_half_float(uv_scale.z);

	// Index format (no device address — caller fills those in)
	if (index_buffer.is_valid() && index_count > 0) {
		bool is_16bit = vertex_count <= 65536 && vertex_count > 0;
		geom.index_format = is_16bit ? RT_INDEX_FORMAT_UINT16 : RT_INDEX_FORMAT_UINT32;
		geom.primitive_count = index_count / 3;
	} else {
		geom.index_format = RT_INDEX_FORMAT_NONE;
		geom.primitive_count = vertex_count / 3;
	}
}

// Fills the geometry metadata, device addresses and BLAS triangle geometry of one mesh surface.
// Returns false when the surface has no vertex buffer to build from.
static bool _fill_surface_blas_geometry(
		void *p_mesh_surface,
		RID p_vertex_buffer_override,
		RID p_index_buffer_override,
		bool p_force_uncompressed,
		RTSurfaceData *r_surf_data,
		RD::AccelerationStructureGeometry &r_as_geom) {
	RID vertex_buffer, attribute_buffer, index_buffer;
	_fill_surface_geometry_data(p_mesh_surface, p_force_uncompressed, r_surf_data,
			&vertex_buffer, &attribute_buffer, &index_buffer);

	if (p_vertex_buffer_override.is_valid()) {
		vertex_buffer = p_vertex_buffer_override;
	}
	if (p_index_buffer_override.is_valid()) {
		index_buffer = p_index_buffer_override;
	}
	if (!vertex_buffer.is_valid()) {
		return false;
	}

	RT_GeometryData &geom = r_surf_data->geometry;
	RTGeometrySources &sources = r_surf_data->sources;

	geom.vertex_buffer_address = _rt_buffer_address(vertex_buffer, sources.vertex);
	if (attribute_buffer.is_valid()) {
		geom.attribute_buffer_address = _rt_buffer_address(attribute_buffer, sources.attribute);
	}
	if (index_buffer.is_valid() && geom.index_format != RT_INDEX_FORMAT_NONE) {
		geom.index_buffer_address = _rt_buffer_address(index_buffer, sources.index);
	}

	const bool is_2d = RendererRD::MeshStorage::get_singleton()->mesh_surface_get_format(p_mesh_surface) & RSE::ARRAY_FLAG_USE_2D_VERTICES;
	RD::DataFormat pos_format;
	if (is_2d) {
		pos_format = RD::DATA_FORMAT_R32G32_SFLOAT;
	} else if (r_surf_data->is_compressed) {
		pos_format = RD::DATA_FORMAT_R16G16B16A16_UNORM;
	} else {
		pos_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
	}

	r_as_geom = RD::AccelerationStructureGeometry();
	r_as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
	r_as_geom.geometry.triangles.vertex_buffer = vertex_buffer;
	r_as_geom.geometry.triangles.vertex_stride = geom.position_stride;
	r_as_geom.geometry.triangles.vertex_count = geom.vertex_count;
	r_as_geom.geometry.triangles.vertex_format = pos_format;

	const uint32_t index_count = (geom.index_format != RT_INDEX_FORMAT_NONE) ? geom.primitive_count * 3 : 0;
	if (index_buffer.is_valid() && index_count > 0) {
		r_as_geom.geometry.triangles.index_buffer = index_buffer;
		r_as_geom.geometry.triangles.index_count = index_count;
	}
	return true;
}

void RenderRaytracing::_populate_surface_blas(
		void *p_mesh_surface,
		RID p_vertex_buffer_override,
		RID p_index_buffer_override,
		bool p_force_uncompressed,
		bool p_prefer_fast_build,
		bool p_allow_update,
		uint32_t p_cache_key,
		RTSurfaceData *r_surf_data,
		LocalVector<RID> &r_dirty_blas_list) {
	blas_transform_pool.release(r_surf_data->blas_transform);
	RD::AccelerationStructureGeometry as_geom;
	if (!_fill_surface_blas_geometry(p_mesh_surface, p_vertex_buffer_override, p_index_buffer_override, p_force_uncompressed, r_surf_data, as_geom)) {
		return;
	}

	const String name = String(p_vertex_buffer_override.is_valid() ? "RT BLAS deformed [" : "RT BLAS [") + itos(p_cache_key) + "]";
	_assign_compression_transforms(blas_transform_pool, &r_surf_data->geometry, &as_geom, &r_surf_data->blas_transform, 1);
	r_surf_data->blas = _create_blas({ &as_geom, 1 }, p_prefer_fast_build, p_allow_update, name, r_dirty_blas_list);
	if (!r_surf_data->blas.is_valid()) {
		_free_blas_resources(blas_transform_pool, r_surf_data->blas, &r_surf_data->blas_transform, 1);
	}
}

// ---------------------------------------------------------------------------
// Uniform packing (file-local helpers)
// ---------------------------------------------------------------------------

static float _def_real(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? u.default_value[idx].real : 0.0f;
}

static int32_t _def_sint(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? u.default_value[idx].sint : 0;
}

static uint32_t _def_uint(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? u.default_value[idx].uint : 0u;
}

static uint32_t _def_bool(const ShaderLanguage::ShaderNode::Uniform &u, int idx) {
	return (int)u.default_value.size() > idx ? (uint32_t)u.default_value[idx].boolean : 0u;
}

// Match raster UBO fill
static bool _val_is_scalar(const Variant &val) {
	return val.get_type() == Variant::FLOAT || val.get_type() == Variant::INT || val.get_type() == Variant::BOOL;
}

static void pack_uniform(const ShaderLanguage::ShaderNode::Uniform &u, const Variant &val, uint8_t *dst) {
	using SL = ShaderLanguage;

	switch (u.type) {
		case SL::TYPE_FLOAT: {
			float v = _val_is_scalar(val) ? (float)(double)val : _def_real(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_INT: {
			int32_t v = _val_is_scalar(val) ? (int32_t)(int64_t)val : _def_sint(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_UINT: {
			uint32_t v = _val_is_scalar(val) ? (uint32_t)(int64_t)val : _def_uint(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_BOOL: {
			uint32_t v = _val_is_scalar(val) ? (uint32_t)(bool)val : _def_bool(u, 0);
			memcpy(dst, &v, 4);
		} break;
		case SL::TYPE_VEC2: {
			float fv[2];
			if (val.get_type() == Variant::VECTOR2 || val.get_type() == Variant::VECTOR2I) {
				Vector2 v = val;
				fv[0] = (float)v.x;
				fv[1] = (float)v.y;
			} else {
				fv[0] = _def_real(u, 0);
				fv[1] = _def_real(u, 1);
			}
			memcpy(dst, fv, 8);
		} break;
		case SL::TYPE_VEC3: {
			float fv[3] = {};
			if (val.get_type() == Variant::VECTOR3) {
				Vector3 v = val;
				fv[0] = (float)v.x;
				fv[1] = (float)v.y;
				fv[2] = (float)v.z;
			} else if (val.get_type() == Variant::COLOR) {
				Color c = val;
				if (u.hint == SL::ShaderNode::Uniform::HINT_SOURCE_COLOR) {
					c = c.srgb_to_linear();
				}
				fv[0] = c.r;
				fv[1] = c.g;
				fv[2] = c.b;
			} else {
				fv[0] = _def_real(u, 0);
				fv[1] = _def_real(u, 1);
				fv[2] = _def_real(u, 2);
			}
			memcpy(dst, fv, 12);
		} break;
		case SL::TYPE_VEC4: {
			float fv[4] = {};
			if (val.get_type() == Variant::COLOR) {
				Color c = val;
				if (u.hint == SL::ShaderNode::Uniform::HINT_SOURCE_COLOR) {
					c = c.srgb_to_linear();
				}
				fv[0] = c.r;
				fv[1] = c.g;
				fv[2] = c.b;
				fv[3] = c.a;
			} else if (val.get_type() == Variant::VECTOR4) {
				Vector4 v = val;
				fv[0] = (float)v.x;
				fv[1] = (float)v.y;
				fv[2] = (float)v.z;
				fv[3] = (float)v.w;
			} else {
				for (int i = 0; i < 4; i++) {
					fv[i] = _def_real(u, i);
				}
			}
			memcpy(dst, fv, 16);
		} break;
		case SL::TYPE_IVEC2: {
			int32_t iv[2];
			if (val.get_type() == Variant::VECTOR2I) {
				Vector2i v = val;
				iv[0] = v.x;
				iv[1] = v.y;
			} else {
				iv[0] = _def_sint(u, 0);
				iv[1] = _def_sint(u, 1);
			}
			memcpy(dst, iv, 8);
		} break;
		case SL::TYPE_IVEC3: {
			int32_t iv[3] = {};
			if (val.get_type() == Variant::VECTOR3I) {
				Vector3i v = val;
				iv[0] = v.x;
				iv[1] = v.y;
				iv[2] = v.z;
			} else {
				for (int i = 0; i < 3; i++) {
					iv[i] = _def_sint(u, i);
				}
			}
			memcpy(dst, iv, 12);
		} break;
		case SL::TYPE_IVEC4: {
			int32_t iv[4] = {};
			if (val.get_type() == Variant::VECTOR4I) {
				Vector4i v = val;
				iv[0] = v.x;
				iv[1] = v.y;
				iv[2] = v.z;
				iv[3] = v.w;
			} else {
				for (int i = 0; i < 4; i++) {
					iv[i] = _def_sint(u, i);
				}
			}
			memcpy(dst, iv, 16);
		} break;
		case SL::TYPE_UVEC2: {
			uint32_t uv[2];
			if (val.get_type() == Variant::VECTOR2I) {
				Vector2i v = val;
				uv[0] = (uint32_t)v.x;
				uv[1] = (uint32_t)v.y;
			} else {
				uv[0] = _def_uint(u, 0);
				uv[1] = _def_uint(u, 1);
			}
			memcpy(dst, uv, 8);
		} break;
		case SL::TYPE_UVEC3: {
			uint32_t uv[3] = {};
			if (val.get_type() == Variant::VECTOR3I) {
				Vector3i v = val;
				uv[0] = (uint32_t)v.x;
				uv[1] = (uint32_t)v.y;
				uv[2] = (uint32_t)v.z;
			} else {
				for (int i = 0; i < 3; i++) {
					uv[i] = _def_uint(u, i);
				}
			}
			memcpy(dst, uv, 12);
		} break;
		case SL::TYPE_UVEC4: {
			uint32_t uv[4] = {};
			if (val.get_type() == Variant::VECTOR4I) {
				Vector4i v = val;
				uv[0] = (uint32_t)v.x;
				uv[1] = (uint32_t)v.y;
				uv[2] = (uint32_t)v.z;
				uv[3] = (uint32_t)v.w;
			} else {
				for (int i = 0; i < 4; i++) {
					uv[i] = _def_uint(u, i);
				}
			}
			memcpy(dst, uv, 16);
		} break;
		case SL::TYPE_BVEC2: {
			uint32_t bv[2] = { _def_bool(u, 0), _def_bool(u, 1) };
			memcpy(dst, bv, 8);
		} break;
		case SL::TYPE_BVEC3: {
			uint32_t bv[3] = { _def_bool(u, 0), _def_bool(u, 1), _def_bool(u, 2) };
			memcpy(dst, bv, 12);
		} break;
		case SL::TYPE_BVEC4: {
			uint32_t bv[4] = { _def_bool(u, 0), _def_bool(u, 1), _def_bool(u, 2), _def_bool(u, 3) };
			memcpy(dst, bv, 16);
		} break;
		case SL::TYPE_MAT2: {
			// std140: mat2 = 2 column vec2s, each padded to vec4 (2x16 = 32 bytes).
			float m[8] = {};
			if (val.get_type() == Variant::TRANSFORM2D) {
				Transform2D t = val;
				m[0] = (float)t[0].x;
				m[1] = (float)t[0].y;
				m[4] = (float)t[1].x;
				m[5] = (float)t[1].y;
			} else {
				for (int i = 0; i < 4; i++) {
					m[(i / 2) * 4 + (i % 2)] = _def_real(u, i);
				}
			}
			memcpy(dst, m, 32);
		} break;
		case SL::TYPE_MAT3: {
			// std140: mat3 = 3 column vec3s, each padded to vec4 (3x16 = 48 bytes).
			float m[12] = {};
			if (val.get_type() == Variant::BASIS) {
				Basis b = val;
				for (int col = 0; col < 3; col++) {
					Vector3 c = b.get_column(col);
					m[col * 4 + 0] = (float)c.x;
					m[col * 4 + 1] = (float)c.y;
					m[col * 4 + 2] = (float)c.z;
				}
			} else {
				for (int i = 0; i < 9; i++) {
					m[(i / 3) * 4 + (i % 3)] = _def_real(u, i);
				}
			}
			memcpy(dst, m, 48);
		} break;
		case SL::TYPE_MAT4: {
			// std140: mat4 = 4 column vec4s (4x16 = 64 bytes).
			float m[16] = {};
			if (val.get_type() == Variant::PROJECTION) {
				Projection p = val;
				for (int col = 0; col < 4; col++) {
					m[col * 4 + 0] = (float)p.columns[col].x;
					m[col * 4 + 1] = (float)p.columns[col].y;
					m[col * 4 + 2] = (float)p.columns[col].z;
					m[col * 4 + 3] = (float)p.columns[col].w;
				}
			} else if (val.get_type() == Variant::TRANSFORM3D) {
				Transform3D t = val;
				Projection p(t);
				for (int col = 0; col < 4; col++) {
					m[col * 4 + 0] = (float)p.columns[col].x;
					m[col * 4 + 1] = (float)p.columns[col].y;
					m[col * 4 + 2] = (float)p.columns[col].z;
					m[col * 4 + 3] = (float)p.columns[col].w;
				}
			} else {
				for (int i = 0; i < 16; i++) {
					m[i] = _def_real(u, i);
				}
			}
			memcpy(dst, m, 64);
		} break;
		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Procedural geometry processing
// ---------------------------------------------------------------------------

void RenderRaytracing::update_procedural_blas(RTProceduralState *p_state, LocalVector<RID> &r_dirty_blas_list) {
	// Pack AABB data into a byte buffer.
	Vector<uint8_t> aabb_bytes;
	uint32_t aabb_count = 1;

	if (p_state->aabb_data.size() >= 6 && (p_state->aabb_data.size() % 6) == 0) {
		aabb_count = p_state->aabb_data.size() / 6;
		aabb_bytes.resize(p_state->aabb_data.size() * sizeof(float));
		memcpy(aabb_bytes.ptrw(), p_state->aabb_data.ptr(), aabb_bytes.size());
	} else {
		const AABB &a = p_state->culling_aabb;
		float single[6] = {
			(float)a.position.x, (float)a.position.y, (float)a.position.z,
			(float)(a.position.x + a.size.x), (float)(a.position.y + a.size.y), (float)(a.position.z + a.size.z)
		};
		aabb_bytes.resize(sizeof(single));
		memcpy(aabb_bytes.ptrw(), single, sizeof(single));
	}

	uint32_t required_bytes = aabb_bytes.size();
	bool needs_new_blas = false;

	// Grow-only: only recreate the buffer when capacity is exceeded or count changed.
	if (required_bytes > p_state->gpu_buffer_capacity || aabb_count != p_state->aabb_count) {
		_free_rid_if_valid(p_state->blas);
		_free_rid_if_valid(p_state->gpu_buffer);
		p_state->gpu_buffer = RD::get_singleton()->storage_buffer_create(required_bytes, aabb_bytes,
				0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
		RD::get_singleton()->set_resource_name(p_state->gpu_buffer, "RT procedural AABB buffer");
		p_state->gpu_buffer_capacity = required_bytes;
		p_state->aabb_count = aabb_count;
		needs_new_blas = true;
	} else {
		// Buffer is large enough -- just update contents.
		RD::get_singleton()->buffer_update(p_state->gpu_buffer, 0, required_bytes, aabb_bytes.ptr());
		needs_new_blas = !p_state->blas.is_valid();
	}

	if (needs_new_blas) {
		ERR_FAIL_COND(!p_state->gpu_buffer.is_valid());

		RD::AccelerationStructureGeometry geom;
		geom.type = RD::AccelerationStructureGeometry::TYPE_AABBS;
		geom.geometry.aabbs.buffer = p_state->gpu_buffer;
		geom.geometry.aabbs.count = aabb_count;
		geom.geometry.aabbs.stride = 24; // VkAabbPositionsKHR: two float3 (min, max).
		p_state->blas = RD::get_singleton()->blas_create({ &geom, 1 }, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		RD::get_singleton()->set_resource_name(p_state->blas, "RT procedural BLAS");
	}

	// BDA for shader access.
	if (p_state->expose_bounds && p_state->gpu_buffer.is_valid()) {
		p_state->gpu_buffer_address = RD::get_singleton()->buffer_get_device_address(p_state->gpu_buffer);
	} else {
		p_state->gpu_buffer_address = 0;
	}

	if (p_state->blas.is_valid()) {
		r_dirty_blas_list.push_back(p_state->blas);
	}
}

// ---------------------------------------------------------------------------
// Material processing
// ---------------------------------------------------------------------------

// Traced for surfaces without a material.
static RTMaterialData &_rt_default_material() {
	static RTMaterialData s_default_mat = [] {
		RTMaterialData mat;
		mat.data = rt_material_data_default();
		return mat;
	}();
	return s_default_mat;
}

const RTMaterialCacheEntry *RenderRaytracing::_find_material_cache_entry(uint32_t p_slot) const {
	const uint32_t chunk_idx = p_slot >> RT_CACHE_CHUNK_SHIFT;
	if (chunk_idx >= uint32_t(material_chunks.size()) || !material_chunks[chunk_idx]) {
		return nullptr;
	}
	return &material_chunks[chunk_idx][p_slot & RT_CACHE_CHUNK_MASK];
}

void RenderRaytracing::_touch_material_entry(RTMaterialCacheEntry &r_entry, uint32_t p_slot, uint32_t p_frame) {
	if (r_entry.last_used_frame != p_frame && r_entry.ptr->is_custom_shader) {
		custom_material_slots.push_back(p_slot);
	}
	r_entry.last_used_frame = p_frame;
}

// A hit group slot only changes when the shader source is first registered, so checking once per frame catches it.
// False when the slot changed, which invalidates the cached UBO layout and BDA.
bool RenderRaytracing::_custom_shader_slot_current(RTMaterialCacheEntry &r_entry, RID p_material_rid, uint32_t p_frame) {
	const uint32_t shader_id = RendererRD::MaterialStorage::get_singleton()->material_get_shader_id(p_material_rid);
	if (SceneShaderRaytracing::get_singleton()->register_custom_shader(shader_id, p_material_rid) != r_entry.ptr->rt_sbt_offset) {
		material_slot_version++;
		return false;
	}
	r_entry.slot_checked_frame = p_frame;
	return true;
}

void RenderRaytracing::_refresh_custom_shader_slots() {
	const uint32_t frame = RSG::rasterizer->get_frame_number();
	if (frame == material_frame) {
		return;
	}
	material_frame = frame;
	SWAP(custom_material_slots, custom_material_slots_prev);
	custom_material_slots.clear();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	for (uint32_t slot : custom_material_slots_prev) {
		RTMaterialCacheEntry *entry = get_material_cache_entry(slot);
		if (!entry->ptr || !entry->ptr->is_custom_shader || entry->slot_checked_frame == frame) {
			continue;
		}
		// Stale entries are rebuilt on use, which registers the shader again.
		const RID material = RID::from_uint64((uint64_t(entry->cached_rid_version) << 32) | slot);
		if (!material_storage->owns_material(material) || material_storage->material_get_rt_invalidation_counter(material) != entry->cached_counter) {
			continue;
		}
		// A changed slot stays unchecked, so the next use rebuilds the entry like process_material always did.
		_custom_shader_slot_current(*entry, material, frame);
	}
}

const RTMaterialData *RenderRaytracing::lookup_material(RID p_material_rid, uint16_t p_material_invalidation_counter, uint32_t &r_slot) const {
	if (!p_material_rid.is_valid()) {
		r_slot = UINT32_MAX;
		return &_rt_default_material();
	}
	r_slot = get_rid_index(p_material_rid);
	const RTMaterialCacheEntry *entry = _find_material_cache_entry(r_slot);
	if (!entry || !entry->ptr || entry->cached_rid_version != get_rid_version(p_material_rid) || entry->cached_counter != p_material_invalidation_counter) {
		return nullptr;
	}
	const bool slot_checked = !entry->ptr->is_custom_shader || entry->slot_checked_frame == RSG::rasterizer->get_frame_number();
	return slot_checked ? entry->ptr : nullptr;
}

RTMaterialData *RenderRaytracing::touch_material(uint32_t p_slot) {
	if (p_slot == UINT32_MAX) {
		return &_rt_default_material();
	}
	RTMaterialCacheEntry *entry = get_material_cache_entry(p_slot);
	_touch_material_entry(*entry, p_slot, RSG::rasterizer->get_frame_number());
	return entry->ptr;
}

RTMaterialData *RenderRaytracing::process_material(RID p_material_rid, uint16_t p_material_invalidation_counter) {
	if (!p_material_rid.is_valid()) {
		return &_rt_default_material();
	}

	// Cache lookup
	uint32_t mat_idx = get_rid_index(p_material_rid);
	uint32_t mat_version = get_rid_version(p_material_rid);
	RTMaterialCacheEntry *entry = get_material_cache_entry(mat_idx);

	uint32_t current_frame = RSG::rasterizer->get_frame_number();
	bool needs_refresh = !entry->ptr ||
			entry->cached_rid_version != mat_version ||
			entry->cached_counter != p_material_invalidation_counter;

	if (!needs_refresh) {
		_touch_material_entry(*entry, mat_idx, current_frame);
		if (entry->ptr->is_custom_shader && entry->slot_checked_frame != current_frame && !_custom_shader_slot_current(*entry, p_material_rid, current_frame)) {
			needs_refresh = true;
		}
		if (!needs_refresh) {
			return entry->ptr;
		}
	}

	// Cache miss - need to rebuild material
	if (!entry->ptr) {
		entry->ptr = memnew(RTMaterialData);
		entry->ptr->table_index = mat_idx + 1;
	}

	RTMaterialData *mat_data = entry->ptr;
	RT_MaterialData &mat = mat_data->data;

	mat = rt_material_data_default();

	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	// Helper lambda to get texture from material parameter
	// p_srgb should be true for color textures (albedo, emission) that need sRGB->linear conversion
	auto get_material_texture = [&](const StringName &p_param, bool p_srgb = false) -> RID {
		Variant tex_var = material_storage->material_get_param(p_material_rid, p_param);
		if (tex_var.get_type() == Variant::OBJECT || tex_var.get_type() == Variant::RID) {
			RID tex_rid = tex_var;
			if (tex_rid.is_valid()) {
				return texture_storage->texture_get_rd_texture(tex_rid, p_srgb);
			}
		}
		return RID();
	};

	// Textures
	// Albedo is a color texture - needs sRGB->linear conversion
	RID albedo_rd = get_material_texture("texture_albedo", true);
	if (albedo_rd.is_valid()) {
		mat.albedo_texture.index = bindless_block->add_texture(albedo_rd);
	}

	RID normal_rd = get_material_texture("texture_normal");
	if (normal_rd.is_valid()) {
		mat.normal_texture.index = bindless_block->add_texture(normal_rd);
		mat.flags |= RT_MAT_FLAG_HAS_NORMAL_MAP;

		Variant normal_scale_var = material_storage->material_get_param(p_material_rid, "normal_scale");
		if (normal_scale_var.get_type() == Variant::FLOAT) {
			mat.normal_map_depth = normal_scale_var;
		}
	}

	// Feature switches packed by BaseMaterial3D::_update_shader. Absent = not a BaseMaterial3D.
	const Variant rt_state_var = material_storage->material_get_param(p_material_rid, RTBaseMaterialState::PARAM_NAME);
	const bool is_base_material = rt_state_var.get_type() == Variant::INT;
	const RTBaseMaterialState rt_state = is_base_material ? RTBaseMaterialState::decode(rt_state_var) : RTBaseMaterialState();

	auto get_texture_idx = [&](const StringName &p_param) -> uint32_t {
		RID tex_rd = get_material_texture(p_param);
		return tex_rd.is_valid() ? bindless_block->add_texture(tex_rd) : 0u; // 0 is the white default.
	};
	auto get_mask_channel = [&](const StringName &p_param) -> RTTextureChannel {
		Variant v = material_storage->material_get_param(p_material_rid, p_param);
		if (v.get_type() != Variant::VECTOR4) {
			return RT_TEXTURE_CHANNEL_RED;
		}
		Vector4 mask = v;
		return rt_texture_channel_from_mask(mask.x, mask.y, mask.z, mask.w);
	};
	auto set_texture = [](auto &r_texture, uint32_t p_index, RTTextureChannel p_channel) {
		r_texture.index = p_index;
		r_texture.channel = p_channel;
	};

	// Roughness, metallic and AO, sampled like BaseMaterial3D's generated fragment code.
	const bool ao_enabled = rt_state.ambient_occlusion;
	if (rt_state.orm) {
		const uint32_t orm_idx = get_texture_idx("texture_orm");
		set_texture(mat.roughness_texture, orm_idx, RT_TEXTURE_CHANNEL_GREEN);
		set_texture(mat.metallic_texture, orm_idx, RT_TEXTURE_CHANNEL_BLUE);
		if (ao_enabled) {
			set_texture(mat.ao_texture, orm_idx, RT_TEXTURE_CHANNEL_RED);
		}
	} else {
		const RTTextureChannel roughness_channel = RTTextureChannel(MIN(uint32_t(rt_state.roughness_texture_channel), uint32_t(RT_TEXTURE_CHANNEL_GRAYSCALE)));
		set_texture(mat.roughness_texture, get_texture_idx("texture_roughness"), roughness_channel);
		set_texture(mat.metallic_texture, get_texture_idx("texture_metallic"), get_mask_channel("metallic_texture_channel"));
		if (ao_enabled) {
			set_texture(mat.ao_texture, get_texture_idx("texture_ambient_occlusion"), get_mask_channel("ao_texture_channel"));
		}
	}

	// BaseMaterial3D keeps emission params populated with the feature disabled.
	const bool emission_enabled = !is_base_material || rt_state.emission;

	// Emission is a color texture - needs sRGB->linear conversion
	RID emission_rd = emission_enabled ? get_material_texture("texture_emission", true) : RID();
	if (emission_rd.is_valid()) {
		mat.emission_texture.index = bindless_block->add_texture(emission_rd);
		mat.flags |= RT_MAT_FLAG_HAS_EMISSION_TEX;
		// Set sensible defaults for emission when texture is present
		mat.emission_color[0] = 1.0f;
		mat.emission_color[1] = 1.0f;
		mat.emission_color[2] = 1.0f;
		mat.emission_strength = 1.0f;
	}

	// Material properties
	// Colors declared with source_color in Godot shaders are stored in sRGB;
	// material_get_param returns the raw sRGB value, so we convert to linear here.
	Variant albedo_var = material_storage->material_get_param(p_material_rid, "albedo");
	if (albedo_var.get_type() == Variant::COLOR) {
		Color c = ((Color)albedo_var).srgb_to_linear();
		mat.albedo_color[0] = c.r;
		mat.albedo_color[1] = c.g;
		mat.albedo_color[2] = c.b;
		mat.albedo_color[3] = c.a;
	}

	const bool is_builtin_standard = material_storage->material_is_builtin_standard_3d(p_material_rid);
	mat_data->is_custom_shader = !is_builtin_standard || material_storage->material_requires_custom_rt_hit_group(p_material_rid);

	if (!mat_data->is_custom_shader) {
		mat_data->rt_sbt_offset = 0;
		// Switched away from a custom shader: uniform storage is dead.
		if (mat_data->uniform_pool_slot != UINT32_MAX) {
			mat_ubo_pool_release(mat_data->uniform_pool_slot);
			mat_data->uniform_pool_slot = UINT32_MAX;
		}
		_mat_free_dedicated_buffer(mat_data);
	} else {
		uint32_t shader_id = material_storage->material_get_shader_id(p_material_rid);
		mat_data->rt_sbt_offset = SceneShaderRaytracing::get_singleton()->register_custom_shader(shader_id, p_material_rid);

		const SceneShaderRaytracing::CustomShaderEntry *cse =
				SceneShaderRaytracing::get_singleton()->get_custom_shader_entry(mat_data->rt_sbt_offset);
		if (!is_builtin_standard && (!cse || cse->alpha_texture_buffer_offset == UINT32_MAX)) {
			mat.flags |= RT_MAT_FLAG_NO_RAY_QUERY_ALPHA;
		}
		if (cse && cse->uniform_total_size > 0) {
			Vector<uint8_t> ubo_data;
			ubo_data.resize(cse->uniform_total_size);
			memset(ubo_data.ptrw(), 0, cse->uniform_total_size);

			for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &kv : cse->uniforms) {
				const ShaderLanguage::ShaderNode::Uniform &u = kv.value;
				if (!SceneShaderRaytracing::is_material_buffer_uniform(u, cse->uniform_offsets)) {
					continue;
				}

				uint32_t offset = cse->uniform_offsets[u.order];
				uint32_t size = ShaderLanguage::get_datatype_size(u.type);
				if (offset + size > cse->uniform_total_size) {
					continue;
				}

				uint8_t *dst = ubo_data.ptrw() + offset;

				if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
					int32_t idx = material_storage->global_shader_uniform_get_buffer_index(kv.key);
					uint32_t uidx = (idx >= 0) ? (uint32_t)idx : 0;
					memcpy(dst, &uidx, sizeof(uint32_t));
				} else {
					Variant val = material_storage->material_get_param(p_material_rid, kv.key);
					pack_uniform(u, val, dst);
				}
			}

			RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
			for (int ti = 0; ti < cse->texture_uniforms.size(); ti++) {
				const SceneShaderRaytracing::TextureUniformInfo &tui = cse->texture_uniforms[ti];
				uint32_t bindless_idx = 0;

				if (tui.is_global) {
					RID tex_rid = material_storage->global_shader_uniform_get_texture(tui.name);
					if (tex_rid.is_valid()) {
						RID rd_tex = ts->texture_get_rd_texture(tex_rid, tui.use_color);
						if (rd_tex.is_valid()) {
							bindless_idx = bindless_block->add_texture(rd_tex);
						}
					}
				} else {
					Variant tex_var = material_storage->material_get_param(p_material_rid, tui.name);
					if (tex_var.get_type() == Variant::OBJECT || tex_var.get_type() == Variant::RID) {
						RID tex_rid = tex_var;
						if (tex_rid.is_valid()) {
							RID rd_tex = ts->texture_get_rd_texture(tex_rid, tui.use_color);
							if (rd_tex.is_valid()) {
								bindless_idx = bindless_block->add_texture(rd_tex);
							}
						}
					}
				}

				if (bindless_idx == 0 && tui.hint != ShaderLanguage::ShaderNode::Uniform::HINT_NONE) {
					using Hint = ShaderLanguage::ShaderNode::Uniform::Hint;
					RID default_tex;
					switch (tui.hint) {
						case Hint::HINT_DEFAULT_BLACK:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
							break;
						case Hint::HINT_DEFAULT_TRANSPARENT:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_TRANSPARENT);
							break;
						case Hint::HINT_NORMAL:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_NORMAL);
							break;
						case Hint::HINT_ANISOTROPY:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_ANISO);
							break;
						default:
							default_tex = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
							break;
					}
					if (default_tex.is_valid()) {
						bindless_idx = bindless_block->add_texture(default_tex);
					}
				}

				RT_VALIDATE(rt_validation().check_texture_slot(tui.name, tui.buffer_offset, cse->uniform_total_size, bindless_idx,
						bindless_block ? bindless_block->get_texture_count() : 0));
				if (tui.buffer_offset + 4 <= cse->uniform_total_size) {
					memcpy(ubo_data.ptrw() + tui.buffer_offset, &bindless_idx, 4);
				}
			}

			// If the shader declared hint_alpha, mirror that bindless index into
			// mat.albedo_texture so ray_query_alpha_test() can sample it cheaply.
			if (cse->alpha_texture_buffer_offset != UINT32_MAX &&
					cse->alpha_texture_buffer_offset + 4 <= cse->uniform_total_size) {
				uint32_t alpha_idx = 0;
				memcpy(&alpha_idx, ubo_data.ptr() + cse->alpha_texture_buffer_offset, 4);
				mat.albedo_texture.index = alpha_idx;
			}

			// Try the suballoc pool first. Common materials (UBO <= slot size)
			// just buffer_update an existing slot - O(1), no driver allocation,
			// no per-frame storage_buffer_create cost.
			bool used_pool = false;
			if (cse->uniform_total_size <= MAT_UBO_POOL_SLOT_SIZE) {
				if (mat_data->uniform_pool_slot == UINT32_MAX) {
					mat_data->uniform_pool_slot = mat_ubo_pool_allocate();
				}
				if (mat_data->uniform_pool_slot != UINT32_MAX) {
					// Transitioning from a dedicated buffer back into the pool.
					_mat_free_dedicated_buffer(mat_data);
					mat_ubo_pool_update(mat_data->uniform_pool_slot, ubo_data.ptr(), cse->uniform_total_size);
					mat.uniform_address = mat_ubo_pool_get_address(mat_data->uniform_pool_slot);
					used_pool = true;
				}
			}

			if (!used_pool) {
				// Oversized or pool exhausted: dedicated per-material buffer.
				// This is the slow path: a per-material storage_buffer_create on
				// every rebuild. Warn once so it's visible in the log; the fix is
				// either to shrink the material's uniform footprint below
				// MAT_UBO_POOL_SLOT_SIZE or to grow the pool slot/capacity.
				const char *reason = (cse->uniform_total_size > MAT_UBO_POOL_SLOT_SIZE)
						? "uniform size exceeds slot"
						: "pool exhausted";
				WARN_PRINT_ONCE(vformat(
						"RT Material UBO falling back to dedicated buffer (%s): "
						"sbt_offset=%u, uniform_total_size=%u, slot_size=%u.",
						String(reason), mat_data->rt_sbt_offset,
						cse->uniform_total_size, MAT_UBO_POOL_SLOT_SIZE));

				if (mat_data->uniform_pool_slot != UINT32_MAX) {
					mat_ubo_pool_release(mat_data->uniform_pool_slot);
					mat_data->uniform_pool_slot = UINT32_MAX;
				}
				_mat_free_dedicated_buffer(mat_data);
				mat_data->uniform_buffer = RD::get_singleton()->storage_buffer_create(cse->uniform_total_size, ubo_data, 0, RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT);
				RD::get_singleton()->set_resource_name(mat_data->uniform_buffer, String("RT Material UBO [sbt=") + itos(mat_data->rt_sbt_offset) + "]");
				mat.uniform_address = RD::get_singleton()->buffer_get_device_address(mat_data->uniform_buffer);
			}
		}
	}

	auto get_float = [&](const StringName &p_param, float p_default) -> float {
		const Variant v = material_storage->material_get_param(p_material_rid, p_param);
		return v.get_type() == Variant::FLOAT ? float(v) : p_default;
	};
	// ORMMaterial3D uses the texture values directly, without the scalar multipliers.
	rt_material_set_surface_params(mat, rt_state.orm ? 1.0f : get_float("roughness", 1.0f),
			rt_state.orm ? 1.0f : get_float("metallic", 0.0f), get_float("specular", 0.5f));
	// Also weights unshaded albedo, so it is read with the emission feature disabled.
	mat.emission_indirect = get_float("emission_indirect_energy", 1.0f);

	// Addressing stored in fp16 slots, packed once below.
	float uv1_blend_sharpness = 1.0f;
	float uv2_blend_sharpness = 1.0f;
	Vector3 uv2_scale(1, 1, 1);
	Vector3 uv2_offset;

	if (emission_enabled) {
		Variant emission_var = material_storage->material_get_param(p_material_rid, "emission");
		if (emission_var.get_type() == Variant::COLOR) {
			Color c = ((Color)emission_var).srgb_to_linear();
			mat.emission_color[0] = c.r;
			mat.emission_color[1] = c.g;
			mat.emission_color[2] = c.b;
		}

		Variant emission_energy_var = material_storage->material_get_param(p_material_rid, "emission_energy");
		if (emission_energy_var.get_type() == Variant::FLOAT) {
			mat.emission_strength = emission_energy_var;
		}

		mat.flags |= rt_material_emission_flags(rt_state.emission_add, rt_state.emission_on_uv2, rt_state.uv2_triplanar, rt_state.uv2_world_triplanar);

		if ((mat.flags & RT_MAT_FLAG_EMISSION_UV2) && (rt_state.uv2_triplanar || rt_state.uv2_vertex_transform)) {
			Variant uv2_scale_var = material_storage->material_get_param(p_material_rid, "uv2_scale");
			Variant uv2_offset_var = material_storage->material_get_param(p_material_rid, "uv2_offset");
			if (uv2_scale_var.get_type() == Variant::VECTOR3 && uv2_offset_var.get_type() == Variant::VECTOR3) {
				uv2_scale = uv2_scale_var;
				uv2_offset = uv2_offset_var;
			}
			Variant uv2_sharpness_var = material_storage->material_get_param(p_material_rid, "uv2_blend_sharpness");
			if (uv2_sharpness_var.get_type() == Variant::FLOAT) {
				uv2_blend_sharpness = uv2_sharpness_var;
			}
		}
	}

	// UV1 scale and offset (vec3 in Godot; Z is only used by triplanar mapping).
	Variant uv1_scale_var = material_storage->material_get_param(p_material_rid, "uv1_scale");
	if (uv1_scale_var.get_type() == Variant::VECTOR3) {
		Vector3 s = uv1_scale_var;
		mat.uv1_scale[0] = s.x;
		mat.uv1_scale[1] = s.y;
		mat.uv1_scale[2] = s.z;
	}

	Variant uv1_offset_var = material_storage->material_get_param(p_material_rid, "uv1_offset");
	if (uv1_offset_var.get_type() == Variant::VECTOR3) {
		Vector3 o = uv1_offset_var;
		mat.uv1_offset[0] = o.x;
		mat.uv1_offset[1] = o.y;
		mat.uv1_offset[2] = o.z;
	}

	if (rt_state.uv1_triplanar) {
		mat.flags |= RT_MAT_FLAG_TRIPLANAR;
		if (rt_state.uv1_world_triplanar) {
			mat.flags |= RT_MAT_FLAG_TRIPLANAR_WORLD;
		}

		Variant sharpness_var = material_storage->material_get_param(p_material_rid, "uv1_blend_sharpness");
		if (sharpness_var.get_type() == Variant::FLOAT) {
			uv1_blend_sharpness = sharpness_var;
		}
	}
	rt_material_set_packed_uv(mat, uv1_blend_sharpness, uv2_scale, uv2_offset, uv2_blend_sharpness);

	if (rt_state.proximity_fade) {
		Variant prox_dist_var = material_storage->material_get_param(p_material_rid, "proximity_fade_distance");
		if (prox_dist_var.get_type() == Variant::FLOAT) {
			mat.flags |= RT_MAT_FLAG_PROXIMITY_FADE;
			mat.proximity_fade_distance = prox_dist_var;
		}
	}
	if (rt_state.distance_fade_mode != 0) {
		Variant dist_min_var = material_storage->material_get_param(p_material_rid, "distance_fade_min");
		Variant dist_max_var = material_storage->material_get_param(p_material_rid, "distance_fade_max");
		if (dist_min_var.get_type() == Variant::FLOAT && dist_max_var.get_type() == Variant::FLOAT) {
			// Dither modes are approximated as smooth alpha fade in RT.
			mat.flags |= RT_MAT_FLAG_DISTANCE_FADE;
			mat.distance_fade.min_distance = Math::make_half_float(float(dist_min_var));
			mat.distance_fade.max_distance = Math::make_half_float(float(dist_max_var));
		}
	}

	// Point filtering: check if material requests nearest filtering (e.g. pixel art).
	// BaseMaterial3D exposes this as "texture_filter" int param (0=nearest, 1=linear, etc.).
	Variant filter_var = material_storage->material_get_param(p_material_rid, "texture_filter");
	if (filter_var.get_type() == Variant::INT) {
		int filter_mode = filter_var;
		// 0 = TEXTURE_FILTER_NEAREST, 2 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS,
		// 4 = TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC
		if (filter_mode == 0 || filter_mode == 2 || filter_mode == 4) {
			mat.flags |= RT_MAT_FLAG_POINT_FILTER;
		}
	}

	// Update cache entry
	entry->cached_counter = p_material_invalidation_counter;
	entry->cached_rid_version = mat_version;
	entry->slot_checked_frame = current_frame;
	_touch_material_entry(*entry, mat_idx, current_frame);
	material_table.set(mat_data->table_index, mat);

	return mat_data;
}

// ---------------------------------------------------------------------------
// Acceleration structure building
// ---------------------------------------------------------------------------

Error RenderRaytracing::build_acceleration_structures(RTViewportState *p_state, const LocalVector<RID> &p_dirty_blas_list, const LocalVector<RID> &p_dirty_blas_update_list) {
	RENDER_TIMESTAMP("Acceleration Structure Build");

	_prepare_tlas(p_state->tlas, p_state->tlas_max_instances, tlas_instances, "RT TLAS", tlas_instance_scratch);
	// Built even when empty so the raygen binding stays valid.
	_prepare_tlas(p_state->tlas_transparent, p_state->tlas_transparent_max_instances, tlas_transparent_instances, "RT TLAS Transparent", tlas_transparent_instance_scratch);
	RD::TLASBuild tlas_builds[] = {
		{ p_state->tlas, tlas_instance_scratch },
		{ p_state->tlas_transparent, tlas_transparent_instance_scratch },
	};

	// The batch waits for the captures and merges writing its inputs; _render_scene() orders its readers after it.
	const Error err = RD::get_singleton()->acceleration_structure_build_batch(p_dirty_blas_list, p_dirty_blas_update_list, tlas_builds);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Acceleration structure build failed; skipping path tracing for this frame.");
	return OK;
}

Error RenderRaytracing::_build_one_tlas(RID &r_tlas, uint32_t &r_max_instances, const RTInstanceArrays &p_instances, const String &p_name) {
	_prepare_tlas(r_tlas, r_max_instances, p_instances, p_name, tlas_instance_scratch);
	return _rt_tlas_build(r_tlas, tlas_instance_scratch);
}

void RenderRaytracing::_prepare_tlas(RID &r_tlas, uint32_t &r_max_instances, const RTInstanceArrays &p_instances, const String &p_name, LocalVector<RD::AccelerationStructureInstance> &r_instances) {
	uint32_t needed = MAX(p_instances.blass.size(), (uint32_t)1);
	if (!r_tlas.is_valid() || needed > r_max_instances) {
		if (r_tlas.is_valid()) {
			RD::get_singleton()->free_rid(r_tlas);
		}
		r_max_instances = needed * 2;
		r_tlas = RD::get_singleton()->tlas_create(r_max_instances, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
		RD::get_singleton()->set_resource_name(r_tlas, p_name);
	}

	r_instances.resize(p_instances.blass.size());
	for (uint32_t i = 0; i < p_instances.blass.size(); i++) {
		RD::AccelerationStructureInstance &inst = r_instances[i];
		inst.id = p_instances.custom_indices[i];
		inst.transform = p_instances.transforms[i];
		inst.blas = p_instances.blass[i];
		inst.flags = BitField<RD::AccelerationStructureInstanceFlagBits>(p_instances.flags[i]);
		inst.mask = p_instances.masks[i];
		uint32_t sbt_off = p_instances.sbt_offsets[i];
		inst.hit_sbt_range = RD::HitShaderBindingTableRange((1ULL << 32) | uint64_t(sbt_off));
	}
}

void RenderRaytracing::finalize_buffers(RTViewportState *p_state) {
	// Grow-only uploads. Callers must not free these in prepare_frame().
	_rt_update_or_grow_storage(p_state->geometry_buffer, p_state->geometry_buffer_capacity,
			geometry_data.ptr(), geometry_data.size() * sizeof(RT_GeometryData), "RT Geometry Buffer");
	material_table.upload();
	_rt_update_or_grow_storage(p_state->motion_index_buffer, p_state->motion_index_buffer_capacity,
			motion_indices.ptr(), motion_indices.size() * sizeof(int32_t), "RT Motion Index Buffer");
	_rt_update_or_grow_storage(p_state->motion_transform_buffer, p_state->motion_transform_buffer_capacity,
			motion_transforms.ptr(), motion_transforms.size() * sizeof(RT_InstanceMotionData), "RT Motion Transform Buffer");
}

// ---------------------------------------------------------------------------
// Material table
// ---------------------------------------------------------------------------

void RTMaterialTable::merge_ranges(LocalVector<uint32_t> &r_dirty, uint32_t p_max_gap, LocalVector<Range> &r_ranges) {
	r_ranges.clear();
	r_dirty.sort();
	for (uint32_t index : r_dirty) {
		if (!r_ranges.is_empty() && index <= r_ranges[r_ranges.size() - 1].end + p_max_gap) {
			Range &last = r_ranges[r_ranges.size() - 1];
			last.end = MAX(last.end, index + 1);
		} else {
			r_ranges.push_back({ index, index + 1 });
		}
	}
}

void RTMaterialTable::_grow(uint32_t p_size) {
	if (entries.is_empty() && p_size > 0) {
		entries.push_back(rt_material_data_default());
		dirty.push_back(0);
	}
	const RT_MaterialData default_data = rt_material_data_default();
	while (entries.size() < p_size) {
		entries.push_back(default_data);
	}
}

void RTMaterialTable::set(uint32_t p_index, const RT_MaterialData &p_data) {
	ERR_FAIL_COND(p_index == 0 || p_index > RT_MATERIAL_INDEX_MASK);
	_grow(p_index + 1);
	entries[p_index] = p_data;
	dirty.push_back(p_index);
}

void RTMaterialTable::upload() {
	_grow(1);
	RenderingDevice *rd = RD::get_singleton();
	if (entries.size() > capacity) {
		_free_rid_if_valid(buffer);
		capacity = MAX(entries.size(), capacity + capacity / 2);
		Vector<uint8_t> init;
		init.resize(capacity * sizeof(RT_MaterialData));
		memset(init.ptrw(), 0, init.size());
		memcpy(init.ptrw(), entries.ptr(), entries.size() * sizeof(RT_MaterialData));
		buffer = rd->storage_buffer_create(init.size(), init);
		rd->set_resource_name(buffer, "RT Material Table");
		dirty.clear();
		return;
	}
	merge_ranges(dirty, MERGE_GAP, ranges);
	for (const Range &range : ranges) {
		rd->buffer_update(buffer, range.begin * sizeof(RT_MaterialData), (range.end - range.begin) * sizeof(RT_MaterialData), &entries[range.begin]);
	}
	dirty.clear();
}

void RTMaterialTable::reset() {
	_free_rid_if_valid(buffer);
	capacity = 0;
	entries.reset();
	dirty.reset();
	ranges.reset();
}

// ---------------------------------------------------------------------------
// Merged MultiMesh BLAS builder
// ---------------------------------------------------------------------------

bool RenderRaytracing::_build_merged_mm_blas(
		const RTInstanceSource &p_source,
		void *p_mesh_surface,
		uint32_t p_surface_counter,
		RTMergedMMEntry &r_entry,
		uint32_t p_cache_key,
		RTViewportState *p_state,
		const RTMergeBakeInputs &p_inputs,
		RTSchedulerAction p_action,
		RD::ComputeListID p_compute_list,
		LocalVector<RID> &r_dirty_blas_list,
		LocalVector<RID> &r_dirty_blas_update_list,
		RTSurfaceData *r_surf_data) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(p_mesh_surface);
	RID index_buffer = mesh_storage->mesh_surface_get_index_buffer(p_mesh_surface, 0);
	uint32_t index_count = mesh_storage->mesh_surface_get_index_count(p_mesh_surface, 0);
	bool indexed = index_buffer.is_valid() && index_count > 0;
	uint32_t prim_count = indexed ? (index_count / 3) : (vertex_count / 3);
	const uint32_t instance_count = p_source.count;
	uint64_t surface_format = mesh_storage->mesh_surface_get_format(p_mesh_surface);

	const bool billboarded = p_inputs.billboard_index != UINT32_MAX;
	RTMergedMMEntry &entry = r_entry;
	const uint32_t cache_key = p_cache_key;

	const uint32_t current_frame = RSG::rasterizer->get_frame_number();

	// First viewport this frame does the merge dispatch + BLAS refit; later
	// viewports referencing the same MultiMesh-surface reuse the result.
	// `last_used_frame` doubles as the per-frame guard.
	RTFrameTouch touch(entry.last_used_frame, current_frame);
	const bool first_touch_this_frame = touch.is_first();

	// Detect whether the bake inputs changed since our last bake. Without this
	// we'd re-merge + refit every frame even for fully-static MultiMeshes, which
	// is significant wasted compute for high-instance-count crowds/foliage.
	// Captured entries only change with a new capture.
	const uint64_t mm_last_change = p_source.last_change;
	const bool new_capture = p_inputs.captured && p_inputs.capture_offset != RT_OFFSET_NONE;
	const bool inputs_changed = p_inputs.captured
			? new_capture
			: entry.cached_mm_last_change != mm_last_change || (billboarded && entry.baked_billboard != p_state->merge_billboards[p_inputs.billboard_index]);

	bool structure_changed = (entry.last_mm_count != instance_count ||
			entry.last_surface_counter != p_surface_counter);
	if (p_action == RT_SCHEDULER_HOLD && (structure_changed || !entry.blas_built_once)) {
		return false; // Held before its first build: nothing to trace yet.
	}
	touch.commit();

	entry.indexed = indexed;

	RD *rd = RD::get_singleton();
	// Freeing a BLAS input buffer also frees the BLAS inside RD; drop our handle with it so a new one is created.
	const auto release_blas = [&]() {
		_free_rid_if_valid(entry.blas);
		entry.blas_built_once = false;
	};

	if (structure_changed) {
		release_blas();
		entry.last_mm_count = instance_count;
		entry.last_surface_counter = p_surface_counter;
		entry.surface_valid = false;
	}

	bool has_normal = surface_format & RSE::ARRAY_FORMAT_NORMAL;
	bool has_tangent = surface_format & RSE::ARRAY_FORMAT_TANGENT;
	bool has_tbn = has_normal;

	// Layout of uncompressed vertex buffer: [float3 positions × V] + [packed TBN × V].
	// normal_stride = 8 when both normal+tangent present (two uint16x2 packed), 4 with normal only.
	uint32_t tbn_stride = 0;
	if (has_normal && has_tangent) {
		tbn_stride = 8; // 2 × uint32 (normal oct, tangent oct+sign)
	} else if (has_normal) {
		tbn_stride = 4; // 1 × uint32 (normal oct only)
	}
	// Byte offset of the TBN block in the source vertex buffer.
	uint32_t src_tbn_byte_offset = vertex_count * 12; // after all float3 positions

	// Merged vertex buffer: [float3 pos × N*V] + [packed TBN × N*V] (if TBN present).
	uint32_t merged_vtx_bytes = instance_count * vertex_count * 12 + (has_tbn ? instance_count * vertex_count * tbn_stride : 0);

	// Every merged buffer size below derives from the structure, so a valid surface never sees a resize.
	if (!entry.surface_valid) {
		_fill_surface_geometry_data(p_mesh_surface, p_inputs.captured, &entry.surface);
	}

	// Attribute buffer: attribute_stride bytes × V, replicated N times.
	uint32_t attrib_stride = entry.surface.geometry.attribute_stride;
	bool has_attr = attrib_stride > 0;
	const uint32_t MIN_ATTR_BYTES = 16;
	uint32_t merged_attr_bytes = has_attr ? (instance_count * vertex_count * attrib_stride) : MIN_ATTR_BYTES;

	BitField<RD::BufferCreationBits> gpu_buf_flags =
			RD::BUFFER_CREATION_AS_STORAGE_BIT |
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT |
			RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT;

	// --- Grow / allocate merged vertex buffer ---
	if (!entry.merged_vtx_buffer.is_valid() || entry.vtx_capacity_bytes < merged_vtx_bytes) {
		release_blas();
		_free_rid_if_valid(entry.merged_vtx_buffer);
		entry.vtx_capacity_bytes = merged_vtx_bytes;
		entry.merged_vtx_buffer = rd->vertex_buffer_create(merged_vtx_bytes, {}, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_vtx_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_vtx_buffer, "RT MM merged vtx [" + itos(cache_key) + "]");
	}

	// --- Grow / allocate merged attribute buffer ---
	if (!entry.merged_attr_buffer.is_valid() || entry.attr_capacity_bytes < merged_attr_bytes) {
		_free_rid_if_valid(entry.merged_attr_buffer);
		entry.attr_capacity_bytes = merged_attr_bytes;
		entry.merged_attr_buffer = rd->storage_buffer_create(merged_attr_bytes, {}, 0, gpu_buf_flags);
		ERR_FAIL_COND_V(!entry.merged_attr_buffer.is_valid(), false);
		rd->set_resource_name(entry.merged_attr_buffer, "RT MM merged attr [" + itos(cache_key) + "]");
	}

	// --- Grow / allocate replicated index buffer ---
	if (indexed) {
		uint32_t needed_idx = instance_count * index_count;
		if (!entry.replicated_idx_buffer.is_valid() || entry.idx_capacity < needed_idx) {
			release_blas();
			_free_rid_if_valid(entry.replicated_idx_buffer);
			entry.idx_capacity = needed_idx;
			entry.replicated_idx_buffer = rd->index_buffer_create(
					needed_idx, RD::INDEX_BUFFER_FORMAT_UINT32, {}, false, gpu_buf_flags);
			ERR_FAIL_COND_V(!entry.replicated_idx_buffer.is_valid(), false);
			rd->set_resource_name(entry.replicated_idx_buffer, "RT MM replicated idx [" + itos(cache_key) + "]");
		}
	}

	RID src_attr_buf;
	if (has_attr) {
		src_attr_buf = mesh_storage->mesh_surface_get_attribute_buffer(p_mesh_surface);
		ERR_FAIL_COND_V(!src_attr_buf.is_valid(), false);
	} else {
		src_attr_buf = entry.merged_attr_buffer;
	}

	MergeShader::Mode merge_mode = indexed ? MergeShader::MODE_INDEXED : MergeShader::MODE_NON_INDEXED;

	// Pre-validate source vertex buffer before opening the compute list.
	RID vtx_buf = mesh_storage->mesh_surface_get_vertex_buffer(p_mesh_surface);
	ERR_FAIL_COND_V(!vtx_buf.is_valid(), false);

	// Dynamic items follow the scheduler; static ones (always REFIT) rebake only when their inputs changed.
	const bool needs_rebake = first_touch_this_frame &&
			(p_action == RT_SCHEDULER_REBUILD || (p_action == RT_SCHEDULER_REFIT && (inputs_changed || !entry.blas_built_once)));
	// Captured previous positions are snapshot before the compute list (_snapshot_captured_positions).
	const bool bake_motion = needs_rebake && p_inputs.bake_motion && !p_inputs.captured;
	if (bake_motion) {
		ERR_FAIL_COND_V(!_rt_ensure_prev_pos_buffer(entry, instance_count * vertex_count * 12), false);
	}

	// Positions-only bakes (motion vectors) use MODE_NON_INDEXED with TBN and attributes off.
	auto dispatch_merge = [&](RID p_dst_vtx, bool p_full, uint32_t p_mm_offset, uint32_t p_billboard_index) -> bool {
		const MergeShader::Mode mode = p_full ? merge_mode : MergeShader::MODE_NON_INDEXED;
		Vector<RD::Uniform> uniforms;
		auto push_buf = [&](uint32_t binding, RID buf) {
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = binding;
			u.append_id(buf);
			uniforms.push_back(u);
		};
		push_buf(0, p_dst_vtx);
		push_buf(1, p_source.gpu_buffer);
		push_buf(2, entry.merged_attr_buffer);
		push_buf(3, src_attr_buf);
		if (mode == MergeShader::MODE_INDEXED) {
			push_buf(4, entry.replicated_idx_buffer);
		}
		push_buf(5, p_state->merge_billboard_buffer);
		push_buf(6, p_inputs.captured ? capture_arena : p_source.gpu_buffer); // Unread without BAKE_CAPTURED.
		push_buf(7, vtx_buf);
		if (mode == MergeShader::MODE_INDEXED) {
			push_buf(8, index_buffer);
		}
		RID merge_uniform_set = rd->uniform_set_create(uniforms, mm_merge_shader.version_shader[mode], 0, /*p_linear_pool=*/true);
		ERR_FAIL_COND_V(!merge_uniform_set.is_valid(), false);
		rd->set_resource_name(merge_uniform_set, "RT MM merge uniform set");
		uint32_t tbn_stride_words = tbn_stride / 4;
		// In the merged vertex buffer the TBN block starts after all N*V float3 positions.
		uint32_t dst_tbn_base_words = instance_count * vertex_count * 3;

		// Matches the push constants of multimesh_merge.glsl.
		struct MergePC {
			uint32_t index_count, src_is_16bit;
			uint32_t vertex_count, instance_count;
			uint32_t captured_base_words;
			uint32_t src_tbn_base_words;
			uint32_t src_tbn_stride_words;
			uint32_t dst_tbn_base_words;
			uint32_t mm_stride, mm_offset;
			uint32_t bake_flags;
			uint32_t attr_stride_words;
			uint32_t billboard_index;
			float dead_position[3];
		};
		static_assert(sizeof(MergePC) % 16 == 0);

		MergePC pc = {};
		if (mode == MergeShader::MODE_INDEXED) {
			pc.index_count = index_count;
			pc.src_is_16bit = (vertex_count <= 65536) ? 1u : 0u;
		}
		pc.vertex_count = vertex_count;
		pc.instance_count = instance_count;
		pc.captured_base_words = p_inputs.captured ? p_inputs.capture_offset / 4 : 0u;
		pc.src_tbn_base_words = src_tbn_byte_offset / 4;
		pc.src_tbn_stride_words = tbn_stride_words;
		pc.dst_tbn_base_words = dst_tbn_base_words;
		pc.mm_stride = p_source.stride;
		pc.mm_offset = p_mm_offset;
		pc.bake_flags = (p_full && has_tbn ? uint32_t(MergeShader::BAKE_TBN) : 0u) | (p_inputs.captured ? uint32_t(MergeShader::BAKE_CAPTURED) : 0u);
		pc.attr_stride_words = (p_full && has_attr) ? (attrib_stride / 4) : 0u;
		pc.billboard_index = p_billboard_index;
		pc.dead_position[0] = p_source.dead_position.x;
		pc.dead_position[1] = p_source.dead_position.y;
		pc.dead_position[2] = p_source.dead_position.z;

		rd->compute_list_bind_compute_pipeline(p_compute_list, mm_merge_shader.pipeline[mode]);
		rd->compute_list_bind_uniform_set(p_compute_list, merge_uniform_set, 0);
		rd->compute_list_set_push_constant(p_compute_list, &pc, sizeof(pc));

		// Single thread count: every thread processes one vertex (idx < N*V)
		// and -- for MODE_INDEXED -- one output index (idx < N*I) using disjoint
		// destination buffers, so no in-shader barrier is required.
		uint32_t thread_count = instance_count * vertex_count;
		if (mode == MergeShader::MODE_INDEXED) {
			thread_count = MAX(thread_count, instance_count * index_count);
		}
		rd->compute_list_dispatch_threads(p_compute_list, thread_count, 1, 1);

		// Drop the RID now that the dispatch is recorded; the underlying Vulkan
		// descriptor set lives in the linear pool until frame end.
		rd->free_rid(merge_uniform_set);
		return true;
	};

	if (needs_rebake) {
		ERR_FAIL_COND_V(!dispatch_merge(entry.merged_vtx_buffer, true, p_source.current_offset, p_inputs.billboard_index), false);
	}
	if (bake_motion) {
		ERR_FAIL_COND_V(!dispatch_merge(entry.prev_pos_buffer, false, p_source.prev_offset, p_inputs.prev_billboard_index), false);
		entry.motion_frame = current_frame;
	}

	// --- Build or refit the merged BLAS (uses merged_vtx_buffer for positions) ---
	if (!entry.blas.is_valid()) {
		RD::AccelerationStructureGeometry as_geom;
		as_geom.type = RD::AccelerationStructureGeometry::TYPE_TRIANGLES;
		as_geom.geometry.triangles.vertex_buffer = entry.merged_vtx_buffer;
		as_geom.geometry.triangles.vertex_stride = 12; // float3, positions section only
		as_geom.geometry.triangles.vertex_count = instance_count * vertex_count;
		as_geom.geometry.triangles.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;

		if (indexed) {
			as_geom.geometry.triangles.index_buffer = entry.replicated_idx_buffer;
			as_geom.geometry.triangles.index_count = instance_count * index_count;
		}

		// Scheduled entries (particles, captures, camera-dependent billboards) are rebuilt or refitted nearly every frame,
		// so build speed wins over trace speed. Only static MultiMeshes keep fast trace.
		const bool transient = p_source.particles || p_inputs.captured || billboarded;
		BitField<RD::AccelerationStructureFlagBits> as_flags =
				(transient ? RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT : RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT) |
				RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT;
		entry.blas = rd->blas_create({ &as_geom, 1 }, as_flags);
		ERR_FAIL_COND_V(!entry.blas.is_valid(), false);
		rd->set_resource_name(entry.blas, "RT MM merged BLAS [" + itos(cache_key) + "]");
	}

	if (needs_rebake) {
		if (!entry.blas_built_once || p_action == RT_SCHEDULER_REBUILD) {
			r_dirty_blas_list.push_back(entry.blas);
			entry.blas_built_once = true;
		} else {
			r_dirty_blas_update_list.push_back(entry.blas);
		}
		entry.cached_mm_last_change = mm_last_change;
		if (billboarded) {
			entry.baked_billboard = p_state->merge_billboards[p_inputs.billboard_index];
		}
	}
	// A BLAS created without a bake this frame (a later viewport after a structure change) was never queued; it cannot be traced.
	if (!entry.blas_built_once) {
		return false;
	}

	// --- Override the surface metadata filled above with the merged buffer layout and addresses.
	if (!entry.surface_valid) {
		entry.surface.is_compressed = false;
		RT_GeometryData &geom = entry.surface.geometry;
		RTGeometrySources &sources = entry.surface.sources;

		// Point vertex address at merged buffer (positions + TBN all baked world-space).
		geom.vertex_buffer_address = _rt_buffer_address(entry.merged_vtx_buffer, sources.vertex);
		geom.vertex_count = instance_count * vertex_count;
		geom.position_stride = 12; // float3, uncompressed
		geom.flags &= ~RT_GEOM_FLAG_COMPRESSED;
		geom.flags |= RT_GEOM_FLAG_DEFORMED | (p_inputs.captured ? uint32_t(RT_GEOM_FLAG_CAPTURED) : 0u);

		// TBN section starts after all positions in the merged vertex buffer.
		if (has_tbn) {
			uint32_t tbn_base = instance_count * vertex_count * 12;
			geom.normal_byte_offset = tbn_base;
			geom.normal_stride = tbn_stride;
			if (has_tangent) {
				geom.tangent_byte_offset = tbn_base; // tangent is at +4 from normal within the same stride pair
				geom.tangent_stride = tbn_stride;
			} else {
				geom.tangent_byte_offset = RT_OFFSET_NONE;
				geom.tangent_stride = 0;
			}
		}

		// Point attribute address at fully replicated attribute buffer.
		if (has_attr && entry.merged_attr_buffer.is_valid()) {
			geom.attribute_buffer_address = _rt_buffer_address(entry.merged_attr_buffer, sources.attribute);
		}

		// Point index address at the replicated (uint32) index buffer.
		if (indexed && entry.replicated_idx_buffer.is_valid()) {
			geom.index_buffer_address = _rt_buffer_address(entry.replicated_idx_buffer, sources.index);
			geom.index_format = RT_INDEX_FORMAT_UINT32; // always uint32 in replicated buffer
			geom.primitive_count = instance_count * prim_count;
		} else {
			geom.index_buffer_address = 0;
			sources.index = RID();
			geom.index_format = RT_INDEX_FORMAT_NONE;
			geom.primitive_count = instance_count * prim_count;
		}
		entry.surface_valid = true;
	}

	// Motion vectors stay out of the cached surface: they are only valid on the frame they were baked for.
	*r_surf_data = entry.surface;
	r_surf_data->blas = entry.blas;
	if (entry.motion_frame == current_frame) {
		const uint64_t prev_address = _rt_buffer_address(entry.prev_pos_buffer, entry.prev_pos_address, r_surf_data->sources.prev_vertex);
		r_surf_data->geometry.prev_vertex_buffer_address_lo = uint32_t(prev_address);
		r_surf_data->geometry.prev_vertex_buffer_address_hi = uint32_t(prev_address >> 32);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Merged mesh surface groups
// ---------------------------------------------------------------------------

RTMeshGroupEntry *RenderRaytracing::_process_mesh_group(const RTMeshGroupKey &p_key, Span<const RTPlanSurface> p_members, LocalVector<RID> &r_dirty_blas_list) {
	RTMeshGroupEntry *entry = mesh_groups.getptr(p_key);
	if (!entry) {
		_trim_mesh_group_variants(p_key.mesh_id);
		entry = &mesh_groups.insert(p_key, RTMeshGroupEntry())->value;
	}
	entry->last_used = ++mesh_group_use_counter;

	bool up_to_date = entry->blas.is_valid() && entry->surface_counters.size() == p_members.size();
	for (uint32_t i = 0; up_to_date && i < p_members.size(); i++) {
		up_to_date = entry->surface_counters[i] == p_members[i].surface_counter;
	}
	if (up_to_date) {
		cache_hits++;
		return entry;
	}
	cache_misses++;

	_free_blas_resources(blas_transform_pool, entry->blas, entry->transforms.ptr(), entry->transforms.size());

	const uint32_t count = p_members.size();
	entry->geometries.resize(count);
	entry->sources.resize(count);
	entry->surface_counters.resize(count);
	entry->transforms.resize(count);
	LocalVector<RD::AccelerationStructureGeometry> as_geoms;
	as_geoms.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const RTPlanSurface &rec = p_members[i];
		void *mesh_surface = static_cast<const RenderForwardClustered::GeometryInstanceSurfaceDataCache *>(rec.surf)->surface;
		RTSurfaceData surf_data;
		if (!_fill_surface_blas_geometry(mesh_surface, RID(), RID(), false, &surf_data, as_geoms[i])) {
			mesh_groups.erase(p_key);
			return nullptr;
		}
		if (p_key.opaque_mask & (1ULL << rec.surface_index)) {
			as_geoms[i].flags.set_flag(RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT);
		}
		entry->geometries[i] = surf_data.geometry;
		entry->sources[i] = surf_data.sources;
		entry->surface_counters[i] = rec.surface_counter;
	}

	_assign_compression_transforms(blas_transform_pool, entry->geometries.ptr(), as_geoms.ptr(), entry->transforms.ptr(), count);
	const String name = "RT BLAS group [" + itos(RID::from_uint64(p_key.mesh_id).get_local_index()) + ":" + String::num_uint64(p_key.surface_mask, 16) + "]";
	entry->blas = _create_blas(as_geoms, false, false, name, r_dirty_blas_list);
	if (!entry->blas.is_valid()) {
		_free_blas_resources(blas_transform_pool, entry->blas, entry->transforms.ptr(), count);
		mesh_groups.erase(p_key);
		return nullptr;
	}
	return entry;
}

void RenderRaytracing::_trim_mesh_group_variants(uint64_t p_mesh_id) {
	while (true) {
		uint32_t variants = 0;
		const RTMeshGroupKey *oldest = nullptr;
		uint64_t oldest_use = UINT64_MAX;
		for (const KeyValue<RTMeshGroupKey, RTMeshGroupEntry> &kv : mesh_groups) {
			if (kv.key.mesh_id != p_mesh_id) {
				continue;
			}
			variants++;
			// Variants used by the TLAS being built must outlive it; the limit is exceeded until the next build.
			if (kv.value.last_used <= mesh_group_build_first_use && kv.value.last_used < oldest_use) {
				oldest_use = kv.value.last_used;
				oldest = &kv.key;
			}
		}
		if (variants < RT_MESH_GROUP_MAX_VARIANTS || !oldest) {
			return;
		}
		const RTMeshGroupKey key = *oldest;
		RTMeshGroupEntry &entry = mesh_groups[key];
		_free_blas_resources(blas_transform_pool, entry.blas, entry.transforms.ptr(), entry.transforms.size());
		mesh_groups.erase(key);
	}
}

void RenderRaytracing::_evict_dead_mesh_groups() {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	LocalVector<RTMeshGroupKey> dead;
	for (const KeyValue<RTMeshGroupKey, RTMeshGroupEntry> &kv : mesh_groups) {
		if (!mesh_storage->owns_mesh(RID::from_uint64(kv.key.mesh_id))) {
			dead.push_back(kv.key);
		}
	}
	for (const RTMeshGroupKey &key : dead) {
		RTMeshGroupEntry &entry = mesh_groups[key];
		_free_blas_resources(blas_transform_pool, entry.blas, entry.transforms.ptr(), entry.transforms.size());
		mesh_groups.erase(key);
	}
}

// ---------------------------------------------------------------------------
// TLAS creation (main entry point per frame)
// ---------------------------------------------------------------------------

static_assert(int(RT_MAT_DIFFUSE_LAMBERT) == int(SceneShaderForwardClustered::ShaderData::DIFFUSE_LAMBERT) &&
				int(RT_MAT_DIFFUSE_BURLEY) == int(SceneShaderForwardClustered::ShaderData::DIFFUSE_BURLEY) &&
				int(RT_MAT_DIFFUSE_LAMBERT_WRAP) == int(SceneShaderForwardClustered::ShaderData::DIFFUSE_LAMBERT_WRAP) &&
				int(RT_MAT_DIFFUSE_TOON) == int(SceneShaderForwardClustered::ShaderData::DIFFUSE_TOON),
		"RT diffuse modes must match ShaderData::DiffuseMode.");
static_assert(int(RT_MAT_SPECULAR_SCHLICK_GGX) == int(SceneShaderForwardClustered::ShaderData::SPECULAR_SCHLICK_GGX) &&
				int(RT_MAT_SPECULAR_TOON) == int(SceneShaderForwardClustered::ShaderData::SPECULAR_TOON) &&
				int(RT_MAT_SPECULAR_DISABLED) == int(SceneShaderForwardClustered::ShaderData::SPECULAR_DISABLED),
		"RT specular modes must match ShaderData::SpecularMode.");

// Shader flags for the per-instance MaterialData copy. Unshaded and the light
// model apply to opaque HG0 surfaces too; blend-class bits are only used during peeling.
static uint32_t _rt_surface_mat_flags(const SceneShaderForwardClustered::ShaderData *sd,
		SceneShaderForwardClustered::ShaderData::RTBlendClass p_class) {
	uint32_t flags = 0u;
	if (sd) {
		flags |= rt_material_shading_mode_flags(sd->diffuse_mode, sd->specular_mode);
		if (sd->unshaded) {
			flags |= RT_MAT_FLAG_UNSHADED;
		}
		if (sd->fog_disabled) {
			flags |= RT_MAT_FLAG_FOG_DISABLED;
		}
	}
	if (p_class == SceneShaderForwardClustered::ShaderData::RT_BLEND_OPAQUE) {
		return flags;
	}
	flags |= RT_MAT_FLAG_TRANSPARENT;
	const int bm = sd ? (sd->rt ? sd->rt->blend_mode : sd->blend_mode) : (int)SceneShaderForwardClustered::ShaderData::BLEND_MODE_MIX;
	uint32_t v = RT_MAT_BLEND_CLASS_MIX;
	if (p_class == SceneShaderForwardClustered::ShaderData::RT_BLEND_OIT) {
		v = RT_MAT_BLEND_CLASS_OIT;
	} else {
		switch (bm) {
			case SceneShaderForwardClustered::ShaderData::BLEND_MODE_ADD:
				v = RT_MAT_BLEND_CLASS_ADD;
				break;
			case SceneShaderForwardClustered::ShaderData::BLEND_MODE_SUB:
				v = RT_MAT_BLEND_CLASS_SUB;
				break;
			case SceneShaderForwardClustered::ShaderData::BLEND_MODE_MUL:
				v = RT_MAT_BLEND_CLASS_MUL;
				break;
			case SceneShaderForwardClustered::ShaderData::BLEND_MODE_OIT:
				v = RT_MAT_BLEND_CLASS_OIT;
				break;
			case SceneShaderForwardClustered::ShaderData::BLEND_MODE_PREMULTIPLIED_ALPHA:
				v = RT_MAT_BLEND_CLASS_PREMULT;
				break;
			default:
				break;
		}
	}
	flags |= v << RT_MAT_BLEND_CLASS_SHIFT;
	if (sd && sd->rt_depth_draw_always()) {
		flags |= RT_MAT_FLAG_DEPTH_DRAW_ALWAYS;
	}
	return flags;
}

static float _rt_dominant_layer_alpha() {
	return (float)GLOBAL_GET_CACHED(double, "rendering/pathtracing/transparency_dominant_layer_alpha");
}

static bool _rt_uses_alpha_scissor(const SceneShaderForwardClustered::ShaderData *p_shader,
		const RTMaterialData *p_material, const SceneShaderRaytracing *p_rt_shader) {
	if (p_material->rt_sbt_offset > 0) {
		const SceneShaderRaytracing::CustomShaderEntry *entry = p_rt_shader->get_custom_shader_entry(p_material->rt_sbt_offset);
		return !entry || entry->uses_alpha_clip;
	}
	return p_shader && (p_shader->uses_alpha_clip || p_shader->uses_alpha_antialiasing);
}

struct RTSurfaceRecordOrder {
	_FORCE_INLINE_ bool operator()(const RTSurfaceRecord &p_a, const RTSurfaceRecord &p_b) const {
		return p_a.surface.surface_index < p_b.surface.surface_index;
	}
};

// Key of a group member traced on its own: members share the group key except for their opacity.
static RTSurfaceGroupKey _rt_member_key(const RTPlanGroup &p_group, const RTPlanSurface &p_member) {
	RTSurfaceGroupKey key = p_group.key;
	if (p_group.surface_count > 1) {
		key.opaque = (p_group.opaque_mask >> p_member.surface_index) & 1;
	}
	return key;
}

static RSE::CullMode _rt_cull_mode(const SceneShaderForwardClustered::ShaderData *p_shader) {
	return p_shader ? p_shader->rt_cull_mode() : RSE::CULL_MODE_BACK;
}

namespace RendererSceneRenderImplementation {

// Gathers the TLAS instances of one view and their geometry and material entries, for RenderRaytracing::build_tlas.
// Mesh instances are planned into the view's arena first, then every instance is applied in order: mesh and procedural
// instances are added, MultiMesh and particle surfaces collected and added afterwards, so their merge dispatches share
// one compute list.
class RTTlasBuilder {
public:
	RTTlasBuilder(RenderRaytracing &p_rt, const RenderDataRD *p_render_data, const RTPlanContext &p_ctx, LocalVector<RTCaptureRequest> &r_captures, RTViewportState *p_state);

	// Plans the mesh instances into r_arena, adds them and the procedural instances and collects the MultiMesh and particle surfaces.
	void gather_instances(RTPlanArena &r_arena);
	// Adds the surfaces collected by gather_instances, merged into one BLAS per surface where the merge bake can.
	void add_multimesh_surfaces();
	// Adds this view's counters to its render info and flags surfaces still waiting to be traced.
	void finish();

	// BLASes this view queued for a build.
	LocalVector<RID> dirty_blas_list;
	// BLASes this view queued for a refit.
	LocalVector<RID> dirty_blas_update_list;

private:
	using Instance = RenderForwardClustered::GeometryInstanceForwardClustered;
	using SurfaceCache = RenderForwardClustered::GeometryInstanceSurfaceDataCache;
	using ShaderData = SceneShaderForwardClustered::ShaderData;

	// TLAS masks of one instance: the camera lane plus its shadow-caster groups.
	struct InstanceMasks {
		bool camera_visible = false;
		uint8_t opaque = 0;
		uint8_t shadow = 0;

		// Transparent surfaces use the transparent camera lane.
		uint8_t for_surface(bool p_transparent) const {
			return p_transparent ? uint8_t((camera_visible ? RT_MASK_TRANSPARENT : 0) | shadow) : opaque;
		}
	};

	// How gather_instances treats an instance of rt_instances.
	enum InstanceKind : uint8_t {
		INSTANCE_SKIPPED,
		INSTANCE_PROCEDURAL,
		INSTANCE_MERGED, // MultiMesh or particles.
		INSTANCE_MESH,
	};

	struct PendingMMSurface {
		RTInstanceSource source;
		const SurfaceCache *mm_surf;
		void *mesh_surface;
		uint32_t surface_key; // Surface index for MultiMesh, list position for particles.
		uint32_t surface_counter;
		Transform3D instance_transform;
		Transform3D prev_instance_transform;
		bool transform_moved;
		bool raster_mirror; // The rasterizer culls the node with the reversed cull mode.
		RTMaterialData *mat_data;
		RTSurfaceGroupKey key;
		bool transparent;
		uint32_t layer_mask;
		uint32_t instance_uniforms_ofs = 0xFFFFFFFFu;
		float instance_alpha = 1.0f;
		uint32_t material_flags = 0;
		RenderingBillboard::Settings billboard;
		RTMMSurfaceKey billboard_key;
		RTMergeBakeInputs bake;
		RID capture_entry; // Merged entry holding the surface's capture when bake.captured.
		RTSchedulerAction capture_action = RT_SCHEDULER_HOLD; // Picked by prepare_vertex_captures when bake.captured.
		AABB aabb; // World-space instance bounds, for scheduling.
		RSE::RTBlasUpdate blas_update = RSE::RT_BLAS_UPDATE_AUTO; // Resolved against the shader, for scheduling.
	};

	struct MergedJob {
		RTMergedMMEntry *entry = nullptr; // Null when the merge bake can't handle the surface.
		uint32_t cache_key = 0; // Resource naming only.
		RTSchedulerAction action = RT_SCHEDULER_REFIT; // Static entries rebake whenever their inputs change.
	};

	// Per-instance color/custom data of a MultiMesh, as read by the hit shaders.
	struct MMInstanceData {
		uint64_t address = 0; // Zero when the MultiMesh has no color/custom data.
		RID source;
		uint32_t stride_bytes = 0;
		uint32_t layout = 0;
	};

	// Surfaces the merge bake can't handle; expanded into one TLAS entry per instance.
	struct MMFallback {
		uint32_t pending_index = 0;
		MMInstanceData instance_data;
	};

	// Dirty list sizes before a surface was added; the growth is the BLAS work it queued.
	struct DirtyMark {
		uint32_t builds = 0;
		uint32_t refits = 0;
	};

	RenderRaytracing &rt;
	const RenderDataRD *render_data = nullptr;
	const RTPlanContext &ctx;
	LocalVector<RTCaptureRequest> &captures;
	RTViewportState *state = nullptr;
	RendererRD::MeshStorage *mesh_storage = nullptr;
	RendererRD::MaterialStorage *material_storage = nullptr;
	SceneShaderRaytracing *rt_shader = nullptr;
	// Path-traced billboards face the camera for every ray type, like raster Enabled/Y modes do for the main camera.
	RenderingBillboard::Camera billboard_camera;
	RenderingBillboard::Camera prev_billboard_camera;

	LocalVector<InstanceKind> instance_kinds; // Per rt_instances entry.
	LocalVector<PendingMMSurface> pending_mm_surfaces;
	uint32_t pending_surface_count = 0; // Surfaces left out until their hit group or capture is ready.

	const bool collect_render_info = false;
	const bool preserve_front_facing = false; // Matches the rasterizer, see rt_triangle_facing().
	uint32_t tlas_instance_count = 0;
	uint32_t tlas_primitive_count = 0;
	uint32_t rt_blas_builds = 0;
	uint32_t rt_blas_refits = 0;
	uint32_t rt_triangles_built = 0;
	uint32_t rt_triangles_refit = 0;

	// GeometryInstance3D.transparency times the visibility-parent fade, as raster applies it.
	static float _instance_alpha(const Instance &p_inst) {
		return p_inst.force_alpha * p_inst.parent_fade_alpha;
	}
	// Fully faded instances, which raster draws with 8-bit alpha 0.
	static bool _fully_faded(const Instance &p_inst) {
		return rt_instance_transparency(_instance_alpha(p_inst)) == 255u;
	}
	static ShaderData::RTBlendClass _shader_blend_class(const ShaderData *p_shader) {
		return p_shader ? p_shader->rt_blend_class() : ShaderData::RT_BLEND_OPAQUE;
	}
	// Raster draws faded instances in the alpha pass, so their opaque materials blend as Mix.
	static ShaderData::RTBlendClass _blend_class(const ShaderData *p_shader, float p_instance_alpha) {
		const ShaderData::RTBlendClass blend_class = _shader_blend_class(p_shader);
		return blend_class == ShaderData::RT_BLEND_OPAQUE && rt_instance_transparency(p_instance_alpha) != 0u ? ShaderData::RT_BLEND_MIX : blend_class;
	}
	// Previous-frame transform for motion vectors; a teleport has no motion.
	static const Transform3D &_prev_transform(const Instance &p_inst) {
		return p_inst.transform_status == Instance::TransformStatus::TELEPORTED ? p_inst.transform : p_inst.prev_transform;
	}
	static bool _moved(const Instance &p_inst) {
		return p_inst.transform_status == Instance::TransformStatus::MOVED;
	}

	static bool _casts_self_shadow(const Instance &p_inst) { return p_inst.rt_self_shadowing && p_inst.rt_shadow_caster; }
	uint32_t _self_shadowing_flag(uint8_t p_tlas_mask) const {
		return (p_tlas_mask & state->shadow_groups.self_shadow_bit) != 0 ? uint32_t(RT_GEOM_FLAG_SELF_SHADOWING) : 0u;
	}
	static InstanceKind _instance_kind(const Instance *p_inst);
	static InstanceMasks _plan_masks(const RTPlanKey &p_key) { return { p_key.camera_visible, p_key.mask_opaque, p_key.mask_shadow }; }

	void _register_shadow_groups();
	InstanceMasks _instance_masks(const Instance &p_inst) const;
	void _add_procedural_instance(const Instance &p_inst, const InstanceMasks &p_masks);
	void _gather_mm_surfaces(const Instance &p_inst, const InstanceMasks &p_masks);

	// Planning: reads p_ctx and the caches, writes only the record p_index of r_arena and r_scratch, so it runs on workers.
	// p_ctx is ctx, with allow_mutation off on workers (see RenderRaytracing::_plan_surface_material).
	bool _plan_mesh_record(const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, RTPlanArena &r_arena, uint32_t p_index);
	bool _reuse_mesh_plan(const Instance &p_inst, RTPlanArena &r_arena, uint32_t p_index);
	bool _plan_surfaces_current(const RTPlanArena &p_arena, const RTPlanInstance &p_record) const;
	bool _plan_mesh_instance(const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, const Instance &p_inst, RTPlanArena &r_arena, uint32_t p_index);
	uint8_t _plan_deformed_kind(const Instance &p_inst, const SurfaceCache *p_surf) const;
	static void _plan_groups(RTPlanScratch &r_scratch, RTPlanArena &r_arena, RTPlanInstance &r_record);
#ifdef RT_VALIDATION_ENABLED
	void _rtv_check_reused_tlas(const Instance &p_inst, const RTPlanArena &p_arena, uint32_t p_index);
#endif

	// Applying a plan: every side effect, in TLAS order.
	void _apply_mesh_instance(const Instance &p_inst, const RTPlanArena &p_arena, const RTPlanInstance &p_record);
	void _apply_surface_group(const Instance &p_inst, const RTPlanGroup &p_group, Span<const RTPlanSurface> p_members, int32_t p_motion_index);
	RTSurfaceData *_process_plan_surface(const Instance &p_inst, const RTPlanSurface &p_surface);
	RTDeformedGeometrySource _deformed_source(const Instance &p_inst, const RTPlanSurface &p_surface);

	void _upload_merge_billboards();
	void _resolve_merged_jobs(LocalVector<MergedJob> &r_jobs);
	bool _resolve_instance_data(const RTInstanceSource &p_source, MMInstanceData &r_data);
	bool _add_merged_mm_surface(const PendingMMSurface &p_pending, const MergedJob &p_job, const MMInstanceData &p_instance_data, RD::ComputeListID p_compute_list);
	void _add_expanded_mm_surface(const PendingMMSurface &p_pending, const MMInstanceData &p_instance_data);

	bool _hit_group_ready(const RTMaterialData *p_mat_data) const;
	void _fill_surface_key(const ShaderData *p_shader, const RTMaterialData *p_mat_data, ShaderData::RTBlendClass p_blend_class, uint8_t p_tlas_mask,
			RTSurfaceGroupKey &r_key, uint32_t &r_material_flags) const;
	uint32_t _push_tlas_instance(const Transform3D &p_transform, RID p_blas, const RTSurfaceGroupKey &p_key, bool p_transparent, bool p_raster_mirror);
	void _push_tlas_entry(const RT_GeometryData &p_geometry, const RTGeometrySources &p_sources, uint32_t p_facing_flags, const RTMaterialData *p_mat_data,
			uint32_t p_material_flags, uint32_t p_layers, uint32_t p_uniforms_ofs, float p_instance_alpha, int32_t p_motion_index);
	int32_t _push_motion_transform(const Transform3D &p_prev_transform);

	DirtyMark _dirty_mark() const { return { dirty_blas_list.size(), dirty_blas_update_list.size() }; }
	void _record_stats(const DirtyMark &p_mark, uint32_t p_instances, uint32_t p_primitives, uint32_t p_blas_primitives);
};

} // namespace RendererSceneRenderImplementation

RTTlasBuilder::RTTlasBuilder(RenderRaytracing &p_rt, const RenderDataRD *p_render_data, const RTPlanContext &p_ctx, LocalVector<RTCaptureRequest> &r_captures, RTViewportState *p_state) :
		rt(p_rt),
		render_data(p_render_data),
		ctx(p_ctx),
		captures(r_captures),
		state(p_state),
		mesh_storage(RendererRD::MeshStorage::get_singleton()),
		material_storage(RendererRD::MaterialStorage::get_singleton()),
		rt_shader(SceneShaderRaytracing::get_singleton()),
		collect_render_info(p_render_data->render_info != nullptr),
		preserve_front_facing(RenderForwardClustered::get_singleton()->is_front_facing_preserved_when_mirrored()) {
	if (p_render_data->scene_data) {
		billboard_camera = _rt_billboard_camera(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection);
		prev_billboard_camera = _rt_billboard_camera(p_render_data->scene_data->prev_cam_transform, p_render_data->scene_data->prev_cam_projection);
	}
	for (const RTCaptureRequest &capture : r_captures) {
		pending_surface_count += capture.recorded ? 0 : 1;
	}
}

RTTlasBuilder::InstanceKind RTTlasBuilder::_instance_kind(const Instance *p_inst) {
	if (!p_inst || !p_inst->data || _fully_faded(*p_inst)) {
		return INSTANCE_SKIPPED;
	}
	if (p_inst->rt_procedural) {
		return INSTANCE_PROCEDURAL;
	}
	const bool merged = p_inst->data->base_type == RSE::INSTANCE_MULTIMESH || p_inst->data->base_type == RSE::INSTANCE_PARTICLES;
	return merged ? INSTANCE_MERGED : INSTANCE_MESH;
}

void RTTlasBuilder::gather_instances(RTPlanArena &r_arena) {
	_register_shadow_groups();

	const PagedArray<RenderGeometryInstance *> &instances = *render_data->rt_instances;
	const uint32_t count = r_arena.instances.size();
	for (uint32_t i = 0; i < count; i++) {
		const Instance *inst = static_cast<const Instance *>(instances[i]);
		if (inst && _casts_self_shadow(*inst)) {
			state->shadow_groups.reserve_self_shadow_group();
			break;
		}
	}
	// Shadow-caster groups are allocated on first use, so masks are assigned serially and in instance order.
	instance_kinds.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const Instance *inst = static_cast<const Instance *>(instances[i]);
		instance_kinds[i] = _instance_kind(inst);
		if (instance_kinds[i] == INSTANCE_SKIPPED) {
			continue;
		}
		const InstanceMasks masks = _instance_masks(*inst);
		RTPlanKey &key = r_arena.instances[i].key;
		key.camera_visible = masks.camera_visible;
		key.mask_opaque = masks.opaque;
		key.mask_shadow = masks.shadow;
		key.transparency = uint8_t(rt_instance_transparency(_instance_alpha(*inst)));
	}

	_rt_plan_instances(ctx, count, rt.plan_workspace, [&](const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, uint32_t p_index) {
		return _plan_mesh_record(p_ctx, r_scratch, r_arena, p_index);
	});
	for (uint32_t i = 0; i < count; i++) {
		const Instance *inst = static_cast<const Instance *>(instances[i]);
		if (instance_kinds[i] == INSTANCE_MESH && inst->rt_index != UINT32_MAX) {
			rt_plan_set_lookup(state->plan_lookup, inst->rt_index, i);
		}
	}

	for (uint32_t i = 0; i < count; i++) {
		const Instance *inst = static_cast<const Instance *>(instances[i]);
		const RTPlanInstance &record = r_arena.instances[i];
		switch (instance_kinds[i]) {
			case INSTANCE_PROCEDURAL:
				_add_procedural_instance(*inst, _plan_masks(record.key));
				break;
			case INSTANCE_MERGED:
				_gather_mm_surfaces(*inst, _plan_masks(record.key));
				break;
			case INSTANCE_MESH:
				_apply_mesh_instance(*inst, r_arena, record);
				break;
			case INSTANCE_SKIPPED:
				break;
		}
	}
}

void RTTlasBuilder::add_multimesh_surfaces() {
	_upload_merge_billboards();
	LocalVector<MergedJob> jobs;
	_resolve_merged_jobs(jobs);

	LocalVector<MMFallback> fallbacks;
	const RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	for (uint32_t i = 0; i < pending_mm_surfaces.size(); i++) {
		const PendingMMSurface &pending = pending_mm_surfaces[i];
		MMInstanceData instance_data;
		if (!_resolve_instance_data(pending.source, instance_data)) {
			continue;
		}
		const bool merged = _add_merged_mm_surface(pending, jobs[i], instance_data, compute_list);
		if (!merged && !pending.source.particles && !pending.bake.captured) {
			fallbacks.push_back({ i, instance_data });
		}
	}
	// process_surface updates and copies buffers, which RD forbids inside a compute list.
	RD::get_singleton()->compute_list_end();

	for (const MMFallback &fallback : fallbacks) {
		_add_expanded_mm_surface(pending_mm_surfaces[fallback.pending_index], fallback.instance_data);
	}
}

void RTTlasBuilder::finish() {
	if (collect_render_info) {
		int *visible_info = render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE];
		visible_info[RSE::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += tlas_instance_count;
		visible_info[RSE::VIEWPORT_RENDER_INFO_PRIMITIVES_IN_FRAME] += tlas_primitive_count;
		visible_info[RSE::VIEWPORT_RENDER_INFO_RT_TLAS_INSTANCES] += tlas_instance_count;
		visible_info[RSE::VIEWPORT_RENDER_INFO_RT_BLAS_BUILDS] += rt_blas_builds;
		visible_info[RSE::VIEWPORT_RENDER_INFO_RT_BLAS_REFITS] += rt_blas_refits;
		visible_info[RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_BUILT] += rt_triangles_built;
		visible_info[RSE::VIEWPORT_RENDER_INFO_RT_TRIANGLES_REFIT] += rt_triangles_refit;
		visible_info[RSE::VIEWPORT_RENDER_INFO_RT_PENDING_SURFACES] += pending_surface_count;
		for (const RTCaptureRequest &capture : captures) {
			if (capture.committed) {
				visible_info[RSE::VIEWPORT_RENDER_INFO_RT_CAPTURED_MESHES] += 1;
				visible_info[RSE::VIEWPORT_RENDER_INFO_RT_CAPTURED_VERTICES] += int(capture.layout.point_count());
			}
		}
	}
	if (pending_surface_count > 0) {
		rt._flag_not_ready(RSE::RENDERING_NOT_READY_RT_PENDING_SURFACES);
	}
}

// Collected from every light this frame so instance masks and light shadow_ray_masks agree (see RTShadowGroupTable).
void RTTlasBuilder::_register_shadow_groups() {
	state->shadow_groups.reset();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	const PagedArray<RID> *light_lists[] = { render_data->lights, render_data->rt_lights };
	for (const PagedArray<RID> *lights : light_lists) {
		if (!lights) {
			continue;
		}
		for (uint32_t li = 0; li < (uint32_t)lights->size(); li++) {
			state->shadow_groups.register_caster_mask(light_storage->light_get_shadow_caster_mask(light_storage->light_instance_get_base_light((*lights)[li])));
		}
	}
}

RTTlasBuilder::InstanceMasks RTTlasBuilder::_instance_masks(const Instance &p_inst) const {
	InstanceMasks masks;
	masks.camera_visible = (p_inst.layer_mask & ctx.camera_visible_layers) != 0 && !p_inst.rt_shadows_only;
	masks.opaque = masks.camera_visible ? uint8_t(RT_VIS_BIT) : uint8_t(0);
	if (_casts_self_shadow(p_inst)) {
		masks.shadow = state->shadow_groups.self_shadow_bits_for_layers(p_inst.layer_mask);
	} else if (p_inst.rt_shadow_caster) {
		masks.shadow = state->shadow_groups.group_bits_for_layers(p_inst.layer_mask);
	}
	masks.opaque |= masks.shadow;
	return masks;
}

// Procedural instances are traced through intersection shaders against their AABB BLAS.
void RTTlasBuilder::_add_procedural_instance(const Instance &p_inst, const InstanceMasks &p_masks) {
	// Intersection code comes from ShaderMaterial on material_override.
	const RID material_rid = p_inst.data->material_override;
	if (!material_rid.is_valid()) {
		return;
	}
	const uint32_t shader_id = material_storage->material_get_shader_id(material_rid);
	if (shader_id == 0) {
		return;
	}
	const uint32_t hg_index = rt_shader->register_procedural_shader(shader_id, material_rid);
	if (hg_index == 0) {
		return;
	}
	if (!rt_shader->is_hg_ready_in_bundle(hg_index, ctx.rt_flags)) {
		pending_surface_count++;
		return;
	}

	RTProceduralState *ps = p_inst.rt_procedural;
	if (ps->dirty) {
		const DirtyMark mark = _dirty_mark();
		rt.update_procedural_blas(ps, dirty_blas_list);
		ps->dirty = false;
		_record_stats(mark, 0, 0, 0);
	}
	if (!ps->blas.is_valid() || p_masks.opaque == 0) {
		return;
	}

	rt.tlas_instances.blass.push_back(ps->blas);
	rt.tlas_instances.transforms.push_back(p_inst.transform);
	rt.tlas_instances.sbt_offsets.push_back(hg_index);

	RT_GeometryData geom = {};
	geom.flags = RT_GEOM_FLAG_PROCEDURAL | _self_shadowing_flag(p_masks.opaque);
	geom.vertex_buffer_address = ps->gpu_buffer_address;
	geom.normal_byte_offset = RT_OFFSET_NONE;
	geom.tangent_byte_offset = RT_OFFSET_NONE;
	geom.uv_byte_offset = RT_OFFSET_NONE;
	geom.uv2_byte_offset = RT_OFFSET_NONE;
	geom.color_byte_offset = RT_OFFSET_NONE;
	geom.aabb_size_x = (float)ps->culling_aabb.size.x;
	geom.aabb_size_y = (float)ps->culling_aabb.size.y;
	geom.aabb_size_z = (float)ps->culling_aabb.size.z;
	geom.layers = p_inst.layer_mask;
	geom.instance_uniforms_ofs = uint32_t(p_inst.shader_uniforms_offset);
	const RTMaterialData *mat_data = rt.process_material(material_rid, material_storage->material_get_rt_invalidation_counter(material_rid));
	_rt_set_entry_material(geom, mat_data, 0, 1.0f);
	rt.geometry_data.push_back(geom);
	RT_VALIDATE(RTGeometrySources sources; sources.vertex = ps->gpu_buffer; rt.geometry_sources.push_back(sources));
	rt.tlas_instances.custom_indices.push_back(rt.geometry_data.size() - 1);
	rt.motion_indices.push_back(_moved(p_inst) ? _push_motion_transform(_prev_transform(p_inst)) : -1);

	// Procedural instances disable triangle culling and are opaque.
	const uint32_t inst_flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT |
			RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
	rt.tlas_instances.flags.push_back(inst_flags);
	rt.tlas_instances.masks.push_back(p_masks.opaque);
}

// Resolves materials and warms data caches; compute dispatches and TLAS assembly happen in add_multimesh_surfaces.
void RTTlasBuilder::_gather_mm_surfaces(const Instance &p_inst, const InstanceMasks &p_masks) {
	const bool is_particles = p_inst.data->base_type == RSE::INSTANCE_PARTICLES;
	RTInstanceSource source;
	if (!_rt_instance_source(p_inst.data->base_type, p_inst.data->base, source)) {
		return;
	}
	if (!is_particles) {
		// Populate data cache now - first access triggers GPU readback, safe here.
		mesh_storage->multimesh_get_local_data_ptr(source.base);
	}

	// World-space particle records are drawn without the node transform.
	const Transform3D node_transform = source.world_space ? Transform3D() : p_inst.transform;
	const Transform3D prev_node_transform = source.world_space ? Transform3D() : _prev_transform(p_inst);
	const bool transform_moved = !source.world_space && _moved(p_inst);

	// Particle surface indices restart for every draw pass mesh, so they are keyed by list position.
	uint32_t surface_ordinal = 0;
	for (const SurfaceCache *mm_surf = p_inst.surface_caches; mm_surf; mm_surf = mm_surf->next, surface_ordinal++) {
		const ShaderData::RTBlendClass blend_class = _blend_class(mm_surf->shader, _instance_alpha(p_inst));
		const bool transparent = blend_class != ShaderData::RT_BLEND_OPAQUE;
		const uint8_t tlas_mask = p_masks.for_surface(transparent);
		if (tlas_mask == 0 || !mm_surf->rt_base_pass) {
			continue;
		}

		RTMaterialData *mat_data = rt._surface_material(mm_surf);
		if (!_hit_group_ready(mat_data)) {
			pending_surface_count++;
			continue;
		}

		PendingMMSurface pending;
		const bool uses_trails = (mm_surf->flags & SurfaceCache::FLAG_USES_PARTICLE_TRAILS) && source.trail_steps > 1;
		const bool captured = rt._resolve_captured_mm(mm_surf, captures, pending.capture_entry, pending.capture_action, pending.bake);
		// Trail ribbons are only shaped by the vertex shader, so they can't be traced from mesh vertices.
		if (uses_trails && !captured) {
			if (RenderRaytracing::is_capture_surface(mm_surf, RenderRaytracing::read_capture_mode())) {
				pending_surface_count++;
			} else {
				WARN_PRINT_ONCE("Path tracing skips particle trails while their vertex capture is disabled (vertex_capture_mode or rt_vertex_static).");
			}
			continue;
		}
		pending.source = _rt_drawn_instances(source, uses_trails);
		pending.mm_surf = mm_surf;
		pending.mesh_surface = mm_surf->surface;
		pending.surface_key = is_particles ? surface_ordinal : mm_surf->surface_index;
		pending.surface_counter = mesh_storage->mesh_surface_get_rt_invalidation_counter(mm_surf->surface);
		pending.instance_transform = node_transform;
		pending.prev_instance_transform = prev_node_transform;
		pending.transform_moved = transform_moved;
		pending.raster_mirror = p_inst.mirror;
		pending.aabb = p_inst.transformed_aabb;
		pending.blas_update = rt_resolve_blas_update(p_inst.rt_blas_update, mm_surf->shader->rt_blas_update);
		pending.mat_data = mat_data;
		_fill_surface_key(mm_surf->shader, mat_data, blend_class, tlas_mask, pending.key, pending.material_flags);
		pending.transparent = transparent;
		pending.layer_mask = p_inst.layer_mask;
		pending.instance_uniforms_ofs = uint32_t(p_inst.shader_uniforms_offset);
		pending.instance_alpha = _instance_alpha(p_inst);
		// Captured positions already include the billboard.
		if (!captured && mm_surf->shader && mm_surf->shader->billboard.is_camera_dependent()) {
			pending.billboard = mm_surf->shader->billboard;
			pending.billboard_key = RTMMSurfaceKey{ source.base.get_id(), uint64_t(uintptr_t(&p_inst)), pending.surface_key };
			pending.bake.billboard_index = state->merge_billboards.size();
			state->merge_billboards.push_back(_rt_merge_billboard_params(pending.billboard, source.custom_offset, node_transform, billboard_camera));
			pending.bake.prev_billboard_index = state->merge_billboards.size();
			state->merge_billboards.push_back(_rt_merge_billboard_params(pending.billboard, source.custom_offset, prev_node_transform, prev_billboard_camera));
		}
		pending.bake.bake_motion = rt_material_can_own_velocity(pending.material_flags, _rt_dominant_layer_alpha()) &&
				(captured || source.prev_offset != source.current_offset ||
						(pending.bake.billboard_index != UINT32_MAX && state->merge_billboards[pending.bake.billboard_index] != state->merge_billboards[pending.bake.prev_billboard_index]));
		pending_mm_surfaces.push_back(pending);
	}
}

// TLAS part of record p_index: reused from last frame or planned. True for instances that aren't meshes; false when deferred.
bool RTTlasBuilder::_plan_mesh_record(const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, RTPlanArena &r_arena, uint32_t p_index) {
	if (instance_kinds[p_index] != INSTANCE_MESH) {
		return true;
	}
	const Instance &inst = *static_cast<const Instance *>((*render_data->rt_instances)[p_index]);
	r_arena.instances[p_index].key.surfaces_version = inst.rt_surfaces_version;
	return _reuse_mesh_plan(inst, r_arena, p_index) || _plan_mesh_instance(p_ctx, r_scratch, inst, r_arena, p_index);
}

// Copies last frame's TLAS plan of p_inst into record p_index when its inputs still match. False to plan it anew.
bool RTTlasBuilder::_reuse_mesh_plan(const Instance &p_inst, RTPlanArena &r_arena, uint32_t p_index) {
	const uint32_t prev_index = _rt_plan_prev_index(*state, p_inst.rt_index, p_inst.rt_surfaces_version);
	if (prev_index == UINT32_MAX) {
		return false;
	}
	const RTPlanArena &prev = _rt_prev_plan_arena(*state);
	const RTPlanInstance &cached = prev.instances[prev_index];
	if (cached.flags & RT_PLAN_INSTANCE_DYNAMIC) {
		return false;
	}
	const RTPlanKeyMatch match = rt_plan_key_match(cached.key, prev.versions, r_arena.instances[p_index].key, r_arena.versions);
	const bool current = match == RT_PLAN_KEY_MATCH || (match == RT_PLAN_KEY_GLOBAL_ONLY && _plan_surfaces_current(prev, cached));
	if (!current || !_rt_copy_tlas_plan(prev, prev_index, r_arena, p_index)) {
		return false;
	}
	RT_VALIDATE(_rtv_check_reused_tlas(p_inst, r_arena, p_index));
	return true;
}

// Slow path of a key that only moved in global versions: whether every planned surface's mesh and material are unchanged.
bool RTTlasBuilder::_plan_surfaces_current(const RTPlanArena &p_arena, const RTPlanInstance &p_record) const {
	// The inputs of surfaces left out until their hit group is ready aren't recorded.
	if (p_record.pending_surfaces > 0) {
		return false;
	}
	for (uint32_t s = p_record.range_begin; s < p_record.range_begin + p_record.surface_count; s++) {
		const RTPlanSurface &surface = p_arena.surfaces[s];
		const SurfaceCache *surf = static_cast<const SurfaceCache *>(surface.surf);
		if (ctx.mesh_storage->mesh_surface_get_rt_invalidation_counter(surf->surface) != surface.surface_counter) {
			return false;
		}
		const RID material = rt._surface_material_rid(surf);
		const uint16_t material_counter = ctx.material_storage->material_get_rt_invalidation_counter(material);
		uint32_t material_slot = UINT32_MAX;
		if (material_counter != surface.material_counter || !rt.lookup_material(material, material_counter, material_slot) || material_slot != surface.material_slot) {
			return false;
		}
	}
	return true;
}

#ifdef RT_VALIDATION_ENABLED
void RTTlasBuilder::_rtv_check_reused_tlas(const Instance &p_inst, const RTPlanArena &p_arena, uint32_t p_index) {
	const bool planned = _plan_mesh_instance(ctx, rt.plan_workspace.serial, p_inst, rt._rtv_replan_arena(p_arena, p_index), 0);
	rt._rtv_check_reused_plan(p_arena, p_index, true, planned);
}
#endif

// Plans the traced surfaces of p_inst: material, readiness, BLAS cache key, deformation and grouping.
bool RTTlasBuilder::_plan_mesh_instance(const RTPlanContext &p_ctx, RTPlanScratch &r_scratch, const Instance &p_inst, RTPlanArena &r_arena, uint32_t p_index) {
	RTPlanInstance &record = r_arena.instances[p_index];
	record.group_count = 0;
	record.surface_count = 0;
	record.pending_surfaces = 0;
	// Deformation can start without a key change (first skinning, first capture), so such instances are never reused.
	const uint8_t dynamic = p_inst.mesh_instance.is_valid() ? RT_PLAN_INSTANCE_DYNAMIC : 0;
	record.flags = uint8_t(RT_PLAN_INSTANCE_VALID | dynamic | (record.flags & RT_PLAN_CAPTURE_PART_FLAGS));

	const InstanceMasks masks = _plan_masks(record.key);
	const float instance_alpha = _instance_alpha(p_inst);
	LocalVector<RTSurfaceRecord> &records = r_scratch.records;
	records.clear();
	bool all_hit_groups_ready = true;
	for (const SurfaceCache *surf = p_inst.surface_caches; surf; surf = surf->next) {
		if (!surf->rt_base_pass) {
			continue;
		}
		const ShaderData::RTBlendClass blend_class = _blend_class(surf->shader, instance_alpha);
		const bool transparent = blend_class != ShaderData::RT_BLEND_OPAQUE;
		const uint8_t tlas_mask = masks.for_surface(transparent);
		if (tlas_mask == 0) {
			continue;
		}

		uint32_t material_slot = UINT32_MAX;
		uint16_t material_counter = 0;
		const RTMaterialData *mat_data = rt._plan_surface_material(p_ctx, surf, material_slot, material_counter);
		if (!mat_data) {
			return false;
		}
		if (!_hit_group_ready(mat_data)) {
			all_hit_groups_ready = false;
			record.pending_surfaces++;
			continue;
		}

		records.resize(records.size() + 1);
		RTSurfaceRecord &rec = records[records.size() - 1];
		rec = RTSurfaceRecord();
		RTPlanSurface &surface = rec.surface;
		surface.surf = surf;
		surface.material_slot = material_slot;
		surface.material_counter = material_counter;
		rt._surface_cache_key(surf, surface.cache_key, surface.mesh_rid_version);
		surface.surface_counter = p_ctx.mesh_storage->mesh_surface_get_rt_invalidation_counter(surf->surface);
		surface.surface_index = uint8_t(surf->surface_index);
		surface.deformed_kind = _plan_deformed_kind(p_inst, surf);
		surface.flags = uint8_t(transparent ? RT_PLAN_SURFACE_TRANSPARENT : 0);
		_fill_surface_key(surf->shader, mat_data, blend_class, tlas_mask, rec.key, surface.material_flags);
		// Captured positions already include the billboard.
		if (surf->shader && surface.deformed_kind != RT_PLAN_DEFORMED_CAPTURED) {
			rec.key.billboard = surf->shader->billboard;
		}
		rec.key.groupable = surf->surface_index < 64 && surface.deformed_kind == RT_PLAN_DEFORMED_NONE;
		if (RenderRaytracing::is_capture_surface(surf, p_ctx.capture_mode)) {
			record.flags |= RT_PLAN_INSTANCE_DYNAMIC;
		}
	}
	if (all_hit_groups_ready) {
		record.flags |= RT_PLAN_INSTANCE_HIT_GROUPS_READY;
	}
	ERR_FAIL_COND_V_MSG(records.size() > record.range_capacity, true, "Instance has more surfaces than its rt_surface_count.");

	// Grouping is deferred until every hit group is live, so the group does not rebuild as each one compiles.
	records.sort_custom<RTSurfaceRecordOrder>();
	r_scratch.group_keys.resize(records.size());
	for (uint32_t s = 0; s < records.size(); s++) {
		records[s].key.groupable = records[s].key.groupable && all_hit_groups_ready;
		r_scratch.group_keys[s] = records[s].key;
	}
	rt_assign_surface_groups(r_scratch.group_keys, r_scratch.group_leaders);
	_plan_groups(r_scratch, r_arena, record);
	return true;
}

// Where the surface's per-frame geometry comes from; resolved again by apply, which also commits captures.
uint8_t RTTlasBuilder::_plan_deformed_kind(const Instance &p_inst, const SurfaceCache *p_surf) const {
	if (rt._peek_captured(p_surf, *ctx.captures)) {
		return RT_PLAN_DEFORMED_CAPTURED;
	}
	const bool deformed = p_inst.mesh_instance.is_valid() && ctx.mesh_storage->mesh_instance_get_vertex_buffer(p_inst.mesh_instance, p_surf->surface_index).is_valid();
	return deformed ? RT_PLAN_DEFORMED_MESH_INSTANCE : RT_PLAN_DEFORMED_NONE;
}

// Writes r_scratch.records into r_record's range, grouped by r_scratch.group_leaders: groups in leader order, each
// group's members contiguous and in surface order, which is the order apply adds TLAS entries in.
void RTTlasBuilder::_plan_groups(RTPlanScratch &r_scratch, RTPlanArena &r_arena, RTPlanInstance &r_record) {
	const LocalVector<RTSurfaceRecord> &records = r_scratch.records;
	const LocalVector<uint32_t> &leaders = r_scratch.group_leaders;
	LocalVector<uint32_t> &first_member = r_scratch.group_cursors;
	LocalVector<uint32_t> &order = r_scratch.group_order;
	const uint32_t count = records.size();

	// Counting sort by leader; a leader precedes its members, so offsets assigned at leaders follow leader order.
	first_member.resize(count);
	for (uint32_t s = 0; s < count; s++) {
		first_member[s] = 0;
	}
	for (uint32_t s = 0; s < count; s++) {
		first_member[leaders[s]]++;
	}
	uint32_t offset = 0;
	for (uint32_t s = 0; s < count; s++) {
		if (leaders[s] == s) {
			const uint32_t members = first_member[s];
			first_member[s] = offset;
			offset += members;
		}
	}
	order.resize(count);
	for (uint32_t s = 0; s < count; s++) {
		order[first_member[leaders[s]]++] = s;
	}

	for (uint32_t m = 0; m < count;) {
		const uint32_t leader = leaders[order[m]];
		RTPlanGroup &group = r_arena.groups[r_record.range_begin + r_record.group_count++];
		group = RTPlanGroup();
		group.key = records[leader].key;
		group.transparent = records[leader].surface.flags & RT_PLAN_SURFACE_TRANSPARENT;
		group.surface_begin = uint16_t(m);
		for (; m < count && leaders[order[m]] == leader; m++) {
			const RTSurfaceRecord &rec = records[order[m]];
			r_arena.surfaces[r_record.range_begin + m] = rec.surface;
			const uint64_t bit = rec.surface.surface_index < 64 ? 1ULL << rec.surface.surface_index : 0;
			group.surface_mask |= bit;
			group.opaque_mask |= rec.key.opaque ? bit : 0;
		}
		group.surface_count = uint16_t(m - group.surface_begin);
	}
	r_record.surface_count = uint16_t(count);
}

void RTTlasBuilder::_apply_mesh_instance(const Instance &p_inst, const RTPlanArena &p_arena, const RTPlanInstance &p_record) {
	pending_surface_count += p_record.pending_surfaces;
	if (p_record.surface_count == 0) {
		return;
	}
	const int32_t motion_index = _moved(p_inst) ? _push_motion_transform(_prev_transform(p_inst)) : -1;
	const RTPlanSurface *surfaces = p_arena.surfaces.ptr() + p_record.range_begin;
	for (uint32_t g = 0; g < p_record.group_count; g++) {
		const RTPlanGroup &group = p_arena.groups[p_record.range_begin + g];
		_apply_surface_group(p_inst, group, Span<const RTPlanSurface>(surfaces + group.surface_begin, group.surface_count), motion_index);
	}
}

// Adds one TLAS instance on the group's shared multi-geometry BLAS, or one per member.
void RTTlasBuilder::_apply_surface_group(const Instance &p_inst, const RTPlanGroup &p_group, Span<const RTPlanSurface> p_members, int32_t p_motion_index) {
	// Group members share the key, so one billboard transform covers the whole group.
	const RenderingBillboard::Settings &billboard = p_group.key.billboard;
	Transform3D tlas_transform = p_inst.transform;
	int32_t motion_index = p_motion_index;
	if (billboard.is_camera_dependent()) {
		tlas_transform = RenderingBillboard::compute_world_transform(billboard, p_inst.transform, billboard_camera);
		const Transform3D prev_tlas_transform = RenderingBillboard::compute_world_transform(billboard, _prev_transform(p_inst), prev_billboard_camera);
		motion_index = prev_tlas_transform != tlas_transform ? _push_motion_transform(prev_tlas_transform) : -1;
	}

	const DirtyMark mark = _dirty_mark();
	const uint32_t uniforms_ofs = uint32_t(p_inst.shader_uniforms_offset);
	const float instance_alpha = _instance_alpha(p_inst);
	RTMeshGroupEntry *group = nullptr;
	if (p_members.size() > 1) {
		RTMeshGroupKey group_key;
		group_key.mesh_id = p_inst.data->base.get_id();
		group_key.surface_mask = p_group.surface_mask;
		group_key.opaque_mask = p_group.opaque_mask;
		group = rt._process_mesh_group(group_key, p_members, dirty_blas_list);
	}
	if (group) {
		// Any-hit runs only on members without the opaque geometry flag.
		RTSurfaceGroupKey instance_key = p_group.key;
		instance_key.opaque = p_group.opaque_mask == p_group.surface_mask;
		const uint32_t facing_flags = _push_tlas_instance(tlas_transform, group->blas, instance_key, p_group.transparent, p_inst.mirror);
		for (uint32_t m = 0; m < p_members.size(); m++) {
			const RTPlanSurface &member = p_members[m];
			_push_tlas_entry(group->geometries[m], group->sources[m], facing_flags, rt.touch_material(member.material_slot), member.material_flags, p_inst.layer_mask, uniforms_ofs, instance_alpha, motion_index);
		}
	} else {
		for (const RTPlanSurface &member : p_members) {
			const RTMaterialData *mat_data = rt.touch_material(member.material_slot);
			RTSurfaceData *surf_data = _process_plan_surface(p_inst, member);
			if (!surf_data || !surf_data->blas.is_valid()) {
				continue;
			}
			const bool transparent = member.flags & RT_PLAN_SURFACE_TRANSPARENT;
			const uint32_t facing_flags = _push_tlas_instance(tlas_transform, surf_data->blas, _rt_member_key(p_group, member), transparent, p_inst.mirror);
			_push_tlas_entry(surf_data->geometry, surf_data->sources, facing_flags, mat_data, member.material_flags, p_inst.layer_mask, uniforms_ofs, instance_alpha, motion_index);
		}
	}

	if (collect_render_info) {
		uint32_t prim_count = 0;
		for (const RTPlanSurface &member : p_members) {
			const SurfaceCache *surf = static_cast<const SurfaceCache *>(member.surf);
			prim_count += _rt_indices_to_primitives(surf->primitive, mesh_storage->mesh_surface_get_vertices_drawn_count(surf->surface));
		}
		_record_stats(mark, group ? 1 : uint32_t(p_members.size()), prim_count, prim_count);
	}
}

// Deformed geometry of the surface when it has any, else its cached static BLAS.
RTSurfaceData *RTTlasBuilder::_process_plan_surface(const Instance &p_inst, const RTPlanSurface &p_surface) {
	const SurfaceCache *surf = static_cast<const SurfaceCache *>(p_surface.surf);
	RTSurfaceData *surf_data = nullptr;
	if (p_surface.deformed_kind != RT_PLAN_DEFORMED_NONE) {
		const RTDeformedGeometrySource source = _deformed_source(p_inst, p_surface);
		if (source.is_deformed()) {
			surf_data = rt.process_deformed_surface(surf->surface, source, dirty_blas_list, dirty_blas_update_list);
		}
	}
	return surf_data ? surf_data : rt.process_surface_cached(surf, p_surface.cache_key, p_surface.mesh_rid_version, p_surface.surface_counter, p_inst.transform, dirty_blas_list);
}

// Commits the surface's capture, or reads the skeleton or blend shape output of its mesh instance.
RTDeformedGeometrySource RTTlasBuilder::_deformed_source(const Instance &p_inst, const RTPlanSurface &p_surface) {
	const SurfaceCache *surf = static_cast<const SurfaceCache *>(p_surface.surf);
	RTDeformedGeometrySource source;
	const bool captured = p_surface.deformed_kind == RT_PLAN_DEFORMED_CAPTURED && rt._resolve_captured_source(surf, captures, source);
	if (!captured && p_inst.mesh_instance.is_valid()) {
		source = _rt_mesh_instance_source(p_inst.mesh_instance, surf->surface_index);
		source.cache_handle = &surf->rt_deformed_handle;
	}
	source.surface_counter = p_surface.surface_counter;
	return source;
}

void RTTlasBuilder::_upload_merge_billboards() {
	_rt_prune_handle_map(state->billboard_mm_entries, state->billboard_mm_prune, rt.merged_mm_pool);
	// The merge shader always binds the billboard buffer, so keep one entry even when unused.
	if (state->merge_billboards.is_empty() && !state->merge_billboard_buffer.is_valid()) {
		state->merge_billboards.push_back(RTMergeBillboardParams());
	}
	_rt_update_or_grow_storage(state->merge_billboard_buffer, state->merge_billboard_buffer_capacity,
			state->merge_billboards.ptr(), state->merge_billboards.size() * sizeof(RTMergeBillboardParams), "RT MM merge billboard params");
}

// Resolves the merged entry of every collected surface, then lets the scheduler pick an action for every dynamic one.
void RTTlasBuilder::_resolve_merged_jobs(LocalVector<MergedJob> &r_jobs) {
	r_jobs.resize(pending_mm_surfaces.size());
	LocalVector<uint32_t> dynamic_job_indices;
	rt.dynamic_items.clear();
	const RTViewFrustum frustum(render_data);
	for (uint32_t i = 0; i < pending_mm_surfaces.size(); i++) {
		const PendingMMSurface &pending = pending_mm_surfaces[i];
		const uint64_t triangles = pending.source.gpu_buffer.is_valid() ? _rt_merged_triangle_count(pending.source, pending.mesh_surface, pending.bake.captured) : 0;
		if (triangles == 0) {
			continue;
		}
		MergedJob &job = r_jobs[i];
		job.cache_key = (pending.source.base.get_local_index() << 8) | (pending.surface_key & 0xFF);
		if (pending.bake.captured) {
			// Scheduled with the capture; a new capture first keeps the previous one for motion vectors.
			job.entry = rt.merged_mm_pool.get_or_null(pending.capture_entry);
			ERR_CONTINUE(!job.entry);
			job.action = pending.capture_action;
			if (pending.bake.bake_motion && pending.bake.capture_offset != RT_OFFSET_NONE) {
				rt._snapshot_captured_positions(*job.entry, pending.source.count * mesh_storage->mesh_surface_get_vertex_count(pending.mesh_surface));
			}
			continue;
		}
		const bool camera_dependent = pending.bake.billboard_index != UINT32_MAX;
		RID &handle = camera_dependent ? state->billboard_mm_entries[pending.billboard_key] : rt._shared_merged_mm_handle(pending.source, pending.surface_key);
		job.entry = rt._access_merged_mm_slot(handle);
		if (!pending.source.particles && !camera_dependent) {
			continue;
		}
		RTSchedulerItem item;
		item.slot = _rt_scheduler_slot(handle, RT_SCHEDULER_POOL_MERGED_MM);
		item.gen = _rt_entry_generation(handle);
		item.needs_build = !job.entry->blas_built_once || job.entry->last_mm_count != pending.source.count || job.entry->last_surface_counter != pending.surface_counter;
		item.aabb = pending.aabb;
		item.in_frustum = frustum.intersects(item.aabb);
		item.blas_update = pending.blas_update;
		item.triangle_count = uint32_t(MIN(triangles, uint64_t(UINT32_MAX)));
		rt.dynamic_items.push_back(item);
		dynamic_job_indices.push_back(i);
	}
	rt._schedule_dynamic_items(render_data, state);
	for (uint32_t k = 0; k < dynamic_job_indices.size(); k++) {
		r_jobs[dynamic_job_indices[k]].action = rt.dynamic_actions[k];
	}
}

// Resolves the per-instance data of p_source. False when the data is unusable.
bool RTTlasBuilder::_resolve_instance_data(const RTInstanceSource &p_source, MMInstanceData &r_data) {
	r_data.stride_bytes = p_source.stride * sizeof(float);
	r_data.layout = rt_pack_instance_data_layout(p_source.stride, p_source.color_offset, p_source.custom_offset);
	if (p_source.color_offset == UINT32_MAX && p_source.custom_offset == UINT32_MAX) {
		return true;
	}
	ERR_FAIL_COND_V(!p_source.gpu_buffer.is_valid(), false);
	const uint64_t buffer_address = _rt_buffer_address(p_source.gpu_buffer, r_data.source);
	ERR_FAIL_COND_V(buffer_address == 0, false);
	ERR_FAIL_COND_V(p_source.color_offset != UINT32_MAX && p_source.color_offset + 4 > p_source.stride, false);
	ERR_FAIL_COND_V(p_source.custom_offset != UINT32_MAX && p_source.custom_offset + 4 > p_source.stride, false);
	// These addresses belong to the final viewport geometry, not the shared surface cache.
	r_data.address = buffer_address + uint64_t(p_source.current_offset) * r_data.stride_bytes;
	return true;
}

// Adds the surface as one TLAS instance on the BLAS of all its instances merged. False when the merge bake can't.
bool RTTlasBuilder::_add_merged_mm_surface(const PendingMMSurface &p_pending, const MergedJob &p_job, const MMInstanceData &p_instance_data, RD::ComputeListID p_compute_list) {
	if (!p_job.entry) {
		return false;
	}
	const DirtyMark mark = _dirty_mark();
	RTSurfaceData merged_sd;
	if (!rt._build_merged_mm_blas(p_pending.source, p_pending.mesh_surface, p_pending.surface_counter, *p_job.entry, p_job.cache_key,
				state, p_pending.bake, p_job.action, p_compute_list, dirty_blas_list, dirty_blas_update_list, &merged_sd)) {
		return false;
	}

	RT_GeometryData geom = merged_sd.geometry;
	RTGeometrySources sources = merged_sd.sources;
	if (p_instance_data.address != 0) {
		geom.instance_data_address = p_instance_data.address;
		geom.instance_data_primitive_divisor = merged_sd.geometry.primitive_count / p_pending.source.count;
		geom.instance_data_layout = p_instance_data.layout;
		sources.instance_data = p_instance_data.source;
	}
	const uint32_t facing_flags = _push_tlas_instance(p_pending.instance_transform, merged_sd.blas, p_pending.key, p_pending.transparent, p_pending.raster_mirror);
	_push_tlas_entry(geom, sources, facing_flags, p_pending.mat_data, p_pending.material_flags, p_pending.layer_mask, p_pending.instance_uniforms_ofs, p_pending.instance_alpha,
			p_pending.transform_moved ? _push_motion_transform(p_pending.prev_instance_transform) : -1);
	_record_stats(mark, 1, merged_sd.geometry.primitive_count, merged_sd.geometry.primitive_count);
	return true;
}

// Adds one TLAS instance per MultiMesh instance, all sharing the surface's BLAS.
void RTTlasBuilder::_add_expanded_mm_surface(const PendingMMSurface &p_pending, const MMInstanceData &p_instance_data) {
	const RTInstanceSource &source = p_pending.source;
	const float *mm_data = mesh_storage->multimesh_get_local_data_ptr(source.base);
	if (!mm_data) {
		return;
	}

	const DirtyMark mark = _dirty_mark();
	RTSurfaceData *surf_data = rt.process_surface(p_pending.mm_surf, p_pending.surface_counter, p_pending.instance_transform, dirty_blas_list);
	if (!surf_data || !surf_data->blas.is_valid()) {
		return;
	}

	for (uint32_t mi = 0; mi < source.count; mi++) {
		const float *d = mm_data + (source.current_offset + mi) * source.stride;
		const float *prev_d = mm_data + (source.prev_offset + mi) * source.stride;
		Transform3D world = p_pending.instance_transform * _rt_mm_record_transform(d);
		Transform3D prev_world = p_pending.prev_instance_transform * _rt_mm_record_transform(prev_d);
		if (p_pending.bake.billboard_index != UINT32_MAX) {
			const uint32_t custom_offset = state->merge_billboards[p_pending.bake.billboard_index].custom_offset;
			const bool has_rotation = custom_offset != UINT32_MAX;
			world = RenderingBillboard::compute_world_transform(p_pending.billboard, world, billboard_camera, has_rotation ? d[custom_offset] : 0.0f);
			prev_world = RenderingBillboard::compute_world_transform(p_pending.billboard, prev_world, prev_billboard_camera, has_rotation ? prev_d[custom_offset] : 0.0f);
		}
		const int32_t motion_index = prev_world != world ? _push_motion_transform(prev_world) : -1;

		RT_GeometryData geom = surf_data->geometry;
		RTGeometrySources sources = surf_data->sources;
		if (p_instance_data.address != 0) {
			geom.instance_data_address = p_instance_data.address + uint64_t(mi) * p_instance_data.stride_bytes;
			geom.instance_data_layout = p_instance_data.layout;
			sources.instance_data = p_instance_data.source;
		}
		const uint32_t facing_flags = _push_tlas_instance(world, surf_data->blas, p_pending.key, p_pending.transparent, p_pending.raster_mirror);
		_push_tlas_entry(geom, sources, facing_flags, p_pending.mat_data, p_pending.material_flags, p_pending.layer_mask, p_pending.instance_uniforms_ofs, p_pending.instance_alpha, motion_index);
	}

	if (collect_render_info) {
		const uint32_t vertices = mesh_storage->mesh_surface_get_vertices_drawn_count(p_pending.mesh_surface);
		const uint32_t prim_count = _rt_indices_to_primitives(p_pending.mm_surf->primitive, vertices);
		_record_stats(mark, source.count, prim_count * source.count, prim_count);
	}
}

// Custom hit groups are traced once they are live in the pipeline bundle.
bool RTTlasBuilder::_hit_group_ready(const RTMaterialData *p_mat_data) const {
	return p_mat_data->rt_sbt_offset == 0 || ctx.rt_shader->is_hg_ready_in_bundle(p_mat_data->rt_sbt_offset, ctx.rt_flags);
}

void RTTlasBuilder::_fill_surface_key(const ShaderData *p_shader, const RTMaterialData *p_mat_data, ShaderData::RTBlendClass p_blend_class, uint8_t p_tlas_mask,
		RTSurfaceGroupKey &r_key, uint32_t &r_material_flags) const {
	const bool alpha_scissor = _rt_uses_alpha_scissor(p_shader, p_mat_data, rt_shader);
	r_key.cull_mode = _rt_cull_mode(p_shader);
	r_key.sbt_offset = p_mat_data->rt_sbt_offset;
	r_key.tlas_mask = p_tlas_mask;
	r_key.opaque = !alpha_scissor;
	r_material_flags = _rt_surface_mat_flags(p_shader, p_blend_class);
	if (alpha_scissor) {
		r_material_flags |= RT_MAT_FLAG_ALPHA_SCISSOR;
	}
	if (p_blend_class != ShaderData::RT_BLEND_OPAQUE && _shader_blend_class(p_shader) == ShaderData::RT_BLEND_OPAQUE) {
		r_material_flags |= RT_MAT_FLAG_INSTANCE_FADE;
	}
}

// Custom index is the first per-geometry entry; push the instance before its entries.
// Returns the per-instance geometry flags of its entries; p_raster_mirror is the mirror flag the rasterizer culls the instance with.
uint32_t RTTlasBuilder::_push_tlas_instance(const Transform3D &p_transform, RID p_blas, const RTSurfaceGroupKey &p_key, bool p_transparent, bool p_raster_mirror) {
	const RTTriangleFacing facing = rt_triangle_facing(p_key.cull_mode, p_raster_mirror, p_transform.basis.determinant() < 0, preserve_front_facing);
	RenderRaytracing::RTInstanceArrays &instances = rt.tlas_instances;
	instances.transforms.push_back(p_transform);
	instances.blass.push_back(p_blas);
	instances.custom_indices.push_back(rt.geometry_data.size());
	instances.sbt_offsets.push_back(p_key.sbt_offset);
	instances.flags.push_back(_rt_tlas_flags(p_key, facing));
	instances.masks.push_back(p_key.tlas_mask);
	if (p_transparent && (p_key.tlas_mask & RT_MASK_TRANSPARENT) != 0) {
		rt.tlas_transparent_instances.push_copy(instances, instances.blass.size() - 1, RT_MASK_TRANSPARENT);
	}
	return facing.geometry_flags | _self_shadowing_flag(p_key.tlas_mask);
}

void RTTlasBuilder::_push_tlas_entry(const RT_GeometryData &p_geometry, const RTGeometrySources &p_sources, uint32_t p_facing_flags, const RTMaterialData *p_mat_data,
		uint32_t p_material_flags, uint32_t p_layers, uint32_t p_uniforms_ofs, float p_instance_alpha, int32_t p_motion_index) {
	RT_VALIDATE(rt.geometry_sources.push_back(p_sources));
	rt.geometry_data.push_back(p_geometry);
	RT_GeometryData &geometry = rt.geometry_data[rt.geometry_data.size() - 1];
	geometry.flags |= p_facing_flags;
	geometry.layers = p_layers;
	geometry.instance_uniforms_ofs = p_uniforms_ofs;
	_rt_set_entry_material(geometry, p_mat_data, p_material_flags, p_instance_alpha);
	rt.motion_indices.push_back(p_motion_index);
}

int32_t RTTlasBuilder::_push_motion_transform(const Transform3D &p_prev_transform) {
	RT_InstanceMotionData motion = {};
	RendererRD::MaterialStorage::store_transform_transposed_3x4(p_prev_transform, motion.prev_object_to_world);
	rt.motion_transforms.push_back(motion);
	return int32_t(rt.motion_transforms.size() - 1);
}

void RTTlasBuilder::_record_stats(const DirtyMark &p_mark, uint32_t p_instances, uint32_t p_primitives, uint32_t p_blas_primitives) {
	if (!collect_render_info) {
		return;
	}
	const uint32_t builds = dirty_blas_list.size() - p_mark.builds;
	const uint32_t refits = dirty_blas_update_list.size() - p_mark.refits;
	tlas_instance_count += p_instances;
	tlas_primitive_count += p_primitives;
	rt_blas_builds += builds;
	rt_blas_refits += refits;
	rt_triangles_built += builds > 0 ? p_blas_primitives : 0;
	rt_triangles_refit += refits > 0 ? p_blas_primitives : 0;
}

RTViewportState *RenderRaytracing::build_tlas(const RenderDataRD *p_render_data, uint32_t p_rt_flags, LocalVector<RTCaptureRequest> &r_captures) {
	if (!p_render_data || !p_render_data->rt_instances) {
		return nullptr;
	}

	RTViewportState *state = _get_or_create_viewport_state(p_render_data);
	if (!state) {
		return nullptr;
	}

	prepare_frame();
	mesh_group_build_first_use = mesh_group_use_counter;
	state->merge_billboards.clear();
#ifdef RT_VALIDATION_ENABLED
	_rtv_begin_view();
#endif

	// Builds bundle if needed; live_ready_mask drives TLAS inclusion below.
	SceneShaderRaytracing::get_singleton()->ensure_pipeline_bundle(p_rt_flags);

	_refresh_custom_shader_slots();
	const RTPlanContext ctx = _make_plan_context(p_render_data, p_rt_flags, &r_captures);
	RTTlasBuilder builder(*this, p_render_data, ctx, r_captures, state);
	builder.gather_instances(_open_plan_arena(state, p_render_data, ctx));
	builder.add_multimesh_surfaces();
	builder.finish();

	SceneShaderRaytracing::get_singleton()->finalize_custom_shaders();

	state->transparent_instance_count = tlas_transparent_instances.blass.size();

	if (p_render_data->environment.is_valid() &&
			RendererEnvironmentStorage::get_singleton()->environment_get_pathtracing_debug_mode(p_render_data->environment) == SceneShaderRaytracing::RT_DEBUG_DYNAMIC_GEOMETRY) {
		_annotate_dynamic_geometry(builder.dirty_blas_list, builder.dirty_blas_update_list);
	}

	// A TLAS that failed to build must not be traced: its instances would index this frame's geometry and material tables.
	const Error build_err = build_acceleration_structures(state, builder.dirty_blas_list, builder.dirty_blas_update_list);
	_record_memory_info(p_render_data);
	if (build_err != OK) {
		return nullptr;
	}
#ifdef RT_VALIDATION_ENABLED
	// Checked after the builds, so a failing view still keeps the caches that queued them consistent.
	if (rt_validation().strict && !_rtv_validate_view(state)) {
		return nullptr;
	}
#endif
	finalize_buffers(state);

	return state;
}

void RenderRaytracing::_record_memory_info(const RenderDataRD *p_render_data) const {
	if (!p_render_data->render_info) {
		return;
	}
	const auto kb = [](uint64_t p_bytes) {
		return int(MIN((p_bytes + 999) / 1000, uint64_t(INT32_MAX)));
	};
	const RD::AccelerationStructureScratchUsage scratch = RD::get_singleton()->acceleration_structure_get_scratch_usage();
	int *info = p_render_data->render_info->info[RSE::VIEWPORT_RENDER_INFO_TYPE_VISIBLE];
	info[RSE::VIEWPORT_RENDER_INFO_RT_SCRATCH_USED_KB] = kb(scratch.used);
	info[RSE::VIEWPORT_RENDER_INFO_RT_SCRATCH_SIZE_KB] = kb(scratch.size);
	info[RSE::VIEWPORT_RENDER_INFO_RT_SCRATCH_ROLLOVERS] = int(scratch.rollovers);
	info[RSE::VIEWPORT_RENDER_INFO_RT_CAPTURE_ARENA_USED_KB] = kb(capture_arena_used);
	info[RSE::VIEWPORT_RENDER_INFO_RT_CAPTURE_ARENA_SIZE_KB] = kb(capture_arena_capacity);
}

void RenderRaytracing::_annotate_dynamic_geometry(const LocalVector<RID> &p_built, const LocalVector<RID> &p_refit) {
	dynamic_geometry_debug.begin_frame(RSG::rasterizer->get_frame_number());
	for (const RID &blas : p_built) {
		dynamic_geometry_debug.record_build(blas);
	}
	for (const RID &blas : p_refit) {
		dynamic_geometry_debug.record_refit(blas);
	}
	// Instances own consecutive geometry entries, starting at their custom index.
	const uint32_t instance_count = tlas_instances.blass.size();
	for (uint32_t i = 0; i < instance_count; i++) {
		const uint32_t end = i + 1 < instance_count ? tlas_instances.custom_indices[i + 1] : geometry_data.size();
		for (uint32_t g = tlas_instances.custom_indices[i]; g < end; g++) {
			RT_GeometryData &geometry = geometry_data[g];
			const bool captured = geometry.flags & RT_GEOM_FLAG_CAPTURED;
			const bool dynamic = geometry.flags & RT_GEOM_FLAG_DEFORMED;
			geometry.debug_state = RTDynamicGeometryDebug::pack(dynamic_geometry_debug.observe(tlas_instances.blass[i], captured, motion_indices[g] >= 0, dynamic));
		}
	}
}

// ---------------------------------------------------------------------------
// Decal volumes
// ---------------------------------------------------------------------------

void RenderRaytracing::_ensure_decal_box_blas() {
	if (decal_box_blas.is_valid()) {
		return;
	}
	RD *rd = RD::get_singleton();

	// uv_local box of a decal: x/z in [0, 1], y in [-1, 1].
	const float box[6] = { 0.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f };
	Vector<uint8_t> bytes;
	bytes.resize(sizeof(box));
	memcpy(bytes.ptrw(), box, sizeof(box));
	decal_box_aabb_buffer = rd->storage_buffer_create(sizeof(box), bytes, 0,
			RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT);
	ERR_FAIL_COND(!decal_box_aabb_buffer.is_valid());
	rd->set_resource_name(decal_box_aabb_buffer, "RT Decal Box AABB");

	RD::AccelerationStructureGeometry geom;
	geom.type = RD::AccelerationStructureGeometry::TYPE_AABBS;
	geom.geometry.aabbs.buffer = decal_box_aabb_buffer;
	geom.geometry.aabbs.count = 1;
	geom.geometry.aabbs.stride = sizeof(box);
	decal_box_blas = rd->blas_create({ &geom, 1 }, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	ERR_FAIL_COND(!decal_box_blas.is_valid());
	rd->set_resource_name(decal_box_blas, "RT Decal Box BLAS");
	ERR_FAIL_COND_MSG(_rt_blas_build(decal_box_blas) != OK, "Couldn't build the decal box BLAS; decals won't be path traced.");
}

void RenderRaytracing::_free_decal_box_blas() {
	// The BLAS goes first: freeing its input buffer frees it implicitly.
	if (decal_box_blas.is_valid()) {
		RD::get_singleton()->free_rid(decal_box_blas);
		decal_box_blas = RID();
	}
	if (decal_box_aabb_buffer.is_valid()) {
		RD::get_singleton()->free_rid(decal_box_aabb_buffer);
		decal_box_aabb_buffer = RID();
	}
}

const RenderRaytracing::DecalCacheEntry &RenderRaytracing::_get_decal_cache_entry(const RendererRD::TextureStorage::DecalSortInfo &p_info) {
	const uint32_t index = p_info.instance.get_local_index();
	if (index >= decal_cache.size()) {
		decal_cache.resize(index + 1);
	}
	DecalCacheEntry &entry = decal_cache[index];
	const uint64_t bindless_version = bindless_block ? bindless_block->get_version() : 0;
	if (entry.instance == p_info.instance && entry.version == p_info.version && entry.bindless_version == bindless_version) {
		return entry;
	}

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::TextureStorage::DecalWorldInfo info;
	texture_storage->decal_instance_get_world_info(p_info.instance, info);

	RT_DecalData &d = entry.data;
	RendererRD::MaterialStorage::store_transform_transposed_3x4(info.world_to_box, d.world_to_box);
	for (int i = 0; i < 3; i++) {
		const Vector3 column = info.normal_basis.get_column(i);
		d.normal_basis[i * 4 + 0] = column.x;
		d.normal_basis[i * 4 + 1] = column.y;
		d.normal_basis[i * 4 + 2] = column.z;
		d.normal_basis[i * 4 + 3] = 0.0f;
		d.normal[i] = info.normal[i];
	}
	d.normal_fade = info.normal_fade;
	d.modulate[0] = info.modulate.r;
	d.modulate[1] = info.modulate.g;
	d.modulate[2] = info.modulate.b;
	d.modulate[3] = info.modulate.a;
	d.emission_energy = info.emission_energy;
	d.albedo_mix = info.albedo_mix;
	d.upper_fade = info.upper_fade;
	d.lower_fade = info.lower_fade;
	d.mask = info.cull_mask;
	for (int i = 0; i < RSE::DECAL_TEXTURE_MAX; i++) {
		const bool srgb = i == RSE::DECAL_TEXTURE_ALBEDO || i == RSE::DECAL_TEXTURE_EMISSION;
		const RID rd_texture = info.textures[i].is_valid() ? texture_storage->texture_get_rd_texture(info.textures[i], srgb) : RID();
		d.textures[i] = (bindless_block && rd_texture.is_valid()) ? bindless_block->add_texture(rd_texture) : RT_DECAL_NO_TEXTURE;
	}

	entry.instance = p_info.instance;
	entry.version = p_info.version;
	// add_texture() may have bumped the version; the indices above stay valid.
	entry.bindless_version = bindless_block ? bindless_block->get_version() : 0;
	entry.box_transform = info.box_transform;
	return entry;
}

// Elements behind the camera fade from 1 to RT_BEHIND_CAMERA_PENALTY over RT_BEHIND_CAMERA_BUFFER meters.
static constexpr float RT_BEHIND_CAMERA_PENALTY = 0.05f;
static constexpr float RT_BEHIND_CAMERA_BUFFER = 20.0f;
// Bonus for decals selected last frame, so the set doesn't flicker at the budget edge.
static constexpr float RT_DECAL_HYSTERESIS = 1.25f;

// p_forward_depth is positive in front of the camera.
static float _rt_behind_camera_factor(float p_forward_depth) {
	const float t = CLAMP((p_forward_depth + RT_BEHIND_CAMERA_BUFFER) / RT_BEHIND_CAMERA_BUFFER, 0.0f, 1.0f);
	return Math::lerp(RT_BEHIND_CAMERA_PENALTY, 1.0f, t);
}

// Projected solid angle proxy of a bounding sphere, clamped when the camera is inside it.
static float _rt_solid_angle(float p_distance, float p_radius) {
	const float d = MAX(p_distance, p_radius);
	if (d <= 0.0f) {
		return 0.0f;
	}
	return float(Math::PI) * p_radius * p_radius / (d * d);
}

static float _rt_decal_score(float p_solid_angle, float p_fade, float p_forward_depth, bool p_was_selected) {
	return p_solid_angle * p_fade * _rt_behind_camera_factor(p_forward_depth) * (p_was_selected ? RT_DECAL_HYSTERESIS : 1.0f);
}

struct RTDecalByScore {
	bool operator()(const RTDecalCandidate &p_a, const RTDecalCandidate &p_b) const { return p_a.score > p_b.score; }
};

struct RTDecalSortKeyBits {
	uint32_t operator()(const RTDecalCandidate &p_candidate) const { return radix_sort_key_float(p_candidate.sort_key); }
};

// Keeps the p_budget highest scoring candidates, ordered by blend key.
static void _rt_select_decals(LocalVector<RTDecalCandidate> &r_candidates, uint32_t p_budget, LocalVector<RTDecalCandidate> &r_scratch) {
	if (r_candidates.size() > p_budget) {
		SortArray<RTDecalCandidate, RTDecalByScore> by_score;
		by_score.nth_element(0, r_candidates.size(), p_budget, r_candidates.ptr());
		r_candidates.resize(p_budget);
	}
	r_scratch.resize(r_candidates.size());
	RadixSort<RTDecalCandidate, RTDecalSortKeyBits> by_key;
	by_key.sort(r_candidates.ptr(), r_scratch.ptr(), r_candidates.size());
}

void RenderRaytracing::build_decal_volumes(RTViewportState *p_state, const RenderDataRD *p_render_data) {
	ERR_FAIL_NULL(p_state);
	ERR_FAIL_NULL(p_render_data);
	RENDER_TIMESTAMP("RT Decal Volumes");

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	const uint32_t budget = (uint32_t)MAX(0, GLOBAL_GET_CACHED(int, "rendering/pathtracing/max_decals"));
	const Transform3D &cam_xform = p_render_data->scene_data->cam_transform;
	const Vector3 cam_forward = -cam_xform.basis.get_column(2).normalized();

	const uint32_t source_count = p_render_data->rt_decals ? p_render_data->rt_decals->size() : 0;
	decal_infos.resize(source_count);
	uint32_t valid_count = 0;
	for (uint32_t i = 0; i < source_count; i++) {
		if (texture_storage->decal_instance_get_sort_info((*p_render_data->rt_decals)[i], cam_xform.origin, decal_infos[valid_count])) {
			valid_count++;
		}
	}
	decal_infos.resize(valid_count);

	// Scores only matter when the budget drops decals.
	const bool over_budget = decal_infos.size() > budget;
	decal_candidates.resize(decal_infos.size());
	for (uint32_t i = 0; i < decal_infos.size(); i++) {
		const RendererRD::TextureStorage::DecalSortInfo &info = decal_infos[i];
		RTDecalCandidate &candidate = decal_candidates[i];
		candidate.source = i;
		candidate.sort_key = info.sort_key;
		candidate.score = over_budget
				? _rt_decal_score(_rt_solid_angle(info.distance, info.radius), info.fade, (info.position - cam_xform.origin).dot(cam_forward), p_state->selected_decals.has(info.instance))
				: 0.0f;
	}
	decal_sort_scratch.resize(MIN(decal_candidates.size(), budget));
	_rt_select_decals(decal_candidates, budget, decal_sort_scratch);

	p_state->selected_decals.clear();
	if (over_budget) {
		for (const RTDecalCandidate &candidate : decal_candidates) {
			p_state->selected_decals.insert(decal_infos[candidate.source].instance);
		}
	}

	_ensure_decal_box_blas();

	const uint32_t count = decal_candidates.size();
	decal_data.resize(MAX(count, 1u));
	decal_instances.blass.resize(count);
	decal_instances.transforms.resize(count);
	decal_instances.flags.resize(count);
	decal_instances.masks.resize(count);
	decal_instances.sbt_offsets.resize(count);
	decal_instances.custom_indices.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const RendererRD::TextureStorage::DecalSortInfo &info = decal_infos[decal_candidates[i].source];
		const DecalCacheEntry &entry = _get_decal_cache_entry(info);
		RT_DecalData &d = decal_data[i];
		d = entry.data;
		d.modulate[3] *= info.fade;
		d.emission_energy *= info.fade;
		decal_instances.transforms[i] = entry.box_transform;
		decal_instances.blass[i] = decal_box_blas;
		decal_instances.flags[i] = 0;
		decal_instances.masks[i] = RT_VOLUME_MASK_DECAL;
		decal_instances.sbt_offsets[i] = 0;
		decal_instances.custom_indices[i] = i;
	}
	p_state->decal_count = count;

	const uint32_t buffer_size = decal_data.size() * sizeof(RT_DecalData);
	if (buffer_size > p_state->decal_buffer_capacity) {
		if (p_state->decal_buffer.is_valid()) {
			RD::get_singleton()->free_rid(p_state->decal_buffer);
		}
		p_state->decal_buffer_capacity = buffer_size;
		p_state->decal_buffer = RD::get_singleton()->storage_buffer_create(buffer_size);
		RD::get_singleton()->set_resource_name(p_state->decal_buffer, "RT Decal Buffer");
	}
	if (count > 0) {
		RD::get_singleton()->buffer_update(p_state->decal_buffer, 0, count * sizeof(RT_DecalData), decal_data.ptr());
	}

	// Built even when empty so the closest-hit binding stays valid.
	_build_one_tlas(p_state->decal_tlas, p_state->decal_tlas_max_instances, decal_instances, "RT Decal TLAS");
}

// ---------------------------------------------------------------------------
// Light gathering
// ---------------------------------------------------------------------------

uint32_t RenderRaytracing::gather_lights(const RenderDataRD *p_render_data, const RTViewportState *p_state, RT_LightData *r_light_data, uint32_t p_max_lights) {
	uint32_t rt_light_count = 0;

	if (!p_render_data || !p_render_data->lights) {
		return rt_light_count;
	}

	RendererRD::LightStorage *ls = RendererRD::LightStorage::get_singleton();
	const Transform3D &cam_xform = p_render_data->scene_data->cam_transform;
	const Vector3 cam_pos = cam_xform.origin;
	const Vector3 cam_forward = -cam_xform.basis.get_column(2).normalized(); // -Z is forward in Godot.

	// Compute light energy matching rasterizer conventions
	auto compute_light_energy = [&](RID p_base, RSE::LightType p_type) -> float {
		float sign = ls->light_is_negative(p_base) ? -1.0f : 1.0f;
		float e = sign * ls->light_get_param(p_base, RSE::LIGHT_PARAM_ENERGY);
		if (owner->is_using_physical_light_units()) {
			e *= ls->light_get_param(p_base, RSE::LIGHT_PARAM_INTENSITY);
			if (p_type == RSE::LIGHT_OMNI) {
				e *= 1.0f / (Math::PI * 4.0f);
			} else if (p_type == RSE::LIGHT_SPOT) {
				e *= 1.0f / Math::PI;
			}
		} else {
			e *= Math::PI;
		}
		if (p_render_data->camera_attributes.is_valid()) {
			e *= RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
		}
		return e;
	};
	// Fields shared by every light type; p_energy includes any distance fade.
	auto store_common = [&](RT_LightData &r_ld, RID p_base, RTLightType p_type, const Vector3 &p_position, float p_energy, float p_radius, float p_specular, float p_attenuation, float p_shadow_opacity) {
		r_ld.position[0] = p_position.x;
		r_ld.position[1] = p_position.y;
		r_ld.position[2] = p_position.z;
		const Color emission = ls->light_get_color(p_base).srgb_to_linear() * p_energy;
		r_ld.emission_rg.r = rt_half(emission.r);
		r_ld.emission_rg.g = rt_half(emission.g);
		r_ld.emission_b_radius.b = rt_half(emission.b);
		r_ld.emission_b_radius.radius = rt_half(p_radius);
		r_ld.energy.specular = rt_half(p_specular);
		r_ld.energy.indirect = rt_half(ls->light_get_param(p_base, RSE::LIGHT_PARAM_INDIRECT_ENERGY));
		r_ld.masks.cull_mask = ls->light_get_cull_mask(p_base);
		r_ld.masks.type = p_type;
		r_ld.masks.shadow_ray_mask = p_state ? p_state->shadow_groups.ray_mask_for_caster_mask(ls->light_get_shadow_caster_mask(p_base)) : uint32_t(RT_SHADOW_GROUPS_MASK);
		r_ld.attenuation_shadow.attenuation = rt_half(p_attenuation);
		r_ld.attenuation_shadow.shadow_opacity = rt_half(p_shadow_opacity);
	};

	// Scoring helper: approximate power/solid-angle contribution.
	struct LightScore {
		RID light_instance;
		float score;
		float distance_fade;
		float camera_distance;
	};

	LocalVector<LightScore> positional_lights;

	// Helper: score a positional light and add to candidates.
	auto score_positional_light = [&](RID light_instance) {
		RID base = ls->light_instance_get_base_light(light_instance);
		Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		Vector3 light_pos = xform.origin;
		Vector3 to_light = light_pos - cam_pos;
		// Same camera distance fade as the rasterizer, which also drops fully faded lights.
		const float camera_distance = to_light.length();
		const float distance_fade = ls->light_get_distance_fade(base, camera_distance);
		if (distance_fade <= 0.0f) {
			return;
		}
		float dist_sq = MAX(to_light.dot(to_light), 0.01f);
		Color color = ls->light_get_color(base);
		float energy = ls->light_get_param(base, RSE::LIGHT_PARAM_ENERGY);
		float lum = color.r * 0.2126f + color.g * 0.7152f + color.b * 0.0722f;

		// Range factor: strongly prefer lights with a large influence radius.
		// Lights with no range limit (range == 0) are treated as infinite.
		float range = ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE);
		float range_factor = (range > 0.0f) ? (range * range) : 1.0e12f;

		// Lights behind the camera are deprioritized.
		float frustum_factor = _rt_behind_camera_factor(to_light.dot(cam_forward));

		float score = (energy * distance_fade * lum * range_factor * frustum_factor) / dist_sq;

		LightScore ls_entry = {};
		ls_entry.light_instance = light_instance;
		ls_entry.score = score;
		ls_entry.distance_fade = distance_fade;
		ls_entry.camera_distance = camera_distance;
		positional_lights.push_back(ls_entry);
	};

	// Directional lights from the frustum-culled list (they're global, always included).
	// They must precede all positional lights: lights_directional_count() relies on it.
	const PagedArray<RID> &lights = *p_render_data->lights;
	for (uint32_t li = 0; li < (uint32_t)lights.size(); li++) {
		RID light_instance = lights[li];
		RID base = ls->light_instance_get_base_light(light_instance);
		RSE::LightType type = ls->light_get_type(base);

		if (type != RSE::LIGHT_DIRECTIONAL) {
			continue;
		}
		if (rt_light_count >= p_max_lights) {
			break;
		}
		const Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		store_common(r_light_data[rt_light_count], base, RT_LIGHT_TYPE_DIRECTIONAL, -xform.basis.get_column(2).normalized(),
				compute_light_energy(base, RSE::LIGHT_DIRECTIONAL),
				Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE) * 0.5f), // Half-angle in radians.
				ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR), 0.0f,
				ls->light_get_path_traced_shadow_opacity(base, 0.0f));
		rt_light_count++;
	}

	// Positional lights from the AABB-culled RT list (superset of frustum).
	if (p_render_data->rt_lights) {
		const PagedArray<RID> &rt_lights = *p_render_data->rt_lights;
		for (uint32_t li = 0; li < (uint32_t)rt_lights.size(); li++) {
			score_positional_light(rt_lights[li]);
		}
	}

	// Sort all positional lights by score descending.
	struct LightScoreComparator {
		bool operator()(const LightScore &a, const LightScore &b) const {
			return a.score > b.score;
		}
	};
	positional_lights.sort_custom<LightScoreComparator>();

	// Fill remaining slots with top positional lights.
	for (uint32_t i = 0; i < positional_lights.size() && rt_light_count < p_max_lights; i++) {
		RID light_instance = positional_lights[i].light_instance;
		RID base = ls->light_instance_get_base_light(light_instance);
		RSE::LightType type = ls->light_get_type(base);

		RT_LightData &ld = r_light_data[rt_light_count];
		const Transform3D xform = ls->light_instance_get_base_transform(light_instance);
		store_common(ld, base, type == RSE::LIGHT_SPOT ? RT_LIGHT_TYPE_SPOT : RT_LIGHT_TYPE_OMNI, xform.origin,
				compute_light_energy(base, type) * positional_lights[i].distance_fade,
				ls->light_get_param(base, RSE::LIGHT_PARAM_SIZE),
				ls->light_get_param(base, RSE::LIGHT_PARAM_SPECULAR) * 2.0f, // Matches rasterizer convention (light_storage.cpp), normalizes 0.5 default to 1.0.
				ls->light_get_param(base, RSE::LIGHT_PARAM_ATTENUATION),
				ls->light_get_path_traced_shadow_opacity(base, positional_lights[i].camera_distance));
		ld.range = MAX(ls->light_get_param(base, RSE::LIGHT_PARAM_RANGE), 0.0f);

		const Quaternion rotation = xform.basis.get_rotation_quaternion();
		ld.orientation_xy.x = rt_snorm16(rotation.x);
		ld.orientation_xy.y = rt_snorm16(rotation.y);
		ld.orientation_zw.z = rt_snorm16(rotation.z);
		ld.orientation_zw.w = rt_snorm16(rotation.w);
		if (type == RSE::LIGHT_SPOT) {
			// 2 sin^2(a/2) keeps 1 - cos(a) precise for narrow cones.
			const float half_angle = Math::deg_to_rad(ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ANGLE)) * 0.5f;
			ld.spot.one_minus_cos = rt_half(2.0f * Math::sin(half_angle) * Math::sin(half_angle));
			ld.spot.inv_attenuation = rt_half(1.0f / MAX(0.001f, ls->light_get_param(base, RSE::LIGHT_PARAM_SPOT_ATTENUATION)));
		}
		ld.projector_texture.index = _light_projector_texture(base);
		rt_light_count++;
	}

	return rt_light_count;
}

uint32_t RenderRaytracing::_light_projector_texture(RID p_light) {
	const RID projector = RendererRD::LightStorage::get_singleton()->light_get_projector(p_light);
	if (projector.is_null() || !bindless_block || !bindless_block->is_initialized()) {
		return RT_LIGHT_NO_PROJECTOR;
	}
	// sRGB view, as the rasterizer samples projectors from decal_atlas_srgb.
	const RID rd_texture = RendererRD::TextureStorage::get_singleton()->texture_get_rd_texture(projector, true);
	return rd_texture.is_valid() ? bindless_block->add_texture(rd_texture) : RT_LIGHT_NO_PROJECTOR;
}

// ---------------------------------------------------------------------------
// Uniform set update
// ---------------------------------------------------------------------------

void RenderRaytracing::update_scene_ubo(RTViewportState *p_state, const RenderDataRD *p_render_data, const Size2i &p_screen_size, const Color &p_default_bg_color) {
	ERR_FAIL_NULL(p_state);
	ERR_FAIL_NULL(p_render_data);
	ERR_FAIL_NULL(p_render_data->scene_data);

	if (p_state->scene_ubo.is_null()) {
		p_state->scene_ubo = p_render_data->scene_data->create_uniform_buffer();
		RD::get_singleton()->set_resource_name(p_state->scene_ubo, "RT Scene Data UBO");
	}

	RID env = owner->is_environment(p_render_data->environment) ? p_render_data->environment : RID();
	RID reflection_probe_instance = p_render_data->reflection_probe.is_valid() ? RendererRD::LightStorage::get_singleton()->reflection_probe_instance_get_probe(p_render_data->reflection_probe) : RID();
	float luminance_multiplier = p_render_data->render_buffers.is_valid() ? p_render_data->render_buffers->get_luminance_multiplier() : 1.0;

	// Keep the arguments in sync with the _setup_environment() call in the PT camera pass.
	p_render_data->scene_data->update_ubo(p_state->scene_ubo, owner->get_debug_draw_mode(), env, reflection_probe_instance, p_render_data->camera_attributes, false, p_screen_size, p_screen_size, p_default_bg_color, luminance_multiplier, false, false);
}

RID RenderRaytracing::update_uniform_set(RTViewportState *p_state, const RenderDataRD *p_render_data, uint32_t p_rt_flags) {
	ERR_FAIL_NULL_V(p_state, RID());

	Ref<RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
	if (p_render_data && p_render_data->render_buffers.is_valid()) {
		if (p_render_data->render_buffers->has_custom_data(RB_SCOPE_FORWARD_CLUSTERED)) {
			rb_data = p_render_data->render_buffers->get_custom_data(RB_SCOPE_FORWARD_CLUSTERED);
		}
	}

	if (rb_data.is_null()) {
		return RID();
	}

	RenderSceneBuffersRD *rb = p_render_data->render_buffers.ptr();

	// SET 0 indices must match raytracing_common_inc.glsl / scene_raytracing_raygen.glsl / samplers includes.
	Vector<RD::Uniform> uniforms;

	{
		RD::Uniform u;
		u.binding = 0;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		rt_ensure_textures(rb);
		u.append_id(rt_get_texture(rb));
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 1;
		u.uniform_type = RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE;
		ERR_FAIL_COND_V(p_state->tlas == RID(), RID());
		u.append_id(p_state->tlas);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 2;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		ERR_FAIL_COND_V(p_state->scene_ubo.is_null(), RID());
		u.append_id(p_state->scene_ubo);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 3;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->geometry_buffer.is_valid()) {
			u.append_id(p_state->geometry_buffer);
		} else {
			// Use a default buffer if no geometry
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 31;
		u.uniform_type = RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE;
		ERR_FAIL_COND_V(p_state->tlas_transparent == RID(), RID());
		u.append_id(p_state->tlas_transparent);
		uniforms.push_back(u);
	}

	// Binding 4: Per-instance motion index buffer (int32 per TLAS instance, -1 = no motion).
	{
		RD::Uniform u;
		u.binding = 4;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->motion_index_buffer.is_valid()) {
			u.append_id(p_state->motion_index_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Motion transforms past sampler block growth reservation (bindings 28-31).
	{
		RD::Uniform u;
		u.binding = 32;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->motion_transform_buffer.is_valid()) {
			u.append_id(p_state->motion_transform_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 5: Material buffer.
	{
		RD::Uniform u;
		u.binding = 5;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (material_table.get_buffer().is_valid()) {
			u.append_id(material_table.get_buffer());
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 6: Raytracing params + unjittered VP matrices.
	{
		struct {
			//TODO: I see no reason why we need to pass this as a float array
			float params[SceneShaderRaytracing::RT_PARAM_COUNT];
			float prev_vp_unjittered[16];
			float curr_vp_unjittered[16];
		} rt_ubo = {};

		if (p_render_data && p_render_data->environment.is_valid()) {
			RendererEnvironmentStorage *env_storage = RendererEnvironmentStorage::get_singleton();
			RID env = p_render_data->environment;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_VIS_MODE] = (float)env_storage->environment_get_pathtracing_debug_mode(env);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_SAMPLE_COUNT] = (float)env_storage->environment_get_pathtracing_samples_per_pixel(env);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_MAX_BOUNCES] = (float)env_storage->environment_get_pathtracing_max_bounces(env);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_DENOISER] = (float)(int)SceneShaderRaytracing::get_effective_denoiser(env);
		}

		// rt_params layout (see RaytracingParamIndex enum):
		// [0] = VIS_MODE, [1] = SAMPLE_COUNT, [2] = MAX_BOUNCES,
		// [3] = DLSS_RR_ENABLED, [14] = LIGHT_COUNT, [15] = FRAME_INDEX
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_MAX_TRANSPARENCY_LAYERS] = (float)GLOBAL_GET_CACHED(int, "rendering/pathtracing/max_transparency_layers");
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_MAX_TRANSPARENCY_LAYERS_REFLECTION] = (float)GLOBAL_GET_CACHED(int, "rendering/pathtracing/max_transparency_layers_reflection");
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_MAX_TRANSPARENCY_LAYERS_INDIRECT] = (float)GLOBAL_GET_CACHED(int, "rendering/pathtracing/max_transparency_layers_indirect");
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_TRANSPARENCY_MAX_BOUNCE] = (float)GLOBAL_GET_CACHED(int, "rendering/pathtracing/transparency_max_bounce");
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_TRANSPARENT_COUNT] = (float)p_state->transparent_instance_count;
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_TRANSPARENCY_INDIRECT_RANGE] = (float)GLOBAL_GET_CACHED(double, "rendering/pathtracing/transparency_indirect_range");
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_TRANSPARENCY_INDIRECT_FADE] = (float)GLOBAL_GET_CACHED(double, "rendering/pathtracing/transparency_indirect_fade");
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_SKY_SCREEN_ENABLED] = p_state->sky_screen_enabled ? 1.0f : 0.0f;
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_FRAME_INDEX] = float(p_state->frame_counter++);
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_DECAL_COUNT] = float(p_state->decal_count);
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_DECAL_MODE] = float(GLOBAL_GET_CACHED(int, "rendering/pathtracing/decals"));
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_TRANSPARENCY_DOMINANT_LAYER_ALPHA] = _rt_dominant_layer_alpha();

		// Unjittered VP for motion vectors (matches raster convention).
		{
			Projection correction;
			correction.set_depth_correction(true);

			Projection prev_vp = (correction * p_render_data->scene_data->prev_cam_projection) * Projection(p_render_data->scene_data->prev_cam_transform.affine_inverse());
			RendererRD::MaterialStorage::store_camera(prev_vp, rt_ubo.prev_vp_unjittered);

			Projection curr_vp = (correction * p_render_data->scene_data->cam_projection) * Projection(p_render_data->scene_data->cam_transform.affine_inverse());
			RendererRD::MaterialStorage::store_camera(curr_vp, rt_ubo.curr_vp_unjittered);
		}

		Ref<RendererRD::Fog::VolumetricFog> vfog = _rt_get_volumetric_fog(p_render_data);
		if (vfog.is_valid()) {
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_VOLUMETRIC_FOG_INV_LENGTH] = vfog->length > 0.0 ? 1.0 / vfog->length : 1.0;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_VOLUMETRIC_FOG_DETAIL_SPREAD] = vfog->spread > 0.0 ? 1.0 / vfog->spread : 1.0;
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_VOLUMETRIC_FOG_SKY_AFFECT] = RendererEnvironmentStorage::get_singleton()->environment_get_volumetric_fog_sky_affect(p_render_data->environment);
			rt_ubo.params[SceneShaderRaytracing::RT_PARAM_VOLUMETRIC_FOG_SAMPLING] = float(owner->volumetric_fog_sampling_get());
		}
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_FOG_USE_LEGACY_BLENDING] = owner->fog_use_legacy_blending_get() ? 1.0 : 0.0;

		// --- Light gathering ---
		uint32_t rt_light_count = 0;
		RT_LightData rt_light_data[RT_LIGHTS_MAX] = {};

		rt_light_count = gather_lights(p_render_data, p_state, rt_light_data, RT_LIGHTS_MAX);

		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_LIGHT_COUNT] = float(rt_light_count);
		rt_ubo.params[SceneShaderRaytracing::RT_PARAM_SELF_SHADOW_MASK] = p_state ? float(p_state->shadow_groups.self_shadow_bit) : 0.0f;

		// Upload light buffer.
		{
			uint32_t buf_size = RT_LIGHTS_MAX * sizeof(RT_LightData);
			if (!p_state->light_buffer.is_valid()) {
				p_state->light_buffer = RD::get_singleton()->storage_buffer_create(buf_size);
				RD::get_singleton()->set_resource_name(p_state->light_buffer, "RT Light Buffer");
			}
			RD::get_singleton()->buffer_update(p_state->light_buffer, 0, buf_size, rt_light_data);
		}

		if (!p_state->params_buffer.is_valid()) {
			p_state->params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(rt_ubo));
			RD::get_singleton()->set_resource_name(p_state->params_buffer, "RT Params Buffer");
		}
		RD::get_singleton()->buffer_update(p_state->params_buffer, 0, sizeof(rt_ubo), &rt_ubo);

		RD::Uniform u;
		u.binding = 6;
		u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		u.append_id(p_state->params_buffer);
		uniforms.push_back(u);
	}

	{
		RD::Uniform u;
		u.binding = 35;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		Ref<RendererRD::Fog::VolumetricFog> vfog = _rt_get_volumetric_fog(p_render_data);
		u.append_id(vfog.is_valid() ? vfog->fog_map : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE));
		uniforms.push_back(u);
	}

	// Binding 7: Sky radiance octahedral map (for pathtracing sky sampling).
	{
		RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
		const bool use_octmap_array = owner->is_using_radiance_octmap_array();
		RID radiance_texture;

		if (p_render_data && p_render_data->environment.is_valid()) {
			RID sky_rid = owner->environment_get_sky(p_render_data->environment);
			if (sky_rid.is_valid()) {
				radiance_texture = use_octmap_array
						? owner->sky.sky_get_radiance_texture_rd(sky_rid)
						: owner->sky.sky_get_radiance_2d_texture_rd(sky_rid);
			}
		}

		// Fall back to a default whose type matches the shader's declared binding.
		if (!radiance_texture.is_valid()) {
			radiance_texture = texture_storage->texture_rd_get_default(use_octmap_array
							? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK
							: RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		}

		RD::Uniform u;
		u.binding = 7;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(radiance_texture);
		uniforms.push_back(u);
	}

	// Binding 8: Sampler for radiance texture (linear filtering with mipmaps and clamp).
	{
		RD::Uniform u;
		u.binding = 8;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.append_id(RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(
				RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
		uniforms.push_back(u);
	}

	// Bindings 9-12: DLSS Ray Reconstruction output buffers (only in DLSS RR shader variant).
	bool dlss_rr_enabled = dlss_rr_has_buffers(rb);
	if (dlss_rr_enabled) {
		// Binding 9: DLSS RR Diffuse Albedo
		{
			RD::Uniform u;
			u.binding = 9;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_diffuse_albedo(rb));
			uniforms.push_back(u);
		}

		// Binding 10: DLSS RR Specular Albedo
		{
			RD::Uniform u;
			u.binding = 10;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_specular_albedo(rb));
			uniforms.push_back(u);
		}

		// Binding 11: DLSS RR Normal + Roughness
		{
			RD::Uniform u;
			u.binding = 11;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_normal_roughness(rb));
			uniforms.push_back(u);
		}

		// Binding 12: DLSS RR Specular Motion Vectors
		{
			RD::Uniform u;
			u.binding = 12;
			u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
			u.append_id(dlss_rr_get_specular_motion_vectors(rb));
			uniforms.push_back(u);
		}
	}

	// Binding 13: Light buffer (SSBO).
	{
		RD::Uniform u;
		u.binding = 13;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		if (p_state->light_buffer.is_valid()) {
			u.append_id(p_state->light_buffer);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 14: Global shader uniforms SSBO.
	{
		RD::Uniform u;
		u.binding = 14;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		RID buf = RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer();
		if (buf.is_valid()) {
			u.append_id(buf);
		} else {
			u.append_id(RendererRD::MeshStorage::get_singleton()->get_default_rd_storage_buffer());
		}
		uniforms.push_back(u);
	}

	// Binding 15: RT depth output (R32F storage image for writing depth from closest_hit/miss).
	{
		RD::Uniform u;
		u.binding = 15;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(rt_get_depth_texture(rb));
		uniforms.push_back(u);
	}

	// Bindings 16-27: Material samplers (12 filter/repeat combinations for custom shaders).
	RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default().append_uniforms(uniforms, 16);

	// Binding 28: Velocity output (RG16F). Past the 16-27 sampler range.
	{
		rb->ensure_velocity();
		RD::Uniform u;
		u.binding = 28;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(rb->get_velocity_buffer(false));
		uniforms.push_back(u);
	}

	// Bindings 29-30: Pre-pass depth/color for depth composite (shared viewports).
	{
		const bool composite = (p_rt_flags & SceneShaderRaytracing::RT_FLAG_DEPTH_COMPOSITE_ENABLED) != 0;
		RID fallback = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RD::Uniform u;
		u.binding = 29;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(composite ? rb->get_depth_texture() : fallback);
		uniforms.push_back(u);

		u = RD::Uniform();
		u.binding = 30;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(composite ? rb->get_internal_texture() : fallback);
		uniforms.push_back(u);
	}

	// Binding 33: screen-space sky drawn before the trace. Primary-ray misses read
	// this instead of the octmap when RT_PARAM_SKY_SCREEN_ENABLED is set.
	{
		RD::Uniform u;
		u.binding = 33;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(rt_has_sky_texture(rb) ? rt_get_sky_texture(rb) : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK));
		uniforms.push_back(u);
	}

	// Binding 39: the rasterizer's DFG LUT, for identical energy compensation and ambient specular.
	{
		RD::Uniform u;
		u.binding = 39;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(owner->dfg_lut.texture);
		uniforms.push_back(u);
	}

	// Binding 40: light projector sampler. Projector textures are bindless (set 1), not the decal atlas.
	{
		RD::Uniform u;
		u.binding = 40;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.append_id(owner->light_projectors_get_sampler(RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default()));
		uniforms.push_back(u);
	}

	// Binding 41: FAST noise for the froxel fog lookup, shared with the rasterizer.
	{
		RD::Uniform u;
		u.binding = 41;
		u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
		u.append_id(owner->volumetric_fog_noise_get_texture());
		uniforms.push_back(u);
	}

	if (p_rt_flags & SceneShaderRaytracing::RT_FLAG_TIMING_ENABLED) {
		if (!rb->has_texture(RB_SCOPE_RT_TIMING, RB_TEX_RT_TIMING)) {
			rb->create_texture(RB_SCOPE_RT_TIMING, RB_TEX_RT_TIMING, RD::DATA_FORMAT_R32_SFLOAT,
					RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT, RD::TEXTURE_SAMPLES_1);
		}
		RD::Uniform u;
		u.binding = 34;
		u.uniform_type = RD::UNIFORM_TYPE_IMAGE;
		u.append_id(rb->get_texture(RB_SCOPE_RT_TIMING, RB_TEX_RT_TIMING));
		uniforms.push_back(u);
	} else if (rb->has_texture(RB_SCOPE_RT_TIMING, RB_TEX_RT_TIMING)) {
		rb->clear_context(RB_SCOPE_RT_TIMING);
	}

	// Bindings 36-38: decal volumes, compiled in only with RT_FLAG_DECALS_ENABLED. Textures are bindless (set 1).
	if (p_rt_flags & SceneShaderRaytracing::RT_FLAG_DECALS_ENABLED) {
		ERR_FAIL_COND_V(p_state->decal_tlas.is_null() || p_state->decal_buffer.is_null(), RID());

		RD::Uniform u;
		u.binding = 36;
		u.uniform_type = RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE;
		u.append_id(p_state->decal_tlas);
		uniforms.push_back(u);

		u = RD::Uniform();
		u.binding = 37;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(p_state->decal_buffer);
		uniforms.push_back(u);

		u = RD::Uniform();
		u.binding = 38;
		u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
		u.append_id(owner->decals_get_sampler(RendererRD::MaterialStorage::get_singleton()->samplers_rd_get_default()));
		uniforms.push_back(u);
	}

	RID shader_rd = shader ? shader->get_pipeline_shader_rd(p_rt_flags) : RID();

	RID result;
	if (shader_rd.is_valid()) {
		// Release the previous frame's scene uniform set now (before allocating
		// this frame's). Disposal is deferred by RenderingDevice until the frame
		// that used it has finished on the GPU, so this is safe and prevents the
		// uniform_set_owner pool from leaking one RID per accumulated frame.
		//
		// The set references render-buffer resources, so RenderingDevice may have
		// already auto-freed it via its dependency cascade (e.g. a texture/buffer
		// it referenced was recreated). RID::is_valid() only checks for non-null,
		// so we must query uniform_set_is_valid() to avoid a double free.
		if (p_state->scene_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(p_state->scene_uniform_set)) {
			RD::get_singleton()->free_rid(p_state->scene_uniform_set);
		}
		p_state->scene_uniform_set = RID();

		result = RD::get_singleton()->uniform_set_create(
				uniforms,
				shader_rd,
				RenderForwardClustered::SCENE_UNIFORM_SET,
				/*p_linear_pool=*/true);
		p_state->scene_uniform_set = result;
		if (result.is_valid()) {
			RD::get_singleton()->set_resource_name(result, "RT Scene Uniform Set");
		}

		// === SET 1: Bindless textures ===
		if (bindless_block && bindless_block->is_initialized()) {
			bindless_block->finalize(shader_rd, 1);
			bindless_uniform_set = bindless_block->get_uniform_set();
		}
	}

	return result;
}

// ---------------------------------------------------------------------------
// Trace-time buffer dependencies
// ---------------------------------------------------------------------------

void RenderRaytracing::_flag_not_ready(RSE::RenderingNotReadyReason p_reason) {
	const uint64_t frame = RSG::rasterizer->get_frame_number();
	if (not_ready_frame != frame) {
		not_ready_frame = frame;
		frame_not_ready_reasons = 0;
	}
	frame_not_ready_reasons.set_flag(p_reason);
}

BitField<RSE::RenderingNotReadyReason> RenderRaytracing::get_not_ready_reasons() const {
	BitField<RSE::RenderingNotReadyReason> reasons = not_ready_frame == RSG::rasterizer->get_frame_number() ? frame_not_ready_reasons : BitField<RSE::RenderingNotReadyReason>(0);
	if (SceneShaderRaytracing::get_singleton()->is_compiling()) {
		reasons.set_flag(RSE::RENDERING_NOT_READY_RT_SHADER_COMPILATION);
	}
	return reasons;
}

// ---------------------------------------------------------------------------
// Output copy
// ---------------------------------------------------------------------------

void RenderRaytracing::copy_output_texture(const RenderDataRD *p_render_data) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	if (!rt_has_texture(rb.ptr())) {
		return;
	}

	// Copy raytracing output to main color buffer
	for (uint32_t v = 0; v < rb->get_view_count(); v++) {
		RID src = rt_get_texture(rb.ptr());
		RID dst = rb->get_internal_texture(v);
		owner->copy_effects->copy_to_rect(src, dst, Rect2i(0, 0, rb->get_internal_size().x, rb->get_internal_size().y), false, false, false, false, false, true);
	}
}
