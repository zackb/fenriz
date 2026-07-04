#pragma once

// wlroots ships C headers without extern "C" guards, so including them directly from
// C++ mangles the symbols and linking fails. Include all wlroots headers through this
// shim instead — add new ones here as the compositor grows.
extern "C" {

// wlroots' render/color.h declares a few functions with C99 `arr[static N]` parameters,
// which C++ rejects. Those functions are unused here and color.h uses `static` for
// nothing else, so neutralize the keyword while pulling color.h in first; its include
// guard then suppresses the breaking re-include from pass.h / wlr_output.h.
// ponytail: pin-scoped #define, not a global one — revisit if a future wlroots header
// needs both `static` and `[static N]` in the same file.
#define static
#include <wlr/render/color.h>
#undef static

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_viewporter.h>
// wlr_layer_shell_v1.h (and its generated protocol header) use `namespace` as a struct
// field / parameter name, which is a C++ keyword. Rename it away for this include only;
// fenriz never touches that field. Same pin-scoped tactic as the `static` guard above.
#define namespace namespace_
#include <wlr/types/wlr_layer_shell_v1.h>
#undef namespace
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
}
