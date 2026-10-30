/* radicalesync - two-way sync of J-Pilot calendar, contacts and to-dos
 * with a Radicale (CalDAV/CardDAV) server.
 *
 * Shared declarations.
 */
#ifndef RS_H
#define RS_H

#include <glib.h>
#include <time.h>
#include <pi-calendar.h>
#include <pi-contact.h>
#include <pi-todo.h>
#include <pi-appinfo.h>

/* ---------- settings ---------- */

typedef enum { RS_PREFER_SERVER = 0, RS_PREFER_PALM = 1 } RsConflict;

typedef struct {
   char *url;            /* e.g. http://host:5232/ */
   char *username;
   char *password;
   char *cal_path;       /* collection paths relative to url, e.g. tom/calendar/ */
   char *todo_path;
   char *card_path;
   gboolean sync_cal, sync_todo, sync_card;
   gboolean after_hotsync;
   gboolean insecure_tls;
   RsConflict conflict;
} RsSettings;

void rs_settings_load(RsSettings *s);
int  rs_settings_save(const RsSettings *s);
void rs_settings_clear(RsSettings *s);
char *rs_state_dir(void);          /* ~/.jpilot/radicalesync (created) */

/* ---------- logging ---------- */

typedef void (*RsLogFunc)(const char *msg, void *data);
void rs_set_log_func(RsLogFunc f, void *data);
void rs_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* ---------- HTTP / WebDAV ---------- */

typedef struct {
   int status;           /* HTTP status, 0 on transport error */
   char *body;
   char *etag;           /* ETag response header, if any */
   char *error;          /* transport error text */
} RsResponse;

void rs_response_free(RsResponse *r);
/* body may be NULL; extra_headers is a NULL-terminated array or NULL */
RsResponse *rs_http(const RsSettings *s, const char *method, const char *url,
                    const char *const *extra_headers, const char *body);
char *rs_join_url(const char *base, const char *path);

typedef struct {
   char *href;
   char *etag;
   char *data;           /* calendar-data / address-data when present */
   gboolean is_collection;
   char *displayname;
   char *kind;           /* "calendar", "addressbook" or NULL */
   char *components;     /* "VEVENT,VTODO" for calendars */
} RsDavItem;

void rs_dav_item_free(RsDavItem *it);
/* Parse a 207 Multi-Status body into a GList of RsDavItem* */
GList *rs_dav_parse_multistatus(const char *xml);

/* ---------- Palm record kinds ---------- */

typedef enum { RS_KIND_EVENT, RS_KIND_TODO, RS_KIND_CONTACT } RsKind;

/* Convert Palm records to/from iCalendar/vCard.  Strings in the Palm
 * structures are UTF-8 here; charset conversion happens in the engine.
 * 'merge_with' is the last copy fetched from the server (or NULL); unknown
 * properties from it are preserved. Returned string is g_malloc'd. */
char *rs_event_to_ical(const struct CalendarEvent *ev, const char *uid,
                       const char *category, int private_rec,
                       const char *merge_with);
char *rs_todo_to_ical(const struct ToDo *td, const char *uid,
                      const char *category, int private_rec,
                      const char *merge_with);
char *rs_contact_to_vcard(const struct Contact *c, const char *uid,
                          const char *category, int private_rec,
                          const char *merge_with);

/* Fill a Palm structure from server data.  The structure should already
 * hold the existing local record (or be zeroed for a new one): fields the
 * server object does not describe are left alone.  Returns 0 on success,
 * -1 if the object does not contain the wanted component. */
int rs_ical_to_event(const char *ical, struct CalendarEvent *ev,
                     char **category, int *private_rec);
int rs_ical_to_todo(const char *ical, struct ToDo *td,
                    char **category, int *private_rec);
int rs_vcard_to_contact(const char *vcard, struct Contact *c,
                        char **category, int *private_rec);
char *rs_ical_get_uid(const char *ical);
char *rs_vcard_get_uid(const char *vcard);
gboolean rs_ical_has_component(const char *ical, const char *comp);

/* ---------- sync engine ---------- */

/* Runs a full two-way sync of every enabled collection.
 * Returns 0 on success, otherwise the number of collections that failed. */
int rs_sync_all(const RsSettings *s);

/* Lists collections under the user's home (for the settings dialog). */
GList *rs_discover(const RsSettings *s, char **error);

#endif
