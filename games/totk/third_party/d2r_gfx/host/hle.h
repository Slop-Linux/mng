#pragma once
#ifndef DM_SEC
#define DM_SEC(name) "__DATA," name   /* Linux: darwin_compat.h */
#endif
#include "dm.h"
typedef struct {const char *name;void *fn;const char *why;} hle_force_ent;
#define HLE_FORCE(mangled,reason,ret,cname,...) static ret cname(__VA_ARGS__); __attribute__((used,section(DM_SEC("totkgfx")))) static const hle_force_ent ent_##cname={mangled,(void *)cname,reason}; static ret cname(__VA_ARGS__)
