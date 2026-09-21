/* sd_paths.h -- map every spelling of a game-data path onto the SD card. */
#ifndef SD_PATHS_H
#define SD_PATHS_H
#include <stddef.h>

/* Returns `path` unchanged, or a rewritten path in `buf`. Never NULL unless
 * `path` was NULL. `buf` must stay alive for as long as the result is used. */
const char *sd_remap_path(const char *path, char *buf, size_t n);

#endif /* SD_PATHS_H */
