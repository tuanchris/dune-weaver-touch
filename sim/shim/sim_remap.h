// Force-included into DEVICE sources only (-include). Rewrites the LittleFS
// mount point /storage/... to a host directory so thr_preview's cache works
// unmodified. shim.c is compiled without this header and provides sim_*.
#pragma once

#include <stdio.h>
#include <sys/stat.h>

FILE *sim_fopen(const char *path, const char *mode);
int sim_stat(const char *path, struct stat *st);
int sim_unlink(const char *path);
int sim_rename(const char *from, const char *to);
int sim_mkdir(const char *path, unsigned short mode);

#define fopen(path, mode) sim_fopen(path, mode)
#define stat(path, st) sim_stat(path, st)
#define unlink(path) sim_unlink(path)
#define rename(a, b) sim_rename(a, b)
#define mkdir(path, mode) sim_mkdir(path, mode)
