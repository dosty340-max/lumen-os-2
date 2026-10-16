// Game: 2048 with sliding/merging animations, swipe + arrow keys, best score saved.
#include "../ui.h"

typedef struct { int val, gx, gy, alive, dying; Spring px, py, sc; } Tile;
#define MAXTILE 64
static Tile tiles[MAXTILE];
static int grid[4][4];               // tile index + 1, 0 = empty
static int score, best = -1, over, won_shown;
static Gest g;
static Spring score_pop;

int game_best(void) { if (best < 0) best = store_geti("game.best", 0); return best; }

static int new_tile(int v, int x, int y) {
    for (int i = 0; i < MAXTILE; i++)
        if (!tiles[i].alive) {
            tiles[i] = (Tile){ v, x, y, 1, 0, { x, 0, x }, { y, 0, y }, { 0, 0, 1 } };
            grid[y][x] = i + 1;
            return i;
        }
    return -1;
}
static void spawn(void) {
    int free_[16], n = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (!grid[y][x]) free_[n++] = y * 4 + x;
    if (!n) return;
    int k = free_[rand() % n];
    new_tile(rand() % 10 ? 2 : 4, k % 4, k / 4);
}
static void reset(void) {
    memset(tiles, 0, sizeof tiles);
    memset(grid, 0, sizeof grid);
    score = 0; over = 0; won_shown = 0;
    spawn(); spawn();
}
static bool can_move(void) {
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        if (!grid[y][x]) return true;
        int v = tiles[grid[y][x] - 1].val;
        if (x < 3 && grid[y][x + 1] && tiles[grid[y][x + 1] - 1].val == v) return true;
        if (y < 3 && grid[y + 1][x] && tiles[grid[y + 1][x] - 1].val == v) return true;
    }
    return false;
}

// dir: 0 left, 1 right, 2 up, 3 down
static void move(int dir) {
    if (over) return;
    for (int i = 0; i < MAXTILE; i++) if (tiles[i].dying) tiles[i].alive = 0;
    bool moved = false;
    for (int line = 0; line < 4; line++) {
        int cells[4][2];
        for (int k = 0; k < 4; k++) {   // cells in the order tiles travel towards
            int a = (dir == 1 || dir == 3) ? 3 - k : k;
            if (dir < 2) { cells[k][0] = a; cells[k][1] = line; }
            else { cells[k][0] = line; cells[k][1] = a; }
        }
        int out = 0, last = -1;    // last: tile index at out-1 that may still merge
        for (int k = 0; k < 4; k++) {
            int x = cells[k][0], y = cells[k][1];
            if (!grid[y][x]) continue;
            int ti = grid[y][x] - 1;
            grid[y][x] = 0;
            Tile *t = &tiles[ti];
            if (last >= 0 && tiles[last].val == t->val) {
                Tile *m = &tiles[last];
                t->gx = m->gx; t->gy = m->gy; t->dying = 1;   // slides under, then disappears
                t->px.t = t->gx; t->py.t = t->gy;
                m->val *= 2; m->sc.x = 1.25f; m->sc.v = 0;
                score += m->val;
                score_pop.x = 1.3f;
                last = -1; moved = true;
                if (m->val == 2048 && !won_shown) { won_shown = 1; notify("Game", "You made 2048!"); }
                continue;
            }
            int nx = cells[out][0], ny = cells[out][1];
            if (nx != x || ny != y) moved = true;
            t->gx = nx; t->gy = ny; t->px.t = nx; t->py.t = ny;
            grid[ny][nx] = ti + 1;
            last = ti; out++;
        }
    }
    if (moved) {
        spawn();
        if (score > game_best()) { best = score; store_seti("game.best", best); }
        if (!can_move()) { over = 1; LOG("game: over, score %d", score); }
    }
}

static uint32_t tile_col(int v) {
    static const uint32_t cols[] = { C_PAPER, C_LILAC, C_SKY, C_LIME, C_ORANGE, C_PAPER, C_LILAC, C_SKY, C_LIME, C_ORANGE, C_PAPER };
    int k = 0;
    while ((2 << k) < v && k < 10) k++;
    return cols[k];
}

static void open_(void) { if (!tiles[0].alive && !tiles[1].alive && !score) reset(); game_best(); }

static void draw(Canvas *c, float dt) {
    g_clear(c, TH.bg);
    float hh = P(250);
    g_rect(c, 0, 0, SW, hh, C_SKY);
    status_line(c, 0, C_INK, true, false);
    text(c, W800, P(52), -0.05f, P(18), P(112), C_INK, "2048");
    text(c, W600, P(14), 0, P(22), P(138), C_INK, "Swipe to slide. Matching tiles merge.");
    score_pop.t = 1;
    if (score_pop.x == 0) score_pop.x = 1;
    spring_step(&score_pop, dt, 4, 0.5f);
    char b[32];
    snprintf(b, sizeof b, "%d", score);
    text(c, W600, P(13), 0.1f, P(22), P(180), C_INK, "SCORE");
    text(c, W800, P(44) * score_pop.x, -0.04f, P(20), P(228), C_INK, b);
    snprintf(b, sizeof b, "%d", game_best());
    text(c, W600, P(13), 0.1f, P(180), P(180), C_INK, "BEST");
    text(c, W800, P(44), -0.04f, P(178), P(228), C_INK, b);
    btn_icon(c, 0xD01, P(318), P(176), P(56), IC_REFRESH, C_INK, 0);
    // board
    float bx = P(16), by = hh + P(40), bs = P(358), gap = P(10), cs = (bs - 5 * gap) / 4;
    g_rrect(c, bx, by, bs, bs, P(28), P(28), SET.dark ? 0xFF22211Fu : 0xFFE2DDD2u);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        g_rrect_line(c, bx + gap + x * (cs + gap), by + gap + y * (cs + gap), cs, cs, P(18), P(2), TH.line);
    for (int pass = 0; pass < 2; pass++)   // dying tiles under live ones
        for (int i = 0; i < MAXTILE; i++) {
            Tile *t = &tiles[i];
            if (!t->alive || (pass == 0) != (t->dying != 0)) continue;
            spring_step(&t->px, dt, 7, 1); spring_step(&t->py, dt, 7, 1);
            spring_step(&t->sc, dt, 5, 0.55f);
            if (t->dying && fabsf(t->px.x - t->px.t) < 0.02f && fabsf(t->py.x - t->py.t) < 0.02f) { t->alive = 0; continue; }
            float s = t->sc.x, w = cs * s;
            float x = bx + gap + t->px.x * (cs + gap) + (cs - w) / 2, y = by + gap + t->py.x * (cs + gap) + (cs - w) / 2;
            g_rrect(c, x, y, w, w, P(18) * s, P(18) * s, tile_col(t->val));
            snprintf(b, sizeof b, "%d", t->val);
            float fs = (t->val < 100 ? P(40) : t->val < 1000 ? P(32) : P(25)) * s;
            text_c(c, W800, fs, -0.04f, x + w / 2, y + w / 2 + fs * 0.36f, C_INK, b);
        }
    if (over) {
        g_rrect(c, bx, by + bs / 2 - P(60), bs, P(120), P(28), P(28), C_ORANGE);
        text_c(c, W800, P(40), -0.04f, SW / 2, by + bs / 2 + P(4), C_INK, "Game over");
        text_c(c, W600, P(14), 0, SW / 2, by + bs / 2 + P(34), C_INK, "Tap the arrow to play again");
    }
    hit(0xD02, bx, by, bs, bs);
    home_bar(c, TH.fg);
}

static void touch(InputEv *e) {
    int r = gest(&g, e);
    if (r == G_DRAG_END) {
        float dx = g.x - g.x0, dy = g.y - g.y0;
        if (hypotf(dx, dy) < P(24)) return;
        if (fabsf(dx) > fabsf(dy)) move(dx < 0 ? 0 : 1);
        else move(dy < 0 ? 2 : 3);
    } else if (r == G_TAP && hit_at(e->x, e->y) == 0xD01) reset();
}
static void key(int k) {
    if (k == K_LEFT || k == 'a') move(0);
    else if (k == K_RIGHT || k == 'd') move(1);
    else if (k == K_UP || k == 'w') move(2);
    else if (k == K_DOWN || k == 's') move(3);
    else if (k == 'r') reset();
}
App app_game = { "Game", IC_GAME, open_, draw, touch, key, 0 };
