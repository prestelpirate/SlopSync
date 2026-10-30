/* radicalesync - Palm Contact <-> vCard 3.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rs.h"

/* Palm Contacts default label indices */
enum { PH_WORK, PH_HOME, PH_FAX, PH_OTHER, PH_EMAIL, PH_MAIN, PH_PAGER, PH_MOBILE };
enum { AD_WORK, AD_HOME, AD_OTHER };

typedef struct {
   char *group;   /* item1. prefix or NULL */
   char *name;    /* upper case */
   char *params;  /* raw, without leading ';' (may be NULL) */
   char *value;   /* raw, still escaped */
   char *raw;     /* original unfolded line */
} VProp;

static void vprop_free(VProp *p)
{
   g_free(p->group);
   g_free(p->name);
   g_free(p->params);
   g_free(p->value);
   g_free(p->raw);
   g_free(p);
}

/* Unfold and split a vCard into VProp lines (only the first card). */
static GList *vcard_parse(const char *text)
{
   GList *props = NULL;
   GString *un = g_string_new(NULL);
   const char *s;
   char **lines;
   int i;

   for (s = text; *s; s++) {
      if (*s == '\r')
         continue;
      if (*s == '\n' && (s[1] == ' ' || s[1] == '\t')) {
         s++;
         continue;
      }
      g_string_append_c(un, *s);
   }
   lines = g_strsplit(un->str, "\n", -1);
   g_string_free(un, TRUE);
   for (i = 0; lines[i]; i++) {
      char *l = lines[i], *colon, *semi, *dot, *head;
      VProp *p;
      gboolean inq = FALSE;

      if (!*l)
         continue;
      /* find the ':' that is not inside a quoted parameter */
      for (colon = l; *colon; colon++) {
         if (*colon == '"')
            inq = !inq;
         else if (*colon == ':' && !inq)
            break;
      }
      if (!*colon)
         continue;
      p = g_new0(VProp, 1);
      p->raw = g_strdup(l);
      head = g_strndup(l, colon - l);
      p->value = g_strdup(colon + 1);
      semi = strchr(head, ';');
      if (semi) {
         p->params = g_strdup(semi + 1);
         *semi = 0;
      }
      dot = strchr(head, '.');
      if (dot) {
         p->group = g_strndup(head, dot - head);
         p->name = g_ascii_strup(dot + 1, -1);
      } else {
         p->name = g_ascii_strup(head, -1);
      }
      g_free(head);
      props = g_list_append(props, p);
      if (!strcmp(p->name, "END") && !g_ascii_strcasecmp(p->value, "VCARD"))
         break;
   }
   g_strfreev(lines);
   return props;
}

static void vcard_free(GList *props)
{
   g_list_free_full(props, (GDestroyNotify)vprop_free);
}

static char *vunescape(const char *v)
{
   GString *g = g_string_new(NULL);
   for (; *v; v++) {
      if (*v == '\\' && v[1]) {
         v++;
         if (*v == 'n' || *v == 'N')
            g_string_append_c(g, '\n');
         else
            g_string_append_c(g, *v);
      } else {
         g_string_append_c(g, *v);
      }
   }
   return g_string_free(g, FALSE);
}

static char *vescape(const char *v)
{
   GString *g = g_string_new(NULL);
   if (!v)
      v = "";
   for (; *v; v++) {
      switch (*v) {
       case '\\': g_string_append(g, "\\\\"); break;
       case ';':  g_string_append(g, "\\;"); break;
       case ',':  g_string_append(g, "\\,"); break;
       case '\n': g_string_append(g, "\\n"); break;
       case '\r': break;
       default:   g_string_append_c(g, *v);
      }
   }
   return g_string_free(g, FALSE);
}

/* Split a structured value on unescaped ';' and unescape each part */
static char **vsplit(const char *v, int min_parts)
{
   GPtrArray *a = g_ptr_array_new();
   GString *cur = g_string_new(NULL);
   const char *s;

   for (s = v; ; s++) {
      if (*s == '\\' && s[1]) {
         g_string_append_c(cur, *s);
         g_string_append_c(cur, *++s);
      } else if (*s == ';' || *s == 0) {
         g_ptr_array_add(a, vunescape(cur->str));
         g_string_truncate(cur, 0);
         if (!*s)
            break;
      } else {
         g_string_append_c(cur, *s);
      }
   }
   while ((int)a->len < min_parts)
      g_ptr_array_add(a, g_strdup(""));
   g_ptr_array_add(a, NULL);
   g_string_free(cur, TRUE);
   return (char **)g_ptr_array_free(a, FALSE);
}

/* Does params contain TYPE value t (TYPE=a,b / TYPE=a;TYPE=b / bare a)? */
static gboolean has_type(const char *params, const char *t)
{
   char *up, **parts;
   gboolean r = FALSE;
   int i;

   if (!params)
      return FALSE;
   up = g_ascii_strup(params, -1);
   parts = g_strsplit_set(up, ";,=\"", -1);
   for (i = 0; parts[i]; i++)
      if (!strcmp(parts[i], t))
         r = TRUE;
   g_strfreev(parts);
   g_free(up);
   return r;
}

static int param_int(const char *params, const char *key, int def)
{
   char *p, *up;
   int r = def;

   if (!params)
      return def;
   up = g_ascii_strup(params, -1);
   p = strstr(up, key);
   if (p && p[strlen(key)] == '=')
      r = atoi(p + strlen(key) + 1);
   g_free(up);
   return r;
}

static const char *managed[] = {
   "VERSION", "PRODID", "REV", "N", "FN", "ORG", "TITLE", "TEL", "EMAIL", "ADR",
   "URL", "IMPP", "X-AIM", "X-ICQ", "X-JABBER", "X-MSN", "X-YAHOO", "X-SKYPE",
   "NOTE", "BDAY", "CATEGORIES", "CLASS", "BEGIN", "END", "UID", "LABEL", NULL
};

static gboolean is_managed(const char *name)
{
   int i;
   if (g_str_has_prefix(name, "X-PALM-"))
      return TRUE;
   for (i = 0; managed[i]; i++)
      if (!strcmp(managed[i], name))
         return TRUE;
   return FALSE;
}

static void fold_line(GString *out, const char *line)
{
   /* fold at 75 octets without splitting UTF-8 sequences */
   int col = 0;
   const char *s;
   for (s = line; *s; s++) {
      if (col >= 74 && ((unsigned char)*s & 0xC0) != 0x80) {
         g_string_append(out, "\r\n ");
         col = 1;
      }
      g_string_append_c(out, *s);
      col++;
   }
   g_string_append(out, "\r\n");
}

static void add_line(GString *out, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void add_line(GString *out, const char *fmt, ...)
{
   va_list ap;
   char *l;
   va_start(ap, fmt);
   l = g_strdup_vprintf(fmt, ap);
   va_end(ap);
   fold_line(out, l);
   g_free(l);
}

static const char *e(const struct Contact *c, int i)
{
   return c->entry[i] ? c->entry[i] : "";
}

char *rs_contact_to_vcard(const struct Contact *c, const char *uid,
                          const char *category, int private_rec,
                          const char *merge_with)
{
   GString *out = g_string_new(NULL);
   GList *old = merge_with ? vcard_parse(merge_with) : NULL, *l;
   char *fn, *a, *b, *x, *y, *z;
   int i;
   const char *olduid = NULL;

   for (l = old; l; l = l->next)
      if (!strcmp(((VProp *)l->data)->name, "UID"))
         olduid = ((VProp *)l->data)->value;

   add_line(out, "BEGIN:VCARD");
   add_line(out, "VERSION:3.0");
   add_line(out, "PRODID:-//J-Pilot//radicalesync 1.0//EN");
   add_line(out, "UID:%s", olduid ? olduid : uid);

   a = vescape(e(c, contLastname));
   b = vescape(e(c, contFirstname));
   add_line(out, "N:%s;%s;;;", a, b);
   g_free(a);
   g_free(b);

   if (c->entry[contFirstname] || c->entry[contLastname])
      fn = g_strstrip(g_strdup_printf("%s %s", e(c, contFirstname), e(c, contLastname)));
   else
      fn = g_strdup(e(c, contCompany));
   a = vescape(fn);
   add_line(out, "FN:%s", a);
   g_free(a);
   g_free(fn);

   if (c->entry[contCompany]) {
      a = vescape(c->entry[contCompany]);
      add_line(out, "ORG:%s", a);
      g_free(a);
   }
   if (c->entry[contTitle]) {
      a = vescape(c->entry[contTitle]);
      add_line(out, "TITLE:%s", a);
      g_free(a);
   }

   for (i = 0; i < 7; i++) {
      const char *v = c->entry[contPhone1 + i];
      const char *type;
      const char *pref = (c->showPhone == i) ? ",PREF" : "";
      if (!v || !*v)
         continue;
      a = vescape(v);
      switch (c->phoneLabel[i]) {
       case PH_EMAIL:
         add_line(out, "EMAIL;TYPE=INTERNET%s:%s", pref, a);
         g_free(a);
         continue;
       case PH_WORK:   type = "WORK,VOICE"; break;
       case PH_HOME:   type = "HOME,VOICE"; break;
       case PH_FAX:    type = "FAX"; break;
       case PH_MAIN:   type = "X-MAIN,VOICE"; break;
       case PH_PAGER:  type = "PAGER"; break;
       case PH_MOBILE: type = "CELL,VOICE"; break;
       default:        type = "VOICE"; break;
      }
      add_line(out, "TEL;TYPE=%s%s:%s", type, pref, a);
      g_free(a);
   }

   for (i = 0; i < 3; i++) {
      int base = contAddress1 + i * 5;
      const char *type = c->addressLabel[i] == AD_WORK ? "WORK" :
                         c->addressLabel[i] == AD_HOME ? "HOME" : "OTHER";
      int k, any = 0;
      for (k = 0; k < 5; k++)
         if (c->entry[base + k] && *c->entry[base + k])
            any = 1;
      if (!any)
         continue;
      a = vescape(e(c, base));
      b = vescape(e(c, base + 1));
      x = vescape(e(c, base + 2));
      y = vescape(e(c, base + 3));
      z = vescape(e(c, base + 4));
      add_line(out, "ADR;TYPE=%s:;;%s;%s;%s;%s;%s", type, a, b, x, y, z);
      g_free(a); g_free(b); g_free(x); g_free(y); g_free(z);
   }

   for (i = 0; i < 2; i++) {
      if (!c->entry[contIM1 + i] || !*c->entry[contIM1 + i])
         continue;
      a = vescape(c->entry[contIM1 + i]);
      add_line(out, "IMPP;X-PALM-LABEL=%d:%s", c->IMLabel[i], a);
      g_free(a);
   }
   if (c->entry[contWebsite] && *c->entry[contWebsite]) {
      a = vescape(c->entry[contWebsite]);
      add_line(out, "URL:%s", a);
      g_free(a);
   }
   for (i = 0; i < 9; i++) {
      if (!c->entry[contCustom1 + i] || !*c->entry[contCustom1 + i])
         continue;
      a = vescape(c->entry[contCustom1 + i]);
      add_line(out, "X-PALM-CUSTOM%d:%s", i + 1, a);
      g_free(a);
   }
   if (c->entry[contNote] && *c->entry[contNote]) {
      a = vescape(c->entry[contNote]);
      add_line(out, "NOTE:%s", a);
      g_free(a);
   }
   if (c->birthdayFlag)
      add_line(out, "BDAY:%04d-%02d-%02d", c->birthday.tm_year + 1900,
               c->birthday.tm_mon + 1, c->birthday.tm_mday);

   /* keep the Palm category first, keep other server categories */
   {
      GString *cats = g_string_new(NULL);
      if (category && *category) {
         a = vescape(category);
         g_string_append(cats, a);
         g_free(a);
      }
      for (l = old; l; l = l->next) {
         VProp *p = l->data;
         char **parts;
         int k;
         if (strcmp(p->name, "CATEGORIES"))
            continue;
         parts = g_strsplit(p->value, ",", -1);
         for (k = 0; parts[k]; k++) {
            char *u = vunescape(parts[k]);
            if (*u && (!category || g_ascii_strcasecmp(u, category))) {
               if (cats->len)
                  g_string_append_c(cats, ',');
               g_string_append(cats, parts[k]);
            }
            g_free(u);
         }
         g_strfreev(parts);
      }
      if (cats->len)
         add_line(out, "CATEGORIES:%s", cats->str);
      g_string_free(cats, TRUE);
   }
   if (private_rec)
      add_line(out, "CLASS:PRIVATE");

   /* everything the Palm does not know about (PHOTO, extra fields ...) */
   for (l = old; l; l = l->next) {
      VProp *p = l->data;
      if (!is_managed(p->name))
         fold_line(out, p->raw);
   }
   {
      GDateTime *now = g_date_time_new_now_utc();
      char *rev = g_date_time_format(now, "%Y%m%dT%H%M%SZ");
      add_line(out, "REV:%s", rev);
      g_free(rev);
      g_date_time_unref(now);
   }
   add_line(out, "END:VCARD");
   vcard_free(old);
   return g_string_free(out, FALSE);
}

static void set_entry(struct Contact *c, int i, const char *v)
{
   free(c->entry[i]);
   c->entry[i] = (v && *v) ? strdup(v) : NULL;
}

static int phone_label_for(const VProp *p)
{
   if (!strcmp(p->name, "EMAIL"))
      return PH_EMAIL;
   if (has_type(p->params, "CELL"))
      return PH_MOBILE;
   if (has_type(p->params, "FAX"))
      return PH_FAX;
   if (has_type(p->params, "PAGER"))
      return PH_PAGER;
   if (has_type(p->params, "X-MAIN") || has_type(p->params, "MAIN"))
      return PH_MAIN;
   if (has_type(p->params, "WORK"))
      return PH_WORK;
   if (has_type(p->params, "HOME"))
      return PH_HOME;
   return PH_OTHER;
}

int rs_vcard_to_contact(const char *vcard, struct Contact *c,
                        char **category, int *private_rec)
{
   GList *props = vcard_parse(vcard), *l;
   int phones = 0, addrs = 0, ims = 0, i;
   char *fn = NULL;
   gboolean have_n = FALSE;
   static const int default_phone_labels[7] = {
      PH_WORK, PH_HOME, PH_FAX, PH_OTHER, PH_EMAIL, PH_MAIN, PH_MOBILE
   };

   if (!props)
      return -1;
   for (i = 0; i < 39; i++)
      set_entry(c, i, NULL);
   for (i = 0; i < 7; i++)
      c->phoneLabel[i] = default_phone_labels[i];
   for (i = 0; i < 3; i++)
      c->addressLabel[i] = i;
   c->IMLabel[0] = c->IMLabel[1] = 0;
   c->showPhone = 0;
   c->birthdayFlag = 0;
   if (category)
      *category = NULL;
   if (private_rec)
      *private_rec = 0;

   for (l = props; l; l = l->next) {
      VProp *p = l->data;
      char *v;

      if (!strcmp(p->name, "N")) {
         char **parts = vsplit(p->value, 2);
         set_entry(c, contLastname, parts[0]);
         set_entry(c, contFirstname, parts[1]);
         g_strfreev(parts);
         have_n = TRUE;
      } else if (!strcmp(p->name, "FN")) {
         g_free(fn);
         fn = vunescape(p->value);
      } else if (!strcmp(p->name, "ORG")) {
         char **parts = vsplit(p->value, 1);
         set_entry(c, contCompany, parts[0]);
         g_strfreev(parts);
      } else if (!strcmp(p->name, "TITLE")) {
         v = vunescape(p->value);
         set_entry(c, contTitle, v);
         g_free(v);
      } else if (!strcmp(p->name, "TEL") || !strcmp(p->name, "EMAIL")) {
         if (phones >= 7)
            continue;
         v = vunescape(p->value);
         if (g_str_has_prefix(v, "tel:"))
            memmove(v, v + 4, strlen(v + 4) + 1);
         set_entry(c, contPhone1 + phones, v);
         c->phoneLabel[phones] = phone_label_for(p);
         if (has_type(p->params, "PREF") || param_int(p->params, "PREF", 0) == 1)
            c->showPhone = phones;
         phones++;
         g_free(v);
      } else if (!strcmp(p->name, "ADR")) {
         char **parts;
         if (addrs >= 3)
            continue;
         parts = vsplit(p->value, 7);
         /* PO box and extended address go in front of the street */
         {
            GString *street = g_string_new(NULL);
            for (i = 0; i < 3; i++) {
               if (*parts[i]) {
                  if (street->len)
                     g_string_append_c(street, '\n');
                  g_string_append(street, parts[i]);
               }
            }
            set_entry(c, contAddress1 + addrs * 5, street->str);
            g_string_free(street, TRUE);
         }
         for (i = 0; i < 4; i++)
            set_entry(c, contAddress1 + addrs * 5 + 1 + i, parts[3 + i]);
         c->addressLabel[addrs] = has_type(p->params, "WORK") ? AD_WORK :
                                  has_type(p->params, "HOME") ? AD_HOME : AD_OTHER;
         addrs++;
         g_strfreev(parts);
      } else if (!strcmp(p->name, "IMPP") || !strcmp(p->name, "X-AIM") ||
                 !strcmp(p->name, "X-ICQ") || !strcmp(p->name, "X-JABBER") ||
                 !strcmp(p->name, "X-MSN") || !strcmp(p->name, "X-YAHOO") ||
                 !strcmp(p->name, "X-SKYPE")) {
         if (ims >= 2)
            continue;
         v = vunescape(p->value);
         set_entry(c, contIM1 + ims, v);
         c->IMLabel[ims] = param_int(p->params, "X-PALM-LABEL", 0);
         ims++;
         g_free(v);
      } else if (!strcmp(p->name, "URL")) {
         v = vunescape(p->value);
         set_entry(c, contWebsite, v);
         g_free(v);
      } else if (g_str_has_prefix(p->name, "X-PALM-CUSTOM")) {
         int n = atoi(p->name + 13);
         if (n >= 1 && n <= 9) {
            v = vunescape(p->value);
            set_entry(c, contCustom1 + n - 1, v);
            g_free(v);
         }
      } else if (!strcmp(p->name, "NOTE")) {
         v = vunescape(p->value);
         set_entry(c, contNote, v);
         g_free(v);
      } else if (!strcmp(p->name, "BDAY")) {
         int y = 0, m = 0, d = 0;
         if (sscanf(p->value, "%4d-%2d-%2d", &y, &m, &d) == 3 ||
             sscanf(p->value, "%4d%2d%2d", &y, &m, &d) == 3) {
            memset(&c->birthday, 0, sizeof(c->birthday));
            c->birthday.tm_year = y - 1900;
            c->birthday.tm_mon = m - 1;
            c->birthday.tm_mday = d;
            c->birthday.tm_isdst = -1;
            mktime(&c->birthday);
            c->birthdayFlag = 1;
         }
      } else if (!strcmp(p->name, "CATEGORIES") && category && !*category) {
         char **parts = g_strsplit(p->value, ",", 2);
         *category = vunescape(parts[0]);
         g_strfreev(parts);
      } else if (!strcmp(p->name, "CLASS") && private_rec) {
         *private_rec = g_ascii_strcasecmp(p->value, "PUBLIC") != 0;
      }
   }
   /* cards without N (e.g. organisations): use FN as last name */
   if (!have_n || (!c->entry[contLastname] && !c->entry[contFirstname])) {
      if (fn && *fn && (!c->entry[contCompany] || strcmp(fn, c->entry[contCompany])))
         set_entry(c, contLastname, fn);
   }
   g_free(fn);
   vcard_free(props);
   return 0;
}

char *rs_vcard_get_uid(const char *vcard)
{
   GList *props = vcard_parse(vcard), *l;
   char *uid = NULL;

   for (l = props; l; l = l->next)
      if (!strcmp(((VProp *)l->data)->name, "UID")) {
         uid = vunescape(((VProp *)l->data)->value);
         break;
      }
   vcard_free(props);
   return uid;
}
