#include "psio.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Host unit test for the loader game index (psio.c): scanning a directory for
// bootable entries, classifying PS-EXE vs CD image, and reporting the boot type
// and path for a selected entry. psio.c is self-contained, so it links alone.

static int failures;

static void
check(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d want %d\n", what, got, want);
        failures++;
    }
}

static void
touch(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fclose(f);
    }
}

int
main(void)
{
    const char *dir = "_out/_ldrtest";
    mkdir(dir, 0777);

    // Sorted by name: a.exe (EXE), b.cue (CD), c.iso (CD). readme.txt is ignored.
    touch(dir, "a.exe");
    touch(dir, "b.cue");
    touch(dir, "c.iso");
    touch(dir, "readme.txt");

    loader_set_games_dir(dir);
    loader_write(LDR_REG_CMD, LDR_CMD_REFRESH, 4);

    int count = (int)loader_read(LDR_REG_INDEX, 4);
    check("entry count (exe+cue+iso, txt ignored)", count, 3);

    // Entry 0: a.exe is a PS-EXE.
    loader_write(LDR_REG_INDEX, 0, 4);
    check("entry 0 not CD", loader_boot_is_cd(), 0);

    // Entry 1: b.cue is a CD image.
    loader_write(LDR_REG_INDEX, 1, 4);
    check("entry 1 is CD", loader_boot_is_cd(), 1);
    check("entry 1 path ends .cue",
          strcmp(loader_boot_path() + strlen(loader_boot_path()) - 4, ".cue") == 0, 1);

    // Entry 2: c.iso is a CD image.
    loader_write(LDR_REG_INDEX, 2, 4);
    check("entry 2 is CD", loader_boot_is_cd(), 1);

    // The name FIFO returns the selected entry's display name.
    loader_write(LDR_REG_INDEX, 1, 4);
    loader_write(LDR_REG_CMD, LDR_CMD_NAME, 4);
    char name[32];
    int i = 0;
    while ((loader_read(LDR_REG_CMD, 4) & LDR_STAT_DATA) && i < 31) {
        name[i++] = (char)loader_read(LDR_REG_DATA, 4);
    }
    name[i] = '\0';
    check("name FIFO returns b.cue", strcmp(name, "b.cue") == 0, 1);

    // Boot strobe marks the selection pending.
    loader_write(LDR_REG_CMD, LDR_CMD_BOOT, 4);
    check("boot pending after strobe", loader_boot_pending(), 1);
    check("pending entry is CD", loader_boot_is_cd(), 1);
    loader_clear_boot();
    check("boot cleared", loader_boot_pending(), 0);

    remove("_out/_ldrtest/a.exe");
    remove("_out/_ldrtest/b.cue");
    remove("_out/_ldrtest/c.iso");
    remove("_out/_ldrtest/readme.txt");
    rmdir(dir);

    printf("%s (%d failure%s)\n",
           failures ? "FAILED" : "PASS: loader indexes and classifies CD images",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
