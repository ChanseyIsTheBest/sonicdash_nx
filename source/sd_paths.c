/* sd_paths.c -- the streaming-assets path remap.
 *
 * THE FAILURE THIS FIXES (second hardware boot, stuck on the SEGA splash):
 * Unity on Android addresses streaming assets as
 *     jar:file://<path-to-APK>!/assets/<file>
 * There is no APK here, and the <path-to-APK> part came out EMPTY. Unity's own
 * jar-URL handling then reduced the URL to a root-relative path and opened
 *     /assets/aa/settings.json          -> -1
 *     /assets/Language Strings/Auto/EnglishUS.bytes -> -1
 * at the root of the SD card instead of under our folder. Addressables got no
 * runtime data ("RuntimeData is null"), every key failed ("No Location found
 * for Key=menu"), and the game sat on its splash with the render loop ticking.
 * Every bundle path in the catalogue has the same shape, so every bundle would
 * have failed the same way.
 *
 * phigros_nx met exactly this, and its libc_shim.c documents the same log
 * line. Its answer, followed here: do not try to make Unity compute the right
 * APK path -- normalise at the I/O layer, so whatever spelling reaches us is
 * mapped onto the real data root:
 *
 *     jar:file://<anything>!/assets/X   ->  GAME_HOME/assets/X
 *     /assets/X                         ->  GAME_HOME/assets/X
 *     file:///P                         ->  /P
 *     anything else                     ->  unchanged
 *
 * "/assets/" at the root of a Switch SD card is not a real location, so there
 * is nothing to shadow (phigros_nx's reasoning, and the same holds here). Only
 * the exact prefix "/assets/" matches: "/assetsX" does not.
 */
#include <stdio.h>
#include <string.h>
#include "sd_paths.h"
#include "config.h"

#ifndef SD_PATHS_NO_LOG
#include "util.h"
#define PLOG(...) diag_log(__VA_ARGS__)
#else
#define PLOG(...) ((void)0)
#endif

const char *sd_remap_path(const char *path, char *buf, size_t n)
{
    if (!path) return path;
    const char *p = path;

    if (!strncmp(p, "jar:", 4)) {
        const char *bang = strstr(p, "!/");
        if (!bang) return path;            /* not a jar URL we understand */
        p = bang + 1;                      /* keep the '/' of "/assets/..." */
    } else if (!strncmp(p, "file:", 5) && p[5] == '/' && p[6] == '/') {
        p += 7;                            /* file:///x -> /x */
    }

    if (!strncmp(p, "/assets/", 8)) {
        int len = snprintf(buf, n, "%s%s", GAME_HOME, p);
        if (len < 0 || (size_t)len >= n) {
            /* Truncating would open the WRONG file. Better an honest miss. */
            PLOG("[path] remap too long for buffer (%d): %s", len, path);
            return path;
        }
        static int logged;
        if (logged < 8) { logged++; PLOG("[path] %s -> %s", path, buf); }
        return buf;
    }
    return p;                              /* scheme stripped, or unchanged */
}
