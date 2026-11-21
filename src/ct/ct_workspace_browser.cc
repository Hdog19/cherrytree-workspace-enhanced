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
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>

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
        return;
    }
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
        _navigate(entryPath, true, true); // click a folder inside the list: drill down
    }
    else {
        _navigate(entryPath, false, true); // click a file: show its properties
    }
}

void CtWorkspaceBrowser::_on_list_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* /*column*/)
{
    // double click: open externally (like explorer)
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
        fs::open_folderpath(fs::path{entryPath}, _pCtMainWin->get_ct_config());
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
