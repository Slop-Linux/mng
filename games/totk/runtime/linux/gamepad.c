/* Linux controllers and per-game settings files. The settings are the one-word files next to TOTK_CONTROLS, as on the
   Mac (runtime/gamepad.m). Controllers: none yet (keyboard input through runtime/input.c). */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int setting_path(const char *name, char *out, size_t size) {
    const char *controls = getenv("TOTK_CONTROLS");
    if (!controls || !*controls) return 0;
    const char *slash = strrchr(controls, '/');
    int n = slash ? snprintf(out, size, "%.*s/%s", (int)(slash - controls), controls, name) : snprintf(out, size, "%s", name);
    return n > 0 && (size_t)n < size;
}
int runtime_setting_word(const char *name, char *word, size_t size) {
    char path[4096]; if (!setting_path(name, path, sizeof path)) return 0;
    FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(word, 1, size - 1, f); fclose(f); word[n] = 0; return 1;
}
void runtime_setting_save(const char *name, const char *word, const char *what) {
    char path[4096], temp[4200]; if (!setting_path(name, path, sizeof path)) return;
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *f = fopen(temp, "w"); size_t n = strlen(word);
    int ok = f && fprintf(f, "%s%s", word, n && word[n - 1] == '\n' ? "" : "\n") >= 0;
    if (f && fclose(f)) ok = 0;
    if (!ok || rename(temp, path)) { fprintf(stderr, "[totk-input] could not save the %s to %s\n", what, path); unlink(temp); }
}
void runtime_setting_remove(const char *name) { char path[4096]; if (setting_path(name, path, sizeof path)) unlink(path); }

void runtime_gamepad_start(void) {}
void runtime_gamepad_read(RuntimeGamepadState *out) { memset(out, 0, sizeof *out); }
void runtime_gamepad_clear(void) {}
int runtime_mouse_enabled(void) { return 0; }
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) { (void)side; (void)amp_low; (void)freq_low; (void)amp_high; (void)freq_high; }
