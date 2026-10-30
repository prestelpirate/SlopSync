#ifndef STUBS_H
#define STUBS_H
#include <glib.h>
extern long stub_prefs[100];
int stub_seed_from_pdb(const char *path, const char *name);
void stub_hotsync(const char *name);
int stub_live_count(const char *name);
GList *stub_records(const char *name);
#endif
