/**************************************************************************/
/*  box_impostor_editor_plugin.cpp                                       */
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

#include "box_impostor_editor_plugin.h"

#include "core/io/resource_saver.h"
#include "core/object/callable_mp.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/3d/box_impostor_baker.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/octahedral_impostor_baker.h"

void BoxImpostorEditorPlugin::_bake_selected(bool p_octahedral) {
	Node *scene_root = EditorNode::get_singleton()->get_edited_scene();
	List<Node3D *> sources;
	if (scene_root) {
		for (Node *n : EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list()) {
			Node3D *n3d = Object::cast_to<Node3D>(n);
			if (n3d && n3d != scene_root && n3d->get_parent()) {
				sources.push_back(n3d);
			}
		}
	}
	if (sources.is_empty()) {
		EditorNode::get_singleton()->show_warning(p_octahedral ? TTR("Select one or more Node3D (not the scene's root) whose children are the model to bake into an octahedral impostor.") : TTR("Select one or more Node3D (not the scene's root) whose children are the model to bake into a box impostor."));
		return;
	}

	const String scene_path = scene_root->get_scene_file_path();
	const String dir = scene_path.is_empty() ? String("res://") : scene_path.get_base_dir();
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(p_octahedral ? TTR("Bake Octahedral Impostor") : TTR("Bake Box Impostor"));
	for (Node3D *source : sources) {
		Ref<ArrayMesh> mesh;
		if (p_octahedral) {
			Ref<OctahedralImpostorBaker> baker; // Mariner G5e.
			baker.instantiate();
			mesh = baker->bake(source);
		} else {
			Ref<BoxImpostorBaker> baker;
			baker.instantiate();
			mesh = baker->bake(source);
		}
		if (mesh.is_null()) {
			continue;
		}
		const String path = dir.path_join(String(source->get_name()).validate_filename() + (p_octahedral ? "_octahedral_impostor.res" : "_impostor.res"));
		ResourceSaver::save(mesh, path, ResourceSaver::FLAG_CHANGE_PATH);
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name(String(source->get_name()) + "Impostor");
		mi->set_mesh(mesh);
		mi->set_transform(source->get_transform());
		Node *parent = source->get_parent();
		ur->add_do_method(parent, "add_child", mi, true);
		ur->add_do_method(mi, "set_owner", scene_root);
		ur->add_do_reference(mi);
		ur->add_undo_method(parent, "remove_child", mi);
	}
	ur->commit_action();
}

BoxImpostorEditorPlugin::BoxImpostorEditorPlugin() {
	add_tool_menu_item(TTR("Bake Box Impostor"), callable_mp(this, &BoxImpostorEditorPlugin::_bake_selected).bind(false));
	add_tool_menu_item(TTR("Bake Octahedral Impostor"), callable_mp(this, &BoxImpostorEditorPlugin::_bake_selected).bind(true));
}

BoxImpostorEditorPlugin::~BoxImpostorEditorPlugin() {
	remove_tool_menu_item(TTR("Bake Box Impostor"));
	remove_tool_menu_item(TTR("Bake Octahedral Impostor"));
}
