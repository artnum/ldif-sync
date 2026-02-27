#include "include/ldif.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {

  ldif_t file = {0};
  if (argc < 2 || !argv[1]) {
    fprintf(stderr, "Usage : %s file.ldif\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  file.state.fd = open(argv[1], 0);
  file.state.max_chunk_count = getpagesize() / sizeof(ldif_entry_t);
  if (file.state.fd < 0) {
    fprintf(stderr, "Opening the file failed : %s", strerror(errno));
    exit(EXIT_FAILURE);
  }

  struct timespec start = {0}, end = {0};
  clock_gettime(CLOCK_MONOTONIC, &start);
  ldif_parse_file(&file);
  clock_gettime(CLOCK_MONOTONIC, &end);

  for (ldif_entry_chunk_t *c = file.first; c; c = c->next) {
    for (size_t i = 0; i < c->length; i++) {
      ldif_entry_t *e = &c->entries[i];
      printf("--- ENTRY %lx---\n", e->hash);
      for (size_t j = 0; j < e->length; j++) {
        printf("ATTR [%lx] [%s] SUBTYPE [", e->attributes[j].hash,
               e->attributes[j].name);
        if (e->attributes[j].subtype.length > 0) {
          for (size_t i = 0; i < e->attributes[j].subtype.length; i++) {
            if (i > 0) {

              printf(",");
            }
            printf("%.*s", (int)e->attributes[j].subtype.len_subtype[i],
                   e->attributes[j].subtype.content[i]);
          }
        }
        printf("] VALUE(%s) [%s]\n", e->attributes[j].binary ? "b" : "t",
               e->attributes[j].value);
      }
    }
  }

  int64_t diff = (end.tv_sec * 1000000000 + end.tv_nsec) -
                 (start.tv_sec * 1000000000 + start.tv_nsec);
  printf("PARSE TIME %f ms\n", (float)diff / 1000000);

  ldif_destroy(&file);
}
