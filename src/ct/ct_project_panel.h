/*
 * ct_project_panel.h
 *
 * Unified workspace panel: the lower half of the left tree area shows
 * - mounted filesystem folders (real Windows folders/files, lazy populated)
 * - shortcuts to existing note nodes (double click jumps to the note)
 * Notes themselves remain untouched in the note TreeStore/database.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <gtkmm.h>
#include <memory>
#include <string>
#include <vector>

#include "ct_filesystem.h"

class CtMainWin;

class CtProjectPanelColumns : public Gtk::TreeModelColumnRecord
{
public:
    CtProjectPanelColumns() {
        add(rColPixbuf); add(colName); add(colPath); add(colIsDir); add(colDummy);
        add(colKind); add(colNodeId);
    }
    Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> rColPixbuf;
    Gtk::TreeModelColumn<Glib::ustring>             colName;
    Gtk::TreeModelColumn<std::string>               colPath;
    Gtk::TreeModelColumn<bool>                      colIsDir;
    Gtk::TreeModelColumn<bool>                      colDummy; // placeholder row for lazy population
    Gtk::TreeModelColumn<int>                       colKind;  // KindFs or KindNoteShortcut
    Gtk::TreeModelColumn<gint64>                    colNodeId; // for KindNoteShortcut rows
};

class CtProjectPanel : public Gtk::Box
{
public:
    static constexpr int KindFs{0};            // real filesystem entry (mount root or file/folder below it)
    static constexpr int KindNoteShortcut{1};  // shortcut pointing to an existing note node

public:
    CtProjectPanel(CtMainWin* pCtMainWin);
    virtual ~CtProjectPanel();

    // sync panel content from config; rebuilds only if the workspace definition changed
    void          set_workspace(const std::vector<std::string>& mounts, const std::vector<gint64>& noteShortcuts);
    void          clear_workspace();
    // bind the panel to the currently opened document: loads <doc>.ctws sidecar next to it;
    // empty path (no document) => empty workspace
    void          attach_to_doc(const std::string& docFilePath);

    // mutate the workspace (also persists to config)
    bool          add_mount(const std::string& folderPath);     // false if invalid/duplicate
    void          remove_mount(const std::string& folderPath);
    void          add_note_shortcut(gint64 nodeId);
    void          remove_note_shortcut(gint64 nodeId);

    const std::vector<std::string>& get_mounts() const { return _mountFolders; }
    const std::vector<gint64>&      get_note_shortcuts() const { return _noteShortcutIds; }

    // rescan everything from disk
    void          refresh();

protected:
    void          _populate_dir(const std::string& dirPath, const Gtk::TreeModel::iterator& parentIter);
    void          _populate_root();
    void          _refresh_ui(); // rebuild from _mountFolders/_noteShortcutIds and manage visibility
    bool          _is_row_dummy(const Gtk::TreeModel::iterator& iter);
    bool          _is_empty_workspace() const;
    bool          _on_test_expand_row(const Gtk::TreeModel::iterator& iter, const Gtk::TreeModel::Path& path);
    void          _on_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column);
    bool          _on_button_press_event(GdkEventButton* pEvent);
    // mouse-over shading on tree rows
    void          _hover_paint(Gtk::CellRenderer* pRenderer, const Gtk::TreeModel::const_iterator& iter);
    Gtk::TreeModel::Path _hoverPath;
    bool          _hoverValid{false};
    void          _show_context_menu(GdkEventButton* pEvent, const Gtk::TreeModel::Path& path);
    void          _on_menu_open();
    void          _on_menu_open_in_file_manager();
    void          _on_menu_add_file();
    void          _on_menu_new_folder();
    void          _on_menu_rename();
    void          _on_menu_delete();
    // quick action bar: new sibling folder / move entry to the parent dir / into the next folder
    void          _new_sibling_folder();
    void          _move_entry_up();
    void          _move_entry_down();
    bool          _create_folder_in(const std::string& dirPath);
    void          _move_entry_to(const fs::path& src, const fs::path& dstDir);
    Gtk::TreeModel::iterator _selected_fs_entry();
    void          _on_menu_refresh_mount();
    void          _on_menu_unmount();
    void          _on_menu_jump_note();
    void          _on_menu_remove_shortcut();
    void          _on_menu_insert_link_into_note();
    void          _on_menu_copy_path();
    void          _add_mount_interactive();
    void          _add_note_shortcut_interactive();
    void          _on_search_entry_activate();
    void          _run_search(const Glib::ustring& query);
    void          _search_recursive(const std::string& dirPath, const Glib::ustring& needleLower,
                                  std::vector<std::pair<std::string, bool>>& results, size_t& count);
    void          _exit_search_mode();
    Glib::RefPtr<Gdk::Pixbuf> _get_entry_icon(const std::string& path, bool isDir);
    void          _start_file_monitors();
    void          _stop_file_monitors();
    void          _refresh_parent_of(const std::string& parentDirPath);
    void          _persist();
    std::string   _sidecar_path() const;

protected:
    CtMainWin* const _pCtMainWin;
    std::string      _docFilePath; // document the workspace belongs to; empty = no document
    std::vector<std::string> _mountFolders;
    std::vector<gint64>      _noteShortcutIds;
    CtProjectPanelColumns _columns;
    Glib::RefPtr<Gtk::TreeStore> _rTreeStore;
    Gtk::TreeView    _treeView;
    Gtk::Label       _headerLabel;
    Gtk::SearchEntry _searchEntry;
    bool             _searchMode{false};
    Glib::ustring    _searchQuery;
    std::vector<std::pair<std::string, bool>> _searchResults; // <path, isDir>
    std::vector<Glib::RefPtr<Gio::FileMonitor>> _fileMonitors;
    sigc::connection _pendingRefreshConn;
    std::list<sigc::connection> _sigcConnections;
};
