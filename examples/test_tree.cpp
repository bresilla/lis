#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <echo/format.hpp>
#include <scan/terminal/raw_mode.hpp>
#include <scan/input/reader.hpp>
#include <iostream>

int main() {
    const char* PIPE   = "│ ";
    const char* BRANCH = "├ ";
    const char* LAST   = "└ ";
    const char* SPACE  = "  ";
    const char* ICON_FOLDER = "\xee\x97\xbe";  // nerd font folder open
    const char* ICON_FILE = "\xee\x98\x92";    // nerd font file

    std::cout << "=== TEST 1: dp::String + std::cout (no ANSI) ===\n";

    dp::String line1;
    line1 += "  ";
    line1 += BRANCH;
    line1 += "dir1/";
    std::cout << line1 << "\n";

    dp::String line2;
    line2 += "> ";
    line2 += BRANCH;
    line2 += "dir2/";
    std::cout << line2 << "\n";

    dp::String line3;
    line3 += "  ";
    line3 += LAST;
    line3 += "file.txt";
    std::cout << line3 << "\n";

    std::cout << "\n=== TEST 2: dp::String + std::cout + ANSI styling ===\n";

    dp::String line4;
    line4 += echo::format::String("> ").fg("#FFFFFF").bold().to_string();
    line4 += BRANCH;
    line4 += "dir2/";
    std::cout << line4 << "\n";

    dp::String line5;
    line5 += "  ";
    line5 += BRANCH;
    line5 += echo::format::String("dir1/").fg("#689FB6").to_string();
    std::cout << line5 << "\n";

    std::cout << "\n=== TEST 3: Full columns - no ANSI ===\n";

    const char* ICON = "";  // folder icon
    const char* GIT = " ";   // no git status (space)

    dp::String line6;
    line6 += "  ";      // cursor
    line6 += BRANCH;    // tree
    line6 += GIT;       // git
    line6 += " ";       // space
    line6 += ICON;      // icon
    line6 += " ";       // space
    line6 += "dir1/";   // name
    std::cout << line6 << "\n";

    dp::String line6b;
    line6b += "> ";
    line6b += BRANCH;
    line6b += GIT;
    line6b += " ";
    line6b += ICON;
    line6b += " ";
    line6b += "dir2/";
    std::cout << line6b << "\n";

    std::cout << "\n=== TEST 4: Full columns - WITH ANSI ===\n";

    dp::String line7;
    line7 += "  ";
    line7 += BRANCH;
    line7 += echo::format::String(GIT).fg("#FFFFFF").to_string();
    line7 += " ";
    line7 += echo::format::String(ICON_FOLDER).fg("#00afaf").to_string();
    line7 += " ";
    line7 += echo::format::String("dir1/").fg("#689FB6").to_string();
    std::cout << line7 << "\n";

    dp::String line8;
    line8 += echo::format::String("> ").fg("#FFFFFF").bold().to_string();
    line8 += BRANCH;
    line8 += echo::format::String(GIT).fg("#FFFFFF").to_string();
    line8 += " ";
    line8 += echo::format::String(ICON_FOLDER).fg("#00afaf").to_string();
    line8 += " ";
    line8 += echo::format::String("dir2/").fg("#689FB6").bold().to_string();
    std::cout << line8 << "\n";

    std::cout << "\n=== TEST 5: Check what echo::format::String returns ===\n";

    auto styled_cursor = echo::format::String("> ").fg("#FFFFFF").bold().to_string();
    auto styled_space = echo::format::String(" ").fg("#FFFFFF").to_string();

    std::cout << "Plain '> ' bytes: 2\n";
    std::cout << "Styled '> ' bytes: " << styled_cursor.size() << "\n";
    std::cout << "Styled '> ' content: [" << styled_cursor << "]\n";
    std::cout << "Plain ' ' bytes: 1\n";
    std::cout << "Styled ' ' bytes: " << styled_space.size() << "\n";
    std::cout << "Styled ' ' content: [" << styled_space << "]\n";

    std::cout << "\n=== TEST 6: Exact lis.cpp simulation ===\n";
    std::cout << "(press enter to see TEST 7 with screen clear)\n";
    std::cin.get();

    // Clear screen like lis.cpp does
    std::cout << "\x1b[2J\x1b[H";
    std::cout << "=== TEST 7: With screen clear (like lis.cpp) ===\n";
    std::cout << "root: /test/path\n";
    std::cout << "keys: j/k move, enter select, q quit\n\n";

    // Simulating what lis.cpp does exactly
    // Root entry (depth 0, cursor)
    dp::String root;
    root += echo::format::String("> ").fg("#FFFFFF").bold().to_string();
    root += echo::format::String(" ").fg("#FFFFFF").to_string();
    root += " ";
    root += echo::format::String(ICON_FOLDER).fg("#00afaf").to_string();
    root += " ";
    root += echo::format::String("root").fg("#689FB6").bold().to_string();
    root += "/";
    std::cout << root << "\n";

    // depth 1, not last (has sibling after)
    dp::String d1;
    d1 += "  ";
    d1 += BRANCH;
    d1 += echo::format::String(" ").fg("#FFFFFF").to_string();
    d1 += " ";
    d1 += echo::format::String(ICON_FOLDER).fg("#00afaf").to_string();
    d1 += " ";
    d1 += echo::format::String("dir1").fg("#689FB6").to_string();
    d1 += "/";
    std::cout << d1 << "\n";

    // depth 2 under dir1 (dir1 is NOT last, so use PIPE for continuation)
    dp::String d2;
    d2 += "  ";
    d2 += PIPE;    // ancestor continuation (dir1 has more siblings)
    d2 += BRANCH;  // this entry's branch
    d2 += echo::format::String(" ").fg("#FFFFFF").to_string();
    d2 += " ";
    d2 += echo::format::String(ICON_FOLDER).fg("#00afaf").to_string();
    d2 += " ";
    d2 += echo::format::String("subdir").fg("#689FB6").to_string();
    d2 += "/";
    std::cout << d2 << "\n";

    // depth 2, last under dir1
    dp::String d2b;
    d2b += "  ";
    d2b += PIPE;
    d2b += LAST;
    d2b += echo::format::String(" ").fg("#FFFFFF").to_string();
    d2b += " ";
    d2b += echo::format::String(ICON_FILE).fg("#999999").to_string();
    d2b += " ";
    d2b += echo::format::String("file1.txt").fg("#F09F17").to_string();
    std::cout << d2b << "\n";

    // depth 1, last sibling
    dp::String d1b;
    d1b += "  ";
    d1b += LAST;
    d1b += echo::format::String(" ").fg("#FFFFFF").to_string();
    d1b += " ";
    d1b += echo::format::String(ICON_FOLDER).fg("#00afaf").to_string();
    d1b += " ";
    d1b += echo::format::String("dir2").fg("#689FB6").to_string();
    d1b += "/";
    std::cout << d1b << "\n";

    // depth 2 under dir2 (dir2 IS last, so use SPACE for continuation)
    dp::String d2c;
    d2c += "  ";
    d2c += SPACE;  // ancestor continuation (dir2 is last, no more siblings)
    d2c += LAST;
    d2c += echo::format::String(" ").fg("#FFFFFF").to_string();
    d2c += " ";
    d2c += echo::format::String(ICON_FILE).fg("#999999").to_string();
    d2c += " ";
    d2c += echo::format::String("file2.txt").fg("#F09F17").to_string();
    std::cout << d2c << "\n";

    std::cout << "\n(press Enter to continue to TEST 8 with raw mode)\n";
    std::cin.get();

    // TEST 8: With raw terminal mode (like lis.cpp)
    // In raw mode, \n doesn't do carriage return, need \r\n
    {
        scan::terminal::RawMode raw;  // enters raw mode

        std::cout << "\x1b[2J\x1b[H";
        std::cout << "=== TEST 8: With RAW TERMINAL MODE ===\r\n";
        std::cout << "root: /test/path\r\n";
        std::cout << "keys: press 'q' to quit\r\n\r\n";

        // Same tree output - use \r\n for raw mode
        std::cout << root << "\r\n";
        std::cout << d1 << "\r\n";
        std::cout << d2 << "\r\n";
        std::cout << d2b << "\r\n";
        std::cout << d1b << "\r\n";
        std::cout << d2c << "\r\n";
        std::cout << std::flush;

        // Wait for 'q'
        for (;;) {
            auto key = scan::input::read_key();
            if (key && key->key == scan::input::Key::Rune && key->rune == 'q') {
                break;
            }
        }
    }  // raw mode exits here

    std::cout << "\nDone! Raw mode exited.\n";
    return 0;
}
