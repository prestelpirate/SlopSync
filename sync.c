/* radicalesync - two-way sync engine
 *
 * For every enabled collection the engine compares three things:
 *   - the J-Pilot database (records read through the plugin API),
 *   - the server collection (PROPFIND for href + ETag),
 *   - the state file from the last successful sync, which links a Palm
 *     unique_id to a server href and remembers the record hash and ETag
 *     seen at that time.
 * A side "changed" when its hash/ETag differs from the state file.
 * Changes on one side are copied to the other; when both sides changed
 * the configured conflict policy decides.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include <pi-dlp.h>
#include <pi-datebook.h>
#include <pi-address.h>
#include "libplugin.h"
#include "rs.h"

/* ---------- J-Pilot internals exported by the jpilot binary ---------- */

#define PREF_CHAR_SET 27
#define PREF_DATEBOOK_VERSION 86
#define PREF_ADDRESS_VERSION 87
#define PREF_TODO_VERSION 88

extern int get_pref(int which, long *n, const char **string);
/* set when J-Pilot runs with -S/--sqlite; weak so older builds still load */
extern int glob_sqlite __attribute__((weak));
extern void jp_charset_j2p(char *buf, int max_len);
extern char *jp_charset_p2newj(const char *buf, int max_len);
extern int jp_unpack_Contact(struct Contact *, pi_buffer_t *);
extern int jp_pack_Contact(struct Contact *, pi_buffer_t *);
extern void jp_free_Contact(struct Contact *);
extern int copy_appointment_to_calendarEvent(const struct Appointment *a,
                                             struct CalendarEvent *cale);
extern int copy_calendarEvent_to_appointment(const struct CalendarEvent *cale,
                                             struct Appointment *a);
extern int copy_address_to_contact(const struct Address *a, struct Contact *c);
extern int copy_contact_to_address(const struct Contact *c, struct Address *a);

#define CATEGORY_MASK 0x0F

/* ---------- record kinds ---------- */

typedef struct Kind Kind;
struct Kind {
   RsKind kind;
   const char *label;          /* for log messages */
   const char *db_name;
   const char *component;      /* VEVENT / VTODO / NULL for vCard */
   const char *ext;
   const char *content_type;
   gboolean carddav;
   int version;                /* J-Pilot app version (Datebook/Calendar ...) */
};

typedef union {
   struct CalendarEvent ev;
   struct ToDo td;
   struct Contact c;
} Rec;

/* string fields of a record, for charset conversion */
static int rec_strings(const Kind *k, Rec *r, char **fields[40])
{
   int n = 0, i;

   switch (k->kind) {
    case RS_KIND_EVENT:
      fields[n++] = &r->ev.description;
      fields[n++] = &r->ev.note;
      fields[n++] = &r->ev.location;
      break;
    case RS_KIND_TODO:
      fields[n++] = &r->td.description;
      fields[n++] = &r->td.note;
      break;
    case RS_KIND_CONTACT:
      for (i = 0; i < 39; i++)
         fields[n++] = &r->c.entry[i];
      break;
   }
   return n;
}

static void rec_to_utf8(const Kind *k, Rec *r)
{
   char **fields[40];
   int n = rec_strings(k, r, fields), i;

   for (i = 0; i < n; i++) {
      char **f = fields[i];
      if (*f) {
         char *u = jp_charset_p2newj(*f, -1);
         free(*f);
         *f = strdup(u);
         g_free(u);
      }
   }
}

static void rec_to_palm(const Kind *k, Rec *r)
{
   char **fields[40];
   int n = rec_strings(k, r, fields), i;

   for (i = 0; i < n; i++) {
      char **f = fields[i];
      if (*f) {
         int len = strlen(*f) * 2 + 1;
         char *b = malloc(len);
         strcpy(b, *f);
         jp_charset_j2p(b, len);
         free(*f);
         *f = b;
      }
   }
}

static void rec_free(const Kind *k, Rec *r)
{
   switch (k->kind) {
    case RS_KIND_EVENT:   free_CalendarEvent(&r->ev); break;
    case RS_KIND_TODO:    free_ToDo(&r->td); break;
    case RS_KIND_CONTACT: jp_free_Contact(&r->c); break;
   }
   memset(r, 0, sizeof(*r));
}

static void rec_init(const Kind *k, Rec *r)
{
   memset(r, 0, sizeof(*r));
   if (k->kind == RS_KIND_EVENT) {
      r->ev.repeatFrequency = 1;
      r->ev.repeatForever = 1;
      r->ev.advance = 5;
   } else if (k->kind == RS_KIND_TODO) {
      r->td.priority = 1;
      r->td.indefinite = 1;
   }
}

/* Palm record bytes -> Rec (UTF-8).  Returns 0 on success. */
static int rec_unpack(const Kind *k, const buf_rec *br, Rec *r)
{
   pi_buffer_t *buf = pi_buffer_new(br->size);
   int ret = -1;

   rec_init(k, r);
   pi_buffer_append(buf, br->buf, br->size);
   switch (k->kind) {
    case RS_KIND_EVENT:
      if (k->version == 1) {
         ret = unpack_CalendarEvent(&r->ev, buf, calendar_v1) < 0 ? -1 : 0;
      } else {
         struct Appointment a;
         memset(&a, 0, sizeof(a));
         if (unpack_Appointment(&a, buf, datebook_v1) >= 0) {
            copy_appointment_to_calendarEvent(&a, &r->ev);
            ret = 0;
         }
         free_Appointment(&a);
      }
      break;
    case RS_KIND_TODO:
      ret = unpack_ToDo(&r->td, buf, todo_v1) < 0 ? -1 : 0;
      break;
    case RS_KIND_CONTACT:
      if (k->version == 1) {
         ret = jp_unpack_Contact(&r->c, buf) < 0 ? -1 : 0;
      } else {
         struct Address a;
         memset(&a, 0, sizeof(a));
         if (unpack_Address(&a, buf, address_v1) >= 0) {
            copy_address_to_contact(&a, &r->c);
            ret = 0;
         }
         free_Address(&a);
      }
      break;
   }
   pi_buffer_free(buf);
   if (ret == 0)
      rec_to_utf8(k, r);
   return ret;
}

/* Rec (UTF-8, converted to Palm charset in place) -> new pi_buffer */
static pi_buffer_t *rec_pack(const Kind *k, Rec *r)
{
   pi_buffer_t *buf = pi_buffer_new(0xFFFF);
   int ret = -1;

   rec_to_palm(k, r);
   switch (k->kind) {
    case RS_KIND_EVENT:
      if (k->version == 1) {
         ret = pack_CalendarEvent(&r->ev, buf, calendar_v1);
      } else {
         struct Appointment a;
         memset(&a, 0, sizeof(a));
         copy_calendarEvent_to_appointment(&r->ev, &a);
         ret = pack_Appointment(&a, buf, datebook_v1);
         free_Appointment(&a);
      }
      break;
    case RS_KIND_TODO:
      ret = pack_ToDo(&r->td, buf, todo_v1);
      break;
    case RS_KIND_CONTACT:
      if (k->version == 1) {
         ret = jp_pack_Contact(&r->c, buf);
      } else {
         struct Address a;
         memset(&a, 0, sizeof(a));
         copy_contact_to_address(&r->c, &a);
         ret = pack_Address(&a, buf, address_v1);
         free_Address(&a);
      }
      break;
   }
   if (ret < 0) {
      pi_buffer_free(buf);
      return NULL;
   }
   return buf;
}

static char *rec_to_text(const Kind *k, const Rec *r, const char *uid,
                         const char *cat, int priv, const char *merge)
{
   switch (k->kind) {
    case RS_KIND_EVENT:   return rs_event_to_ical(&r->ev, uid, cat, priv, merge);
    case RS_KIND_TODO:    return rs_todo_to_ical(&r->td, uid, cat, priv, merge);
    case RS_KIND_CONTACT: return rs_contact_to_vcard(&r->c, uid, cat, priv, merge);
   }
   return NULL;
}

static int rec_from_text(const Kind *k, const char *text, Rec *r,
                         char **cat, int *priv)
{
   switch (k->kind) {
    case RS_KIND_EVENT:   return rs_ical_to_event(text, &r->ev, cat, priv);
    case RS_KIND_TODO:    return rs_ical_to_todo(text, &r->td, cat, priv);
    case RS_KIND_CONTACT: return rs_vcard_to_contact(text, &r->c, cat, priv);
   }
   return -1;
}

/* A short key used to pair records on the first sync so existing data
 * on both sides is linked instead of duplicated. */
static char *rec_match_key(const Kind *k, const Rec *r)
{
   char *s, *key;

   switch (k->kind) {
    case RS_KIND_EVENT:
      s = g_strdup_printf("%s|%04d%02d%02d|%d|%02d%02d",
                          r->ev.description ? r->ev.description : "",
                          r->ev.begin.tm_year, r->ev.begin.tm_mon, r->ev.begin.tm_mday,
                          r->ev.event, r->ev.event ? 0 : r->ev.begin.tm_hour,
                          r->ev.event ? 0 : r->ev.begin.tm_min);
      break;
    case RS_KIND_TODO:
      s = g_strdup_printf("%s", r->td.description ? r->td.description : "");
      break;
    default:
      s = g_strdup_printf("%s|%s|%s",
                          r->c.entry[contLastname] ? r->c.entry[contLastname] : "",
                          r->c.entry[contFirstname] ? r->c.entry[contFirstname] : "",
                          r->c.entry[contCompany] ? r->c.entry[contCompany] : "");
      break;
   }
   key = g_utf8_casefold(s, -1);
   g_free(s);
   return key;
}

static char *rec_title(const Kind *k, const Rec *r)
{
   switch (k->kind) {
    case RS_KIND_EVENT:
      return g_strdup(r->ev.description ? r->ev.description : "(no description)");
    case RS_KIND_TODO:
      return g_strdup(r->td.description ? r->td.description : "(no description)");
    default:
      if (r->c.entry[contLastname] || r->c.entry[contFirstname])
         return g_strstrip(g_strdup_printf("%s %s",
                                           r->c.entry[contFirstname] ? r->c.entry[contFirstname] : "",
                                           r->c.entry[contLastname] ? r->c.entry[contLastname] : ""));
      return g_strdup(r->c.entry[contCompany] ? r->c.entry[contCompany] : "(no name)");
   }
}

/* ---------- categories ---------- */

typedef struct {
   struct CategoryAppInfo cai;
   gboolean valid;
} Cats;

static void cats_load(const Kind *k, Cats *c)
{
   unsigned char *buf = NULL;
   int size = 0;

   memset(c, 0, sizeof(*c));
   if (jp_get_app_info(k->db_name, &buf, &size) == 0 && buf && size > 0) {
      c->valid = unpack_CategoryAppInfo(&c->cai, buf, size) > 0;
   }
   free(buf);
}

/* UTF-8 name for a category index, NULL for Unfiled */
static char *cats_name(const Cats *c, int idx)
{
   if (!c->valid || idx <= 0 || idx > 15 || !c->cai.name[idx][0])
      return NULL;
   return jp_charset_p2newj(c->cai.name[idx], 16);
}

static int cats_index(const Cats *c, const char *utf8)
{
   int i;

   if (!c->valid || !utf8 || !*utf8)
      return 0;
   for (i = 1; i < 16; i++) {
      char *n = cats_name(c, i);
      gboolean eq = n && !g_utf8_collate(n, utf8);
      if (!eq && n) {
         char *a = g_utf8_casefold(n, -1), *b = g_utf8_casefold(utf8, -1);
         eq = !strcmp(a, b);
         g_free(a);
         g_free(b);
      }
      g_free(n);
      if (eq)
         return i;
   }
   return 0;
}

/* ---------- state file ---------- */

typedef struct {
   unsigned int local_id;   /* 0: server object this collection ignores */
   char *href;
   char *etag;
   char *hash;
   gboolean seen;           /* processed in this run */
} StateEntry;

static void state_entry_free(StateEntry *e)
{
   g_free(e->href);
   g_free(e->etag);
   g_free(e->hash);
   g_free(e);
}

static char *state_path(const Kind *k, const char *coll_url)
{
   /* one state file per database + collection, so switching the
    * collection path starts a fresh link instead of mass-deleting */
   char *dir = rs_state_dir();
   char *h = g_compute_checksum_for_string(G_CHECKSUM_SHA1, coll_url, -1);
   char *name = g_strdup_printf("%s-%.12s.state", k->db_name, h);
   char *p = g_build_filename(dir, name, NULL);
   g_free(dir);
   g_free(h);
   g_free(name);
   return p;
}

static char *cache_dir(const Kind *k, const char *coll_url)
{
   char *sp = state_path(k, coll_url);
   char *d = g_strconcat(sp, ".d", NULL);
   g_mkdir_with_parents(d, 0700);
   g_free(sp);
   return d;
}

static GList *state_load(const char *path)
{
   char *text = NULL, **lines;
   GList *list = NULL;
   int i;

   if (!g_file_get_contents(path, &text, NULL, NULL))
      return NULL;
   lines = g_strsplit(text, "\n", -1);
   for (i = 0; lines[i]; i++) {
      char **f = g_strsplit(lines[i], "\t", -1);
      if (g_strv_length(f) >= 4 && lines[i][0] != '#') {
         StateEntry *e = g_new0(StateEntry, 1);
         e->local_id = strtoul(f[0], NULL, 10);
         e->href = g_strdup(f[1]);
         e->etag = g_strdup(f[2]);
         e->hash = g_strdup(f[3]);
         list = g_list_append(list, e);
      }
      g_strfreev(f);
   }
   g_strfreev(lines);
   g_free(text);
   return list;
}

static int state_save(const char *path, GList *list)
{
   GString *s = g_string_new("# radicalesync state: palm-id href etag record-hash\n");
   GError *err = NULL;
   int ret = 0;

   for (; list; list = list->next) {
      StateEntry *e = list->data;
      g_string_append_printf(s, "%u\t%s\t%s\t%s\n", e->local_id, e->href,
                             e->etag ? e->etag : "", e->hash ? e->hash : "");
   }
   if (!g_file_set_contents(path, s->str, s->len, &err)) {
      rs_log("Could not save sync state: %s", err->message);
      g_error_free(err);
      ret = -1;
   }
   g_string_free(s, TRUE);
   return ret;
}

static char *cache_file(const char *dir, const char *href)
{
   char *h = g_compute_checksum_for_string(G_CHECKSUM_SHA1, href, -1);
   char *p = g_build_filename(dir, h, NULL);
   g_free(h);
   return p;
}

static char *cache_get(const char *dir, const char *href)
{
   char *p = cache_file(dir, href), *text = NULL;
   g_file_get_contents(p, &text, NULL, NULL);
   g_free(p);
   return text;
}

static void cache_put(const char *dir, const char *href, const char *text)
{
   char *p = cache_file(dir, href);
   if (text)
      g_file_set_contents(p, text, -1, NULL);
   else
      g_unlink(p);
   g_free(p);
}

/* ---------- local records ---------- */

typedef struct {
   buf_rec *br;
   char *hash;
   gboolean linked;
} Local;

static char *record_hash(const void *buf, int size, unsigned char attrib)
{
   GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA1);
   unsigned char a = attrib & (CATEGORY_MASK | dlpRecAttrSecret);
   char *r;

   g_checksum_update(ck, buf, size);
   g_checksum_update(ck, &a, 1);
   r = g_strdup(g_checksum_get_string(ck));
   g_checksum_free(ck);
   return r;
}

static gboolean is_live(const buf_rec *br)
{
   if (br->attrib & dlpRecAttrDeleted)
      return FALSE;
   return br->rt == PALM_REC || br->rt == NEW_PC_REC || br->rt == REPLACEMENT_PALM_REC;
}

/* ---------- sync context ---------- */

typedef struct {
   const RsSettings *s;
   Kind *k;
   char *coll_url;
   char *coll_path;          /* server-absolute path of the collection */
   char *cache;
   Cats cats;
   GHashTable *locals;       /* local_id -> Local* */
   GHashTable *remote;       /* href -> etag */
   GHashTable *fetched;      /* href -> data */
   GList *state;
   int n_up, n_down, n_del_local, n_del_remote, n_conflict, n_err;
} Ctx;

static const char *url_path(const char *url)
{
   const char *p = strstr(url, "://");
   p = p ? strchr(p + 3, '/') : url;
   return p ? p : "/";
}

static gboolean http_ok(RsResponse *r)
{
   return r && r->status >= 200 && r->status < 300;
}

static void log_http_error(const char *what, const char *href, RsResponse *r)
{
   if (r->error)
      rs_log("%s %s failed: %s", what, href, r->error);
   else
      rs_log("%s %s failed: HTTP %d", what, href, r->status);
}

static gboolean fetch_etags(Ctx *c)
{
   static const char body[] =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:getetag/><D:resourcetype/>"
      "</D:prop></D:propfind>";
   const char *hdrs[] = { "Depth: 1", "Content-Type: application/xml; charset=utf-8", NULL };
   RsResponse *r = rs_http(c->s, "PROPFIND", c->coll_url, hdrs, body);
   GList *items, *l;

   if (r->status != 207) {
      if (r->status == 404)
         rs_log("%s: collection %s does not exist on the server", c->k->label, c->coll_url);
      else if (r->status == 401)
         rs_log("%s: the server rejected the username or password", c->k->label);
      else
         log_http_error("Listing", c->coll_url, r);
      rs_response_free(r);
      return FALSE;
   }
   items = rs_dav_parse_multistatus(r->body);
   for (l = items; l; l = l->next) {
      RsDavItem *it = l->data;
      if (it->is_collection || !it->etag)
         continue;
      g_hash_table_insert(c->remote, g_strdup(it->href), g_strdup(it->etag));
   }
   g_list_free_full(items, (GDestroyNotify)rs_dav_item_free);
   rs_response_free(r);
   return TRUE;
}

static char *xml_escape_href(const char *h)
{
   return g_markup_escape_text(h, -1);
}

/* REPORT multiget in batches; results go into c->fetched */
static void fetch_objects(Ctx *c, GPtrArray *hrefs)
{
   const char *hdrs[] = { "Depth: 1", "Content-Type: application/xml; charset=utf-8", NULL };
   guint i = 0;

   while (i < hrefs->len) {
      GString *b = g_string_new("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
      guint end = MIN(i + 100, hrefs->len), j;
      RsResponse *r;

      if (c->k->carddav)
         g_string_append(b, "<C:addressbook-multiget xmlns:D=\"DAV:\" "
                            "xmlns:C=\"urn:ietf:params:xml:ns:carddav\">"
                            "<D:prop><D:getetag/><C:address-data/></D:prop>");
      else
         g_string_append(b, "<C:calendar-multiget xmlns:D=\"DAV:\" "
                            "xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
                            "<D:prop><D:getetag/><C:calendar-data/></D:prop>");
      for (j = i; j < end; j++) {
         char *e = xml_escape_href(g_ptr_array_index(hrefs, j));
         g_string_append_printf(b, "<D:href>%s</D:href>", e);
         g_free(e);
      }
      g_string_append(b, c->k->carddav ? "</C:addressbook-multiget>"
                                       : "</C:calendar-multiget>");
      r = rs_http(c->s, "REPORT", c->coll_url, hdrs, b->str);
      if (r->status == 207) {
         GList *items = rs_dav_parse_multistatus(r->body), *l;
         for (l = items; l; l = l->next) {
            RsDavItem *it = l->data;
            if (!it->data)
               continue;
            g_hash_table_insert(c->fetched, g_strdup(it->href), g_strdup(it->data));
            if (it->etag)
               g_hash_table_insert(c->remote, g_strdup(it->href), g_strdup(it->etag));
         }
         g_list_free_full(items, (GDestroyNotify)rs_dav_item_free);
      } else {
         log_http_error("Fetching from", c->coll_url, r);
      }
      rs_response_free(r);
      g_string_free(b, TRUE);
      i = end;
   }

   /* anything the REPORT did not deliver: plain GET */
   for (i = 0; i < hrefs->len; i++) {
      const char *h = g_ptr_array_index(hrefs, i);
      char *url;
      RsResponse *r;
      if (g_hash_table_contains(c->fetched, h))
         continue;
      url = rs_join_url(c->s->url, h);
      r = rs_http(c->s, "GET", url, NULL, NULL);
      if (http_ok(r) && r->body) {
         g_hash_table_insert(c->fetched, g_strdup(h), g_strdup(r->body));
         if (r->etag)
            g_hash_table_insert(c->remote, g_strdup(h), g_strdup(r->etag));
      } else {
         log_http_error("Downloading", h, r);
      }
      rs_response_free(r);
      g_free(url);
   }
}

/* Upload a local record.  if_match NULL means "create" (If-None-Match).
 * On success updates e->etag (may become "" when the server sends none). */
static gboolean push_local(Ctx *c, StateEntry *e, Local *lo, const char *if_match)
{
   Rec r;
   char *merge = cache_get(c->cache, e->href);
   char *uid = NULL, *text, *cat, *url, *title;
   char *ct = g_strdup_printf("Content-Type: %s", c->k->content_type);
   char *cond = if_match ? g_strdup_printf("If-Match: %s", if_match)
                         : g_strdup("If-None-Match: *");
   const char *hdrs[] = { ct, cond, NULL };
   RsResponse *resp;
   gboolean ok;

   if (rec_unpack(c->k, lo->br, &r) != 0) {
      rs_log("%s: could not read Palm record %u", c->k->label, lo->br->unique_id);
      g_free(merge);
      g_free(ct);
      g_free(cond);
      c->n_err++;
      return FALSE;
   }
   if (merge)
      uid = c->k->carddav ? rs_vcard_get_uid(merge) : rs_ical_get_uid(merge);
   if (!uid) {
      /* use the file name we chose for the href as UID */
      const char *slash = strrchr(e->href, '/');
      uid = g_strdup(slash ? slash + 1 : e->href);
      if (g_str_has_suffix(uid, c->k->ext))
         uid[strlen(uid) - strlen(c->k->ext)] = 0;
   }
   cat = cats_name(&c->cats, lo->br->attrib & CATEGORY_MASK);
   text = rec_to_text(c->k, &r, uid, cat, (lo->br->attrib & dlpRecAttrSecret) != 0, merge);
   title = rec_title(c->k, &r);
   url = rs_join_url(c->s->url, e->href);
   resp = rs_http(c->s, "PUT", url, hdrs, text);
   ok = http_ok(resp);
   if (ok) {
      g_free(e->etag);
      e->etag = g_strdup(resp->etag ? resp->etag : "");
      cache_put(c->cache, e->href, text);
      rs_log("%s: sent \"%s\" to server", c->k->label, title);
      c->n_up++;
   } else if (resp->status == 412) {
      rs_log("%s: \"%s\" changed on the server during sync; it will be handled next time",
             c->k->label, title);
   } else {
      log_http_error("Uploading", e->href, resp);
      c->n_err++;
   }
   rs_response_free(resp);
   rec_free(c->k, &r);
   g_free(merge);
   g_free(uid);
   g_free(text);
   g_free(cat);
   g_free(title);
   g_free(url);
   g_free(ct);
   g_free(cond);
   return ok;
}

static gboolean delete_remote(Ctx *c, StateEntry *e)
{
   char *url = rs_join_url(c->s->url, e->href);
   char *cond = (e->etag && *e->etag) ? g_strdup_printf("If-Match: %s", e->etag) : NULL;
   const char *hdrs[] = { cond, NULL };
   RsResponse *r = rs_http(c->s, "DELETE", url, cond ? hdrs : NULL, NULL);
   gboolean ok = http_ok(r) || r->status == 404;

   if (ok) {
      rs_log("%s: deleted %s on server", c->k->label, e->href);
      cache_put(c->cache, e->href, NULL);
      c->n_del_remote++;
   } else if (r->status == 412) {
      rs_log("%s: %s changed on the server, not deleting it", c->k->label, e->href);
   } else {
      log_http_error("Deleting", e->href, r);
      c->n_err++;
   }
   rs_response_free(r);
   g_free(url);
   g_free(cond);
   return ok;
}

/* Write server data into the J-Pilot database.  lo == NULL creates a new
 * record.  Returns the new Local (owned by the caller) or NULL. */
static gboolean pull_remote(Ctx *c, StateEntry *e, Local *lo, const char *data)
{
   Rec r;
   char *cat = NULL, *title;
   int priv = 0, cat_idx;
   pi_buffer_t *pb;
   buf_rec nb;

   if (lo) {
      if (rec_unpack(c->k, lo->br, &r) != 0)
         rec_init(c->k, &r);
   } else {
      rec_init(c->k, &r);
   }
   if (rec_from_text(c->k, data, &r, &cat, &priv) != 0) {
      rs_log("%s: could not understand %s from the server", c->k->label, e->href);
      rec_free(c->k, &r);
      g_free(cat);
      c->n_err++;
      return FALSE;
   }
   title = rec_title(c->k, &r);
   pb = rec_pack(c->k, &r);
   if (!pb) {
      rs_log("%s: \"%s\" cannot be stored on the Palm", c->k->label, title);
      rec_free(c->k, &r);
      g_free(cat);
      g_free(title);
      c->n_err++;
      return FALSE;
   }

   memset(&nb, 0, sizeof(nb));
   nb.buf = pb->data;
   nb.size = pb->used;
   cat_idx = cat ? cats_index(&c->cats, cat) : (lo ? (lo->br->attrib & CATEGORY_MASK) : 0);
   nb.attrib = (lo ? (lo->br->attrib & ~(CATEGORY_MASK | dlpRecAttrSecret)) : 0) |
               cat_idx | (priv ? dlpRecAttrSecret : 0);
   nb.attrib &= ~dlpRecAttrDeleted;

   if (lo) {
      if (lo->br->rt == PALM_REC || lo->br->rt == REPLACEMENT_PALM_REC) {
         nb.rt = REPLACEMENT_PALM_REC;
         nb.unique_id = lo->br->unique_id;
      } else {
         nb.rt = NEW_PC_REC;
         nb.unique_id = 0;
      }
      jp_delete_record(c->k->db_name, lo->br, MODIFY_FLAG);
   } else {
      nb.rt = NEW_PC_REC;
      nb.unique_id = 0;
   }

   if (jp_pc_write(c->k->db_name, &nb) != EXIT_SUCCESS) {
      rs_log("%s: could not write \"%s\" to the J-Pilot database", c->k->label, title);
      c->n_err++;
   } else {
      e->local_id = nb.unique_id;
      g_free(e->hash);
      e->hash = record_hash(nb.buf, nb.size, nb.attrib);
      cache_put(c->cache, e->href, data);
      rs_log("%s: %s \"%s\" from server", c->k->label, lo ? "updated" : "added", title);
      c->n_down++;
   }
   pi_buffer_free(pb);
   rec_free(c->k, &r);
   g_free(cat);
   g_free(title);
   return TRUE;
}

static void delete_local(Ctx *c, Local *lo)
{
   Rec r;
   char *title = NULL;

   if (rec_unpack(c->k, lo->br, &r) == 0) {
      title = rec_title(c->k, &r);
      rec_free(c->k, &r);
   }
   jp_delete_record(c->k->db_name, lo->br, DELETE_FLAG);
   rs_log("%s: deleted \"%s\" (deleted on server)", c->k->label, title ? title : "?");
   g_free(title);
   c->n_del_local++;
}

static char *new_href(Ctx *c)
{
   char *uuid = g_uuid_string_random();
   char *h = g_strdup_printf("%s%s%s%s", c->coll_path,
                             g_str_has_suffix(c->coll_path, "/") ? "" : "/",
                             uuid, c->k->ext);
   g_free(uuid);
   return h;
}

static gboolean remote_has_component(Ctx *c, const char *data)
{
   if (c->k->carddav)
      return TRUE;
   return rs_ical_has_component(data, c->k->component);
}

/* ---------- one collection ---------- */

static int sync_collection(const RsSettings *s, Kind *k, const char *path)
{
   Ctx c;
   GList *records = NULL, *l, *new_state = NULL;
   GPtrArray *to_fetch;
   GHashTableIter it;
   gpointer key, val;
   char *spath;
   int mapped = 0;
   gboolean prefer_palm = s->conflict == RS_PREFER_PALM;

   memset(&c, 0, sizeof(c));
   c.s = s;
   c.k = k;
   c.coll_url = rs_join_url(s->url, path);
   if (!g_str_has_suffix(c.coll_url, "/")) {
      char *t = g_strconcat(c.coll_url, "/", NULL);
      g_free(c.coll_url);
      c.coll_url = t;
   }
   c.coll_path = g_strdup(url_path(c.coll_url));
   c.cache = cache_dir(k, c.coll_url);
   c.locals = g_hash_table_new(g_direct_hash, g_direct_equal);
   c.remote = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
   c.fetched = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
   spath = state_path(k, c.coll_url);
   c.state = state_load(spath);

   rs_log("%s: syncing with %s", k->label, c.coll_url);

   if (!fetch_etags(&c)) {
      c.n_err++;
      goto out;
   }

   cats_load(k, &c.cats);
   if (jp_read_DB_files(k->db_name, &records) < 0) {
      rs_log("%s: could not read %s", k->label, k->db_name);
      c.n_err++;
      goto out;
   }
   for (l = records; l; l = l->next) {
      buf_rec *br = l->data;
      Local *lo;
      if (!is_live(br))
         continue;
      lo = g_new0(Local, 1);
      lo->br = br;
      lo->hash = record_hash(br->buf, br->size, br->attrib);
      g_hash_table_insert(c.locals, GUINT_TO_POINTER(br->unique_id), lo);
   }

   /* Safety: an empty side while we remember many links almost always
    * means a wrong path or a reset database, not mass deletion. */
   for (l = c.state; l; l = l->next)
      if (((StateEntry *)l->data)->local_id)
         mapped++;
   if (mapped >= 3 && g_hash_table_size(c.remote) == 0) {
      rs_log("%s: the server collection is empty but %d items were synced before. "
             "Not deleting anything - check the collection path, or delete %s to start over.",
             k->label, mapped, spath);
      c.n_err++;
      goto out;
   }
   if (mapped >= 3 && g_hash_table_size(c.locals) == 0) {
      rs_log("%s: the J-Pilot database is empty but %d items were synced before. "
             "Not deleting anything on the server - delete %s to start over.",
             k->label, mapped, spath);
      c.n_err++;
      goto out;
   }

   /* Relink records whose id changed in a HotSync (new PC records get a
    * Palm id): same content hash, id no longer present. */
   for (l = c.state; l; l = l->next) {
      StateEntry *e = l->data;
      if (!e->local_id || g_hash_table_contains(c.locals, GUINT_TO_POINTER(e->local_id)))
         continue;
      g_hash_table_iter_init(&it, c.locals);
      while (g_hash_table_iter_next(&it, &key, &val)) {
         Local *lo = val;
         gboolean taken = FALSE;
         GList *m;
         if (strcmp(lo->hash, e->hash))
            continue;
         for (m = c.state; m; m = m->next)
            if (((StateEntry *)m->data)->local_id == lo->br->unique_id)
               taken = TRUE;
         if (!taken) {
            e->local_id = lo->br->unique_id;
            break;
         }
      }
   }

   /* J-Pilot gives a PC-created record a new id when it is edited: the old
    * copy stays behind as DELETED_PC_REC.  Pair such a vanished record with
    * an unlinked new one so the edit updates the server object instead of
    * deleting it and creating another. */
   {
      GPtrArray *gone = g_ptr_array_new(), *fresh = g_ptr_array_new();
      guint i, j;

      for (l = c.state; l; l = l->next) {
         StateEntry *e = l->data;
         GList *m;
         if (!e->local_id || g_hash_table_contains(c.locals, GUINT_TO_POINTER(e->local_id)))
            continue;
         for (m = records; m; m = m->next) {
            buf_rec *br = m->data;
            if (br->unique_id == e->local_id && br->rt == DELETED_PC_REC) {
               g_ptr_array_add(gone, e);
               g_ptr_array_add(gone, br);
               break;
            }
         }
      }
      g_hash_table_iter_init(&it, c.locals);
      while (g_hash_table_iter_next(&it, &key, &val)) {
         Local *lo = val;
         gboolean linked = FALSE;
         for (l = c.state; l; l = l->next)
            if (((StateEntry *)l->data)->local_id == lo->br->unique_id)
               linked = TRUE;
         if (!linked && lo->br->rt == NEW_PC_REC)
            g_ptr_array_add(fresh, lo);
      }
      /* same match key first */
      for (i = 0; i < gone->len; i += 2) {
         StateEntry *e = g_ptr_array_index(gone, i);
         Rec r;
         char *old_key;
         if (rec_unpack(k, g_ptr_array_index(gone, i + 1), &r) != 0)
            continue;
         old_key = rec_match_key(k, &r);
         rec_free(k, &r);
         for (j = 0; j < fresh->len; j++) {
            Local *lo = g_ptr_array_index(fresh, j);
            char *new_key;
            if (rec_unpack(k, lo->br, &r) != 0)
               continue;
            new_key = rec_match_key(k, &r);
            rec_free(k, &r);
            if (!strcmp(old_key, new_key)) {
               e->local_id = lo->br->unique_id;
               g_ptr_array_remove_index(fresh, j);
               g_ptr_array_index(gone, i) = NULL;
               g_free(new_key);
               break;
            }
            g_free(new_key);
         }
         g_free(old_key);
      }
      /* one edited record left on each side: that is the edit */
      {
         StateEntry *only = NULL;
         int n = 0;
         for (i = 0; i < gone->len; i += 2)
            if (g_ptr_array_index(gone, i)) {
               only = g_ptr_array_index(gone, i);
               n++;
            }
         if (n == 1 && fresh->len == 1)
            only->local_id = ((Local *)g_ptr_array_index(fresh, 0))->br->unique_id;
      }
      g_ptr_array_free(gone, TRUE);
      g_ptr_array_free(fresh, TRUE);
   }

   /* What must be downloaded: new hrefs and changed ETags */
   to_fetch = g_ptr_array_new();
   g_hash_table_iter_init(&it, c.remote);
   while (g_hash_table_iter_next(&it, &key, &val)) {
      StateEntry *known = NULL;
      for (l = c.state; l; l = l->next)
         if (!strcmp(((StateEntry *)l->data)->href, key))
            known = l->data;
      if (!known || strcmp(known->etag, val))
         g_ptr_array_add(to_fetch, key);
   }
   if (to_fetch->len)
      fetch_objects(&c, to_fetch);
   g_ptr_array_free(to_fetch, TRUE);

   /* 1. Items linked in the previous sync */
   for (l = c.state; l; l = l->next) {
      StateEntry *e = l->data;
      Local *lo = e->local_id ? g_hash_table_lookup(c.locals, GUINT_TO_POINTER(e->local_id)) : NULL;
      const char *retag = g_hash_table_lookup(c.remote, e->href);
      const char *data = g_hash_table_lookup(c.fetched, e->href);
      gboolean lchg = lo && strcmp(lo->hash, e->hash);
      gboolean rchg = retag && strcmp(retag, e->etag);
      gboolean keep = TRUE;

      e->seen = TRUE;
      if (lo)
         lo->linked = TRUE;

      if (!e->local_id) {
         /* object of another type (e.g. a VTODO in the event calendar) */
         if (!retag) {
            keep = FALSE;
         } else if (rchg && data && remote_has_component(&c, data)) {
            /* became relevant: treat as new below */
            keep = FALSE;
            e->seen = FALSE;
         } else {
            g_free(e->etag);
            e->etag = g_strdup(retag);
         }
      } else if (!lo && !retag) {
         keep = FALSE;
         cache_put(c.cache, e->href, NULL);
      } else if (!lo) {
         /* deleted on the Palm */
         if (rchg && !prefer_palm && data) {
            rs_log("%s: %s was deleted in J-Pilot but changed on the server; restoring it",
                   k->label, e->href);
            c.n_conflict++;
            pull_remote(&c, e, NULL, data);
            g_free(e->etag);
            e->etag = g_strdup(retag);
         } else {
            keep = !delete_remote(&c, e);
         }
      } else if (!retag) {
         /* deleted on the server */
         if (lchg && prefer_palm) {
            rs_log("%s: an item deleted on the server was changed in J-Pilot; re-uploading",
                   k->label);
            c.n_conflict++;
            if (push_local(&c, e, lo, NULL)) {
               g_free(e->hash);
               e->hash = g_strdup(lo->hash);
            }
         } else {
            delete_local(&c, lo);
            cache_put(c.cache, e->href, NULL);
            keep = FALSE;
         }
      } else if (lchg && rchg) {
         c.n_conflict++;
         if (prefer_palm || !data) {
            if (push_local(&c, e, lo, retag)) {
               g_free(e->hash);
               e->hash = g_strdup(lo->hash);
            }
         } else {
            pull_remote(&c, e, lo, data);
            g_free(e->etag);
            e->etag = g_strdup(retag);
         }
      } else if (lchg) {
         if (push_local(&c, e, lo, e->etag)) {
            g_free(e->hash);
            e->hash = g_strdup(lo->hash);
         }
      } else if (rchg && data) {
         if (pull_remote(&c, e, lo, data)) {
            g_free(e->etag);
            e->etag = g_strdup(retag);
         }
      }
      if (keep)
         new_state = g_list_append(new_state, e);
      else
         state_entry_free(e);
   }
   g_list_free(c.state);
   c.state = new_state;

   /* 2. New on the server */
   {
      GHashTable *by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      /* keys of unlinked local records, for first-sync pairing */
      g_hash_table_iter_init(&it, c.locals);
      while (g_hash_table_iter_next(&it, &key, &val)) {
         Local *lo = val;
         Rec r;
         if (lo->linked || rec_unpack(k, lo->br, &r) != 0)
            continue;
         g_hash_table_insert(by_key, rec_match_key(k, &r), lo);
         rec_free(k, &r);
      }

      g_hash_table_iter_init(&it, c.remote);
      while (g_hash_table_iter_next(&it, &key, &val)) {
         const char *href = key, *data;
         StateEntry *e;
         gboolean linked = FALSE;
         Local *match = NULL;
         Rec r;

         for (l = c.state; l; l = l->next)
            if (!strcmp(((StateEntry *)l->data)->href, href))
               linked = TRUE;
         if (linked)
            continue;
         data = g_hash_table_lookup(c.fetched, href);
         if (!data)
            continue;
         e = g_new0(StateEntry, 1);
         e->href = g_strdup(href);
         e->etag = g_strdup(val);
         e->hash = g_strdup("");
         if (!remote_has_component(&c, data)) {
            c.state = g_list_append(c.state, e);   /* remember, ignore */
            continue;
         }
         rec_init(k, &r);
         if (rec_from_text(k, data, &r, NULL, NULL) == 0) {
            char *mk = rec_match_key(k, &r);
            match = g_hash_table_lookup(by_key, mk);
            if (match && !match->linked)
               g_hash_table_remove(by_key, mk);
            else
               match = NULL;
            g_free(mk);
         }
         rec_free(k, &r);

         if (match) {
            /* same item on both sides: link it, then settle differences */
            match->linked = TRUE;
            e->local_id = match->br->unique_id;
            e->hash = (g_free(e->hash), g_strdup(match->hash));
            cache_put(c.cache, href, data);
            if (prefer_palm)
               push_local(&c, e, match, e->etag);
            else
               pull_remote(&c, e, match, data);
         } else if (!pull_remote(&c, e, NULL, data)) {
            state_entry_free(e);
            continue;
         }
         c.state = g_list_append(c.state, e);
      }
      g_hash_table_destroy(by_key);
   }

   /* 3. New in J-Pilot */
   g_hash_table_iter_init(&it, c.locals);
   while (g_hash_table_iter_next(&it, &key, &val)) {
      Local *lo = val;
      StateEntry *e;
      if (lo->linked)
         continue;
      e = g_new0(StateEntry, 1);
      e->local_id = lo->br->unique_id;
      e->href = new_href(&c);
      e->hash = g_strdup(lo->hash);
      if (push_local(&c, e, lo, NULL)) {
         lo->linked = TRUE;
         c.state = g_list_append(c.state, e);
      } else {
         state_entry_free(e);
      }
   }

   /* Servers may omit ETag on PUT: fill in from a fresh listing */
   {
      gboolean missing = FALSE;
      for (l = c.state; l; l = l->next)
         if (!*((StateEntry *)l->data)->etag)
            missing = TRUE;
      if (missing) {
         g_hash_table_remove_all(c.remote);
         if (fetch_etags(&c)) {
            for (l = c.state; l; l = l->next) {
               StateEntry *e = l->data;
               const char *t = g_hash_table_lookup(c.remote, e->href);
               if (!*e->etag && t) {
                  g_free(e->etag);
                  e->etag = g_strdup(t);
               }
            }
         }
      }
   }

   state_save(spath, c.state);
   rs_log("%s: %d sent, %d received, %d deleted here, %d deleted on server, "
          "%d conflicts, %d errors",
          k->label, c.n_up, c.n_down, c.n_del_local, c.n_del_remote,
          c.n_conflict, c.n_err);

out:
   g_list_free_full(c.state, (GDestroyNotify)state_entry_free);
   g_hash_table_iter_init(&it, c.locals);
   while (g_hash_table_iter_next(&it, &key, &val)) {
      g_free(((Local *)val)->hash);
      g_free(val);
   }
   g_hash_table_destroy(c.locals);
   g_hash_table_destroy(c.remote);
   g_hash_table_destroy(c.fetched);
   jp_free_DB_records(&records);
   g_free(spath);
   g_free(c.coll_url);
   g_free(c.coll_path);
   g_free(c.cache);
   return c.n_err ? 1 : 0;
}

/* ---------- entry points ---------- */

int rs_sync_all(const RsSettings *s)
{
   long dv = 1, av = 1, tv = 0;
   int failed = 0;

   if (!s->url || !*s->url) {
      rs_log("No Radicale address configured - open the settings first");
      return 1;
   }
   if (&glob_sqlite && glob_sqlite) {
      /* J-Pilot then keeps its data in SQLite, not in the .pdb/.pc3 files
       * the plugin API reads and writes */
      rs_log("J-Pilot is running with SQLite storage (-S); Radicale Sync only "
             "works with the standard file storage. Nothing was synced.");
      return 1;
   }
   get_pref(PREF_DATEBOOK_VERSION, &dv, NULL);
   get_pref(PREF_ADDRESS_VERSION, &av, NULL);
   get_pref(PREF_TODO_VERSION, &tv, NULL);

   if (s->sync_cal && s->cal_path && *s->cal_path) {
      Kind k = { RS_KIND_EVENT, "Calendar", dv ? "CalendarDB-PDat" : "DatebookDB",
                 "VEVENT", ".ics", "text/calendar; charset=utf-8", FALSE, (int)dv };
      failed += sync_collection(s, &k, s->cal_path);
   }
   if (s->sync_todo && s->todo_path && *s->todo_path) {
      Kind k = { RS_KIND_TODO, "To-dos", tv ? "TasksDB-PTod" : "ToDoDB",
                 "VTODO", ".ics", "text/calendar; charset=utf-8", FALSE, (int)tv };
      failed += sync_collection(s, &k, s->todo_path);
   }
   if (s->sync_card && s->card_path && *s->card_path) {
      Kind k = { RS_KIND_CONTACT, "Contacts", av ? "ContactsDB-PAdd" : "AddressDB",
                 NULL, ".vcf", "text/vcard; charset=utf-8", TRUE, (int)av };
      failed += sync_collection(s, &k, s->card_path);
   }
   rs_log(failed ? "Sync finished with errors" : "Sync finished");
   return failed;
}

GList *rs_discover(const RsSettings *s, char **error)
{
   static const char body[] =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
      "<D:prop><D:resourcetype/><D:displayname/><C:supported-calendar-component-set/>"
      "</D:prop></D:propfind>";
   const char *hdrs[] = { "Depth: 1", "Content-Type: application/xml; charset=utf-8", NULL };
   char *home = g_strdup_printf("%s/", s->username);
   char *url = rs_join_url(s->url, home);
   RsResponse *r = rs_http(s, "PROPFIND", url, hdrs, body);
   GList *items = NULL, *l, *out = NULL;

   if (r->status == 207) {
      items = rs_dav_parse_multistatus(r->body);
      for (l = items; l; l = l->next) {
         RsDavItem *it = l->data;
         if (it->kind)
            out = g_list_append(out, it);
         else
            rs_dav_item_free(it);
      }
      g_list_free(items);
      if (!out && error)
         *error = g_strdup_printf("No calendars or address books found under %s", url);
   } else if (error) {
      if (r->status == 401)
         *error = g_strdup("The server rejected the username or password.");
      else if (r->error)
         *error = g_strdup_printf("Could not reach %s: %s", url, r->error);
      else
         *error = g_strdup_printf("%s answered HTTP %d", url, r->status);
   }
   rs_response_free(r);
   g_free(url);
   g_free(home);
   return out;
}
