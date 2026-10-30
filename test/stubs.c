/* Test doubles for the J-Pilot functions the plugin calls.  Records live in
 * memory with the same record-type rules J-Pilot uses for its .pc3 files,
 * and can be seeded from a real .pdb. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <glib.h>
#include <pi-file.h>
#include <pi-calendar.h>
#include <pi-datebook.h>
#include <pi-address.h>
#include <pi-contact.h>
#include "libplugin.h"
#include "stubs.h"

static GHashTable *dbs;            /* name -> GList* of buf_rec* */
static unsigned int next_pc_id = 1;
static unsigned int next_palm_id = 0x900000;
static unsigned char *appinfo[8];
static int appinfo_len[8];
static char *appinfo_name[8];
static int n_appinfo;
long stub_prefs[100];

static GList **db(const char *name)
{
   GList **l;
   if (!dbs)
      dbs = g_hash_table_new(g_str_hash, g_str_equal);
   l = g_hash_table_lookup(dbs, name);
   if (!l) {
      l = g_new0(GList *, 1);
      g_hash_table_insert(dbs, g_strdup(name), l);
   }
   return l;
}

static buf_rec *br_dup(const buf_rec *b)
{
   buf_rec *n = g_new0(buf_rec, 1);
   *n = *b;
   n->buf = g_memdup2(b->buf, b->size);
   return n;
}

int stub_seed_from_pdb(const char *path, const char *name)
{
   pi_file_t *pf = pi_file_open(path);
   int n = 0, i, nrec = 0, attr, cat;
   size_t size;
   void *buf, *ai;
   size_t ailen;
   recordid_t uid;

   if (!pf)
      return -1;
   pi_file_get_entries(pf, &nrec);
   for (i = 0; i < nrec; i++) {
      buf_rec b;
      if (pi_file_read_record(pf, i, &buf, &size, &attr, &cat, &uid) < 0)
         continue;
      if (attr & 0x80)
         continue;
      memset(&b, 0, sizeof(b));
      b.rt = PALM_REC;
      b.unique_id = uid;
      b.attrib = (attr & 0xF0) | (cat & 0x0F);
      b.buf = buf;
      b.size = size;
      *db(name) = g_list_append(*db(name), br_dup(&b));
      n++;
   }
   pi_file_get_app_info(pf, &ai, &ailen);
   if (ai && n_appinfo < 8) {
      appinfo_name[n_appinfo] = g_strdup(name);
      appinfo[n_appinfo] = g_memdup2(ai, ailen);
      appinfo_len[n_appinfo++] = ailen;
   }
   pi_file_close(pf);
   return n;
}

/* Simulate a HotSync: PC changes become Palm records. */
void stub_hotsync(const char *name)
{
   GList *l, *out = NULL;
   for (l = *db(name); l; l = l->next) {
      buf_rec *b = l->data;
      if (b->rt == NEW_PC_REC) {
         b->rt = PALM_REC;
         b->unique_id = next_palm_id++;
      } else if (b->rt == REPLACEMENT_PALM_REC) {
         b->rt = PALM_REC;
      } else if (b->rt != PALM_REC) {
         g_free(b->buf);
         g_free(b);
         continue;
      }
      out = g_list_append(out, b);
   }
   g_list_free(*db(name));
   *db(name) = out;
}

int stub_live_count(const char *name)
{
   GList *l;
   int n = 0;
   for (l = *db(name); l; l = l->next) {
      buf_rec *b = l->data;
      if (b->rt == PALM_REC || b->rt == NEW_PC_REC || b->rt == REPLACEMENT_PALM_REC)
         n++;
   }
   return n;
}

GList *stub_records(const char *name)
{
   return *db(name);
}

/* ---- J-Pilot API ---- */

int jp_read_DB_files(const char *name, GList **records)
{
   GList *l;
   *records = NULL;
   for (l = *db(name); l; l = l->next)
      *records = g_list_append(*records, br_dup(l->data));
   return g_list_length(*records);
}

int jp_free_DB_records(GList **records)
{
   GList *l;
   for (l = *records; l; l = l->next) {
      g_free(((buf_rec *)l->data)->buf);
      g_free(l->data);
   }
   g_list_free(*records);
   *records = NULL;
   return 0;
}

int jp_pc_write(const char *name, buf_rec *br)
{
   if (br->unique_id == 0)
      br->unique_id = next_pc_id++;
   *db(name) = g_list_append(*db(name), br_dup(br));
   return EXIT_SUCCESS;
}

int jp_delete_record(const char *name, buf_rec *br, int flag)
{
   GList *l;
   for (l = *db(name); l; l = l->next) {
      buf_rec *b = l->data;
      if (b->unique_id != br->unique_id)
         continue;
      if (b->rt == PALM_REC) {
         b->rt = flag == MODIFY_FLAG ? MODIFIED_PALM_REC : DELETED_PALM_REC;
         return 0;
      }
      if (b->rt == NEW_PC_REC || b->rt == REPLACEMENT_PALM_REC) {
         b->rt = DELETED_PC_REC;
         return 0;
      }
   }
   return -1;
}

int jp_get_app_info(const char *name, unsigned char **buf, int *size)
{
   int i;
   *buf = NULL;
   *size = 0;
   for (i = 0; i < n_appinfo; i++)
      if (!strcmp(appinfo_name[i], name)) {
         *buf = malloc(appinfo_len[i]);
         memcpy(*buf, appinfo[i], appinfo_len[i]);
         *size = appinfo_len[i];
      }
   return 0;
}

int get_pref(int which, long *n, const char **s)
{
   if (n)
      *n = stub_prefs[which];
   return 0;
}

int jp_logf(int level, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   return 0;
}

void jp_init(void) {}

/* Palm side is Windows-1252 in this setup */
char *jp_charset_p2newj(const char *buf, int max_len)
{
   char *r = g_convert(buf, -1, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
   return r ? r : g_strdup(buf);
}

void jp_charset_j2p(char *buf, int max_len)
{
   char *r = g_convert_with_fallback(buf, -1, "WINDOWS-1252", "UTF-8", "?", NULL, NULL, NULL);
   if (r) {
      g_strlcpy(buf, r, max_len);
      g_free(r);
   }
}

int jp_unpack_Contact(struct Contact *c, pi_buffer_t *b) { return unpack_Contact(c, b, contacts_v10); }
int jp_pack_Contact(struct Contact *c, pi_buffer_t *b) { return pack_Contact(c, b, contacts_v10); }
void jp_free_Contact(struct Contact *c) { free_Contact(c); }

/* Datebook/Address are not exercised by the tests */
int copy_appointment_to_calendarEvent(const struct Appointment *a, struct CalendarEvent *c) { return -1; }
int copy_calendarEvent_to_appointment(const struct CalendarEvent *c, struct Appointment *a) { return -1; }
int copy_address_to_contact(const struct Address *a, struct Contact *c) { return -1; }
int copy_contact_to_address(const struct Contact *c, struct Address *a) { return -1; }
