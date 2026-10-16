// Debug tool: parse+style+layout an HTML file with the phone's web engine and dump the display list.
// Built and run in the toolchain container by test/webdump.sh
#include "../src/web/web.h"
void request_reboot(void) {}
int main(int argc, char **argv) {
    font_init();
    FILE *f = fopen(argv[1], "rb");
    static char buf[8 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    Doc *d = html_parse(buf, n, argc > 2 ? argv[2] : "https://example.com/");
    Sheet *sh = css_parse(UA_CSS, 0);
    for (int i = 3; i < argc; i++) {   // extra stylesheet files
        FILE *cf = fopen(argv[i], "rb");
        static char cb[4 << 20];
        size_t cn = fread(cb, 1, sizeof cb - 1, cf); cb[cn] = 0; fclose(cf);
        css_parse(cb, sh);
    }
    for (Node *c = d->head->first; c; c = c->next) if (c->type == N_ELEM && !strcmp(c->tag, "style") && c->first) css_parse(c->first->text, sh);
    double t0 = now();
    css_style_doc(d, sh, 16);
    double t1 = now();
    DList *dl = layout_doc(d, SW, 1);
    printf("title: %s\nstyle %.0f ms, layout %.0f ms, %d items, height %.0f\n", d->title, (t1 - t0) * 1000, (now() - t1) * 1000, dl->n, dl->height);
    for (int i = 0; i < dl->n && i < (getenv("MAX") ? atoi(getenv("MAX")) : 200); i++) {
        DItem *it = &dl->it[i];
        printf("%d k%d x%.0f y%.0f w%.0f h%.0f c%08x f%d s%.0f '%.*s' link%d\n", i, it->kind, it->x, it->y, it->w, it->h, it->color, it->font, it->size, it->kind == DI_TEXT ? it->len : 0, it->text ? it->text : "", it->link);
    }
}
