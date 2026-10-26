#include "libpieg.h"

#define MAX_FILE_SIZE 8192

// Static buffers to keep the kernel stack clean
static char file_buf[MAX_FILE_SIZE];
static char path_buf[256];
static uint32_t buf_len = 0;

// Render the editor frame, content, and the interactive bottom menu
static void draw_interface(const char *filename)
{
    pieg->clear();

    // Header Bar (Nano-style)
    p_puts(" Microcephaly (mc) |  File: ");
    p_putln(filename);
    p_putln("--------------------------------------------------------------------------------");

    // Print file contents onto the UEFI framebuffer
    if (buf_len > 0) {
        file_buf[buf_len] = '\0';
        p_puts(file_buf);
    }

    p_putln("\n");

    // Interactive Shortcuts Bar
    p_putln("--------------------------------------------------------------------------------");
    p_putln(" ^X Exit          ^S Save File       ^O WriteOut        [Type text freely]");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        p_putln("Usage: mc <filename>");
        return 1;
    }

    // Resolve user argument into a clean absolute path
    pieg->resolve(argv[1], path_buf, sizeof(path_buf));

    // Try loading an existing file from the filesystem
    uint32_t bytes_read = 0;
    int r = pieg->fs_read(path_buf, 0, file_buf, sizeof(file_buf) - 2, &bytes_read);

    if (!r) {
        buf_len = bytes_read;
    } else {
        // If file doesn't exist, start fresh with an empty buffer
        buf_len = 0;
    }

    // Main text-editing operational loop
    while (1) {
        draw_interface(argv[1]);

        // Blocking keyboard hook
        char c = p_getc();

        // KEYBINDING: Ctrl+X (ASCII 24) -> Exit application
        if (c == 24) {
            pieg->clear();
            p_putln("Microcephaly closed safely.");
            return 0;
        }

        // KEYBINDING: Ctrl+S (ASCII 19) -> Instantly write buffer to current file
        if (c == 19) {
            int save_res = pieg->fs_write(path_buf, file_buf, buf_len, 0);
            if (save_res) {
                p_puts("\nError saving file: ");
                p_putln(pieg->fs_error(save_res));
                p_getc(); // Wait for user acknowledgment
            }
            continue;
        }

        // KEYBINDING: Ctrl+O (ASCII 15) -> "WriteOut" confirmation flash
        if (c == 15) {
            p_puts("\nWriting current buffer to disk...");
            int save_res = pieg->fs_write(path_buf, file_buf, buf_len, 0);
            if (!save_res) {
                p_puts(" [OK] Saved!");
            } else {
                p_puts(" [FAILED]: ");
                p_puts(pieg->fs_error(save_res));
            }
            // Brief visual pause using non-blocking keyboard bypass or simple loop
            for(volatile int i=0; i<50000000; i++);
            continue;
        }

        // KEYBINDING: Backspace / Delete handling (ASCII 8 or 127)
        if (c == 8 || c == 127) {
            if (buf_len > 0) {
                buf_len--;
                file_buf[buf_len] = '\0';
            }
        }
        // Standard Character Append (Filtering displayable ASCII, newlines, or carriage returns)
        else if (c >= 32 || c == '\n' || c == '\r') {
            if (buf_len < (MAX_FILE_SIZE - 2)) {
                // Adapt carriage returns to standard PIEG system newlines
                if (c == '\r') c = '\n';

                file_buf[buf_len] = c;
                buf_len++;
            }
        }
    }

    return 0;
}
