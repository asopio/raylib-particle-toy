/*
 * Toy Bubble Chamber Simulation
 * A minimal C/Raylib particle simulation that renders charged particle tracks
 * as strings of bubbles spiralling through a magnetic field.
 *
 * Compiles natively or to WebAssembly via Emscripten.
 */

#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── configuration ──────────────────────────────────────────────── */

#define SCREEN_W       800
#define SCREEN_H       600
#define MAX_BUBBLES    32000
#define MAX_VERTICES   64
#define MAX_TRACKS     128
#define MAX_EVENTS     32
#define BUBBLE_SPACING 3.0f
#define EVENT_INTERVAL_MIN 2.0f
#define EVENT_INTERVAL_MAX 5.0f
#define MAGNETIC_FIELD 1.0f          /* arbitrary units, controls curvature */
#define BUBBLE_LIFETIME_MIN 1.5f
#define BUBBLE_LIFETIME_MAX 4.0f
#define TRACK_MAX_LENGTH 600.0f
#define TRACK_MIN_LENGTH 150.0f

/* ── types ──────────────────────────────────────────────────────── */

typedef struct Bubble {
    Vector2 pos;
    float   radius;
    float   lifetime;  /* seconds until fully faded */
    float   age;       /* seconds since spawn */
} Bubble;

typedef struct Track {
    /* physics */
    float charge;      /* sign: +1 or -1 (0 = neutral/invisible) */
    float mass;        /* controls radius of curvature */
    float speed;       /* px / s along track                     */
    float curv_radius; /* computed: mass*speed / (charge*B)       */

    /* state */
    Vector2 pos;
    float   angle;     /* current heading (radians)               */
    float   dist;      /* total distance drawn so far             */
    float   max_dist;  /* how far this track goes                 */
    float   accum;     /* accumulator for bubble spacing          */
    bool    active;
    bool    visible;   /* false for neutral particles             */
} Track;

typedef struct Event {
    Track  tracks[6];  /* up to 6 outgoing tracks per vertex      */
    int    nTracks;
    bool   active;
} Event;

/* ── globals ────────────────────────────────────────────────────── */

static Bubble  bubbles[MAX_BUBBLES];
static int     bubbleCount  = 0;
static Event   events[MAX_EVENTS];
static int     eventCount   = 0;
static float   nextEventTimer = 0.0f;

/* ── helpers ────────────────────────────────────────────────────── */

static float RandF(float lo, float hi) {
    return lo + (float)GetRandomValue(0, 10000) / 10000.0f * (hi - lo);
}

static Color BubbleColor(float alpha) {
    /* pale blue-white tint, like real bubble chamber photos */
    unsigned char a = (unsigned char)(alpha * 200.0f);
    return (Color){ 200, 215, 255, a };
}

/* ── bubble management ──────────────────────────────────────────── */

static void SpawnBubble(Vector2 pos) {
    if (bubbleCount >= MAX_BUBBLES) return;
    Bubble *b  = &bubbles[bubbleCount++];
    b->pos     = pos;
    b->radius  = RandF(1.0f, 2.8f);
    b->lifetime = RandF(BUBBLE_LIFETIME_MIN, BUBBLE_LIFETIME_MAX);
    b->age     = 0.0f;
}

static void UpdateBubbles(float dt) {
    for (int i = 0; i < bubbleCount; ) {
        Bubble *b = &bubbles[i];
        b->age += dt;
        if (b->age >= b->lifetime) {
            bubbles[i] = bubbles[--bubbleCount];   /* swap-remove */
        } else {
            i++;
        }
    }
}

static void DrawBubbles(void) {
    for (int i = 0; i < bubbleCount; i++) {
        Bubble *b = &bubbles[i];
        float t   = b->age / b->lifetime;          /* 0 → 1 */
        float alpha = 1.0f - t * t;                /* fade curve */
        DrawCircleV(b->pos, b->radius, BubbleColor(alpha));
    }
}

/* ── track stepping ─────────────────────────────────────────────── */

static void StepTrack(Track *t, float dt) {
    if (!t->active) return;

    float step = t->speed * dt;
    Vector2 prevPos = t->pos;
    t->dist   += step;
    t->accum  += step;

    /* curve heading if charged */
    if (t->charge != 0.0f && t->curv_radius != 0.0f) {
        float next_speed = fmaxf(t->speed - 18.0f * dt, 40.0f);
        t->curv_radius = (t->mass * next_speed) /
                         (fabsf(t->charge) * MAGNETIC_FIELD);
        float dtheta = step / t->curv_radius;
        if (t->charge < 0.0f) dtheta = -dtheta;
        t->angle    += dtheta;
        t->speed     = next_speed;
    }

    /* advance position */
    t->pos.x += cosf(t->angle) * step;
    t->pos.y += sinf(t->angle) * step;

    /* spawn bubbles along the path if visible */
    if (t->visible) {
        while (t->accum >= BUBBLE_SPACING) {
            t->accum -= BUBBLE_SPACING;
            float bubbleT = (step > 0.0f) ? (step - t->accum) / step : 0.0f;
            Vector2 bubblePos = Vector2Lerp(prevPos, t->pos, bubbleT);
            /* jitter position slightly */
            Vector2 bp = {
                bubblePos.x + RandF(-1.5f, 1.5f),
                bubblePos.y + RandF(-1.5f, 1.5f)
            };
            SpawnBubble(bp);
        }
    }

    /* deactivate when max distance reached or off screen */
    if (t->dist >= t->max_dist ||
        t->pos.x < -50 || t->pos.x > SCREEN_W + 50 ||
        t->pos.y < -50 || t->pos.y > SCREEN_H + 50) {
        t->active = false;
    }
}

/* ── event (vertex) creation ────────────────────────────────────── */

/*
 * Create a physics event at a random screen position.
 * Momentum is conserved across all outgoing tracks (including invisible
 * neutral ones).  Because neutral tracks are not rendered, some vertices
 * will appear to violate conservation when only visible tracks are
 * considered.
 */
static void CreateEvent(void) {
    if (eventCount >= MAX_EVENTS) {
        /* recycle oldest finished event slot */
        for (int i = 0; i < eventCount; i++) {
            bool done = true;
            for (int j = 0; j < events[i].nTracks; j++) {
                if (events[i].tracks[j].active) { done = false; break; }
            }
            if (done) { events[i] = events[eventCount - 1]; eventCount--; break; }
        }
        if (eventCount >= MAX_EVENTS) return;
    }

    Event *ev  = &events[eventCount++];
    memset(ev, 0, sizeof(*ev));
    ev->active = true;

    Vector2 origin = {
        RandF(100, SCREEN_W - 100),
        RandF(100, SCREEN_H - 100)
    };

    /* decide topology: 2-4 visible tracks plus one neutral residual track */
    int nVisible  = GetRandomValue(2, 4);
    int nNeutral  = 1;
    ev->nTracks   = nVisible + nNeutral;
    if (ev->nTracks > 6) ev->nTracks = 6;

    /* first pass – assign random momenta to visible tracks */
    float totalPx = 0.0f, totalPy = 0.0f;

    for (int i = 0; i < nVisible; i++) {
        Track *t   = &ev->tracks[i];
        t->charge  = (GetRandomValue(0, 1) == 0) ? 1.0f : -1.0f;
        t->mass    = RandF(1.0f, 10.0f);
        t->speed   = RandF(120.0f, 280.0f);
        t->angle   = RandF(0, 2.0f * PI);
        t->pos     = origin;
        t->dist    = 0.0f;
        t->max_dist = RandF(TRACK_MIN_LENGTH, TRACK_MAX_LENGTH);
        t->accum   = 0.0f;
        t->active  = true;
        t->visible = true;

        /* curvature: R = m*v / (|q|*B); sign of charge controls direction */
        t->curv_radius = (t->mass * t->speed) /
                         (fabsf(t->charge) * MAGNETIC_FIELD);

        float px = t->mass * t->speed * cosf(t->angle);
        float py = t->mass * t->speed * sinf(t->angle);
        totalPx += px;
        totalPy += py;
    }

    /* neutral track carries the leftover momentum so conservation holds */
    if (nNeutral > 0) {
        float neutralPx = -totalPx / nNeutral;
        float neutralPy = -totalPy / nNeutral;
        float neutralP  = sqrtf(neutralPx * neutralPx + neutralPy * neutralPy);
        for (int i = nVisible; i < ev->nTracks; i++) {
            Track *t   = &ev->tracks[i];
            t->charge  = 0.0f;
            t->speed   = RandF(120.0f, 280.0f);
            t->mass    = (neutralP > 0.0f) ? neutralP / t->speed : 0.0f;
            t->angle   = atan2f(neutralPy, neutralPx);
            t->pos     = origin;
            t->dist    = 0.0f;
            t->max_dist = (neutralP > 0.0f) ? RandF(TRACK_MIN_LENGTH, TRACK_MAX_LENGTH) : 0.0f;
            t->accum   = 0.0f;
            t->active  = (neutralP > 0.0f);
            t->visible = false;  /* invisible neutral track */
            t->curv_radius = 0.0f;
        }
    }
}

/* ── main loop ──────────────────────────────────────────────────── */

#if defined(PLATFORM_WEB)
    #include <emscripten/emscripten.h>
#endif

static void UpdateDrawFrame(void);

int main(void) {
    InitWindow(SCREEN_W, SCREEN_H, "Bubble Chamber");
    SetTargetFPS(60);

    nextEventTimer = RandF(0.5f, 1.5f);  /* first event soon */

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(UpdateDrawFrame, 0, 1);
#else
    while (!WindowShouldClose()) {
        UpdateDrawFrame();
    }
#endif

    CloseWindow();
    return 0;
}

static void UpdateDrawFrame(void) {
    float dt = GetFrameTime();

    /* ── periodic event spawning ── */
    nextEventTimer -= dt;
    if (nextEventTimer <= 0.0f) {
        CreateEvent();
        nextEventTimer = RandF(EVENT_INTERVAL_MIN, EVENT_INTERVAL_MAX);
    }

    /* ── update tracks ── */
    for (int e = 0; e < eventCount; e++) {
        for (int t = 0; t < events[e].nTracks; t++) {
            StepTrack(&events[e].tracks[t], dt);
        }
    }

    /* ── update bubbles ── */
    UpdateBubbles(dt);

    /* ── draw ── */
    BeginDrawing();
        ClearBackground((Color){ 10, 12, 18, 255 });   /* dark blue-black */
        DrawBubbles();
        DrawText("Bubble Chamber", 10, 10, 16,
                 (Color){ 100, 110, 140, 180 });
    EndDrawing();
}
