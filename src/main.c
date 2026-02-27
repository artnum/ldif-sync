#include "../tomlc17/src/tomlc17.h"
#include "include/ldif.h"
#include <assert.h>
#include <errno.h>
#include <ldap.h>
#include <notcurses/nckeys.h>
#include <notcurses/notcurses.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <unistd.h>

struct sync_state {
  struct {
    char **content;
    size_t length;
    size_t capacity;
  } filter;
};

LDAP *connect_ldap_simple(const char *url, const char *binddn,
                          const char *password) {
  LDAP *ld = NULL;
  if (ldap_initialize(&ld, url) != LDAP_SUCCESS) {
    fprintf(stderr, "ldap INIT\n");
    return NULL;
  }

  int v = 3;
  if (ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &v) != LDAP_SUCCESS) {
    fprintf(stderr, "ldap SETOP\n");
    ldap_unbind_ext(ld, NULL, NULL);
    return NULL;
  }

  const char *pw = password;
  struct berval credentials = {.bv_len = strlen(pw), .bv_val = (char *)pw};
  if (ldap_sasl_bind_s(ld, binddn, LDAP_SASL_SIMPLE, &credentials, NULL, NULL,
                       NULL) != LDAP_SUCCESS) {
    fprintf(stderr, "ldap BIND\n");
    ldap_unbind_ext(ld, NULL, NULL);
    return NULL;
  }

  return ld;
}

void render_ldif_entry(struct ncplane *plan, int cursor, ldif_entry_t *entry,
                       struct sync_state *s) {
  if (!entry) {
    return;
  }
  ncplane_erase(plan);
  int y = 0;
  for (size_t i = 0; i < entry->length; i++) {
    bool found = false;
    for (size_t j = 0; j < s->filter.length; j++) {
      if (strncmp(s->filter.content[j], entry->attributes[i].name,
                  entry->attributes[i].len_name) == 0) {
        found = true;
        break;
      }
    }
    if (i == cursor) {
      if (found) {
        ncplane_set_bg_rgb8(plan, 128, 128, 128);
        ncplane_set_fg_rgb8(plan, 0, 0, 0);
      } else {
        ncplane_set_bg_rgb8(plan, 180, 180, 180);
        ncplane_set_fg_rgb8(plan, 0, 0, 0);
      }
    } else {
      if (found) {
        ncplane_set_bg_rgb8(plan, 0, 0, 0);
        ncplane_set_fg_rgb8(plan, 128, 128, 128);
      } else {
        ncplane_set_bg_rgb8(plan, 0, 0, 0);
        ncplane_set_fg_rgb8(plan, 255, 255, 255);
      }
    }
    ncplane_printf_yx(plan, y, 0, "%.*s", (int)entry->attributes[i].len_name,
                      entry->attributes[i].name);
    if (entry->attributes[i].binary) {
      ncplane_printf_yx(plan, y, 40, "[binary] %ld",
                        entry->attributes[i].len_value);
    } else {
      ncplane_printf_yx(plan, y, 40, "%.*s",
                        (int)entry->attributes[i].len_value,
                        entry->attributes[i].value);
    }
    y++;
  }
}

bool sync_ldap_server(LDAP *ld, ldif_t *file, struct ncplane *log_plane) {
  int y = 0;
  for (ldif_entry_chunk_t *c = file->first; c; c = c->next) {
    for (size_t i = 0; i < c->length; i++) {
      ldif_entry_t *e = &c->entries[i];
      if (!e) {
        continue;
      }
      ldif_iter_t iter = {0};
      const char *dn = ldif_first_attr(e, "dn", &iter);
      if (dn) {
        LDAPMessage *msg = NULL;
        char *attrs[] = {"*", NULL};
        int err = LDAP_SUCCESS;
        if ((err = ldap_search_ext_s(ld, dn, LDAP_SCOPE_BASE, "(objectclass=*)",
                                     attrs, 0, NULL, NULL, NULL, 0, &msg)) !=
            LDAP_SUCCESS) {
          ncplane_printf_yx(log_plane, y++, 0, "LDAP ERROR [%s] %s\n", dn,
                            ldap_err2string(err));
          ldap_msgfree(msg);
          continue;
        }
        ldap_msgfree(msg);
      }
    }
  }
  return true;
}

bool setup_ui(struct notcurses *nc, struct ncplane **main_plane,
              struct ncplane **log_plane) {
  assert(nc != NULL);
  assert(main_plane != NULL);
  assert(log_plane != NULL);
  const int width = ncplane_dim_x(notcurses_stdplane(nc));
  const int height = ncplane_dim_y(notcurses_stdplane(nc));

  const int main_h = (height - 1) * 0.75;
  const int log_h = height - main_h - 1;

  const ncplane_options main_plan_options = {
      .cols = width * 0.75, .rows = main_h, .x = (width * 0.25) / 2, .y = 1};
  *main_plane = ncplane_create(notcurses_stdplane(nc), &main_plan_options);
  if (!*main_plane) {
    return false;
  }
  const ncplane_options log_plan_options = {
      .cols = width, .rows = log_h, .x = 0, .y = main_h + 1};
  *log_plane = ncplane_create(notcurses_stdplane(nc), &log_plan_options);
  if (!*log_plane) {
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc < 3 || !argv[1] || !argv[2]) {
    fprintf(stderr, "Usage : %s conf.toml file.ldif\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  /* configuration */
  toml_result_t conf = toml_parse_file_ex(argv[1]);
  if (!conf.ok) {
    fprintf(stderr, "Configuration parse failed (%s)\n", argv[1]);
    exit(EXIT_FAILURE);
  }

  toml_datum_t ldap_url = toml_seek(conf.toptab, "ldap.url");
  toml_datum_t ldap_binddn = toml_seek(conf.toptab, "ldap.binddn");
  toml_datum_t ldap_password = toml_seek(conf.toptab, "ldap.password");

  if (ldap_url.type != TOML_STRING || ldap_binddn.type != TOML_STRING ||
      ldap_password.type != TOML_STRING) {
    fprintf(stderr, "Configuration error, check file %s\n", argv[1]);
    toml_free(conf);
    exit(EXIT_FAILURE);
  }

  /* connect ldap */
  LDAP *ld =
      connect_ldap_simple(ldap_url.u.s, ldap_binddn.u.s, ldap_password.u.s);
  if (ld == NULL) {
    fprintf(stderr, "Cannot connect to server %s", ldap_url.u.s);
    toml_free(conf);
    exit(EXIT_FAILURE);
  }
  toml_free(conf); /* not need anymore */

  /* init ldif file
   * TODO : make an ldif_init function
   */
  ldif_t file = {0};
  file.state.fd = open(argv[2], 0);
  file.state.max_chunk_count = getpagesize() / sizeof(ldif_entry_t);
  if (file.state.fd < 0) {
    fprintf(stderr, "Opening the file failed : %s", strerror(errno));
    ldap_unbind_ext_s(ld, NULL, NULL);
    exit(EXIT_FAILURE);
  }
  if (!ldif_parse_file(&file)) {
    fprintf(stderr, "Parsing failed\n");
    ldap_unbind_ext_s(ld, NULL, NULL);
    ldif_destroy(&file);
    exit(EXIT_FAILURE);
  }

  /* UI starting */
  struct notcurses *nc = notcurses_init(NULL, NULL);
  if (!nc) {
    fprintf(stderr, "Failed init notcurses\n");
    ldap_unbind_ext_s(ld, NULL, NULL);
    ldif_destroy(&file);
    exit(EXIT_FAILURE);
  }

  struct ncplane *main_plane = NULL;
  struct ncplane *log_plane = NULL;
  if (!setup_ui(nc, &main_plane, &log_plane)) {
    fprintf(stderr, "Cannot create plane");
    ldap_unbind_ext_s(ld, NULL, NULL);
    ldif_destroy(&file);
    notcurses_stop(nc);
    exit(EXIT_FAILURE);
  }

  notcurses_render(nc);
  int stop = 0;
  ldif_entry_chunk_t *chunk = file.first;
  size_t position = 0;
  ldif_entry_t *current = NULL;
  int cursor = 0;
  struct sync_state s = {0};

  goto render_first_frame;
  while (stop != 1) {
    /*  Event loop */
    struct ncinput event = {0};
    uint32_t id = 0;
    id = notcurses_get_blocking(nc, &event);
    if (id == (uint32_t)-1) {
      break;
    }

    switch (event.id) {
    case NCKEY_ESC: {
      stop = 1;
    } break;
    case NCKEY_RIGHT: {
      if (position + 1 >= chunk->length) {
        chunk = chunk->next;
        position = 0;
      } else {
        position++;
      }
    } break;
    case NCKEY_LEFT: {
      if (position - 1 >= 0) {
        position--;
      } else {
        ldif_entry_chunk_t *p = NULL;
        for (ldif_entry_chunk_t *c = file.first; c; c = c->next) {
          if (c == chunk) {
            break;
          }
          p = c;
        }
        if (p) {
          chunk = p;
        } else {
          p = file.last;
        }
        position = p->length - 1;
      }
    }; break;

    case NCKEY_F05: {
      sync_ldap_server(ld, &file, log_plane);
    } break;

    case NCKEY_UP: {
      if (!current) {
        break;
      }
      if (cursor - 1 >= 0) {
        cursor--;
      } else {
        cursor = current->length - 1;
      }
    } break;

    case NCKEY_DOWN: {
      if (!current) {
        break;
      }
      if (cursor + 1 < current->length) {
        cursor++;
      } else {
        cursor = 0;
      }
    } break;
    case NCKEY_SPACE: {
      if (!current) {
        break;
      }
      if (cursor >= current->length) {
        break;
      }

      char *name =
          calloc(current->attributes[cursor].len_name + 1, sizeof(char));
      if (!name) {
        break;
      }
      memcpy(name, current->attributes[cursor].name,
             current->attributes[cursor].len_name);
      bool found = false;
      for (size_t i = 0; i < s.filter.length; i++) {
        if (strcmp(name, s.filter.content[i]) == 0) {
          if (i == s.filter.length - 1) {
            free(s.filter.content[i]);
            s.filter.content[i] = NULL;
          } else {
            free(s.filter.content[i]);
            s.filter.content[i] = s.filter.content[s.filter.length - 1];
            s.filter.content[s.filter.length - 1] = NULL;
          }
          s.filter.length--;
          found = true;
          break;
        }
      }
      if (!found) {
        if (s.filter.length + 1 >= s.filter.capacity) {
          void *tmp = realloc(s.filter.content,
                              (s.filter.capacity + 1) * sizeof(char *));
          if (!tmp) {
            break;
          }
          s.filter.content = tmp;
          s.filter.content[s.filter.capacity] = NULL;
          s.filter.capacity++;
        }
        s.filter.content[s.filter.length++] = name;
      } else {
        free(name);
      }
    } break;

    } /* switch */
    if (stop) {
      break;
    }
  render_first_frame:

    if (chunk && position < chunk->length) {
      current = &chunk->entries[position];
    }

    /* Render */
    render_ldif_entry(main_plane, cursor, current, &s);
    notcurses_render(nc);
  }

  for (size_t i = 0; i < s.filter.length; i++) {
    if (s.filter.content[i]) {
      free(s.filter.content[i]);
    }
  }
  free(s.filter.content);

  ldap_unbind_ext_s(ld, NULL, NULL);
  ldif_destroy(&file);
  notcurses_stop(nc);
}
