/*
 * ct_workspace_browser.h
 *
 * Edit-area companion of the project panel: when a filesystem entry is selected
 * in the workspace panel, the edit area switches to this widget.
 * - a folder shows its content like Windows Explorer (list: icon/name/size/type/mtime)
 * - a file shows its properties (icon, path, size, times, attributes, image preview)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <gtkmm.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace stdfs = std::filesystem;

class CtMainWin;

class CtWorkspaceBrowserColumns : public Gtk::TreeModelColumnRecord
{
public:
    CtWorkspaceBrowserColumns() {
        add(colIcon); add(colName); add(colSize); add(colType); add(colMTime);
        add(colPath); add(colIsDir);
    }
    Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> colIcon;
    Gtk::TreeModelColumn<Glib::ustring>             colName;
    Gtk::TreeModelColumn<Glib::ustring>             colSize;
    Gtk::TreeModelColumn<Glib::ustring>             colType;
    Gtk::TreeModelColumn<Glib::ustring>             colMTime;
    Gtk::TreeModelColumn<std::string>               colPath;
    Gtk::TreeModelColumn<bool>                      colIsDir;
};

class CtWorkspaceBrowser : public Gtk::Box
{
public:
    CtWorkspaceBrowser(CtMainWin* pCtMainWin);
    virtual ~CtWorkspaceBrowser();

    // entry point from the project panel: path = selected folder or file
    void browse_path(const std::string& path, bool isDir);

    // shared file operations: the workspace panel context menu uses the same
    // operations and the same clipboard as the edit-area browser
    void set_file_clipboard(const std::vector<std::string>& paths, bool cut);
    bool has_file_clipboard() const { return not _clipPaths.empty(); }
    bool is_file_clipboard_cut() const { return _clipCut; }
    void paste_into(const std::string& destDir);
    void new_folder_in(const std::string& dirPath);
    void new_file_in(const std::string& dirPath);
    void rename_path(const std::string& path);
    void delete_paths(const std::vector<std::string>& paths);
    void open_in_powershell(const std::string& dirPath);
    void refresh() { _refresh_current(); }
    std::string current_dir() const;

protected:
    void        _navigate(const std::string& path, bool isDir, bool pushHistory);
    void        _fill_dir(const std::string& dirPath);
    void        _fill_props(const std::string& filePath);
    void        _on_list_cursor_changed();
    void        _on_list_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column);
    bool        _on_list_button_press(GdkEventButton* pEvent);
    bool        _on_list_key_press(GdkEventKey* pEvent);
    void        _show_list_menu(GdkEventButton* pEvent, const std::vector<std::string>& selPaths);
    // explorer-like view modes
    enum class ViewMode { Details, LargeIcons, ExtraLargeIcons };
    void        _set_view_mode(ViewMode mode);
    int         _view_icon_size() const;
    Glib::RefPtr<Gdk::Pixbuf> _entry_pixbuf(const std::string& path, bool isDir);
    Glib::RefPtr<Gdk::Pixbuf> _load_thumbnail(const std::string& path, int size);
    void        _on_icon_selection_changed();
    void        _on_icon_item_activated(const Gtk::TreeModel::Path& path);
    bool        _on_icon_button_press(GdkEventButton* pEvent);
    bool        _on_icon_key_press(GdkEventKey* pEvent);
    bool        _handle_key_press(GdkEventKey* pEvent, const std::vector<std::string>& selPaths);
    std::vector<std::string> _list_sel_paths();
    std::vector<std::string> _icon_sel_paths();
    void        _activate_entry(const std::string& entryPath, bool isDir);
    void        _show_details(const Gtk::TreeModel::Row& row);
    void        _clear_details();
    void        _refresh_current();
    void        _on_back_clicked();
    void        _go_up();
    void        _switch_list();
    void        _switch_props();
    // file operations (explorer-like context menu)
    void        _do_new_folder();
    void        _do_new_file();
    void        _do_paste();
    void        _do_rename(const std::string& srcPath);
    void        _do_delete(const std::vector<std::string>& paths);
    void        _refresh_dir_if_current(const std::string& dirPath);
    std::vector<std::string> _system_clipboard_files(); // files copied in Windows Explorer
    Glib::ustring _entry_dialog(const Glib::ustring& title, const Glib::ustring& defaultText);
    static void _copy_recursive(const stdfs::path& src, const stdfs::path& dst);
    static Glib::ustring _format_size(guint64 bytes);
    static Glib::ustring _format_time(std::time_t t);
    static Glib::ustring _get_mtime_str(const std::string& path);
    static Glib::ustring _type_name_of(const std::string& path, bool isDir);
    Glib::RefPtr<Gdk::Pixbuf> _get_icon(const std::string& path, bool isDir, int size = 16);
    static bool _is_image(const std::string& path);

protected:
    CtMainWin* const _pCtMainWin;
    CtWorkspaceBrowserColumns _columns;
    Glib::RefPtr<Gtk::ListStore> _rListStore;
    Gtk::TreeView   _listView;
    Gtk::IconView   _iconView;     // icons view (large / extra-large thumbnails)
    Gtk::Stack      _viewStack;    // list page content: "details" (TreeView) / "icons" (IconView)
    ViewMode        _viewMode{ViewMode::Details};
    Gtk::Stack      _pages;        // "list" / "props"
    Gtk::Button     _backButton;
    Gtk::Button     _upButton;
    Gtk::Label      _pathLabel;
    Gtk::Box        _navBar;
    // properties page
    Gtk::Image      _propIcon;
    Gtk::Label      _propName;
    Gtk::Label      _propRows;
    Gtk::Image      _propPreview;
    Gtk::Button     _openButton;
    Gtk::Button     _openFolderButton;
    Gtk::Button     _copyPathButton;
    std::string     _currPath;      // path currently displayed (folder on list page, file on props page)
    bool            _currIsDir{true};
    std::vector<std::string> _navHistory;
    bool            _filling{false};
    // explorer-like state
    Gtk::Label      _detailsLabel;  // bottom strip: info of the selected entry
    std::vector<std::string> _clipPaths;
    bool            _clipCut{false};
};
