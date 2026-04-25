/*
 * gui.c — Smart Classroom Resource Manager
 * Raylib-based GUI: dark dashboard, 4 tabs, live simulation.
 *
 * Tabs:  [1] Dashboard  [2] Sessions  [3] Statistics  [4] Event Log
 *
 * The simulation starts automatically in background threads.
 * The Raylib window is the main thread — it just reads shared state.
 */

#include "raylib.h"
#include "resources.h"
#include "scheduler.h"
#include "logger.h"
#include "stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

/* ── Simulation config ──────────────────────────────────────────────────── */
#define NUM_SESSIONS        12
#define MAX_USE_TIME_SEC     8
#define LOG_FILE            "classroom_log.txt"
#define PROJECTOR_UNITS      2
#define COMP_LAB_UNITS       1
#define MULTIMEDIA_UNITS     2

/* ── Window / UI ────────────────────────────────────────────────────────── */
#define WIN_W   1100
#define WIN_H    670
#define TAB_H     42
#define PANEL_PAD 14

/* ── Session state ──────────────────────────────────────────────────────── */
typedef enum {
    SS_IDLE = 0,
    SS_REQUESTING,
    SS_WAITING,
    SS_ACQUIRED,
    SS_RELEASED
} SessionStatus;

typedef struct {
    int           id;
    int           resource_type;
    int           priority;
    SessionStatus status;
    double        wait_time;
    int           use_duration;
    int           in_conflict;   /* flashes red when conflict detected */
} SessionInfo;

/* ── Log ring buffer ────────────────────────────────────────────────────── */
#define MAX_LOG_LINES 256
typedef struct {
    char lines[MAX_LOG_LINES][128];
    int  head;   /* next write index */
    int  count;
    pthread_mutex_t mu;
} GuiLog;

/* ── Shared GUI state ───────────────────────────────────────────────────── */
static ResourcePool  pool;
static WaitQueue     wq;
static Stats         stats;

static SessionInfo   sessions[NUM_SESSIONS];
static pthread_mutex_t sess_mu = PTHREAD_MUTEX_INITIALIZER;

static GuiLog        gui_log;
static int           sim_done = 0;

/* conflict flash timer (seconds) */
static double        conflict_flash[NUM_SESSIONS];

/* ── Colours ────────────────────────────────────────────────────────────── */
static const Color C_BG        = {15,  17,  23,  255};
static const Color C_PANEL     = {24,  27,  36,  255};
static const Color C_PANEL2    = {32,  36,  48,  255};
static const Color C_BORDER    = {50,  55,  72,  255};
static const Color C_TEXT      = {220, 225, 235, 255};
static const Color C_MUTED     = {110, 118, 140, 255};
static const Color C_ACCENT    = {99,  179, 237, 255};   /* blue  */
static const Color C_GREEN     = {72,  199, 142, 255};
static const Color C_YELLOW    = {246, 194,  62, 255};
static const Color C_RED       = {252, 100,  90, 255};
static const Color C_PURPLE    = {159, 122, 234, 255};
static const Color C_TAB_ACT   = {36,  40,  54,  255};
static const Color C_TAB_INACT = {20,  23,  31,  255};

/* ── Font ───────────────────────────────────────────────────────────────── */
static Font G_FONT;   /* loaded after InitWindow */

/* Draw text using our custom font. 'size' is the original DrawText size —
 * we scale it up slightly so it matches Raylib's bitmap-font pixel sizes. */
static void TXT(const char *text, int x, int y, int size, Color color) {
    DrawTextEx(G_FONT, text, (Vector2){(float)x, (float)y},
               (float)(size + 2), 1.0f, color);
}
/* Measure text width using our font (matches TXT scaling). */
static int TXTW(const char *text, int size) {
    return (int)MeasureTextEx(G_FONT, text, (float)(size + 2), 1.0f).x;
}

/* status → colour */
static Color status_color(SessionStatus s) {
    switch (s) {
        case SS_REQUESTING: return C_ACCENT;
        case SS_WAITING:    return C_YELLOW;
        case SS_ACQUIRED:   return C_GREEN;
        case SS_RELEASED:   return C_MUTED;
        default:            return C_BORDER;
    }
}
static const char *status_label(SessionStatus s) {
    switch (s) {
        case SS_REQUESTING: return "REQUESTING";
        case SS_WAITING:    return "WAITING";
        case SS_ACQUIRED:   return "ACQUIRED";
        case SS_RELEASED:   return "RELEASED";
        default:            return "IDLE";
    }
}
static const char *res_label(int r) {
    return resource_names[r];
}
static Color res_color(int r) {
    if (r == RESOURCE_PROJECTOR)  return C_ACCENT;
    if (r == RESOURCE_COMP_LAB)   return C_PURPLE;
    return C_GREEN;
}

/* ── GUI log helpers ────────────────────────────────────────────────────── */
static void gui_log_init(void) {
    memset(&gui_log, 0, sizeof(gui_log));
    pthread_mutex_init(&gui_log.mu, NULL);
}
static void gui_log_push(const char *line) {
    pthread_mutex_lock(&gui_log.mu);
    strncpy(gui_log.lines[gui_log.head], line, 127);
    gui_log.lines[gui_log.head][127] = '\0';
    gui_log.head = (gui_log.head + 1) % MAX_LOG_LINES;
    if (gui_log.count < MAX_LOG_LINES) gui_log.count++;
    pthread_mutex_unlock(&gui_log.mu);
}

/* ── Session thread ─────────────────────────────────────────────────────── */
typedef struct { int idx; } ThreadArg;

static void *session_thread(void *arg) {
    ThreadArg *ta  = (ThreadArg *)arg;
    int idx        = ta->idx;
    free(ta);

    pthread_mutex_lock(&sess_mu);
    int id    = sessions[idx].id;
    int rtype = sessions[idx].resource_type;
    int prio  = sessions[idx].priority;
    sessions[idx].status = SS_REQUESTING;
    pthread_mutex_unlock(&sess_mu);

    char buf[128];
    snprintf(buf, sizeof(buf), "[REQUEST ] S%-2d %-11s pri=%d",
             id, res_label(rtype), prio);
    gui_log_push(buf);
    log_event("REQUEST", id, res_label(rtype), "Session requesting resource");
    stats_record_request(&stats, rtype);

    time_t t_req = time(NULL);

    if (!resource_try_acquire(&pool, rtype)) {
        /* check for conflict — any other session waiting for same resource */
        pthread_mutex_lock(&sess_mu);
        for (int i = 0; i < NUM_SESSIONS; i++) {
            if (i != idx &&
                sessions[i].resource_type == rtype &&
                sessions[i].status == SS_WAITING) {
                sessions[idx].in_conflict = 1;
                sessions[i].in_conflict   = 1;
                conflict_flash[idx] = 2.0;
                conflict_flash[i]   = 2.0;
                snprintf(buf, sizeof(buf),
                         "[CONFLICT] S%d and S%d compete for %s",
                         sessions[i].id, id, res_label(rtype));
                gui_log_push(buf);
                log_conflict(sessions[i].id, id, res_label(rtype));
                stats_record_conflict(&stats);
                break;
            }
        }
        sessions[idx].status = SS_WAITING;
        pthread_mutex_unlock(&sess_mu);

        snprintf(buf, sizeof(buf), "[QUEUED  ] S%-2d %-11s (waiting)",
                 id, res_label(rtype));
        gui_log_push(buf);

        time_t t_wait = time(NULL);
        scheduler_wait(&wq, &pool, id, rtype, prio);

        double waited = difftime(time(NULL), t_wait);
        stats_record_wait(&stats, rtype, waited);
        pthread_mutex_lock(&sess_mu);
        sessions[idx].wait_time = waited;
        pthread_mutex_unlock(&sess_mu);
    }

    double total_wait = difftime(time(NULL), t_req);
    pthread_mutex_lock(&sess_mu);
    sessions[idx].status      = SS_ACQUIRED;
    sessions[idx].in_conflict = 0;
    pthread_mutex_unlock(&sess_mu);

    snprintf(buf, sizeof(buf), "[ACQUIRED] S%-2d %-11s waited=%.0fs",
             id, res_label(rtype), total_wait);
    gui_log_push(buf);
    log_event("ACQUIRED", id, res_label(rtype), "Resource acquired");
    stats_record_granted(&stats, rtype);

    int use_dur = 1 + rand() % MAX_USE_TIME_SEC;
    pthread_mutex_lock(&sess_mu);
    sessions[idx].use_duration = use_dur;
    pthread_mutex_unlock(&sess_mu);
    sleep(use_dur);

    resource_release(&pool, rtype);
    pthread_mutex_lock(&sess_mu);
    sessions[idx].status = SS_RELEASED;
    pthread_mutex_unlock(&sess_mu);

    snprintf(buf, sizeof(buf), "[RELEASED] S%-2d %-11s used=%ds",
             id, res_label(rtype), use_dur);
    gui_log_push(buf);
    log_event("RELEASED", id, res_label(rtype), "Resource released");

    scheduler_notify(&wq, &pool, rtype);
    return NULL;
}

/* ── Draw helpers ───────────────────────────────────────────────────────── */
static void draw_rect_rounded_border(Rectangle r, float rad, Color fill, Color border) {
    DrawRectangleRounded(r, rad, 6, fill);
    DrawRectangleRoundedLines(r, rad, 6, border);
}

/* single tab button — returns 1 if clicked */
static int draw_tab(int x, int y, int w, int h, const char *label, int active) {
    Rectangle r = {(float)x, (float)y, (float)w, (float)h};
    Color bg = active ? C_TAB_ACT : C_TAB_INACT;
    DrawRectangleRec(r, bg);
    if (active) {
        DrawRectangle(x, y + h - 3, w, 3, C_ACCENT);
    }
    int tw = TXTW(label, 16);
    TXT(label, x + (w - tw) / 2, y + (h - 16) / 2,
             16, active ? C_TEXT : C_MUTED);
    return IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
           CheckCollisionPointRec(GetMousePosition(), r);
}

/* resource bar — used / total */
static void draw_resource_bar(int x, int y, int w, int h,
                               const char *name, int used, int total,
                               Color col) {
    /* background panel */
    Rectangle panel = {(float)x, (float)y, (float)w, (float)h};
    draw_rect_rounded_border(panel, 0.08f, C_PANEL2, C_BORDER);

    /* label */
    TXT(name, x + 14, y + 14, 18, C_TEXT);

    /* unit count */
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d / %d in use", used, total);
    int cw = TXTW(cnt, 14);
    TXT(cnt, x + w - cw - 14, y + 16, 14, C_MUTED);

    /* bar track */
    int bx = x + 14, by = y + h - 26, bw = w - 28, bh = 12;
    DrawRectangleRounded((Rectangle){(float)bx,(float)by,(float)bw,(float)bh},
                          0.5f, 6, C_PANEL);
    /* bar fill */
    int fill_w = total > 0 ? (bw * used / total) : 0;
    if (fill_w > 0)
        DrawRectangleRounded(
            (Rectangle){(float)bx,(float)by,(float)fill_w,(float)bh},
            0.5f, 6, col);

    /* unit dots */
    int dot_r = 6;
    for (int i = 0; i < total; i++) {
        int dx = x + 14 + i * (dot_r * 2 + 6);
        int dy = y + h - 50;
        Color dc = (i < used) ? col : C_BORDER;
        DrawCircle(dx + dot_r, dy + dot_r, (float)dot_r, dc);
    }
}

/* ── TAB 1: Dashboard ───────────────────────────────────────────────────── */
static void draw_dashboard(int y0) {
    int pad = PANEL_PAD;

    /* title */
    TXT("Resource Pool - Live Status", pad, y0 + 10, 20, C_TEXT);

    /* 3 resource bars */
    int bw = (WIN_W - pad * 4) / 3;
    int bh = 110;
    int by = y0 + 46;

    pthread_mutex_lock(&pool.pool_mutex);
    int used[3], total[3];
    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        used[i]  = pool.in_use[i];
        total[i] = pool.total_units[i];
    }
    pthread_mutex_unlock(&pool.pool_mutex);

    Color rcols[3] = {C_ACCENT, C_PURPLE, C_GREEN};
    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        int bx = pad + i * (bw + pad);
        draw_resource_bar(bx, by, bw, bh,
                          resource_names[i], used[i], total[i], rcols[i]);
    }

    /* Queue depth */
    int qy = by + bh + pad;
    TXT("Wait Queue Depth", pad, qy, 18, C_TEXT);
    int qbw = (WIN_W - pad * 4) / 3;
    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        int depth = scheduler_queue_depth(&wq, i);
        int qx = pad + i * (qbw + pad);
        Rectangle qr = {(float)qx, (float)(qy + 28), (float)qbw, 56};
        draw_rect_rounded_border(qr, 0.08f, C_PANEL2, C_BORDER);
        TXT(resource_names[i], qx + 12, qy + 38, 14, C_MUTED);
        char dstr[16]; snprintf(dstr, sizeof(dstr), "%d waiting", depth);
        TXT(dstr, qx + 12, qy + 58, 20,
                 depth > 0 ? C_YELLOW : C_GREEN);
    }

    /* Conflict alerts panel */
    int cy = qy + 28 + 56 + pad;

    pthread_mutex_lock(&stats.stats_mutex);
    int conflicts = stats.conflicts_detected;
    pthread_mutex_unlock(&stats.stats_mutex);

    Rectangle cr = {(float)pad, (float)cy, (float)(WIN_W - pad*2), 60};
    Color cbg = conflicts > 0 ? (Color){60,20,20,255} : C_PANEL2;
    Color cborder = conflicts > 0 ? C_RED : C_BORDER;
    draw_rect_rounded_border(cr, 0.06f, cbg, cborder);

    char cstr[64];
    snprintf(cstr, sizeof(cstr), "Conflict Alerts Detected: %d", conflicts);
    TXT(cstr, pad + 16, cy + 20, 20,
             conflicts > 0 ? C_RED : C_MUTED);

    /* Simulation status */
    int sy = cy + 60 + pad;
    Rectangle sr = {(float)pad, (float)sy, (float)(WIN_W - pad*2), 50};
    draw_rect_rounded_border(sr, 0.06f, C_PANEL2, C_BORDER);
    const char *sim_status = sim_done ? "Simulation Complete" : "Simulation Running...";
    Color sim_col = sim_done ? C_GREEN : C_ACCENT;
    TXT(sim_status, pad + 16, sy + 15, 18, sim_col);

    if (!sim_done) {
        /* pulsing dot */
        float t = (float)GetTime();
        float alpha = (sinf(t * 4.0f) + 1.0f) * 0.5f;
        Color dot = {(unsigned char)(C_ACCENT.r),
                     (unsigned char)(C_ACCENT.g),
                     (unsigned char)(C_ACCENT.b),
                     (unsigned char)(alpha * 255)};
        int tw = TXTW(sim_status, 18);
        DrawCircle(pad + 16 + tw + 14, sy + 24, 6, dot);
    }
}

/* ── TAB 2: Sessions ────────────────────────────────────────────────────── */
static void draw_sessions(int y0) {
    int pad = PANEL_PAD;
    TXT("Session Cards - Live View", pad, y0 + 10, 20, C_TEXT);

    /* 4 columns × 3 rows */
    int cols = 4;
    int card_w = (WIN_W - pad * (cols + 1)) / cols;
    int card_h = 158;
    int start_y = y0 + 44;

    pthread_mutex_lock(&sess_mu);
    SessionInfo snap[NUM_SESSIONS];
    memcpy(snap, sessions, sizeof(snap));
    pthread_mutex_unlock(&sess_mu);

    double now = GetTime();

    for (int i = 0; i < NUM_SESSIONS; i++) {
        int col = i % cols;
        int row = i / cols;
        int cx = pad + col * (card_w + pad);
        int cy = start_y + row * (card_h + pad);

        SessionStatus st = snap[i].status;
        Color sc = status_color(st);

        /* conflict flash — red border override */
        Color border = C_BORDER;
        if (conflict_flash[i] > 0.0) {
            conflict_flash[i] -= GetFrameTime();
            float f = (float)(conflict_flash[i] / 2.0);
            border = (Color){
                (unsigned char)(C_RED.r),
                (unsigned char)(C_RED.g),
                (unsigned char)(C_RED.b),
                (unsigned char)(128 + 127 * f)
            };
        }

        Rectangle card = {(float)cx, (float)cy, (float)card_w, (float)card_h};
        draw_rect_rounded_border(card, 0.07f, C_PANEL2, border);

        /* coloured left accent bar */
        DrawRectangleRounded(
            (Rectangle){(float)cx, (float)cy, 4, (float)card_h},
            0.3f, 4, sc);

        /* Session ID */
        char sid[16]; snprintf(sid, sizeof(sid), "S%02d", snap[i].id);
        TXT(sid, cx + 14, cy + 12, 22, C_TEXT);

        /* Resource chip */
        Color rc = res_color(snap[i].resource_type);
        const char *rl = res_label(snap[i].resource_type);
        int rlw = TXTW(rl, 13) + 16;
        Rectangle chip = {(float)(cx + card_w - rlw - 10),
                          (float)(cy + 12), (float)rlw, 22};
        DrawRectangleRounded(chip, 0.4f, 6,
            (Color){rc.r, rc.g, rc.b, 40});
        DrawRectangleRoundedLines(chip, 0.4f, 6, rc);
        TXT(rl, (int)chip.x + 8, (int)chip.y + 4, 13, rc);

        /* Priority label + value on same row, bar below */
        TXT("Priority", cx + 14, cy + 48, 13, C_MUTED);
        char pstr[8]; snprintf(pstr, sizeof(pstr), "%d/10", snap[i].priority);
        TXT(pstr, cx + card_w - TXTW(pstr, 12) - 14, cy + 48, 12, C_MUTED);

        /* Priority bar — sits on its own row, clear of the label */
        int pb_w = card_w - 28;
        int pb_x = cx + 14, pb_y = cy + 68;
        DrawRectangleRounded(
            (Rectangle){(float)pb_x,(float)pb_y,(float)pb_w,7},
            0.5f, 4, C_PANEL);
        int pf = pb_w * snap[i].priority / MAX_PRIORITY;
        Color pc = snap[i].priority >= 7 ? C_GREEN :
                   snap[i].priority >= 4 ? C_YELLOW : C_RED;
        if (pf > 0)
            DrawRectangleRounded(
                (Rectangle){(float)pb_x,(float)pb_y,(float)pf,7},
                0.5f, 4, pc);

        /* Status badge */
        const char *sl = status_label(st);
        int slw = TXTW(sl, 13) + 16;
        Rectangle badge = {(float)(cx + 14), (float)(cy + 92),
                           (float)slw, 24};
        DrawRectangleRounded(badge, 0.4f, 6,
            (Color){sc.r, sc.g, sc.b, 30});
        DrawRectangleRoundedLines(badge, 0.4f, 6, sc);
        TXT(sl, (int)badge.x + 8, (int)badge.y + 5, 13, sc);

        /* Wait / use time */
        char tstr[48];
        if (snap[i].status == SS_WAITING || snap[i].status == SS_ACQUIRED)
            snprintf(tstr, sizeof(tstr), "wait %.0fs  use %ds",
                     snap[i].wait_time, snap[i].use_duration);
        else if (snap[i].status == SS_RELEASED)
            snprintf(tstr, sizeof(tstr), "waited %.0fs  used %ds",
                     snap[i].wait_time, snap[i].use_duration);
        else
            tstr[0] = '\0';
        if (tstr[0])
            TXT(tstr, cx + 14, cy + 132, 12, C_MUTED);

        (void)now;
    }
}

/* ── TAB 3: Statistics ──────────────────────────────────────────────────── */
static void draw_stats(int y0) {
    int pad = PANEL_PAD;
    TXT("Simulation Statistics", pad, y0 + 10, 20, C_TEXT);

    pthread_mutex_lock(&stats.stats_mutex);
    Stats snap = stats;
    pthread_mutex_unlock(&stats.stats_mutex);

    pthread_mutex_lock(&pool.pool_mutex);
    int avail[3], total_u[3], inuse[3];
    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        avail[i]   = pool.available_units[i];
        total_u[i] = pool.total_units[i];
        inuse[i]   = pool.in_use[i];
    }
    pthread_mutex_unlock(&pool.pool_mutex);

    /* Table header */
    int tx = pad, ty = y0 + 48;
    int col_w[] = {160, 110, 110, 110, 130};
    const char *headers[] = {"Resource","Requests","Granted","Waited","Avg Wait(s)"};
    Color hcol = C_MUTED;
    int hx = tx;
    for (int c = 0; c < 5; c++) {
        TXT(headers[c], hx + 8, ty, 14, hcol);
        hx += col_w[c];
    }

    /* divider */
    DrawLine(tx, ty + 22, WIN_W - pad, ty + 22, C_BORDER);

    Color rcols[3] = {C_ACCENT, C_PURPLE, C_GREEN};
    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        int ry = ty + 32 + i * 52;
        Rectangle row = {(float)tx, (float)ry,
                         (float)(WIN_W - pad * 2), 44};
        draw_rect_rounded_border(row, 0.05f, C_PANEL2, C_BORDER);

        double avg = snap.total_waited[i] > 0
                   ? snap.total_wait_time[i] / snap.total_waited[i]
                   : 0.0;
        char vals[5][32];
        snprintf(vals[0], 32, "%s", resource_names[i]);
        snprintf(vals[1], 32, "%d",  snap.total_requests[i]);
        snprintf(vals[2], 32, "%d",  snap.total_granted[i]);
        snprintf(vals[3], 32, "%d",  snap.total_waited[i]);
        snprintf(vals[4], 32, "%.2f", avg);

        int vx = tx;
        for (int c = 0; c < 5; c++) {
            Color vc = (c == 0) ? rcols[i] : C_TEXT;
            TXT(vals[c], vx + 8, ry + 13, 15, vc);
            vx += col_w[c];
        }
    }

    /* Conflict count */
    int cy2 = ty + 32 + 3 * 52 + pad;
    Rectangle cr = {(float)pad, (float)cy2, 320, 56};
    draw_rect_rounded_border(cr, 0.08f, C_PANEL2, C_BORDER);
    TXT("Total Conflicts", pad + 14, cy2 + 10, 14, C_MUTED);
    char cstr[16]; snprintf(cstr, sizeof(cstr), "%d", snap.conflicts_detected);
    TXT(cstr, pad + 14, cy2 + 28, 20,
             snap.conflicts_detected > 0 ? C_RED : C_GREEN);

    /* Utilisation */
    int uy = cy2;
    int uw = WIN_W - pad * 2 - 340;
    Rectangle ur = {(float)(pad + 340), (float)uy, (float)uw, 56};
    draw_rect_rounded_border(ur, 0.08f, C_PANEL2, C_BORDER);
    TXT("Resource Utilisation at end", pad + 354, uy + 10, 14, C_MUTED);
    char ustr[128] = "";
    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "%s: %d/%d  ",
                 resource_names[i], inuse[i], total_u[i]);
        strncat(ustr, tmp, sizeof(ustr) - strlen(ustr) - 1);
        (void)avail[i];
    }
    TXT(ustr, pad + 354, uy + 30, 14, C_TEXT);
}

/* ── TAB 4: Event Log ───────────────────────────────────────────────────── */
static int log_scroll = 0;

static void draw_log_tab(int y0) {
    int pad = PANEL_PAD;
    TXT("Event Log", pad, y0 + 10, 20, C_TEXT);

    /* clipping region */
    int lx = pad, ly = y0 + 44;
    int lw = WIN_W - pad * 2, lh = WIN_H - ly - pad;
    int line_h = 22;
    int visible = lh / line_h;

    pthread_mutex_lock(&gui_log.mu);
    int count = gui_log.count;

    /* scroll with mouse wheel */
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        log_scroll -= (int)wheel * 3;
        if (log_scroll < 0) log_scroll = 0;
        int max_scroll = count - visible;
        if (max_scroll < 0) max_scroll = 0;
        if (log_scroll > max_scroll) log_scroll = max_scroll;
    }

    /* auto-scroll to bottom if not scrolled up */
    int max_scroll = count - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (log_scroll >= max_scroll) log_scroll = max_scroll;

    BeginScissorMode(lx, ly, lw, lh);

    for (int i = 0; i < visible && i + log_scroll < count; i++) {
        int line_idx = (gui_log.head - count + log_scroll + i + MAX_LOG_LINES)
                       % MAX_LOG_LINES;
        const char *line = gui_log.lines[line_idx];

        /* colour by prefix */
        Color lc = C_TEXT;
        if (strncmp(line, "[CONFLICT", 9) == 0) lc = C_RED;
        else if (strncmp(line, "[ACQUIRED", 9) == 0) lc = C_GREEN;
        else if (strncmp(line, "[WAITING", 8) == 0 ||
                 strncmp(line, "[QUEUED",  7) == 0) lc = C_YELLOW;
        else if (strncmp(line, "[RELEASED", 9) == 0) lc = C_MUTED;
        else if (strncmp(line, "[GRANTED",  8) == 0) lc = C_PURPLE;
        else if (strncmp(line, "[REQUEST",  8) == 0) lc = C_ACCENT;

        TXT(line, lx + 4, ly + i * line_h, 15, lc);
    }

    EndScissorMode();
    pthread_mutex_unlock(&gui_log.mu);

    /* scroll hint */
    if (count > visible) {
        char sh[32]; snprintf(sh, sizeof(sh), "scroll ↑↓  (%d lines)", count);
        TXT(sh, WIN_W - TXTW(sh,13) - pad,
                 y0 + 14, 13, C_MUTED);
    }
}

/* ── resource cycle (shared between runs) ──────────────────────────────── */
static const int resource_cycle[NUM_SESSIONS] = {
    RESOURCE_PROJECTOR, RESOURCE_COMP_LAB,   RESOURCE_MULTIMEDIA,
    RESOURCE_PROJECTOR, RESOURCE_PROJECTOR,  RESOURCE_COMP_LAB,
    RESOURCE_MULTIMEDIA, RESOURCE_COMP_LAB,  RESOURCE_PROJECTOR,
    RESOURCE_MULTIMEDIA, RESOURCE_PROJECTOR, RESOURCE_COMP_LAB
};

/* ── sim_reset: tear down previous run state, rebuild everything fresh ── */
static pthread_t sim_threads[NUM_SESSIONS];

static void sim_reset(void) {
    /* 1. Tear down subsystems from previous run.
     *    On first call, skip destroy — structs are zero-init and not yet live. */
    static int first_run = 1;
    if (!first_run) {
        scheduler_destroy(&wq);
        stats_destroy(&stats);
        resource_pool_destroy(&pool);
        logger_close();
    }
    first_run = 0;

    /* 2. Clear GUI log */
    pthread_mutex_lock(&gui_log.mu);
    gui_log.head  = 0;
    gui_log.count = 0;
    pthread_mutex_unlock(&gui_log.mu);
    log_scroll = 0;

    /* 3. Reset conflict flash timers */
    for (int i = 0; i < NUM_SESSIONS; i++)
        conflict_flash[i] = 0.0;

    /* 4. Re-init subsystems */
    logger_init(LOG_FILE);
    resource_pool_init(&pool, PROJECTOR_UNITS, COMP_LAB_UNITS, MULTIMEDIA_UNITS);
    stats_init(&stats);
    scheduler_init(&wq, &stats);

    /* 5. Re-randomise session priorities, reset status */
    srand((unsigned)time(NULL));
    pthread_mutex_lock(&sess_mu);
    for (int i = 0; i < NUM_SESSIONS; i++) {
        sessions[i].id            = i + 1;
        sessions[i].resource_type = resource_cycle[i];
        sessions[i].priority      = (rand() % MAX_PRIORITY) + MIN_PRIORITY;
        sessions[i].status        = SS_IDLE;
        sessions[i].wait_time     = 0;
        sessions[i].use_duration  = 0;
        sessions[i].in_conflict   = 0;
    }
    pthread_mutex_unlock(&sess_mu);

    /* 6. Mark sim as running */
    sim_done = 0;

    /* 7. Launch session threads with stagger */
    for (int i = 0; i < NUM_SESSIONS; i++) {
        ThreadArg *ta = malloc(sizeof(ThreadArg));
        ta->idx = i;
        pthread_create(&sim_threads[i], NULL, session_thread, ta);
        usleep(400000); /* 400ms stagger — spread sessions out visibly */
    }
}

/* ── Main ───────────────────────────────────────────────────────────────── */
int main(void) {
    srand((unsigned)time(NULL));

    /* Init GUI log mutex (only once — never destroyed) */
    gui_log_init();

    /* Init Raylib window */
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(WIN_W, WIN_H, "Smart Classroom Resource Manager");
    SetTargetFPS(60);

    /* Load font */
    G_FONT = LoadFontEx("resources/fonts/JetBrainsMono-Regular.ttf", 32, NULL, 0);
    if (G_FONT.texture.id == 0)
        G_FONT = LoadFontEx("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                            32, NULL, 0);
    if (G_FONT.texture.id == 0)
        G_FONT = GetFontDefault();
    SetTextureFilter(G_FONT.texture, TEXTURE_FILTER_BILINEAR);

    /* First run — initialise subsystems then launch threads via sim_reset.
     * sim_reset calls scheduler_destroy/stats_destroy/etc first, but those
     * are safe on a zero-initialised struct so this is fine. */
    memset(&pool, 0, sizeof(pool));
    memset(&wq,   0, sizeof(wq));
    memset(&stats, 0, sizeof(stats));
    sim_reset();

    /* ── Render loop ──────────────────────────────────────────────────── */
    int active_tab  = 0;
    int restarting  = 0;   /* 1 while we wait for old threads to join */
    const char *tab_labels[] = {"  Dashboard  ","  Sessions  ",
                                 "  Statistics  ","  Event Log  "};
    int num_tabs = 4;

    while (!WindowShouldClose()) {

        /* ── Handle restart (R key) ───────────────────────────────────── */
        if (IsKeyPressed(KEY_R) && sim_done && !restarting) {
            /* Join all finished threads first */
            for (int i = 0; i < NUM_SESSIONS; i++)
                pthread_join(sim_threads[i], NULL);
            sim_reset();   /* resets state and launches new threads */
        }

        BeginDrawing();
        ClearBackground(C_BG);

        /* ── Tab bar ──────────────────────────────────────────────────── */
        int tab_w = WIN_W / num_tabs;
        for (int t = 0; t < num_tabs; t++) {
            if (draw_tab(t * tab_w, 0, tab_w, TAB_H,
                         tab_labels[t], active_tab == t))
                active_tab = t;
        }
        DrawLine(0, TAB_H, WIN_W, TAB_H, C_BORDER);

        int content_y = TAB_H + 4;

        /* ── Tab content ──────────────────────────────────────────────── */
        switch (active_tab) {
            case 0: draw_dashboard(content_y); break;
            case 1: draw_sessions(content_y);  break;
            case 2: draw_stats(content_y);     break;
            case 3: draw_log_tab(content_y);   break;
        }

        /* ── Check if all done ────────────────────────────────────────── */
        if (!sim_done) {
            int all_done = 1;
            pthread_mutex_lock(&sess_mu);
            for (int i = 0; i < NUM_SESSIONS; i++)
                if (sessions[i].status != SS_RELEASED) { all_done = 0; break; }
            pthread_mutex_unlock(&sess_mu);
            if (all_done) sim_done = 1;
        }

        /* ── Restart hint shown when sim is complete ──────────────────── */
        if (sim_done) {
            const char *hint = "Press R to restart simulation";
            int hw = TXTW(hint, 14);
            TXT(hint, (WIN_W - hw) / 2, WIN_H - 28, 14, C_MUTED);
        }

        EndDrawing();
    }

    /* ── Cleanup ──────────────────────────────────────────────────────── */
    for (int i = 0; i < NUM_SESSIONS; i++)
        pthread_join(sim_threads[i], NULL);

    scheduler_destroy(&wq);
    stats_print_summary(&stats, &pool);
    stats_destroy(&stats);
    resource_pool_destroy(&pool);
    logger_close();
    if (G_FONT.texture.id != 0) UnloadFont(G_FONT);
    CloseWindow();
    return 0;
}
