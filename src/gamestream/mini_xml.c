#include "mini_xml.h"
#include "gs_errors.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Tag scanner: walks the document and yields open, close, and text events.
// Enough for GameStream's flat XML.
// ---------------------------------------------------------------------------

typedef struct {
    const char *p;
    const char *end;
} scanner_t;

typedef enum {
    TOK_EOF,
    TOK_OPEN,      // <name ...>  (name in buf)
    TOK_CLOSE,     // </name>
    TOK_SELFCLOSE, // <name ... />
} token_t;

#define NAME_MAX_LEN 128

static void skip_ws(scanner_t *s) {
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\r' || *s->p == '\n'))
        s->p++;
}

// Decode basic XML entities in range [in,in+len) into a malloc'd buffer.
static char *decode_entities(const char *in, size_t len) {
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        if (in[i] == '&') {
            if (len - i >= 5 && !strncmp(in + i, "&amp;", 5)) { out[o++] = '&'; i += 5; continue; }
            if (len - i >= 4 && !strncmp(in + i, "&lt;", 4)) { out[o++] = '<'; i += 4; continue; }
            if (len - i >= 4 && !strncmp(in + i, "&gt;", 4)) { out[o++] = '>'; i += 4; continue; }
            if (len - i >= 6 && !strncmp(in + i, "&quot;", 6)) { out[o++] = '"'; i += 6; continue; }
            if (len - i >= 6 && !strncmp(in + i, "&apos;", 6)) { out[o++] = '\''; i += 6; continue; }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
    return out;
}

// Advance to the next tag. Returns type, name in `name`, and,
// if attrs is not NULL, raw attributes (range within the document).
static token_t next_tag(scanner_t *s, char *name, const char **attrs, size_t *attrs_len) {
    for (;;) {
        while (s->p < s->end && *s->p != '<')
            s->p++;
        if (s->p >= s->end)
            return TOK_EOF;
        s->p++; // consume '<'
        if (s->p >= s->end)
            return TOK_EOF;

        // Skip declarations, comments, and CDATA.
        if (*s->p == '?' || *s->p == '!') {
            while (s->p < s->end && *s->p != '>')
                s->p++;
            if (s->p < s->end)
                s->p++;
            continue;
        }

        int closing = 0;
        if (*s->p == '/') {
            closing = 1;
            s->p++;
        }

        size_t n = 0;
        while (s->p < s->end && *s->p != '>' && *s->p != '/' && *s->p != ' ' &&
               *s->p != '\t' && *s->p != '\r' && *s->p != '\n') {
            if (n < NAME_MAX_LEN - 1)
                name[n++] = *s->p;
            s->p++;
        }
        name[n] = '\0';

        skip_ws(s);
        const char *attr_start = s->p;

        // Find end of tag respecting attribute quotes.
        int in_quote = 0;
        char quote_ch = 0;
        while (s->p < s->end) {
            char c = *s->p;
            if (in_quote) {
                if (c == quote_ch)
                    in_quote = 0;
            } else if (c == '"' || c == '\'') {
                in_quote = 1;
                quote_ch = c;
            } else if (c == '>') {
                break;
            }
            s->p++;
        }
        if (s->p >= s->end)
            return TOK_EOF;

        int selfclose = (s->p > attr_start && s->p[-1] == '/');
        size_t alen = (size_t)(s->p - attr_start) - (selfclose ? 1 : 0);
        s->p++; // consume '>'

        if (attrs) {
            *attrs = attr_start;
            *attrs_len = alen;
        }

        if (closing)
            return TOK_CLOSE;
        return selfclose ? TOK_SELFCLOSE : TOK_OPEN;
    }
}

// Text between current position and the next '<'.
static void tag_text(scanner_t *s, const char **text, size_t *text_len) {
    *text = s->p;
    const char *q = s->p;
    while (q < s->end && *q != '<')
        q++;
    *text_len = (size_t)(q - *text);
}

// Value of an attribute within the raw attribute range.
static int attr_value(const char *attrs, size_t len, const char *key, char *out, size_t outlen) {
    size_t klen = strlen(key);
    const char *p = attrs;
    const char *end = attrs + len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            p++;
        const char *name_start = p;
        while (p < end && *p != '=' && *p != ' ')
            p++;
        size_t nlen = (size_t)(p - name_start);
        while (p < end && *p != '=')
            p++;
        if (p >= end)
            break;
        p++; // '='
        while (p < end && (*p == ' '))
            p++;
        if (p >= end || (*p != '"' && *p != '\''))
            break;
        char q = *p++;
        const char *val_start = p;
        while (p < end && *p != q)
            p++;
        size_t vlen = (size_t)(p - val_start);
        if (p < end)
            p++; // closing quote
        if (nlen == klen && !strncmp(name_start, key, klen)) {
            if (vlen >= outlen)
                vlen = outlen - 1;
            memcpy(out, val_start, vlen);
            out[vlen] = '\0';
            return 0;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// API publica
// ---------------------------------------------------------------------------

int xml_status(const char *data, size_t len) {
    scanner_t s = { data, data + len };
    char name[NAME_MAX_LEN];
    const char *attrs;
    size_t alen;

    for (;;) {
        token_t t = next_tag(&s, name, &attrs, &alen);
        if (t == TOK_EOF)
            return -1;
        if ((t == TOK_OPEN || t == TOK_SELFCLOSE) && !strcmp(name, "root")) {
            char val[16];
            if (attr_value(attrs, alen, "status_code", val, sizeof(val)) == 0)
                return atoi(val);
            return -1;
        }
    }
}

int xml_search(const char *data, size_t len, const char *node, char **result) {
    scanner_t s = { data, data + len };
    char name[NAME_MAX_LEN];

    *result = NULL;
    for (;;) {
        token_t t = next_tag(&s, name, NULL, NULL);
        if (t == TOK_EOF)
            return GS_INVALID;
        if (t == TOK_OPEN && !strcmp(name, node)) {
            const char *text;
            size_t tlen;
            tag_text(&s, &text, &tlen);
            *result = decode_entities(text, tlen);
            return *result ? GS_OK : GS_OUT_OF_MEMORY;
        }
        if (t == TOK_SELFCLOSE && !strcmp(name, node)) {
            *result = calloc(1, 1);
            return *result ? GS_OK : GS_OUT_OF_MEMORY;
        }
    }
}

int xml_applist(const char *data, size_t len, app_entry_t **list) {
    scanner_t s = { data, data + len };
    char name[NAME_MAX_LEN];
    app_entry_t *head = NULL;
    app_entry_t *cur = NULL;

    *list = NULL;
    for (;;) {
        token_t t = next_tag(&s, name, NULL, NULL);
        if (t == TOK_EOF)
            break;
        if (t == TOK_OPEN && !strcmp(name, "App")) {
            app_entry_t *app = calloc(1, sizeof(*app));
            if (!app)
                goto oom;
            app->next = head;
            head = app;
            cur = app;
        } else if (t == TOK_OPEN && cur && !strcmp(name, "ID")) {
            const char *text;
            size_t tlen;
            tag_text(&s, &text, &tlen);
            cur->id = atoi(text);
        } else if (t == TOK_OPEN && cur && !strcmp(name, "AppTitle")) {
            const char *text;
            size_t tlen;
            tag_text(&s, &text, &tlen);
            free(cur->name);
            cur->name = decode_entities(text, tlen);
            if (!cur->name)
                goto oom;
        }
    }

    // List was built reversed; reverse it to preserve order.
    app_entry_t *prev = NULL;
    while (head) {
        app_entry_t *next = head->next;
        head->next = prev;
        prev = head;
        head = next;
    }
    *list = prev;
    return GS_OK;

oom:
    xml_applist_free(head);
    return GS_OUT_OF_MEMORY;
}

void xml_applist_free(app_entry_t *list) {
    while (list) {
        app_entry_t *next = list->next;
        free(list->name);
        free(list);
        list = next;
    }
}
