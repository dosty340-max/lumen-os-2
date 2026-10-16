// Shell (M1 placeholder: solid colour screen)
#include "lumen.h"

void shell_init(void) {}
void shell_event(InputEv *e) { (void)e; }
void shell_frame(Canvas *c, float dt) {
    (void)dt;
    g_clear(c, C_ORANGE);
}
