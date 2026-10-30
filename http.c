/* radicalesync - settings, logging, HTTP via the curl program, WebDAV parsing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include "rs.h"

/* ---------- logging ---------- */

static RsLogFunc log_func;
static void *log_data;

void rs_set_log_func(RsLogFunc f, void *data)
{
   log_func = f;
   log_data = data;
}

void rs_log(const char *fmt, ...)
{
   va_list ap;
   char *msg;

   va_start(ap, fmt);
   msg = g_strdup_vprintf(fmt, ap);
   va_end(ap);
   if (log_func)
      log_func(msg, log_data);
   else
      fprintf(stderr, "radicalesync: %s\n", msg);
   g_free(msg);
}

/* ---------- settings ---------- */

char *rs_state_dir(void)
{
   const char *home = g_getenv("JPILOT_HOME");
   char *dir;

   if (!home)
      home = g_get_home_dir();
   dir = g_build_filename(home, ".jpilot", "radicalesync", NULL);
   g_mkdir_with_parents(dir, 0700);
   return dir;
}

static char *settings_file(void)
{
   char *dir = rs_state_dir();
   char *f = g_build_filename(dir, "radicalesync.conf", NULL);
   g_free(dir);
   return f;
}

static char *kf_str(GKeyFile *kf, const char *key, const char *def)
{
   char *v = g_key_file_get_string(kf, "radicale", key, NULL);
   return v ? v : g_strdup(def);
}

static gboolean kf_bool(GKeyFile *kf, const char *key, gboolean def)
{
   GError *err = NULL;
   gboolean v = g_key_file_get_boolean(kf, "radicale", key, &err);
   if (err) {
      g_error_free(err);
      return def;
   }
   return v;
}

void rs_settings_load(RsSettings *s)
{
   GKeyFile *kf = g_key_file_new();
   char *f = settings_file();

   g_key_file_load_from_file(kf, f, G_KEY_FILE_NONE, NULL);
   s->url = kf_str(kf, "url", "http://localhost:5232/");
   s->username = kf_str(kf, "username", "");
   s->password = kf_str(kf, "password", "");
   s->cal_path = kf_str(kf, "calendar_collection", "");
   s->todo_path = kf_str(kf, "todo_collection", "");
   s->card_path = kf_str(kf, "addressbook_collection", "");
   s->sync_cal = kf_bool(kf, "sync_calendar", TRUE);
   s->sync_todo = kf_bool(kf, "sync_todo", TRUE);
   s->sync_card = kf_bool(kf, "sync_contacts", TRUE);
   s->after_hotsync = kf_bool(kf, "sync_after_hotsync", FALSE);
   s->insecure_tls = kf_bool(kf, "allow_insecure_tls", FALSE);
   s->conflict = kf_bool(kf, "palm_wins_conflicts", FALSE) ? RS_PREFER_PALM
                                                          : RS_PREFER_SERVER;
   g_key_file_free(kf);
   g_free(f);
}

int rs_settings_save(const RsSettings *s)
{
   GKeyFile *kf = g_key_file_new();
   char *f = settings_file();
   GError *err = NULL;
   int ret = 0;

   g_key_file_set_string(kf, "radicale", "url", s->url);
   g_key_file_set_string(kf, "radicale", "username", s->username);
   g_key_file_set_string(kf, "radicale", "password", s->password);
   g_key_file_set_string(kf, "radicale", "calendar_collection", s->cal_path);
   g_key_file_set_string(kf, "radicale", "todo_collection", s->todo_path);
   g_key_file_set_string(kf, "radicale", "addressbook_collection", s->card_path);
   g_key_file_set_boolean(kf, "radicale", "sync_calendar", s->sync_cal);
   g_key_file_set_boolean(kf, "radicale", "sync_todo", s->sync_todo);
   g_key_file_set_boolean(kf, "radicale", "sync_contacts", s->sync_card);
   g_key_file_set_boolean(kf, "radicale", "sync_after_hotsync", s->after_hotsync);
   g_key_file_set_boolean(kf, "radicale", "allow_insecure_tls", s->insecure_tls);
   g_key_file_set_boolean(kf, "radicale", "palm_wins_conflicts",
                          s->conflict == RS_PREFER_PALM);
   if (!g_key_file_save_to_file(kf, f, &err)) {
      rs_log("Could not save settings: %s", err->message);
      g_error_free(err);
      ret = -1;
   } else {
      g_chmod(f, 0600);  /* holds the password */
   }
   g_key_file_free(kf);
   g_free(f);
   return ret;
}

void rs_settings_clear(RsSettings *s)
{
   g_free(s->url);
   g_free(s->username);
   g_free(s->password);
   g_free(s->cal_path);
   g_free(s->todo_path);
   g_free(s->card_path);
   memset(s, 0, sizeof(*s));
}

/* ---------- URLs ---------- */

/* "http://host:5232/a/b" -> "http://host:5232" */
static char *url_origin(const char *url)
{
   const char *p = strstr(url, "://");
   const char *slash;

   if (!p)
      return g_strdup(url);
   slash = strchr(p + 3, '/');
   return slash ? g_strndup(url, slash - url) : g_strdup(url);
}

/* Join base URL with a collection path or server-absolute href. */
char *rs_join_url(const char *base, const char *path)
{
   if (!path || !*path)
      return g_strdup(base);
   if (strstr(path, "://"))
      return g_strdup(path);
   if (path[0] == '/') {
      char *o = url_origin(base);
      char *r = g_strconcat(o, path, NULL);
      g_free(o);
      return r;
   }
   if (g_str_has_suffix(base, "/"))
      return g_strconcat(base, path, NULL);
   return g_strconcat(base, "/", path, NULL);
}

/* ---------- HTTP ---------- */

void rs_response_free(RsResponse *r)
{
   if (!r)
      return;
   g_free(r->body);
   g_free(r->etag);
   g_free(r->error);
   g_free(r);
}

/* curl config-file quoting */
static char *cfg_quote(const char *s)
{
   GString *g = g_string_new("\"");
   for (; *s; s++) {
      if (*s == '"' || *s == '\\')
         g_string_append_c(g, '\\');
      g_string_append_c(g, *s);
   }
   g_string_append_c(g, '"');
   return g_string_free(g, FALSE);
}

static void parse_headers(const char *hdr, RsResponse *r)
{
   char **lines = g_strsplit(hdr, "\n", -1);
   int i;

   /* With redirects/100-continue there may be several blocks; the last
    * status line wins and header values reset with it. */
   for (i = 0; lines[i]; i++) {
      char *l = g_strstrip(lines[i]);
      if (g_str_has_prefix(l, "HTTP/")) {
         char *sp = strchr(l, ' ');
         r->status = sp ? atoi(sp + 1) : 0;
         g_clear_pointer(&r->etag, g_free);
      } else if (g_ascii_strncasecmp(l, "etag:", 5) == 0) {
         g_free(r->etag);
         r->etag = g_strdup(g_strstrip(l + 5));
      }
   }
   g_strfreev(lines);
}

RsResponse *rs_http(const RsSettings *s, const char *method, const char *url,
                    const char *const *extra_headers, const char *body)
{
   RsResponse *r = g_new0(RsResponse, 1);
   GPtrArray *argv = g_ptr_array_new();
   char *dir = rs_state_dir();
   char *cfgpath = g_build_filename(dir, ".curlcfg-XXXXXX", NULL);
   char *hdrpath = g_build_filename(dir, ".curlhdr-XXXXXX", NULL);
   GSubprocess *proc = NULL;
   GBytes *in = NULL, *out = NULL, *errb = NULL;
   GError *err = NULL;
   int fd;
   char *hdr = NULL;

   /* Credentials go through a private config file, never the command
    * line, so they do not show up in ps. */
   fd = g_mkstemp_full(cfgpath, O_WRONLY, 0600);
   if (fd >= 0) {
      char *up = g_strdup_printf("%s:%s", s->username, s->password);
      char *q = cfg_quote(up);
      char *line = g_strdup_printf("user = %s\n", q);
      if (write(fd, line, strlen(line)) < 0)
         rs_log("Could not write curl config");
      close(fd);
      g_free(line);
      g_free(q);
      memset(up, 0, strlen(up));
      g_free(up);
   }
   fd = g_mkstemp_full(hdrpath, O_WRONLY, 0600);
   if (fd >= 0)
      close(fd);

   g_ptr_array_add(argv, "curl");
   g_ptr_array_add(argv, "-sS");
   g_ptr_array_add(argv, "--max-time");
   g_ptr_array_add(argv, "120");
   if (*s->username) {
      g_ptr_array_add(argv, "-K");
      g_ptr_array_add(argv, cfgpath);
   }
   if (s->insecure_tls)
      g_ptr_array_add(argv, "-k");
   g_ptr_array_add(argv, "-X");
   g_ptr_array_add(argv, (char *)method);
   g_ptr_array_add(argv, "-D");
   g_ptr_array_add(argv, hdrpath);
   g_ptr_array_add(argv, "-H");
   g_ptr_array_add(argv, "Expect:");
   for (; extra_headers && *extra_headers; extra_headers++) {
      g_ptr_array_add(argv, "-H");
      g_ptr_array_add(argv, (char *)*extra_headers);
   }
   if (body) {
      g_ptr_array_add(argv, "--data-binary");
      g_ptr_array_add(argv, "@-");
   }
   g_ptr_array_add(argv, "--");
   g_ptr_array_add(argv, (char *)url);
   g_ptr_array_add(argv, NULL);

   proc = g_subprocess_newv((const char *const *)argv->pdata,
                            G_SUBPROCESS_FLAGS_STDIN_PIPE |
                            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                            G_SUBPROCESS_FLAGS_STDERR_PIPE, &err);
   if (!proc) {
      r->error = g_strdup_printf("cannot run curl: %s", err->message);
      g_error_free(err);
      goto done;
   }
   in = g_bytes_new(body ? body : "", body ? strlen(body) : 0);
   if (!g_subprocess_communicate(proc, in, NULL, &out, &errb, &err)) {
      r->error = g_strdup(err->message);
      g_error_free(err);
      goto done;
   }
   if (g_file_get_contents(hdrpath, &hdr, NULL, NULL))
      parse_headers(hdr, r);
   if (out)
      r->body = g_strndup(g_bytes_get_data(out, NULL), g_bytes_get_size(out));
   if (!g_subprocess_get_successful(proc) && errb) {
      r->error = g_strstrip(g_strndup(g_bytes_get_data(errb, NULL),
                                      g_bytes_get_size(errb)));
      r->status = 0;
   }

done:
   g_unlink(cfgpath);
   g_unlink(hdrpath);
   g_free(cfgpath);
   g_free(hdrpath);
   g_free(dir);
   g_free(hdr);
   if (in) g_bytes_unref(in);
   if (out) g_bytes_unref(out);
   if (errb) g_bytes_unref(errb);
   if (proc) g_object_unref(proc);
   g_ptr_array_free(argv, TRUE);
   return r;
}

/* ---------- WebDAV multistatus parser ---------- */

void rs_dav_item_free(RsDavItem *it)
{
   if (!it)
      return;
   g_free(it->href);
   g_free(it->etag);
   g_free(it->data);
   g_free(it->displayname);
   g_free(it->kind);
   g_free(it->components);
   g_free(it);
}

typedef struct {
   GList *items;
   RsDavItem *cur;
   GString *text;
   gboolean in_resourcetype, in_compset;
} ParseCtx;

static const char *local_name(const char *name)
{
   const char *c = strrchr(name, ':');
   return c ? c + 1 : name;
}

static void p_start(GMarkupParseContext *ctx, const char *name,
                    const char **attr_names, const char **attr_values,
                    gpointer data, GError **error)
{
   ParseCtx *p = data;
   const char *n = local_name(name);

   g_string_truncate(p->text, 0);
   if (!strcmp(n, "response")) {
      p->cur = g_new0(RsDavItem, 1);
   } else if (!p->cur) {
      return;
   } else if (!strcmp(n, "resourcetype")) {
      p->in_resourcetype = TRUE;
   } else if (p->in_resourcetype) {
      if (!strcmp(n, "collection"))
         p->cur->is_collection = TRUE;
      else if (!strcmp(n, "calendar") || !strcmp(n, "addressbook")) {
         g_free(p->cur->kind);
         p->cur->kind = g_strdup(n);
      }
   } else if (!strcmp(n, "supported-calendar-component-set")) {
      p->in_compset = TRUE;
   } else if (p->in_compset && !strcmp(n, "comp")) {
      int i;
      for (i = 0; attr_names[i]; i++) {
         if (!strcmp(attr_names[i], "name")) {
            char *old = p->cur->components;
            p->cur->components = old ? g_strconcat(old, ",", attr_values[i], NULL)
                                     : g_strdup(attr_values[i]);
            g_free(old);
         }
      }
   }
}

static void p_end(GMarkupParseContext *ctx, const char *name,
                  gpointer data, GError **error)
{
   ParseCtx *p = data;
   const char *n = local_name(name);
   char *t;

   if (!p->cur)
      return;
   t = p->text->str;
   if (!strcmp(n, "response")) {
      if (p->cur->href)
         p->items = g_list_append(p->items, p->cur);
      else
         rs_dav_item_free(p->cur);
      p->cur = NULL;
   } else if (!strcmp(n, "href") && !p->cur->href) {
      p->cur->href = g_strdup(g_strstrip(t));
   } else if (!strcmp(n, "getetag") && *t) {
      g_free(p->cur->etag);
      p->cur->etag = g_strdup(g_strstrip(t));
   } else if ((!strcmp(n, "calendar-data") || !strcmp(n, "address-data")) && *t) {
      g_free(p->cur->data);
      p->cur->data = g_strdup(t);
   } else if (!strcmp(n, "displayname") && *t) {
      g_free(p->cur->displayname);
      p->cur->displayname = g_strdup(g_strstrip(t));
   } else if (!strcmp(n, "resourcetype")) {
      p->in_resourcetype = FALSE;
   } else if (!strcmp(n, "supported-calendar-component-set")) {
      p->in_compset = FALSE;
   }
   g_string_truncate(p->text, 0);
}

static void p_text(GMarkupParseContext *ctx, const char *text, gsize len,
                   gpointer data, GError **error)
{
   ParseCtx *p = data;
   g_string_append_len(p->text, text, len);
}

GList *rs_dav_parse_multistatus(const char *xml)
{
   GMarkupParser parser = { p_start, p_end, p_text, NULL, NULL };
   ParseCtx p = { 0 };
   GMarkupParseContext *ctx;
   GError *err = NULL;
   const char *start;

   if (!xml)
      return NULL;
   /* GMarkup rejects the <?xml ...?> declaration in some versions */
   start = xml;
   if (g_str_has_prefix(start, "<?xml")) {
      const char *e = strstr(start, "?>");
      if (e)
         start = e + 2;
   }
   p.text = g_string_new(NULL);
   ctx = g_markup_parse_context_new(&parser, 0, &p, NULL);
   if (!g_markup_parse_context_parse(ctx, start, -1, &err) ||
       !g_markup_parse_context_end_parse(ctx, &err)) {
      rs_log("Could not parse server response: %s", err->message);
      g_error_free(err);
   }
   g_markup_parse_context_free(ctx);
   rs_dav_item_free(p.cur);
   g_string_free(p.text, TRUE);
   return p.items;
}
