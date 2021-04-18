#include "mouse.hpp"

#include "graphics.hpp"
#include "logger.hpp"
namespace {
// mouse cursor
const char mouse_cursor_shape[kMouseCursorHeight][kMouseCursorWidth + 1] = {
    // clang-format off
    ".              ",
    "..             ",
    ".@.            ",
    ".@@.           ",
    ".@@@.          ",
    ".@@@@.         ",
    ".@@@@@.        ",
    ".@@@@@@.       ",
    ".@@@@@@@.      ",
    ".@@@@@@@@.     ",
    ".@@@@@@@@@.    ",
    ".@@@@@@@@@@.   ",
    ".@@@@@@@@@@@.  ",
    ".@@@@@@@@@@@@. ",
    ".@@@@@@........",
    ".@@@@@@.       ",
    ".@@@@..@.      ",
    ".@@@. .@.      ",
    ".@@.   .@.     ",
    ".@.    .@.     ",
    "..      .@.    ",
    ".       .@.    ",
    "         .@.   ",
    "         ...   ",
    // clang-format on
};

} // namespace

void DrawMouseCursor(PixelWriter *pixel_writer, Vector2D<int> position) {
    for (int dy = 0; dy < kMouseCursorHeight; ++dy) {
        for (int dx = 0; dx < kMouseCursorWidth; ++dx) {
            if (mouse_cursor_shape[dy][dx] == '@') {
                Log(kDebug, "@");
                pixel_writer->Write(position.x + dx, position.y + dy,
                                    {0, 0, 0});
            } else if (mouse_cursor_shape[dy][dx] == '.') {
                Log(kDebug, ".");
                pixel_writer->Write(position.x + dx, position.y + dy,
                                    {255, 255, 255});
            } else {
                Log(kDebug, "$");
                pixel_writer->Write(position.x + dx, position.y + dy,
                                    kMouseTransparentColor);
            }
        }
    }
}