#ifndef LDIF_H__
#define LDIF_H__ 1

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <xxhash.h>

typedef struct {
  uint64_t hash;
  char *name;
  char *value;
  struct {
    char **content;
    size_t *len_subtype;
    size_t length;
    size_t capacity;
  } subtype;
  size_t len_name;
  size_t len_value;
  bool binary;
} ldif_kv_t;

typedef struct {
  ldif_kv_t *attributes;
  size_t length;
  size_t capacity;
} ldif_entry_t;

typedef struct {
  ldif_entry_t **entries;
  size_t length;
  size_t capacity;
  struct {
    ldif_entry_t *current_entry;
    ldif_kv_t *current_attribute;
    XXH3_state_t *current_hash_state;
    struct {
      char *content;
      size_t length;
      size_t capacity;
    } buffer;
    int fd;

    unsigned short attr_name : 1;
    unsigned short attr_value : 1;
    unsigned short attr_subtype : 1;
    unsigned short line_start : 1;
    unsigned short comment : 1;
    unsigned short continuation : 1;
    unsigned short prev_was_sep : 1;
  } state;
} ldif_t;

void ldif_parse_file(ldif_t *ldif);
void ldif_destroy(ldif_t *ldif);

#endif /* LDIF_H__ */
