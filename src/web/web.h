// Lumen Web engine: HTML tokenizer + tree builder, CSS, layout, display list.
#pragma once
#include "../net/net.h"

// ---- arena (one per document) -----------------------------------------------------------
typedef struct Arena { struct ArenaBlk *blk; } Arena;
void *ar_alloc(Arena *a, size_t n);
char *ar_strndup(Arena *a, const char *s, size_t n);
void ar_free(Arena *a);

// ---- DOM -------------------------------------------------------------------------------------
enum { N_ELEM = 1, N_TEXT = 3 };
typedef struct Style Style;
typedef struct Node {
    int type;
    const char *tag;            // lower-case (elements)
    char **attr;                // name, value pairs
    int nattr;
    char *text; int tlen;       // text nodes
    struct Node *parent, *first, *last, *next;
    Style *st;                  // computed style
    // replaced/interactive content
    Image *img; int img_state;  // 0 none, 1 loading, 2 done, 3 failed
    char *value;                // form control value (malloc'd, UI-editable)
    int checked;
} Node;

typedef struct Doc {
    Arena ar;
    Node *root, *head, *body;
    char *title;
    char *base;                 // base URL
    char *refresh;              // meta refresh target
} Doc;

Doc *html_parse(const char *src, size_t len, const char *url);
void doc_free(Doc *d);
const char *attr(const Node *n, const char *name);
bool has_class(const Node *n, const char *cls);
void text_content(const Node *n, char *out, int cap);   // concatenated text, whitespace collapsed

// ---- CSS ---------------------------------------------------------------------------------------
enum { D_NONE, D_INLINE, D_BLOCK, D_LIST_ITEM, D_TABLE, D_ROW, D_CELL, D_INLINE_BLOCK, D_ROWGROUP };
typedef struct { float v; uint8_t unit; } Len;   // unit: 0 auto/none, 1 px, 2 %, 3 em
struct Style {
    uint8_t display, align, ws, list, underline, bold, hidden, italic, pos_abs, floated;
    uint32_t color, bg;
    float fs, lh;
    Len margin[4], padding[4], width, maxw, height, left, top;
    float border[4];
    uint32_t bcolor[4];
};
typedef struct Sheet Sheet;
Sheet *css_parse(const char *src, Sheet *into);     // append rules
void css_free(Sheet *s);
void css_style_doc(Doc *d, Sheet *s, float base_px);  // computes n->st for all elements
uint32_t css_color(const char *v, bool *ok);
extern const char *UA_CSS;

// ---- layout -> display list ---------------------------------------------------------------------
enum { DI_RECT, DI_TEXT, DI_IMAGE, DI_LINE, DI_INPUT, DI_BUTTON };
typedef struct {
    uint8_t kind, font;
    float x, y, w, h, size;
    uint32_t color;
    const char *text; int len;
    Node *node;                 // image / input owner
    int link;                   // index into links (-1)
} DItem;
typedef struct DList {
    DItem *it; int n, cap;
    char **links; int nlinks, capl;
    float height;
    uint32_t page_bg;
} DList;
DList *layout_doc(Doc *d, float width, float scale);
void dlist_free(DList *l);
