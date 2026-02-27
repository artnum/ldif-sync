#include "include/ldif.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define LDIF_ENTRY_GROW 20
#define LDIF_SUBTYPE_GROW 4
#define LDIF_ATTRIBUTE_GROW 20
#define LDIF_BUFFER_GROW 128

static inline void _swap_subtype(ldif_kv_t *kv, size_t s1, size_t s2) {
  size_t l = kv->subtype.len_subtype[s1];
  void *v = kv->subtype.content[s1];
  kv->subtype.len_subtype[s1] = kv->subtype.len_subtype[s2];
  kv->subtype.content[s1] = kv->subtype.content[s2];
  kv->subtype.len_subtype[s2] = l;
  kv->subtype.content[s2] = v;
}

static inline int _cmp_subtype(ldif_kv_t *kv, size_t s1, size_t s2) {
  if (kv->subtype.len_subtype[s1] == kv->subtype.len_subtype[s2]) {
    return strncasecmp(kv->subtype.content[s1], kv->subtype.content[s2],
                       kv->subtype.len_subtype[s1]) > 0;
  }
  if (kv->subtype.len_subtype[s1] < kv->subtype.len_subtype[s2]) {
    return 0;
  }
  return 1;
}

static inline void _heapify_subtype(ldif_kv_t *kv, size_t rootnode,
                                    size_t length) {
  size_t largest;
  size_t left;
  size_t right;

__heapify:
  largest = rootnode;
  left = 2 * rootnode + 1;
  right = 2 * rootnode + 2;

  if (left < length && _cmp_subtype(kv, left, largest))
    largest = left;
  if (right < length && _cmp_subtype(kv, right, largest))
    largest = right;

  if (largest != rootnode) {
    _swap_subtype(kv, rootnode, largest);
    rootnode = largest;
    goto __heapify;
  }
}

void sort_subtypes(ldif_kv_t *kv) {
  for (size_t i = kv->subtype.length / 2 - 1; i + 1 != 0; i--) {
    _heapify_subtype(kv, i, kv->subtype.length);
  }
  for (size_t i = kv->subtype.length - 1; i > 0; i--) {
    _swap_subtype(kv, 0, i);
    _heapify_subtype(kv, 0, i);
  }
}

static inline void _heapify_attributes(ldif_entry_t *entry, size_t rootnode,
                                       size_t length) {
  size_t largest;
  size_t left;
  size_t right;

__heapify:
  largest = rootnode;
  left = 2 * rootnode + 1;
  right = 2 * rootnode + 2;

  if (left < length &&
      entry->attributes[left].hash > entry->attributes[largest].hash)
    largest = left;
  if (right < length &&
      entry->attributes[right].hash > entry->attributes[largest].hash)
    largest = right;

  if (largest != rootnode) {
    ldif_kv_t e = {0};
    memcpy(&e, &entry->attributes[rootnode], sizeof(ldif_kv_t));
    mempcpy(&entry->attributes[rootnode], &entry->attributes[largest],
            sizeof(ldif_kv_t));
    mempcpy(&entry->attributes[largest], &e, sizeof(ldif_kv_t));
    rootnode = largest;
    goto __heapify;
  }
}

void sort_attributes(ldif_entry_t *entry) {
  for (size_t i = entry->length / 2 - 1; i + 1 != 0; i--) {
    _heapify_attributes(entry, i, entry->length);
  }
  for (size_t i = entry->length - 1; i > 0; i--) {
    ldif_kv_t e = {0};
    memcpy(&e, &entry->attributes[0], sizeof(ldif_kv_t));
    mempcpy(&entry->attributes[0], &entry->attributes[i], sizeof(ldif_kv_t));
    mempcpy(&entry->attributes[i], &e, sizeof(ldif_kv_t));
    _heapify_attributes(entry, 0, i);
  }
}

static inline bool push_char(ldif_t *ldif, char c) {
  assert(ldif != NULL);
  if (ldif->state.buffer.length + 1 >= ldif->state.buffer.capacity) {
    void *tmp = realloc(ldif->state.buffer.content,
                        sizeof(char) *
                            (ldif->state.buffer.capacity + LDIF_BUFFER_GROW));
    if (!tmp) {
      return false;
    }
    ldif->state.buffer.content = tmp;
    ldif->state.buffer.capacity += LDIF_BUFFER_GROW;
  }
  ldif->state.buffer.content[ldif->state.buffer.length++] = c;
  return true;
}

#define lower_case_string(str, len)                                            \
  do {                                                                         \
    for (size_t i = 0; i < len; i++) {                                         \
      (str)[i] = tolower((unsigned char)(str)[i]);                             \
    }                                                                          \
  } while (0)

/**
 * Noramlize a attribute by setting subtype and name to lower case
 */
#define normalize_current_attribute(ldif)                                      \
  do {                                                                         \
    if ((ldif)->state.current_attribute) {                                     \
      if ((ldif)->state.current_attribute->name &&                             \
          (ldif)->state.current_attribute->len_name > 0) {                     \
        lower_case_string((ldif)->state.current_attribute->name,               \
                          (ldif)->state.current_attribute->len_name);          \
      }                                                                        \
      if ((ldif)->state.current_attribute->subtype.length > 1) {               \
        sort_subtypes(ldif->state.current_attribute);                          \
      }                                                                        \
    }                                                                          \
  } while (0)

#define hash_current_attribute(ldif)                                           \
  do {                                                                         \
    if ((ldif)->state.current_attribute) {                                     \
      XXH3_64bits_reset((ldif)->state.current_hash_state);                     \
      XXH3_64bits_update((ldif)->state.current_hash_state,                     \
                         (ldif)->state.current_attribute->name,                \
                         (ldif)->state.current_attribute->len_name);           \
      if ((ldif)->state.current_attribute->subtype.length > 0) {               \
        for (size_t i = 0;                                                     \
             i < (ldif)->state.current_attribute->subtype.length; i++) {       \
          XXH3_64bits_update(                                                  \
              (ldif)->state.current_hash_state,                                \
              (ldif)->state.current_attribute->subtype.content[i],             \
              (ldif)->state.current_attribute->subtype.len_subtype[i]);        \
        }                                                                      \
      }                                                                        \
      XXH3_64bits_update((ldif)->state.current_hash_state,                     \
                         (ldif)->state.current_attribute->value,               \
                         (ldif)->state.current_attribute->len_value);          \
      (ldif)->state.current_attribute->hash =                                  \
          XXH3_64bits_digest((ldif)->state.current_hash_state);                \
    }                                                                          \
  } while (0)

#define hash_current_entry(ldif)                                               \
  do {                                                                         \
    if ((ldif)->state.current_entry) {                                         \
      sort_attributes((ldif)->state.current_entry);                            \
      XXH3_64bits_reset((ldif)->state.current_hash_state);                     \
      for (size_t i = 0; i < (ldif)->state.current_entry->length; i++) {       \
        XXH3_64bits_update(                                                    \
            (ldif)->state.current_hash_state,                                  \
            (void *)&(ldif)->state.current_entry->attributes[i].hash,          \
            sizeof(uint64_t));                                                 \
      }                                                                        \
                                                                               \
      (ldif)->state.current_entry->hash =                                      \
          XXH3_64bits_digest((ldif)->state.current_hash_state);                \
    }                                                                          \
  } while (0)

static bool add_subtype(ldif_kv_t *attribute, const char *value,
                        size_t length) {
  assert(attribute != NULL);

  if (value == NULL) {
    return false;
  }
  if (length == 0) {
    return false;
  }

  /* binary attribute may be used but is ignored */
  if (strncasecmp(value, "binary", length) == 0) {
    return true;
  }
  if (attribute->subtype.length + 1 >= attribute->subtype.capacity) {
    void *tmp = realloc(attribute->subtype.content,
                        (LDIF_SUBTYPE_GROW + attribute->subtype.capacity) *
                            sizeof(char *));
    if (!tmp) {
      return false;
    }
    attribute->subtype.content = tmp;

    tmp = realloc(attribute->subtype.len_subtype,
                  (LDIF_SUBTYPE_GROW + attribute->subtype.capacity) *
                      sizeof(size_t));
    if (!tmp) {
      return false;
    }
    attribute->subtype.len_subtype = tmp;
    memset(attribute->subtype.content + attribute->subtype.capacity, 0,
           sizeof(char *) * LDIF_SUBTYPE_GROW);
    memset(attribute->subtype.len_subtype + attribute->subtype.capacity, 0,
           sizeof(size_t) * LDIF_SUBTYPE_GROW);
    attribute->subtype.capacity += LDIF_SUBTYPE_GROW;
  }

  size_t i = attribute->subtype.length;
  char *str = calloc(length + 1, sizeof(char));
  if (!str) {
    return false;
  }
  memcpy(str, value, length);

  lower_case_string(str, length);
  attribute->subtype.len_subtype[i] = length;
  attribute->subtype.content[i] = str;

  attribute->subtype.length++;

  return true;
}

static inline ldif_kv_t *add_attribute(ldif_t *ldif) {
  assert(ldif != NULL);
  ldif_entry_t *entry = ldif->state.current_entry;

  normalize_current_attribute(ldif);
  hash_current_attribute(ldif);

  if (!entry) {
    return NULL;
  }
  if (entry->length + 1 >= entry->capacity) {
    void *tmp =
        realloc(entry->attributes,
                sizeof(ldif_kv_t) * (entry->capacity + LDIF_ATTRIBUTE_GROW));
    if (!tmp) {
      return NULL;
    }
    memset((uint8_t *)tmp + (entry->capacity * sizeof(ldif_kv_t)), 0,
           LDIF_ATTRIBUTE_GROW * sizeof(ldif_kv_t));
    entry->attributes = tmp;
    entry->capacity += LDIF_ATTRIBUTE_GROW;
  }

  ldif->state.current_attribute = &entry->attributes[entry->length++];
  return ldif->state.current_attribute;
}

static inline ldif_entry_t *add_entry(ldif_t *ldif) {
  assert(ldif != NULL);

  normalize_current_attribute(ldif);
  hash_current_attribute(ldif);
  hash_current_entry(ldif);

  if (!ldif->last || ldif->last->length >= ldif->state.max_chunk_count) {
    ldif_entry_chunk_t *new_chunk = calloc(1, sizeof(ldif_entry_chunk_t));
    if (!new_chunk) {
      return NULL;
    }
    if (!ldif->first) {
      ldif->first = new_chunk;
    }
    if (ldif->last) {
      ldif->last->next = new_chunk;
    }
    ldif->last = new_chunk;
  }

  if (ldif->last->length + 1 >= ldif->last->capacity) {
    void *tmp = realloc(ldif->last->entries,
                        sizeof(ldif_entry_t) *
                            (ldif->last->capacity + LDIF_ENTRY_GROW));
    if (!tmp) {
      return NULL;
    }
    memset((uint8_t *)tmp + (ldif->last->capacity * sizeof(ldif_entry_t)), 0,
           LDIF_ENTRY_GROW * sizeof(ldif_entry_t));
    ldif->last->entries = tmp;
    ldif->last->capacity += LDIF_ENTRY_GROW;
  }

  ldif->state.current_entry = &ldif->last->entries[ldif->last->length++];
  ldif->state.current_attribute = NULL;
  return ldif->state.current_entry;
}

#define BUFFER_SIZE 4096
#define CR '\r'
#define LF '\n'
#define SP ' '
#define SEP ':'
#define SUBSEP ';'

#define buffer_clean(ldif)                                                     \
  do {                                                                         \
    if (ldif) {                                                                \
      ldif->state.buffer.length = 0;                                           \
    }                                                                          \
  } while (0)

#define reset_state(ldif)                                                      \
  do {                                                                         \
    ldif->state.eol = 0;                                                       \
    ldif->state.sol = 0;                                                       \
    ldif->state.lc = 0;                                                        \
    ldif->state.sep = 0;                                                       \
    ldif->state.val = 0;                                                       \
    ldif->state.bin = 0;                                                       \
  } while (0)

static inline char *buffer_dup(ldif_t *ldif, size_t *len) {
  char *p = ldif->state.buffer.content;
  size_t l = ldif->state.buffer.length;
  if (l == 0) {
    return NULL;
  }
  while (l + 1 > 0 && p[l - 1] == SP) {
    l--;
  }
  if (l == 0) {
    return NULL;
  }
  while (*p == SP) {
    p++;
    l--;
  }
  if (*p == '\0') {
    return NULL;
  }
  char *tmp = calloc(l + 1, sizeof(char));
  if (tmp) {
    memcpy(tmp, p, l);
  }
  if (len) {
    *len = l;
  }
  return tmp;
}

static inline char *buffer_cat_value(ldif_t *ldif, char *dest, size_t *len) {
  char *p = ldif->state.buffer.content;
  size_t l = ldif->state.buffer.length;
  if (l == 0) {
    return NULL;
  }
  if (!dest) {
    return NULL;
  }
  if (*p == '\0') {
    return false;
  }
  size_t value_len = strlen(dest);
  if (value_len == 0) {
    return NULL;
  }
  char *tmp = realloc(dest, sizeof(char) * (value_len + l + 1));
  if (!tmp) {
    return NULL;
  }
  memcpy(tmp + value_len, p, l);
  *(tmp + l + value_len) = '\0';
  if (len) {
    *len = l + value_len;
  }
  return tmp;
}

void ldif_parse_file(ldif_t *ldif) {
  char buffer[BUFFER_SIZE];
  size_t line_count = 1;
  ldif->state.line_start = 1;
  ldif->state.current_hash_state = XXH3_createState();
  if (!ldif->state.current_hash_state) {
    fprintf(stderr, "Hash init failed\n");
  }
  while (1) {
    ssize_t rlen = read(ldif->state.fd, buffer, BUFFER_SIZE);
    if (rlen <= 0) {
      break;
    }
    for (ssize_t i = 0; i < rlen; i++) {
      switch (buffer[i]) {
      case '#': {
        if (ldif->state.line_start) {
          ldif->state.comment = 1;
        }
      } break;
      case LF: {
        line_count++;
        if (ldif->state.comment) {
          ldif->state.comment = 0;
          ldif->state.line_start = 1;
          break;
        }
        if (ldif->state.line_start) {
          if (!add_entry(ldif)) {
            /* TODO : handle error */
            fprintf(stderr, "Memory allocation of entry %ld\n", line_count);
            return;
          }
          break;
        }
        if (ldif->state.attr_value) {
          if (!ldif->state.current_entry) {
            if (!add_entry(ldif)) {
              /* TODO : handle error */
              fprintf(stderr, "Memory allocation of entry %ld\n", line_count);
              return;
            }
          }
          if (ldif->state.continuation) {
            char *tmp =
                buffer_cat_value(ldif, ldif->state.current_attribute->value,
                                 &ldif->state.current_attribute->len_value);
            if (tmp) {
              ldif->state.current_attribute->value = tmp;
            }
            buffer_clean(ldif);
          } else {
            ldif->state.current_attribute->value =
                buffer_dup(ldif, &ldif->state.current_attribute->len_value);
            buffer_clean(ldif);
          }

          ldif->state.continuation = 0;
          ldif->state.attr_value = 0;
          ldif->state.line_start = 1;
        }
      } break;

      case SP: {
        if (!ldif->state.line_start) {
          push_char(ldif, buffer[i]);
        } else {
          ldif->state.continuation = 1;
          ldif->state.attr_value = 1;
          ldif->state.attr_name = 0;
          ldif->state.line_start = 0;
        }
      } break;

      case SUBSEP: {
        if (ldif->state.attr_subtype) {
          add_subtype(ldif->state.current_attribute, ldif->state.buffer.content,
                      ldif->state.buffer.length);
          buffer_clean(ldif);
          break;
        }
        if (ldif->state.attr_name) {
          ldif->state.attr_subtype = 1;
          ldif->state.current_attribute->name =
              buffer_dup(ldif, &ldif->state.current_attribute->len_name);
          buffer_clean(ldif);
        } else {

          push_char(ldif, buffer[i]);
        }
      } break;

      case SEP: {
        if (ldif->state.line_start) {
          /* TODO : handle error */
          fprintf(stderr, "Separator at line start %ld\n", line_count);
          return;
        }
        if (ldif->state.prev_was_sep) {
          ldif->state.current_attribute->binary = true;
          break;
        }
        if (ldif->state.attr_value) {
          push_char(ldif, buffer[i]);
          break;
        }
        if (ldif->state.attr_name) {
          ldif->state.prev_was_sep = 1;
          ldif->state.attr_value = 1;
          ldif->state.attr_name = 0;
          if (!ldif->state.current_entry) {
            if (!add_entry(ldif)) {
              /* TODO : handle error */
              fprintf(stderr, "Memory allocation of entry %ld\n", line_count);
              return;
            }
          }
          if (!ldif->state.current_attribute) {
            if (!add_attribute(ldif)) {
              /* TODO : handle error */
              fprintf(stderr, "Memory allocation of attribute\n");
              return;
            }
          }
          if (ldif->state.attr_subtype) {
            add_subtype(ldif->state.current_attribute,
                        ldif->state.buffer.content, ldif->state.buffer.length);
          } else {
            ldif->state.current_attribute->name =
                buffer_dup(ldif, &ldif->state.current_attribute->len_name);
          }
          ldif->state.attr_subtype = 0;
          buffer_clean(ldif);
        }
      } break;

      default: {
        if (ldif->state.prev_was_sep == 1) {
          ldif->state.prev_was_sep = 0;
        }
        if (ldif->state.line_start) {
          ldif->state.line_start = 0;
          ldif->state.attr_name = 1;
          if (!ldif->state.current_entry) {
            if (!add_entry(ldif)) {
              /* TODO : handle error */
              fprintf(stderr, "Memory allocation of entry %ld\n", line_count);
              return;
            }
          }

          if (!add_attribute(ldif)) {
            /* TODO : handle error */
            fprintf(stderr, "Memory allocation of attribute %ld\n", line_count);
            return;
          }
        }
        push_char(ldif, buffer[i]);
      } break;

      } /* switch */
    } /* while */
  }
}

static inline void ldif_free_entry(ldif_entry_t *entry) {
  for (size_t i = 0; i < entry->length; i++) {
    ldif_kv_t *attr = &entry->attributes[i];
    if (attr) {
      if (attr->name) {
        free(attr->name);
      }
      if (attr->value) {
        free(attr->value);
      }
      if (attr->subtype.length > 0) {
        for (size_t j = 0; j < attr->subtype.length; j++) {
          free(attr->subtype.content[j]);
        }
        free(attr->subtype.content);
        free(attr->subtype.len_subtype);
      }
    }
  }
  free(entry->attributes);
}

void ldif_destroy(ldif_t *ldif) {
  if (ldif->state.fd >= 0) {
    close(ldif->state.fd);
  }
  if (ldif->state.buffer.content) {
    free(ldif->state.buffer.content);
  }

  XXH3_freeState(ldif->state.current_hash_state);
  for (ldif_entry_chunk_t *c = ldif->first; c;) {
    ldif_entry_chunk_t *n = c->next;
    for (size_t i = 0; i < c->length; i++) {
      ldif_free_entry(&c->entries[i]);
    }
    free(c->entries);
    free(c);
    c = n;
  }
}
