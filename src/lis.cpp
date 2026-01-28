#include <argu/argu.hpp>
#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <echo/format.hpp>
#include <scan/input/reader.hpp>
#include <scan/terminal/raw_mode.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

    // =============================================================================================
    // Constants - tree.nvim style glyphs
    // =============================================================================================

    constexpr const char *INDENT_PIPE = "│ ";
    constexpr const char *INDENT_BRANCH = "├ ";
    constexpr const char *INDENT_LAST = "└ ";
    constexpr const char *INDENT_SPACE = "  ";

    constexpr const char *GIT_UNTRACKED = "✭";
    constexpr const char *GIT_MODIFIED = "✹";
    constexpr const char *GIT_STAGED = "✚";
    constexpr const char *GIT_RENAMED = "➜";
    constexpr const char *GIT_IGNORED = "☒";
    constexpr const char *GIT_UNMERGED = "═";
    constexpr const char *GIT_DELETED = "✖";
    constexpr const char *GIT_UNKNOWN = "?";

    constexpr const char *MARK_SELECTED = "✓";
    constexpr const char *MARK_READONLY = "✗";

    constexpr const char *ICON_FOLDER_CLOSED = "\xee\x97\xbf";
    constexpr const char *ICON_FOLDER_OPEN = "\xee\x97\xbe";
    constexpr const char *ICON_FOLDER_SYMLINK = "\xef\x92\x82";
    constexpr const char *ICON_FILE_DEFAULT = "\xee\x98\x92";
    constexpr const char *ICON_FILE_SYMLINK = "\xef\x92\x81";

    // =============================================================================================
    // Enums
    // =============================================================================================

    enum class EntryKind : dp::u8 { Directory, File, Symlink };

    enum class GitKind : dp::u8 { Untracked, Modified, Staged, Renamed, Ignored, Unmerged, Deleted, Unknown, None };

    enum class SortKind : dp::u8 { Name, Extension, Size, Time, NameRev, ExtensionRev, SizeRev, TimeRev };

    // =============================================================================================
    // Entry - represents a file/directory in the tree
    // =============================================================================================

    struct Entry {
        dp::String name;
        fs::path path;
        EntryKind kind{};
        GitKind git{};
        bool is_hidden{};
        bool is_readonly{};
        bool is_selected{};

        dp::u16 depth{};
        bool is_last{};
        dp::Vector<bool> ancestor_has_more;

        bool is_expanded{};
        dp::String icon;

        // File metadata
        dp::u64 size{};
        std::time_t mtime{};
        dp::String extension;
    };

    // =============================================================================================
    // Clipboard - for copy/move/paste
    // =============================================================================================

    struct Clipboard {
        dp::Vector<fs::path> paths;
        bool is_cut = false; // false = copy, true = move
    };

    // =============================================================================================
    // TreeState - global state
    // =============================================================================================

    struct TreeState {
        fs::path root;
        dp::Vector<Entry> visible;
        dp::i32 cursor = 0;

        // Display options
        bool show_hidden = false;
        bool show_git = false;
        bool show_size = false;
        bool show_time = false;
        bool show_mark = true;
        bool show_header = true;
        bool use_ansi = true;
        bool alt_screen = false;
        bool generic_icons = false; // Use same icon for all files
        dp::i32 max_depth = -1;     // -1 = unlimited
        dp::i32 bg_color = -1;      // -1 = default, 0-255 = ANSI 256-color for terminal bg
        dp::i32 sel_bg_color = -1;  // -1 = default, 0-255 = ANSI 256-color for selection bg

        // Sorting
        SortKind sort = SortKind::Name;

        // Selection
        std::set<fs::path> selected;

        // Clipboard
        Clipboard clipboard;

        // Git status cache
        std::map<fs::path, GitKind> git_status;
        fs::path git_root;

        // Message to display
        dp::String message;

        // Initial highlight target
        fs::path highlight_target;
    };

    // =============================================================================================
    // Utility functions
    // =============================================================================================

    static dp::String path_to_string(const fs::path &p) { return dp::String(p.string().c_str()); }

    static int get_terminal_width() {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
            return w.ws_col;
        }
        return 80; // fallback
    }

    // Calculate visible width (excluding ANSI escape codes)
    static int visible_width(const std::string &s) {
        int width = 0;
        bool in_escape = false;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\x1b') {
                in_escape = true;
            } else if (in_escape) {
                if (s[i] == 'm')
                    in_escape = false;
            } else {
                // Handle UTF-8: count codepoints, not bytes
                unsigned char c = static_cast<unsigned char>(s[i]);
                if ((c & 0xC0) != 0x80) { // Not a continuation byte
                    ++width;
                }
            }
        }
        return width;
    }

    // Replace all ANSI resets with reset + background color
    static std::string apply_persistent_bg(const std::string &s, int bg_color) {
        if (bg_color < 0)
            return s;
        std::string bg_code = "\x1b[48;5;" + std::to_string(bg_color) + "m";
        std::string reset = "\x1b[0m";
        std::string result;
        result.reserve(s.size() * 2);

        size_t pos = 0;
        size_t found;
        while ((found = s.find(reset, pos)) != std::string::npos) {
            result.append(s, pos, found - pos);
            result.append(reset);
            result.append(bg_code);
            pos = found + reset.size();
        }
        result.append(s, pos, s.size() - pos);
        return result;
    }

    static bool is_hidden_name(const dp::String &name) { return !name.empty() && name[0] == '.'; }

    static dp::String format_size(dp::u64 bytes) {
        const char *units[] = {"B", "K", "M", "G", "T"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024.0 && unit < 4) {
            size /= 1024.0;
            unit++;
        }
        std::ostringstream oss;
        if (unit == 0) {
            oss << bytes << units[unit];
        } else {
            oss << std::fixed << std::setprecision(1) << size << units[unit];
        }
        return dp::String(oss.str().c_str());
    }

    static dp::String format_time(std::time_t t) {
        std::tm *tm = std::localtime(&t);
        std::ostringstream oss;
        oss << std::put_time(tm, "%b %d %H:%M");
        return dp::String(oss.str().c_str());
    }

    // =============================================================================================
    // Git status detection
    // =============================================================================================

    static fs::path find_git_root(const fs::path &start) {
        fs::path current = start;
        while (!current.empty() && current != current.root_path()) {
            if (fs::exists(current / ".git")) {
                return current;
            }
            current = current.parent_path();
        }
        return {};
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

    static void refresh_git_status(TreeState &s) {
        s.git_status.clear();
        s.git_root = find_git_root(s.root);
        if (s.git_root.empty())
            return;

        // Run git status --porcelain
        std::string cmd = "cd \"" + s.git_root.string() + "\" && git status --porcelain -uall 2>/dev/null";
        FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            return;

        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            std::string line(buffer);
            if (line.size() < 4)
                continue;
            char x = line[0];
            char y = line[1];
            std::string path_str = line.substr(3);
            // Remove trailing newline
            while (!path_str.empty() && (path_str.back() == '\n' || path_str.back() == '\r')) {
                path_str.pop_back();
            }
            fs::path full_path = s.git_root / path_str;
            s.git_status[fs::weakly_canonical(full_path)] = classify_git(x, y);
        }
        pclose(pipe);
    }

    // =============================================================================================
    // Icons
    // =============================================================================================

    static dp::String git_glyph(GitKind g) {
        switch (g) {
        case GitKind::Untracked:
            return GIT_UNTRACKED;
        case GitKind::Modified:
            return GIT_MODIFIED;
        case GitKind::Staged:
            return GIT_STAGED;
        case GitKind::Renamed:
            return GIT_RENAMED;
        case GitKind::Ignored:
            return GIT_IGNORED;
        case GitKind::Unmerged:
            return GIT_UNMERGED;
        case GitKind::Deleted:
            return GIT_DELETED;
        case GitKind::Unknown:
            return GIT_UNKNOWN;
        default:
            return " ";
        }
    }

    static echo::format::String git_styled_ansi(GitKind g) {
        switch (g) {
        case GitKind::Modified:
        case GitKind::Renamed:
            return echo::format::String(git_glyph(g).c_str()).fg("#fabd2f");
        case GitKind::Staged:
            return echo::format::String(git_glyph(g).c_str()).fg("#b8bb26");
        case GitKind::Unmerged:
        case GitKind::Deleted:
            return echo::format::String(git_glyph(g).c_str()).fg("#fb4934");
        case GitKind::Untracked:
        case GitKind::Ignored:
        case GitKind::Unknown:
            return echo::format::String(git_glyph(g).c_str()).fg("#928374");
        default:
            return echo::format::String(" ");
        }
    }

    struct IconDef {
        const char *icon;
        const char *color;
    };

    static const std::map<dp::String, IconDef> &get_icon_map() {
        static const std::map<dp::String, IconDef> icons = {
            // C/C++
            {"c", {"\xee\x98\x9e", "#599EFF"}},
            {"cpp", {"\xee\x98\x9d", "#519ABA"}},
            {"cc", {"\xee\x98\x9d", "#F34B7D"}},
            {"cxx", {"\xee\x98\x9d", "#519ABA"}},
            {"h", {"\xef\x83\xbd", "#A074C4"}},
            {"hpp", {"\xef\x83\xbd", "#A074C4"}},
            {"hxx", {"\xef\x83\xbd", "#A074C4"}},
            {"hh", {"\xef\x83\xbd", "#A074C4"}},
            // Rust
            {"rs", {"\xee\x9a\x8b", "#DEA584"}},
            // Python
            {"py", {"\xee\x98\x86", "#FFBC03"}},
            {"pyi", {"\xee\x98\x86", "#FFBC03"}},
            {"pyc", {"\xee\x98\x86", "#FFE873"}},
            {"pyw", {"\xee\x98\x86", "#FFBC03"}},
            // Lua
            {"lua", {"\xee\x98\xa0", "#51A0CF"}},
            {"luau", {"\xee\x98\xa0", "#00A2FF"}},
            // JavaScript/TypeScript
            {"js", {"\xee\x98\x8c", "#CBCB41"}},
            {"mjs", {"\xee\x98\x8c", "#F1E05A"}},
            {"cjs", {"\xee\x98\x8c", "#CBCB41"}},
            {"ts", {"\xee\x98\xa8", "#519ABA"}},
            {"mts", {"\xee\x98\xa8", "#519ABA"}},
            {"cts", {"\xee\x98\xa8", "#519ABA"}},
            {"jsx", {"\xee\x98\xa5", "#20C2E3"}},
            {"tsx", {"\xee\x9e\xba", "#1354BF"}},
            {"d.ts", {"\xee\x98\xa8", "#D59855"}},
            // Web
            {"html", {"\xee\x9c\xb6", "#E44D26"}},
            {"htm", {"\xee\x98\x8e", "#E34C26"}},
            {"css", {"\xee\x9a\xb8", "#663399"}},
            {"scss", {"\xee\x98\x83", "#F55385"}},
            {"sass", {"\xee\x98\x83", "#F55385"}},
            {"less", {"\xee\x98\x94", "#563D7C"}},
            {"vue", {"\xee\x9a\xa0", "#8DC149"}},
            {"svelte", {"\xee\x9a\x97", "#FF3E00"}},
            {"astro", {"\xee\x9a\xb3", "#E23F67"}},
            // Data formats
            {"json", {"\xee\x98\x8b", "#CBCB41"}},
            {"jsonc", {"\xee\x98\x8b", "#CBCB41"}},
            {"json5", {"\xee\x98\x8b", "#CBCB41"}},
            {"yaml", {"\xee\x98\x95", "#6D8086"}},
            {"yml", {"\xee\x98\x95", "#6D8086"}},
            {"toml", {"\xee\x9a\xb2", "#9C4221"}},
            {"xml", {"\xf3\xb0\x97\x80", "#E37933"}},
            {"csv", {"\xee\x99\x8a", "#89E051"}},
            // Shell
            {"sh", {"\xee\x9e\x95", "#4D5A5E"}},
            {"bash", {"\xee\x9d\xa0", "#89E051"}},
            {"zsh", {"\xee\x9e\x95", "#89E051"}},
            {"fish", {"\xee\x9e\x95", "#4D5A5E"}},
            {"ps1", {"\xee\x98\x95", "#012456"}},
            {"bat", {"\xee\x98\x95", "#C1F12E"}},
            {"cmd", {"\xee\x98\x95", "#C1F12E"}},
            {"awk", {"\xee\x9e\x95", "#4D5A5E"}},
            // Go
            {"go", {"\xee\x98\xa7", "#00ADD8"}},
            {"mod", {"\xee\x98\xa7", "#00ADD8"}},
            {"sum", {"\xee\x98\xa7", "#00ADD8"}},
            // Java/JVM
            {"java", {"\xee\x9c\xb8", "#CC3E44"}},
            {"jar", {"\xee\x9c\xb8", "#CC3E44"}},
            {"class", {"\xee\x9c\xb8", "#CC3E44"}},
            {"kt", {"\xee\x98\xb4", "#7F52FF"}},
            {"kts", {"\xee\x98\xb4", "#7F52FF"}},
            {"scala", {"\xee\x98\xb7", "#CC3E44"}},
            {"groovy", {"\xee\x98\xb7", "#4298B8"}},
            {"gradle", {"\xee\x99\xa0", "#005F87"}},
            // .NET
            {"cs", {"\xf3\xb0\x8c\x9b", "#596706"}},
            {"csx", {"\xf3\xb0\x8c\x9b", "#596706"}},
            {"fs", {"\xee\x9e\xa7", "#519ABA"}},
            {"fsx", {"\xee\x9e\xa7", "#519ABA"}},
            {"vb", {"\xee\x98\x97", "#945DB7"}},
            {"sln", {"\xee\x98\x97", "#854CC7"}},
            {"csproj", {"\xf3\xb0\xaa\xae", "#512BD4"}},
            // Ruby
            {"rb", {"\xee\x9e\x91", "#701516"}},
            {"erb", {"\xee\x98\x8e", "#701516"}},
            {"rake", {"\xee\x9e\x91", "#701516"}},
            {"gemspec", {"\xee\x9e\x91", "#701516"}},
            // PHP
            {"php", {"\xee\x98\x88", "#A074C4"}},
            {"phtml", {"\xee\x98\x88", "#A074C4"}},
            // Swift/Apple
            {"swift", {"\xee\x9d\x95", "#E37933"}},
            {"m", {"\xee\x98\x9e", "#599EFF"}},
            {"mm", {"\xee\x98\x9d", "#519ABA"}},
            // Zig/Nim
            {"zig", {"\xee\x9a\xa9", "#F69A1B"}},
            {"nim", {"\xee\x99\xb7", "#F3D400"}},
            // Functional
            {"hs", {"\xee\x98\x9f", "#A074C4"}},
            {"lhs", {"\xee\x98\x9f", "#A074C4"}},
            {"ml", {"\xee\x99\xba", "#E37933"}},
            {"mli", {"\xee\x99\xba", "#E37933"}},
            {"ex", {"\xee\x98\xad", "#A074C4"}},
            {"exs", {"\xee\x98\xad", "#A074C4"}},
            {"erl", {"\xee\x9e\xb1", "#B83998"}},
            {"hrl", {"\xee\x9e\xb1", "#B83998"}},
            {"clj", {"\xee\x9d\xa8", "#8DC149"}},
            {"cljs", {"\xee\x9d\xaa", "#519ABA"}},
            {"cljc", {"\xee\x9d\xa8", "#8DC149"}},
            {"el", {"\xee\x98\xb2", "#8172BE"}},
            {"elm", {"\xee\x98\xac", "#519ABA"}},
            // Data science
            {"r", {"\xf3\xb0\x9f\x94", "#2266BA"}},
            {"rmd", {"\xf3\xb0\x9f\x94", "#2266BA"}},
            {"jl", {"\xee\x98\xa4", "#A270BA"}},
            {"ipynb", {"\xee\xa0\x8f", "#F57D01"}},
            // Mobile
            {"dart", {"\xee\x9e\x98", "#03589C"}},
            // Database
            {"sql", {"\xee\x9c\x86", "#DAD8D8"}},
            {"sqlite", {"\xee\x9c\x86", "#DAD8D8"}},
            {"db", {"\xee\x9c\x86", "#DAD8D8"}},
            {"graphql", {"\xef\x88\x8e", "#E535AB"}},
            {"gql", {"\xef\x88\x8e", "#E535AB"}},
            {"prisma", {"\xee\x98\x8b", "#0C344B"}},
            // DevOps/Config
            {"dockerfile", {"\xf3\xb0\xa1\xa8", "#458EE6"}},
            {"dockerignore", {"\xf3\xb0\xa1\xa8", "#458EE6"}},
            {"nix", {"\xef\x8c\x93", "#7EBAE4"}},
            {"tf", {"\xee\x98\x97", "#5C4EE5"}},
            {"tfvars", {"\xee\x98\x97", "#5C4EE5"}},
            {"hcl", {"\xee\x98\x97", "#5C4EE5"}},
            // Build/Make
            {"makefile", {"\xee\x9d\xb9", "#6D8086"}},
            {"gnumakefile", {"\xee\x9d\xb9", "#6D8086"}},
            {"cmake", {"\xee\x9e\x94", "#DCE3EB"}},
            {"meson", {"\xee\x98\x97", "#6D8086"}},
            // Docs
            {"md", {"\xef\x92\x8a", "#DDDDDD"}},
            {"markdown", {"\xee\x98\x89", "#DDDDDD"}},
            {"mdx", {"\xef\x92\x8a", "#519ABA"}},
            {"rst", {"\xef\x92\x8a", "#DDDDDD"}},
            {"txt", {"\xef\x85\x9c", "#89E051"}},
            {"org", {"\xee\x98\xb3", "#77AA99"}},
            {"tex", {"\xee\x98\x97", "#3D6117"}},
            {"bib", {"\xf3\xb1\x89\x9f", "#CBCB41"}},
            // Git
            {"git", {"\xee\x9c\x82", "#F14C28"}},
            {"gitignore", {"\xee\x9c\x82", "#F14C28"}},
            {"gitmodules", {"\xee\x9c\x82", "#F14C28"}},
            {"gitattributes", {"\xee\x9c\x82", "#F14C28"}},
            // Editor
            {"vim", {"\xee\x98\xab", "#019833"}},
            {"nvim", {"\xee\x98\xab", "#019833"}},
            {"vimrc", {"\xee\x98\xab", "#019833"}},
            {"editorconfig", {"\xee\x98\x8b", "#FFFFFF"}},
            // Archives
            {"zip", {"\xef\x90\x90", "#ECA517"}},
            {"tar", {"\xef\x90\x90", "#ECA517"}},
            {"gz", {"\xef\x90\x90", "#ECA517"}},
            {"xz", {"\xef\x90\x90", "#ECA517"}},
            {"bz2", {"\xef\x90\x90", "#ECA517"}},
            {"7z", {"\xef\x90\x90", "#ECA517"}},
            {"rar", {"\xef\x90\x90", "#ECA517"}},
            {"deb", {"\xef\x90\x90", "#A80030"}},
            {"rpm", {"\xef\x90\x90", "#EE0000"}},
            // Images
            {"png", {"\xee\x98\x8d", "#A074C4"}},
            {"jpg", {"\xee\x98\x8d", "#A074C4"}},
            {"jpeg", {"\xee\x98\x8d", "#A074C4"}},
            {"gif", {"\xee\x98\x8d", "#A074C4"}},
            {"bmp", {"\xee\x98\x8d", "#A074C4"}},
            {"ico", {"\xee\x98\x8d", "#CBCB41"}},
            {"webp", {"\xee\x98\x8d", "#A074C4"}},
            {"svg", {"\xef\x86\xb2", "#FFB13B"}},
            {"avif", {"\xee\x98\x8d", "#A074C4"}},
            // Audio/Video
            {"mp3", {"\xef\x80\x81", "#00AFFF"}},
            {"wav", {"\xef\x80\x81", "#00AFFF"}},
            {"flac", {"\xef\x80\x81", "#0075AA"}},
            {"ogg", {"\xef\x80\x81", "#0075AA"}},
            {"aac", {"\xef\x80\x81", "#00AFFF"}},
            {"mp4", {"\xee\x9a\x9f", "#FD971F"}},
            {"mkv", {"\xee\x9a\x9f", "#FD971F"}},
            {"avi", {"\xee\x9a\x9f", "#FD971F"}},
            {"mov", {"\xee\x9a\x9f", "#FD971F"}},
            {"webm", {"\xee\x9a\x9f", "#FD971F"}},
            // Fonts
            {"ttf", {"\xef\x80\xb1", "#ECECEC"}},
            {"otf", {"\xef\x80\xb1", "#ECECEC"}},
            {"woff", {"\xef\x80\xb1", "#ECECEC"}},
            {"woff2", {"\xef\x80\xb1", "#ECECEC"}},
            // Documents
            {"pdf", {"\xee\x98\x87", "#B30B00"}},
            {"doc", {"\xf3\xb0\x88\xac", "#185ABD"}},
            {"docx", {"\xf3\xb0\x88\xac", "#185ABD"}},
            {"xls", {"\xef\x8d\xb8", "#207245"}},
            {"xlsx", {"\xef\x8d\xb8", "#207245"}},
            {"ppt", {"\xef\x8d\xba", "#CB4A32"}},
            {"pptx", {"\xef\x8d\xba", "#CB4A32"}},
            {"odt", {"\xef\x8d\xbc", "#2DCBFD"}},
            {"ods", {"\xef\x8d\xb8", "#78FC4E"}},
            {"odp", {"\xef\x8d\xba", "#FE9C45"}},
            // Misc
            {"lock", {"\xee\x99\xb2", "#BBBBBB"}},
            {"log", {"\xf3\xb0\x8c\xb1", "#DDDDDD"}},
            {"env", {"\xef\x91\xa2", "#FAF743"}},
            {"conf", {"\xee\x98\x95", "#6D8086"}},
            {"cfg", {"\xee\x98\x95", "#6D8086"}},
            {"ini", {"\xee\x98\x95", "#6D8086"}},
            {"license", {"\xee\x98\x8a", "#CBCB41"}},
            {"readme", {"\xef\x92\x8a", "#DDDDDD"}},
            // Additional common
            {"asm", {"\xee\x98\xb7", "#0091BD"}},
            {"s", {"\xee\x98\xb7", "#0091BD"}},
            {"cr", {"\xee\x98\xaf", "#C8C8C8"}},
            {"coffee", {"\xee\x98\x9b", "#CBCB41"}},
            {"diff", {"\xee\x9c\xa8", "#41535B"}},
            {"patch", {"\xee\x9c\xa8", "#41535B"}},
            {"d", {"\xee\x9e\xaf", "#B03931"}},
            {"ada", {"\xee\x9a\xb5", "#599EFF"}},
            {"adb", {"\xee\x9a\xb5", "#599EFF"}},
            {"ads", {"\xee\x9a\xb5", "#A074C4"}},
            {"hbs", {"\xee\x98\x8f", "#F0772B"}},
            {"mustache", {"\xee\x98\x8f", "#E37933"}},
            {"ejs", {"\xee\x98\x8e", "#CBCB41"}},
            {"haml", {"\xee\x98\x8e", "#EAEAE1"}},
            {"pug", {"\xee\x98\x8e", "#A86454"}},
            {"hx", {"\xee\x99\xa6", "#EA8220"}},
            {"gleam", {"\xef\x80\x85", "#FFAFF3"}},
            {"odin", {"\xf3\xb0\x9f\xa2", "#3882D2"}},
            {"v", {"\xee\x98\x97", "#5D87BF"}},
            {"vert", {"\xee\xa1\x95", "#5586A6"}},
            {"frag", {"\xee\xa1\x95", "#5586A6"}},
            {"glsl", {"\xee\xa1\x95", "#5586A6"}},
            {"wgsl", {"\xee\xa1\x95", "#5586A6"}},
            {"cu", {"\xee\x99\x8b", "#89E051"}},
            {"cuh", {"\xee\x99\x8b", "#A074C4"}},
        };
        return icons;
    }

    static dp::String file_icon_for(const dp::String &name, bool is_symlink) {
        if (is_symlink)
            return ICON_FILE_SYMLINK;

        // Check full filename first (for files like Makefile, Dockerfile)
        dp::String lower_name;
        for (char c : name)
            lower_name += static_cast<char>(std::tolower(c));

        const auto &icons = get_icon_map();
        auto it = icons.find(lower_name);
        if (it != icons.end())
            return it->second.icon;

        // Check extension
        auto dot = name.rfind('.');
        if (dot == dp::String::npos)
            return ICON_FILE_DEFAULT;

        dp::String ext;
        for (size_t i = dot + 1; i < name.size(); i++) {
            ext += static_cast<char>(std::tolower(name[i]));
        }

        it = icons.find(ext);
        if (it != icons.end())
            return it->second.icon;

        return ICON_FILE_DEFAULT;
    }

    static dp::String file_icon_color(const dp::String &name) {
        dp::String lower_name;
        for (char c : name)
            lower_name += static_cast<char>(std::tolower(c));

        const auto &icons = get_icon_map();
        auto it = icons.find(lower_name);
        if (it != icons.end())
            return it->second.color;

        auto dot = name.rfind('.');
        if (dot == dp::String::npos)
            return "#999999";

        dp::String ext;
        for (size_t i = dot + 1; i < name.size(); i++) {
            ext += static_cast<char>(std::tolower(name[i]));
        }

        it = icons.find(ext);
        if (it != icons.end())
            return it->second.color;

        return "#999999";
    }

    static echo::format::String icon_styled_ansi(const dp::String &icon, const Entry &e) {
        if (e.kind == EntryKind::Directory || (e.kind == EntryKind::Symlink && fs::is_directory(e.path))) {
            return echo::format::String(icon.c_str()).fg("#00afaf");
        }
        return echo::format::String(icon.c_str()).fg(file_icon_color(e.name).c_str());
    }

    static echo::format::String filename_styled_ansi(const Entry &e, bool is_cursor) {
        auto base = echo::format::String(e.name.c_str());
        if (e.kind == EntryKind::Directory)
            base = base.fg("#689FB6");
        else if (e.is_selected)
            base = base.fg("#b8bb26");
        else
            base = base.fg("#F09F17");
        if (is_cursor)
            base = base.bold();
        return base;
    }

    // =============================================================================================
    // Directory listing with sorting
    // =============================================================================================

    static dp::Result<dp::Vector<Entry>, dp::String> list_dir_entries(const fs::path &dir, dp::u16 depth,
                                                                      TreeState &state) {
        dp::Vector<Entry> out;
        try {
            dp::Vector<Entry> dirs;
            dp::Vector<Entry> files;

            for (const auto &it : fs::directory_iterator(dir)) {
                fs::path p = it.path();
                dp::String name = dp::String(p.filename().string().c_str());
                bool hidden = is_hidden_name(name);
                if (hidden && !state.show_hidden)
                    continue;

                Entry e;
                e.name = std::move(name);
                e.path = p;
                e.depth = depth;
                e.is_hidden = hidden;
                e.is_expanded = false;
                e.is_selected = state.selected.count(fs::weakly_canonical(p)) > 0;

                // Get git status
                auto git_it = state.git_status.find(fs::weakly_canonical(p));
                e.git = (git_it != state.git_status.end()) ? git_it->second : GitKind::None;

                // Get file metadata
                try {
                    auto status = fs::status(p);
                    e.is_readonly = (status.permissions() & fs::perms::owner_write) == fs::perms::none;
                    if (fs::is_regular_file(status)) {
                        e.size = fs::file_size(p);
                    }
                    e.mtime =
                        std::chrono::system_clock::to_time_t(std::chrono::file_clock::to_sys(fs::last_write_time(p)));
                } catch (...) {
                }

                // Get extension
                auto dot = e.name.rfind('.');
                if (dot != dp::String::npos) {
                    e.extension = e.name.substr(dot + 1);
                }

                if (fs::is_symlink(it.symlink_status())) {
                    e.kind = EntryKind::Symlink;
                    try {
                        if (fs::is_directory(p)) {
                            e.kind = EntryKind::Directory;
                            e.icon = ICON_FOLDER_SYMLINK;
                            dirs.push_back(std::move(e));
                        } else {
                            e.kind = EntryKind::File;
                            e.icon = state.generic_icons ? ICON_FILE_DEFAULT : file_icon_for(e.name, true);
                            files.push_back(std::move(e));
                        }
                    } catch (...) {
                        e.kind = EntryKind::File;
                        e.icon = state.generic_icons ? ICON_FILE_DEFAULT : ICON_FILE_SYMLINK;
                        files.push_back(std::move(e));
                    }
                } else if (fs::is_directory(it.status())) {
                    e.kind = EntryKind::Directory;
                    e.icon = ICON_FOLDER_CLOSED;
                    dirs.push_back(std::move(e));
                } else {
                    e.kind = EntryKind::File;
                    e.icon = state.generic_icons ? ICON_FILE_DEFAULT : file_icon_for(e.name, false);
                    files.push_back(std::move(e));
                }
            }

            // Sort based on current sort mode
            auto sorter = [&state](const Entry &a, const Entry &b) -> bool {
                switch (state.sort) {
                case SortKind::Name:
                    return a.name < b.name;
                case SortKind::NameRev:
                    return a.name > b.name;
                case SortKind::Extension:
                    return a.extension < b.extension;
                case SortKind::ExtensionRev:
                    return a.extension > b.extension;
                case SortKind::Size:
                    return a.size < b.size;
                case SortKind::SizeRev:
                    return a.size > b.size;
                case SortKind::Time:
                    return a.mtime < b.mtime;
                case SortKind::TimeRev:
                    return a.mtime > b.mtime;
                default:
                    return a.name < b.name;
                }
            };

            std::sort(dirs.begin(), dirs.end(), sorter);
            std::sort(files.begin(), files.end(), sorter);

            for (auto &d : dirs)
                out.push_back(std::move(d));
            for (auto &f : files)
                out.push_back(std::move(f));
        } catch (const fs::filesystem_error &ex) {
            return dp::result::Err(dp::String(ex.what()));
        }

        return dp::result::Ok(std::move(out));
    }

    // =============================================================================================
    // Tree building
    // =============================================================================================

    static dp::i32 find_entry_index(const TreeState &s, const fs::path &p) {
        auto canon = fs::weakly_canonical(p);
        for (dp::i32 i = 0; i < static_cast<dp::i32>(s.visible.size()); ++i) {
            if (fs::weakly_canonical(s.visible[static_cast<dp::usize>(i)].path) == canon)
                return i;
        }
        return -1;
    }

    static void rebuild_visible(TreeState &s) {
        dp::Vector<fs::path> expanded;
        for (const auto &e : s.visible) {
            if (e.kind == EntryKind::Directory && e.is_expanded) {
                expanded.push_back(fs::weakly_canonical(e.path));
            }
        }

        s.visible.clear();

        Entry root;
        root.name = dp::String(s.root.filename().string().empty() ? s.root.string().c_str()
                                                                  : s.root.filename().string().c_str());
        root.path = s.root;
        root.kind = EntryKind::Directory;
        root.git = GitKind::None;
        root.is_hidden = false;
        root.depth = 0;
        root.is_last = true;
        root.is_expanded = true;
        root.icon = ICON_FOLDER_OPEN;
        root.is_selected = s.selected.count(fs::weakly_canonical(s.root)) > 0;
        s.visible.push_back(std::move(root));

        for (dp::i32 i = 0; i < static_cast<dp::i32>(s.visible.size()); ++i) {
            auto &e = s.visible[static_cast<dp::usize>(i)];
            if (e.kind != EntryKind::Directory || !e.is_expanded)
                continue;

            auto children_res = list_dir_entries(e.path, static_cast<dp::u16>(e.depth + 1), s);
            if (!children_res)
                continue;
            auto children = std::move(children_res.value());

            for (dp::usize idx = 0; idx < children.size(); ++idx) {
                auto &c = children[idx];
                c.is_last = (idx + 1 == children.size());
                c.ancestor_has_more = e.ancestor_has_more;
                if (e.depth > 0) {
                    c.ancestor_has_more.push_back(!e.is_last);
                }
                if (c.kind == EntryKind::Directory) {
                    fs::path canon = fs::weakly_canonical(c.path);
                    c.is_expanded = std::find(expanded.begin(), expanded.end(), canon) != expanded.end();
                    c.icon = c.is_expanded ? ICON_FOLDER_OPEN : ICON_FOLDER_CLOSED;
                }
            }

            dp::usize insert_at = static_cast<dp::usize>(i + 1);
            for (auto &c : children) {
                s.visible.insert(s.visible.begin() + static_cast<std::ptrdiff_t>(insert_at), std::move(c));
                insert_at++;
            }
        }

        if (s.cursor < 0)
            s.cursor = 0;
        if (s.cursor >= static_cast<dp::i32>(s.visible.size()))
            s.cursor = static_cast<dp::i32>(s.visible.empty() ? 0 : (s.visible.size() - 1));
    }

    // =============================================================================================
    // Rendering
    // =============================================================================================

    static dp::String sort_name(SortKind s) {
        switch (s) {
        case SortKind::Name:
            return "name";
        case SortKind::NameRev:
            return "name-rev";
        case SortKind::Extension:
            return "ext";
        case SortKind::ExtensionRev:
            return "ext-rev";
        case SortKind::Size:
            return "size";
        case SortKind::SizeRev:
            return "size-rev";
        case SortKind::Time:
            return "time";
        case SortKind::TimeRev:
            return "time-rev";
        default:
            return "name";
        }
    }

    static void render(const TreeState &s) {
        int term_width = get_terminal_width();

        // Clear and set terminal background if alt_screen and bg_color set
        if (s.alt_screen && s.bg_color >= 0) {
            // Set bg color, clear screen (which fills with bg), then home
            std::cout << "\x1b[48;5;" << s.bg_color << "m\x1b[2J\x1b[H";
        } else {
            std::cout << "\x1b[2J\x1b[H";
        }
        if (s.show_header) {
            std::cout << "lis - tree.nvim-ish file browser\r\n";
            std::cout << "root: " << s.root.string() << "  [sort: " << sort_name(s.sort) << "]";
            if (!s.selected.empty()) {
                std::cout << "  [" << s.selected.size() << " selected]";
            }
            if (!s.clipboard.paths.empty()) {
                std::cout << "  [" << s.clipboard.paths.size() << (s.clipboard.is_cut ? " cut" : " copied") << "]";
            }
            std::cout << "\r\n";
            std::cout << "j/k:move l/h/enter:open/close space:mark .:hidden s:sort c:cd\r\n";
            std::cout << "y:copy d:cut p:paste D:delete r:rename n:file N:dir o:open q:quit\r\n";
            if (!s.message.empty()) {
                std::string msg = echo::format::String(s.message.c_str()).fg("#fabd2f").to_string();
                if (s.alt_screen && s.bg_color >= 0) {
                    msg = apply_persistent_bg(msg, s.bg_color);
                }
                std::cout << msg << "\r\n";
            }
            std::cout << "\r\n";
        } else if (!s.message.empty()) {
            std::string msg = echo::format::String(s.message.c_str()).fg("#fabd2f").to_string();
            if (s.alt_screen && s.bg_color >= 0) {
                msg = apply_persistent_bg(msg, s.bg_color);
            }
            std::cout << msg << "\r\n";
        }

        for (dp::i32 i = 0; i < static_cast<dp::i32>(s.visible.size()); ++i) {
            const auto &e = s.visible[static_cast<dp::usize>(i)];
            bool is_cursor = (i == s.cursor);
            std::string out_line;

            // Cursor prefix
            if (is_cursor) {
                if (s.use_ansi)
                    out_line += echo::format::String("> ").fg("#FFFFFF").bold().to_string();
                else
                    out_line += "> ";
            } else {
                out_line += "  ";
            }

            // Mark column (selected/readonly)
            if (s.show_mark) {
                if (e.is_selected) {
                    if (s.use_ansi)
                        out_line += echo::format::String(MARK_SELECTED).fg("#b8bb26").to_string();
                    else
                        out_line += MARK_SELECTED;
                } else if (e.is_readonly) {
                    if (s.use_ansi)
                        out_line += echo::format::String(MARK_READONLY).fg("#fb4934").to_string();
                    else
                        out_line += MARK_READONLY;
                } else {
                    out_line += " ";
                }
                out_line += " ";
            }

            // Indent column
            if (e.depth > 0) {
                dp::usize start_ancestor = 0;
                if (s.max_depth >= 0 && e.ancestor_has_more.size() > static_cast<dp::usize>(s.max_depth)) {
                    start_ancestor = e.ancestor_has_more.size() - static_cast<dp::usize>(s.max_depth);
                }
                for (dp::usize a = start_ancestor; a < e.ancestor_has_more.size(); ++a) {
                    out_line += (e.ancestor_has_more[a] ? INDENT_PIPE : INDENT_SPACE);
                }
                out_line += (e.is_last ? INDENT_LAST : INDENT_BRANCH);
            }

            // Git column
            if (s.show_git) {
                if (s.use_ansi)
                    out_line += git_styled_ansi(e.git).to_string();
                else
                    out_line += git_glyph(e.git).c_str();
                out_line += " ";
            }

            // Icon column
            if (s.use_ansi)
                out_line += icon_styled_ansi(e.icon, e).to_string();
            else
                out_line += e.icon.c_str();
            out_line += " ";

            // Filename column
            if (s.use_ansi)
                out_line += filename_styled_ansi(e, is_cursor).to_string();
            else
                out_line += e.name.c_str();
            if (e.kind == EntryKind::Directory)
                out_line += "/";

            // Size column
            if (s.show_size && e.kind == EntryKind::File) {
                out_line += "  ";
                if (s.use_ansi)
                    out_line += echo::format::String(format_size(e.size).c_str()).fg("#928374").to_string();
                else
                    out_line += format_size(e.size).c_str();
            }

            // Time column
            if (s.show_time && e.mtime > 0) {
                out_line += "  ";
                if (s.use_ansi)
                    out_line += echo::format::String(format_time(e.mtime).c_str()).fg("#928374").to_string();
                else
                    out_line += format_time(e.mtime).c_str();
            }

            // Determine which background to use for this line
            int line_bg = (is_cursor && s.alt_screen && s.sel_bg_color >= 0) ? s.sel_bg_color
                          : (s.alt_screen && s.bg_color >= 0)                ? s.bg_color
                                                                             : -1;

            if (line_bg >= 0) {
                // Apply background to entire line content
                std::string styled_line = apply_persistent_bg(out_line, line_bg);
                int vis_width = visible_width(out_line);
                int padding = term_width - vis_width;
                if (padding < 0)
                    padding = 0;

                std::cout << "\x1b[48;5;" << line_bg << "m" << styled_line;
                for (int p = 0; p < padding; ++p)
                    std::cout << ' ';
                std::cout << "\x1b[0m";
                // Restore terminal bg if set
                if (s.bg_color >= 0)
                    std::cout << "\x1b[48;5;" << s.bg_color << "m";
                std::cout << "\r\n";
            } else {
                std::cout << out_line << "\r\n";
            }
        }
        std::cout << std::flush;
    }

    // =============================================================================================
    // File operations
    // =============================================================================================

    static void toggle_select(TreeState &s) {
        if (s.visible.empty())
            return;
        auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
        auto canon = fs::weakly_canonical(e.path);
        if (s.selected.count(canon)) {
            s.selected.erase(canon);
            e.is_selected = false;
        } else {
            s.selected.insert(canon);
            e.is_selected = true;
        }
    }

    static void select_all(TreeState &s) {
        for (auto &e : s.visible) {
            auto canon = fs::weakly_canonical(e.path);
            s.selected.insert(canon);
            e.is_selected = true;
        }
    }

    static void clear_selection(TreeState &s) {
        s.selected.clear();
        for (auto &e : s.visible) {
            e.is_selected = false;
        }
    }

    static void copy_selected(TreeState &s) {
        s.clipboard.paths.clear();
        s.clipboard.is_cut = false;
        if (s.selected.empty()) {
            if (!s.visible.empty()) {
                s.clipboard.paths.push_back(s.visible[static_cast<dp::usize>(s.cursor)].path);
            }
        } else {
            for (const auto &p : s.selected) {
                s.clipboard.paths.push_back(p);
            }
        }
        s.message = dp::String(std::to_string(s.clipboard.paths.size()).c_str()) + " file(s) copied";
    }

    static void cut_selected(TreeState &s) {
        s.clipboard.paths.clear();
        s.clipboard.is_cut = true;
        if (s.selected.empty()) {
            if (!s.visible.empty()) {
                s.clipboard.paths.push_back(s.visible[static_cast<dp::usize>(s.cursor)].path);
            }
        } else {
            for (const auto &p : s.selected) {
                s.clipboard.paths.push_back(p);
            }
        }
        s.message = dp::String(std::to_string(s.clipboard.paths.size()).c_str()) + " file(s) cut";
    }

    static void paste_clipboard(TreeState &s) {
        if (s.clipboard.paths.empty()) {
            s.message = "Nothing to paste";
            return;
        }

        // Determine destination directory
        fs::path dest_dir = s.root;
        if (!s.visible.empty()) {
            auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
            if (e.kind == EntryKind::Directory) {
                dest_dir = e.path;
            } else {
                dest_dir = e.path.parent_path();
            }
        }

        int success = 0;
        for (const auto &src : s.clipboard.paths) {
            fs::path dest = dest_dir / src.filename();
            try {
                if (s.clipboard.is_cut) {
                    fs::rename(src, dest);
                } else {
                    fs::copy(src, dest, fs::copy_options::recursive);
                }
                success++;
            } catch (const fs::filesystem_error &ex) {
                s.message = dp::String("Error: ") + ex.what();
            }
        }

        if (s.clipboard.is_cut) {
            s.clipboard.paths.clear();
            clear_selection(s);
        }

        s.message = dp::String(std::to_string(success).c_str()) + " file(s) pasted";
        refresh_git_status(s);
        rebuild_visible(s);
    }

    static bool delete_selected(TreeState &s) {
        dp::Vector<fs::path> to_delete;
        if (s.selected.empty()) {
            if (!s.visible.empty()) {
                to_delete.push_back(s.visible[static_cast<dp::usize>(s.cursor)].path);
            }
        } else {
            for (const auto &p : s.selected) {
                to_delete.push_back(p);
            }
        }

        if (to_delete.empty())
            return false;

        int success = 0;
        for (const auto &p : to_delete) {
            try {
                fs::remove_all(p);
                success++;
            } catch (const fs::filesystem_error &ex) {
                s.message = dp::String("Error: ") + ex.what();
            }
        }

        clear_selection(s);
        s.message = dp::String(std::to_string(success).c_str()) + " file(s) deleted";
        refresh_git_status(s);
        rebuild_visible(s);
        return true;
    }

    static void cycle_sort(TreeState &s) {
        s.sort = static_cast<SortKind>((static_cast<int>(s.sort) + 1) % 8);
        rebuild_visible(s);
    }

    static void open_system(TreeState &s) {
        if (s.visible.empty())
            return;
        auto &e = s.visible[static_cast<dp::usize>(s.cursor)];

#ifdef __APPLE__
        std::string cmd = "open \"" + e.path.string() + "\" 2>/dev/null &";
#elif defined(_WIN32)
        std::string cmd = "start \"\" \"" + e.path.string() + "\"";
#else
        std::string cmd = "xdg-open \"" + e.path.string() + "\" 2>/dev/null &";
#endif
        system(cmd.c_str());
        s.message = "Opened: " + path_to_string(e.path);
    }

    static void yank_path(TreeState &s) {
        if (s.visible.empty())
            return;
        auto &e = s.visible[static_cast<dp::usize>(s.cursor)];

        // Try to copy to system clipboard
#ifdef __APPLE__
        std::string cmd = "echo -n \"" + e.path.string() + "\" | pbcopy";
#elif defined(_WIN32)
        std::string cmd = "echo " + e.path.string() + " | clip";
#else
        std::string cmd = "echo -n \"" + e.path.string() + "\" | xclip -selection clipboard 2>/dev/null";
#endif
        system(cmd.c_str());
        s.message = "Yanked: " + path_to_string(e.path);
    }

    // Read a line of input in raw mode (for rename/new)
    static dp::String read_line_raw(const dp::String &prompt) {
        std::cout << prompt.c_str() << std::flush;
        dp::String result;
        for (;;) {
            auto key = scan::input::read_key();
            if (!key)
                break;
            using scan::input::Key;
            if (key->key == Key::Enter) {
                std::cout << "\r\n" << std::flush;
                break;
            } else if (key->key == Key::Escape || key->key == Key::CtrlC) {
                std::cout << "\r\n" << std::flush;
                return ""; // Cancelled
            } else if (key->key == Key::Backspace) {
                if (!result.empty()) {
                    result.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
            } else if (key->key == Key::Rune && key->rune >= 32 && key->rune < 127) {
                result += static_cast<char>(key->rune);
                std::cout << static_cast<char>(key->rune) << std::flush;
            }
        }
        return result;
    }

    static void rename_entry(TreeState &s) {
        if (s.visible.empty())
            return;
        auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
        if (e.depth == 0) {
            s.message = "Cannot rename root";
            return;
        }

        dp::String prompt = "Rename to: ";
        dp::String new_name = read_line_raw(prompt);
        if (new_name.empty()) {
            s.message = "Rename cancelled";
            return;
        }

        fs::path new_path = e.path.parent_path() / new_name.c_str();
        try {
            fs::rename(e.path, new_path);
            s.message = "Renamed to: " + new_name;
            refresh_git_status(s);
            rebuild_visible(s);
        } catch (const fs::filesystem_error &ex) {
            s.message = dp::String("Error: ") + ex.what();
        }
    }

    static void create_new(TreeState &s, bool is_dir) {
        // Determine parent directory
        fs::path parent_dir = s.root;
        if (!s.visible.empty()) {
            auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
            if (e.kind == EntryKind::Directory) {
                parent_dir = e.path;
            } else {
                parent_dir = e.path.parent_path();
            }
        }

        dp::String prompt = is_dir ? "New directory: " : "New file: ";
        dp::String name = read_line_raw(prompt);
        if (name.empty()) {
            s.message = "Create cancelled";
            return;
        }

        fs::path new_path = parent_dir / name.c_str();
        try {
            if (is_dir) {
                fs::create_directories(new_path);
                s.message = "Created directory: " + name;
            } else {
                std::ofstream ofs(new_path);
                ofs.close();
                s.message = "Created file: " + name;
            }
            refresh_git_status(s);
            rebuild_visible(s);
        } catch (const fs::filesystem_error &ex) {
            s.message = dp::String("Error: ") + ex.what();
        }
    }

    // =============================================================================================
    // Main event loop
    // =============================================================================================

    static dp::Result<dp::Optional<fs::path>, dp::String> run_tree(TreeState &s) {
        // Enter alternate screen if requested
        if (s.alt_screen) {
            std::cout << "\x1b[?1049h" << std::flush;
        }

        scan::terminal::RawMode raw;

        refresh_git_status(s);
        rebuild_visible(s);

        // If highlight target is set, find and select it
        if (!s.highlight_target.empty()) {
            // Expand parents to make target visible
            fs::path target = fs::weakly_canonical(s.highlight_target);
            fs::path current = s.root;
            dp::Vector<fs::path> parents_to_expand;

            // Collect all parent directories that need expanding
            fs::path rel = fs::relative(target, s.root);
            for (const auto &part : rel) {
                current = current / part;
                if (current != target && fs::is_directory(current)) {
                    parents_to_expand.push_back(fs::weakly_canonical(current));
                }
            }

            // Expand each parent
            for (const auto &p : parents_to_expand) {
                dp::i32 idx = find_entry_index(s, p);
                if (idx >= 0) {
                    auto &e = s.visible[static_cast<dp::usize>(idx)];
                    if (e.kind == EntryKind::Directory && !e.is_expanded) {
                        e.is_expanded = true;
                        e.icon = ICON_FOLDER_OPEN;
                        rebuild_visible(s);
                    }
                }
            }

            // Now find and highlight the target
            dp::i32 idx = find_entry_index(s, target);
            if (idx >= 0) {
                s.cursor = idx;
            }
        }

        render(s);

        for (;;) {
            auto key = scan::input::read_key();
            if (!key) {
                if (s.alt_screen)
                    std::cout << "\x1b[?1049l" << std::flush;
                return dp::result::Err(dp::String("failed to read key"));
            }

            s.message.clear(); // Clear message on any key

            using scan::input::Key;
            switch (key->key) {
            case Key::Up:
            case Key::CtrlP:
                if (s.cursor > 0)
                    s.cursor--;
                break;
            case Key::Down:
            case Key::CtrlN:
                if (s.cursor + 1 < static_cast<dp::i32>(s.visible.size()))
                    s.cursor++;
                break;
            case Key::Rune:
                switch (key->rune) {
                case 'q':
                case 'Q':
                    if (s.alt_screen)
                        std::cout << "\x1b[?1049l" << std::flush;
                    return dp::result::Ok(dp::Optional<fs::path>{});
                case 'j':
                case 'J':
                    if (s.cursor + 1 < static_cast<dp::i32>(s.visible.size()))
                        s.cursor++;
                    break;
                case 'k':
                case 'K':
                    if (s.cursor > 0)
                        s.cursor--;
                    break;
                case 'g': // Go to top
                    s.cursor = 0;
                    break;
                case 'G': // Go to bottom
                    s.cursor = static_cast<dp::i32>(s.visible.size()) - 1;
                    break;
                case 'h':
                case 'H': {
                    if (s.visible.empty())
                        break;
                    auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
                    fs::path entry_path = e.path;
                    if (e.kind == EntryKind::Directory && e.is_expanded && e.depth != 0) {
                        e.is_expanded = false;
                        e.icon = ICON_FOLDER_CLOSED;
                        rebuild_visible(s);
                        s.cursor = find_entry_index(s, entry_path);
                    } else if (e.depth > 0) {
                        fs::path parent = entry_path.parent_path();
                        s.cursor = find_entry_index(s, parent);
                    }
                } break;
                case 'l':
                case 'L': {
                    if (s.visible.empty())
                        break;
                    auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
                    fs::path entry_path = e.path;
                    if (e.kind == EntryKind::Directory) {
                        e.is_expanded = true;
                        e.icon = ICON_FOLDER_OPEN;
                        rebuild_visible(s);
                        s.cursor = find_entry_index(s, entry_path);
                    }
                } break;
                case '.':
                    s.show_hidden = !s.show_hidden;
                    rebuild_visible(s);
                    break;
                case ' ': // Toggle select
                    toggle_select(s);
                    if (s.cursor + 1 < static_cast<dp::i32>(s.visible.size()))
                        s.cursor++;
                    break;
                case 'a': // Select all
                    select_all(s);
                    break;
                case 'A': // Clear selection
                    clear_selection(s);
                    break;
                case 'y': // Copy
                    copy_selected(s);
                    break;
                case 'd': // Cut
                    cut_selected(s);
                    break;
                case 'p': // Paste
                    paste_clipboard(s);
                    break;
                case 'D': // Delete
                    delete_selected(s);
                    break;
                case 's': // Cycle sort
                    cycle_sort(s);
                    break;
                case 'S': // Toggle size column
                    s.show_size = !s.show_size;
                    break;
                case 't': // Toggle time column
                    s.show_time = !s.show_time;
                    break;
                case 'o': // Open with system
                    open_system(s);
                    break;
                case 'Y': // Yank path
                    yank_path(s);
                    break;
                case 'R': // Refresh git status
                    refresh_git_status(s);
                    rebuild_visible(s);
                    s.message = "Refreshed";
                    break;
                case '-': // Go to parent directory
                    s.root = s.root.parent_path();
                    refresh_git_status(s);
                    rebuild_visible(s);
                    break;
                case 'r': // Rename
                    rename_entry(s);
                    break;
                case 'n': // New file
                    create_new(s, false);
                    break;
                case 'N': // New directory
                    create_new(s, true);
                    break;
                case 'c': { // cd into directory
                    if (s.visible.empty())
                        break;
                    auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
                    if (e.kind == EntryKind::Directory) {
                        s.root = e.path;
                        s.cursor = 0;
                        refresh_git_status(s);
                        rebuild_visible(s);
                    }
                } break;
                }
                break;
            case Key::Left: {
                if (s.visible.empty())
                    break;
                auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
                fs::path entry_path = e.path;
                if (e.kind == EntryKind::Directory && e.is_expanded && e.depth != 0) {
                    e.is_expanded = false;
                    e.icon = ICON_FOLDER_CLOSED;
                    rebuild_visible(s);
                    s.cursor = find_entry_index(s, entry_path);
                }
            } break;
            case Key::Right: {
                if (s.visible.empty())
                    break;
                auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
                fs::path entry_path = e.path;
                if (e.kind == EntryKind::Directory) {
                    e.is_expanded = true;
                    e.icon = ICON_FOLDER_OPEN;
                    rebuild_visible(s);
                    s.cursor = find_entry_index(s, entry_path);
                }
            } break;
            case Key::Enter: {
                if (s.visible.empty())
                    break;
                auto &e = s.visible[static_cast<dp::usize>(s.cursor)];
                if (e.kind == EntryKind::Directory) {
                    // Toggle expand/collapse
                    fs::path entry_path = e.path; // Save before rebuild invalidates ref
                    e.is_expanded = !e.is_expanded;
                    e.icon = e.is_expanded ? ICON_FOLDER_OPEN : ICON_FOLDER_CLOSED;
                    rebuild_visible(s);
                    s.cursor = find_entry_index(s, entry_path);
                } else {
                    if (s.alt_screen)
                        std::cout << "\x1b[?1049l" << std::flush;
                    return dp::result::Ok(dp::Optional<fs::path>(e.path));
                }
            } break;
            case Key::Backspace: {
                // Go to parent directory
                if (s.root.has_parent_path() && s.root != s.root.root_path()) {
                    s.root = s.root.parent_path();
                    s.cursor = 0;
                    refresh_git_status(s);
                    rebuild_visible(s);
                }
            } break;
            case Key::Escape:
            case Key::CtrlC:
                if (s.alt_screen)
                    std::cout << "\x1b[?1049l" << std::flush;
                return dp::result::Ok(dp::Optional<fs::path>{});
            default:
                break;
            }

            render(s);
        }
    }

} // namespace

int main(int argc, char *argv[]) {
    std::string path_str;
    std::string cwd_str;
    bool show_hidden = false;
    bool alt_screen = false;
    bool no_header = false;
    bool generic_icons = false;
    bool show_git = false;
    bool show_size = false;
    int max_depth = -1;
    int bg_color = -1;
    int sel_bg_color = -1;

    auto cmd =
        argu::Command("lis")
            .version("0.3.0")
            .about("Interactive tree file browser (tree.nvim-ish)")
            .arg(argu::Arg("path")
                     .positional()
                     .help("Path to open (file or directory, or file to highlight if --cwd is set)")
                     .value_of(path_str))
            .arg(argu::Arg("cwd").long_name("cwd").help("Root directory for the tree").value_of(cwd_str))
            .arg(argu::Arg("all").short_name('a').long_name("all").help("Show hidden files").flag(show_hidden))
            .arg(argu::Arg("alt")
                     .short_name('A')
                     .long_name("alt-screen")
                     .help("Use alternate screen buffer")
                     .flag(alt_screen))
            .arg(argu::Arg("compact").short_name('c').long_name("compact").help("Hide header and help").flag(no_header))
            .arg(argu::Arg("generic")
                     .short_name('g')
                     .long_name("generic-icons")
                     .help("Use generic icon for all files")
                     .flag(generic_icons))
            .arg(argu::Arg("git").short_name('G').long_name("git").help("Show git status column").flag(show_git))
            .arg(argu::Arg("size").short_name('s').long_name("size").help("Show file size column").flag(show_size))
            .arg(argu::Arg("depth")
                     .short_name('d')
                     .long_name("depth")
                     .help("Max indent depth (-1 = unlimited)")
                     .value_of(max_depth)
                     .default_value(-1))
            .arg(argu::Arg("bg")
                     .long_name("background")
                     .help("Terminal background (0-255, needs -A)")
                     .value_of(bg_color)
                     .default_value(-1))
            .arg(argu::Arg("selbg")
                     .long_name("selection-background")
                     .help("Selection line background (0-255, needs -A)")
                     .value_of(sel_bg_color)
                     .default_value(-1));

    auto parsed = cmd.parse(argc, argv);
    if (!parsed || !parsed.message().empty())
        return parsed.exit();

    fs::path root;
    fs::path highlight_target;

    if (!cwd_str.empty()) {
        // --cwd provided: use it as root, positional path is file to highlight
        root = fs::absolute(fs::path(cwd_str));
        if (!fs::exists(root)) {
            std::cerr << "error: cwd path does not exist: " << root.string() << "\n";
            return 2;
        }
        if (!fs::is_directory(root)) {
            std::cerr << "error: cwd must be a directory: " << root.string() << "\n";
            return 2;
        }
        if (!path_str.empty()) {
            highlight_target = fs::absolute(fs::path(path_str));
            if (!fs::exists(highlight_target)) {
                std::cerr << "error: file path does not exist: " << highlight_target.string() << "\n";
                return 2;
            }
        }
    } else {
        // No --cwd: use positional argument to determine root (and possibly highlight)
        fs::path input_path = fs::absolute(path_str.empty() ? fs::current_path() : fs::path(path_str));

        if (!fs::exists(input_path)) {
            std::cerr << "error: path does not exist: " << input_path.string() << "\n";
            return 2;
        }

        if (fs::is_directory(input_path)) {
            root = input_path;
        } else {
            // It's a file - open parent directory and highlight the file
            root = input_path.parent_path();
            highlight_target = input_path;
        }
    }

    TreeState state;
    state.root = root;
    state.show_hidden = show_hidden;
    state.alt_screen = alt_screen;
    state.show_header = !no_header;
    state.generic_icons = generic_icons;
    state.show_git = show_git;
    state.show_size = show_size;
    state.max_depth = static_cast<dp::i32>(max_depth);
    state.bg_color = static_cast<dp::i32>(bg_color);
    state.sel_bg_color = static_cast<dp::i32>(sel_bg_color);
    state.highlight_target = highlight_target;

    auto selected = run_tree(state);
    if (!selected) {
        std::cerr << "error: " << selected.error() << "\n";
        return 1;
    }

    if (selected.value().has_value()) {
        std::cout << selected.value().value().string() << "\n";
        return 0;
    }

    return 0;
}
