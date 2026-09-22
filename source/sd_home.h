/* sd_home.h -- the game folder, decided at launch (sd_home.c). */
#ifndef SD_HOME_H
#define SD_HOME_H
#include <stddef.h>

#define SD_DEFAULT_HOME "sdmc:/switch/sonicdash_nx"   /* when the launcher passes no usable path */

const char *sd_home(void);          /* e.g. "sdmc:/switch/sonic_nx" -- no trailing slash */
const char *sd_home_source(void);   /* why that folder, for the boot log */
void sd_home_init(int argc, char **argv);          /* first thing in main() */
int  sd_home_from_argv0(const char *argv0, char *out, size_t n);  /* pure; host-tested */
#endif
