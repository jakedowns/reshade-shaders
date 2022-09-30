/*
 * 2022 Jake Downs
 */

//#include <imgui.h>
#include <reshade.hpp>
//#include <cmath>
//#include <cstring>
//#include <algorithm>
//#include <vector>
//#include <shared_mutex>
//#include <unordered_map>

using namespace reshade::api;

//static std::shared_mutex s_mutex;

static void on_reshade_reloaded_effects(effect_runtime *runtime)
{
	resource_view srv, srv_srgb;

	effect_texture_variable ModifiedDepthTex_handle = runtime->find_texture_variable("ModifyDepth.fx", "ModifiedDepthTex");
	runtime->get_texture_binding(ModifiedDepthTex_handle, &srv, &srv_srgb);

	runtime->update_texture_bindings("DEPTH", srv, srv_srgb);
}

extern "C" __declspec(dllexport) const char *NAME = "Citra Add-On";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "add-on that pre-processes depth buffer from Citra to be standardized / aligned for other add-ons to consume it.";

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hinstDLL))
			return FALSE;
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&on_reshade_reloaded_effects);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(hinstDLL);
		break;
	}
	return TRUE;
}
