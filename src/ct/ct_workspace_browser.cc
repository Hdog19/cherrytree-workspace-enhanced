/*
 * ct_workspace_browser.cc
 *
 * See ct_workspace_browser.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ct_workspace_browser.h"
#include "ct_main_win.h"
#include "ct_filesystem.h"
#include "ct_dialogs.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace stdfs = std::filesystem;

CtWorkspaceBrowser::CtWorkspaceBrowser(CtMainWin* pCtMainWin)
 : Gtk::Box{Gtk::ORIENTATION_VERTICAL}
 , _pCtMainWin{pCtMainWin}
{
    set_spacing(0);

    // navigation bar: back / up / current path
#if GTKMM_MAJOR_VERSION >= 4
    _navBar.set_margin_start(4); _navBar.set_margin_end(4);
    _navBar.set_margin_top(4); _navBar.set_margin_bottom(4);
    _navBar.append(_backButton);
    _navBar.append(_upButton);
    _navBar.append(_pathLabel);
#else
    _navBar.set_border_width(4);
    _navBar.pack_start(_backButton, Gtk::PACK_SHRINK);
    _navBar.pack_start(_upButton, Gtk::PACK_SHRINK);
    _navBar.pack_start(_pathLabel);
#endif
    _navBar.set_spacing(4);
    _backButton.set_label("←");
    _backButton.set_tooltip_text("后退");
    _backButton.set_sensitive(false);
    _backButton.signal_clicked().connect(sigc::mem_fun(*this, &CtWorkspaceBrowser::_on_back_clicked));
    _upButton.set_label("↑");
    _upButton.set_tooltip_text("上一级");
    _upButton.signal_clicked().connect(sigc::mem_fun(*this, &CtWorkspaceBrowser::_go_up));
    _pathLabel.set_ellipsize(Pango::ELLIPSIZE_START);
    _pathLabel.set_halign(Gtk::ALIGN_START);

    // list page (explorer-like)
    _rListStore = Gtk::ListStore::create(_columns);
    _listView.set_model(_rListStore);
    Gtk::TreeViewColumn* pIconColumn = Gtk::manage(new Gtk::TreeViewColumn{});
    pIconColumn->pack_start(_columns.colIcon, false);
    _listView.append_column(*pIconColumn);
    _listView.append_column("名称", _columns.colName);
    _listView.get_column(1)->set_expand(true);
    _listView.append_column("大小", _columns.colSize);
    _listView.append_column("类型", _columns.colType);
    _listView.append_column("修改时间", _columns.colMTime);
    _listView.get_column(0)->set_fixed_width(28);
    Gtk::ScrolledWindow* pScrolledList = Gtk::manage(new Gtk::ScrolledWindow{});
    pScrolledList->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    _listView.signal_cursor_changed().connect(sigc::mem_fun(*this, &CtWorkspaceBrowser::_on_list_cursor_changed));
    _listView.signal_row_activated().connect(sigc::mem_fun(*this, &CtWorkspaceBrowser::_on_list_row_activated));
#if GTKMM_MAJOR_VERSION < 4
    _listView.signal_button_press_event().connect(
        [this](GdkEventButton* pEvent) { return _on_list_button_press(pEvent); }, false);
    _listView.signal_key_press_event().connect(
        [this](GdkEventKey* pEvent) { return _on_list_key_press(pEvent); }, false);
#endif

    // properties page
    Gtk::Box* pPropsBox = Gtk::manage(new Gtk::Box{Gtk::ORIENTATION_VERTICAL, 8});
#if GTKMM_MAJOR_VERSION >= 4
    pPropsBox->set_margin_start(16); pPropsBox->set_margin_end(16);
    pPropsBox->set_margin_top(16); pPropsBox->set_margin_bottom(16);
    pPropsBox->append(_propIcon);
    pPropsBox->append(_propName);
    pPropsBox->append(_propRows);
    pPropsBox->append(_propPreview);
#else
    pPropsBox->set_border_width(16);
    pPropsBox->pack_start(_propIcon, Gtk::PACK_SHRINK);
    pPropsBox->pack_start(_propName, Gtk::PACK_SHRINK);
    pPropsBox->pack_start(_propRows, Gtk::PACK_SHRINK);
    pPropsBox->pack_start(_propPreview, Gtk::PACK_SHRINK);
#endif
    _propIcon.set_pixel_size(48);
    _propName.set_halign(Gtk::ALIGN_START);
    _propName.override_font(Pango::FontDescription{"bold 12"});
    _propRows.set_halign(Gtk::ALIGN_START);
    _propRows.set_line_wrap(true);
    Gtk::Box* pButtonBox = Gtk::manage(new Gtk::Box{Gtk::ORIENTATION_HORIZONTAL, 8});
    pButtonBox->pack_start(_openButton, Gtk::PACK_SHRINK);
    pButtonBox->pack_start(_openFolderButton, Gtk::PACK_SHRINK);
    pButtonBox->pack_start(_copyPathButton, Gtk::PACK_SHRINK);
#if GTKMM_MAJOR_VERSION >= 4
    pPropsBox->append(*pButtonBox);
#else
    pPropsBox->pack_start(*pButtonBox, Gtk::PACK_SHRINK);
#endif
    _openButton.set_label("打开");
    _openFolderButton.set_label("打开所在文件夹");
    _copyPathButton.set_label("复制路径");
    _openButton.signal_clicked().connect([this]() {
        if (not _currPath.empty() and not _currIsDir) {
            fs::open_filepath(fs::path{_currPath}, false, _pCtMainWin->get_ct_config());
        }
    });
    _openFolderButton.signal_clicked().connect([this]() {
        if (not _currPath.empty()) {
            const stdfs::path parent = stdfs::path{_currPath}.parent_path();
            if (not parent.empty()) {
                fs::open_folderpath(fs::path{parent.string()}, _pCtMainWin->get_ct_config());
            }
        }
    });
    _copyPathButton.signal_clicked().connect([this]() {
#if GTKMM_MAJOR_VERSION >= 4
        _pCtMainWin->get_clipboard()->set_text(_currPath);
#else
        Gtk::Clipboard::get()->set_text(_currPath);
#endif
    });

#if GTKMM_MAJOR_VERSION >= 4
    pScrolledList->set_child(_listView);
    _pages.append(*pScrolledList);
    _pages.append(*pPropsBox);
    set_start_child(_navBar);
    set_child(_pages);
#else
    pScrolledList->add(_listView);
    _pages.add(*pScrolledList, "list");
    _pages.add(*pPropsBox, "props");
    pack_start(_navBar, Gtk::PACK_SHRINK);
    pack_start(_pages);
    // bottom details strip for the selected entry (explorer-like)
    Gtk::Separator* pDetailsSeparator = Gtk::manage(new Gtk::Separator{});
    pack_start(*pDetailsSeparator, Gtk::PACK_SHRINK);
    pack_end(_detailsLabel, Gtk::PACK_SHRINK);
#endif
#if GTKMM_MAJOR_VERSION >= 4
    append(_detailsLabel);
#endif
    _detailsLabel.set_halign(Gtk::ALIGN_START);
    _detailsLabel.set_ellipsize(Pango::ELLIPSIZE_END);
#if GTKMM_MAJOR_VERSION >= 4
    _detailsLabel.set_margin_start(6); _detailsLabel.set_margin_end(6);
    _detailsLabel.set_margin_top(3); _detailsLabel.set_margin_bottom(3);
#else
    _detailsLabel.set_margin_left(6); _detailsLabel.set_margin_right(6);
    _detailsLabel.set_margin_top(3); _detailsLabel.set_margin_bottom(3);
#endif
    _pages.set_visible_child("list");
}

CtWorkspaceBrowser::~CtWorkspaceBrowser() = default;

void CtWorkspaceBrowser::browse_path(const std::string& path, bool isDir)
{
    _navHistory.clear();
    _navigate(path, isDir, false);
}

void CtWorkspaceBrowser::_navigate(const std::string& path, bool isDir, bool pushHistory)
{
    if (path.empty() or not Glib::file_test(path, Glib::FILE_TEST_EXISTS)) {
        return;
    }
    if (pushHistory and not _currPath.empty() and _currPath != path) {
        _navHistory.push_back(_currPath);
    }
    if (isDir) {
        _fill_dir(path);
    }
    else {
        _fill_props(path);
    }
}

void CtWorkspaceBrowser::_fill_dir(const std::string& dirPath)
{
    _currPath = dirPath;
    _currIsDir = true;
    _filling = true;

    _rListStore->clear();
    std::vector<stdfs::directory_entry> entries;
    try {
        for (const stdfs::directory_entry& entry : stdfs::directory_iterator(stdfs::path{dirPath})) {
            entries.push_back(entry);
        }
    }
    catch (const std::exception& e) {
        spdlog::warn("cannot list dir {}: {}", dirPath, e.what());
    }
    std::sort(entries.begin(), entries.end(),
              [](const stdfs::directory_entry& a, const stdfs::directory_entry& b) {
                  const bool aDir = a.is_directory();
                  const bool bDir = b.is_directory();
                  if (aDir != bDir) return aDir; // folders first
                  return a.path().filename() < b.path().filename();
              });
    for (const stdfs::directory_entry& entry : entries) {
        std::error_code ec;
        const bool isDirEntry = entry.is_directory(ec);
        if (not isDirEntry and not entry.is_regular_file(ec)) {
            continue;
        }
        Gtk::TreeModel::Row row = *(_rListStore->append());
        row[_columns.colIcon] = _get_icon(entry.path().string(), isDirEntry);
        row[_columns.colName] = Glib::path_get_basename(entry.path().string());
        if (isDirEntry) {
            row[_columns.colSize] = "";
        }
        else {
            try {
                row[_columns.colSize] = _format_size(stdfs::file_size(entry.path()));
            }
            catch (...) {
                row[_columns.colSize] = "";
            }
        }
        row[_columns.colType] = _type_name_of(entry.path().string(), isDirEntry);
        row[_columns.colMTime] = _get_mtime_str(entry.path().string());
        row[_columns.colPath] = entry.path().string();
        row[_columns.colIsDir] = isDirEntry;
    }
    _pathLabel.set_text(dirPath);
    _backButton.set_sensitive(not _navHistory.empty());
    _upButton.set_sensitive(not stdfs::path{dirPath}.parent_path().empty());
    _filling = false;
    _pages.set_visible_child("list");
}

void CtWorkspaceBrowser::_fill_props(const std::string& filePath)
{
    _currPath = filePath;
    _currIsDir = false;
    const stdfs::path fpath{filePath};

    _propIcon.set(_get_icon(filePath, false, 48));
    _propName.set_text(Glib::path_get_basename(filePath));

    Glib::ustring props;
    props += Glib::ustring::compose("路径：%1\n", filePath);
    props += Glib::ustring::compose("类型：%1\n", _type_name_of(filePath, false));
    try {
        props += Glib::ustring::compose("大小：%1\n", _format_size(stdfs::file_size(fpath)));
    } catch (...) {}
    const Glib::ustring mtimeStr = _get_mtime_str(filePath);
    if (not mtimeStr.empty()) {
        props += Glib::ustring::compose("修改时间：%1\n", mtimeStr);
    }
    try {
        std::error_code ec;
        const auto perms = stdfs::status(fpath, ec).permissions();
        if (not ec) {
            const bool writable = (perms & stdfs::perms::owner_write) != stdfs::perms::none;
            props += Glib::ustring::compose("属性：%1\n", writable ? "可写" : "只读");
        }
    } catch (...) {}
    _propRows.set_text(props);

    // image preview
    if (_is_image(filePath)) {
        try {
            Glib::RefPtr<Gdk::Pixbuf> rPixbuf = Gdk::Pixbuf::create_from_file(filePath);
            const int w = rPixbuf->get_width(), h = rPixbuf->get_height();
            const int maxSize = 320;
            if (w > maxSize or h > maxSize) {
                const double scale = std::min((double)maxSize / w, (double)maxSize / h);
                rPixbuf = rPixbuf->scale_simple((int)(w * scale), (int)(h * scale), Gdk::INTERP_BILINEAR);
            }
            _propPreview.set(rPixbuf);
        }
        catch (...) {
            _propPreview.clear();
        }
    }
    else {
        _propPreview.clear();
    }

    _pathLabel.set_text(filePath);
    _backButton.set_sensitive(not _navHistory.empty());
    _upButton.set_sensitive(not fpath.parent_path().empty());
    _pages.set_visible_child("props");
}

void CtWorkspaceBrowser::_on_list_cursor_changed()
{
    if (_filling) {
        return;
    }
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* pColumn{nullptr};
    _listView.get_cursor(path, pColumn);
    if (not path) {
        _clear_details();
        return;
    }
    Gtk::TreeModel::iterator iter = _rListStore->get_iter(path);
    if (not iter) {
        _clear_details();
        return;
    }
    // explorer-like: selection only, info shown in the bottom strip; the view does not change
    _show_details(*iter);
}

bool CtWorkspaceBrowser::_on_list_button_press(GdkEventButton* pEvent)
{
    Gtk::TreeModel::Path path;
    Gtk::TreeViewColumn* pColumn{nullptr};
    int cellX{0}, cellY{0};
    const bool hitRow = _listView.get_path_at_pos((int)pEvent->x, (int)pEvent->y, path, pColumn, cellX, cellY);
    if (pEvent->type == GDK_BUTTON_PRESS and 1 == pEvent->button and not hitRow) {
        // clicking the blank area only clears the selection (explorer-like)
        _listView.get_selection()->unselect_all();
        _clear_details();
        return false;
    }
    if (pEvent->type == GDK_BUTTON_PRESS and 3 == pEvent->button) {
        std::vector<std::string> selPaths;
        if (hitRow) {
            _listView.set_cursor(path);
            // right-click must also select the row: the copy/cut actions rely on it
            _listView.get_selection()->select(path);
            if (Gtk::TreeModel::iterator iter = _rListStore->get_iter(path)) {
                Gtk::TreeModel::Row row = *iter;
                if (not ((std::string)row[_columns.colPath]).empty()) {
                    selPaths.push_back(row[_columns.colPath]);
                }
            }
        }
        else {
            _listView.get_selection()->unselect_all();
            _clear_details();
        }
        _show_list_menu(pEvent, selPaths);
        return true;
    }
    return false;
}

bool CtWorkspaceBrowser::_on_list_key_press(GdkEventKey* pEvent)
{
    const guint keyval = pEvent->keyval;
    const guint state = pEvent->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);
    if (keyval == GDK_KEY_F2) {
        const auto sel = _listView.get_selection()->get_selected_rows();
        if (sel.size() == 1) {
            if (Gtk::TreeModel::iterator iter = _rListStore->get_iter(sel[0])) {
                _do_rename((*iter)[_columns.colPath]);
            }
        }
        return true;
    }
    if (keyval == GDK_KEY_Delete) {
        std::vector<std::string> paths;
        for (const auto& row : _listView.get_selection()->get_selected_rows()) {
            if (Gtk::TreeModel::iterator iter = _rListStore->get_iter(row)) {
                paths.push_back((*iter)[_columns.colPath]);
            }
        }
        _do_delete(paths);
        return true;
    }
    if (state == GDK_CONTROL_MASK and keyval == GDK_KEY_c) { _do_copy(false); return true; }
    if (state == GDK_CONTROL_MASK and keyval == GDK_KEY_x) { _do_copy(true); return true; }
    if (state == GDK_CONTROL_MASK and keyval == GDK_KEY_v) { _do_paste(); return true; }
    return false;
}

void CtWorkspaceBrowser::_show_details(const Gtk::TreeModel::Row& row)
{
    const Glib::ustring size = row[_columns.colSize];
    const Glib::ustring mtime = row[_columns.colMTime];
    _detailsLabel.set_text(Glib::ustring::compose("%1　类型：%2　大小：%3　修改：%4",
        Glib::ustring{row[_columns.colName]},
        Glib::ustring{row[_columns.colType]},
        size.empty() ? Glib::ustring{"—"} : size,
        mtime.empty() ? Glib::ustring{"—"} : mtime));
}

void CtWorkspaceBrowser::_clear_details()
{
    _detailsLabel.set_text("");
}

void CtWorkspaceBrowser::_refresh_current()
{
    if (_currIsDir and not _currPath.empty()) {
        _fill_dir(_currPath);
    }
}

void CtWorkspaceBrowser::_show_list_menu(GdkEventButton* pEvent, const std::vector<std::string>& selPaths)
{
    // One and the same menu for a selected entry and for the blank area:
    // items that need a selection are simply disabled on the blank area.
    Gtk::Menu* pMenu = Gtk::manage(new Gtk::Menu{});
    auto addMenuItem = [pMenu](const char* label, std::function<void()> slot, bool sensitive = true) {
        Gtk::MenuItem* pMenuItem = Gtk::manage(new Gtk::MenuItem{label, true});
        pMenuItem->signal_activate().connect(slot);
        pMenuItem->set_sensitive(sensitive);
        pMenu->append(*pMenuItem);
    };
    auto addSeparator = [pMenu]() {
        Gtk::SeparatorMenuItem* pSep = Gtk::manage(new Gtk::SeparatorMenuItem{});
        pMenu->append(*pSep);
    };

    const bool hasSel = not selPaths.empty();
    const std::string selPath = hasSel ? selPaths[0] : std::string{};
    const bool selIsDir = hasSel and Glib::file_test(selPath, Glib::FILE_TEST_IS_DIR);
    const std::string psDir = hasSel ? (selIsDir ? selPath : stdfs::path{selPath}.parent_path().string())
                                     : _currPath;
    const std::string copyTarget = hasSel ? selPath : _currPath;

    addMenuItem("打开（系统默认程序）", [this, selPath, selIsDir]() {
        if (selIsDir) {
            fs::open_folderpath(fs::path{selPath}, _pCtMainWin->get_ct_config());
        }
        else {
            fs::open_filepath(fs::path{selPath}, false, _pCtMainWin->get_ct_config());
        }
    }, hasSel);
    addMenuItem("在 PowerShell 中打开", [this, psDir]() { open_in_powershell(psDir); });
    addMenuItem("复制路径", [this, copyTarget]() {
        Gtk::Clipboard::get()->set_text(copyTarget);
    });
    addSeparator();
    addMenuItem("新建文件夹", [this]() { new_folder_in(_currPath); });
    addMenuItem("新建文本文件", [this]() { new_file_in(_currPath); });
    addSeparator();
    addMenuItem("剪切", [this, selPaths]() { set_file_clipboard(selPaths, true); }, hasSel);
    addMenuItem("复制", [this, selPaths]() { set_file_clipboard(selPaths, false); }, hasSel);
    // "粘贴" is always present and usable, on nodes and on the blank area alike;
    // paste_into() falls back to files copied in Windows Explorer
    addMenuItem(_clipCut ? "粘贴（剪切）" : "粘贴（复制）",
                [this]() { paste_into(_currPath); });
    addSeparator();
    addMenuItem("重命名（F2）", [this, selPath]() { rename_path(selPath); }, hasSel);
    addMenuItem("删除（Del）", [this, selPaths]() { delete_paths(selPaths); }, hasSel);
    addSeparator();
    addMenuItem("刷新", [this]() { refresh(); });

    pMenu->show_all();
#if GTKMM_MAJOR_VERSION >= 4
    pMenu->popup();
#else
    pMenu->popup_at_pointer(reinterpret_cast<const GdkEvent*>(pEvent));
#endif
}

std::string CtWorkspaceBrowser::current_dir() const
{
    if (_currIsDir) {
        return _currPath;
    }
    return _currPath.empty() ? std::string{} : stdfs::path{_currPath}.parent_path().string();
}

void CtWorkspaceBrowser::_refresh_dir_if_current(const std::string& dirPath)
{
    if (dirPath.empty()) {
        return;
    }
    if (_currIsDir and not _currPath.empty() and stdfs::path{_currPath} == stdfs::path{dirPath}) {
        _fill_dir(_currPath);
    }
}

void CtWorkspaceBrowser::set_file_clipboard(const std::vector<std::string>& paths, bool cut)
{
    _clipPaths = paths;
    _clipCut = cut;
}

void CtWorkspaceBrowser::_do_new_folder() { new_folder_in(_currPath); }
void CtWorkspaceBrowser::_do_new_file()   { new_file_in(_currPath); }
void CtWorkspaceBrowser::_do_paste()     { paste_into(_currPath); }
void CtWorkspaceBrowser::_do_rename(const std::string& srcPath) { rename_path(srcPath); }
void CtWorkspaceBrowser::_do_delete(const std::vector<std::string>& paths) { delete_paths(paths); }

void CtWorkspaceBrowser::_do_copy(bool cut)
{
    std::vector<std::string> paths;
    for (const auto& row : _listView.get_selection()->get_selected_rows()) {
        if (Gtk::TreeModel::iterator iter = _rListStore->get_iter(row)) {
            paths.push_back((*iter)[_columns.colPath]);
        }
    }
    if (paths.empty()) {
        // keyboard shortcut: fall back to the row under the cursor
        Gtk::TreeModel::Path cursorPath;
        Gtk::TreeViewColumn* pColumn{nullptr};
        _listView.get_cursor(cursorPath, pColumn);
        if (cursorPath) {
            if (Gtk::TreeModel::iterator iter = _rListStore->get_iter(cursorPath)) {
                paths.push_back((*iter)[_columns.colPath]);
            }
        }
    }
    set_file_clipboard(paths, cut);
}

void CtWorkspaceBrowser::new_folder_in(const std::string& dirPath)
{
    if (not Glib::file_test(dirPath, Glib::FILE_TEST_IS_DIR)) {
        return;
    }
    const Glib::ustring name = _entry_dialog("新建文件夹", "新建文件夹");
    if (name.empty()) {
        return;
    }
    std::error_code ec;
    stdfs::create_directories(stdfs::path{dirPath} / stdfs::path{name.raw()}, ec);
    if (ec) {
        spdlog::warn("cannot create folder {}: {}", name.raw(), ec.message());
        return;
    }
    _refresh_dir_if_current(dirPath);
}

void CtWorkspaceBrowser::new_file_in(const std::string& dirPath)
{
    if (not Glib::file_test(dirPath, Glib::FILE_TEST_IS_DIR)) {
        return;
    }
    const Glib::ustring name = _entry_dialog("新建文本文件", "新建文本文件.txt");
    if (name.empty()) {
        return;
    }
    const stdfs::path newPath = stdfs::path{dirPath} / stdfs::path{name.raw()};
    {
        std::ofstream ofs{newPath, std::ios::app};
    }
    _refresh_dir_if_current(dirPath);
}

void CtWorkspaceBrowser::rename_path(const std::string& srcPath)
{
    const stdfs::path src{srcPath};
    const Glib::ustring oldName = Glib::path_get_basename(srcPath);
    const Glib::ustring name = _entry_dialog("重命名", oldName);
    if (name.empty() or name == oldName) {
        return;
    }
    std::error_code ec;
    stdfs::rename(src, src.parent_path() / stdfs::path{name.raw()}, ec);
    if (ec) {
        spdlog::warn("cannot rename {}: {}", srcPath, ec.message());
        return;
    }
    _refresh_dir_if_current(src.parent_path().string());
}

void CtWorkspaceBrowser::delete_paths(const std::vector<std::string>& paths)
{
    if (paths.empty()) {
        return;
    }
    if (not CtDialogs::question_dialog(
            Glib::ustring::compose("确定删除 %1 个项目吗？此操作不可恢复！",
                                   Glib::ustring{std::to_string(paths.size())}), *_pCtMainWin)) {
        return;
    }
    for (const std::string& path : paths) {
        std::error_code ec;
        stdfs::remove_all(stdfs::path{path}, ec);
        if (ec) {
            spdlog::warn("cannot delete {}: {}", path, ec.message());
        }
    }
    _refresh_dir_if_current(stdfs::path{paths[0]}.parent_path().string());
}

void CtWorkspaceBrowser::paste_into(const std::string& destDir)
{
    if (destDir.empty() or not Glib::file_test(destDir, Glib::FILE_TEST_IS_DIR)) {
        return;
    }
    std::vector<std::string> srcPaths = _clipPaths;
    const bool cut = _clipCut;
    if (srcPaths.empty()) {
        // files copied in Windows Explorer (CF_HDROP) or in a GTK app (text/uri-list)
        srcPaths = _system_clipboard_files();
    }
    spdlog::debug("workspace paste: {} internal / {} clipboard file(s)", _clipPaths.size(), srcPaths.size());
    if (srcPaths.empty()) {
        CtDialogs::warning_dialog("剪贴板中没有可粘贴的文件。\n可先在文件/文件夹上右键「复制」，或在 Windows 资源管理器中复制文件后再粘贴。", *_pCtMainWin);
        return;
    }
    const stdfs::path destDirPath{destDir};
    for (const std::string& srcStr : srcPaths) {
        const stdfs::path src{srcStr};
        if (not stdfs::exists(src)) {
            continue;
        }
        const stdfs::path dest = destDirPath / src.filename();
        std::error_code ec;
        std::error_code eqEc;
        const bool sameFile = stdfs::equivalent(src, dest, eqEc);
        if (not eqEc and sameFile) {
            continue; // pasting onto itself
        }
        if (cut and not _clipPaths.empty()) {
            stdfs::rename(src, dest, ec);
            if (ec) {
                // cross-drive move: copy then remove
                if (stdfs::is_directory(src)) {
                    _copy_recursive(src, dest);
                }
                else {
                    stdfs::copy_file(src, dest, stdfs::copy_options::overwrite_existing, ec);
                }
                stdfs::remove_all(src, ec);
            }
        }
        else if (stdfs::is_directory(src)) {
            _copy_recursive(src, dest);
        }
        else {
            stdfs::copy_file(src, dest, stdfs::copy_options::overwrite_existing, ec);
        }
    }
    if (cut and not _clipPaths.empty()) {
        set_file_clipboard({}, false);
    }
    _refresh_dir_if_current(destDir);
}

std::vector<std::string> CtWorkspaceBrowser::_system_clipboard_files()
{
    std::vector<std::string> paths;
#if GTKMM_MAJOR_VERSION < 4
    GtkClipboard* pClipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (not pClipboard) {
        return paths;
    }
#ifdef _WIN32
    // Windows Explorer exposes copied files as CF_HDROP (a raw DROPFILES structure),
    // NOT as "text/uri-list" -- same target as Cherrytree's own clipboard handling.
    GtkSelectionData* pHdrop = gtk_clipboard_wait_for_contents(
        pClipboard, gdk_atom_intern("CF_HDROP", FALSE));
    if (pHdrop) {
        const guint8* data = gtk_selection_data_get_data(pHdrop);
        const gint len = gtk_selection_data_get_length(pHdrop);
        if (data and len >= 20) {
            // first DWORD of DROPFILES = byte offset of the UTF-16LE, double-NUL terminated file list
            const guint32 offset = *reinterpret_cast<const guint32*>(data);
            if (offset < static_cast<guint32>(len)) {
                const wchar_t* pStr = reinterpret_cast<const wchar_t*>(data + offset);
                const guint8* pEnd = data + len;
                while (reinterpret_cast<const guint8*>(pStr) < pEnd and *pStr != L'\0') {
                    const std::wstring wpath{pStr};
                    try {
                        const Glib::ustring utf8 = Glib::convert(
                            std::string{reinterpret_cast<const char*>(wpath.c_str()),
                                        wpath.size() * sizeof(wchar_t)},
                            "UTF-8", "UTF-16LE");
                        paths.push_back(utf8.raw());
                    }
                    catch (const Glib::Error&) {}
                    pStr += wpath.size() + 1;
                }
            }
        }
        gtk_selection_data_free(pHdrop);
    }
#endif
    if (paths.empty()) {
        // GTK applications expose file lists as text/uri-list
        GtkSelectionData* pData = gtk_clipboard_wait_for_contents(
            pClipboard, gdk_atom_intern("text/uri-list", FALSE));
        if (pData) {
            gchar** uris = gtk_selection_data_get_uris(pData);
            if (uris) {
                for (gchar** pUri = uris; *pUri; ++pUri) {
                    gchar* filename = g_filename_from_uri(*pUri, nullptr, nullptr);
                    if (filename) {
                        paths.emplace_back(filename);
                        g_free(filename);
                    }
                }
                g_strfreev(uris);
            }
            gtk_selection_data_free(pData);
        }
    }
#endif
    return paths;
}

void CtWorkspaceBrowser::_copy_recursive(const stdfs::path& src, const stdfs::path& dst)
{
    std::error_code ec;
    stdfs::create_directories(dst, ec);
    for (const stdfs::directory_entry& entry : stdfs::directory_iterator(src)) {
        const stdfs::path destEntry = dst / entry.path().filename();
        if (entry.is_directory()) {
            _copy_recursive(entry.path(), destEntry);
        }
        else {
            stdfs::copy_file(entry.path(), destEntry, stdfs::copy_options::overwrite_existing, ec);
        }
    }
}

void CtWorkspaceBrowser::open_in_powershell(const std::string& dirPath)
{
    if (dirPath.empty()) {
        return;
    }
    Glib::ustring psPath = dirPath;
    // escape single quotes for the PowerShell single-quoted literal
    Glib::ustring safePath;
    for (gunichar ch : psPath) {
        safePath += (ch == '\'' ? Glib::ustring{"''"} : Glib::ustring(1, ch));
    }
    try {
        Glib::spawn_command_line_async(Glib::ustring::compose(
            "powershell -NoExit -Command \"Set-Location -LiteralPath '%1'\"", safePath));
    }
    catch (const Glib::Error& e) {
        spdlog::warn("cannot start powershell: {}", e.what().raw());
    }
}

Glib::ustring CtWorkspaceBrowser::_entry_dialog(const Glib::ustring& title, const Glib::ustring& defaultText)
{
#if GTKMM_MAJOR_VERSION >= 4
    Gtk::Dialog dialog{title, true};
    dialog.set_transient_for(*_pCtMainWin);
    Gtk::Entry entry;
    entry.set_text(defaultText);
    entry.select_region(0, (int)defaultText.raw().size());
    dialog.set_child(entry);
    dialog.add_button("确定", Gtk::RESPONSE_OK);
    dialog.add_button("取消", Gtk::RESPONSE_CANCEL);
    dialog.set_default_response(Gtk::RESPONSE_OK);
    entry.signal_activate().connect([&]() { dialog.response(Gtk::RESPONSE_OK); });
    if (dialog.run() != Gtk::RESPONSE_OK) {
        return {};
    }
    return entry.get_text();
#else
    Gtk::Dialog dialog{title, *_pCtMainWin, Gtk::DIALOG_MODAL};
    dialog.add_button("确定", Gtk::RESPONSE_OK);
    dialog.add_button("取消", Gtk::RESPONSE_CANCEL);
    dialog.set_default_response(Gtk::RESPONSE_OK);
    dialog.set_border_width(10);
    Gtk::Entry entry;
    entry.set_text(defaultText);
    entry.select_region(0, (int)defaultText.raw().size());
    dialog.get_content_area()->pack_start(entry, Gtk::PACK_SHRINK);
    entry.signal_activate().connect([&]() { dialog.response(Gtk::RESPONSE_OK); });
    dialog.show_all_children();
    if (dialog.run() != Gtk::RESPONSE_OK) {
        return {};
    }
    return entry.get_text();
#endif
}

void CtWorkspaceBrowser::_on_list_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* /*column*/)
{
    // double click: drill into a folder, open a file externally (like explorer)
    Gtk::TreeModel::iterator iter = _rListStore->get_iter(path);
    if (not iter) {
        return;
    }
    Gtk::TreeModel::Row row = *iter;
    const std::string entryPath = row[_columns.colPath];
    if (entryPath.empty()) {
        return;
    }
    if (row[_columns.colIsDir]) {
        _navigate(entryPath, true, true);
    }
    else {
        fs::open_filepath(fs::path{entryPath}, false, _pCtMainWin->get_ct_config());
    }
}

void CtWorkspaceBrowser::_on_back_clicked()
{
    if (_navHistory.empty()) {
        return;
    }
    const std::string prevPath = _navHistory.back();
    _navHistory.pop_back();
    const bool isDirPrev = Glib::file_test(prevPath, Glib::FILE_TEST_IS_DIR);
    // navigate without pushing history again
    if (isDirPrev) {
        _fill_dir(prevPath);
    }
    else {
        _fill_props(prevPath);
    }
    // the page we just left stays reachable via "前进-less" up-navigation; keep history minimal
    if (not _navHistory.empty()) {
        _backButton.set_sensitive(true);
    }
}

void CtWorkspaceBrowser::_go_up()
{
    if (_currPath.empty()) {
        return;
    }
    const stdfs::path parent = stdfs::path{_currPath}.parent_path();
    if (parent.empty()) {
        return;
    }
    _navigate(parent.string(), true, true);
}

Glib::ustring CtWorkspaceBrowser::_format_size(guint64 bytes)
{
    char buf[64]{};
    if (bytes < 1024) {
        snprintf(buf, sizeof(buf), "%u B", (unsigned)bytes);
    }
    else if (bytes < 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    }
    else if (bytes < 1024ULL * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    }
    else {
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return Glib::ustring{buf};
}

Glib::ustring CtWorkspaceBrowser::_format_time(std::time_t t)
{
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[64]{};
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmBuf);
    return Glib::ustring{buf};
}

Glib::ustring CtWorkspaceBrowser::_get_mtime_str(const std::string& path)
{
    try {
        auto info = Gio::File::create_for_path(path)->query_info(G_FILE_ATTRIBUTE_TIME_MODIFIED);
        return _format_time((std::time_t)info->get_attribute_uint64(G_FILE_ATTRIBUTE_TIME_MODIFIED));
    }
    catch (...) {
        return {};
    }
}

Glib::ustring CtWorkspaceBrowser::_type_name_of(const std::string& path, bool isDir)
{
    if (isDir) {
        return "文件夹";
    }
    const std::string ext = stdfs::path{path}.extension().string();
    if (ext.size() > 1) {
        return Glib::ustring{ext.substr(1)}.uppercase() + " 文件";
    }
    return "文件";
}

Glib::RefPtr<Gdk::Pixbuf> CtWorkspaceBrowser::_get_icon(const std::string& path, bool isDir, int size)
{
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
        return rIconTheme->load_icon(iconName, size);
    }
    catch (...) {
        return Glib::RefPtr<Gdk::Pixbuf>{};
    }
}

bool CtWorkspaceBrowser::_is_image(const std::string& path)
{
    static const char* const imageExts[] = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".ico", nullptr};
    const Glib::ustring ext = Glib::ustring{stdfs::path{path}.extension().string()}.lowercase();
    for (int i = 0; imageExts[i]; ++i) {
        if (ext == imageExts[i]) {
            return true;
        }
    }
    return false;
}
