#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <wlc/wlc.h>
#include "extensions.h"
#include "layout.h"
#include "log.h"
#include "list.h"
#include "config.h"
#include "container.h"
#include "workspace.h"
#include "focus.h"
#include "output.h"
#include "ipc-server.h"

swayc_t root_container;
list_t *scratchpad;

int min_sane_h = 60;
int min_sane_w = 100;

void init_layout(void) {
	root_container.type = C_ROOT;
	root_container.layout = L_NONE;
	root_container.children = create_list();
	root_container.handle = -1;
	root_container.visible = true;
	scratchpad = create_list();
}

int index_child(const swayc_t *child) {
	swayc_t *parent = child->parent;
	int i, len;
	if (!child->is_floating) {
		len = parent->children->length;
		for (i = 0; i < len; ++i) {
			if (parent->children->items[i] == child) {
				break;
			}
		}
	} else {
		len = parent->floating->length;
		for (i = 0; i < len; ++i) {
			if (parent->floating->items[i] == child) {
				break;
			}
		}
	}
	if (!sway_assert(i < len, "Stray container")) {
		return -1;
	}
	return i;
}

void add_child(swayc_t *parent, swayc_t *child) {
	sway_log(L_DEBUG, "Adding %p (%d, %fx%f) to %p (%d, %fx%f)", child, child->type,
		child->width, child->height, parent, parent->type, parent->width, parent->height);
	list_add(parent->children, child);
	child->parent = parent;
	// set focus for this container
	if (!parent->focused) {
		parent->focused = child;
	}
}

void insert_child(swayc_t *parent, swayc_t *child, int index) {
	if (index > parent->children->length) {
		index = parent->children->length;
	}
	if (index < 0) {
		index = 0;
	}
	list_insert(parent->children, index, child);
	child->parent = parent;
	if (!parent->focused) {
		parent->focused = child;
	}
}

void add_floating(swayc_t *ws, swayc_t *child) {
	sway_log(L_DEBUG, "Adding %p (%d, %fx%f) to %p (%d, %fx%f)", child, child->type,
		child->width, child->height, ws, ws->type, ws->width, ws->height);
	if (!sway_assert(ws->type == C_WORKSPACE, "Must be of workspace type")) {
		return;
	}
	list_add(ws->floating, child);
	child->parent = ws;
	child->is_floating = true;
	if (!ws->focused) {
		ws->focused = child;
	}
}

swayc_t *add_sibling(swayc_t *fixed, swayc_t *active) {
	swayc_t *parent = fixed->parent;
	int i = index_child(fixed);
	if (fixed->is_floating) {
		list_insert(parent->floating, i + 1, active);
	} else {
		list_insert(parent->children, i + 1, active);
	}
	active->parent = parent;
	// focus new child
	parent->focused = active;
	return active->parent;
}

swayc_t *replace_child(swayc_t *child, swayc_t *new_child) {
	swayc_t *parent = child->parent;
	if (parent == NULL) {
		return NULL;
	}
	int i = index_child(child);
	if (child->is_floating) {
		parent->floating->items[i] = new_child;
	} else {
		parent->children->items[i] = new_child;
	}
	// Set parent and focus for new_child
	new_child->parent = child->parent;
	if (child->parent->focused == child) {
		child->parent->focused = new_child;
	}
	child->parent = NULL;

	// Set geometry for new child
	new_child->x = child->x;
	new_child->y = child->y;
	new_child->width = child->width;
	new_child->height = child->height;

	// reset geometry for child
	child->width = 0;
	child->height = 0;

	// deactivate child
	if (child->type == C_VIEW) {
		wlc_view_set_state(child->handle, WLC_BIT_ACTIVATED, false);
	}
	return parent;
}

swayc_t *remove_child(swayc_t *child) {
	int i;
	swayc_t *parent = child->parent;
	if (child->is_floating) {
		// Special case for floating views
		for (i = 0; i < parent->floating->length; ++i) {
			if (parent->floating->items[i] == child) {
				list_del(parent->floating, i);
				break;
			}
		}
		i = 0;
	} else {
		for (i = 0; i < parent->children->length; ++i) {
			if (parent->children->items[i] == child) {
				list_del(parent->children, i);
				break;
			}
		}
	}
	// Set focused to new container
	if (parent->focused == child) {
		if (parent->children->length > 0) {
			parent->focused = parent->children->items[i ? i-1:0];
		} else if (parent->floating && parent->floating->length) {
			parent->focused = parent->floating->items[parent->floating->length - 1];
		} else {
			parent->focused = NULL;
		}
	}
	child->parent = NULL;
	// deactivate view
	if (child->type == C_VIEW) {
		wlc_view_set_state(child->handle, WLC_BIT_ACTIVATED, false);
	}
	return parent;
}

void swap_container(swayc_t *a, swayc_t *b) {
	if (!sway_assert(a&&b, "parameters must be non null") ||
		!sway_assert(a->parent && b->parent, "containers must have parents")) {
		return;
	}
	size_t a_index = index_child(a);
	size_t b_index = index_child(b);
	swayc_t *a_parent = a->parent;
	swayc_t *b_parent = b->parent;
	// Swap the pointers
	if (a->is_floating) {
		a_parent->floating->items[a_index] = b;
	} else {
		a_parent->children->items[a_index] = b;
	}
	if (b->is_floating) {
		b_parent->floating->items[b_index] = a;
	} else {
		b_parent->children->items[b_index] = a;
	}
	a->parent = b_parent;
	b->parent = a_parent;
	if (a_parent->focused == a) {
		a_parent->focused = b;
	}
	// dont want to double switch
	if (b_parent->focused == b && a_parent != b_parent) {
		b_parent->focused = a;
	}
}

void swap_geometry(swayc_t *a, swayc_t *b) {
	double x = a->x;
	double y = a->y;
	double w = a->width;
	double h = a->height;
	a->x = b->x;
	a->y = b->y;
	a->width = b->width;
	a->height = b->height;
	b->x = x;
	b->y = y;
	b->width = w;
	b->height = h;
}

void move_container(swayc_t *container, enum movement_direction dir) {
	enum swayc_layouts layout;
	if (container->is_floating
			|| (container->type != C_VIEW && container->type != C_CONTAINER)) {
		return;
	}
	if (dir == MOVE_UP || dir == MOVE_DOWN) {
		layout = L_VERT;
	} else if (dir == MOVE_LEFT || dir == MOVE_RIGHT) {
		layout = L_HORIZ;
	} else {
		return;
	}
	swayc_t *parent = container->parent;
	swayc_t *child = container;
	bool ascended = false;
	while (true) {
		sway_log(L_DEBUG, "container:%p, parent:%p, child %p,",
				container,parent,child);
		if (parent->layout == layout) {
			int diff;
			// If it has ascended (parent has moved up), no container is removed
			// so insert it at index, or index+1.
			// if it has not, the moved container is removed, so it needs to be
			// inserted at index-1, or index+1
			if (ascended) {
				diff = dir == MOVE_LEFT || dir == MOVE_UP ? 0 : 1;
			} else {
				diff = dir == MOVE_LEFT || dir == MOVE_UP ? -1 : 1;
			}
			int desired = index_child(child) + diff;
			// when it has ascended, legal insertion position is 0:len
			// when it has not, legal insertion position is 0:len-1
			if (desired >= 0 && desired - ascended < parent->children->length) {
				if (!ascended) {
					child = parent->children->items[desired];
					// Move container into sibling container
					if (child->type == C_CONTAINER) {
						parent = child;
						// Insert it in first/last if matching layout,otherwise
						// inesrt it next to focused container
						if (parent->layout == layout) {
							desired = (diff < 0) * parent->children->length;
						} else {
							desired = index_child(child->focused);
						}
						//reset geometry
						container->width = container->height = 0;
					}
				}
				swayc_t *old_parent = remove_child(container);
				insert_child(parent, container, desired);
				destroy_container(old_parent);
				sway_log(L_DEBUG,"Moving to %p %d",parent, desired);
				break;
			}
		}
		// Change parent layout if we need to
		if (parent->children->length == 1 && parent->layout != layout) {
			parent->layout = layout;
			continue;
		}
		if (parent->type == C_WORKSPACE) {
			// We simply cannot move any further.
			if (parent->layout == layout) {
				break;
			}
			// Create container around workspace to insert child into
			parent = new_container(parent, layout);
		}
		ascended = true;
		child = parent;
		parent = child->parent;
	}
	// Dirty hack to fix a certain case
	arrange_windows(parent, -1, -1);
	arrange_windows(parent->parent, -1, -1);
	set_focused_container_for(parent->parent, container);
}

void move_container_to(swayc_t* container, swayc_t* destination) {
	if (container == destination || swayc_is_parent_of(container, destination)) {
		return;
	}
	swayc_t *parent = remove_child(container);
	// Send to new destination
	if (container->is_floating) {
		swayc_t *ws = swayc_active_workspace_for(destination);
		add_floating(ws, container);

		// If the workspace only has one child after adding one, it
		// means that the workspace was just initialized.
		if (ws->children->length + ws->floating->length == 1) {
			ipc_event_workspace(NULL, ws, "init");
		}
	} else if (destination->type == C_WORKSPACE) {
		// reset container geometry
		container->width = container->height = 0;
		add_child(destination, container);

		// If the workspace only has one child after adding one, it
		// means that the workspace was just initialized.
		if (destination->children->length + destination->floating->length == 1) {
			ipc_event_workspace(NULL, destination, "init");
		}
	} else {
		// reset container geometry
		container->width = container->height = 0;
		add_sibling(destination, container);
	}
	// Destroy old container if we need to
	parent = destroy_container(parent);
	// Refocus
	swayc_t *op1 = swayc_parent_by_type(destination, C_OUTPUT);
	swayc_t *op2 = swayc_parent_by_type(parent, C_OUTPUT);
	set_focused_container(get_focused_view(op1));
	arrange_windows(op1, -1, -1);
	update_visibility(op1);
	if (op1 != op2) {
		set_focused_container(get_focused_view(op2));
		arrange_windows(op2, -1, -1);
		update_visibility(op2);
	}
}

void move_workspace_to(swayc_t* workspace, swayc_t* destination) {
	if (workspace == destination || swayc_is_parent_of(workspace, destination)) {
		return;
	}
	swayc_t *src_op = remove_child(workspace);
	// reset container geometry
	workspace->width = workspace->height = 0;
	add_child(destination, workspace);
	// Refocus destination (change to new workspace)
	set_focused_container(get_focused_view(workspace));
	arrange_windows(destination, -1, -1);
	update_visibility(destination);

	// make sure source output has a workspace
	if (src_op->children->length == 0) {
		char *ws_name = workspace_next_name();
		swayc_t *ws = new_workspace(src_op, ws_name);
		ws->is_focused = true;
		free(ws_name);
	}
	set_focused_container(get_focused_view(src_op));
	update_visibility(src_op);
}

void update_geometry(swayc_t *container) {
	if (container->type != C_VIEW) {
		return;
	}
	swayc_t *ws = swayc_parent_by_type(container, C_WORKSPACE);
	swayc_t *op = ws->parent;
	int gap = container->is_floating ? 0 : swayc_gap(container);
	if (gap % 2 != 0) {
		// because gaps are implemented as "half sized margins" it's currently
		// not possible to align views properly with odd sized gaps.
		gap -= 1;
	}

	struct wlc_geometry geometry = {
		.origin = {
			.x = container->x + gap/2 < op->width  ? container->x + gap/2 : op->width-1,
			.y = container->y + gap/2 < op->height ? container->y + gap/2 : op->height-1
		},
		.size = {
			.w = container->width > gap ? container->width - gap : 1,
			.h = container->height > gap ? container->height - gap : 1,
		}
	};
	if (swayc_is_fullscreen(container)) {
		swayc_t *output = swayc_parent_by_type(container, C_OUTPUT);
		const struct wlc_size *size = wlc_output_get_resolution(output->handle);
		geometry.origin.x = 0;
		geometry.origin.y = 0;
		geometry.size.w = size->w;
		geometry.size.h = size->h;
		if (op->focused == ws) {
			wlc_view_bring_to_front(container->handle);
		}
	} else if (!config->edge_gaps && gap > 0) {
		// Remove gap against the workspace edges. Because a pixel is not
		// divisable, depending on gap size and the number of siblings our view
		// might be at the workspace edge without being exactly so (thus test
		// with gap, and align correctly).
		if (container->x - gap <= ws->x) {
			geometry.origin.x = ws->x;
			geometry.size.w = container->width - gap/2;
		}
		if (container->y - gap <= ws->y) {
			geometry.origin.y = ws->y;
			geometry.size.h = container->height - gap/2;
		}
		if (container->x + container->width + gap >= ws->x + ws->width) {
			geometry.size.w = ws->x + ws->width - geometry.origin.x;
		}
		if (container->y + container->height + gap >= ws->y + ws->height) {
			geometry.size.h = ws->y + ws->height - geometry.origin.y;
		}
	}
	wlc_view_set_geometry(container->handle, 0, &geometry);
}

static void arrange_windows_r(swayc_t *container, double width, double height) {
	int i;
	if (width == -1 || height == -1) {
		swayc_log(L_DEBUG, container, "Arranging layout for %p", container);
		width = container->width;
		height = container->height;
	}
	// pixels are indivisable. if we don't round the pixels, then the view
	// calculations will be off (e.g. 50.5 + 50.5 = 101, but in reality it's
	// 50 + 50 = 100). doing it here cascades properly to all width/height/x/y.
	width = floor(width);
	height = floor(height);

	sway_log(L_DEBUG, "Arranging layout for %p %s %fx%f+%f,%f", container,
		container->name, container->width, container->height, container->x, container->y);

	double x = 0, y = 0;
	switch (container->type) {
	case C_ROOT:
		for (i = 0; i < container->children->length; ++i) {
			swayc_t *output = container->children->items[i];
			sway_log(L_DEBUG, "Arranging output '%s' at %f,%f", output->name, output->x, output->y);
			arrange_windows_r(output, -1, -1);
		}
		return;
	case C_OUTPUT:
		{
			struct wlc_size resolution = *wlc_output_get_resolution(container->handle);
			width = resolution.w; height = resolution.h;
			// output must have correct size due to e.g. seamless mouse,
			// but a workspace might be smaller depending on panels.
			container->width = width;
			container->height = height;
		}
		// arrange all workspaces:
		for (i = 0; i < container->children->length; ++i) {
			swayc_t *child = container->children->items[i];
			arrange_windows_r(child, -1, -1);
		}
		// Bring all unmanaged views to the front
		for (i = 0; i < container->unmanaged->length; ++i) {
			wlc_handle *handle = container->unmanaged->items[i];
			wlc_view_bring_to_front(*handle);
		}
		return;
	case C_WORKSPACE:
		{
			swayc_t *output = swayc_parent_by_type(container, C_OUTPUT);
			width = output->width, height = output->height;
			for (i = 0; i < desktop_shell.panels->length; ++i) {
				struct panel_config *config = desktop_shell.panels->items[i];
				if (config->output == output->handle) {
					struct wlc_size size = *wlc_surface_get_size(config->surface);
					sway_log(L_DEBUG, "-> Found panel for this workspace: %ux%u, position: %u", size.w, size.h, config->panel_position);
					switch (config->panel_position) {
					case DESKTOP_SHELL_PANEL_POSITION_TOP:
						y += size.h; height -= size.h;
						break;
					case DESKTOP_SHELL_PANEL_POSITION_BOTTOM:
						height -= size.h;
						break;
					case DESKTOP_SHELL_PANEL_POSITION_LEFT:
						x += size.w; width -= size.w;
						break;
					case DESKTOP_SHELL_PANEL_POSITION_RIGHT:
						width -= size.w;
						break;
					}
				}
			}
			int gap = swayc_gap(container);
			x = container->x = x + gap;
			y = container->y = y + gap;
			width = container->width = width - gap * 2;
			height = container->height = height - gap * 2;
			sway_log(L_DEBUG, "Arranging workspace '%s' at %f, %f", container->name, container->x, container->y);
		}
		 // children are properly handled below
		break;
	case C_VIEW:
		{
			container->width = width;
			container->height = height;
			update_geometry(container);
			sway_log(L_DEBUG, "Set view to %.f x %.f @ %.f, %.f", container->width,
					container->height, container->x, container->y);
		}
		return;
	default:
		container->width = width;
		container->height = height;
		x = container->x;
		y = container->y;
		break;
	}

	double scale = 0;
	switch (container->layout) {
	case L_HORIZ:
	default:
		// Calculate total width
		for (i = 0; i < container->children->length; ++i) {
			double *old_width = &((swayc_t *)container->children->items[i])->width;
			if (*old_width <= 0) {
				if (container->children->length > 1) {
					*old_width = width / (container->children->length - 1);
				} else {
					*old_width = width;
				}
			}
			scale += *old_width;
		}
		// Resize windows
		if (scale > 0.1) {
			scale = width / scale;
			sway_log(L_DEBUG, "Arranging %p horizontally", container);
			for (i = 0; i < container->children->length; ++i) {
				swayc_t *child = container->children->items[i];
				sway_log(L_DEBUG, "Calculating arrangement for %p:%d (will scale %f by %f)", child, child->type, width, scale);
				child->x = x;
				child->y = y;
				if (i == container->children->length - 1) {
					double remaining_width = container->x + width - x;
					arrange_windows_r(child, remaining_width, height);
				} else {
					arrange_windows_r(child, child->width * scale, height);
				}
				x += child->width;
			}
		}
		break;
	case L_VERT:
		// Calculate total height
		for (i = 0; i < container->children->length; ++i) {
			double *old_height = &((swayc_t *)container->children->items[i])->height;
			if (*old_height <= 0) {
				if (container->children->length > 1) {
					*old_height = height / (container->children->length - 1);
				} else {
					*old_height = height;
				}
			}
			scale += *old_height;
		}
		// Resize
		if (scale > 0.1) {
			scale = height / scale;
			sway_log(L_DEBUG, "Arranging %p vertically", container);
			for (i = 0; i < container->children->length; ++i) {
				swayc_t *child = container->children->items[i];
				sway_log(L_DEBUG, "Calculating arrangement for %p:%d (will scale %f by %f)", child, child->type, height, scale);
				child->x = x;
				child->y = y;
				if (i == container->children->length - 1) {
					double remaining_height = container->y + height - y;
					arrange_windows_r(child, width, remaining_height);
				} else {
					arrange_windows_r(child, width, child->height * scale);
				}
				y += child->height;
			}
		}
		break;
	}

	// Arrage floating layouts for workspaces last
	if (container->type == C_WORKSPACE) {
		for (i = 0; i < container->floating->length; ++i) {
			swayc_t *view = container->floating->items[i];
			if (view->type == C_VIEW) {
				update_geometry(view);
				if (swayc_is_fullscreen(view)) {
					wlc_view_bring_to_front(view->handle);
				} else if (!container->focused
						|| !swayc_is_fullscreen(container->focused)) {
					wlc_view_bring_to_front(view->handle);
				}
			}
		}
	}
}

void arrange_windows(swayc_t *container, double width, double height) {
	update_visibility(container);
	arrange_windows_r(container, width, height);
	layout_log(&root_container, 0);
}

swayc_t *get_swayc_in_direction_under(swayc_t *container, enum movement_direction dir, swayc_t *limit) {
	swayc_t *parent = container->parent;
	if (dir == MOVE_PARENT) {
		if (parent->type == C_OUTPUT) {
			return NULL;
		} else {
			return parent;
		}
	}
	// If moving to an adjacent output we need a starting position (since this
	// output might border to multiple outputs).
	struct wlc_point abs_pos;
	get_absolute_center_position(container, &abs_pos);

	if (container->type == C_VIEW && swayc_is_fullscreen(container)) {
		sway_log(L_DEBUG, "Moving from fullscreen view, skipping to output");
		container = swayc_parent_by_type(container, C_OUTPUT);
		get_absolute_center_position(container, &abs_pos);
		return swayc_adjacent_output(container, dir, &abs_pos, true);
	}

	if (container->type == C_WORKSPACE && container->fullscreen) {
		sway_log(L_DEBUG, "Moving to fullscreen view");
		return container->fullscreen;
	}

	while (true) {
		// Test if we can even make a difference here
		bool can_move = false;
		int diff = 0;
		if (parent->type == C_ROOT) {
			sway_log(L_DEBUG, "Moving between outputs");
			return swayc_adjacent_output(container, dir, &abs_pos, true);
		} else {
			if (dir == MOVE_LEFT || dir == MOVE_RIGHT) {
				if (parent->layout == L_HORIZ) {
					can_move = true;
					diff = dir == MOVE_LEFT ? -1 : 1;
				}
			} else {
				if (parent->layout == L_VERT) {
					can_move = true;
					diff = dir == MOVE_UP ? -1 : 1;
				}
			}
		}

		if (can_move) {
			int desired = index_child(container) + diff;
			if (container->is_floating || desired < 0 || desired >= parent->children->length) {
				can_move = false;
			} else {
				return parent->children->items[desired];
			}
		}
		if (!can_move) {
			container = parent;
			parent = parent->parent;
			if (!parent || container == limit) {
				// Nothing we can do
				return NULL;
			}
		}
	}
}

swayc_t *get_swayc_in_direction(swayc_t *container, enum movement_direction dir) {
	return get_swayc_in_direction_under(container, dir, NULL);
}

void recursive_resize(swayc_t *container, double amount, enum wlc_resize_edge edge) {
	int i;
	bool layout_match = true;
	sway_log(L_DEBUG, "Resizing %p with amount: %f", container, amount);
	if (edge == WLC_RESIZE_EDGE_LEFT || edge == WLC_RESIZE_EDGE_RIGHT) {
		container->width += amount;
		layout_match = container->layout == L_HORIZ;
	} else if (edge == WLC_RESIZE_EDGE_TOP || edge == WLC_RESIZE_EDGE_BOTTOM) {
		container->height += amount;
		layout_match = container->layout == L_VERT;
	}
	if (container->type == C_VIEW) {
		update_geometry(container);
		return;
	}
	if (layout_match) {
		for (i = 0; i < container->children->length; i++) {
			recursive_resize(container->children->items[i], amount/container->children->length, edge);
		}
	} else {
		for (i = 0; i < container->children->length; i++) {
			recursive_resize(container->children->items[i], amount, edge);
		}
	}
}
