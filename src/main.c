#include "include/ldif.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {

  ldif_t file = {0};
  if (argc < 2 || !argv[1]) {
    fprintf(stderr, "Usage : %s file.ldif\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  file.state.fd = open(argv[1], 0);
  if (file.state.fd < 0) {
    fprintf(stderr, "Opening the file failed : %s", strerror(errno));
    exit(EXIT_FAILURE);
  }

  ldif_parse_file(&file);

  for (size_t i = 0; i < file.length; i++) {
    ldif_entry_t *e = file.entries[i];
    printf("--- ENTRY---\n");
    for (size_t j = 0; j < e->length; j++) {
      printf("ATTR [%lx] [%s] SUBTYPE [", e->attributes[j]->hash,
             e->attributes[j]->name);
      if (e->attributes[j]->subtype.length > 0) {
        for (size_t i = 0; i < e->attributes[j]->subtype.length; i++) {
          if (i > 0) {

            printf(",");
          }
          printf("%.*s", (int)e->attributes[j]->subtype.len_subtype[i],
                 e->attributes[j]->subtype.content[i]);
        }
      }
      printf("] VALUE(%s) [%s]\n", e->attributes[j]->binary ? "b" : "t",
             e->attributes[j]->value);
    }
  }

  ldif_destroy(&file);
}
