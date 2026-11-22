/*
 * ct_project_panel.cc
 *
 * Unified workspace panel implementation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ct_project_panel.h"
#include "ct_main_win.h"
#include "ct_actions.h"
#include "ct_treestore.h"
#include "ct_dialogs.h"
#include "ct_filesystem.h"
#include "ct_workspace_browser.h"
#include <glibmm/i18n.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <set>

CtProjectPanel::CtProjectPanel(CtMainWin* pCtMainWin)
 : Gtk::Box{Gtk::ORIENTATION_VERTICAL}
 , _pCtMainWin{pCtMainWin}
{
    set_spacing(2);

    // header row with quick action buttons (explorer-like tree maintenance)
    Gtk::Box* pHeaderBox = Gtk::manage(new Gtk::Box{Gtk::ORIENTATION_HORIZONTAL, 2});
    _headerLabel.set_text("工作空间");
    _headerLabel.set_xalign(0.02f);
    _headerLabel.get_style_context()->add_class("ct-project-panel-header");
    pHeaderBox->pack_start(_headerLabel, Gtk::PACK_EXPAND_WIDGET);
    const auto addQuickBtn = [pHeaderBox](const char* iconName, const Glib::ustring& tooltip,
                                          std::function<void()> slot) {
        Gtk::Button* pBtn = Gtk::manage(new Gtk::Button{});
        pBtn->set_image_from_icon_name(iconName, Gtk::ICON_SIZE_MENU);
        pBtn->set_tooltip_text(tooltip);
        pBtn->set_relief(Gtk::RELIEF_NONE);
        pBtn->signal_clicked().connect(slot);
        pHeaderBox->pack_start(*pBtn, Gtk::PACK_SHRINK);
    };
    addQuickBtn("folder-new", "新建同级文件夹", sigc::mem_fun(*this, &CtProjectPanel::_new_sibling_folder));
    addQuickBtn("go-previous", "左移（升级）：移出当前文件夹，减少缩进", sigc::mem_fun(*this, &CtProjectPanel::_move_entry_up));
    addQuickBtn("go-next", "右移（降级）：移入上一个兄弟文件夹，增加缩进", sigc::mem_fun(*this, &CtProjectPanel::_move_entry_into_prev_folder));
    addQuickBtn("go-up", "上移：移到根文件夹的第一层", sigc::mem_fun(*this, &CtProjectPanel::_move_entry_to_root));
    addQuickBtn("go-down", "下移（下一级）：移入下方相邻的文件夹节点", sigc::mem_fun(*this, &CtProjectPanel::_move_entry_down));
    pack_start(*pHeaderBox, Gtk::PACK_SHRINK);

    // filename search box (Enter to search across all mounted folders)
    _searchEntry.set_placeholder_text("搜索文件名，回车执行...");
    pack_start(_searchEntry, Gtk::PACK_SHRINK);
    _searchEntry.signal_activate().connect(sigc::mem_fun(*this, &CtProjectPanel::_on_search_entry_activate));
    _searchEntry.signal_icon_press().connect(
        [this](Gtk::EntryIconPosition, const GdkEventButton*) { _exit_search_mode(); });

    _rTreeStore = Gtk::TreeStore::create(_columns);
    _treeView.set_model(_rTreeStore);
    _treeView.set_headers_visible(false);
    _treeView.set_enable_search(false);
    _treeView.set_level_indentation(4);
    _treeView.set_tooltip_column(-1);
    {
        Gtk::TreeViewColumn* pColumn = Gtk::manage(new Gtk::TreeViewColumn{"name"});
        Gtk::CellRendererPixbuf* pCellIcon = Gtk::manage(new Gtk::CellRendererPixbuf);
        pColumn->pack_start(*pCellIcon, false);
        pColumn->add_attribute(pCellIcon->property_pixbuf(), _columns.rColPixbuf);
        Gtk::CellRendererText* pCellText = Gtk::manage(new Gtk::CellRendererText);
        pColumn->pack_start(*pCellText, true);
        pColumn->add_attribute(pCellText->property_text(), _columns.colName);
        // mouse-over shading: shade both renderers of the hovered row
        const auto hoverFunc = [this](Gtk::CellRenderer* pRenderer, const Gtk::TreeModel::const_iterator& iter) {
            _hover_paint(pRenderer, iter);
        };
        pColumn->set_cell_data_func(*pCellIcon, hoverFunc);
        pColumn->set_cell_data_func(*pCellText, hoverFunc);
        _treeView.append_column(*pColumn);
    }

    _sigcConnections.push_back(_treeView.signal_test_expand_row().connect(
        sigc::mem_fun(*this, &CtProjectPanel::_on_test_expand_row)));
    _sigcConnections.push_back(_treeView.signal_row_activated().connect(
        sigc::mem_fun(*this, &CtProjectPanel::_on_row_activated)));
    _sigcConnections.push_back(_treeView.signal_button_press_event().connect(
        [this](GdkEventButton* pEvent) { return _on_button_press_event(pEvent); }, false));
#if GTKMM_MAJOR_VERSION < 4
    // mouse-over shading: track the row under the cursor and repaint
    _treeView.add_events(Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
    _sigcConnections.push_back(_treeView.signal_motion_notify_event().connect([this](GdkEventMotion* pEvent) {
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn* pColumn{nullptr};
        int cellX{0}, cellY{0};
        const bool has = _treeView.get_path_at_pos((int)pEvent->x, (int)pEvent->y, path, pColumn, cellX, cellY);
        const bool changed = (has != _hoverValid) or (has and path != _hoverPath);
        _hoverValid = has;
        if (has) {
            _hoverPath = path;
        }
        if (changed) {
            _treeView.queue_draw();
        }
        return false;
    }));
    _sigcConnections.push_back(_treeView.signal_leave_notify_event().connect([this](GdkEventCrossing*) {
        if (_hoverValid) {
            _hoverValid = false;
            _treeView.queue_draw();
        }
        return false;
    }));
#endif
    // workspace <-> edit-area link: selecting a filesystem entry browses it in the edit area
    _sigcConnections.push_back(_treeView.signal_cursor_changed().connect([this]() {
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn* pColumn{nullptr};
        _treeView.get_cursor(path, pColumn);
        if (not path) {
            return;
        }
        Gtk::TreeModel::iterator iter = _rTreeStore->get_iter(path);
        if (not iter) {
            return;
        }
        Gtk::TreeModel::Row row = *iter;
        const std::string entryPath = row[_columns.colPath];
        if (KindFs != (int)row[_columns.colKind] or row[_columns.colDummy] or entryPath.empty()) {
            return;
        }
        _pCtMainWin->show_workspace_entry(entryPath, (bool)row[_columns.colIsDir]);
    }));

    Gtk::ScrolledWindow* pScrolledWindow = Gtk::manage(new Gtk::ScrolledWindow{});
    pScrolledWindow->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
#if GTKMM_MAJOR_VERSION >= 4
    pScrolledWindow->set_child(_treeView);
#else
    pScrolledWindow->add(_treeView);
#endif
    pack_start(*pScrolledWindow, Gtk::PACK_EXPAND_WIDGET);

    hide();
}

CtProjectPanel::~CtProjectPanel()
{
    _pendingRefreshConn.disconnect();
    _stop_file_monitors();
}

void CtProjectPanel::set_workspace(const std::vector<std::string>& mounts, const std::vector<gint64>& noteShortcuts)
{
    if (mounts == _mountFolders and noteShortcuts == _noteShortcutIds) {
        return;
    }
    _mountFolders = mounts;
    _noteShortcutIds = noteShortcuts;
    _refresh_ui();
}

// rebuild the visible tree from _mountFolders/_noteShortcutIds (must be called after mutating them)
void CtProjectPanel::_refresh_ui()
{
    if (_is_empty_workspace()) {
        _pendingRefreshConn.disconnect();
        _stop_file_monitors();
        _rTreeStore->clear();
        hide();
        return;
    }
    show_all_children();
    if (_pCtMainWin->get_ct_config()->rightSidebarVisible) {
        show(); // the panel may have been hidden while the workspace was empty
    }
    if (_searchMode) {
        _run_search(_searchQuery); // workspace changed: re-run the active search
    }
    else {
        _populate_root();
    }
    _start_file_monitors();
}

void CtProjectPanel::clear_workspace()
{
    set_workspace({}, {});
    _persist();
}

void CtProjectPanel::attach_to_doc(const std::string& docFilePath)
{
    if (docFilePath == _docFilePath) {
        return; // already showing this document's workspace
    }
    _docFilePath = docFilePath;
    if (docFilePath.empty()) {
        // no document opened: start with an empty workspace
        set_workspace({}, {});
        return;
    }
    Glib::KeyFile kf;
    try {
        if (kf.load_from_file(_sidecar_path())) {
            std::vector<std::string> mounts;
            for (const Glib::ustring& s : kf.get_string_list("Workspace", "mounts")) {
                mounts.push_back(s.raw());
            }
            std::vector<gint64> shortcuts;
            for (const Glib::ustring& s : kf.get_string_list("Workspace", "noteShortcuts")) {
                shortcuts.push_back(std::stoll(s.raw()));
            }
            set_workspace(mounts, shortcuts);
            return;
        }
    }
    catch (const Glib::Error&) {
        // unreadable sidecar falls through
    }
    catch (const std::exception&) {
        // malformed numbers etc. fall through
    }
    // no sidecar: this document has no workspace yet (keep session state, e.g. built before the doc was saved)
    if (not _mountFolders.empty() or not _noteShortcutIds.empty()) {
        _persist();
    }
}

std::string CtProjectPanel::_sidecar_path() const
{
    return _docFilePath + ".ctws";
}

bool CtProjectPanel::_is_empty_workspace() const
{
    return _mountFolders.empty() and _noteShortcutIds.empty();
}

void CtProjectPanel::add_note_shortcut(gint64 nodeId)
{
    if (0 == nodeId) {
        return;
    }
    if (std::find(_noteShortcutIds.begin(), _noteShortcutIds.end(), nodeId) != _noteShortcutIds.end()) {
        return; // already there
    }
    _noteShortcutIds.push_back(nodeId);
    _persist();
    _refresh_ui();
}

void CtProjectPanel::remove_note_shortcut(gint64 nodeId)
{
    auto it = std::find(_noteShortcutIds.begin(), _noteShortcutIds.end(), nodeId);
    if (it != _noteShortcutIds.end()) {
        _noteShortcutIds.erase(it);
        _persist();
        _refresh_ui();
    }
}

bool CtProjectPanel::add_mount(const std::string& folderPath)
{
    if (folderPath.empty() or not Glib::file_test(folderPath, Glib::FILE_TEST_IS_DIR)) {
        return false;
    }
    if (std::find(_mountFolders.begin(), _mountFolders.end(), folderPath) != _mountFolders.end()) {
        return false; // already mounted
    }
    _mountFolders.push_back(folderPath);
    _persist();
    _refresh_ui();
    return true;
}

void CtProjectPanel::remove_mount(const std::string& folderPath)
{
    auto it = std::find(_mountFolders.begin(), _mountFolders.end(), folderPath);
    if (it != _mountFolders.end()) {
        _mountFolders.erase(it);
        _persist();
        _refresh_ui();
    }
}

void CtProjectPanel::_persist()
{
    if (_docFilePath.empty()) {
        return; // no document: workspace is session-only
    }
    Glib::KeyFile kf;
    std::vector<Glib::ustring> mountStrs;
    for (const std::string& folder : _mountFolders) {
        mountStrs.push_back(folder);
    }
    std::vector<Glib::ustring> shortcutStrs;
    for (gint64 nodeId : _noteShortcutIds) {
        shortcutStrs.push_back(std::to_string(nodeId));
    }
    kf.set_string_list("Workspace", "mounts", mountStrs);
    kf.set_string_list("Workspace", "noteShortcuts", shortcutStrs);
    try {
        kf.save_to_file(_sidecar_path());
    }
    catch (const Glib::Error& e) {
        spdlog::warn("failed to save workspace sidecar {}: {}", _sidecar_path(), e.what().raw());
    }
}

void CtProjectPanel::refresh()
{
    if (_is_empty_workspace()) {
        return;
    }
    _populate_root();
}

void CtProjectPanel::_populate_root()
{
    if (_searchMode) {
        // flat list of search results
        _rTreeStore->clear();
        for (const auto& result : _searchResults) {
            const std::string& fullPath = result.first;
            Gtk::TreeModel::Row row = *_rTreeStore->append();
            row[_columns.rColPixbuf] = _get_entry_icon(fullPath, result.second);
            row[_columns.colName] = Glib::path_get_basename(fullPath);
            row[_columns.colPath] = fullPath;
            row[_columns.colIsDir] = result.second;
            row[_columns.colKind] = KindFs;
            row[_columns.colNodeId] = 0;
            row[_columns.colDummy] = false;
        }
        return;
    }

    // remember expanded folders by filesystem path (stable across reorders and moves;
    // tree row numbers shift when entries are added/removed, which used to collapse everything)
    std::set<std::string> expandedPaths;
    std::function<void(const Gtk::TreeModel::Children&)> collectExpanded =
        [&](const Gtk::TreeModel::Children& children) {
            for (auto& row : children) {
                if ((bool)row[_columns.colDummy]) {
                    continue;
                }
                if (_treeView.row_expanded(_rTreeStore->get_path(row))) {
                    expandedPaths.insert((std::string)row[_columns.colPath]);
                    collectExpanded(row.children());
                }
            }
        };
    collectExpanded(_rTreeStore->children());

    _rTreeStore->clear();

    // mounted folders as top level rows
    for (const std::string& mountPath : _mountFolders) {
        const Glib::ustring mountName = Glib::path_get_basename(mountPath);
        Gtk::TreeModel::Row row = *_rTreeStore->append();
        row[_columns.rColPixbuf] = _get_entry_icon(mountPath, true);
        row[_columns.colName] = mountName;
        row[_columns.colPath] = mountPath;
        row[_columns.colIsDir] = true;
        row[_columns.colKind] = KindFs;
        row[_columns.colNodeId] = 0;
        row[_columns.colDummy] = false;
        // placeholder child so that the expander arrow is shown for lazy population
        Gtk::TreeModel::Row dummyRow = *_rTreeStore->append(row.children());
        dummyRow[_columns.colName] = "";
        dummyRow[_columns.colPath] = "";
        dummyRow[_columns.colIsDir] = false;
        dummyRow[_columns.colDummy] = true;
        dummyRow[_columns.colKind] = -1;
        dummyRow[_columns.colNodeId] = 0;
    }

    // shortcuts to existing note nodes
    for (const gint64 nodeId : _noteShortcutIds) {
        CtTreeIter ctIter = _pCtMainWin->get_tree_store().get_node_from_node_id(nodeId);
        Gtk::TreeModel::Row row = *_rTreeStore->append();
        row[_columns.colName] = ctIter ? ctIter.get_node_name()
                                       : Glib::ustring::compose("（笔记缺失 %1）", nodeId);
        row[_columns.colPath] = "";
        row[_columns.colIsDir] = false;
        row[_columns.colKind] = KindNoteShortcut;
        row[_columns.colNodeId] = nodeId;
        row[_columns.colDummy] = false;
        if (ctIter) {
            try {
                const int iconDepth = (int)_pCtMainWin->get_tree_store().get_store()->iter_depth(Gtk::TreeModel::iterator{ctIter});
                const char* iconStock = _pCtMainWin->get_tree_store().get_node_icon(
                    iconDepth, ctIter.get_node_syntax_highlighting(), ctIter.get_node_custom_icon_id());
                row[_columns.rColPixbuf] = Gtk::IconTheme::get_default()->load_icon(iconStock, 16);
            }
            catch (...) {}
        }
    }

    // restore expansion (deeper levels lazy-fill synchronously while expanding)
    _restore_expanded_rows(_rTreeStore->children(), expandedPaths);
}

bool CtProjectPanel::_is_row_dummy(const Gtk::TreeModel::iterator& iter)
{
    if (not iter) {
        return false;
    }
    return (*iter)[_columns.colDummy];
}

void CtProjectPanel::_populate_dir(const std::string& dirPath, const Gtk::TreeModel::iterator& parentIter)
{
    // filesystem paths of the expanded folders inside this subtree (may stay empty)
    std::set<std::string> expandedPaths;
    // clear placeholder children
    if (parentIter) {
        // remember the expanded folders inside this subtree (by filesystem path),
        // so that rescanning this dir does not collapse them
        std::function<void(const Gtk::TreeModel::Children&)> collectExpanded =
            [&](const Gtk::TreeModel::Children& children) {
                for (auto& row : children) {
                    if (_treeView.row_expanded(_rTreeStore->get_path(row))) {
                        expandedPaths.insert((std::string)row[_columns.colPath]);
                        collectExpanded(row.children());
                    }
                }
            };
        collectExpanded(parentIter->children());
        auto children = parentIter->children();
        while (children.size() > 0) {
            _rTreeStore->erase(children.begin());
        }
    }

    std::vector<std::pair<std::string, bool>> entries; // <name, isDir>
    try {
        Glib::Dir dir{dirPath};
        for (const std::string& name : dir) {
            if (not name.empty() and name[0] == '.') {
                continue; // skip hidden entries
            }
            const std::string fullPath = Glib::build_filename(dirPath, name);
            entries.emplace_back(name, Glib::file_test(fullPath, Glib::FILE_TEST_IS_DIR));
        }
    }
    catch (const Glib::Error& ex) {
        spdlog::warn("CtProjectPanel: failed to read dir {}, {}", dirPath, ex.what().c_str());
        return;
    }

    // directories first, then alphabetical (case insensitive)
    std::sort(entries.begin(), entries.end(),
        [](const std::pair<std::string, bool>& a, const std::pair<std::string, bool>& b) {
            if (a.second != b.second) {
                return a.second; // dirs first
            }
            const Glib::ustring ua = Glib::ustring(a.first).lowercase();
            const Glib::ustring ub = Glib::ustring(b.first).lowercase();
            return ua < ub;
        });

    for (const auto& entry : entries) {
        const std::string fullPath = Glib::build_filename(dirPath, entry.first);
        Gtk::TreeModel::Row row = parentIter ? *_rTreeStore->append(parentIter->children())
                                             : *_rTreeStore->append();
        row[_columns.rColPixbuf] = _get_entry_icon(fullPath, entry.second);
        row[_columns.colName] = entry.first;
        row[_columns.colPath] = fullPath;
        row[_columns.colIsDir] = entry.second;
        row[_columns.colKind] = KindFs;
        row[_columns.colNodeId] = 0;
        row[_columns.colDummy] = false;
        if (entry.second) {
            // placeholder child so that the expander arrow is shown for lazy population
            Gtk::TreeModel::Row dummyRow = *_rTreeStore->append(row.children());
            dummyRow[_columns.colName] = "";
            dummyRow[_columns.colPath] = "";
            dummyRow[_columns.colIsDir] = false;
            dummyRow[_columns.colDummy] = true;
            dummyRow[_columns.colKind] = -1;
            dummyRow[_columns.colNodeId] = 0;
        }
    }

    if (parentIter and not expandedPaths.empty()) {
        _restore_expanded_rows(parentIter->children(), expandedPaths);
    }
}

// recursive helper: expand every row whose filesystem path was expanded before a rescan;
// each expand_row lazily fills its level synchronously, so deeper levels can follow
void CtProjectPanel::_restore_expanded_rows(const Gtk::TreeModel::Children& children,
                                            const std::set<std::string>& expandedPaths)
{
    for (auto& row : children) {
        const std::string rowFsPath = row[_columns.colPath];
        if ((bool)row[_columns.colIsDir] and not (bool)row[_columns.colDummy]
            and not rowFsPath.empty() and expandedPaths.count(rowFsPath))
        {
            _treeView.expand_row(_rTreeStore->get_path(row), false);
            _restore_expanded_rows(row.children(), expandedPaths);
        }
    }
}

Gtk::TreeModel::iterator CtProjectPanel::_find_row_by_fs_path(const std::string& fsPath)
{
    std::function<Gtk::TreeModel::iterator(const Gtk::TreeModel::Children&)> f_find =
        [&](const Gtk::TreeModel::Children& children) -> Gtk::TreeModel::iterator {
            for (auto& row : children) {
                if ((bool)row[_columns.colDummy]) {
                    continue;
                }
                const std::string rowPath = row[_columns.colPath];
                if (rowPath == fsPath) {
                    return Gtk::TreeModel::iterator{row};
                }
                if ((bool)row[_columns.colIsDir]) {
                    Gtk::TreeModel::iterator found = f_find(row.children());
                    if (found) {
                        return found;
                    }
                }
            }
            return {};
        };
    return f_find(_rTreeStore->children());
}

Gtk::TreeModel::iterator CtProjectPanel::_root_row_of(Gtk::TreeModel::iterator iter)
{
    while (iter and iter->parent()) {
        iter = iter->parent();
    }
    return iter;
}

void CtProjectPanel::_hover_paint(Gtk::CellRenderer* pRenderer, const Gtk::TreeModel::const_iterator& iter)
{
    bool highlight = false;
    if (_hoverValid and iter) {
        const Gtk::TreeModel::Path path = _rTreeStore->get_path(iter);
        highlight = (path == _hoverPath) and not _treeView.get_selection()->is_selected(path);
    }
    if (highlight) {
        // explorer-like mouse-over shading (a soft translucent shadow)
        static const Gdk::RGBA hoverColor = Gdk::RGBA{"rgba(0,0,0,0.08)"};
        pRenderer->property_cell_background_set() = true;
        pRenderer->property_cell_background_rgba() = hoverColor;
    }
    else {
        pRenderer->property_cell_background_set() = false;
    }
}

bool CtProjectPanel::_on_test_expand_row(const Gtk::TreeModel::iterator& iter, const Gtk::TreeModel::Path& /*path*/)
{
    if (not iter) {
        return false;
    }
    Gtk::TreeModel::Row row = *iter;
    if (row[_columns.colIsDir] and KindFs == row[_columns.colKind] and _is_row_dummy(row.children().begin())) {
        _populate_dir(row[_columns.colPath], iter);
    }
    return false; // allow the expansion
}

void CtProjectPanel::_on_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* /*column*/)
{
    Gtk::TreeModel::iterator iter = _rTreeStore->get_iter(path);
    if (not iter) {
        return;
    }
    Gtk::TreeModel::Row row = *iter;
    const int kind = row[_columns.colKind];
    if (KindNoteShortcut == kind) {
        // jump to the note node
        CtTreeIter ctIter = _pCtMainWin->get_tree_store().get_node_from_node_id(row[_columns.colNodeId]);
        if (ctIter) {
            _pCtMainWin->get_tree_view().set_cursor_safe(ctIter);
        }
        return;
    }
    const std::string entryPath = row[_columns.colPath];
    if (entryPath.empty()) {
        return;
    }
    if (row[_columns.colIsDir]) {
        // double click on a folder just toggles expand/collapse, like a tree node
        if (_treeView.row_expanded(path)) {
            _treeView.collapse_row(path);
        }
        else {
            _treeView.expand_row(path, false); // lazy-loads children via _on_test_expand_row
        }
        return;
    }
    fs::open_filepath(fs::path{entryPath}, false, _pCtMainWin->get_ct_config());
}

bool CtProjectPanel::_on_button_press_event(GdkEventButton* pEvent)
{
    if (pEvent->type == GDK_BUTTON_PRESS and 3 == pEvent->button) {
        Gtk::TreeModel::Path path;
        Gtk::TreeViewColumn* pColumn{nullptr};
        int cellX{0}, cellY{0};
        if (_treeView.get_path_at_pos((int)pEvent->x, (int)pEvent->y, path, pColumn, cellX, cellY)) {
            _treeView.set_cursor(path);
            _show_context_menu(pEvent, path);
        }
        else {
            // empty area: workspace-level actions
            _show_context_menu(pEvent, Gtk::TreeModel::Path{});
        }
        return true;
    }
    return false;
}

void CtProjectPanel::_show_context_menu(GdkEventButton* pEvent, const Gtk::TreeModel::Path& path)
{
    Gtk::Menu* pMenu = Gtk::manage(new Gtk::Menu{});
    auto addMenuItem = [pMenu](const char* label, std::function<void()> slot, bool sensitive = true) {
        Gtk::MenuItem* pMenuItem = Gtk::manage(new Gtk::MenuItem{label, true});
        pMenuItem->signal_activate().connect(slot);
        pMenuItem->set_sensitive(sensitive);
        pMenu->append(*pMenuItem);
    };
    auto addSeparator = [pMenu]() {
        pMenu->append(*Gtk::manage(new Gtk::SeparatorMenuItem{}));
    };
    CtWorkspaceBrowser* pBrowser = _pCtMainWin->get_workspace_browser();

    if (path.empty()) {
        // blank area of the panel: same explorer actions, targeting the folder shown in the edit area
        const std::string currDir = pBrowser ? pBrowser->current_dir() : std::string{};
        addMenuItem("添加挂载文件夹...", sigc::mem_fun(*this, &CtProjectPanel::_add_mount_interactive));
        addMenuItem("添加当前笔记快捷入口", sigc::mem_fun(*this, &CtProjectPanel::_add_note_shortcut_interactive));
        addSeparator();
        const bool canPaste = pBrowser and not currDir.empty();
        addMenuItem(pBrowser and pBrowser->is_file_clipboard_cut() ? "粘贴（剪切）到当前文件夹" : "粘贴（复制）到当前文件夹",
                    [pBrowser, currDir]() { if (pBrowser) pBrowser->paste_into(currDir); }, canPaste);
        addMenuItem("在 PowerShell 中打开当前文件夹",
                    [pBrowser, currDir]() { if (pBrowser) pBrowser->open_in_powershell(currDir); }, not currDir.empty());
        addMenuItem("刷新", [pBrowser]() { if (pBrowser) pBrowser->refresh(); }, pBrowser);
        pMenu->show_all();
#if GTKMM_MAJOR_VERSION >= 4
        pMenu->popup();
#else
        pMenu->popup_at_pointer(reinterpret_cast<const GdkEvent*>(pEvent));
#endif
        return;
    }

    Gtk::TreeModel::iterator iter = _rTreeStore->get_iter(path);
    if (not iter or _is_row_dummy(iter)) {
        return;
    }
    Gtk::TreeModel::Row row = *iter;
    const int kind = row[_columns.colKind];
    const bool isDir = row[_columns.colIsDir];
    const bool isMountRoot = (KindFs == kind) and not _searchMode and not iter->parent();

    if (KindNoteShortcut == kind) {
        addMenuItem("跳转到笔记", sigc::mem_fun(*this, &CtProjectPanel::_on_menu_jump_note));
        addMenuItem("移除快捷入口", sigc::mem_fun(*this, &CtProjectPanel::_on_menu_remove_shortcut));
    }
    else {
        const std::string entryPath = row[_columns.colPath];
        const std::string targetDir = isDir ? entryPath : Glib::path_get_dirname(entryPath);
        const std::string parentDir = Glib::path_get_dirname(entryPath);
        const bool isFs = (KindFs == kind) and not entryPath.empty();
        auto refreshPanel = [this](const std::string& dirPath) { _refresh_parent_of(dirPath); };

        // same menu as the edit-area browser (plus panel-specific entries)
        addMenuItem("打开（系统默认程序）", sigc::mem_fun(*this, &CtProjectPanel::_on_menu_open));
        addMenuItem("在文件管理器中打开", sigc::mem_fun(*this, &CtProjectPanel::_on_menu_open_in_file_manager));
        addMenuItem("在 PowerShell 中打开", [pBrowser, targetDir]() {
            if (pBrowser) pBrowser->open_in_powershell(targetDir);
        }, pBrowser);
        addMenuItem("复制路径", sigc::mem_fun(*this, &CtProjectPanel::_on_menu_copy_path));
        addMenuItem("插入链接到当前笔记", sigc::mem_fun(*this, &CtProjectPanel::_on_menu_insert_link_into_note));
        addSeparator();
        addMenuItem("新建文件夹", [this, pBrowser, targetDir, refreshPanel]() {
            if (pBrowser) pBrowser->new_folder_in(targetDir);
            refreshPanel(targetDir);
        }, pBrowser and isFs);
        addMenuItem("新建文本文件", [this, pBrowser, targetDir, refreshPanel]() {
            if (pBrowser) pBrowser->new_file_in(targetDir);
            refreshPanel(targetDir);
        }, pBrowser and isFs);
        addMenuItem("添加文件到此处...", sigc::mem_fun(*this, &CtProjectPanel::_on_menu_add_file), isDir);
        addSeparator();
        addMenuItem("剪切", [pBrowser, entryPath]() {
            if (pBrowser) pBrowser->set_file_clipboard({entryPath}, true);
        }, pBrowser and isFs);
        addMenuItem("复制", [pBrowser, entryPath]() {
            if (pBrowser) pBrowser->set_file_clipboard({entryPath}, false);
        }, pBrowser and isFs);
        addMenuItem(pBrowser and pBrowser->is_file_clipboard_cut() ? "粘贴（剪切）" : "粘贴（复制）",
                    [pBrowser, targetDir, refreshPanel]() {
                        if (pBrowser) pBrowser->paste_into(targetDir);
                        refreshPanel(targetDir);
                    }, pBrowser);
        addSeparator();
        addMenuItem("重命名（F2）", [this, pBrowser, entryPath, parentDir, refreshPanel]() {
            if (pBrowser) pBrowser->rename_path(entryPath);
            refreshPanel(parentDir);
        }, pBrowser and isFs);
        addMenuItem("删除（Del）", [this, pBrowser, entryPath, parentDir, refreshPanel]() {
            if (pBrowser) pBrowser->delete_paths({entryPath});
            refreshPanel(parentDir);
        }, pBrowser and isFs and not isMountRoot);
        addSeparator();
        addMenuItem("刷新", [this, pBrowser, entryPath, parentDir, isDir, iter, refreshPanel]() {
            if (pBrowser) pBrowser->refresh();
            if (isDir) {
                _populate_dir(entryPath, iter);
            }
            else {
                refreshPanel(parentDir);
            }
        }, isFs);
        if (isMountRoot) {
            addSeparator();
            addMenuItem("卸载此文件夹", sigc::mem_fun(*this, &CtProjectPanel::_on_menu_unmount));
        }
    }
    pMenu->show_all();
#if GTKMM_MAJOR_VERSION >= 4
    pMenu->popup();
#else
    pMenu->popup_at_pointer(reinterpret_cast<const GdkEvent*>(pEvent));
#endif
}

void CtProjectPanel::_add_mount_interactive()
{
    const std::string folderPath = CtDialogs::folder_select_dialog(_pCtMainWin, _pCtMainWin->get_ct_config()->pickDirFile);
    if (folderPath.empty()) {
        return;
    }
    _pCtMainWin->get_ct_config()->pickDirFile = folderPath;
    add_mount(folderPath);
}

void CtProjectPanel::_add_note_shortcut_interactive()
{
    CtTreeIter ctIter = _pCtMainWin->curr_tree_iter();
    if (not ctIter) {
        CtDialogs::warning_dialog("尚未选中任何笔记", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    add_note_shortcut(ctIter.get_node_id());
}

void CtProjectPanel::_on_menu_open()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter) {
        return;
    }
    // the menu's "打开" keeps the explorer semantics: open externally
    Gtk::TreeModel::Row row = *iter;
    if (KindNoteShortcut == row[_columns.colKind]) {
        CtTreeIter ctIter = _pCtMainWin->get_tree_store().get_node_from_node_id(row[_columns.colNodeId]);
        if (ctIter) {
            _pCtMainWin->get_tree_view().set_cursor_safe(ctIter);
        }
        return;
    }
    const std::string entryPath = row[_columns.colPath];
    if (entryPath.empty()) {
        return;
    }
    if (row[_columns.colIsDir]) {
        fs::open_folderpath(fs::path{entryPath}, _pCtMainWin->get_ct_config());
    }
    else {
        fs::open_filepath(fs::path{entryPath}, false, _pCtMainWin->get_ct_config());
    }
}

void CtProjectPanel::_on_menu_open_in_file_manager()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter) {
        return;
    }
    Gtk::TreeModel::Row row = *iter;
    fs::open_folderpath(fs::path{row[_columns.colPath]}, _pCtMainWin->get_ct_config());
}

void CtProjectPanel::_on_menu_add_file()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or not (*iter)[_columns.colIsDir] or KindNoteShortcut == (*iter)[_columns.colKind]) {
        return;
    }
    const std::string dirPath = (*iter)[_columns.colPath];
    CtDialogs::CtFileSelectArgs args;
    args.curr_folder = fs::path{dirPath};
    const std::string filepath = CtDialogs::file_select_dialog(static_cast<Gtk::Window*>(_pCtMainWin), args);
    if (filepath.empty()) {
        return;
    }
    const std::string destPath = Glib::build_filename(dirPath, Glib::path_get_basename(filepath));
    if (Glib::file_test(destPath, Glib::FILE_TEST_EXISTS)) {
        if (not CtDialogs::question_dialog(Glib::ustring::compose("已存在同名文件，覆盖 {} 吗？", Glib::path_get_basename(filepath)),
                                           *static_cast<Gtk::Window*>(_pCtMainWin)))
        {
            return;
        }
    }
    if (not fs::copy_file(fs::path{filepath}, fs::path{destPath})) {
        spdlog::error("CtProjectPanel: failed to copy {} to {}", filepath, destPath);
        return;
    }
    _populate_dir(dirPath, iter);
}

// ---- quick action bar ------------------------------------------------------

Gtk::TreeModel::iterator CtProjectPanel::_selected_fs_entry()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter) {
        return {};
    }
    Gtk::TreeModel::Row row = *iter;
    if (KindFs != (int)row[_columns.colKind] or (bool)row[_columns.colDummy]
        or ((std::string)row[_columns.colPath]).empty())
    {
        return {};
    }
    return iter;
}

bool CtProjectPanel::_create_folder_in(const std::string& dirPath)
{
    Gtk::Dialog dialog{"新建文件夹", *static_cast<Gtk::Window*>(_pCtMainWin), true};
    dialog.add_button("取消", Gtk::RESPONSE_CANCEL);
    dialog.add_button("确定", Gtk::RESPONSE_OK);
    dialog.set_default_response(Gtk::RESPONSE_OK);
    Gtk::Entry entry;
    entry.set_activates_default(true);
    Gtk::Box* pBox = dialog.get_content_area();
    pBox->pack_start(entry, Gtk::PACK_SHRINK);
    pBox->show_all();
    if (Gtk::RESPONSE_OK != dialog.run() or entry.get_text().empty()) {
        return false;
    }
    const std::string newDirPath = Glib::build_filename(dirPath, entry.get_text());
    if (Glib::file_test(newDirPath, Glib::FILE_TEST_EXISTS)) {
        CtDialogs::warning_dialog("同名文件夹已存在", *static_cast<Gtk::Window*>(_pCtMainWin));
        return false;
    }
    if (g_mkdir_with_parents(newDirPath.c_str(), 0755) == 0) {
        _refresh_parent_of(dirPath);
        return true;
    }
    return false;
}

void CtProjectPanel::_move_entry_to(const fs::path& src, const fs::path& dstDir)
{
    const fs::path dst = dstDir / src.filename();
    if (Glib::file_test(dst.string(), Glib::FILE_TEST_EXISTS)) {
        CtDialogs::warning_dialog("目标位置已存在同名条目：\n" + dst.string(), *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    // remember which parts of the moved subtree were expanded (by filesystem path),
    // so they stay expanded at the new location after the refreshes below
    std::set<std::string> movedExpanded;
    Gtk::TreeModel::iterator srcIter = _find_row_by_fs_path(src.string());
    if (srcIter) {
        std::function<void(Gtk::TreeModel::iterator)> collectExpanded =
            [&](Gtk::TreeModel::iterator it) {
                if (_treeView.row_expanded(_rTreeStore->get_path(it))) {
                    movedExpanded.insert((*it)[_columns.colPath]);
                }
                for (auto& childRow : it->children()) {
                    collectExpanded(Gtk::TreeModel::iterator{childRow});
                }
            };
        collectExpanded(srcIter);
    }
    if (not fs::move_file(src, dst)) {
        CtDialogs::warning_dialog("移动失败：" + src.string(), *static_cast<Gtk::Window*>(_pCtMainWin));
        spdlog::warn("workspace move failed: {} -> {}", src.string(), dst.string());
        return;
    }
    spdlog::debug("workspace moved {} -> {}", src.string(), dst.string());
    // rescan both the source dir row and the target dir row (if present in the tree)
    _refresh_parent_of(Glib::path_get_dirname(src.string()));
    _refresh_parent_of(dstDir.string());
    // restore the moved subtree's expansion at its new location (remap src -> dst prefixes)
    if (not movedExpanded.empty()) {
        const std::string srcStr = src.string();
        const std::string dstStr = dst.string();
        std::set<std::string> mappedPaths;
        for (const std::string& oldPath : movedExpanded) {
            if (oldPath == srcStr) {
                mappedPaths.insert(dstStr);
            }
            else if (oldPath.size() > srcStr.size()
                     and oldPath.compare(0, srcStr.size(), srcStr) == 0
                     and (oldPath[srcStr.size()] == '\\' or oldPath[srcStr.size()] == '/'))
            {
                mappedPaths.insert(dstStr + oldPath.substr(srcStr.size()));
            }
        }
        if (Gtk::TreeModel::iterator dstIter = _find_row_by_fs_path(dstStr)) {
            if (mappedPaths.count(dstStr)) {
                _treeView.expand_row(_rTreeStore->get_path(dstIter), false);
            }
            std::function<void(const Gtk::TreeModel::Children&)> restoreSubtree =
                [&](const Gtk::TreeModel::Children& children) {
                    for (auto& row : children) {
                        const std::string rowFsPath = row[_columns.colPath];
                        if ((bool)row[_columns.colIsDir] and not (bool)row[_columns.colDummy]
                            and mappedPaths.count(rowFsPath))
                        {
                            _treeView.expand_row(_rTreeStore->get_path(row), false);
                            restoreSubtree(row.children());
                        }
                    }
                };
            restoreSubtree(dstIter->children());
        }
    }
}

void CtProjectPanel::_new_sibling_folder()
{
    Gtk::TreeModel::iterator iter = _selected_fs_entry();
    if (not iter) {
        CtDialogs::warning_dialog("请先在树中选择一个文件或文件夹", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    // a top-level mount row creates inside the mount; other entries create in their parent (same level)
    const std::string entryPath = (*iter)[_columns.colPath];
    const std::string dirPath = iter->parent() ? Glib::path_get_dirname(entryPath) : entryPath;
    _create_folder_in(dirPath);
}

void CtProjectPanel::_move_entry_up()
{
    Gtk::TreeModel::iterator iter = _selected_fs_entry();
    if (not iter) {
        CtDialogs::warning_dialog("请先在树中选择一个文件或文件夹", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    const std::string entryPath = (*iter)[_columns.colPath];
    Gtk::TreeModel::iterator parentIter = iter->parent();
    if (not parentIter) {
        CtDialogs::warning_dialog("顶层挂载文件夹不能移动", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    if (not parentIter->parent()) {
        // the parent row is a mount root: moving up would leave the workspace root
        CtDialogs::warning_dialog("已在根文件夹的第一层，不能移动到根文件夹之外或与之平级",
                                  *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    // target = the parent dir's own parent row path (always inside the mount root)
    const std::string grandDir = (*parentIter->parent())[_columns.colPath];
    _move_entry_to(fs::path{entryPath}, fs::path{grandDir});
}

void CtProjectPanel::_move_entry_down()
{
    Gtk::TreeModel::iterator iter = _selected_fs_entry();
    if (not iter) {
        CtDialogs::warning_dialog("请先在树中选择一个文件或文件夹", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    if (not iter->parent()) {
        // top-level mount rows are workspace roots themselves: moving one into
        // another root would take it out of its own root folder
        CtDialogs::warning_dialog("顶层挂载文件夹不能移动", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    // target = the first folder row shown below the selected row (its next folder sibling;
    // siblings of a non-root row always stay inside the same mount root)
    Gtk::TreeModel::iterator it = iter;
    std::string targetDir;
    for (++it; it; ++it) {
        Gtk::TreeModel::Row row = *it;
        if (KindFs == (int)row[_columns.colKind] and not (bool)row[_columns.colDummy]
            and (bool)row[_columns.colIsDir] and not ((std::string)row[_columns.colPath]).empty())
        {
            targetDir = row[_columns.colPath];
            break;
        }
    }
    if (targetDir.empty()) {
        CtDialogs::warning_dialog("所选节点下方没有相邻的文件夹节点，无法移动到下一级",
                                  *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    _move_entry_to(fs::path{(*iter)[_columns.colPath]}, fs::path{targetDir});
}

void CtProjectPanel::_move_entry_to_root()
{
    Gtk::TreeModel::iterator iter = _selected_fs_entry();
    if (not iter) {
        CtDialogs::warning_dialog("请先在树中选择一个文件或文件夹", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    if (not iter->parent()) {
        CtDialogs::warning_dialog("顶层挂载文件夹不能移动", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    const std::string rootPath = (*_root_row_of(iter))[_columns.colPath];
    const std::string parentPath = (*iter->parent())[_columns.colPath];
    if (parentPath == rootPath) {
        CtDialogs::warning_dialog("已在根文件夹的第一层", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    _move_entry_to(fs::path{(*iter)[_columns.colPath]}, fs::path{rootPath});
}

void CtProjectPanel::_move_entry_into_prev_folder()
{
    Gtk::TreeModel::iterator iter = _selected_fs_entry();
    if (not iter) {
        CtDialogs::warning_dialog("请先在树中选择一个文件或文件夹", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    if (not iter->parent()) {
        CtDialogs::warning_dialog("顶层挂载文件夹不能移动", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    // cherrytree-like demote: become a child of the previous sibling;
    // the nearest folder sibling above the row (siblings of a non-root row stay in the same root)
    Gtk::TreeModel::iterator it = iter;
    std::string targetDir;
    for (--it; it; --it) {
        Gtk::TreeModel::Row row = *it;
        if (KindFs == (int)row[_columns.colKind] and not (bool)row[_columns.colDummy]
            and (bool)row[_columns.colIsDir] and not ((std::string)row[_columns.colPath]).empty())
        {
            targetDir = row[_columns.colPath];
            break;
        }
    }
    if (targetDir.empty()) {
        CtDialogs::warning_dialog("所选节点上方没有相邻的文件夹节点，无法降级",
                                  *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    _move_entry_to(fs::path{(*iter)[_columns.colPath]}, fs::path{targetDir});
}

void CtProjectPanel::_on_menu_new_folder()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or not (*iter)[_columns.colIsDir] or KindNoteShortcut == (*iter)[_columns.colKind]) {
        return;
    }
    const std::string dirPath = (*iter)[_columns.colPath];
    if (_create_folder_in(dirPath)) {
        _populate_dir(dirPath, iter);
    }
}

void CtProjectPanel::_on_menu_rename()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or _is_row_dummy(iter)) {
        return;
    }
    Gtk::TreeModel::Row row = *iter;
    const std::string oldPath = row[_columns.colPath];

    Gtk::Dialog dialog{"重命名", *static_cast<Gtk::Window*>(_pCtMainWin), true};
    dialog.add_button("取消", Gtk::RESPONSE_CANCEL);
    dialog.add_button("确定", Gtk::RESPONSE_OK);
    dialog.set_default_response(Gtk::RESPONSE_OK);
    Gtk::Entry entry;
    entry.set_text(row[_columns.colName]);
    entry.set_activates_default(true);
    Gtk::Box* pBox = dialog.get_content_area();
    pBox->pack_start(entry, Gtk::PACK_SHRINK);
    pBox->show_all();
    if (Gtk::RESPONSE_OK != dialog.run() or entry.get_text().empty()) {
        return;
    }
    const std::string newPath = Glib::build_filename(Glib::path_get_dirname(oldPath), entry.get_text());
    if (Glib::file_test(newPath, Glib::FILE_TEST_EXISTS)) {
        CtDialogs::warning_dialog("目标名称已存在", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    if (not fs::move_file(fs::path{oldPath}, fs::path{newPath})) {
        spdlog::error("CtProjectPanel: failed to rename {} to {}", oldPath, newPath);
        return;
    }
    _refresh_parent_of(Glib::path_get_dirname(oldPath));
}

void CtProjectPanel::_on_menu_delete()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or _is_row_dummy(iter)) {
        return;
    }
    Gtk::TreeModel::Row row = *iter;
    const std::string delPath = row[_columns.colPath];
    if (not CtDialogs::question_dialog(Glib::ustring::compose("确定删除 {} 吗？", Glib::ustring{row[_columns.colName]}),
                                       *static_cast<Gtk::Window*>(_pCtMainWin)))
    {
        return;
    }
    fs::remove_all(fs::path{delPath});
    _refresh_parent_of(Glib::path_get_dirname(delPath));
}

void CtProjectPanel::_on_menu_refresh_mount()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or KindFs != (*iter)[_columns.colKind]) {
        return;
    }
    _populate_dir((*iter)[_columns.colPath], iter);
}

void CtProjectPanel::_on_menu_unmount()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or KindFs != (*iter)[_columns.colKind]) {
        return;
    }
    remove_mount((*iter)[_columns.colPath]);
}

void CtProjectPanel::_on_menu_jump_note()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or KindNoteShortcut != (*iter)[_columns.colKind]) {
        return;
    }
    CtTreeIter ctIter = _pCtMainWin->get_tree_store().get_node_from_node_id((*iter)[_columns.colNodeId]);
    if (ctIter) {
        _pCtMainWin->get_tree_view().set_cursor_safe(ctIter);
    }
}

void CtProjectPanel::_on_menu_remove_shortcut()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or KindNoteShortcut != (*iter)[_columns.colKind]) {
        return;
    }
    remove_note_shortcut((*iter)[_columns.colNodeId]);
}

void CtProjectPanel::_on_menu_insert_link_into_note()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or KindNoteShortcut == (*iter)[_columns.colKind]) {
        return;
    }
    Gtk::TreeModel::Row row = *iter;
    const std::string entryPath = row[_columns.colPath];
    if (entryPath.empty()) {
        return;
    }
    CtTreeIter ctIter = _pCtMainWin->curr_tree_iter();
    if (not ctIter) {
        CtDialogs::warning_dialog("尚未选中任何笔记", *static_cast<Gtk::Window*>(_pCtMainWin));
        return;
    }
    auto rBuffer = _pCtMainWin->get_text_view().get_buffer();
    const int startOffset = rBuffer->get_insert()->get_iter().get_offset();
    rBuffer->insert(rBuffer->get_insert()->get_iter(), row[_columns.colName]);
    const std::string propertyValue = std::string(row[_columns.colIsDir] ? CtConst::LINK_TYPE_FOLD : CtConst::LINK_TYPE_FILE)
        + " " + Glib::Base64::encode(entryPath);
    rBuffer->apply_tag_by_name(
        _pCtMainWin->get_text_tag_name_exist_or_create(CtConst::TAG_LINK, propertyValue),
        rBuffer->get_iter_at_offset(startOffset), rBuffer->get_insert()->get_iter());
}

void CtProjectPanel::_on_menu_copy_path()
{
    Gtk::TreeModel::iterator iter = _treeView.get_selection()->get_selected();
    if (not iter or KindNoteShortcut == (*iter)[_columns.colKind]) {
        return;
    }
    const std::string entryPath = (*iter)[_columns.colPath];
    if (entryPath.empty()) {
        return;
    }
    Gtk::Clipboard::get(GDK_SELECTION_CLIPBOARD)->set_text(entryPath);
}

void CtProjectPanel::_on_search_entry_activate()
{
    const Glib::ustring query = _searchEntry.get_text();
    if (query.empty()) {
        _exit_search_mode();
        return;
    }
    _run_search(query);
}

void CtProjectPanel::_exit_search_mode()
{
    if (not _searchMode) {
        return;
    }
    _searchMode = false;
    _searchQuery.clear();
    _searchResults.clear();
    if (not _searchEntry.get_text().empty()) {
        _searchEntry.set_text("");
    }
    _populate_root();
}

void CtProjectPanel::_run_search(const Glib::ustring& query)
{
    if (_mountFolders.empty()) {
        return;
    }
    _searchMode = true;
    _searchQuery = query;
    _searchResults.clear();
    const Glib::ustring needleLower = query.lowercase();
    size_t count = 0;
    for (const std::string& mountPath : _mountFolders) {
        _search_recursive(mountPath, needleLower, _searchResults, count);
        if (count >= 500) {
            break; // safety cap
        }
    }
    _populate_root();
}

void CtProjectPanel::_search_recursive(const std::string& dirPath, const Glib::ustring& needleLower,
                                       std::vector<std::pair<std::string, bool>>& results, size_t& count)
{
    if (count >= 500) {
        return;
    }
    std::vector<std::string> names;
    try {
        Glib::Dir dir{dirPath};
        for (const std::string& name : dir) {
            if (not name.empty() and name[0] == '.') {
                continue; // skip hidden entries
            }
            names.push_back(name);
        }
    }
    catch (const Glib::Error&) {
        return;
    }
    for (const std::string& name : names) {
        if (count >= 500) {
            return;
        }
        const std::string fullPath = Glib::build_filename(dirPath, name);
        const bool isDir = Glib::file_test(fullPath, Glib::FILE_TEST_IS_DIR);
        if (Glib::ustring(name).lowercase().find(needleLower) != Glib::ustring::npos) {
            results.emplace_back(fullPath, isDir);
            ++count;
        }
        if (isDir) {
            _search_recursive(fullPath, needleLower, results, count);
        }
    }
}

Glib::RefPtr<Gdk::Pixbuf> CtProjectPanel::_get_entry_icon(const std::string& path, bool isDir)
{
    const int iconSize = 16;
    Glib::RefPtr<Gtk::IconTheme> rIconTheme = Gtk::IconTheme::get_default();
    std::string iconName = isDir ? "folder" : "text-x-generic";
    try {
        auto fileInfo = Gio::File::create_for_path(path)->query_info(G_FILE_ATTRIBUTE_STANDARD_ICON);
        auto icon = fileInfo->get_icon();
        if (auto pThemedIcon = Glib::RefPtr<Gio::ThemedIcon>::cast_dynamic(icon)) {
            for (const Glib::ustring& name : pThemedIcon->get_names()) {
                if (rIconTheme->has_icon(name)) {
                    iconName = name;
                    break;
                }
            }
        }
    }
    catch (...) {}
    try {
        return rIconTheme->load_icon(iconName, iconSize);
    }
    catch (...) {
        return Glib::RefPtr<Gdk::Pixbuf>{};
    }
}

void CtProjectPanel::_refresh_parent_of(const std::string& parentDirPath)
{
    // find the tree row of parentDirPath among already-populated rows and rescan it
    std::function<Gtk::TreeModel::iterator(const Gtk::TreeModel::Children&)> f_find_row =
    [&](const Gtk::TreeModel::Children& children) -> Gtk::TreeModel::iterator {
        for (auto& row : children) {
            const std::string rowPath = row[_columns.colPath];
            const bool isDummy = row[_columns.colDummy];
            if (rowPath == parentDirPath and not isDummy) {
                return Gtk::TreeModel::iterator{row};
            }
            if (row[_columns.colIsDir] and _treeView.row_expanded(_rTreeStore->get_path(row))) {
                Gtk::TreeModel::iterator found = f_find_row(row.children());
                if (found) {
                    return found;
                }
            }
        }
        return Gtk::TreeModel::iterator{};
    };
    Gtk::TreeModel::iterator foundIter = f_find_row(_rTreeStore->children());
    if (foundIter) {
        _populate_dir(parentDirPath, foundIter);
    }
}

void CtProjectPanel::_start_file_monitors()
{
    _stop_file_monitors();
    for (const std::string& mountPath : _mountFolders) {
        try {
            auto file = Gio::File::create_for_path(mountPath);
            auto monitor = file->monitor_directory(Gio::FILE_MONITOR_NONE);
            if (monitor) {
                _fileMonitors.push_back(monitor);
                monitor->signal_changed().connect(
                    [this](const Glib::RefPtr<Gio::File>&, const Glib::RefPtr<Gio::File>&, Gio::FileMonitorEvent) {
                        // debounce: multiple events can be emitted for one operation
                        _pendingRefreshConn.disconnect();
                        _pendingRefreshConn = Glib::signal_timeout().connect(
                            [this]() {
                                _populate_root();
                                return false;
                            }, 300);
                    });
            }
        }
        catch (const Glib::Error& ex) {
            spdlog::warn("CtProjectPanel: failed to monitor dir {}, {}", mountPath, ex.what().c_str());
        }
    }
}

void CtProjectPanel::_stop_file_monitors()
{
    for (auto& monitor : _fileMonitors) {
        monitor->cancel();
    }
    _fileMonitors.clear();
}
