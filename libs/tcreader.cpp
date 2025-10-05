//===================================================================
// tcreader.cpp  –  Terminal comic reader (refined)
//===================================================================

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <cmath>

#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <termios.h>
#include <signal.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"

namespace fs = std::filesystem;

// ------------------------------------------------------------------
// Simple JSON writer for progress tracking
// ------------------------------------------------------------------
class SimpleJSON {
public:
    std::map<std::string, int> data;

    void load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(line.find('"') + 1);
                key = key.substr(0, key.find('"'));

                std::string val = line.substr(colon + 1);
                val.erase(0, val.find_first_not_of(" \t"));
                val.erase(val.find_last_not_of(" ,\t\n") + 1);

                data[key] = std::stoi(val);
            }
        }
    }

    void save(const std::string& path) {
        std::ofstream file(path);
        if (!file.is_open()) return;

        file << "{\n";
        bool first = true;
        for (const auto& [key, val] : data) {
            if (!first) file << ",\n";
            file << "  \"" << key << "\": " << val;
            first = false;
        }
        file << "\n}\n";
    }
};

// ------------------------------------------------------------------
// Base64 encoding for Kitty graphics protocol
// ------------------------------------------------------------------
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* data, size_t len) {
    std::string ret;
    int i = 0, j = 0;
    unsigned char arr3[3], arr4[4];

    while (len--) {
        arr3[i++] = *(data++);
        if (i == 3) {
            arr4[0] = (arr3[0] & 0xfc) >> 2;
            arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
            arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
            arr4[3] = arr3[2] & 0x3f;
            for (i = 0; i < 4; i++) ret += base64_chars[arr4[i]];
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 3; j++) arr3[j] = '\0';
        arr4[0] = (arr3[0] & 0xfc) >> 2;
        arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
        arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
        for (j = 0; j < i + 1; j++) ret += base64_chars[arr4[j]];
        while (i++ < 3) ret += '=';
    }
    return ret;
}

// ------------------------------------------------------------------
// Natural sort comparator (used for file listings)
// ------------------------------------------------------------------
bool natural_sort_compare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(a[i]) && std::isdigit(b[j])) {
            size_t num_start_a = i, num_start_b = j;
            while (i < a.size() && std::isdigit(a[i])) i++;
            while (j < b.size() && std::isdigit(b[j])) j++;

            long long num_a = std::stoll(a.substr(num_start_a, i - num_start_a));
            long long num_b = std::stoll(b.substr(num_start_b, j - num_start_b));

            if (num_a != num_b) return num_a < num_b;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i; ++j;
        }
    }
    return a.size() < b.size();
}

// ------------------------------------------------------------------
// Rendering mode enum
// ------------------------------------------------------------------
enum class RenderMode { KITTY, TIMG, ASCII };

// ------------------------------------------------------------------
// Configuration holder (includes key‑map)
// ------------------------------------------------------------------
struct Config {
    std::map<std::string, std::string> keymap;
    bool double_page = false;
    bool show_help   = false;
    RenderMode render_mode = RenderMode::KITTY;
    std::vector<std::string> library_paths;

    Config() {
        // -------------------- DEFAULT KEYMAP --------------------
        keymap["quit"]          = "q";
        keymap["refresh"]       = "r";
        keymap["toggle_help"]   = "?";
        keymap["first_page"]    = "g";
        keymap["last_page"]     = "G";

        // Page navigation (plain keys)
        keymap["next"]          = "l";               // next page
        keymap["prev"]          = "h";               // previous page
        keymap["next_alt"]      = "\x1b[C";          // →  (right arrow)
        keymap["prev_alt"]      = "\x1b[D";          // ←  (left arrow)
        keymap["up"]            = "k";               // up in file list
        keymap["down"]          = "j";               // down in file list

        // Zoom
        keymap["zoom_in"]       = "=";               // Shift + +
        keymap["zoom_out"]      = "-";               // plain -
        keymap["zoom_out_alt"]  = "_";               // Shift + -
        keymap["zoom_reset"]    = "0";               // reset zoom

        // Pan while zoomed – Shift + H/J/K/L **or** Shift + arrows
        // Upper‑case letters are what you receive when Shift is held.
        keymap["pan_up"]        = "K";               // Shift‑K  (or Shift‑↑)
        keymap["pan_down"]      = "J";               // Shift‑J  (or Shift‑↓)
        keymap["pan_left"]      = "H";               // Shift‑H  (or Shift‑←)
        keymap["pan_right"]     = "L";               // Shift‑L  (or Shift‑→)

        // Miscellaneous
        keymap["toggle_spread"] = "s";
        keymap["double_page"]   = "d";   // optional extra shortcut
    }

    // ------------------------------------------------------------------
    // Load configuration from a simple INI‑like file
    // ------------------------------------------------------------------
    void load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;

            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);

                // Trim whitespace
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                val.erase(0, val.find_first_not_of(" \t"));
                val.erase(val.find_last_not_of(" \t") + 1);

                if (key == "double_page") {
                    double_page = (val == "true" || val == "1");
                } else if (key == "show_help") {
                    show_help = (val == "true" || val == "1");
                } else if (key == "render_mode") {
                    if (val == "timg") render_mode = RenderMode::TIMG;
                    else if (val == "ascii") render_mode = RenderMode::ASCII;
                    else render_mode = RenderMode::KITTY;
                } else if (key == "library") {
                    library_paths.push_back(val);
                } else {
                    keymap[key] = val;
                }
            }
        }
    }
};

// ------------------------------------------------------------------
// Terminal size helper
// ------------------------------------------------------------------
struct TermSize {
    int rows, cols;
    int pixel_width, pixel_height;
};

TermSize get_term_size() {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    TermSize size;
    size.rows = w.ws_row;
    size.cols = w.ws_col;
    size.pixel_width  = w.ws_xpixel > 0 ? w.ws_xpixel : w.ws_col * 10;
    size.pixel_height = w.ws_ypixel > 0 ? w.ws_ypixel : w.ws_row * 20;
    return size;
}

// ------------------------------------------------------------------
// Archive handling (libarchive wrapper)
// ------------------------------------------------------------------
struct PageEntry {
    std::string name;
    size_t index_in_archive;
};

class ArchiveReader {
private:
    struct archive* archive_handle = nullptr;
    std::string archive_path;
    std::vector<PageEntry> entries;
    int current_entry = -1;

public:
    ~ArchiveReader() { close(); }

    bool open(const std::string& path) {
        close();

        archive_path = path;
        archive_handle = archive_read_new();
        archive_read_support_format_all(archive_handle);
        archive_read_support_filter_all(archive_handle);

        if (archive_read_open_filename(archive_handle, path.c_str(), 10240) != ARCHIVE_OK) {
            close();
            return false;
        }

        // Index image entries
        entries.clear();
        struct archive_entry* entry;
        size_t idx = 0;

        while (archive_read_next_header(archive_handle, &entry) == ARCHIVE_OK) {
            std::string name = archive_entry_pathname(entry);
            std::string ext = fs::path(name).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
                ext == ".gif" || ext == ".webp" || ext == ".bmp") {
                PageEntry pg{ name, idx };
                entries.push_back(pg);
            }
            archive_read_data_skip(archive_handle);
            ++idx;
        }

        // Natural sort
        std::sort(entries.begin(), entries.end(),
                  [](const PageEntry& a, const PageEntry& b) {
                      return natural_sort_compare(a.name, b.name);
                  });

        // Re‑open for sequential reading
        archive_read_free(archive_handle);
        archive_handle = archive_read_new();
        archive_read_support_format_all(archive_handle);
        archive_read_support_filter_all(archive_handle);
        archive_read_open_filename(archive_handle, path.c_str(), 10240);
        current_entry = -1;

        return !entries.empty();
    }

    void close() {
        if (archive_handle) {
            archive_read_free(archive_handle);
            archive_handle = nullptr;
        }
        entries.clear();
        current_entry = -1;
    }

    std::vector<unsigned char> read_page(size_t page_idx) {
        if (page_idx >= entries.size() || !archive_handle) return {};

        size_t target_idx = entries[page_idx].index_in_archive;

        if (current_entry > static_cast<int>(target_idx) || current_entry == -1) {
            // Need to rewind
            archive_read_free(archive_handle);
            archive_handle = archive_read_new();
            archive_read_support_format_all(archive_handle);
            archive_read_support_filter_all(archive_handle);
            archive_read_open_filename(archive_handle, archive_path.c_str(), 10240);
            current_entry = -1;
        }

        struct archive_entry* entry;
        while (archive_read_next_header(archive_handle, &entry) == ARCHIVE_OK) {
            ++current_entry;
            if (static_cast<size_t>(current_entry) == target_idx) {
                size_t sz = archive_entry_size(entry);
                std::vector<unsigned char> data(sz);
                ssize_t got = archive_read_data(archive_handle, data.data(), sz);
                if (got < 0) return {};
                return data;
            }
            archive_read_data_skip(archive_handle);
        }
        return {};
    }

    size_t page_count() const { return entries.size(); }
    const std::vector<PageEntry>& get_entries() const { return entries; }
};

// ------------------------------------------------------------------
// File‑system entry used for the directory browser
// ------------------------------------------------------------------
struct FileEntry {
    std::string name;
    std::string full_path;
    bool is_directory;
};

// ------------------------------------------------------------------
// Main comic reader class (handles UI, input, rendering, etc.)
// ------------------------------------------------------------------
class ComicReader {
private:
    std::vector<FileEntry> entries;
    std::string current_dir;
    int selected_idx = 0;

    // Archive & caching
    ArchiveReader archive;
    std::map<int, std::vector<unsigned char>> page_cache;
    int current_page = 0;
    bool viewing_comic = false;
    std::string current_comic_filename;

    // Zoom / pan state
    float zoom_level = 1.0f;
    int pan_x = 0, pan_y = 0;

    // Config & progress
    Config config;
    SimpleJSON progress;
    std::string progress_path;

    // Terminal handling
    struct termios orig_termios;
    bool need_redraw = false;

    // ------------------------------------------------------------------
    // Terminal raw‑mode helpers
    // ------------------------------------------------------------------
    void enable_raw_mode() {
        tcgetattr(STDIN_FILENO, &orig_termios);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
    void disable_raw_mode() {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }

    // ------------------------------------------------------------------
    // Screen utilities
    // ------------------------------------------------------------------
    void clear_screen() { std::cout << "\033[2J\033[H" << std::flush; }

    static void handle_sigwinch(int) { /* empty – just forces a redraw */ }

    // ------------------------------------------------------------------
    // Directory scanning
    // ------------------------------------------------------------------
    void scan_directory() {
        entries.clear();

        // Parent entry (..) if not at filesystem root
        if (current_dir != "/" && current_dir.find('/') != std::string::npos) {
            FileEntry parent;
            parent.name = "..";
            parent.full_path = fs::path(current_dir).parent_path().string();
            parent.is_directory = true;
            entries.push_back(parent);
        }

        std::vector<FileEntry> dirs, files;
        try {
            for (const auto& e : fs::directory_iterator(current_dir)) {
                FileEntry fe;
                fe.name = e.path().filename().string();
                fe.full_path = e.path().string();
                fe.is_directory = e.is_directory();

                if (fe.is_directory) {
                    dirs.push_back(fe);
                } else if (e.is_regular_file()) {
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".cbz" || ext == ".cbr" || ext == ".zip")
                        files.push_back(fe);
                }
            }
        } catch (...) {
            // ignore permission errors etc.
        }

        std::sort(dirs.begin(), dirs.end(),
                  [](const FileEntry& a, const FileEntry& b) {
                      return natural_sort_compare(a.name, b.name);
                  });
        std::sort(files.begin(), files.end(),
                  [](const FileEntry& a, const FileEntry& b) {
                      return natural_sort_compare(a.name, b.name);
                  });

        entries.insert(entries.end(), dirs.begin(), dirs.end());
        entries.insert(entries.end(), files.begin(), files.end());

        if (selected_idx >= static_cast<int>(entries.size())) selected_idx = 0;
    }

    // ------------------------------------------------------------------
    // Page loading & pre‑loading
    // ------------------------------------------------------------------
    std::vector<unsigned char> load_page(int page_idx) {
        if (page_cache.count(page_idx))
            return page_cache[page_idx];

        auto data = archive.read_page(page_idx);
        if (!data.empty()) {
            // Simple LRU‑ish eviction (keep pages within ±2 of current)
            std::vector<int> to_remove;
            for (const auto& [idx, _] : page_cache)
                if (std::abs(idx - page_idx) > 2) to_remove.push_back(idx);
            for (int i : to_remove) page_cache.erase(i);
            page_cache[page_idx] = data;
        }
        return data;
    }

    // ------------------------------------------------------------------
    // Pre‑load the pages adjacent to the current one (for smoother paging)
    // ------------------------------------------------------------------
    void preload_adjacent() {
        if (current_page + 1 < static_cast<int>(archive.page_count()))
            load_page(current_page + 1);
        if (current_page > 0)
            load_page(current_page - 1);
    }

    // ------------------------------------------------------------------
    // Rendering helpers
    // ------------------------------------------------------------------
    bool render_with_timg(const std::vector<unsigned char>& img_data,
                          const std::string& temp_path, int cols, int rows) {
        // Write image to a temporary file
        std::ofstream tmp(temp_path, std::ios::binary);
        if (!tmp) return false;
        tmp.write(reinterpret_cast<const char*>(img_data.data()), img_data.size());
        tmp.close();

        // Build timg command using specified dimensions
        std::string cmd = "timg -g " +
                          std::to_string(cols) + "x" +
                          std::to_string(rows) + " " + temp_path;
        system(cmd.c_str());
        return true;
    }

    void render_ascii(const std::vector<unsigned char>& img_data) {
        int w, h, ch;
        unsigned char* pixels = stbi_load_from_memory(
            img_data.data(), static_cast<int>(img_data.size()), &w, &h, &ch, 1);
        if (!pixels) {
            std::cout << "[Failed to decode image]\n";
            return;
        }

        TermSize term = get_term_size();
        int target_w = std::min(term.cols - 4, 80);
        int target_h = std::min(term.rows - 4, 40);

        std::vector<unsigned char> resized(target_w * target_h);
        stbir_resize_uint8_linear(pixels, w, h, 0,
                                  resized.data(), target_w, target_h, 0,
                                  STBIR_1CHANNEL);

        const char* charset =
            " .'`^\",:;Il!i><~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
        int charset_len = static_cast<int>(strlen(charset));

        for (int y = 0; y < target_h; ++y) {
            for (int x = 0; x < target_w; ++x) {
                int brightness = resized[y * target_w + x];
                int idx = (brightness * (charset_len - 1)) / 255;
                std::cout << charset[idx];
            }
            std::cout << '\n';
        }

        stbi_image_free(pixels);
    }

    void render_kitty(const std::vector<unsigned char>& img_data,
                      int col_offset = 0, int width_px = 0) {
        int w, h, ch;
        unsigned char* pixels = stbi_load_from_memory(
            img_data.data(), static_cast<int>(img_data.size()), &w, &h, &ch, 3);
        if (!pixels) {
            std::cout << "[Failed to decode: " << stbi_failure_reason()
                      << "]\n";
            return;
        }

        TermSize term = get_term_size();
        int target_w = width_px > 0 ? width_px : term.pixel_width;
        int target_h = term.pixel_height - 100;  // leave room for status line

        // Apply zoom (scale uniformly)
        float scale = std::min(static_cast<float>(target_w) / w,
                              static_cast<float>(target_h) / h) *
                      zoom_level;
        int new_w = static_cast<int>(w * scale);
        int new_h = static_cast<int>(h * scale);

        // Clamp pan so we never scroll past the image edges
        int max_pan_x = std::max(0, new_w - target_w);
        int max_pan_y = std::max(0, new_h - target_h);
        pan_x = std::clamp(pan_x, -max_pan_x, 0);
        pan_y = std::clamp(pan_y, -max_pan_y, 0);

        // Resize
        std::vector<unsigned char> resized(new_w * new_h * 3);
        stbir_resize_uint8_linear(pixels, w, h, 0,
                                  resized.data(), new_w, new_h, 0,
                                  STBIR_RGB);

        // Crop to the visible region (taking pan into account)
        int crop_x = std::abs(pan_x);
        int crop_y = std::abs(pan_y);
        int crop_w = std::min(new_w - crop_x, target_w);
        int crop_h = std::min(new_h - crop_y, target_h);

        std::vector<unsigned char> cropped(crop_w * crop_h * 3);
        for (int y = 0; y < crop_h; ++y) {
            for (int x = 0; x < crop_w; ++x) {
                int src_idx = ((crop_y + y) * new_w + (crop_x + x)) * 3;
                int dst_idx = (y * crop_w + x) * 3;
                cropped[dst_idx + 0] = resized[src_idx + 0];
                cropped[dst_idx + 1] = resized[src_idx + 1];
                cropped[dst_idx + 2] = resized[src_idx + 2];
            }
        }

        // Encode to base64 for the Kitty graphics protocol
        std::string b64 = base64_encode(cropped.data(),
                                        static_cast<size_t>(cropped.size()));

        // Determine how many character cells the image occupies
        int char_w = term.pixel_width / term.cols;
        int char_h = term.pixel_height / term.rows;
        int cols_needed = (crop_w + char_w - 1) / char_w;
        int rows_needed = (crop_h + char_h - 1) / char_h;

        // Center the image (unless an explicit column offset was given)
        int col_center = col_offset +
                         (target_w / char_w - cols_needed) / 2;
        int row_center = (target_h / char_h - rows_needed) / 2 + 1;
        col_center = std::max(0, col_center);
        row_center = std::max(1, row_center);

        // Position cursor
        printf("\033[%d;%dH", row_center, col_center + 1);
        fflush(stdout);

        // Send the image in chunks (Kitty protocol)
        const size_t chunk_sz = 4096;
        size_t offset = 0;
        while (offset < b64.size()) {
            size_t remain = b64.size() - offset;
            size_t this_chunk = std::min(chunk_sz, remain);
            bool last = (offset + this_chunk >= b64.size());

            if (offset == 0) {
                // First chunk – include dimensions & placement
                printf("\033_Gf=24,a=T,s=%d,v=%d,c=%d,r=%d,m=%d;",
                       crop_w, crop_h, cols_needed, rows_needed,
                       last ? 0 : 1);
            } else {
                // Subsequent chunks
                printf("\033_Gm=%d;", last ? 0 : 1);
            }

            fwrite(b64.data() + offset, 1, this_chunk, stdout);
            printf("\033\\");
            fflush(stdout);
            offset += this_chunk;
        }

        stbi_image_free(pixels);
    }

    // ------------------------------------------------------------------
    // Dispatch to the selected renderer
    // ------------------------------------------------------------------
    void display_image(const std::vector<unsigned char>& img_data,
                       int x_offset = 0, int width = 0) {
        if (img_data.empty()) {
            std::cout << "[Empty image data]\n";
            return;
        }

        TermSize term = get_term_size();
        int target_cols = width > 0 ? width / (term.pixel_width / term.cols) : term.cols;
        int target_rows = term.rows - 3;

        switch (config.render_mode) {
            case RenderMode::KITTY:
                render_kitty(img_data, x_offset, width);
                break;
            case RenderMode::TIMG:
                render_with_timg(img_data, "/tmp/tcreader_page_" + std::to_string(x_offset) + ".tmp", target_cols, target_rows);
                break;
            case RenderMode::ASCII:
                render_ascii(img_data);
                break;
        }
    }

    // ------------------------------------------------------------------
    // UI: file‑list view
    // ------------------------------------------------------------------
    void draw_file_list() {
        clear_screen();
        std::cout << "\033[1;1H";
        std::cout << "tcreader - " << current_dir << "\n";

        if (config.show_help) {
            std::cout << "Enter=open | r=refresh | g/G=first/last | j/k=down/up | ?=help | q=quit\n";
        }
        std::cout << "\n";

        TermSize term = get_term_size();
        int lines_available = term.rows - (config.show_help ? 6 : 5);
        int start = std::max(0, selected_idx - lines_available / 2);
        int end   = std::min(static_cast<int>(entries.size()),
                             start + lines_available);

        for (int i = start; i < end; ++i) {
            const FileEntry& fe = entries[i];
            std::string prefix = fe.is_directory ? "📁 " : "  ";

            if (i == selected_idx) {
                std::cout << "\033[7m► " << prefix << fe.name << "\033[0m\n";
            } else {
                std::cout << "  " << prefix << fe.name << "\n";
            }
        }

        // Summary line
        int comic_cnt = 0, folder_cnt = 0;
        for (const auto& e : entries) {
            if (e.is_directory && e.name != "..")
                ++folder_cnt;
            else if (!e.is_directory)
                ++comic_cnt;
        }
        std::cout << "\n" << folder_cnt << " folders, " << comic_cnt
                  << " comics\n";
        std::cout << std::flush;
    }

    // ------------------------------------------------------------------
    // UI: comic‑view (single page or double‑page spread)
    // ------------------------------------------------------------------
    void draw_comic_view() {
        clear_screen();

        bool effective_double = config.double_page &&
                               std::abs(zoom_level - 1.0f) < 0.001f &&
                               (config.render_mode == RenderMode::KITTY ||
                                config.render_mode == RenderMode::TIMG);
        if (effective_double && current_page + 1 < static_cast<int>(archive.page_count())) {
            // Double‑page spread (only when not zoomed and in KITTY or TIMG mode)
            auto p1 = load_page(current_page);
            auto p2 = load_page(current_page + 1);

            TermSize term = get_term_size();
            int half_cols = term.cols / 2;
            int half_px   = term.pixel_width / 2;

            if (!p1.empty())
                display_image(p1, 0, half_px);
            if (!p2.empty())
                display_image(p2, half_cols, half_px);
        } else {
            // Single page (or zoomed view)
            auto page = load_page(current_page);
            if (!page.empty())
                display_image(page, 0, 0);
        }

        // Status line (bottom of the screen)
        TermSize term = get_term_size();
        std::cout << "\033[" << term.rows << ";1H\033[K";

        if (config.show_help) {
            if (config.double_page) {
                std::cout << "Pages " << (current_page + 1) << "-"
                          << std::min(current_page + 2,
                                      static_cast<int>(archive.page_count()))
                          << "/" << archive.page_count();
            } else {
                std::cout << "Page " << (current_page + 1) << "/"
                          << archive.page_count();
            }
            std::cout << " | Zoom: " << static_cast<int>(zoom_level * 100)
                      << "% | Shift+=/Shift-=zoom | 0=reset | arrows/hjkl=nav | Shift+arrows/HJKL=pan | s=spread | q=back";
        }
        std::cout << std::flush;

        // Save progress
        progress.data[current_comic_filename] = current_page;
        progress.save(progress_path);

        // Pre‑load neighbours for smoother paging
        preload_adjacent();
    }

public:
    ComicReader(const std::string& initial_dir) : current_dir(initial_dir) {
        std::string home = getenv("HOME") ? getenv("HOME") : "";
        if (!home.empty()) {
            config.load(home + "/.tcreader.conf");
            progress_path = home + "/.tcreader_progress.json";
            progress.load(progress_path);
        }
        scan_directory();
    }

    // ------------------------------------------------------------------
    // Main event loop
    // ------------------------------------------------------------------
    void run() {
        enable_raw_mode();
        draw_file_list();

        char c;
        while (read(STDIN_FILENO, &c, 1) == 1) {
            // ------------------------------------------------------------
            // 1️⃣  LIST BROWSER (not currently viewing a comic)
            // ------------------------------------------------------------
            if (!viewing_comic) {
                if (c == config.keymap["quit"][0]) break;

                else if (c == config.keymap["refresh"][0]) {
                    scan_directory();
                    draw_file_list();
                }

                else if (c == config.keymap["toggle_help"][0]) {
                    config.show_help = !config.show_help;
                    draw_file_list();
                }

                else if (c == config.keymap["first_page"][0]) {
                    selected_idx = 0;
                    draw_file_list();
                }

                else if (c == config.keymap["last_page"][0]) {
                    selected_idx = static_cast<int>(entries.size()) - 1;
                    draw_file_list();
                }

                else if (c == config.keymap["up"][0] && selected_idx > 0) {
                    --selected_idx;
                    draw_file_list();
                }

                else if (c == config.keymap["down"][0] &&
                         selected_idx < static_cast<int>(entries.size()) - 1) {
                    ++selected_idx;
                    draw_file_list();
                }

                else if (c == '\033') {          // Arrow keys (up/down)
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
                    if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;

                    if (seq[0] == '[') {
                        if (seq[1] == 'A' && selected_idx > 0) {
                            --selected_idx;
                            draw_file_list();
                        } else if (seq[1] == 'B' &&
                                   selected_idx < static_cast<int>(entries.size()) - 1) {
                            ++selected_idx;
                            draw_file_list();
                        }
                    }
                }

                else if (c == '\n' || c == '\r') {   // Enter → open
                    if (entries.empty() ||
                        selected_idx >= static_cast<int>(entries.size()))
                        continue;

                    const FileEntry& fe = entries[selected_idx];
                    if (fe.is_directory) {
                        // Change directory
                        current_dir = fe.full_path;
                        selected_idx = 0;
                        scan_directory();
                        draw_file_list();
                    } else {
                        // Try to open the archive
                        if (archive.open(fe.full_path)) {
                            viewing_comic = true;
                            current_comic_filename = fe.name;
                            zoom_level = 1.0f;
                            pan_x = pan_y = 0;

                            // Restore saved page if we have one
                            if (progress.data.count(current_comic_filename)) {
                                current_page = progress.data[current_comic_filename];
                                if (current_page >= static_cast<int>(archive.page_count()))
                                    current_page = 0;
                            } else {
                                current_page = 0;
                            }
                            draw_comic_view();
                        } else {
                            std::cout << "\n[Failed to open archive]\n";
                            std::fflush(stdout);
                            sleep(1);
                            draw_file_list();
                        }
                    }
                }

                // --------------------------------------------------------
                // 2️⃣  COMIC VIEW (already opened a comic)
                // --------------------------------------------------------
            } else {
                bool navigate = false;
                int step = config.double_page ? 2 : 1;

                // ---- Helper: pan while zoomed (Shift + H/J/K/L or Shift + Arrows) ----
                auto do_pan = [&](char dir) {
                    if (std::abs(zoom_level - 1.0f) < 0.001f) return;  // Only pan when zoomed
                    switch (dir) {
                        case 'K': pan_y += 50; break;   // Shift‑K → up
                        case 'J': pan_y -= 50; break;   // Shift‑J → down
                        case 'H': pan_x += 50; break;   // Shift‑H → left
                        case 'L': pan_x -= 50; break;   // Shift‑L → right
                    }
                    navigate = true;
                };

                // ---- Escape sequences (arrows & Shift‑arrows) ----
                if (c == '\033') {
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) != 1) {
                        // Plain ESC → exit comic view
                        viewing_comic = false;
                        archive.close();
                        page_cache.clear();
                        draw_file_list();
                        continue;
                    }
                    if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;

                    if (seq[0] == '[') {
                        // Detect Shift‑arrow: ESC [ 1 ; 2 A/B/C/D
                        if (seq[1] == '1') {
                            char semi, two, dir;
                            if (read(STDIN_FILENO, &semi, 1) != 1) continue;
                            if (read(STDIN_FILENO, &two, 1) != 1) continue;
                            if (read(STDIN_FILENO, &dir, 1) != 1) continue;
                            if (semi != ';' || two != '2') continue;  // not a Shift‑arrow

                            // Only allow panning when we are zoomed
                            if (std::abs(zoom_level - 1.0f) >= 0.001f) {
                                switch (dir) {
                                    case 'A': do_pan('K'); break;   // Shift‑↑
                                    case 'B': do_pan('J'); break;   // Shift‑↓
                                    case 'C': do_pan('L'); break;   // Shift‑→
                                    case 'D': do_pan('H'); break;   // Shift‑←
                                }
                            }
                            continue;   // consumed the whole sequence
                        }

                        // Plain arrows – page navigation (right/left)
                        if (seq[1] == 'C' && current_page + step < static_cast<int>(archive.page_count())) {
                            current_page += step;
                            navigate = true;
                        } else if (seq[1] == 'D' && current_page > 0) {
                            current_page = std::max(0, current_page - step);
                            navigate = true;
                        }
                    }
                }
                // ----- Regular keys (no ESC) -----
                else if (c == config.keymap["next"][0] && current_page + step < static_cast<int>(archive.page_count())) {
                    current_page += step;
                    navigate = true;
                }
                else if (c == config.keymap["prev"][0] && current_page > 0) {
                    current_page = std::max(0, current_page - step);
                    navigate = true;
                }
                else if (c == config.keymap["first_page"][0]) {
                    current_page = 0;
                    navigate = true;
                }
                else if (c == config.keymap["last_page"][0]) {
                    current_page = static_cast<int>(archive.page_count()) - 1;
                    navigate = true;
                }
                else if (c == config.keymap["toggle_spread"][0] || c == config.keymap["double_page"][0]) {
                    config.double_page = !config.double_page;
                    navigate = true;
                }
                else if (c == config.keymap["toggle_help"][0]) {
                    config.show_help = !config.show_help;
                    navigate = true;
                }
                else if (c == config.keymap["quit"][0]) {
                    // Leave comic view, go back to file list
                    viewing_comic = false;
                    archive.close();
                    page_cache.clear();
                    draw_file_list();
                    continue;
                }
                else if (c == config.keymap["zoom_in"][0]) {
                    zoom_level = std::min(zoom_level + 0.1f, 3.0f);
                    navigate = true;
                }
                else if (c == config.keymap["zoom_out"][0] || c == config.keymap["zoom_out_alt"][0]) {
                    zoom_level = std::max(zoom_level - 0.1f, 0.5f);
                    navigate = true;
                }
                else if (c == config.keymap["zoom_reset"][0]) {
                    zoom_level = 1.0f;
                    pan_x = pan_y = 0;
                    navigate = true;
                }
                else if (c == config.keymap["pan_up"][0]) {
                    do_pan('K');
                }
                else if (c == config.keymap["pan_down"][0]) {
                    do_pan('J');
                }
                else if (c == config.keymap["pan_left"][0]) {
                    do_pan('H');
                }
                else if (c == config.keymap["pan_right"][0]) {
                    do_pan('L');
                }

                // If anything changed that requires a redraw, do it now
                if (navigate) {
                    draw_comic_view();
                }
            }
        }

        // Clean up terminal state before exiting
        clear_screen();
        disable_raw_mode();
    }
};

// ===================================================================
//  main()
// ===================================================================
int main(int argc, char* argv[]) {
    std::string dir;

    // Load temporary config to see if a library path is stored
    Config temp_cfg;
    std::string home = getenv("HOME") ? getenv("HOME") : "";
    if (!home.empty())
        temp_cfg.load(home + "/.tcreader.conf");

    if (argc > 1) {
        dir = argv[1];                     // explicit argument wins
    } else if (!temp_cfg.library_paths.empty()) {
        dir = temp_cfg.library_paths[0];   // first library path from config
    } else {
        dir = ".";                         // fallback to current directory
    }

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::cerr << "Usage: tcreader [directory]\n";
        std::cerr << "Invalid directory: " << dir << "\n";
        std::cerr << "\nTip: Set library paths in ~/.tcreader.conf\n";
        return 1;
    }

    ComicReader reader(dir);
    reader.run();
    return 0;
}