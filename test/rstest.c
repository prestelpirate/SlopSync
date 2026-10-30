/* End-to-end tests for the sync engine against a running Radicale.
 * usage: rstest URL USER PASS PDB_DIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <pi-calendar.h>
#include <pi-contact.h>
#include <pi-todo.h>
#include "libplugin.h"
#include "rs.h"
#include "stubs.h"

#define CAL "CalendarDB-PDat"
#define CON "ContactsDB-PAdd"
#define TODO "ToDoDB"

static GString *logbuf;
static int failures, checks;
static RsSettings S;

/* server path under the test user's home */
static const char *P(const char *rel)
{
   return g_intern_string(g_strdup_printf("/%s/%s", S.username, rel));
}

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
   printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
   else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void capture(const char *msg, void *d)
{
   g_string_append(logbuf, msg);
   g_string_append_c(logbuf, '\n');
}

static void sync_now(const char *what)
{
   g_string_truncate(logbuf, 0);
   rs_sync_all(&S);
   printf("---- sync: %s\n%s", what, logbuf->str);
}

static gboolean log_has(const char *s)
{
   return strstr(logbuf->str, s) != NULL;
}

/* did the last sync change anything? */
static gboolean log_changed(void)
{
   return log_has(" sent \"") || log_has("from server") || log_has("deleted \"") ||
          log_has("deleted /");
}

static GList *remote_list(const char *path)
{
   const char *h[] = { "Depth: 1", NULL };
   char *url = rs_join_url(S.url, path);
   RsResponse *r = rs_http(&S, "PROPFIND", url, h,
      "<?xml version=\"1.0\"?><D:propfind xmlns:D=\"DAV:\"><D:prop><D:getetag/>"
      "<D:resourcetype/></D:prop></D:propfind>");
   GList *items = rs_dav_parse_multistatus(r->body), *l, *out = NULL;
   for (l = items; l; l = l->next) {
      RsDavItem *it = l->data;
      if (it->is_collection)
         rs_dav_item_free(it);
      else
         out = g_list_append(out, it);
   }
   g_list_free(items);
   rs_response_free(r);
   g_free(url);
   return out;
}

static int remote_count(const char *path)
{
   GList *l = remote_list(path);
   int n = g_list_length(l);
   g_list_free_full(l, (GDestroyNotify)rs_dav_item_free);
   return n;
}

static char *remote_get(const char *href, char **etag)
{
   char *url = rs_join_url(S.url, href);
   RsResponse *r = rs_http(&S, "GET", url, NULL, NULL);
   char *b = g_strdup(r->body);
   if (etag)
      *etag = g_strdup(r->etag);
   rs_response_free(r);
   g_free(url);
   return b;
}

static int remote_put(const char *href, const char *body, const char *ctype)
{
   char *url = rs_join_url(S.url, href);
   char *ct = g_strdup_printf("Content-Type: %s", ctype);
   const char *h[] = { ct, NULL };
   RsResponse *r = rs_http(&S, "PUT", url, h, body);
   int st = r->status;
   rs_response_free(r);
   g_free(url);
   g_free(ct);
   return st;
}

static int remote_delete(const char *href)
{
   char *url = rs_join_url(S.url, href);
   RsResponse *r = rs_http(&S, "DELETE", url, NULL, NULL);
   int st = r->status;
   rs_response_free(r);
   g_free(url);
   return st;
}

/* find a remote object whose body contains needle */
static char *remote_find(const char *path, const char *needle, char **body_out)
{
   GList *items = remote_list(path), *l;
   char *found = NULL;
   for (l = items; l && !found; l = l->next) {
      RsDavItem *it = l->data;
      char *b = remote_get(it->href, NULL);
      if (b && strstr(b, needle)) {
         found = g_strdup(it->href);
         if (body_out)
            *body_out = g_strdup(b);
      }
      g_free(b);
   }
   g_list_free_full(items, (GDestroyNotify)rs_dav_item_free);
   return found;
}

static buf_rec *live_rec(const char *db, int nth)
{
   GList *l;
   for (l = stub_records(db); l; l = l->next) {
      buf_rec *b = l->data;
      if ((b->rt == PALM_REC || b->rt == NEW_PC_REC || b->rt == REPLACEMENT_PALM_REC) && nth-- == 0)
         return b;
   }
   return NULL;
}

/* first live event whose description contains s */
static buf_rec *find_event(const char *s, struct CalendarEvent *out)
{
   GList *l;
   for (l = stub_records(CAL); l; l = l->next) {
      buf_rec *b = l->data;
      pi_buffer_t *pb;
      struct CalendarEvent ev;
      if (!(b->rt == PALM_REC || b->rt == NEW_PC_REC || b->rt == REPLACEMENT_PALM_REC))
         continue;
      pb = pi_buffer_new(b->size);
      pi_buffer_append(pb, b->buf, b->size);
      memset(&ev, 0, sizeof(ev));
      unpack_CalendarEvent(&ev, pb, calendar_v1);
      pi_buffer_free(pb);
      if (ev.description && strstr(ev.description, s)) {
         if (out)
            *out = ev;
         else
            free_CalendarEvent(&ev);
         return b;
      }
      free_CalendarEvent(&ev);
   }
   return NULL;
}

static void local_modify_event(buf_rec *b, const char *new_desc)
{
   pi_buffer_t *pb = pi_buffer_new(b->size), *out = pi_buffer_new(0xffff);
   struct CalendarEvent ev;
   buf_rec nb;

   pi_buffer_append(pb, b->buf, b->size);
   memset(&ev, 0, sizeof(ev));
   unpack_CalendarEvent(&ev, pb, calendar_v1);
   free(ev.description);
   ev.description = strdup(new_desc);
   pack_CalendarEvent(&ev, out, calendar_v1);
   nb = *b;
   nb.buf = out->data;
   nb.size = out->used;
   /* what J-Pilot does: Palm records keep their id, PC records get a new one */
   nb.rt = (b->rt == PALM_REC || b->rt == REPLACEMENT_PALM_REC) ? REPLACEMENT_PALM_REC : NEW_PC_REC;
   if (nb.rt == NEW_PC_REC)
      nb.unique_id = 0;
   jp_delete_record(CAL, b, MODIFY_FLAG);
   jp_pc_write(CAL, &nb);
   free_CalendarEvent(&ev);
   pi_buffer_free(pb);
   pi_buffer_free(out);
}

/* Unfold, drop lines that change on every conversion, and ignore blanks
 * (libical trims leading/trailing blanks of text values) */
static char *stable(const char *text)
{
   GString *u = g_string_new(NULL);
   char **lines;
   GString *g = g_string_new(NULL);
   const char *s;
   int i;
   for (s = text; *s; s++) {
      if (*s == '\r' || *s == ' ')
         continue;
      if (*s == '\n' && s[1] == ' ') {
         s++;
         continue;
      }
      g_string_append_c(u, *s);
   }
   lines = g_strsplit(u->str, "\n", -1);
   for (i = 0; lines[i]; i++)
      if (!g_str_has_prefix(lines[i], "DTSTAMP") && !g_str_has_prefix(lines[i], "LAST-MODIFIED") &&
          !g_str_has_prefix(lines[i], "REV:"))
         g_string_append_printf(g, "%s\n", lines[i]);
   g_strfreev(lines);
   g_string_free(u, TRUE);
   return g_string_free(g, FALSE);
}

/* Palm -> iCal -> Palm -> iCal must describe the same event */
static void roundtrip_events(void)
{
   GList *l;
   int n = 0, bad = 0;
   for (l = stub_records(CAL); l; l = l->next) {
      buf_rec *b = l->data;
      pi_buffer_t *pb = pi_buffer_new(b->size), *out = pi_buffer_new(0xffff);
      struct CalendarEvent ev, ev2, ev3;
      char *ics, *ics2, *s1, *s2;
      pi_buffer_append(pb, b->buf, b->size);
      memset(&ev, 0, sizeof(ev));
      memset(&ev2, 0, sizeof(ev2));
      memset(&ev3, 0, sizeof(ev3));
      unpack_CalendarEvent(&ev, pb, calendar_v1);
      ics = rs_event_to_ical(&ev, "x", NULL, 0, NULL);
      ev2.repeatFrequency = 1;
      rs_ical_to_event(ics, &ev2, NULL, NULL);
      pack_CalendarEvent(&ev2, out, calendar_v1);
      unpack_CalendarEvent(&ev3, out, calendar_v1);
      ics2 = rs_event_to_ical(&ev3, "x", NULL, 0, NULL);
      s1 = stable(ics);
      s2 = stable(ics2);
      n++;
      if (strcmp(s1, s2)) {
         bad++;
         if (bad <= 8) {
            char *f1 = g_strdup_printf("/tmp/rt-a-%d", bad), *f2 = g_strdup_printf("/tmp/rt-b-%d", bad);
            g_file_set_contents(f1, s1, -1, NULL);
            g_file_set_contents(f2, s2, -1, NULL);
            printf("     roundtrip differs: \"%s\" (diff %s %s)\n", ev.description, f1, f2);
            g_free(f1); g_free(f2);
         }
      }
      g_free(ics); g_free(ics2); g_free(s1); g_free(s2);
      free_CalendarEvent(&ev);
      free_CalendarEvent(&ev2);
      free_CalendarEvent(&ev3);
      pi_buffer_free(pb);
      pi_buffer_free(out);
   }
   CHECK(bad == 0, "calendar round trip: %d of %d events identical", n - bad, n);
}

static void roundtrip_contacts(void)
{
   GList *l;
   int n = 0, bad = 0;
   for (l = stub_records(CON); l; l = l->next) {
      buf_rec *b = l->data;
      pi_buffer_t *pb = pi_buffer_new(b->size), *out = pi_buffer_new(0xffff);
      struct Contact c, c2, c3;
      char *vcf, *vcf2, *s1, *s2;
      pi_buffer_append(pb, b->buf, b->size);
      memset(&c, 0, sizeof(c));
      memset(&c2, 0, sizeof(c2));
      memset(&c3, 0, sizeof(c3));
      unpack_Contact(&c, pb, contacts_v10);
      vcf = rs_contact_to_vcard(&c, "x", NULL, 0, NULL);
      rs_vcard_to_contact(vcf, &c2, NULL, NULL);
      pack_Contact(&c2, out, contacts_v10);
      unpack_Contact(&c3, out, contacts_v10);
      vcf2 = rs_contact_to_vcard(&c3, "x", NULL, 0, NULL);
      s1 = stable(vcf);
      s2 = stable(vcf2);
      n++;
      if (strcmp(s1, s2)) {
         bad++;
         if (bad <= 8) {
            char *f1 = g_strdup_printf("/tmp/rc-a-%d", bad), *f2 = g_strdup_printf("/tmp/rc-b-%d", bad);
            g_file_set_contents(f1, s1, -1, NULL);
            g_file_set_contents(f2, s2, -1, NULL);
            printf("     roundtrip differs: %s (diff %s %s)\n", c.entry[0] ? c.entry[0] : "", f1, f2);
            g_free(f1); g_free(f2);
         }
      }
      g_free(vcf); g_free(vcf2); g_free(s1); g_free(s2);
      free_Contact(&c);
      free_Contact(&c2);
      free_Contact(&c3);
      pi_buffer_free(pb);
      pi_buffer_free(out);
   }
   CHECK(bad == 0, "contacts round trip: %d of %d contacts identical", n - bad, n);
}

int main(int argc, char **argv)
{
   char *p;
   int ncal, ncon, ntodo, before;
   buf_rec *b;
   char *href, *body, *etag;
   struct CalendarEvent ev;

   if (argc < 5) {
      fprintf(stderr, "usage: rstest URL USER PASS PDB_DIR\n");
      return 2;
   }
   logbuf = g_string_new(NULL);
   rs_set_log_func(capture, NULL);
   stub_prefs[86] = 1;   /* Calendar */
   stub_prefs[87] = 1;   /* Contacts */
   stub_prefs[88] = 0;   /* ToDo */

   p = g_build_filename(argv[4], "CalendarDB-PDat.pdb", NULL);
   ncal = stub_seed_from_pdb(p, CAL);
   g_free(p);
   p = g_build_filename(argv[4], "ContactsDB-PAdd.pdb", NULL);
   ncon = stub_seed_from_pdb(p, CON);
   g_free(p);
   p = g_build_filename(argv[4], "ToDoDB.pdb", NULL);
   ntodo = stub_seed_from_pdb(p, TODO);
   g_free(p);
   printf("seeded %d events, %d contacts, %d to-dos\n", ncal, ncon, ntodo);

   roundtrip_events();
   roundtrip_contacts();

   S.url = g_strdup(argv[1]);
   S.username = g_strdup(argv[2]);
   S.password = g_strdup("wrong");
   S.cal_path = g_strdup(P("calendar/"));
   S.todo_path = g_strdup(P("tasks/"));
   S.card_path = g_strdup(P("contacts/"));
   S.sync_cal = S.sync_todo = S.sync_card = TRUE;

   /* discovery + bad password */
   sync_now("wrong password");
   CHECK(log_has("rejected the username or password"), "wrong password reported");
   CHECK(remote_count(P("calendar/")) == 0, "nothing uploaded with wrong password");
   g_free(S.password);
   S.password = g_strdup(argv[3]);
   {
      char *err = NULL;
      GList *c = rs_discover(&S, &err);
      CHECK(g_list_length(c) == 3, "discovery finds 3 collections (%s)", err ? err : "");
      g_list_free_full(c, (GDestroyNotify)rs_dav_item_free);
   }

   /* 1. first sync uploads everything */
   sync_now("initial");
   CHECK(remote_count(P("calendar/")) == stub_live_count(CAL),
         "all %d events on server", stub_live_count(CAL));
   CHECK(remote_count(P("contacts/")) == stub_live_count(CON),
         "all %d contacts on server", stub_live_count(CON));
   CHECK(remote_count(P("tasks/")) == stub_live_count(TODO),
         "all %d to-dos on server", stub_live_count(TODO));

   /* 2. nothing changed -> nothing happens */
   sync_now("no changes");
   CHECK(!log_changed(), "second sync is a no-op");

   /* 3. local change goes up */
   b = live_rec(CAL, 0);
   local_modify_event(b, "Changed in J-Pilot");
   sync_now("local edit");
   href = remote_find(P("calendar/"), "SUMMARY:Changed in J-Pilot", &body);
   CHECK(href != NULL, "edited event uploaded");
   CHECK(remote_count(P("calendar/")) == stub_live_count(CAL), "no duplicate after edit");

   /* 4. server change comes down (and keeps unknown properties) */
   {
      char *nb = g_strdup(body), *pos = strstr(nb, "SUMMARY:Changed in J-Pilot");
      GString *g = g_string_new(NULL);
      g_string_append_len(g, nb, pos - nb);
      g_string_append(g, "SUMMARY:Changed on server\r\nX-SERVER-ONLY:keep me");
      g_string_append(g, pos + strlen("SUMMARY:Changed in J-Pilot"));
      CHECK(remote_put(href, g->str, "text/calendar") / 100 == 2, "server-side edit stored");
      g_string_free(g, TRUE);
      g_free(nb);
   }
   sync_now("server edit");
   CHECK(find_event("Changed on server", NULL) != NULL, "server edit arrived in J-Pilot");
   CHECK(!find_event("Changed in J-Pilot", NULL), "old version gone locally");

   /* local edit again: unknown server property must survive */
   local_modify_event(find_event("Changed on server", NULL), "Edited again");
   sync_now("local edit keeps X- props");
   g_free(body);
   body = remote_get(href, NULL);
   CHECK(strstr(body, "X-SERVER-ONLY:keep me") && strstr(body, "SUMMARY:Edited again"),
         "server-only property preserved on upload");

   /* 5. new recurring event created on the server */
   {
      const char *ics =
         "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:test\r\n"
         "BEGIN:VEVENT\r\nUID:server-new-1\r\nDTSTAMP:20261030T120000Z\r\n"
         "DTSTART;TZID=Europe/Rome:20261103T183000\r\nDTEND;TZID=Europe/Rome:20261103T200000\r\n"
         "RRULE:FREQ=WEEKLY;BYDAY=TU,TH;COUNT=6\r\n"
         "EXDATE;TZID=Europe/Rome:20261105T183000\r\n"
         "SUMMARY:Choir practice àèé\r\nLOCATION:Sala 2\r\nCATEGORIES:Personal\r\n"
         "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\nDESCRIPTION:x\r\nEND:VALARM\r\n"
         "END:VEVENT\r\nEND:VCALENDAR\r\n";
      CHECK(remote_put(P("calendar/server-new-1.ics"), ics, "text/calendar") / 100 == 2,
            "server-side new event stored");
   }
   before = stub_live_count(CAL);
   sync_now("server new event");
   CHECK(stub_live_count(CAL) == before + 1, "new server event added locally");
   b = find_event("Choir practice", &ev);
   CHECK(b && ev.begin.tm_hour == 18 && ev.begin.tm_min == 30 && ev.end.tm_hour == 20,
         "time 18:30-20:00");
   CHECK(b && ev.repeatType == calendarRepeatWeekly && ev.repeatDays[2] && ev.repeatDays[4],
         "weekly Tue/Thu");
   CHECK(b && !ev.repeatForever && ev.repeatEnd.tm_mday == 19 && ev.repeatEnd.tm_mon == 10,
         "COUNT=6 -> ends 19 Nov (got %d/%d)", ev.repeatEnd.tm_mday, ev.repeatEnd.tm_mon + 1);
   CHECK(b && ev.exceptions == 1 && ev.exception[0].tm_mday == 5, "exception on 5 Nov");
   CHECK(b && ev.alarm && ev.advance == 15 && ev.advanceUnits == calendar_advMinutes,
         "alarm 15 minutes before");
   CHECK(b && ev.location && !strcmp(ev.location, "Sala 2"), "location");
   CHECK(b && ev.description && strstr(ev.description, "\xe0\xe8\xe9"),
         "accented text converted to Palm charset");
   if (b)
      free_CalendarEvent(&ev);

   /* editing a record that only exists on the PC gives it a new id in
    * J-Pilot; the server object must be updated, not replaced */
   local_modify_event(find_event("Choir practice", NULL), "Choir practice (moved)");
   sync_now("edit of PC record");
   CHECK(!log_has("deleted /") && log_has("Calendar: 1 sent"), "edit sent as update, not delete+add");
   body = remote_get(P("calendar/server-new-1.ics"), NULL);
   CHECK(body && strstr(body, "SUMMARY:Choir practice (moved)") && strstr(body, "UID:server-new-1"),
         "same server object updated");
   CHECK(remote_count(P("calendar/")) == stub_live_count(CAL), "no duplicates after PC edit");

   /* 6. HotSync gives the new record a Palm id: must relink, not duplicate */
   stub_hotsync(CAL);
   stub_hotsync(CON);
   stub_hotsync(TODO);
   sync_now("after hotsync");
   CHECK(!log_changed(), "ids changed by HotSync are relinked");
   CHECK(remote_count(P("calendar/")) == stub_live_count(CAL), "still no duplicates");

   /* 7. deletions both ways */
   b = find_event("Choir practice", NULL);
   jp_delete_record(CAL, b, DELETE_FLAG);
   sync_now("local delete");
   CHECK(!remote_find(P("calendar/"), "Choir practice", NULL), "deleted on server");

   href = remote_find(P("calendar/"), "SUMMARY:Edited again", NULL);
   remote_delete(href);
   sync_now("server delete");
   CHECK(!find_event("Edited again", NULL), "server delete removed it locally");
   CHECK(remote_count(P("calendar/")) == stub_live_count(CAL), "counts match");

   /* 8. conflict, server wins by default */
   b = live_rec(CAL, 3);
   local_modify_event(b, "Conflict local");
   sync_now("prep");   /* uploads Conflict local */
   href = remote_find(P("calendar/"), "SUMMARY:Conflict local", &body);
   local_modify_event(find_event("Conflict local", NULL), "Conflict local 2");
   {
      char *pos = strstr(body, "SUMMARY:Conflict local");
      GString *g = g_string_new(NULL);
      g_string_append_len(g, body, pos - body);
      g_string_append(g, "SUMMARY:Conflict server");
      g_string_append(g, pos + strlen("SUMMARY:Conflict local"));
      remote_put(href, g->str, "text/calendar");
      g_string_free(g, TRUE);
   }
   sync_now("conflict");
   CHECK(find_event("Conflict server", NULL) && !find_event("Conflict local 2", NULL),
         "conflict: server version kept");

   /* 9. contacts from the server */
   {
      const char *vcf =
         "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:card-1\r\nN:Rossi;Maria;;;\r\nFN:Maria Rossi\r\n"
         "ORG:ACME S.p.A.\r\nTITLE:CTO\r\nTEL;TYPE=CELL:+39 333 1234567\r\n"
         "TEL;TYPE=WORK,VOICE:+39 06 123456\r\nEMAIL;TYPE=INTERNET:maria@example.com\r\n"
         "ADR;TYPE=HOME:;;Via Roma 1;Roma;RM;00100;Italia\r\nBDAY:1980-05-17\r\n"
         "NOTE:Line one\\nLine two\\, with comma\r\n"
         "PHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQSkZJRgABAQAAAQABAAA=\r\nEND:VCARD\r\n";
      CHECK(remote_put(P("contacts/card-1.vcf"), vcf, "text/vcard") / 100 == 2,
            "server-side new contact stored");
   }
   before = stub_live_count(CON);
   sync_now("server new contact");
   CHECK(stub_live_count(CON) == before + 1, "contact added locally");
   {
      GList *l;
      gboolean found = FALSE;
      for (l = stub_records(CON); l; l = l->next) {
         buf_rec *r = l->data;
         pi_buffer_t *pb;
         struct Contact c;
         if (r->rt != NEW_PC_REC)
            continue;
         pb = pi_buffer_new(r->size);
         pi_buffer_append(pb, r->buf, r->size);
         memset(&c, 0, sizeof(c));
         unpack_Contact(&c, pb, contacts_v10);
         if (c.entry[contLastname] && !strcmp(c.entry[contLastname], "Rossi")) {
            int i, cell = -1, mail = -1;
            found = TRUE;
            for (i = 0; i < 7; i++) {
               if (c.entry[contPhone1 + i] && c.phoneLabel[i] == 7) cell = i;
               if (c.entry[contPhone1 + i] && c.phoneLabel[i] == 4) mail = i;
            }
            CHECK(cell >= 0 && !strcmp(c.entry[contPhone1 + cell], "+39 333 1234567"), "mobile number");
            CHECK(mail >= 0 && !strcmp(c.entry[contPhone1 + mail], "maria@example.com"), "e-mail");
            CHECK(c.entry[contCity1] && !strcmp(c.entry[contCity1], "Roma") && c.addressLabel[0] == 1,
                  "home address");
            CHECK(c.birthdayFlag && c.birthday.tm_mday == 17 && c.birthday.tm_mon == 4, "birthday");
            CHECK(c.entry[contNote] && !strcmp(c.entry[contNote], "Line one\nLine two, with comma"),
                  "note unescaped");
            /* edit it locally: PHOTO must survive the upload */
            free(c.entry[contTitle]);
            c.entry[contTitle] = strdup("CEO");
            {
               pi_buffer_t *out = pi_buffer_new(0xffff);
               buf_rec nb = *r;
               pack_Contact(&c, out, contacts_v10);
               jp_delete_record(CON, r, MODIFY_FLAG);
               nb.buf = out->data;
               nb.size = out->used;
               nb.unique_id = 0;
               jp_pc_write(CON, &nb);
               pi_buffer_free(out);
            }
            free_Contact(&c);
            pi_buffer_free(pb);
            break;
         }
         free_Contact(&c);
         pi_buffer_free(pb);
      }
      CHECK(found, "contact Rossi present");
   }
   sync_now("local contact edit");
   body = remote_get(P("contacts/card-1.vcf"), &etag);
   CHECK(body && strstr(body, "TITLE:CEO") && strstr(body, "PHOTO;") && strstr(body, "UID:card-1"),
         "contact edit uploaded, PHOTO and UID kept");

   /* 10. to-do completed locally */
   if (stub_live_count(TODO) > 0) {
      buf_rec *t = live_rec(TODO, 0);
      pi_buffer_t *pb = pi_buffer_new(t->size), *out = pi_buffer_new(0xffff);
      struct ToDo td;
      buf_rec nb;
      char *desc;
      pi_buffer_append(pb, t->buf, t->size);
      memset(&td, 0, sizeof(td));
      unpack_ToDo(&td, pb, todo_v1);
      td.complete = !td.complete;
      desc = g_strdup(td.description);
      pack_ToDo(&td, out, todo_v1);
      nb = *t;
      nb.buf = out->data;
      nb.size = out->used;
      nb.rt = REPLACEMENT_PALM_REC;
      jp_delete_record(TODO, t, MODIFY_FLAG);
      jp_pc_write(TODO, &nb);
      sync_now("todo toggled");
      CHECK(log_has("To-dos: sent"), "to-do change uploaded");
      free_ToDo(&td);
      pi_buffer_free(pb);
      pi_buffer_free(out);
      g_free(desc);
   }

   /* 11. safety: server collection emptied -> refuse to wipe J-Pilot */
   {
      GList *items = remote_list(P("calendar/")), *l;
      before = stub_live_count(CAL);
      for (l = items; l; l = l->next)
         remote_delete(((RsDavItem *)l->data)->href);
      g_list_free_full(items, (GDestroyNotify)rs_dav_item_free);
      sync_now("server emptied");
      CHECK(stub_live_count(CAL) == before && log_has("Not deleting anything"),
            "empty server collection does not wipe J-Pilot");
   }

   printf("\n%d checks, %d failures\n", checks, failures);
   return failures ? 1 : 0;
}
