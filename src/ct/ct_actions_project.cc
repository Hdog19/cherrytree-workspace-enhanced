/*
 * ct_actions_project.cc
 *
 * Workspace actions: mount real folders and add note shortcuts so that
 * notes and related files can be managed together.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <glibmm/i18n.h>
#include "ct_actions.h"
#include "ct_project_panel.h"
#include "ct_dialogs.h"
#include "ct_filesystem.h"

void CtActions::project_bind_folder()
{
    const std::string folder_path = CtDialogs::folder_select_dialog(_pCtMainWin, _pCtConfig->pickDirFile);
    if (folder_path.empty()) {
        return;
    }
    _pCtConfig->pickDirFile = folder_path;
    _pCtMainWin->get_project_panel()->add_mount(folder_path);
}

void CtActions::project_add_note_shortcut()
{
    CtTreeIter ct_tree_iter = _pCtMainWin->curr_tree_iter();
    if (not ct_tree_iter) {
        CtDialogs::warning_dialog("尚未选中任何笔记", *_pCtMainWin);
        return;
    }
    _pCtMainWin->get_project_panel()->add_note_shortcut(ct_tree_iter.get_node_id());
}

void CtActions::project_unbind_folder()
{
    CtProjectPanel* pPanel = _pCtMainWin->get_project_panel();
    if (pPanel->get_mounts().empty() and pPanel->get_note_shortcuts().empty()) {
        return;
    }
    if (not CtDialogs::question_dialog("确定清空整个工作空间吗？", *_pCtMainWin)) {
        return;
    }
    pPanel->clear_workspace();
}

void CtActions::project_open_in_file_manager()
{
    CtProjectPanel* pPanel = _pCtMainWin->get_project_panel();
    if (pPanel->get_mounts().empty()) {
        CtDialogs::warning_dialog("尚无挂载文件夹，请先使用\"项目 → 添加挂载文件夹\"", *_pCtMainWin);
        return;
    }
    fs::open_folderpath(fs::path{pPanel->get_mounts().front()}, _pCtConfig);
}
