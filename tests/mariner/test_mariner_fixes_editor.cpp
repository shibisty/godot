/**************************************************************************/
/*  test_mariner_fixes_editor.cpp                                         */
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

TEST_FORCE_LINK(test_mariner_fixes_editor)

#ifdef TOOLS_ENABLED

#include "editor/docks/editor_dock.h"

// Mariner engine fixes in the editor:
// 1. EditorDock::current_layout was uninitialized and read by the first EditorDockManager::_move_dock() of every dock
//    (valgrind, editor start): now DOCK_LAYOUT_NONE until docked (as upstream Godot).

namespace TestMarinerFixesEditor {

TEST_CASE("[Mariner][fixes][SceneTree] EditorDock: no layout until docked") {
	EditorDock *dock = memnew(EditorDock);
	CHECK(dock->get_current_layout() == EditorDock::DOCK_LAYOUT_NONE);
	memdelete(dock);
}

} // namespace TestMarinerFixesEditor

#endif // TOOLS_ENABLED
