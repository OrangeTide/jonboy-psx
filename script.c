#include "script.h"
#include "pad.h"
#include "cdrom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See script.h for the format. The parser splits events into two kinds: pad
// keyframes, whose state is held until the next pad keyframe, and one-shot events
// (analog mode, disc insert/eject), each fired once when its frame is reached.

#define MAX_PAD_KEYFRAMES 4096
#define MAX_EVENTS        4096
#define MAX_PATH          1024

typedef struct {
    uint32_t frame;
    uint16_t buttons; // active low, 0xFFFF = nothing pressed
    uint8_t lx, ly, rx, ry;
} PadKeyframe;

typedef enum { EV_ANALOG, EV_DISC_INSERT, EV_DISC_EJECT } EventType;

typedef struct {
    uint32_t frame;
    EventType type;
    int arg;              // EV_ANALOG: 0/1
    char path[MAX_PATH];  // EV_DISC_INSERT
} Event;

static struct {
    PadKeyframe pad[MAX_PAD_KEYFRAMES];
    int pad_count;

    Event ev[MAX_EVENTS];
    int ev_count;
    int ev_cursor;        // next one-shot event to fire

    uint32_t last_frame;  // to detect a frame-counter restart (reboot)
    int active;
} script;

static const struct {
    const char *name;
    uint16_t bit;
} btn_names[] = {
    { "select", PAD_BTN_SELECT }, { "start", PAD_BTN_START },
    { "up", PAD_BTN_UP }, { "down", PAD_BTN_DOWN },
    { "left", PAD_BTN_LEFT }, { "right", PAD_BTN_RIGHT },
    { "l1", PAD_BTN_L1 }, { "r1", PAD_BTN_R1 },
    { "l2", PAD_BTN_L2 }, { "r2", PAD_BTN_R2 },
    { "l3", PAD_BTN_L3 }, { "r3", PAD_BTN_R3 },
    { "triangle", PAD_BTN_TRIANGLE }, { "circle", PAD_BTN_CIRCLE },
    { "cross", PAD_BTN_CROSS }, { "square", PAD_BTN_SQUARE },
};

// Match a stick token like "lx=40" or "ry=0xC0"; stores the value if `tok` sets
// the axis named `axis`.
static int
match_stick(const char *tok, const char *axis, uint8_t *out)
{
    size_t n = strlen(axis);
    if (strncmp(tok, axis, n) != 0 || tok[n] != '=') {
        return 0;
    }
    long v = strtol(tok + n + 1, NULL, 0);
    if (v < 0) {
        v = 0;
    }
    if (v > 255) {
        v = 255;
    }
    *out = (uint8_t)v;
    return 1;
}

// Parse the remaining tokens of a "pad" line into a keyframe.
static void
parse_pad(PadKeyframe *kf)
{
    kf->buttons = 0xFFFF;
    kf->lx = kf->ly = kf->rx = kf->ry = 0x80;

    char *tok;
    while ((tok = strtok(NULL, " \t\r\n")) != NULL) {
        if (strcmp(tok, "none") == 0) {
            continue;
        }
        if (match_stick(tok, "lx", &kf->lx) || match_stick(tok, "ly", &kf->ly) ||
            match_stick(tok, "rx", &kf->rx) || match_stick(tok, "ry", &kf->ry)) {
            continue;
        }
        int matched = 0;
        for (size_t i = 0; i < sizeof btn_names / sizeof btn_names[0]; i++) {
            if (strcmp(tok, btn_names[i].name) == 0) {
                kf->buttons &= (uint16_t)~btn_names[i].bit;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            fprintf(stderr, "script: unknown pad token '%s'\n", tok);
        }
    }
}

static int
pad_cmp(const void *a, const void *b)
{
    uint32_t fa = ((const PadKeyframe *)a)->frame;
    uint32_t fb = ((const PadKeyframe *)b)->frame;
    return (fa > fb) - (fa < fb);
}

static int
ev_cmp(const void *a, const void *b)
{
    uint32_t fa = ((const Event *)a)->frame;
    uint32_t fb = ((const Event *)b)->frame;
    return (fa > fb) - (fa < fb);
}

// Add one line's event. `frame` is parsed; strtok is positioned after it.
static void
parse_line(uint32_t frame)
{
    char *cmd = strtok(NULL, " \t\r\n");
    if (!cmd) {
        return;
    }

    if (strcmp(cmd, "pad") == 0) {
        if (script.pad_count < MAX_PAD_KEYFRAMES) {
            PadKeyframe *kf = &script.pad[script.pad_count++];
            kf->frame = frame;
            parse_pad(kf);
        }
        return;
    }

    if (script.ev_count >= MAX_EVENTS) {
        return;
    }
    Event *e = &script.ev[script.ev_count];
    e->frame = frame;
    e->path[0] = '\0';

    if (strcmp(cmd, "analog") == 0) {
        char *arg = strtok(NULL, " \t\r\n");
        e->type = EV_ANALOG;
        e->arg = (arg && strcmp(arg, "on") == 0) ? 1 : 0;
        script.ev_count++;
    } else if (strcmp(cmd, "disc") == 0) {
        char *arg = strtok(NULL, " \t\r\n");
        if (arg && strcmp(arg, "eject") == 0) {
            e->type = EV_DISC_EJECT;
        } else if (arg) {
            e->type = EV_DISC_INSERT;
            snprintf(e->path, sizeof e->path, "%s", arg);
        } else {
            fprintf(stderr, "script: disc needs a path or 'eject'\n");
            return;
        }
        script.ev_count++;
    } else {
        fprintf(stderr, "script: unknown command '%s'\n", cmd);
    }
}

int
script_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }

    memset(&script, 0, sizeof script);

    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') {
            continue;
        }

        char *tok = strtok(p, " \t\r\n");
        if (!tok) {
            continue;
        }
        uint32_t frame = (uint32_t)strtoul(tok, NULL, 0);
        parse_line(frame);
    }
    fclose(f);

    if (script.pad_count == 0 && script.ev_count == 0) {
        fprintf(stderr, "script: %s has no events\n", path);
        return -1;
    }

    qsort(script.pad, script.pad_count, sizeof script.pad[0], pad_cmp);
    qsort(script.ev, script.ev_count, sizeof script.ev[0], ev_cmp);
    script.active = 1;
    return 0;
}

int
script_active(void)
{
    return script.active;
}

static void
fire_event(const Event *e)
{
    switch (e->type) {
    case EV_ANALOG:
        if (pad_analog_mode() != e->arg) {
            pad_toggle_analog();
        }
        break;
    case EV_DISC_INSERT:
        if (cdrom_mount(e->path) == 0) {
            printf("script: inserted disc %s\n", e->path);
        } else {
            fprintf(stderr, "script: failed to insert %s\n", e->path);
        }
        break;
    case EV_DISC_EJECT:
        cdrom_eject();
        printf("script: ejected disc\n");
        break;
    }
}

// Apply the pad keyframe active at `frame` to the controller state.
static void
apply_pad(uint32_t frame)
{
    if (script.pad_count == 0) {
        return;
    }

    int idx = -1;
    for (int i = 0; i < script.pad_count; i++) {
        if (script.pad[i].frame <= frame) {
            idx = i;
        } else {
            break;
        }
    }

    if (idx < 0) {
        pad_buttons = 0xFFFF;
        pad_lstick_x = pad_lstick_y = 0x80;
        pad_rstick_x = pad_rstick_y = 0x80;
        return;
    }

    const PadKeyframe *kf = &script.pad[idx];
    pad_buttons = kf->buttons;
    pad_lstick_x = kf->lx;
    pad_lstick_y = kf->ly;
    pad_rstick_x = kf->rx;
    pad_rstick_y = kf->ry;
}

void
script_frame(uint32_t frame)
{
    if (!script.active) {
        return;
    }

    // A frame counter that went backwards means the machine rebooted; replay the
    // script from the start.
    if (frame < script.last_frame) {
        script.ev_cursor = 0;
    }
    script.last_frame = frame;

    apply_pad(frame);

    while (script.ev_cursor < script.ev_count &&
           script.ev[script.ev_cursor].frame <= frame) {
        fire_event(&script.ev[script.ev_cursor++]);
    }
}
