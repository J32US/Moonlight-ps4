// Minimal XML parser for GameStream/Sunshine responses.
// Format is flat and known; a general parser is not needed.
#pragma once

#include <stddef.h>

typedef struct app_entry {
    int id;
    char *name;
    struct app_entry *next;
} app_entry_t;

// Returns the status_code attribute of <root>, or -1 if not found.
int xml_status(const char *data, size_t len);

// Find the first <node>text</node> tag and return the text in
// *result (malloc, with entities decoded). GS_OK if found.
int xml_search(const char *data, size_t len, const char *node, char **result);

// Extract the <App><ID/><AppTitle/></App> list. Free with xml_applist_free.
int xml_applist(const char *data, size_t len, app_entry_t **list);
void xml_applist_free(app_entry_t *list);
