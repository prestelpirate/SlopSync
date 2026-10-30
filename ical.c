/* radicalesync - Palm CalendarEvent / ToDo <-> iCalendar (libical) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libical/ical.h>
#include "rs.h"

#define PRODID "-//J-Pilot//radicalesync 1.0//EN"

/* ---------- time zone helpers ---------- */

static icaltimezone *local_zone(void)
{
   static icaltimezone *zone;
   static gboolean looked;
   char *name = NULL;

   if (looked)
      return zone;
   looked = TRUE;
   /* plain "Europe/Rome" TZIDs, as other clients write them */
   icaltimezone_set_tzid_prefix("");
   if (g_getenv("TZ") && *g_getenv("TZ") && g_getenv("TZ")[0] != ':')
      name = g_strdup(g_getenv("TZ"));
   if (!name && g_file_get_contents("/etc/timezone", &name, NULL, NULL))
      g_strstrip(name);
   if (!name) {
      char *link = g_file_read_link("/etc/localtime", NULL);
      if (link) {
         char *p = strstr(link, "zoneinfo/");
         if (p)
            name = g_strdup(p + 9);
         g_free(link);
      }
   }
   if (name)
      zone = icaltimezone_get_builtin_timezone(name);
   g_free(name);
   return zone;
}

static struct icaltimetype tm_to_ical(const struct tm *t, gboolean date_only)
{
   struct icaltimetype it = icaltime_null_time();

   it.year = t->tm_year + 1900;
   it.month = t->tm_mon + 1;
   it.day = t->tm_mday;
   if (date_only) {
      it.is_date = 1;
   } else {
      it.hour = t->tm_hour;
      it.minute = t->tm_min;
      it.zone = local_zone();
   }
   return it;
}

/* Server time -> local broken-down time, as the Palm stores it */
static void ical_to_tm(struct icaltimetype it, struct tm *t)
{
   memset(t, 0, sizeof(*t));
   if (!it.is_date && (icaltime_is_utc(it) || it.zone)) {
      time_t tt = icaltime_as_timet_with_zone(it, it.zone ? it.zone
                                              : icaltimezone_get_utc_timezone());
      localtime_r(&tt, t);
      t->tm_sec = 0;
      return;
   }
   t->tm_year = it.year - 1900;
   t->tm_mon = it.month - 1;
   t->tm_mday = it.day;
   if (!it.is_date) {
      t->tm_hour = it.hour;
      t->tm_min = it.minute;
   }
   t->tm_isdst = -1;
   mktime(t);   /* normalise wday/yday */
}

static void set_dt(icalcomponent *c, icalproperty *p, struct icaltimetype it)
{
   if (!it.is_date && it.zone)
      icalproperty_add_parameter(p, icalparameter_new_tzid(
         icaltimezone_get_tzid((icaltimezone *)it.zone)));
   icalcomponent_add_property(c, p);
}

/* ---------- generic helpers ---------- */

static void remove_props(icalcomponent *c, icalproperty_kind kind)
{
   icalproperty *p;
   while ((p = icalcomponent_get_first_property(c, kind))) {
      icalcomponent_remove_property(c, p);
      icalproperty_free(p);
   }
}

static void remove_subcomps(icalcomponent *c, icalcomponent_kind kind)
{
   icalcomponent *s;
   while ((s = icalcomponent_get_first_component(c, kind))) {
      icalcomponent_remove_component(c, s);
      icalcomponent_free(s);
   }
}

static icalcomponent *find_master(icalcomponent *cal, icalcomponent_kind kind)
{
   icalcomponent *c, *first = NULL;

   if (icalcomponent_isa(cal) == kind)
      return cal;
   for (c = icalcomponent_get_first_component(cal, kind); c;
        c = icalcomponent_get_next_component(cal, kind)) {
      if (!first)
         first = c;
      if (!icalcomponent_get_first_property(c, ICAL_RECURRENCEID_PROPERTY))
         return c;
   }
   return first;
}

static void ensure_timezone(icalcomponent *cal)
{
   icaltimezone *z = local_zone();
   icalcomponent *vtz;
   const char *tzid;

   if (!z)
      return;
   tzid = icaltimezone_get_tzid(z);
   for (vtz = icalcomponent_get_first_component(cal, ICAL_VTIMEZONE_COMPONENT); vtz;
        vtz = icalcomponent_get_next_component(cal, ICAL_VTIMEZONE_COMPONENT)) {
      icalproperty *p = icalcomponent_get_first_property(vtz, ICAL_TZID_PROPERTY);
      if (p && !strcmp(icalproperty_get_tzid(p), tzid))
         return;
   }
   vtz = icalcomponent_new_clone(icaltimezone_get_component(z));
   {
      /* the full history since the 1800s is not needed */
      struct icaltimetype from = icaltime_from_string("19700101T000000Z");
      icaltimezone_truncate_vtimezone(vtz, from, icaltime_null_time(), 1);
   }
   icalcomponent_add_component(cal, vtz);
}

/* Parse merge_with or build a fresh VCALENDAR; returns the wanted component */
static icalcomponent *start_component(const char *merge_with, icalcomponent_kind kind,
                                      const char *uid, icalcomponent **cal_out)
{
   icalcomponent *cal = NULL, *comp = NULL;

   if (merge_with) {
      cal = icalparser_parse_string(merge_with);
      if (cal && icalcomponent_isa(cal) != ICAL_VCALENDAR_COMPONENT) {
         icalcomponent *wrap = icalcomponent_new(ICAL_VCALENDAR_COMPONENT);
         icalcomponent_add_component(wrap, cal);
         cal = wrap;
      }
      if (cal)
         comp = find_master(cal, kind);
      if (!comp && cal) {
         icalcomponent_free(cal);
         cal = NULL;
      }
   }
   if (!cal) {
      cal = icalcomponent_new(ICAL_VCALENDAR_COMPONENT);
      icalcomponent_add_property(cal, icalproperty_new_version("2.0"));
      icalcomponent_add_property(cal, icalproperty_new_prodid(PRODID));
      comp = icalcomponent_new(kind);
      icalcomponent_add_property(comp, icalproperty_new_uid(uid));
      icalcomponent_add_component(cal, comp);
   } else {
      icalproperty *seq = icalcomponent_get_first_property(comp, ICAL_SEQUENCE_PROPERTY);
      if (seq)
         icalproperty_set_sequence(seq, icalproperty_get_sequence(seq) + 1);
   }
   remove_props(comp, ICAL_DTSTAMP_PROPERTY);
   remove_props(comp, ICAL_LASTMODIFIED_PROPERTY);
   icalcomponent_add_property(comp, icalproperty_new_dtstamp(
      icaltime_current_time_with_zone(icaltimezone_get_utc_timezone())));
   icalcomponent_add_property(comp, icalproperty_new_lastmodified(
      icaltime_current_time_with_zone(icaltimezone_get_utc_timezone())));
   *cal_out = cal;
   return comp;
}

static void set_text_prop(icalcomponent *c, icalproperty_kind kind, const char *v)
{
   remove_props(c, kind);
   if (!v || !*v)
      return;
   switch (kind) {
    case ICAL_SUMMARY_PROPERTY:
      icalcomponent_add_property(c, icalproperty_new_summary(v));
      break;
    case ICAL_DESCRIPTION_PROPERTY:
      icalcomponent_add_property(c, icalproperty_new_description(v));
      break;
    case ICAL_LOCATION_PROPERTY:
      icalcomponent_add_property(c, icalproperty_new_location(v));
      break;
    default:
      break;
   }
}

static void set_category_class(icalcomponent *c, const char *category, int private_rec)
{
   icalproperty *p;
   gboolean have_cat = FALSE;

   /* Keep other categories the server knows about; only make sure the
    * Palm category comes first. */
   if (category && *category) {
      for (p = icalcomponent_get_first_property(c, ICAL_CATEGORIES_PROPERTY); p;
           p = icalcomponent_get_next_property(c, ICAL_CATEGORIES_PROPERTY)) {
         if (!g_ascii_strcasecmp(icalproperty_get_categories(p), category))
            have_cat = TRUE;
      }
      if (!have_cat) {
         GSList *keep = NULL, *l;
         while ((p = icalcomponent_get_first_property(c, ICAL_CATEGORIES_PROPERTY))) {
            icalcomponent_remove_property(c, p);
            keep = g_slist_append(keep, p);
         }
         icalcomponent_add_property(c, icalproperty_new_categories(category));
         for (l = keep; l; l = l->next)
            icalcomponent_add_property(c, l->data);
         g_slist_free(keep);
      }
   }
   remove_props(c, ICAL_CLASS_PROPERTY);
   if (private_rec)
      icalcomponent_add_property(c, icalproperty_new_class(ICAL_CLASS_PRIVATE));
}

static void get_category_class(icalcomponent *c, char **category, int *private_rec)
{
   icalproperty *p;

   if (category) {
      *category = NULL;
      p = icalcomponent_get_first_property(c, ICAL_CATEGORIES_PROPERTY);
      if (p && icalproperty_get_categories(p)) {
         /* a single CATEGORIES line may carry a comma list */
         char **parts = g_strsplit(icalproperty_get_categories(p), ",", 2);
         *category = g_strdup(g_strstrip(parts[0]));
         g_strfreev(parts);
      }
   }
   if (private_rec) {
      p = icalcomponent_get_first_property(c, ICAL_CLASS_PROPERTY);
      *private_rec = p && icalproperty_get_class(p) != ICAL_CLASS_PUBLIC;
   }
}

static char *finish(icalcomponent *cal)
{
   char *s = icalcomponent_as_ical_string_r(cal);
   char *r = g_strdup(s);
   free(s);
   icalcomponent_free(cal);
   return r;
}

static const char *const byday_names[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

/* libc-allocated copy: pilot-link frees with free() */
static char *cdup(const char *s)
{
   return s ? strdup(s) : NULL;
}

static void replace_str(char **dst, const char *src)
{
   free(*dst);
   *dst = (src && *src) ? cdup(src) : NULL;
}

/* ---------- events ---------- */

char *rs_event_to_ical(const struct CalendarEvent *ev, const char *uid,
                       const char *category, int private_rec,
                       const char *merge_with)
{
   icalcomponent *cal, *c;
   struct icaltimetype start, end;
   int i;

   c = start_component(merge_with, ICAL_VEVENT_COMPONENT, uid, &cal);

   set_text_prop(c, ICAL_SUMMARY_PROPERTY, ev->description);
   set_text_prop(c, ICAL_DESCRIPTION_PROPERTY, ev->note);
   set_text_prop(c, ICAL_LOCATION_PROPERTY, ev->location);
   set_category_class(c, category, private_rec);

   remove_props(c, ICAL_DTSTART_PROPERTY);
   remove_props(c, ICAL_DTEND_PROPERTY);
   remove_props(c, ICAL_DURATION_PROPERTY);
   if (ev->event) {
      start = tm_to_ical(&ev->begin, TRUE);
      end = icaltime_add(start, icaldurationtype_from_int(24 * 3600));
   } else {
      struct tm e = ev->begin;
      start = tm_to_ical(&ev->begin, FALSE);
      e.tm_hour = ev->end.tm_hour;
      e.tm_min = ev->end.tm_min;
      end = tm_to_ical(&e, FALSE);
      if (icaltime_compare(end, start) < 0)
         end = start;
      if (start.zone)
         ensure_timezone(cal);
   }
   set_dt(c, icalproperty_new_dtstart(start), start);
   set_dt(c, icalproperty_new_dtend(end), end);

   /* recurrence */
   remove_props(c, ICAL_RRULE_PROPERTY);
   remove_props(c, ICAL_EXDATE_PROPERTY);
   if (ev->repeatType != calendarRepeatNone) {
      GString *r = g_string_new("FREQ=");
      switch (ev->repeatType) {
       case calendarRepeatDaily:
         g_string_append(r, "DAILY");
         break;
       case calendarRepeatWeekly: {
         gboolean first = TRUE;
         g_string_append(r, "WEEKLY;BYDAY=");
         for (i = 0; i < 7; i++) {
            if (ev->repeatDays[i]) {
               g_string_append_printf(r, "%s%s", first ? "" : ",", byday_names[i]);
               first = FALSE;
            }
         }
         if (first)
            g_string_append(r, byday_names[ev->begin.tm_wday]);
         g_string_append_printf(r, ";WKST=%s", ev->repeatWeekstart == 1 ? "MO" : "SU");
         break;
       }
       case calendarRepeatMonthlyByDay: {
         int week = ev->repeatDay / 7, wday = ev->repeatDay % 7;
         g_string_append_printf(r, "MONTHLY;BYDAY=%d%s", week >= 4 ? -1 : week + 1,
                                byday_names[wday]);
         break;
       }
       case calendarRepeatMonthlyByDate:
         g_string_append_printf(r, "MONTHLY;BYMONTHDAY=%d", ev->begin.tm_mday);
         break;
       case calendarRepeatYearly:
         g_string_append(r, "YEARLY");
         break;
       default:
         break;
      }
      if (ev->repeatFrequency > 1)
         g_string_append_printf(r, ";INTERVAL=%d", ev->repeatFrequency);
      if (!ev->repeatForever) {
         if (ev->event)
            g_string_append_printf(r, ";UNTIL=%04d%02d%02d",
                                   ev->repeatEnd.tm_year + 1900,
                                   ev->repeatEnd.tm_mon + 1, ev->repeatEnd.tm_mday);
         else {
            /* UNTIL must be UTC when DTSTART has a TZID */
            struct tm u = ev->repeatEnd;
            time_t tt;
            struct tm g;
            u.tm_hour = 23;
            u.tm_min = 59;
            u.tm_sec = 59;
            u.tm_isdst = -1;
            tt = mktime(&u);
            gmtime_r(&tt, &g);
            g_string_append_printf(r, ";UNTIL=%04d%02d%02dT%02d%02d%02dZ",
                                   g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                                   g.tm_hour, g.tm_min, g.tm_sec);
         }
      }
      icalcomponent_add_property(c, icalproperty_new_rrule(
         icalrecurrencetype_from_string(r->str)));
      g_string_free(r, TRUE);

      for (i = 0; i < ev->exceptions; i++) {
         struct tm x = ev->exception[i];
         struct icaltimetype xt;
         if (ev->event) {
            xt = tm_to_ical(&x, TRUE);
         } else {
            x.tm_hour = ev->begin.tm_hour;
            x.tm_min = ev->begin.tm_min;
            xt = tm_to_ical(&x, FALSE);
         }
         set_dt(c, icalproperty_new_exdate(xt), xt);
      }
   }

   /* alarm */
   remove_subcomps(c, ICAL_VALARM_COMPONENT);
   if (ev->alarm) {
      icalcomponent *a = icalcomponent_new(ICAL_VALARM_COMPONENT);
      struct icaltriggertype trig;
      int secs = ev->advance * (ev->advanceUnits == calendar_advDays ? 86400 :
                                ev->advanceUnits == calendar_advHours ? 3600 : 60);
      memset(&trig, 0, sizeof(trig));
      trig.time = icaltime_null_time();
      trig.duration = icaldurationtype_from_int(-secs);
      icalcomponent_add_property(a, icalproperty_new_action(ICAL_ACTION_DISPLAY));
      icalcomponent_add_property(a, icalproperty_new_trigger(trig));
      icalcomponent_add_property(a, icalproperty_new_description(
         ev->description ? ev->description : "Reminder"));
      icalcomponent_add_component(c, a);
   }

   return finish(cal);
}

/* Last date a COUNT-limited rule produces */
static struct icaltimetype last_occurrence(struct icalrecurrencetype rec,
                                           struct icaltimetype start)
{
   icalrecur_iterator *it = icalrecur_iterator_new(rec, start);
   struct icaltimetype t, last = start;
   int guard = 0;

   if (!it)
      return start;
   while (!icaltime_is_null_time(t = icalrecur_iterator_next(it)) && guard++ < 100000)
      last = t;
   icalrecur_iterator_free(it);
   return last;
}

static int ical_day_to_palm(short d)
{
   /* libical: 1=SU .. 7=SA */
   return (icalrecurrencetype_day_day_of_week(d) - 1 + 7) % 7;
}

int rs_ical_to_event(const char *ical, struct CalendarEvent *ev,
                     char **category, int *private_rec)
{
   icalcomponent *cal = icalparser_parse_string(ical), *c;
   icalproperty *p;
   struct icaltimetype start, end;
   int i;

   if (!cal)
      return -1;
   c = find_master(cal, ICAL_VEVENT_COMPONENT);
   if (!c) {
      icalcomponent_free(cal);
      return -1;
   }

   replace_str(&ev->description, icalcomponent_get_summary(c));
   p = icalcomponent_get_first_property(c, ICAL_DESCRIPTION_PROPERTY);
   replace_str(&ev->note, p ? icalproperty_get_description(p) : NULL);
   p = icalcomponent_get_first_property(c, ICAL_LOCATION_PROPERTY);
   replace_str(&ev->location, p ? icalproperty_get_location(p) : NULL);
   get_category_class(c, category, private_rec);

   start = icalcomponent_get_dtstart(c);
   end = icalcomponent_get_dtend(c);
   ev->event = start.is_date;
   ical_to_tm(start, &ev->begin);
   if (icaltime_is_null_time(end))
      end = start;
   ical_to_tm(end, &ev->end);
   if (ev->event) {
      ev->end = ev->begin;
   } else if (ev->end.tm_year != ev->begin.tm_year || ev->end.tm_yday != ev->begin.tm_yday) {
      /* the Palm cannot end on another day: stop at midnight */
      int hour = ev->end.tm_hour, min = ev->end.tm_min;
      ev->end = ev->begin;
      if (hour == 0 && min == 0) {
         ev->end.tm_hour = 23;
         ev->end.tm_min = 59;
      } else {
         ev->end.tm_hour = 23;
         ev->end.tm_min = 59;
      }
   }

   /* recurrence */
   ev->repeatType = calendarRepeatNone;
   ev->repeatForever = 1;
   ev->repeatFrequency = 1;
   ev->repeatDay = 0;
   memset(ev->repeatDays, 0, sizeof(ev->repeatDays));
   ev->repeatWeekstart = 0;
   free(ev->exception);
   ev->exception = NULL;
   ev->exceptions = 0;

   p = icalcomponent_get_first_property(c, ICAL_RRULE_PROPERTY);
   if (p) {
      struct icalrecurrencetype rec = icalproperty_get_rrule(p);
      ev->repeatFrequency = rec.interval > 0 ? rec.interval : 1;
      switch (rec.freq) {
       case ICAL_DAILY_RECURRENCE:
         ev->repeatType = calendarRepeatDaily;
         break;
       case ICAL_WEEKLY_RECURRENCE:
         ev->repeatType = calendarRepeatWeekly;
         for (i = 0; i < ICAL_BY_DAY_SIZE && rec.by_day[i] != ICAL_RECURRENCE_ARRAY_MAX; i++)
            ev->repeatDays[ical_day_to_palm(rec.by_day[i])] = 1;
         if (i == 0)
            ev->repeatDays[ev->begin.tm_wday] = 1;
         ev->repeatWeekstart = rec.week_start == ICAL_MONDAY_WEEKDAY ? 1 : 0;
         break;
       case ICAL_MONTHLY_RECURRENCE:
         if (rec.by_day[0] != ICAL_RECURRENCE_ARRAY_MAX) {
            int pos = icalrecurrencetype_day_position(rec.by_day[0]);
            int wday = ical_day_to_palm(rec.by_day[0]);
            if (pos == 0 && rec.by_set_pos[0] != ICAL_RECURRENCE_ARRAY_MAX)
               pos = rec.by_set_pos[0];
            if (pos == 0)
               pos = (ev->begin.tm_mday - 1) / 7 + 1;
            ev->repeatType = calendarRepeatMonthlyByDay;
            ev->repeatDay = (pos < 0 || pos > 4 ? 4 : pos - 1) * 7 + wday;
         } else {
            ev->repeatType = calendarRepeatMonthlyByDate;
         }
         break;
       case ICAL_YEARLY_RECURRENCE:
         ev->repeatType = calendarRepeatYearly;
         break;
       default:
         rs_log("Event \"%s\": repeat rule not supported on the Palm, imported once",
                ev->description ? ev->description : "");
         break;
      }
      if (ev->repeatType != calendarRepeatNone) {
         struct icaltimetype until = icaltime_null_time();
         if (!icaltime_is_null_time(rec.until))
            until = rec.until;
         else if (rec.count > 0)
            until = last_occurrence(rec, start);
         if (!icaltime_is_null_time(until)) {
            ev->repeatForever = 0;
            ical_to_tm(until, &ev->repeatEnd);
            ev->repeatEnd.tm_hour = ev->repeatEnd.tm_min = 0;
         }
      }
   } else if (ev->event) {
      /* multi-day all-day event -> daily repeat, the Palm way */
      struct icaltimetype last = icaltime_add(end, icaldurationtype_from_int(-86400));
      if (icaltime_compare_date_only(last, start) > 0) {
         ev->repeatType = calendarRepeatDaily;
         ev->repeatForever = 0;
         ical_to_tm(last, &ev->repeatEnd);
      }
   }

   if (ev->repeatType != calendarRepeatNone) {
      int n = 0;
      for (p = icalcomponent_get_first_property(c, ICAL_EXDATE_PROPERTY); p;
           p = icalcomponent_get_next_property(c, ICAL_EXDATE_PROPERTY))
         n++;
      if (n) {
         ev->exception = calloc(n, sizeof(struct tm));
         for (p = icalcomponent_get_first_property(c, ICAL_EXDATE_PROPERTY); p;
              p = icalcomponent_get_next_property(c, ICAL_EXDATE_PROPERTY)) {
            struct icaltimetype x = icalproperty_get_exdate(p);
            icalparameter *tz = icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER);
            if (tz && !x.zone)
               x.zone = icaltimezone_get_builtin_timezone(icalparameter_get_tzid(tz));
            ical_to_tm(x, &ev->exception[ev->exceptions]);
            ev->exception[ev->exceptions].tm_hour = 0;
            ev->exception[ev->exceptions].tm_min = 0;
            ev->exceptions++;
         }
      }
   }

   /* alarm: first relative trigger */
   ev->alarm = 0;
   {
      icalcomponent *a;
      for (a = icalcomponent_get_first_component(c, ICAL_VALARM_COMPONENT); a;
           a = icalcomponent_get_next_component(c, ICAL_VALARM_COMPONENT)) {
         icalproperty *tp = icalcomponent_get_first_property(a, ICAL_TRIGGER_PROPERTY);
         struct icaltriggertype trig;
         int secs;
         if (!tp)
            continue;
         trig = icalproperty_get_trigger(tp);
         if (!icaltime_is_null_time(trig.time))
            continue;
         secs = -icaldurationtype_as_int(trig.duration);
         if (secs < 0)
            secs = 0;
         ev->alarm = 1;
         if (secs && secs % 86400 == 0) {
            ev->advance = secs / 86400;
            ev->advanceUnits = calendar_advDays;
         } else if (secs && secs % 3600 == 0) {
            ev->advance = secs / 3600;
            ev->advanceUnits = calendar_advHours;
         } else {
            ev->advance = secs / 60;
            ev->advanceUnits = calendar_advMinutes;
         }
         if (ev->advance > 99) {   /* Palm limit */
            ev->advance = (secs + 3599) / 3600;
            ev->advanceUnits = calendar_advHours;
            if (ev->advance > 99) {
               ev->advance = (secs + 86399) / 86400;
               ev->advanceUnits = calendar_advDays;
            }
         }
         break;
      }
   }

   icalcomponent_free(cal);
   return 0;
}

/* ---------- to-dos ---------- */

char *rs_todo_to_ical(const struct ToDo *td, const char *uid,
                      const char *category, int private_rec,
                      const char *merge_with)
{
   icalcomponent *cal, *c;
   static const int prio[] = { 0, 1, 3, 5, 7, 9 };

   c = start_component(merge_with, ICAL_VTODO_COMPONENT, uid, &cal);
   set_text_prop(c, ICAL_SUMMARY_PROPERTY, td->description);
   set_text_prop(c, ICAL_DESCRIPTION_PROPERTY, td->note);
   set_category_class(c, category, private_rec);

   remove_props(c, ICAL_DUE_PROPERTY);
   if (!td->indefinite) {
      struct icaltimetype d = tm_to_ical(&td->due, TRUE);
      icalcomponent_add_property(c, icalproperty_new_due(d));
   }
   remove_props(c, ICAL_PRIORITY_PROPERTY);
   if (td->priority >= 1 && td->priority <= 5)
      icalcomponent_add_property(c, icalproperty_new_priority(prio[td->priority]));

   {
      icalproperty *st = icalcomponent_get_first_property(c, ICAL_STATUS_PROPERTY);
      gboolean was_done = st && icalproperty_get_status(st) == ICAL_STATUS_COMPLETED;
      if (td->complete && !was_done) {
         remove_props(c, ICAL_STATUS_PROPERTY);
         remove_props(c, ICAL_COMPLETED_PROPERTY);
         remove_props(c, ICAL_PERCENTCOMPLETE_PROPERTY);
         icalcomponent_add_property(c, icalproperty_new_status(ICAL_STATUS_COMPLETED));
         icalcomponent_add_property(c, icalproperty_new_percentcomplete(100));
         icalcomponent_add_property(c, icalproperty_new_completed(
            icaltime_current_time_with_zone(icaltimezone_get_utc_timezone())));
      } else if (!td->complete && (was_done || !st)) {
         remove_props(c, ICAL_STATUS_PROPERTY);
         remove_props(c, ICAL_COMPLETED_PROPERTY);
         remove_props(c, ICAL_PERCENTCOMPLETE_PROPERTY);
         icalcomponent_add_property(c, icalproperty_new_status(ICAL_STATUS_NEEDSACTION));
      }
   }
   return finish(cal);
}

int rs_ical_to_todo(const char *ical, struct ToDo *td,
                    char **category, int *private_rec)
{
   icalcomponent *cal = icalparser_parse_string(ical), *c;
   icalproperty *p;

   if (!cal)
      return -1;
   c = find_master(cal, ICAL_VTODO_COMPONENT);
   if (!c) {
      icalcomponent_free(cal);
      return -1;
   }
   replace_str(&td->description, icalcomponent_get_summary(c));
   p = icalcomponent_get_first_property(c, ICAL_DESCRIPTION_PROPERTY);
   replace_str(&td->note, p ? icalproperty_get_description(p) : NULL);
   get_category_class(c, category, private_rec);

   p = icalcomponent_get_first_property(c, ICAL_DUE_PROPERTY);
   if (p) {
      struct icaltimetype d = icalproperty_get_due(p);
      icalparameter *tz = icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER);
      if (tz && !d.zone)
         d.zone = icaltimezone_get_builtin_timezone(icalparameter_get_tzid(tz));
      td->indefinite = 0;
      ical_to_tm(d, &td->due);
      td->due.tm_hour = td->due.tm_min = 0;
   } else {
      td->indefinite = 1;
   }

   p = icalcomponent_get_first_property(c, ICAL_PRIORITY_PROPERTY);
   if (p && icalproperty_get_priority(p) > 0) {
      int v = icalproperty_get_priority(p);
      td->priority = v <= 2 ? 1 : v <= 4 ? 2 : v <= 6 ? 3 : v <= 8 ? 4 : 5;
   } else if (td->priority < 1 || td->priority > 5) {
      td->priority = 1;
   }

   p = icalcomponent_get_first_property(c, ICAL_STATUS_PROPERTY);
   td->complete = (p && icalproperty_get_status(p) == ICAL_STATUS_COMPLETED) ||
                  icalcomponent_get_first_property(c, ICAL_COMPLETED_PROPERTY) != NULL;

   icalcomponent_free(cal);
   return 0;
}

/* ---------- misc ---------- */

char *rs_ical_get_uid(const char *ical)
{
   icalcomponent *cal = icalparser_parse_string(ical), *c;
   char *uid = NULL;

   if (!cal)
      return NULL;
   c = find_master(cal, ICAL_VEVENT_COMPONENT);
   if (!c)
      c = find_master(cal, ICAL_VTODO_COMPONENT);
   if (c && icalcomponent_get_uid(c))
      uid = g_strdup(icalcomponent_get_uid(c));
   icalcomponent_free(cal);
   return uid;
}

gboolean rs_ical_has_component(const char *ical, const char *comp)
{
   icalcomponent *cal = icalparser_parse_string(ical);
   gboolean r;

   if (!cal)
      return FALSE;
   r = find_master(cal, icalcomponent_string_to_kind(comp)) != NULL;
   icalcomponent_free(cal);
   return r;
}
