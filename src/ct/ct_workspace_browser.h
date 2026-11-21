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
#include <memory>
#include <string>
#include <vector>

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

protected:
    void        _navigate(const std::string& path, bool isDir, bool pushHistory);
    void        _fill_dir(const std::string& dirPath);
    void        _fill_props(const std::string& filePath);
    void        _on_list_cursor_changed();
    void        _on_list_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* column);
    void        _on_back_clicked();
    void        _go_up();
    void        _switch_list();
    void        _switch_props();
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
};
