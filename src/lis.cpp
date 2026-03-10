#include <locale.h>
#include <ncurses.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

    constexpr std::string_view INDENT_PIPE = "│ ";
    constexpr std::string_view INDENT_BRANCH = "├ ";
    constexpr std::string_view INDENT_LAST = "└ ";
    constexpr std::string_view INDENT_SPACE = "  ";

    constexpr std::string_view GIT_UNTRACKED = "✭";
    constexpr std::string_view GIT_MODIFIED = "✹";
    constexpr std::string_view GIT_STAGED = "✚";
    constexpr std::string_view GIT_RENAMED = "➜";
    constexpr std::string_view GIT_IGNORED = "☒";
    constexpr std::string_view GIT_UNMERGED = "═";
    constexpr std::string_view GIT_DELETED = "✖";
    constexpr std::string_view GIT_UNKNOWN = "?";

    constexpr std::string_view MARK_SELECTED = "✓";
    constexpr std::string_view MARK_READONLY = "✗";

    constexpr std::string_view ICON_FOLDER_CLOSED = "";
    constexpr std::string_view ICON_FOLDER_OPEN = "";
    constexpr std::string_view ICON_FOLDER_SYMLINK = "";
    constexpr std::string_view ICON_FILE_DEFAULT = "";
    constexpr std::string_view ICON_FILE_SYMLINK = "";

    enum class EntryKind { Directory, File };
    enum class GitKind { None, Untracked, Modified, Staged, Renamed, Ignored, Unmerged, Deleted, Unknown };

    enum ColorPairId : short {
        PAIR_DEFAULT = 1,
        PAIR_DIRECTORY = 2,
        PAIR_FILE = 3,
        PAIR_GIT_WARN = 4,
        PAIR_GIT_OK = 5,
        PAIR_GIT_BAD = 6,
        PAIR_DIM = 7,
        PAIR_SELECTION = 8,
    };

    constexpr short ANSI_WHITE = 15;
    constexpr short ANSI_TEXT = 253;
    constexpr short ANSI_DIM = 244;
    constexpr short ANSI_FILE = 2;
    constexpr short ANSI_GIT_WARN = 220;
    constexpr short ANSI_GIT_OK = 142;
    constexpr short ANSI_GIT_BAD = 167;
    constexpr short ANSI_ICON_DIR = 37;
    constexpr short ANSI_ACCENT_RED = 167;

    struct Entry {
        fs::path path;
        std::string name;
        std::string icon;
        EntryKind kind = EntryKind::File;
        GitKind git = GitKind::None;
        bool is_hidden = false;
        bool is_readonly = false;
        bool is_last = false;
        bool is_expanded = false;
        int depth = 0;
        std::vector<bool> ancestor_has_more;
        std::uintmax_t size = 0;
    };

    struct Options {
        fs::path root;
        fs::path highlight_target;
        std::string output_file;
        bool show_hidden = false;
        bool show_header = true;
        bool alt_screen = false;
        bool show_git = false;
        bool show_size = false;
        bool generic_icons = false;
        int max_depth = -1;
        int background_color = -1;
        int selection_background_color = -1;
    };

    struct TreeState {
        Options options;
        std::vector<Entry> visible;
        int cursor = 0;
        std::map<fs::path, GitKind> git_status;
    };

    struct CursesSession {
        explicit CursesSession(const Options &options) {
            setlocale(LC_ALL, "");
            initscr();
            raw();
            noecho();
            nonl();
            keypad(stdscr, TRUE);
            leaveok(stdscr, TRUE);
            curs_set(0);
            init_colors(options);
        }

        ~CursesSession() {
            curs_set(1);
            endwin();
        }

        static void init_colors(const Options &options) {
            if (!has_colors()) {
                return;
            }

            start_color();
            use_default_colors();

            const short base_bg = (options.background_color >= 0 && COLORS > options.background_color)
                                      ? static_cast<short>(options.background_color)
                                      : -1;
            const short selection_bg =
                (options.selection_background_color >= 0 && COLORS > options.selection_background_color)
                    ? static_cast<short>(options.selection_background_color)
                    : COLOR_BLACK;

            init_pair(PAIR_DEFAULT, -1, base_bg);
            init_pair(PAIR_DIRECTORY, 1, base_bg);
            init_pair(PAIR_FILE, ANSI_FILE, base_bg);
            init_pair(PAIR_GIT_WARN, ANSI_GIT_WARN, base_bg);
            init_pair(PAIR_GIT_OK, ANSI_GIT_OK, base_bg);
            init_pair(PAIR_GIT_BAD, ANSI_GIT_BAD, base_bg);
            init_pair(PAIR_DIM, ANSI_DIM, base_bg);
            init_pair(PAIR_SELECTION, -1, selection_bg);
            bkgd(COLOR_PAIR(PAIR_DEFAULT));
        }
    };

    static bool is_hidden_name(const std::string &name) { return !name.empty() && name.front() == '.'; }

    static std::string lower_string(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    static std::string icon_for_name(const std::string &name, bool generic_icons) {
        if (generic_icons) {
            return std::string(ICON_FILE_DEFAULT);
        }

        struct IconDef {
            const char *icon;
            const char *color;
        };

        static const std::map<std::string, IconDef> icons = {
            {"c", {"", "#599eff"}},     {"cpp", {"", "#519aba"}},       {"cc", {"", "#f34b7d"}},
            {"cxx", {"", "#519aba"}},   {"h", {"", "#a074c4"}},         {"hpp", {"", "#a074c4"}},
            {"rs", {"", "#dea584"}},    {"py", {"", "#ffbc03"}},        {"lua", {"", "#00a2ff"}},
            {"js", {"", "#cbcb41"}},    {"ts", {"", "#519aba"}},        {"jsx", {"", "#20c2e3"}},
            {"tsx", {"", "#1354bf"}},   {"html", {"", "#e44d26"}},      {"css", {"", "#663399"}},
            {"json", {"", "#cbcb41"}},  {"yaml", {"", "#6d8086"}},      {"yml", {"", "#6d8086"}},
            {"toml", {"", "#9c4221"}},  {"sh", {"", "#4d5a5e"}},        {"bash", {"", "#89e051"}},
            {"zsh", {"", "#89e051"}},   {"go", {"", "#00add8"}},        {"zig", {"", "#f69a1b"}},
            {"md", {"", "#dddddd"}},    {"txt", {"", "#89e051"}},       {"makefile", {"", "#6d8086"}},
            {"cmake", {"", "#dce3eb"}}, {"gitignore", {"", "#f54d27"}}, {"vim", {"", "#019833"}},
            {"nvim", {"", "#019833"}},  {"pdf", {"", "#b30b00"}},       {"png", {"", "#a074c4"}},
            {"jpg", {"", "#a074c4"}},   {"jpeg", {"", "#a074c4"}},      {"svg", {"", "#ffb13b"}},
        };

        const std::string lower_name = lower_string(name);
        auto full = icons.find(lower_name);
        if (full != icons.end()) {
            return full->second.icon;
        }
        const auto dot = lower_name.rfind('.');
        if (dot == std::string::npos || dot + 1 >= lower_name.size()) {
            return std::string(ICON_FILE_DEFAULT);
        }

        auto it = icons.find(lower_name.substr(dot + 1));
        return it == icons.end() ? std::string(ICON_FILE_DEFAULT) : it->second.icon;
    }

    static std::string icon_color_for_name(const std::string &name) {
        struct IconDef {
            const char *icon;
            const char *color;
        };
        static const std::map<std::string, IconDef> icons = {
            {"c", {"", "#599eff"}},     {"cpp", {"", "#519aba"}},       {"cc", {"", "#f34b7d"}},
            {"cxx", {"", "#519aba"}},   {"h", {"", "#a074c4"}},         {"hpp", {"", "#a074c4"}},
            {"rs", {"", "#dea584"}},    {"py", {"", "#ffbc03"}},        {"lua", {"", "#00a2ff"}},
            {"js", {"", "#cbcb41"}},    {"ts", {"", "#519aba"}},        {"jsx", {"", "#20c2e3"}},
            {"tsx", {"", "#1354bf"}},   {"html", {"", "#e44d26"}},      {"css", {"", "#663399"}},
            {"json", {"", "#cbcb41"}},  {"yaml", {"", "#6d8086"}},      {"yml", {"", "#6d8086"}},
            {"toml", {"", "#9c4221"}},  {"sh", {"", "#4d5a5e"}},        {"bash", {"", "#89e051"}},
            {"zsh", {"", "#89e051"}},   {"go", {"", "#00add8"}},        {"zig", {"", "#f69a1b"}},
            {"md", {"", "#dddddd"}},    {"txt", {"", "#89e051"}},       {"makefile", {"", "#6d8086"}},
            {"cmake", {"", "#dce3eb"}}, {"gitignore", {"", "#f54d27"}}, {"vim", {"", "#019833"}},
            {"nvim", {"", "#019833"}},  {"pdf", {"", "#b30b00"}},       {"png", {"", "#a074c4"}},
            {"jpg", {"", "#a074c4"}},   {"jpeg", {"", "#a074c4"}},      {"svg", {"", "#ffb13b"}},
        };
        const std::string lower_name = lower_string(name);
        auto full = icons.find(lower_name);
        if (full != icons.end()) {
            return full->second.color;
        }
        const auto dot = lower_name.rfind('.');
        if (dot == std::string::npos || dot + 1 >= lower_name.size()) {
            return "#999999";
        }
        auto it = icons.find(lower_name.substr(dot + 1));
        return it == icons.end() ? "#999999" : it->second.color;
    }

    static GitKind classify_git(char x, char y) {
        if (x == '?' && y == '?')
            return GitKind::Untracked;
        if (x == '!' && y == '!')
            return GitKind::Ignored;
        if (x == ' ' && y == 'M')
            return GitKind::Modified;
        if (x == 'M' || x == 'A' || x == 'C')
            return GitKind::Staged;
        if (x == 'R')
            return GitKind::Renamed;
        if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D'))
            return GitKind::Unmerged;
        if (x == 'D' || y == 'D')
            return GitKind::Deleted;
        if (x == ' ' && y == ' ')
            return GitKind::None;
        return GitKind::Unknown;
    }

    static std::string git_glyph(GitKind git) {
        switch (git) {
        case GitKind::Untracked:
            return std::string(GIT_UNTRACKED);
        case GitKind::Modified:
            return std::string(GIT_MODIFIED);
        case GitKind::Staged:
            return std::string(GIT_STAGED);
        case GitKind::Renamed:
            return std::string(GIT_RENAMED);
        case GitKind::Ignored:
            return std::string(GIT_IGNORED);
        case GitKind::Unmerged:
            return std::string(GIT_UNMERGED);
        case GitKind::Deleted:
            return std::string(GIT_DELETED);
        case GitKind::Unknown:
            return std::string(GIT_UNKNOWN);
        default:
            return " ";
        }
    }

    static void refresh_git_status(TreeState &state) {
        state.git_status.clear();
        if (!state.options.show_git) {
            return;
        }

        fs::path current = state.options.root;
        while (!current.empty() && current != current.root_path() && !fs::exists(current / ".git")) {
            current = current.parent_path();
        }
        if (current.empty() || !fs::exists(current / ".git")) {
            return;
        }

        const std::string command = "cd \"" + current.string() + "\" && git status --porcelain -uall 2>/dev/null";
        FILE *pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return;
        }

        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            std::string line(buffer);
            if (line.size() < 4) {
                continue;
            }
            std::string path_part = line.substr(3);
            while (!path_part.empty() && (path_part.back() == '\n' || path_part.back() == '\r')) {
                path_part.pop_back();
            }
            std::error_code error;
            const fs::path full_path = fs::weakly_canonical(current / path_part, error);
            if (!error) {
                state.git_status[full_path] = classify_git(line[0], line[1]);
            }
        }
        pclose(pipe);
    }

    static int find_entry_index(const TreeState &state, const fs::path &path) {
        std::error_code path_error;
        const fs::path target = fs::weakly_canonical(path, path_error);
        for (std::size_t index = 0; index < state.visible.size(); ++index) {
            std::error_code entry_error;
            const fs::path current = fs::weakly_canonical(state.visible[index].path, entry_error);
            if (!path_error && !entry_error && current == target) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    static std::vector<Entry> list_children(const fs::path &dir, TreeState &state, int depth) {
        std::vector<Entry> directories;
        std::vector<Entry> files;

        for (const auto &item : fs::directory_iterator(dir)) {
            const fs::path path = item.path();
            const std::string name = path.filename().string();
            if (!state.options.show_hidden && is_hidden_name(name)) {
                continue;
            }

            Entry entry;
            entry.path = path;
            entry.name = name;
            entry.depth = depth;
            std::error_code perms_error;
            const auto status = item.status(perms_error);
            entry.is_readonly = !perms_error && (status.permissions() & fs::perms::owner_write) == fs::perms::none;

            std::error_code type_error;
            if (item.is_directory(type_error) && !type_error) {
                entry.kind = EntryKind::Directory;
                entry.icon = std::string(ICON_FOLDER_CLOSED);
                directories.push_back(std::move(entry));
            } else {
                entry.kind = EntryKind::File;
                entry.icon = icon_for_name(name, state.options.generic_icons);
                std::error_code size_error;
                entry.size = item.is_regular_file(size_error) && !size_error ? item.file_size(size_error) : 0;
                files.push_back(std::move(entry));
            }
        }

        auto sort_by_name = [](const Entry &left, const Entry &right) { return left.name < right.name; };
        std::sort(directories.begin(), directories.end(), sort_by_name);
        std::sort(files.begin(), files.end(), sort_by_name);

        directories.insert(directories.end(), files.begin(), files.end());
        for (std::size_t index = 0; index < directories.size(); ++index) {
            directories[index].is_last = index + 1 == directories.size();
            std::error_code error;
            const fs::path canonical = fs::weakly_canonical(directories[index].path, error);
            if (!error) {
                auto git_it = state.git_status.find(canonical);
                if (git_it != state.git_status.end()) {
                    directories[index].git = git_it->second;
                }
            }
        }
        return directories;
    }

    static void rebuild_visible(TreeState &state) {
        std::set<fs::path> expanded;
        for (const Entry &entry : state.visible) {
            if (entry.kind == EntryKind::Directory && entry.is_expanded) {
                std::error_code error;
                expanded.insert(fs::weakly_canonical(entry.path, error));
            }
        }

        state.visible.clear();
        Entry root;
        root.path = state.options.root;
        root.name = state.options.root.filename().empty() ? state.options.root.string()
                                                          : state.options.root.filename().string();
        root.icon = std::string(ICON_FOLDER_OPEN);
        root.kind = EntryKind::Directory;
        root.is_last = true;
        root.is_expanded = true;
        state.visible.push_back(root);

        for (std::size_t index = 0; index < state.visible.size(); ++index) {
            Entry &entry = state.visible[index];
            if (entry.kind != EntryKind::Directory || !entry.is_expanded) {
                continue;
            }

            std::vector<Entry> children = list_children(entry.path, state, entry.depth + 1);
            for (Entry &child : children) {
                child.ancestor_has_more = entry.ancestor_has_more;
                if (entry.depth > 0) {
                    child.ancestor_has_more.push_back(!entry.is_last);
                }
                if (child.kind == EntryKind::Directory) {
                    std::error_code error;
                    const fs::path canonical = fs::weakly_canonical(child.path, error);
                    child.is_expanded = !error && expanded.contains(canonical);
                    child.icon = child.is_expanded ? std::string(ICON_FOLDER_OPEN) : std::string(ICON_FOLDER_CLOSED);
                }
            }
            state.visible.insert(state.visible.begin() + static_cast<std::ptrdiff_t>(index + 1), children.begin(),
                                 children.end());
        }

        if (state.visible.empty()) {
            state.cursor = 0;
        } else {
            state.cursor = std::clamp(state.cursor, 0, static_cast<int>(state.visible.size()) - 1);
        }
    }

    static void reveal_target(TreeState &state) {
        if (state.options.highlight_target.empty()) {
            return;
        }

        std::error_code relative_error;
        const fs::path relative = fs::relative(state.options.highlight_target, state.options.root, relative_error);
        if (relative_error) {
            return;
        }

        fs::path current = state.options.root;
        for (const auto &part : relative) {
            current /= part;
            if (current == state.options.highlight_target) {
                break;
            }
            int index = find_entry_index(state, current);
            if (index >= 0) {
                state.visible[static_cast<std::size_t>(index)].is_expanded = true;
                state.visible[static_cast<std::size_t>(index)].icon = std::string(ICON_FOLDER_OPEN);
                rebuild_visible(state);
            }
        }

        const int index = find_entry_index(state, state.options.highlight_target);
        if (index >= 0) {
            state.cursor = index;
        }
    }

    static short color_pair_for(short fg, short bg) {
        static std::map<int, short> pair_cache;
        static short next_pair = 16;
        const int key = (fg + 1) * 512 + (bg + 1);
        auto it = pair_cache.find(key);
        if (it != pair_cache.end())
            return it->second;
        if (next_pair >= COLOR_PAIRS)
            return PAIR_DEFAULT;
        init_pair(next_pair, fg, bg);
        pair_cache[key] = next_pair;
        return next_pair++;
    }

    static attr_t attr_from_ansi(short fg, short bg, bool bold = false) {
        attr_t attr = has_colors() ? COLOR_PAIR(color_pair_for(fg, bg)) : A_NORMAL;
        if (bold)
            attr |= A_BOLD;
        return attr;
    }

    static short color_from_hex(const std::string &hex) {
        if (hex.size() != 7 || hex.front() != '#')
            return ANSI_TEXT;

        auto hex_byte = [&](int offset) { return std::stoi(hex.substr(offset, 2), nullptr, 16); };
        const int r = hex_byte(1);
        const int g = hex_byte(3);
        const int b = hex_byte(5);

        if (!can_change_color()) {
            const int rr = (r * 5 + 127) / 255;
            const int gg = (g * 5 + 127) / 255;
            const int bb = (b * 5 + 127) / 255;
            return static_cast<short>(16 + 36 * rr + 6 * gg + bb);
        }

        static std::map<std::string, short> color_cache;
        static short next_color = 16;
        auto it = color_cache.find(hex);
        if (it != color_cache.end())
            return it->second;
        if (next_color >= COLORS)
            return ANSI_TEXT;

        const short color_id = next_color++;
        init_color(color_id, static_cast<short>((r * 1000) / 255), static_cast<short>((g * 1000) / 255),
                   static_cast<short>((b * 1000) / 255));
        color_cache[hex] = color_id;
        return color_id;
    }

    static attr_t attr_from_hex(const std::string &hex, short bg, bool bold = false) {
        return attr_from_ansi(color_from_hex(hex), bg, bold);
    }

    static void draw_plain_line(int row, const std::string &line, attr_t attr) {
        move(row, 0);
        clrtoeol();
        attrset(attr);
        addnstr(line.c_str(), COLS);
        attrset(A_NORMAL);
    }

    static void draw_chunk(int row, int &col, const std::string &text, attr_t attr) {
        move(row, col);
        attrset(attr);
        addstr(text.c_str());
        col = getcurx(stdscr);
        attrset(A_NORMAL);
    }

    static void fill_line_bg(int row, short bg) {
        move(row, 0);
        attrset(has_colors() ? COLOR_PAIR(color_pair_for(-1, bg)) : A_NORMAL);
        for (int col = 0; col < COLS; ++col)
            addch(' ');
        attrset(A_NORMAL);
    }

    static void draw_entry_line(int row, const TreeState &state, const Entry &entry, bool is_cursor) {
        const short base_bg = (state.options.background_color >= 0 && COLORS > state.options.background_color)
                                  ? static_cast<short>(state.options.background_color)
                                  : -1;
        const short selected_bg =
            (state.options.selection_background_color >= 0 && COLORS > state.options.selection_background_color)
                ? static_cast<short>(state.options.selection_background_color)
                : COLOR_BLACK;
        const short bg = is_cursor ? selected_bg : base_bg;
        fill_line_bg(row, bg);
        int col = 0;

        draw_chunk(row, col, is_cursor ? "> " : "  ", attr_from_ansi(ANSI_WHITE, bg, is_cursor));
        draw_chunk(row, col, entry.is_readonly ? std::string(MARK_READONLY) : " ", attr_from_ansi(ANSI_ACCENT_RED, bg));
        draw_chunk(row, col, " ", attr_from_ansi(ANSI_WHITE, bg));

        if (entry.depth > 0) {
            std::size_t start = 0;
            if (state.options.max_depth >= 0 &&
                entry.ancestor_has_more.size() > static_cast<std::size_t>(state.options.max_depth)) {
                start = entry.ancestor_has_more.size() - static_cast<std::size_t>(state.options.max_depth);
            }
            for (std::size_t index = start; index < entry.ancestor_has_more.size(); ++index) {
                draw_chunk(row, col,
                           entry.ancestor_has_more[index] ? std::string(INDENT_PIPE) : std::string(INDENT_SPACE),
                           attr_from_ansi(ANSI_DIM, bg));
            }
            draw_chunk(row, col, entry.is_last ? std::string(INDENT_LAST) : std::string(INDENT_BRANCH),
                       attr_from_ansi(ANSI_DIM, bg));
        }

        if (state.options.show_git) {
            short git_color = ANSI_DIM;
            switch (entry.git) {
            case GitKind::Modified:
            case GitKind::Renamed:
                git_color = ANSI_GIT_WARN;
                break;
            case GitKind::Staged:
                git_color = ANSI_GIT_OK;
                break;
            case GitKind::Unmerged:
            case GitKind::Deleted:
                git_color = ANSI_GIT_BAD;
                break;
            default:
                break;
            }
            draw_chunk(row, col, git_glyph(entry.git), attr_from_ansi(git_color, bg));
            draw_chunk(row, col, " ", attr_from_ansi(ANSI_WHITE, bg));
        }

        const std::string file_hex = icon_color_for_name(entry.name);
        if (entry.kind == EntryKind::Directory)
            draw_chunk(row, col, entry.icon, attr_from_ansi(1, bg));
        else
            draw_chunk(row, col, entry.icon, attr_from_hex(file_hex, bg));
        draw_chunk(row, col, " ", attr_from_ansi(ANSI_WHITE, bg));

        std::string name = entry.name;
        if (entry.kind == EntryKind::Directory)
            name += '/';
        if (entry.kind == EntryKind::Directory)
            draw_chunk(row, col, name, attr_from_ansi(1, bg, is_cursor));
        else
            draw_chunk(row, col, name, attr_from_hex(file_hex, bg, is_cursor));

        if (state.options.show_size && entry.kind == EntryKind::File) {
            draw_chunk(row, col, "  " + std::to_string(entry.size), attr_from_ansi(ANSI_DIM, bg));
        }
    }

    static void render(const TreeState &state) {
        erase();

        int row = 0;
        if (state.options.show_header && row < LINES) {
            draw_plain_line(row++, "lis - tree.nvim-ish file browser", attr_from_ansi(ANSI_WHITE, -1, true));
            draw_plain_line(row++, "root: " + state.options.root.string(), attr_from_ansi(ANSI_TEXT, -1));
            draw_plain_line(row++, "j/k or arrows: move  h/l: collapse/expand  enter: select",
                            attr_from_ansi(ANSI_DIM, -1));
            draw_plain_line(row++, "g/G: top/bottom  q/esc/ctrl-c: quit", attr_from_ansi(ANSI_DIM, -1));
            if (row < LINES) {
                draw_plain_line(row++, "", A_NORMAL);
            }
        }

        for (std::size_t index = 0; index < state.visible.size() && row < LINES; ++index) {
            const bool is_cursor = static_cast<int>(index) == state.cursor;
            draw_entry_line(row++, state, state.visible[index], is_cursor);
        }

        while (row < LINES) {
            draw_plain_line(row++, "", A_NORMAL);
        }

        move(LINES - 1, COLS - 1);
        wnoutrefresh(stdscr);
        doupdate();
    }

    static void collapse_or_parent(TreeState &state) {
        if (state.visible.empty()) {
            return;
        }
        Entry &entry = state.visible[static_cast<std::size_t>(state.cursor)];
        const fs::path path = entry.path;
        if (entry.kind == EntryKind::Directory && entry.is_expanded && entry.depth > 0) {
            entry.is_expanded = false;
            entry.icon = std::string(ICON_FOLDER_CLOSED);
            rebuild_visible(state);
            state.cursor = std::max(0, find_entry_index(state, path));
        } else if (entry.depth > 0) {
            state.cursor = std::max(0, find_entry_index(state, path.parent_path()));
        }
    }

    static void expand_or_open(TreeState &state) {
        if (state.visible.empty()) {
            return;
        }
        Entry &entry = state.visible[static_cast<std::size_t>(state.cursor)];
        if (entry.kind != EntryKind::Directory) {
            return;
        }
        const fs::path path = entry.path;
        entry.is_expanded = true;
        entry.icon = std::string(ICON_FOLDER_OPEN);
        rebuild_visible(state);
        state.cursor = std::max(0, find_entry_index(state, path));
    }

    static void toggle_directory(TreeState &state) {
        if (state.visible.empty()) {
            return;
        }
        Entry &entry = state.visible[static_cast<std::size_t>(state.cursor)];
        if (entry.kind != EntryKind::Directory) {
            return;
        }
        const fs::path path = entry.path;
        entry.is_expanded = !entry.is_expanded;
        entry.icon = entry.is_expanded ? std::string(ICON_FOLDER_OPEN) : std::string(ICON_FOLDER_CLOSED);
        rebuild_visible(state);
        state.cursor = std::max(0, find_entry_index(state, path));
    }

    static void print_help() {
        std::cout << "lis - tree picker\n"
                  << "usage: lis [options] [path]\n\n"
                  << "options:\n"
                  << "  -a, --all                         show hidden files\n"
                  << "  -A, --alt-screen                  accepted for compatibility\n"
                  << "  -c, --compact                     hide header\n"
                  << "  -o, --output <file>               write selection to a file\n"
                  << "      --cwd <dir>                   root directory for the tree\n"
                  << "      --background <0-255>          background color\n"
                  << "      --selection-background <n>    selection line background\n"
                  << "      --git                         show git status glyphs\n"
                  << "      --size                        show file sizes\n"
                  << "      --generic-icons               use one icon for all files\n"
                  << "      --max-depth <n>               indentation depth cap\n"
                  << "      --help                        show this help\n"
                  << "      --version                     show version\n";
    }

    static bool parse_number(const std::string &text, int &value) {
        char *end = nullptr;
        errno = 0;
        const long parsed = std::strtol(text.c_str(), &end, 10);
        if (errno != 0 || end == text.c_str() || *end != '\0') {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    }

    static bool split_long_option(const std::string &argument, const std::string &name, std::string &value) {
        const std::string prefix = name + "=";
        if (argument.rfind(prefix, 0) != 0) {
            return false;
        }
        value = argument.substr(prefix.size());
        return true;
    }

    static bool read_value(int argc, char **argv, int &index, std::string &value) {
        if (index + 1 >= argc) {
            return false;
        }
        value = argv[++index];
        return true;
    }

    static std::optional<Options> parse_args(int argc, char **argv) {
        Options options;
        std::optional<std::string> path_argument;

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--help") {
                print_help();
                return std::nullopt;
            }
            if (argument == "--version") {
                std::cout << "0.3.0\n";
                return std::nullopt;
            }
            if (argument == "-a" || argument == "--all") {
                options.show_hidden = true;
                continue;
            }
            if (argument == "-A" || argument == "--alt-screen") {
                options.alt_screen = true;
                continue;
            }
            if (argument == "-c" || argument == "--compact") {
                options.show_header = false;
                continue;
            }
            if (argument == "--git") {
                options.show_git = true;
                continue;
            }
            if (argument == "--size") {
                options.show_size = true;
                continue;
            }
            if (argument == "--generic-icons") {
                options.generic_icons = true;
                continue;
            }
            if (argument == "-o" || argument == "--output") {
                if (!read_value(argc, argv, index, options.output_file)) {
                    std::cerr << "error: missing value for " << argument << "\n";
                    return std::nullopt;
                }
                continue;
            }
            if (split_long_option(argument, "--output", options.output_file)) {
                continue;
            }

            auto read_path_option = [&](const std::string &name, fs::path &target) -> bool {
                std::string value;
                if (argument == name) {
                    if (!read_value(argc, argv, index, value)) {
                        std::cerr << "error: missing value for " << name << "\n";
                        return false;
                    }
                    target = fs::absolute(value);
                    return true;
                }
                if (split_long_option(argument, name, value)) {
                    target = fs::absolute(value);
                    return true;
                }
                return false;
            };

            auto read_int_option = [&](const std::string &name, int &target) -> int {
                std::string value;
                if (argument == name) {
                    if (!read_value(argc, argv, index, value)) {
                        return -1;
                    }
                } else if (!split_long_option(argument, name, value)) {
                    return 0;
                }
                return parse_number(value, target) ? 1 : -1;
            };

            if (read_path_option("--cwd", options.root)) {
                continue;
            }

            const int background = read_int_option("--background", options.background_color);
            if (background == 1) {
                continue;
            }
            if (background == -1) {
                std::cerr << "error: invalid value for --background\n";
                return std::nullopt;
            }

            const int selection = read_int_option("--selection-background", options.selection_background_color);
            if (selection == 1) {
                continue;
            }
            if (selection == -1) {
                std::cerr << "error: invalid value for --selection-background\n";
                return std::nullopt;
            }

            const int max_depth = read_int_option("--max-depth", options.max_depth);
            if (max_depth == 1) {
                continue;
            }
            if (max_depth == -1) {
                std::cerr << "error: invalid value for --max-depth\n";
                return std::nullopt;
            }

            if (argument.size() > 2 && argument.front() == '-' && argument[1] != '-') {
                bool valid = true;
                for (std::size_t short_index = 1; short_index < argument.size(); ++short_index) {
                    switch (argument[short_index]) {
                    case 'a':
                        options.show_hidden = true;
                        break;
                    case 'A':
                        options.alt_screen = true;
                        break;
                    case 'c':
                        options.show_header = false;
                        break;
                    default:
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    std::cerr << "error: unknown option " << argument << "\n";
                    return std::nullopt;
                }
                continue;
            }

            if (!argument.empty() && argument.front() == '-') {
                std::cerr << "error: unknown option " << argument << "\n";
                return std::nullopt;
            }
            path_argument = argument;
        }

        if (options.root.empty()) {
            fs::path input = path_argument.has_value() ? fs::absolute(*path_argument) : fs::current_path();
            if (!fs::exists(input)) {
                std::cerr << "error: path does not exist: " << input.string() << "\n";
                return std::nullopt;
            }
            if (fs::is_directory(input)) {
                options.root = input;
            } else {
                options.root = input.parent_path();
                options.highlight_target = input;
            }
        } else if (path_argument.has_value()) {
            options.highlight_target = fs::absolute(*path_argument);
        }

        if (!fs::exists(options.root) || !fs::is_directory(options.root)) {
            std::cerr << "error: cwd must be an existing directory\n";
            return std::nullopt;
        }
        return options;
    }

    static std::optional<fs::path> run_tree(TreeState &state) {
        CursesSession session(state.options);
        refresh_git_status(state);
        rebuild_visible(state);
        reveal_target(state);
        render(state);

        for (;;) {
            const int key = getch();
            switch (key) {
            case KEY_UP:
            case 'k':
            case 'K':
                if (state.cursor > 0) {
                    --state.cursor;
                }
                break;
            case KEY_DOWN:
            case 'j':
            case 'J':
                if (state.cursor + 1 < static_cast<int>(state.visible.size())) {
                    ++state.cursor;
                }
                break;
            case KEY_LEFT:
            case 'h':
            case 'H':
            case KEY_BACKSPACE:
            case 127:
            case 8:
                collapse_or_parent(state);
                break;
            case KEY_RIGHT:
            case 'l':
            case 'L':
                expand_or_open(state);
                break;
            case '\n':
            case '\r':
            case KEY_ENTER:
                if (state.visible.empty()) {
                    break;
                }
                if (state.visible[static_cast<std::size_t>(state.cursor)].kind == EntryKind::Directory) {
                    toggle_directory(state);
                    break;
                }
                return state.visible[static_cast<std::size_t>(state.cursor)].path.filename();
            case 'g':
                state.cursor = 0;
                break;
            case 'G':
                if (!state.visible.empty()) {
                    state.cursor = static_cast<int>(state.visible.size()) - 1;
                }
                break;
            case 'q':
            case 'Q':
            case 27:
            case 3:
                return std::nullopt;
            default:
                break;
            }
            render(state);
        }
    }

} // namespace

int main(int argc, char **argv) {
    const std::optional<Options> options = parse_args(argc, argv);
    if (!options.has_value()) {
        return 0;
    }

    TreeState state;
    state.options = *options;

    const std::optional<fs::path> selected = run_tree(state);
    if (!selected.has_value()) {
        return 0;
    }

    if (!options->output_file.empty()) {
        std::ofstream stream(options->output_file);
        stream << selected->string() << '\n';
    }
    std::cout << selected->string() << '\n';
    return 0;
}
