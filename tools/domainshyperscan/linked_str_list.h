#ifndef LINKED_STR_LIST_H
#define LINKED_STR_LIST_H

#include <stdint.h>

typedef struct linked_str_list_t* LinkedStrList;

LinkedStrList
linked_str_list_create();

void
linked_str_list_destroy(LinkedStrList list);

void
linked_str_list_free_and_destroy(LinkedStrList list);

uint32_t
linked_str_list_count(LinkedStrList list);

uint32_t
linked_str_list_add_value(LinkedStrList list, char* str);

int
linked_str_list_contains(LinkedStrList list, const char* str);

char**
linked_str_list_to_array(LinkedStrList list);
#endif //LINKED_STR_LIST_H
