// Test helper (#93): sends keys and pointer events through KWin's fake input. Only for a nested,
// throw-away KWin (tests/nested/session.sh grants it the interface through a desktop file).
// Usage: itsova-fakekey <step>...  steps: <keycode>[+<keycode>...] ("29+46" = Ctrl+C), m:X,Y
// (absolute pointer), d / u (left button down / up), w:MS (wait).
#include "fake-input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static struct org_kde_kwin_fake_input *fake;
static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    (void)d;
    if (strcmp(iface, org_kde_kwin_fake_input_interface.name) == 0)
        fake = wl_registry_bind(r, name, &org_kde_kwin_fake_input_interface, v < 4 ? v : 4);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener listener = {global, global_remove};

int main(int argc, char **argv)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "no display\n"); return 1; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    wl_display_roundtrip(display);
    if (!fake) { fprintf(stderr, "no fake input\n"); return 2; }
    org_kde_kwin_fake_input_authenticate(fake, "fakekey", "itsova panel test");
    for (int i = 1; i < argc; ++i) {
        int x, y, ms;
        if (sscanf(argv[i], "m:%d,%d", &x, &y) == 2) { /* absolute pointer position (#79) */
            org_kde_kwin_fake_input_pointer_motion_absolute(fake, wl_fixed_from_int(x), wl_fixed_from_int(y));
            wl_display_roundtrip(display);
            usleep(150000);
            continue;
        }
        if (strcmp(argv[i], "d") == 0 || strcmp(argv[i], "u") == 0) { /* left button down/up (#30) */
            org_kde_kwin_fake_input_button(fake, 0x110, argv[i][0] == 'd' ? 1 : 0);
            wl_display_roundtrip(display);
            usleep(150000);
            continue;
        }
        if (sscanf(argv[i], "w:%d", &ms) == 1) { /* wait */
            usleep(ms * 1000);
            continue;
        }
        int codes[8], n = 0;
        char *arg = strdup(argv[i]);
        for (char *t = strtok(arg, "+"); t && n < 8; t = strtok(NULL, "+")) codes[n++] = atoi(t);
        for (int k = 0; k < n; ++k) org_kde_kwin_fake_input_keyboard_key(fake, codes[k], 1);
        for (int k = n - 1; k >= 0; --k) org_kde_kwin_fake_input_keyboard_key(fake, codes[k], 0);
        wl_display_roundtrip(display);
        free(arg);
        usleep(150000);
    }
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
