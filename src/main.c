// PID 1 + the UI main loop.
#include "lumen.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EXIT_REBOOT 42

void request_reboot(void) {
    sync();
    LOG("ui: reboot requested");
    exit(EXIT_REBOOT);
}

static void pid1(void) {
    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    mount("tmpfs", "/tmp", "tmpfs", 0, 0);
    LOG("init: Lumen OS 2 starting");
    for (;;) {
        pid_t ui = fork();
        if (ui == 0) return;            // child continues as the UI
        int st = 0;
        pid_t p;
        while ((p = wait(&st)) != ui) if (p < 0 && errno == ECHILD) break;
        if (WIFEXITED(st) && WEXITSTATUS(st) == EXIT_REBOOT) {
            sync();
            reboot(RB_AUTOBOOT);
        }
        LOG("init: ui exited (status %d), restarting", st);
        sleep(1);
    }
}

void shell_init(void);
void shell_event(InputEv *e);
void shell_frame(Canvas *c, float dt);

int main(void) {
    if (getpid() == 1) pid1();
    signal(SIGPIPE, SIG_IGN);
    Canvas *scr = drm_init();
    if (!scr) { LOG("ui: no display"); sleep(5); return 1; }
    font_init();
    input_init();
    shell_init();

    double last = now(), stat_t = last, worst = 0;
    int frames = 0;
    const double period = 1.0 / 60;
    for (;;) {
        InputEv ev[64];
        int n = input_poll(ev, 64);
        for (int i = 0; i < n; i++) shell_event(&ev[i]);
        double t = now();
        float dt = (float)fmin(t - last, 0.05);
        last = t;
        shell_frame(scr, dt);
        double work = now() - t;
        if (work > worst) worst = work;
        scr = drm_flip();
        frames++;
        if (t - stat_t >= 10) {   // frame-time log for the perf report
            LOG("perf: %.1f fps, worst frame work %.1f ms", frames / (t - stat_t), worst * 1000);
            frames = 0; worst = 0; stat_t = t;
        }
        // pace to 60 Hz even if the flip event came back early
        double next = t + period, rem = next - now();
        if (rem > 0.0005) {
            struct timespec ts = { 0, (long)(rem * 1e9) };
            nanosleep(&ts, 0);
        }
    }
}
