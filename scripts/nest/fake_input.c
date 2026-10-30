// Test tool: inject input into a (nested, throwaway) KWin via org_kde_kwin_fake_input.
// usage: fake_input move X Y | click [BTN] | down BTN | up BTN | key EVDEV_CODE | wheel N ...   (commands run in order)
#include <wayland-client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fake_input-client-protocol.h"
static struct org_kde_kwin_fake_input *fi;
static void reg(void *d, struct wl_registry *r, uint32_t id, const char *i, uint32_t v) {
    if (!strcmp(i, "org_kde_kwin_fake_input")) fi = wl_registry_bind(r, id, &org_kde_kwin_fake_input_interface, 4);
}
static void rm(void *d, struct wl_registry *r, uint32_t id) {}
static const struct wl_registry_listener rl = {reg, rm};
int main(int c, char **v) {
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    wl_registry_add_listener(wl_display_get_registry(d), &rl, NULL);
    wl_display_roundtrip(d);
    if (!fi) { fprintf(stderr, "fake_input not offered\n"); return 2; }
    org_kde_kwin_fake_input_authenticate(fi, "wuwa-test", "verification");
    for (int i = 1; i < c; i++) {
        if (!strcmp(v[i], "move")) { org_kde_kwin_fake_input_pointer_motion_absolute(fi, wl_fixed_from_double(atof(v[i+1])), wl_fixed_from_double(atof(v[i+2]))); i += 2; }
        else if (!strcmp(v[i], "click")) { int b = (i+1<c && atoi(v[i+1])) ? atoi(v[++i]) : 0x110; org_kde_kwin_fake_input_button(fi, b, 1); wl_display_flush(d); usleep(60000); org_kde_kwin_fake_input_button(fi, b, 0); }
        else if (!strcmp(v[i], "down")) { org_kde_kwin_fake_input_button(fi, atoi(v[i+1]), 1); i++; }
        else if (!strcmp(v[i], "up")) { org_kde_kwin_fake_input_button(fi, atoi(v[i+1]), 0); i++; }
        else if (!strcmp(v[i], "key")) { int k = atoi(v[++i]); org_kde_kwin_fake_input_keyboard_key(fi, k, 1); wl_display_flush(d); usleep(60000); org_kde_kwin_fake_input_keyboard_key(fi, k, 0); }
        else if (!strcmp(v[i], "wheel")) { org_kde_kwin_fake_input_axis(fi, 0, wl_fixed_from_double(atof(v[++i]))); }
        else if (!strcmp(v[i], "sleep")) { wl_display_flush(d); usleep(atoi(v[++i]) * 1000); }
        wl_display_flush(d); usleep(80000);
    }
    wl_display_roundtrip(d);
    return 0;
}
