#include "linked_str_list.h"

#include <stdlib.h>
#include <string.h>

struct linked_str_list_node_t
{
    char* value;
    struct linked_str_list_node_t* next;
};

struct linked_str_list_t
{
    uint32_t count;
    struct linked_str_list_node_t* head;
    struct linked_str_list_node_t* tail;
};

LinkedStrList
linked_str_list_create()
{
    struct linked_str_list_t* list = malloc(sizeof(struct linked_str_list_t));
    if (list == NULL)
    {
        return NULL;
    }
    memset(list, 0, sizeof(struct linked_str_list_t));
    return list;
}

void
linked_str_list_destroy(LinkedStrList list)
{
    if (list == NULL)
    {
        return;
    }
    struct linked_str_list_node_t* node = list->head;
    struct linked_str_list_node_t* prev = NULL;
    while (node)
    {
        prev = node;
        node = prev->next;
        list->count -= 1;
        free(prev);
    }
    free(list);
}

void
linked_str_list_free_and_destroy(LinkedStrList list)
{
    if (list == NULL)
    {
        return;
    }
    struct linked_str_list_node_t* node = list->head;
    struct linked_str_list_node_t* prev = NULL;
    while (node)
    {
        prev = node;
        node = prev->next;
        list->count -= 1;
        free(prev->value);
        free(prev);
    }
    free(list);
}


uint32_t
linked_str_list_count(LinkedStrList list)
{
    return list->count;
}

uint32_t
linked_str_list_add_value(LinkedStrList list, char* str)
{
    if (list->count == UINT32_MAX)
    {
        return 0;
    }
    struct linked_str_list_node_t* node = malloc(sizeof(struct linked_str_list_node_t));
    if (node == NULL)
    {
        return 0;
    }
    node->value = str;
    node->next = NULL;
    if (list->head == NULL)
    {
        list->head = node;
        list->tail = node;
    }
    else
    {
        list->tail->next = node;
        list->tail = node;
    }
    list->count += 1;
    return list->count;
}

int
linked_str_list_contains(LinkedStrList list, const char* str)
{
    struct linked_str_list_node_t* node = list->head;
    while (node)
    {
        if (strcmp(node->value, str) == 0)
        {
            return 1;
        }
        node = node->next;
    }
    return 0;
}


char**
linked_str_list_to_array(LinkedStrList list)
{
    char** arr = malloc(sizeof(char*) * list->count);
    if (arr == NULL)
    {
        return NULL;
    }
    struct linked_str_list_node_t* node = list->head;
    const uint32_t count = list->count;
    for (uint32_t i = 0; i < count && node != NULL; i++)
    {
        arr[i] = node->value;
        node = node->next;
    }
    return arr;
}
