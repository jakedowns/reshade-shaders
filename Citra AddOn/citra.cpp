/*
 * 2022 Jake Downs
 */

/*
*
* Based on generic_depth
* Copyright (C) 2021 Patrick Mours
* SPDX-License-Identifier: BSD-3-Clause
*/


#include <imgui.h>
#include <reshade.hpp>
#include "hook_info.hpp"
#include <cmath>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <vector>
#include <shared_mutex>
#include <unordered_map>

using namespace reshade::api;

static std::shared_mutex s_mutex;

static bool s_disable_intz = false;
// Enable or disable the creation of backup copies at clear operations on the selected depth-stencil
static unsigned int s_preserve_depth_buffers = 2;
// Enable or disable the aspect ratio check from 'check_aspect_ratio' in the detection heuristic
static unsigned int s_use_aspect_ratio_heuristics = 0;
static unsigned int s_do_break_on_clear = 0;

struct __declspec(uuid("0D7525F9-C4E1-426E-BC99-15BBD5FD51F2")) user_data
{

	reshade::api::resource host_resource = { 0 };
};

enum class clear_op
{
	clear_depth_stencil_view,
	fullscreen_draw,
	unbind_depth_stencil_view,
};

struct draw_stats
{
	uint32_t vertices = 0;
	uint32_t drawcalls = 0;
	uint32_t drawcalls_indirect = 0;
	viewport last_viewport = {};
};
struct clear_stats : public draw_stats
{
	clear_op clear_op = clear_op::clear_depth_stencil_view;
	bool copied_during_frame = false;
};

struct depth_stencil_info
{
	draw_stats total_stats;
	draw_stats current_stats; // Stats since last clear operation
	std::vector<clear_stats> clears;
	bool copied_during_frame = false;
};

struct depth_stencil_hash
{
	inline size_t operator()(resource value) const
	{
		// Simply use the handle (which is usually a pointer) as hash value (with some bits shaved off due to pointer alignment)
		return static_cast<size_t>(value.handle >> 4);
	}
};

struct __declspec(uuid("ad059cc1-c3ad-4cef-a4a9-401f672c6c37")) state_tracking
{
	viewport current_viewport = {};
	resource current_depth_stencil = { 0 };
	std::unordered_map<resource, depth_stencil_info, depth_stencil_hash> counters_per_used_depth_stencil;
	bool first_draw_since_bind = true;
	draw_stats best_copy_stats;

	state_tracking()
	{
		// Reserve some space upfront to avoid rehashing during command recording
		counters_per_used_depth_stencil.reserve(32);
	}

	void reset()
	{
		reset_on_present();
		current_depth_stencil = { 0 };
	}
	void reset_on_present()
	{
		best_copy_stats = { 0, 0 };
		counters_per_used_depth_stencil.clear();
	}

	void merge(const state_tracking &source)
	{
		// Executing a command list in a different command list inherits state
		current_depth_stencil = source.current_depth_stencil;

		if (source.best_copy_stats.vertices >= best_copy_stats.vertices)
			best_copy_stats = source.best_copy_stats;

		if (source.counters_per_used_depth_stencil.empty())
			return;

		counters_per_used_depth_stencil.reserve(source.counters_per_used_depth_stencil.size());
		for (const auto &[depth_stencil_handle, snapshot] : source.counters_per_used_depth_stencil)
		{
			depth_stencil_info &target_snapshot = counters_per_used_depth_stencil[depth_stencil_handle];
			target_snapshot.total_stats.vertices += snapshot.total_stats.vertices;
			target_snapshot.total_stats.drawcalls += snapshot.total_stats.drawcalls;
			target_snapshot.total_stats.drawcalls_indirect += snapshot.total_stats.drawcalls_indirect;
			target_snapshot.current_stats.vertices += snapshot.current_stats.vertices;
			target_snapshot.current_stats.drawcalls += snapshot.current_stats.drawcalls;
			target_snapshot.current_stats.drawcalls_indirect += snapshot.current_stats.drawcalls_indirect;

			target_snapshot.clears.insert(target_snapshot.clears.end(), snapshot.clears.begin(), snapshot.clears.end());

			target_snapshot.copied_during_frame |= snapshot.copied_during_frame;
		}
	}
};

struct __declspec(uuid("7c6363c7-f94e-437a-9160-141782c44a98")) generic_depth_data
{
	// The depth-stencil resource that is currently selected as being the main depth target
	resource selected_depth_stencil = { 0 };

	// Resource used to override automatic depth-stencil selection
	resource override_depth_stencil = { 0 };

	// The current depth shader resource view bound to shaders
	// This can be created from either the selected depth-stencil resource (if it supports shader access) or from a backup resource
	resource_view selected_shader_resource = { 0 };

	// secondary shader resource view created from depth_stencil_backup->backup_texture_right
	resource_view selected_shader_resource_right = { 0 };

	// True when the shader resource view was created from the backup resource, false when it was created from the original depth-stencil
	bool using_backup_texture = false;

	std::unordered_map<resource, unsigned int, depth_stencil_hash> display_count_per_depth_stencil;
};

struct __declspec(uuid("3af1cea5-87bd-47c3-9aea-caf10f159c1b")) generic_backbuffer_backup
{
	// The number of effect runtimes referencing this backup
	size_t references = 1;

	resource_desc left_resource_desc;

	resource_view_desc left_rsvd;

	resource left_pass_texture_resource = { 0 };

	resource_view left_pass_resource_view = { 0 };

	resource right_pass_texture_resource = { 0 };

	resource_view right_pass_resource_view = { 0 };

	swapchain* swapchain_pointer;

};

struct depth_stencil_backup
{
	// The number of effect runtimes referencing this backup
	size_t references = 1;

	// A resource used as target for a backup copy of this depth-stencil
	resource backup_texture = { 0 };

	// secondary texture storage on the same stencil backup
	resource backup_texture_right = { 0 };

	// The depth-stencil that should be copied from
	resource depth_stencil_resource = { 0 };

	// Set to zero for automatic detection, otherwise will use the clear operation at the specific index within a frame
	size_t force_clear_index = 0;

	// Frame dimensions of the last effect runtime this backup was used with
	uint32_t frame_width = 0;
	uint32_t frame_height = 0;
};

struct __declspec(uuid("036CD16B-E823-4D6C-A137-5C335D6FD3E6")) command_list_data
{
	bool has_multiple_rtvs = false;
	resource_view current_main_rtv = { 0 };
	uint32_t current_render_pass_index = 0;
};

struct __declspec(uuid("e006e162-33ac-4b9f-b10f-0e15335c7bdb")) generic_depth_device_data
{

	effect_runtime* main_runtime = nullptr;
	uint32_t offset_from_last_pass = 0;
	uint32_t last_render_pass_count = 1;
	uint32_t current_render_pass_count = 0;

	// List of queues created for this device
	std::vector<command_queue *> queues;

	// List of resources that were deleted this frame
	std::vector<resource> destroyed_resources;

	// List of resources that are enqueued for delayed destruction in the future
	std::vector<std::pair<resource, int>> delayed_destroy_resources;

	// List of all encountered depth-stencils of the last frame
	std::vector<std::pair<resource, depth_stencil_info>> current_depth_stencil_list;

	// List of depth-stencils that should be tracked throughout each frame and potentially be backed up during clear operations
	std::vector<depth_stencil_backup> depth_stencil_backups;

	generic_backbuffer_backup my_backbuffer_backup = generic_backbuffer_backup();

	depth_stencil_backup *find_depth_stencil_backup(resource resource)
	{
		for (depth_stencil_backup &backup : depth_stencil_backups)
			if (backup.depth_stencil_resource == resource)
				return &backup;
		return nullptr;
	}

	depth_stencil_backup *track_depth_stencil_for_backup(device *device, resource resource, resource_desc desc)
	{
		const auto it = std::find_if(depth_stencil_backups.begin(), depth_stencil_backups.end(),
			[resource](const depth_stencil_backup &existing) { return existing.depth_stencil_resource == resource; });
		if (it != depth_stencil_backups.end())
		{
			it->references++;
			return &(*it);
		}

		depth_stencil_backup &backup = depth_stencil_backups.emplace_back();
		backup.depth_stencil_resource = resource;

		desc.type = resource_type::texture_2d;
		desc.heap = memory_heap::gpu_only;
		desc.usage = resource_usage::shader_resource | resource_usage::copy_dest;

		if (device->get_api() == device_api::d3d9)
			desc.texture.format = format::r32_float; // D3DFMT_R32F, since INTZ does not support D3DUSAGE_RENDERTARGET which is required for copying
		// Use depth format as-is in OpenGL and Vulkan, since those are valid for shader resource views there
		else if (device->get_api() != device_api::opengl && device->get_api() != device_api::vulkan)
			desc.texture.format = format_to_typeless(desc.texture.format);

		// First try to revive a backup resource that was previously enqueued for delayed destruction
		for (auto delayed_destroy_it = delayed_destroy_resources.begin(); delayed_destroy_it != delayed_destroy_resources.end(); ++delayed_destroy_it)
		{
			const resource_desc delayed_destroy_desc = device->get_resource_desc(delayed_destroy_it->first);

			if (desc.texture.width == delayed_destroy_desc.texture.width && desc.texture.height == delayed_destroy_desc.texture.height && desc.texture.format == delayed_destroy_desc.texture.format)
			{
				backup.backup_texture = delayed_destroy_it->first;
				delayed_destroy_resources.erase(delayed_destroy_it);
				return &backup;
			}
		}

		// RIGHT EYE
		for (auto delayed_destroy_it = delayed_destroy_resources.begin(); delayed_destroy_it != delayed_destroy_resources.end(); ++delayed_destroy_it)
		{
			const resource_desc delayed_destroy_desc = device->get_resource_desc(delayed_destroy_it->first);

			if (desc.texture.width == delayed_destroy_desc.texture.width && desc.texture.height == delayed_destroy_desc.texture.height && desc.texture.format == delayed_destroy_desc.texture.format)
			{
				backup.backup_texture_right = delayed_destroy_it->first;
				delayed_destroy_resources.erase(delayed_destroy_it);
				return &backup;
			}
		}

		// LEFT EYE
		if (device->create_resource(desc, nullptr, resource_usage::copy_dest, &backup.backup_texture))
			device->set_resource_name(backup.backup_texture, "ReShade depth backup texture");
		else
			reshade::log_message(1, "Failed to create backup depth-stencil texture LEFT!");

		// RIGHT EYE VARIANT
		if (device->create_resource(desc, nullptr, resource_usage::copy_dest, &backup.backup_texture_right))
			device->set_resource_name(backup.backup_texture_right, "ReShade depth backup texture RIGHT");
		else
			reshade::log_message(1, "Failed to create backup depth-stencil texture RIGHT!");

		return &backup;
	}

	void untrack_depth_stencil(resource resource)
	{
		const auto it = std::find_if(depth_stencil_backups.begin(), depth_stencil_backups.end(),
			[resource](const depth_stencil_backup &existing) { return existing.depth_stencil_resource == resource; });
		if (it == depth_stencil_backups.end() || --it->references != 0)
			return;

		depth_stencil_backup &backup = *it;

		if (backup.backup_texture != 0)
		{
			// Do not destroy backup texture immediately since it may still be referenced by a command list that is in flight or was prerecorded
			// Instead enqueue it for delayed destruction in the future
			delayed_destroy_resources.emplace_back(backup.backup_texture, 50); // Destroy after 50 frames
		}

		if (backup.backup_texture_right != 0)
		{
			// Do not destroy backup texture immediately since it may still be referenced by a command list that is in flight or was prerecorded
			// Instead enqueue it for delayed destruction in the future
			delayed_destroy_resources.emplace_back(backup.backup_texture_right, 50); // Destroy after 50 frames
		}

		depth_stencil_backups.erase(it);
	}
};

struct capture_data
{
	uint32_t cx;
	uint32_t cy;
	reshade::api::format format;
	bool using_shtex;
	bool multisampled;

	union
	{
		/* shared texture */
		/*struct
		{
			shtex_data* shtex_info;
			reshade::api::resource texture;
			HANDLE handle;
		} shtex;*/
		/* shared memory */
		struct
		{
			reshade::api::resource copy_surfaces[NUM_BUFFERS];
			bool texture_ready[NUM_BUFFERS];
			bool texture_mapped[NUM_BUFFERS];
			uint32_t pitch;
			shmem_data* shmem_info;
			int cur_tex;
			int copy_wait;
		} shmem;
	};
} data;

std::string config_get_string(effect_runtime* runtime, const char* section, const char* key) {
	char buffer[512] = "";
	size_t buffer_length = sizeof(buffer);
	reshade::config_get_value(runtime, section, key, buffer, &buffer_length);
	return std::string(buffer, buffer + buffer_length);
}

// Checks whether the aspect ratio of the two sets of dimensions is similar or not
static bool check_aspect_ratio(float width_to_check, float height_to_check, uint32_t width, uint32_t height)
{
	if (width_to_check == 0.0f || height_to_check == 0.0f)
		return true;

	const float w = static_cast<float>(width);
	float w_ratio = w / width_to_check;
	const float h = static_cast<float>(height);
	float h_ratio = h / height_to_check;
	const float aspect_ratio = (w / h) - (static_cast<float>(width_to_check) / height_to_check);

	// Accept if dimensions are similar in value or almost exact multiples
	return std::fabs(aspect_ratio) <= 0.1f && ((w_ratio <= 1.85f && w_ratio >= 0.5f && h_ratio <= 1.85f && h_ratio >= 0.5f) || (s_use_aspect_ratio_heuristics == 2 && std::modf(w_ratio, &w_ratio) <= 0.02f && std::modf(h_ratio, &h_ratio) <= 0.02f));
}

unsigned int draws_without_depth = 0;
unsigned int max_draws_without_depth = 0;
unsigned int max_draws_per_frame = 0;
unsigned int frames = 0;
unsigned int draws_this_frame = 0;
unsigned int left_eye_copies_this_frame = 0;
unsigned int right_eye_copies_this_frame = 0;
unsigned int clears_this_frame = 0;
unsigned int fullscreen_draws_this_frame = 0;
enum class my_ops
{
	clear,
	draw,
	present,
	render_fx_start
};
void copy_rgb_buffer(command_list* cmd_list, my_ops op, uint32_t vertices, uint32_t draw_calls)
{
	bool test = false;

	if (s_do_break_on_clear) {
		test = true;
	}
	device* const device = cmd_list->get_device();
	generic_depth_device_data depth_dev = device->get_private_data<generic_depth_device_data>();
	generic_backbuffer_backup my_backup = depth_dev.my_backbuffer_backup;
	resource_view_desc srv_desc = my_backup.left_rsvd;
	//const resource_desc rd = device->get_resource_desc(swapchain->get_current_back_buffer());
	//resource_view_desc rvd = device->get_resource_view_desc();
	// TODO: determine if we need to save backbuffer to LEFT eye backup or RIGHT eye backup
	if (my_backup.swapchain_pointer == 0 || my_backup.swapchain_pointer == nullptr) {
		return;
	}
	reshade::api::resource back_buffer = my_backup.swapchain_pointer->get_current_back_buffer();
	//cmd_list->barrier(back_buffer, resource_usage::shader_resource | resource_usage::present, resource_usage::copy_source);
	if (op == my_ops::clear || op == my_ops::draw) {
		// left eye
		left_eye_copies_this_frame++;

		cmd_list->copy_resource(back_buffer, my_backup.left_pass_texture_resource);
	}
	else {
		// right eye
		right_eye_copies_this_frame++;
		
		cmd_list->copy_resource(back_buffer, my_backup.right_pass_texture_resource);
		
	}
	//cmd_list->barrier(back_buffer, resource_usage::copy_dest, resource_usage::shader_resource);// , resource_usage::present);
}

static void on_clear_depth_impl(command_list *cmd_list, state_tracking &state, resource depth_stencil, clear_op op)
{
	if (depth_stencil == 0) {
		clears_this_frame++;
		copy_rgb_buffer(cmd_list, my_ops::clear, 0, 0);
		return;
	}

	device *const device = cmd_list->get_device();

	depth_stencil_backup *const depth_stencil_backup = device->get_private_data<generic_depth_device_data>().find_depth_stencil_backup(depth_stencil);

	

	if (depth_stencil_backup == nullptr || depth_stencil_backup->backup_texture == 0 || depth_stencil_backup->backup_texture_right == 0)
		return;

	bool do_copy = true;
	depth_stencil_info &counters = state.counters_per_used_depth_stencil[depth_stencil];

	// Ignore clears when there was no meaningful workload (e.g. at the start of a frame)
	/*if (counters.current_stats.drawcalls == 0)
		return;*/

	// Ignore clears when the last viewport rendered to only affected a small subset of the depth-stencil (fixes flickering in some games)
	//switch (op)
	//{
	//case clear_op::clear_depth_stencil_view:
	//	// Mirror's Edge and Portal occasionally render something into a small viewport (16x16 in Mirror's Edge, 512x512 in Portal to render underwater geometry)
	//	// DISABLED FOR CITRA which can use buffers as small as 240,400,720,800 wide
	//	// do_copy = counters.current_stats.last_viewport.width > 1024 || (counters.current_stats.last_viewport.width == 0 || depth_stencil_backup->frame_width <= 1024);
	//	break;
	//case clear_op::fullscreen_draw:
	//	// Mass Effect 3 in Mass Effect Legendary Edition sometimes uses a larger common depth buffer for shadow map and scene rendering, where the former uses a 1024x1024 viewport and the latter uses a viewport matching the render resolution
	//	//do_copy = check_aspect_ratio(counters.current_stats.last_viewport.width, counters.current_stats.last_viewport.height, depth_stencil_backup->frame_width, depth_stencil_backup->frame_height);
	//	break;
	//case clear_op::unbind_depth_stencil_view:
	//	break;
	//}

	// consider the "copy on clear" buffer the Left eye, since citra games probably draw left before right
	// TODO: maybe some games do this in reverse, in which case we'll need to add an option to swap L/R but we can do that on the Effect side
	bool is_left_eye = counters.clears.size() == (depth_stencil_backup->force_clear_index - 1);
	resource destination = is_left_eye ? depth_stencil_backup->backup_texture : depth_stencil_backup->backup_texture_right;

	if (do_copy)
	{
		if (op != clear_op::unbind_depth_stencil_view)
		{
			/*if (counters.current_stats.vertices >= state.best_copy_stats.vertices) {
				reshade::log_message(3, "beat");
			}*/
			// If clear index override is set to zero, always copy any suitable buffers
			if (depth_stencil_backup->force_clear_index == 0)
			{
				// Use greater equals operator here to handle case where the same scene is first rendered into a shadow map and then for real (e.g. Mirror's Edge main menu)
				do_copy = counters.current_stats.vertices >= state.best_copy_stats.vertices || (op == clear_op::fullscreen_draw && counters.current_stats.drawcalls >= state.best_copy_stats.drawcalls);
			}
			else if (std::numeric_limits<size_t>::max() == depth_stencil_backup->force_clear_index)
			{
				// Special case for Garry's Mod which chooses the last clear operation that has a high workload
				do_copy = counters.current_stats.vertices >= 5000;
			}
			else
			{
				// This is not really correct, since clears may accumulate over multiple command lists, but it's unlikely that the same depth-stencil is used in more than one
				// CITRA NOTE: "but it's unlikely that the same depth-stencil is used in more than one" this is NOT TRUE for Citra emulator
				// as far as i can tell, all the games i've tested so far use the SAME depth-stencil for Left/Right eye to save memory
				// first drawing left, then clearing and drawing right
				do_copy = counters.clears.size() == (depth_stencil_backup->force_clear_index - 1);
			}

			// CITRA VERSION
			/*if (counters.current_stats.vertices < 10) {
				do_copy = false;
			}*/
			/*do_copy = counters.current_stats.vertices >= state.best_copy_stats.vertices 
				|| (op == clear_op::fullscreen_draw && counters.current_stats.drawcalls >= state.best_copy_stats.drawcalls)
				|| counters.clears.size() == (depth_stencil_backup->force_clear_index - 1);*/
			do_copy = true;

			// record the clear op
			try {
				counters.clears.push_back({ counters.current_stats, op, do_copy });
			}
			catch (std::exception& e) {
				reshade::log_message(1, e.what());
			}
			
		}

		// Make a backup copy of the depth texture before it is cleared
		if (do_copy)
		{
			state.best_copy_stats = counters.current_stats;

			

			// A resource has to be in this state for a clear operation, so can assume it here
			cmd_list->barrier(depth_stencil, resource_usage::depth_stencil_write, resource_usage::copy_source);
			cmd_list->copy_resource(depth_stencil, destination); // copy to backup_texture || backup_texture_right
			cmd_list->barrier(depth_stencil, resource_usage::copy_source, resource_usage::depth_stencil_write);

			counters.copied_during_frame = true;
		}
	}

	// Reset draw call stats for clears
	counters.current_stats = { 0, 0 };
}

static void update_effect_runtime(effect_runtime *runtime)
{
	const generic_depth_data &instance = runtime->get_private_data<generic_depth_data>();
	const generic_depth_device_data& dev_data = runtime->get_private_data<generic_depth_device_data>();
	const generic_backbuffer_backup& bbb = dev_data.my_backbuffer_backup; //runtime->get_private_data<generic_backbuffer_backup>();

	runtime->update_texture_bindings("ORIG_DEPTH", instance.selected_shader_resource);
	// ORIG_DEPTH_RIGHT
	runtime->update_texture_bindings("ORIG_DEPTH_2", instance.selected_shader_resource_right);

	runtime->update_texture_bindings("RGB_LEFT", bbb.left_pass_resource_view);

	runtime->update_texture_bindings("RGB_RIGHT", bbb.right_pass_resource_view);

	runtime->enumerate_uniform_variables(nullptr, [&instance](effect_runtime *runtime, auto variable) {
		char source[32] = "";
		if (runtime->get_annotation_string_from_uniform_variable(variable, "source", source) && std::strcmp(source, "bufready_depth") == 0)
			runtime->set_uniform_value_bool(variable, instance.selected_shader_resource != 0);
	});

	resource_view srv, srv_srgb;

	effect_texture_variable ModifiedDepthTex_handle = runtime->find_texture_variable("Citra.fx", "ModifiedDepthTex");
	runtime->get_texture_binding(ModifiedDepthTex_handle, &srv, &srv_srgb);

	runtime->update_texture_bindings("DEPTH", srv, srv_srgb);
}

static void on_init_device(device *device)
{
	device->create_private_data<generic_depth_device_data>();

	reshade::config_get_value(nullptr, "DEPTH", "DisableINTZ", s_disable_intz);
	reshade::config_get_value(nullptr, "DEPTH", "DepthCopyBeforeClears", s_preserve_depth_buffers);
	reshade::config_get_value(nullptr, "DEPTH", "UseAspectRatioHeuristics", s_use_aspect_ratio_heuristics);
	reshade::config_get_value(nullptr, "DEPTH", "DoBreakOnClear", s_do_break_on_clear);
}
static void on_init_command_list(command_list *cmd_list)
{
	cmd_list->create_private_data<state_tracking>();
}
static void on_init_command_queue(command_queue *cmd_queue)
{
	cmd_queue->create_private_data<state_tracking>();

	if ((cmd_queue->get_type() & command_queue_type::graphics) == 0)
		return;

	auto &device_data = cmd_queue->get_device()->get_private_data<generic_depth_device_data>();
	device_data.queues.push_back(cmd_queue);
}
static void on_init_effect_runtime(effect_runtime *runtime)
{
	runtime->create_private_data<generic_depth_data>();
	auto& device_data = runtime->get_private_data<generic_depth_device_data>();
	device_data.main_runtime = runtime;
}
static void on_destroy_device(device *device)
{
	auto &device_data = device->get_private_data<generic_depth_device_data>();

	// Destroy any remaining resources
	for (const auto &[resource, _] : device_data.delayed_destroy_resources)
	{
		device->destroy_resource(resource);
	}

	for (depth_stencil_backup &depth_stencil_backup : device_data.depth_stencil_backups)
	{
		if (depth_stencil_backup.backup_texture != 0)
			device->destroy_resource(depth_stencil_backup.backup_texture);
	}

	device->destroy_private_data<generic_depth_device_data>();
}
static void on_destroy_command_list(command_list *cmd_list)
{
	cmd_list->destroy_private_data<state_tracking>();
}
static void on_destroy_command_queue(command_queue *cmd_queue)
{
	cmd_queue->destroy_private_data<state_tracking>();

	auto &device_data = cmd_queue->get_device()->get_private_data<generic_depth_device_data>();
	device_data.queues.erase(std::remove(device_data.queues.begin(), device_data.queues.end(), cmd_queue), device_data.queues.end());
}
static void on_destroy_effect_runtime(effect_runtime *runtime)
{
	device *const device = runtime->get_device();
	generic_depth_data &data = runtime->get_private_data<generic_depth_data>();

	if (data.selected_shader_resource != 0)
		device->destroy_resource_view(data.selected_shader_resource);

	if (data.selected_shader_resource_right != 0)
		device->destroy_resource_view(data.selected_shader_resource_right);

	runtime->destroy_private_data<generic_depth_data>();
}

static bool on_create_resource(device *device, resource_desc &desc, subresource_data *, resource_usage)
{
	if (desc.type != resource_type::surface && desc.type != resource_type::texture_2d)
		return false; // Skip resources that are not 2D textures
	if (desc.texture.samples != 1 || (desc.usage & resource_usage::depth_stencil) == 0 || desc.texture.format == format::s8_uint)
		return false; // Skip MSAA textures and resources that are not used as depth buffers

	switch (device->get_api())
	{
	case device_api::d3d9:
		if (s_disable_intz)
			return false;
		// Skip textures that are sampled as PCF shadow maps (see https://aras-p.info/texts/D3D9GPUHacks.html#shadowmap) using hardware support, since changing format would break that
		if (desc.type == resource_type::texture_2d && (desc.texture.format == format::d16_unorm || desc.texture.format == format::d24_unorm_x8_uint || desc.texture.format == format::d24_unorm_s8_uint))
			return false;
		// Skip small textures that are likely just shadow maps too (fixes a hang in Dragon's Dogma: Dark Arisen when changing areas)
		if (desc.texture.width <= 512)
			return false;
		// Replace texture format with special format that supports normal sampling (see https://aras-p.info/texts/D3D9GPUHacks.html#depth)
		desc.texture.format = format::intz;
		desc.usage |= resource_usage::shader_resource;
		break;
	case device_api::d3d10:
	case device_api::d3d11:
		// Allow shader access to images that are used as depth-stencil attachments
		desc.texture.format = format_to_typeless(desc.texture.format);
		desc.usage |= resource_usage::shader_resource;
		break;
	case device_api::d3d12:
	case device_api::vulkan:
		// D3D12 and Vulkan always use backup texture, but need to be able to copy to it
		desc.usage |= resource_usage::copy_source;
		break;
	case device_api::opengl:
		// No need to change anything in OpenGL
		return false;
	}

	return true;
}
static bool on_create_resource_view(device *device, resource resource, resource_usage usage_type, resource_view_desc &desc)
{
	// A view cannot be created with a typeless format (which was set in 'on_create_resource' above), so fix it in case defaults are used
	if ((device->get_api() != device_api::d3d10 && device->get_api() != device_api::d3d11) || desc.format != format::unknown)
		return false;

	const resource_desc texture_desc = device->get_resource_desc(resource);
	// Only non-MSAA textures where modified, so skip all others
	if (texture_desc.texture.samples != 1 || (texture_desc.usage & resource_usage::depth_stencil) == 0)
		return false;

	switch (usage_type)
	{
	case resource_usage::depth_stencil:
		desc.format = format_to_depth_stencil_typed(texture_desc.texture.format);
		break;
	case resource_usage::shader_resource:
		desc.format = format_to_default_typed(texture_desc.texture.format);
		break;
	}

	// Only need to set the rest of the fields if the application did not pass in a valid description already
	if (desc.type == resource_view_type::unknown)
	{
		desc.type = texture_desc.texture.depth_or_layers > 1 ? resource_view_type::texture_2d_array : resource_view_type::texture_2d;
		desc.texture.first_level = 0;
		desc.texture.level_count = (usage_type == resource_usage::shader_resource) ? UINT32_MAX : 1;
		desc.texture.first_layer = 0;
		desc.texture.layer_count = (usage_type == resource_usage::shader_resource) ? UINT32_MAX : 1;
	}

	return true;
}
static void on_destroy_resource(device *device, resource resource)
{
	auto &device_data = device->get_private_data<generic_depth_device_data>();

	// In some cases the 'destroy_device' event may be called before all resources have been destroyed
	// The state tracking context would have been destroyed already in that case, so return early if it does not exist
	if (std::addressof(device_data) == nullptr)
		return;

	std::unique_lock<std::shared_mutex> lock(s_mutex);

	device_data.destroyed_resources.push_back(resource);

	// Remove this destroyed resource from the list of tracked depth-stencil resources
	const auto it = std::find_if(device_data.current_depth_stencil_list.begin(), device_data.current_depth_stencil_list.end(),
		[resource](const auto &current) { return current.first == resource; });
	if (it != device_data.current_depth_stencil_list.end())
	{
		const bool copied_during_frame = it->second.copied_during_frame;

		device_data.current_depth_stencil_list.erase(it);

		lock.unlock();

		// This is bad ... the resource may still be in use by an effect on the GPU and destroying it would crash it
		// Try to mitigate that somehow by delaying this thread a little to hopefully give the GPU enough time to catch up before the resource memory is deallocated
		if (device->get_api() == device_api::d3d12 || device->get_api() == device_api::vulkan)
		{
			reshade::log_message(2, "A depth-stencil resource was destroyed while still being tracked.");

			if (!copied_during_frame)
				Sleep(250);
		}
	}
}



static bool on_draw(command_list *cmd_list, uint32_t vertices, uint32_t instances, uint32_t, uint32_t)
{
	auto &state = cmd_list->get_private_data<state_tracking>();
	// Check if this draw call likely represets a fullscreen rectangle (two triangles), which would clear the depth-stencil
	const bool fullscreen_draw = vertices == 6 && instances == 1;

	draws_this_frame++;
	if (state.current_depth_stencil == 0) {
		draws_without_depth++;
		fullscreen_draws_this_frame += fullscreen_draw ? 1 : 0;
		copy_rgb_buffer(cmd_list, my_ops::draw, vertices, 0);
		return false; // This is a draw call with no depth-stencil bound
	}
		

	
	if (fullscreen_draw
		//&& s_preserve_depth_buffers == 2
		&& state.first_draw_since_bind
		// But ignore that in Vulkan (since it is invalid to copy a resource inside an active render pass)
		//cmd_list->get_device()->get_api() != device_api::vulkan
		)
		on_clear_depth_impl(cmd_list, state, state.current_depth_stencil, clear_op::fullscreen_draw);

	state.first_draw_since_bind = false;

	try {
		depth_stencil_info& counters = state.counters_per_used_depth_stencil[state.current_depth_stencil];
		counters.total_stats.vertices += vertices * instances;
		counters.total_stats.drawcalls += 1;
		counters.current_stats.vertices += vertices * instances;
		counters.current_stats.drawcalls += 1;

		// Skip updating last viewport for fullscreen draw calls, to prevent a clear operation in Prince of Persia: The Sands of Time from getting filtered out
		if (!fullscreen_draw)
			counters.current_stats.last_viewport = state.current_viewport;
	}
	catch (std::exception& e) {
		reshade::log_message(1, e.what());
	}

	return false;
}
static bool on_draw_indexed(command_list *cmd_list, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t)
{
	on_draw(cmd_list, indices, instances, 0, 0);

	return false;
}

static bool on_draw_indirect(command_list *cmd_list, indirect_command type, resource, uint64_t, uint32_t draw_count, uint32_t)
{
	if (type == indirect_command::dispatch)
		return false;

	auto &state = cmd_list->get_private_data<state_tracking>();
	if (state.current_depth_stencil == 0) {
		copy_rgb_buffer(cmd_list,my_ops::draw,0,draw_count);
		return false; // This is a draw call with no depth-stencil bound
	}

	depth_stencil_info &counters = state.counters_per_used_depth_stencil[state.current_depth_stencil];
	counters.total_stats.drawcalls += draw_count;
	counters.total_stats.drawcalls_indirect += draw_count;
	counters.current_stats.drawcalls += draw_count;
	counters.current_stats.drawcalls_indirect += draw_count;
	counters.current_stats.last_viewport = state.current_viewport;

	return false;
}

static void on_bind_viewport(command_list *cmd_list, uint32_t first, uint32_t count, const viewport *viewport)
{
	if (first != 0 || count == 0)
		return; // Only interested in the main viewport

	auto &state = cmd_list->get_private_data<state_tracking>();
	state.current_viewport = viewport[0];
}
static void on_bind_depth_stencil(command_list *cmd_list, uint32_t, const resource_view *, resource_view depth_stencil_view)
{
	auto &state = cmd_list->get_private_data<state_tracking>();

	const resource depth_stencil = (depth_stencil_view != 0) ? cmd_list->get_device()->get_resource_from_view(depth_stencil_view) : resource{ 0 };

	if (depth_stencil != state.current_depth_stencil)
	{
		if (depth_stencil != 0)
			state.first_draw_since_bind = true;

		// Make a backup of the depth texture before it is used differently, since in D3D12 or Vulkan the underlying memory may be aliased to a different resource, so cannot just access it at the end of the frame
		if (s_preserve_depth_buffers == 2 &&
			state.current_depth_stencil != 0 && depth_stencil == 0 && (
			cmd_list->get_device()->get_api() == device_api::d3d12 || cmd_list->get_device()->get_api() == device_api::vulkan))
			on_clear_depth_impl(cmd_list, state, state.current_depth_stencil, clear_op::unbind_depth_stencil_view);
	}

	state.current_depth_stencil = depth_stencil;
}
static bool on_clear_depth_stencil(command_list *cmd_list, resource_view dsv, const float *depth, const uint8_t *, uint32_t, const rect *)
{
	// Ignore clears that do not affect the depth buffer (stencil clears)
	if (depth != nullptr && s_preserve_depth_buffers)
	{
		auto &state = cmd_list->get_private_data<state_tracking>();

		const resource depth_stencil = cmd_list->get_device()->get_resource_from_view(dsv);

		// Note: This does not work when called from 'vkCmdClearAttachments', since it is invalid to copy a resource inside an active render pass
		on_clear_depth_impl(cmd_list, state, depth_stencil, clear_op::clear_depth_stencil_view);
	}

	return false;
}
static void on_begin_render_pass_with_depth_stencil(command_list *cmd_list, uint32_t, const render_pass_render_target_desc *, const render_pass_depth_stencil_desc *depth_stencil_desc)
{
	if (depth_stencil_desc != nullptr && depth_stencil_desc->depth_load_op == render_pass_load_op::clear)
	{
		on_clear_depth_stencil(cmd_list, depth_stencil_desc->view, &depth_stencil_desc->clear_depth, nullptr, 0, nullptr);

		// Prevent 'on_bind_depth_stencil' from copying depth buffer again
		auto &state = cmd_list->get_private_data<state_tracking>();
		state.current_depth_stencil = { 0 };
	}

	// If render pass has depth store operation set to 'discard', any copy performed after the render pass will likely contain broken data, so can only hope that the depth buffer can be copied before that ...

	on_bind_depth_stencil(cmd_list, 0, nullptr, depth_stencil_desc != nullptr ? depth_stencil_desc->view : resource_view{});
}

static void on_reset(command_list *cmd_list)
{
	auto &target_state = cmd_list->get_private_data<state_tracking>();
	target_state.reset();
}
static void on_execute_primary(command_queue *queue, command_list *cmd_list)
{
	auto &target_state = queue->get_private_data<state_tracking>();
	const auto &source_state = cmd_list->get_private_data<state_tracking>();

	// Skip merging state when this execution event is just the immediate command list getting flushed
	if (std::addressof(target_state) != std::addressof(source_state))
	{
		target_state.merge(source_state);
	}
}
static void on_execute_secondary(command_list *cmd_list, command_list *secondary_cmd_list)
{
	auto &target_state = cmd_list->get_private_data<state_tracking>();
	const auto &source_state = secondary_cmd_list->get_private_data<state_tracking>();

	// If this is a secondary command list that was recorded without a depth-stencil binding, but is now executed using a depth-stencil binding, handle it as if an indirect draw call was performed to ensure the depth-stencil is tracked
	if (target_state.current_depth_stencil != 0 && source_state.current_depth_stencil == 0 && source_state.counters_per_used_depth_stencil.empty())
	{
		target_state.current_viewport = source_state.current_viewport;

		on_draw_indirect(cmd_list, indirect_command::draw, { 0 }, 0, 1, 0);
	}
	else
	{
		target_state.merge(source_state);
	}
}

static bool capture_impl_init(reshade::api::swapchain* swapchain)
{
	reshade::api::device* const device = swapchain->get_device();

	const reshade::api::resource_desc desc = device->get_resource_desc(swapchain->get_current_back_buffer());
	data.format = reshade::api::format_to_default_typed(desc.texture.format, 0);
	data.multisampled = desc.texture.samples > 1;
	data.cx = desc.texture.width;
	data.cy = desc.texture.height;

	data.using_shtex = false;

	for (int i = 0; i < NUM_BUFFERS; i++)
	{
		if (!device->create_resource(
			reshade::api::resource_desc(data.cx, data.cy, 1, 1, data.format, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::copy_dest),
			nullptr,
			reshade::api::resource_usage::shader_resource,
			&data.shmem.copy_surfaces[i]))
			return false;
	}

	reshade::api::subresource_data mapped;
	if (device->map_texture_region(data.shmem.copy_surfaces[0], 0, nullptr, reshade::api::map_access::read_only, &mapped))
	{
		data.shmem.pitch = mapped.row_pitch;
		device->unmap_texture_region(data.shmem.copy_surfaces[0], 0);
	}

	/*if (!capture_init_shmem(data.shmem.shmem_info, swapchain->get_hwnd(), data.cx, data.cy, data.shmem.pitch, static_cast<uint32_t>(data.format), false))
		return false;*/

	return true;
}

static void capture_impl_free(reshade::api::swapchain* swapchain)
{
	reshade::api::device* const device = swapchain->get_device();

	capture_free();

	
		for (int i = 0; i < NUM_BUFFERS; i++)
		{
			if (data.shmem.copy_surfaces[i] == 0)
				continue;

			if (data.shmem.texture_mapped[i])
				device->unmap_texture_region(data.shmem.copy_surfaces[i], 0);

			device->destroy_resource(data.shmem.copy_surfaces[i]);
		}

	memset(&data, 0, sizeof(data));
}

static void capture_impl_shmem(reshade::api::command_queue* queue, reshade::api::resource back_buffer)
{
	reshade::api::device* device = queue->get_device();
	reshade::api::command_list* cmd_list = queue->get_immediate_command_list();

	int next_tex = (data.shmem.cur_tex + 1) % NUM_BUFFERS;

	if (data.shmem.texture_ready[next_tex])
	{
		data.shmem.texture_ready[next_tex] = false;

		reshade::api::subresource_data mapped;
		if (device->map_texture_region(data.shmem.copy_surfaces[next_tex], 0, nullptr, reshade::api::map_access::read_only, &mapped))
		{
			data.shmem.texture_mapped[next_tex] = true;
			//shmem_copy_data(next_tex, mapped.data);
		}
	}

	if (data.shmem.copy_wait < NUM_BUFFERS - 1)
	{
		data.shmem.copy_wait++;
	}
	else
	{
		/*if (shmem_texture_data_lock(data.shmem.cur_tex))
		{
			device->unmap_texture_region(data.shmem.copy_surfaces[data.shmem.cur_tex], 0);
			data.shmem.texture_mapped[data.shmem.cur_tex] = false;
			shmem_texture_data_unlock(data.shmem.cur_tex);
		}*/

		/*if (data.multisampled)
		{
			cmd_list->barrier(back_buffer, reshade::api::resource_usage::present, reshade::api::resource_usage::resolve_source);
			cmd_list->barrier(data.shmem.copy_surfaces[data.shmem.cur_tex], reshade::api::resource_usage::cpu_access, reshade::api::resource_usage::resolve_dest);

			cmd_list->resolve_texture_region(back_buffer, 0, nullptr, data.shmem.copy_surfaces[data.shmem.cur_tex], 0, 0, 0, 0, static_cast<reshade::api::format>(data.format));

			cmd_list->barrier(data.shmem.copy_surfaces[data.shmem.cur_tex], reshade::api::resource_usage::resolve_dest, reshade::api::resource_usage::cpu_access);
			cmd_list->barrier(back_buffer, reshade::api::resource_usage::resolve_source, reshade::api::resource_usage::present);
		}
		else
		{
			cmd_list->barrier(back_buffer, reshade::api::resource_usage::present, reshade::api::resource_usage::copy_source);
			cmd_list->barrier(data.shmem.copy_surfaces[data.shmem.cur_tex], reshade::api::resource_usage::cpu_access, reshade::api::resource_usage::copy_dest);

			cmd_list->copy_resource(back_buffer, data.shmem.copy_surfaces[data.shmem.cur_tex]);

			cmd_list->barrier(data.shmem.copy_surfaces[data.shmem.cur_tex], reshade::api::resource_usage::copy_dest, reshade::api::resource_usage::cpu_access);
			cmd_list->barrier(back_buffer, reshade::api::resource_usage::copy_source, reshade::api::resource_usage::present);
		}*/

		data.shmem.texture_ready[data.shmem.cur_tex] = true;
	}

	data.shmem.cur_tex = next_tex;
}

// 
//static void on_present_swapchain(command_queue* queue, swapchain* swapchain)
//{
//	// via https://reshade.me/forum/general-discussion/7538-capture-final-frame-color-buffer-to-file
//	user_data& data = swapchain->get_private_data<user_data>();
//
//	device* const device = swapchain->get_device();
//
//	command_list* cmd_list = queue->get_immediate_command_list();
//	// TODO: Add barriers/state transitions for DX12/Vulkan support (using "cmd_list->barrier()")
//	// Copy current frame into the CPU-accessible texture
//	cmd_list->copy_resource(swapchain->get_current_back_buffer(), data.host_resource);
//	// Very slow ... but ensures the copy has completed before accessing the data next
//	queue->wait_idle();
//
//	// Map CPU-accessible texture to read the data
//	subresource_data host_data;
//	if (!device->map_texture_region(
//		data.host_resource, 0, nullptr, map_access::read_only, &host_data))
//		return;
//
//	const resource_desc desc = device->get_resource_desc(data.host_resource);
//
//	// TODO: This assumes that the format is RGBA8, need to handle differently for different formats
//	assert(desc.texture.format == format::r8g8b8a8_unorm);
//
//	
//
//	//for (int y = 0; y < desc.texture.height; ++y)
//	//{
//	//	for (int x = 0; x < desc.texture.width; ++x)
//	//	{
//	//		const size_t host_data_index = y * host_data.row_pitch + x * 4;
//
//	//		const uint8_t r = static_cast<const uint8_t*>(host_data.data)[host_data_index + 0];
//	//		const uint8_t g = static_cast<const uint8_t*>(host_data.data)[host_data_index + 1];
//	//		const uint8_t b = static_cast<const uint8_t*>(host_data.data)[host_data_index + 2];
//	//		const uint8_t a = static_cast<const uint8_t*>(host_data.data)[host_data_index + 3];
//
//	//		// TODO: Do something with the pixel, e.g. dump this whole image to an image file
//	//	}
//	//}
//
//	device->unmap_texture_region(data.host_resource, 0);
//}

bool capture_started = false;

static void on_present(command_queue *queue, swapchain *swapchain, const rect *source_rect, const rect *dest_rect, uint32_t dirty_rect_count, const rect *dirty_rects)
{
	device *const device = swapchain->get_device();
	generic_depth_device_data &device_data = device->get_private_data<generic_depth_device_data>();
	
	//on_present_swapchain(queue, swapchain);
	
	// Keep track of the total render pass count of this frame
	device_data.last_render_pass_count = device_data.current_render_pass_count > 0 ? device_data.current_render_pass_count : 1;
	device_data.current_render_pass_count = 0;

	if (device_data.offset_from_last_pass > device_data.last_render_pass_count)
		device_data.offset_from_last_pass = device_data.last_render_pass_count;

	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	// Merge state from all graphics queues
	state_tracking queue_state;
	for (command_queue *const queue : device_data.queues)
		queue_state.merge(queue->get_private_data<state_tracking>());

	// Only update device list if there are any depth-stencils, otherwise this may be a second present call (at which point 'reset_on_present' already cleared out the queue list in the first present call)
	if (queue_state.counters_per_used_depth_stencil.empty())
		return;

	// Also skip update when there has been very little activity (special case for emulators like PCSX2 which may present more often than they render a frame)
	// CITRA NOTE we might want to remove this. in order to get all our buffers, maybe we WANT to catch presents with few draw calls
	// OR: maybe we RAISE the limiter above 8 to correctly filter out things like the EXTRA BACKUP ITEM bug in Super Mario 3D Land, where having a backup item takes over the depth buffer
	size_t drawcalls = queue_state.counters_per_used_depth_stencil.begin()->second.total_stats.drawcalls;
	if (queue_state.counters_per_used_depth_stencil.size() == 1 && drawcalls <= 8)
		return;

	device_data.current_depth_stencil_list.clear();
	device_data.current_depth_stencil_list.reserve(queue_state.counters_per_used_depth_stencil.size());

	for (const auto &[resource, snapshot] : queue_state.counters_per_used_depth_stencil)
	{
		if (snapshot.total_stats.drawcalls == 0)
			continue; // Skip unused

		if (std::find(device_data.destroyed_resources.begin(), device_data.destroyed_resources.end(), resource) != device_data.destroyed_resources.end())
			continue; // Skip resources that were destroyed by the application

		// Save to current list of depth-stencils on the device, so that it can be displayed in the GUI
		device_data.current_depth_stencil_list.emplace_back(resource, snapshot);
	}

	for (command_queue *const queue : device_data.queues)
		queue->get_private_data<state_tracking>().reset_on_present();

	device_data.destroyed_resources.clear();

	// Destroy resources that were enqueued for delayed destruction and have reached the targeted number of passed frames
	for (auto it = device_data.delayed_destroy_resources.begin(); it != device_data.delayed_destroy_resources.end();)
	{
		if (--it->second == 0)
		{
			device->destroy_resource(it->first);

			it = device_data.delayed_destroy_resources.erase(it);
		}
		else
		{
			++it;
		}
	}
}

static void on_begin_render_effects(effect_runtime *runtime, command_list *cmd_list, resource_view, resource_view)
{
	frames++;
	if (draws_this_frame > max_draws_per_frame) {
		max_draws_per_frame = draws_this_frame;
	}
	if (draws_without_depth > max_draws_without_depth) {
		max_draws_without_depth = draws_without_depth;
	}
	// TODO: do one last right eye backbuffer copy here?
	copy_rgb_buffer(cmd_list, my_ops::render_fx_start,0,0);
	draws_this_frame = 0;
	fullscreen_draws_this_frame = 0;
	clears_this_frame = 0;
	draws_without_depth = 0;
	left_eye_copies_this_frame = 0;
	right_eye_copies_this_frame = 0;
	device *const device = runtime->get_device();
	generic_depth_data &depth_data = runtime->get_private_data<generic_depth_data>();
	generic_depth_device_data &device_data = device->get_private_data<generic_depth_device_data>();

	resource best_match = { 0 };
	resource_desc best_match_desc;
	const depth_stencil_info *best_snapshot = nullptr;

	//resource best_match_right_eye = { 0 };
	//resource_desc match_desc_right_eye;
	//const depth_stencil_info* best_snapshot_right_eye = nullptr;

	// testing grabbing backbuffer twice during single frame to detangle left and right rgb, so we can bind it for shader access
	// Change this event to e.g. 'reshade_begin_effects' to send images to OBS before ReShade effects are applied, or 'reshade_render_technique' to send after a specific technique.
	/*size_t bb_count = swapchain->get_back_buffer_count();
	resource current_bb = swapchain->get_current_back_buffer();
	resource_desc bb_desc = device->get_resource_desc(current_bb);*/

	/*if (global_hook_info == nullptr)
		return;*/

	/*if (capture_should_stop())
		capture_impl_free(swapchain);*/

	uint32_t frame_width, frame_height;
	runtime->get_screenshot_width_and_height(&frame_width, &frame_height);

	std::shared_lock<std::shared_mutex> lock(s_mutex);
	const auto current_depth_stencil_list = device_data.current_depth_stencil_list;
	// Unlock while calling into device below, since device may hold a lock itself and that then can deadlock another thread that calls into 'on_destroy_resource' from the device holding that lock
	lock.unlock();

	for (auto &[resource, snapshot] : current_depth_stencil_list)
	{
		const resource_desc desc = device->get_resource_desc(resource);
		if (desc.texture.samples > 1)
			continue; // Ignore MSAA textures, since they would need to be resolved first

		//if (s_use_aspect_ratio_heuristics && !check_aspect_ratio(static_cast<float>(desc.texture.width), static_cast<float>(desc.texture.height), frame_width, frame_height))
		//	continue; // Not a good fit

		if (best_snapshot == nullptr || (snapshot.total_stats.drawcalls_indirect < (snapshot.total_stats.drawcalls / 3) ?
			// Choose snapshot with the most vertices, since that is likely to contain the main scene
			snapshot.total_stats.vertices > best_snapshot->total_stats.vertices :
		// Or check draw calls, since vertices may not be accurate if application is using indirect draw calls
		snapshot.total_stats.drawcalls > best_snapshot->total_stats.drawcalls))
		{
			best_match = resource;
			best_match_desc = desc;
			best_snapshot = &snapshot;
		}
	}

	if (depth_data.override_depth_stencil != 0)
	{
		const auto it = std::find_if(
			current_depth_stencil_list.begin(), 
			current_depth_stencil_list.end(),
			[resource = depth_data.override_depth_stencil](const auto &current) { return current.first == resource; });

		if (it != current_depth_stencil_list.end())
		{
			best_match = it->first;
			best_match_desc = device->get_resource_desc(it->first);
			best_snapshot = &it->second;
		}
	}

	if (best_match != 0)
	{
		const device_api api = device->get_api();

		depth_stencil_backup *depth_stencil_backup = device_data.find_depth_stencil_backup(best_match);

		// if it changed or it was never set
		if (best_match != depth_data.selected_depth_stencil || depth_data.selected_shader_resource == 0 || (s_preserve_depth_buffers && depth_stencil_backup == nullptr))
		{
			// Destroy previous resource view, since the underlying resource has changed
			if (depth_data.selected_shader_resource != 0)
			{
				runtime->get_command_queue()->wait_idle(); // Ensure resource view is no longer in-use before destroying it
				device->destroy_resource_view(depth_data.selected_shader_resource);

				//device_data.untrack_depth_stencil(data.selected_depth_stencil);
			}

			if (depth_data.selected_shader_resource_right != 0)
			{
				runtime->get_command_queue()->wait_idle(); // Ensure resource view is no longer in-use before destroying it
				device->destroy_resource_view(depth_data.selected_shader_resource_right);

				device_data.untrack_depth_stencil(depth_data.selected_depth_stencil);
			}

			depth_data.using_backup_texture = false;
			depth_data.selected_depth_stencil = best_match;
			depth_data.selected_shader_resource = { 0 };
			depth_data.selected_shader_resource_right = { 0 };

			// Create two-dimensional resource view to the first level and layer of the depth-stencil resource
			resource_view_desc srv_desc(api != device_api::opengl && api != device_api::vulkan ? format_to_default_typed(best_match_desc.texture.format) : best_match_desc.texture.format);
			//resource_view_desc srv_desc(best_match_desc.texture.format);

			// Need to create backup texture only if doing backup copies or original resource does not support shader access (which is necessary for binding it to effects)
			// Also always create a backup texture in D3D12 or Vulkan to circument problems in case application makes use of resource aliasing
			//if (true) //s_preserve_depth_buffers || (best_match_desc.usage & resource_usage::shader_resource) == 0 || (api == device_api::d3d12 || api == device_api::vulkan))
			//{
				depth_stencil_backup = device_data.track_depth_stencil_for_backup(device, best_match, best_match_desc);

				// Abort in case backup texture creation failed
				if (depth_stencil_backup->backup_texture == 0)
					return;

				if (depth_stencil_backup->backup_texture_right == 0)
					return;

				depth_stencil_backup->frame_width = frame_width;
				depth_stencil_backup->frame_height = frame_height;

				if (s_preserve_depth_buffers)
					reshade::config_get_value(nullptr, "DEPTH", "DepthCopyAtClearIndex", depth_stencil_backup->force_clear_index);
				else
					depth_stencil_backup->force_clear_index = 0;

				//if (api == device_api::d3d9)
				//	srv_desc.format = format::r32_float; // Same format as backup texture, as set in 'track_depth_stencil_for_backup'

				//if (!device->create_resource_view(depth_stencil_backup->backup_texture, resource_usage::shader_resource, srv_desc, &data.selected_shader_resource))
				//	return;

				depth_data.using_backup_texture = true;

				if (!device->create_resource_view(depth_stencil_backup->backup_texture, resource_usage::shader_resource, srv_desc, &depth_data.selected_shader_resource))
					return;

				if (!device->create_resource_view(depth_stencil_backup->backup_texture_right, resource_usage::shader_resource, srv_desc, &depth_data.selected_shader_resource_right))
					return;

				//data.using_backup_texture = true;
			//}
			// CITRA - this code path should never run cause we ALWAYS preserve_depth_buffers
			// else
			// {
			// 	if (!device->create_resource_view(best_match, resource_usage::shader_resource, srv_desc, &data.selected_shader_resource))
			// 		return;
			// }

			update_effect_runtime(runtime);
		}

		if (depth_data.using_backup_texture)
		{
			assert(depth_stencil_backup != nullptr && depth_stencil_backup->backup_texture != 0 && best_snapshot != nullptr);
			const resource backup_texture = depth_stencil_backup->backup_texture;
			const resource backup_texture_right = depth_stencil_backup->backup_texture_right;

			// Copy to backup texture unless already copied during the current frame
			if (!best_snapshot->copied_during_frame && (best_match_desc.usage & resource_usage::copy_source) != 0)
			{
				// Ensure barriers are not created with 'D3D12_RESOURCE_STATE_[...]_SHADER_RESOURCE' when resource has 'D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE' flag set
				const resource_usage old_state = best_match_desc.usage & (resource_usage::depth_stencil | resource_usage::shader_resource);

				lock.lock();
				const auto it = std::find_if(device_data.current_depth_stencil_list.begin(), device_data.current_depth_stencil_list.end(),
					[best_match](const auto &current) { return current.first == best_match; });
				// Indicate that the copy is now being done, so it is not repeated in case effects are rendered by another runtime (e.g. when there are multiple present calls in a frame)
				if (it != device_data.current_depth_stencil_list.end())
					it->second.copied_during_frame = true;
				else
					// Resource disappeared from the current depth-stencil list between earlier in this function and now, which indicates that it was destroyed in the meantime
					return;
				lock.unlock();

				cmd_list->barrier(best_match, old_state, resource_usage::copy_source);
				cmd_list->copy_resource(best_match, backup_texture);
				cmd_list->barrier(best_match, resource_usage::copy_source, old_state);

				cmd_list->barrier(best_match, old_state, resource_usage::copy_source);
				cmd_list->copy_resource(best_match, backup_texture_right);
				cmd_list->barrier(best_match, resource_usage::copy_source, old_state);
			}

			cmd_list->barrier(backup_texture, resource_usage::copy_dest, resource_usage::shader_resource);

			cmd_list->barrier(backup_texture_right, resource_usage::copy_dest, resource_usage::shader_resource);
		}

		// Citra Change: note: this used to be if/else
		// i'm changing it to ALWAYS copy the selected override clear pass for the stencil AND the final output of the stencil
		// we want to bind left and right eye z-depth passes to shader land
		// NOTE/TODO: for some games, we _might_ want the "other" pass to be a *different* clear pass, or even a *different* stencil's final||specific clear pass
		// we might also need the flexibility for a DYNAMIC/Auto "best" clear pass with most verticies or something like that
		// for now, i'm just trying to get the basics working, which is, copying TWO buffer snapshots and binding them
		// once that's working, i can get more fancy with cloning / assigning what is Left vs. Right from different combinations of Stencil+Pass combinations / definitions.
		// SO; right now i'm just testing, IF i call cmd_list->barrier twice, can i then fetch & bind the resources? or does calling it twice just clobber the memory?
		// IF it just clobbers it, how can I reserve a space in linear memory for the ADDITIONAL z-buffer?
		// I know for a fact that Geo3D for example, uses alternating frames in order to handle his left/right z-passes
		// where as I want to handle them simultaneously with each frame, not in alternating frames.
		// Citra makes them both available by RE-WRITING to the same stencil, 
		// SO, in theory, I should be able to copy on Clear and copy on Final (or another Clear on the same stencil / final or clear on a different stencil depending on the game) THEN, if i just bind that memory to ORIG_DEPTH_LEFT and ORIG_DEPTH_RIGHT
		// my Effects/Shaders should be able to display Side-by-Side depth data
		//else
		//{
			// Unset current depth-stencil view, in case it is bound to an effect as a shader resource (which will fail if it is still bound on output)
			/*if (api <= device_api::d3d11)
				cmd_list->bind_render_targets_and_depth_stencil(0, nullptr);

			cmd_list->barrier(best_match, resource_usage::depth_stencil | resource_usage::shader_resource, resource_usage::shader_resource);*/
		//}
	}
	else
	{
		// Unset any existing depth-stencil selected in previous frames
		if (depth_data.selected_depth_stencil != 0)
		{
			if (depth_data.selected_shader_resource != 0)
			{
				runtime->get_command_queue()->wait_idle(); // Ensure resource view is no longer in-use before destroying it
				device->destroy_resource_view(depth_data.selected_shader_resource);
			}
			if (depth_data.selected_shader_resource_right != 0)
			{
				runtime->get_command_queue()->wait_idle(); // Ensure resource view is no longer in-use before destroying it
				device->destroy_resource_view(depth_data.selected_shader_resource_right);
			}
			device_data.untrack_depth_stencil(depth_data.selected_depth_stencil);

			depth_data.using_backup_texture = false;
			depth_data.selected_depth_stencil = { 0 };
			depth_data.selected_shader_resource = { 0 };
			depth_data.selected_shader_resource_right = { 0 };

			update_effect_runtime(runtime);
		}
	}
}
static void on_finish_render_effects(effect_runtime *runtime, command_list *cmd_list, resource_view, resource_view)
{
	const generic_depth_data &data = runtime->get_private_data<generic_depth_data>();

	if (data.selected_shader_resource != 0)
	{
		if (data.using_backup_texture)
		{
			const resource backup_texture = runtime->get_device()->get_resource_from_view(data.selected_shader_resource);
			cmd_list->barrier(backup_texture, resource_usage::shader_resource, resource_usage::copy_dest);

			const resource backup_texture_right = runtime->get_device()->get_resource_from_view(data.selected_shader_resource_right);
			cmd_list->barrier(backup_texture_right, resource_usage::shader_resource, resource_usage::copy_dest);
		}
		else
		{
			cmd_list->barrier(data.selected_depth_stencil, resource_usage::shader_resource, resource_usage::depth_stencil | resource_usage::shader_resource);
		}
	}
}

static inline const char *format_to_string(format format) {
	switch (format)
	{
	case format::d16_unorm:
	case format::r16_typeless:
		return "D16  ";
	case format::d16_unorm_s8_uint:
		return "D16S8";
	case format::d24_unorm_x8_uint:
		return "D24X8";
	case format::d24_unorm_s8_uint:
	case format::r24_g8_typeless:
		return "D24S8";
	case format::d32_float:
	case format::r32_float:
	case format::r32_typeless:
		return "D32  ";
	case format::d32_float_s8_uint:
	case format::r32_g8_typeless:
		return "D32S8";
	case format::intz:
		return "INTZ ";
	default:
		return "     ";
	}
}

static void draw_settings_overlay(effect_runtime *runtime)
{
	device *const device = runtime->get_device();
	generic_depth_data &data = runtime->get_private_data<generic_depth_data>();
	generic_depth_device_data &device_data = device->get_private_data<generic_depth_device_data>();

	ImGui::Text("draws_without_depth %u", draws_without_depth);
	ImGui::Text("max_draws_without_depth %u", max_draws_without_depth);
	ImGui::Text("max_draws_per_frame %u", max_draws_per_frame);
	ImGui::Text("frames %u", frames);
	ImGui::Text("draws_this_frame %u", draws_this_frame);
	ImGui::Text("left_eye_copies_this_frame %u", left_eye_copies_this_frame);
	ImGui::Text("right_eye_copies_this_frame %u", right_eye_copies_this_frame);
	ImGui::Text("clears_this_frame %u", clears_this_frame);
	ImGui::Text("fullscreen_draws_this_frame %u", fullscreen_draws_this_frame);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Text("Current render pass count: %u", device_data.last_render_pass_count);
	ImGui::Text("Offset from end of frame to render effects at: %u", device_data.offset_from_last_pass);

	if (device_data.offset_from_last_pass < device_data.last_render_pass_count)
	{
		ImGui::SameLine();
		if (ImGui::SmallButton("+"))
			device_data.offset_from_last_pass++;
	}

	if (device_data.offset_from_last_pass != 0)
	{
		ImGui::SameLine();
		if (ImGui::SmallButton("-"))
			device_data.offset_from_last_pass--;
	}

	bool force_reset = false;

	if (bool do_break_on_clear = s_do_break_on_clear != 0;
		ImGui::Checkbox("Do Break On Clear", &do_break_on_clear)
		)
	{
		s_do_break_on_clear = do_break_on_clear ? 1 : 0;
		reshade::config_set_value(nullptr, "DEPTH", "DoBreakOnClear", s_do_break_on_clear);
	}

	/*if (bool use_aspect_ratio_heuristics = s_use_aspect_ratio_heuristics != 0;
		ImGui::Checkbox("Use aspect ratio heuristics", &use_aspect_ratio_heuristics))
	{
		s_use_aspect_ratio_heuristics = use_aspect_ratio_heuristics ? 1 : 0;
		reshade::config_set_value(nullptr, "DEPTH", "UseAspectRatioHeuristics", s_use_aspect_ratio_heuristics);
		force_reset = true;
	}*/

	/*if (s_use_aspect_ratio_heuristics)
	{
		if (bool use_aspect_ratio_heuristics_ex = s_use_aspect_ratio_heuristics == 2;
			ImGui::Checkbox("Use extended aspect ratio heuristics (for DLSS or resolution scaling)", &use_aspect_ratio_heuristics_ex))
		{
			s_use_aspect_ratio_heuristics = use_aspect_ratio_heuristics_ex ? 2 : 1;
			reshade::config_set_value(nullptr, "DEPTH", "UseAspectRatioHeuristics", s_use_aspect_ratio_heuristics);
			force_reset = true;
		}
	}*/

	// CITRA - disabling this as an option. we _always_ want to do this. (bringing back temporarily to test using it as a jogger)
	if (bool copy_before_clear_operations = s_preserve_depth_buffers != 0;
		ImGui::Checkbox("Copy depth buffer before clear operations", &copy_before_clear_operations))
	{
		s_preserve_depth_buffers = copy_before_clear_operations ? 1 : 0;
		reshade::config_set_value(nullptr, "DEPTH", "DepthCopyBeforeClears", s_preserve_depth_buffers);
		force_reset = true;
	}

	const bool is_d3d12_or_vulkan = device->get_api() == device_api::d3d12 || device->get_api() == device_api::vulkan;

	// CITRA - do we need this option?
	if (s_preserve_depth_buffers || is_d3d12_or_vulkan)
	{
		if (bool copy_before_fullscreen_draws = s_preserve_depth_buffers == 2;
			ImGui::Checkbox(is_d3d12_or_vulkan ? "Copy depth buffer during frame to prevent artifacts" : "Copy depth buffer before fullscreen draw calls", &copy_before_fullscreen_draws))
		{
			s_preserve_depth_buffers = copy_before_fullscreen_draws ? 2 : 1;
			reshade::config_set_value(nullptr, "DEPTH", "DepthCopyBeforeClears", s_preserve_depth_buffers);
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	std::shared_lock<std::shared_mutex> lock(s_mutex);

	if (device_data.current_depth_stencil_list.empty())
	{
		ImGui::TextUnformatted("No depth buffers found.");
		return;
	}

	// Sort pointer list so that added/removed items do not change the GUI much
	struct depth_stencil_item
	{
		unsigned int display_count;
		resource resource;
		depth_stencil_info snapshot;
		resource_desc desc;
	};

	std::vector<depth_stencil_item> sorted_item_list;
	sorted_item_list.reserve(device_data.current_depth_stencil_list.size());

	for (const auto &[resource, snapshot] : device_data.current_depth_stencil_list)
	{
		if (auto it = data.display_count_per_depth_stencil.find(resource);
			it == data.display_count_per_depth_stencil.end())
		{
			sorted_item_list.push_back({ 1u, resource, snapshot, device->get_resource_desc(resource) });
		}
		else
		{
			sorted_item_list.push_back({ it->second + 1u, resource, snapshot, device->get_resource_desc(resource) });
		}
	}

	lock.unlock();

	std::sort(sorted_item_list.begin(), sorted_item_list.end(), [](const depth_stencil_item &a, const depth_stencil_item &b) {
		return (a.display_count > b.display_count) ||
			(a.display_count == b.display_count && ((a.desc.texture.width > b.desc.texture.width || (a.desc.texture.width == b.desc.texture.width && a.desc.texture.height > b.desc.texture.height)) ||
			(a.desc.texture.width == b.desc.texture.width && a.desc.texture.height == b.desc.texture.height && a.resource < b.resource)));
	});

	bool has_msaa_depth_stencil = false;
	bool has_no_clear_operations = false;

	data.display_count_per_depth_stencil.clear();
	for (const depth_stencil_item &item : sorted_item_list)
	{
		data.display_count_per_depth_stencil[item.resource] = item.display_count;

		char label[512] = "";
		sprintf_s(label, "%c 0x%016llx", (item.resource == data.selected_depth_stencil ? '>' : ' '), item.resource.handle);

		if (item.desc.texture.samples > 1) // Disable widget for MSAA textures
		{
			has_msaa_depth_stencil = true;

			ImGui::BeginDisabled();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
		}

		if (bool value = (item.resource == data.override_depth_stencil);
			ImGui::Checkbox(label, &value))
		{
			data.override_depth_stencil = value ? item.resource : resource{ 0 };
			force_reset = true;
		}

		ImGui::SameLine();
		ImGui::Text("| %4ux%-4u | %s | %5u draw calls (%5u indirect) ==> %8u vertices |%s",
			item.desc.texture.width,
			item.desc.texture.height,
			format_to_string(item.desc.texture.format),
			item.snapshot.total_stats.drawcalls,
			item.snapshot.total_stats.drawcalls_indirect,
			item.snapshot.total_stats.vertices,
			(item.desc.texture.samples > 1 ? " MSAA" : ""));

		if (item.desc.texture.samples > 1)
		{
			ImGui::PopStyleColor();
			ImGui::EndDisabled();
		}

		if (s_preserve_depth_buffers && item.resource == data.selected_depth_stencil)
		{
			if (item.snapshot.clears.empty())
			{
				has_no_clear_operations = !is_d3d12_or_vulkan;
				continue;
			}

			depth_stencil_backup *const depth_stencil_backup = device_data.find_depth_stencil_backup(item.resource);
			if (depth_stencil_backup == nullptr)// || depth_stencil_backup->backup_texture == 0)
				continue;

			for (size_t clear_index = 1; clear_index <= item.snapshot.clears.size(); ++clear_index)
			{
				const auto &clear_stats = item.snapshot.clears[clear_index - 1];

				sprintf_s(label, "%c   CLEAR %2zu", clear_stats.copied_during_frame ? '>' : ' ', clear_index);

				if (bool value = (depth_stencil_backup->force_clear_index == clear_index);
					ImGui::Checkbox(label, &value))
				{
					depth_stencil_backup->force_clear_index = value ? clear_index : 0;
					reshade::config_set_value(nullptr, "DEPTH", "DepthCopyAtClearIndex", depth_stencil_backup->force_clear_index);
				}

				ImGui::SameLine();
				ImGui::Text("        |           |       | %5u draw calls (%5u indirect) ==> %8u vertices |%s",
					clear_stats.drawcalls,
					clear_stats.drawcalls_indirect,
					clear_stats.vertices,
					clear_stats.clear_op == clear_op::fullscreen_draw ? " Fullscreen draw call" : "");
			}

			if (sorted_item_list.size() == 1 && !is_d3d12_or_vulkan)
			{
				if (bool value = (depth_stencil_backup->force_clear_index == std::numeric_limits<size_t>::max());
					ImGui::Checkbox("    Choose last clear operation with high number of draw calls", &value))
				{
					depth_stencil_backup->force_clear_index = value ? std::numeric_limits<size_t>::max() : 0;
					reshade::config_set_value(nullptr, "DEPTH", "DepthCopyAtClearIndex", depth_stencil_backup->force_clear_index);
				}
			}
		}
	}

	if (has_msaa_depth_stencil || has_no_clear_operations)
	{
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		ImGui::PushTextWrapPos();
		if (has_msaa_depth_stencil)
			ImGui::TextUnformatted("Not all depth buffers are available.\nYou may have to disable MSAA in the game settings for depth buffer detection to work!");
		if (has_no_clear_operations)
			ImGui::Text("No clear operations were found for the selected depth buffer.\n%s",
				s_preserve_depth_buffers != 2 ? "Try enabling \"Copy depth buffer before fullscreen draw calls\" or disable \"Copy depth buffer before clear operations\"!" : "Disable \"Copy depth buffer before clear operations\" or select a different depth buffer!");
		ImGui::PopTextWrapPos();
	}

	if (force_reset)
	{
		// Reset selected depth-stencil to force re-creation of resources next frame (like the backup texture)
		if (data.selected_shader_resource != 0)
		{
			command_queue *const queue = runtime->get_command_queue();

			queue->wait_idle(); // Ensure resource view is no longer in-use before destroying it
			device->destroy_resource_view(data.selected_shader_resource);
			device->destroy_resource_view(data.selected_shader_resource_right);

			device_data.untrack_depth_stencil(data.selected_depth_stencil);
		}

		data.using_backup_texture = false;
		data.selected_depth_stencil = { 0 };
		data.selected_shader_resource = { 0 };
		data.selected_shader_resource_right = { 0 };

		update_effect_runtime(runtime);
	}
}

// d3d12 / vulkan, but not ogl :G
// Called after game has rendered a render pass, so check if it makes sense to render effects then (e.g. after main scene rendering, before UI rendering)
//static void on_end_render_pass(command_list* cmd_list)
//{
//	auto& data = cmd_list->get_private_data<command_list_data>();
//
//	if (data.has_multiple_rtvs || data.current_main_rtv == 0)
//		return; // Ignore when game is rendering to multiple render targets simultaneously
//
//	device* const device = cmd_list->get_device();
//	const auto& dev_data = device->get_private_data<generic_depth_device_data>();
//
//	auto& state = cmd_list->get_private_data<state_tracking>();
//
//	uint32_t width, height;
//	dev_data.main_runtime->get_screenshot_width_and_height(&width, &height);
//
//	const resource_desc render_target_desc = device->get_resource_desc(device->get_resource_from_view(data.current_main_rtv));
//
//	if (render_target_desc.texture.width != width || render_target_desc.texture.height != height)
//		return; // Ignore render targets that do not match the effect runtime back buffer dimensions
//
//	//generic_backbuffer_backup *my_backup = dev_data->my_backbuffer_backup;
//	//resource destination = dev_data.my_backbuffer_backup->left_pass_texture_resource;
//
//	// Render post-processing effects when a specific render pass is found (instead of at the end of the frame)
//	// This is not perfect, since there may be multiple command lists at this will try and render effects in every single one ...
//	if (data.current_render_pass_index++ == (dev_data.last_render_pass_count - dev_data.offset_from_last_pass)) {
//		//dev_data.main_runtime->render_effects(cmd_list, data.current_main_rtv);
//		//device->get_resource_desc(it->first)
//		// TODO: allow setting TWO offsets, one for left eye, one for right eye
//		// TODO: update
//		
//		//cmd_list->copy_resource(render_target_desc, dev_data.my_backbuffer_backup.left_resource_desc);
//		//cmd_list->copy_resource(depth_stencil, destination);
//
//		/*if (!device->create_resource_view(my_backup.left_pass_resource_view, resource_usage::shader_resource, srv_desc, &data.selected_shader_resource))
//			return;
//
//		if (!device->create_resource_view(my_backup->right_pass_resource_view, resource_usage::shader_resource, srv_desc, &data.selected_shader_resource_right))
//			return;*/
//	}
//}

UINT32 num_swapchains_init = 0;

static void on_init_swapchain(swapchain* swapchain)
{
	num_swapchains_init++;
	if (num_swapchains_init < 3) {
		return;
	}
	user_data& data = swapchain->create_private_data<user_data>();

	device *const my_device = swapchain->get_device();
	generic_depth_device_data &depth_dev = my_device->get_private_data<generic_depth_device_data>();
	generic_backbuffer_backup &my_backup = depth_dev.my_backbuffer_backup;

	my_backup.swapchain_pointer = swapchain;

	// Get description of the back buffer resources
	const resource_desc desc = my_device->get_resource_desc(swapchain->get_current_back_buffer());
	const resource_desc my_desc = resource_desc(
		desc.texture.width,
		desc.texture.height,
		1,
		1,
		//format::b8g8r8a8_unorm,
		desc.texture.format,
		//format::r8g8b8a8_typeless,
		1,
		memory_heap::gpu_only, //memory_heap::gpu_to_cpu,
		resource_usage::shader_resource | resource_usage::copy_dest);

	const resource_desc my_desc_2 = resource_desc(
		desc.texture.width,
		desc.texture.height,
		1,
		1,
		//format::r8g8b8a8_typeless,
		desc.texture.format,
		1,
		memory_heap::gpu_only, //memory_heap::gpu_to_cpu,
		resource_usage::shader_resource | resource_usage::copy_dest);

	// LEFT EYE
	if (!my_device->create_resource(
		my_desc,
		nullptr,
		resource_usage::shader_resource | resource_usage::copy_dest, //resource_usage::cpu_access,
		&my_backup.left_pass_texture_resource))
	{
		reshade::log_message(1, "Failed to create host resource");
		return;
	}

	// RIGHT EYE
	if (!my_device->create_resource(
		my_desc_2,
		nullptr,
		resource_usage::shader_resource | resource_usage::copy_dest, //resource_usage::cpu_access,
		&my_backup.right_pass_texture_resource))
	{
		reshade::log_message(1, "Failed to create host resource");
		return;
	}


	resource_view_desc srv_desc(my_desc.texture.format);
	resource_view_desc srv_desc2(my_desc_2.texture.format);

	if (!my_device->create_resource_view(my_backup.left_pass_texture_resource, resource_usage::shader_resource, srv_desc, &my_backup.left_pass_resource_view)) {
		return;
	}
		
	if (!my_device->create_resource_view(my_backup.right_pass_texture_resource, resource_usage::shader_resource, srv_desc2, &my_backup.right_pass_resource_view)) {
		return;
	}

	/*if (depth_dev.main_runtime) {
		update_effect_runtime(depth_dev.main_runtime);
	}*/
	

	bool complete = true;
}

static void on_destroy_swapchain(swapchain* swapchain)
{
	// destroy swapchain referencing resources
	/*device* const my_device = swapchain->get_device();
	generic_depth_device_data& depth_dev = my_device->get_private_data<generic_depth_device_data>();
	generic_backbuffer_backup& my_backup = depth_dev.my_backbuffer_backup;

	my_device->destroy_resource_view(my_backup.left_pass_resource_view);
	my_device->destroy_resource_view(my_backup.right_pass_resource_view);

	my_device->destroy_resource(my_backup.left_pass_texture_resource);
	my_device->destroy_resource(my_backup.right_pass_texture_resource);

	my_backup.left_pass_resource_view = { 0 };
	my_backup.right_pass_resource_view = { 0 };
	my_backup.left_pass_texture_resource = { 0 };
	my_backup.right_pass_texture_resource = { 0 };
	my_backup.swapchain_pointer = { 0 };*/
}

void register_addon_depth()
{
	reshade::register_overlay(nullptr, draw_settings_overlay);

	reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
	reshade::register_event<reshade::addon_event::init_device>(on_init_device);
	reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
	reshade::register_event<reshade::addon_event::init_command_queue>(on_init_command_queue);
	reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
	reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
	reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
	reshade::register_event<reshade::addon_event::destroy_command_queue>(on_destroy_command_queue);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);

	reshade::register_event<reshade::addon_event::create_resource>(on_create_resource);
	reshade::register_event<reshade::addon_event::create_resource_view>(on_create_resource_view);
	reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);

	reshade::register_event<reshade::addon_event::draw>(on_draw);
	reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
	reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
	reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewport);
	reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass_with_depth_stencil);
	reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_depth_stencil);
	reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_depth_stencil);

	reshade::register_event<reshade::addon_event::reset_command_list>(on_reset);
	reshade::register_event<reshade::addon_event::execute_command_list>(on_execute_primary);
	reshade::register_event<reshade::addon_event::execute_secondary_command_list>(on_execute_secondary);

	reshade::register_event<reshade::addon_event::present>(on_present);

	reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_render_effects);
	reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_render_effects);
	// Need to set texture binding again after reloading
	reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(update_effect_runtime);

	reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
}
void unregister_addon_depth()
{
	reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
	reshade::unregister_event<reshade::addon_event::init_command_list>(on_init_command_list);
	reshade::unregister_event<reshade::addon_event::init_command_queue>(on_init_command_queue);
	reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
	reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
	reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
	reshade::unregister_event<reshade::addon_event::destroy_command_queue>(on_destroy_command_queue);
	reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);

	reshade::unregister_event<reshade::addon_event::create_resource>(on_create_resource);
	reshade::unregister_event<reshade::addon_event::create_resource_view>(on_create_resource_view);
	reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);

	reshade::unregister_event<reshade::addon_event::draw>(on_draw);
	reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
	reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(on_draw_indirect);
	reshade::unregister_event<reshade::addon_event::bind_viewports>(on_bind_viewport);
	reshade::unregister_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass_with_depth_stencil);
	reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_depth_stencil);
	reshade::unregister_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_depth_stencil);

	reshade::unregister_event<reshade::addon_event::reset_command_list>(on_reset);
	reshade::unregister_event<reshade::addon_event::execute_command_list>(on_execute_primary);
	reshade::unregister_event<reshade::addon_event::execute_secondary_command_list>(on_execute_secondary);

	reshade::unregister_event<reshade::addon_event::present>(on_present);

	reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin_render_effects);
	reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_finish_render_effects);
	reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(update_effect_runtime);

	reshade::unregister_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
}

extern "C" __declspec(dllexport) const char *NAME = "Citra";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "add-on that pre-processes depth buffer from Citra to be standardized / aligned for other add-ons to consume it.";

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hinstDLL))
			return FALSE;
		register_addon_depth();
		break;
	case DLL_PROCESS_DETACH:
		unregister_addon_depth();
		reshade::unregister_addon(hinstDLL);
		break;
	}
	return TRUE;
}
