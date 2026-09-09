/*
 * Toy Bubble Chamber Simulation (3D)
 * A minimal C/Raylib particle simulation that renders charged particle
 * tracks as strings of bubbles spiralling through a magnetic field.
 *
 * The simulation takes place in a 3D box that wraps horizontally: tracks
 * (and their bubbles) leaving the left edge reappear on the right.  The
 * camera slowly drifts along the horizontal axis so that the parallax
 * between near and far tracks is visible.  Bubbles fade with distance
 * from the viewer (perspective depth cue).
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
#define MAX_BUBBLES    48000
#define MAX_TRACKS     128
#define MAX_EVENTS     32
#define BUBBLE_SPACING 3.0f

/* simulation box (world units) */
#define WORLD_X0      -400.0f     /* left wrapping edge  */
#define WORLD_X1       400.0f     /* right wrapping edge */
#define WORLD_Z0       -40.0f     /* near plane-ish      */
#define WORLD_Z1       600.0f     /* far plane           */
#define WORLD_Y0        -60.0f
#define WORLD_Y1        340.0f

/* camera */
#define CAM_DISTANCE    120.0f    /* camera sits this far in front of z=0 */
#define CAM_SPEED        20.0f    /* px/s horizontal scroll speed        */

/* perspective depth fading:  alpha = 1/(1 + k * (z/cam_z - 1)^2) */
#define DEPTH_FADE_K      1.2f
#define MIN_FADE          0.12f   /* floor for very far bubbles        */

#define EVENT_INTERVAL_MIN 2.0f
#define EVENT_INTERVAL_MAX 5.0f
#define MAGNETIC_FIELD 1.0f          /* arbitrary units, controls curvature */
#define BUBBLE_LIFETIME_MIN 1.5f
#define BUBBLE_LIFETIME_MAX 4.0f
#define TRACK_MAX_LENGTH 600.0f
#define TRACK_MIN_LENGTH 150.0f
#define TRACK_DECELERATION 18.0f
#define TRACK_MIN_SPEED 40.0f

/* fixed-timestep physics */
#define PHYSICS_DT      (1.0f / 120.0f)  /* 120 Hz physics substeps       */
#define MAX_FRAME_DT    (1.0f / 15.0f)   /* clamp: ignore >~66 ms frames  */

/* ── types ──────────────────────────────────────────────────────── */

typedef struct Bubble {
    Vector3 pos;
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
    Vector3 pos;
    float   angle;     /* current heading in the XZ plane (radians) */
    float   dy;        /* vertical drift component (px/s)           */
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
static float   trackAccum     = 0.0f;  /* fixed-timestep physics accumulator */
static Camera3D cam;
static float   camX = 0.0f;   /* camera's world-x position (scrolls) */

static const float WORLD_DX = WORLD_X1 - WORLD_X0;   /* wrapping width */

/* ── helpers ────────────────────────────────────────────────────── */

static float RandF(float lo, float hi) {
    return lo + (float)GetRandomValue(0, 10000) / 10000.0f * (hi - lo);
}

/* wrap x coordinate into [WORLD_X0, WORLD_X0 + WORLD_DX) */
static float WrapX(float x) {
    x -= WORLD_X0;
    x = x - WORLD_DX * floorf(x / WORLD_DX);
    return x + WORLD_X0;
}

/*
 * Wrap a track's position into the box.  If it jumped more than half the
 * box in one step (cannot happen with sane speeds, but be safe), snap it.
 */
static void WrapPos(Vector3 *p) {
    p->x = WrapX(p->x);
    if (p->y < WORLD_Y0) p->y = WORLD_Y0;
    if (p->y > WORLD_Y1) p->y = WORLD_Y1;
    if (p->z < WORLD_Z0) p->z = WORLD_Z0;
    if (p->z > WORLD_Z1) p->z = WORLD_Z1;
}

/*
 * Perspective distance fading.
 * zRel is the bubble's distance in front of the camera (its world z).
 * Returns 1.0 at the camera plane, falling off with distance squared.
 */
static float DepthFade(float zRel) {
    /* zRel is the bubble's distance in front of the camera plane (z=0 at
       the near reference, z=CAM_DISTANCE is roughly one "unit" away). */
    float d = fabsf(zRel / CAM_DISTANCE);
    float f = 1.0f / (1.0f + DEPTH_FADE_K * d * d);
    return f < MIN_FADE ? MIN_FADE : f;
}

static Color BubbleColor(float alpha) {
    /* pale blue-white tint, like real bubble chamber photos */
    if (alpha > 1.0f) alpha = 1.0f;
    unsigned char a = (unsigned char)(alpha * 230.0f);
    return (Color){ 200, 215, 255, a };
}

/* ── bubble management ──────────────────────────────────────────── */

static void SpawnBubble(Vector3 pos) {
    if (bubbleCount >= MAX_BUBBLES) return;
    WrapPos(&pos);
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
        float lifeAlpha = 1.0f - t * t;            /* fade curve */

        /* depth fading: distance in front of the camera plane (z=0) */
        float zRel = b->pos.z;               /* 0 = near, CAM_DISTANCE = far */
        float alpha = lifeAlpha * DepthFade(zRel);

        Vector2 sp = GetWorldToScreen(b->pos, cam);
        DrawCircleV(sp, b->radius, BubbleColor(alpha));
    }
}

/* ── track stepping ─────────────────────────────────────────────── */

static void StepTrack(Track *t, float dt) {
    if (!t->active) return;

    float angle = t->angle;

    if (t->charge != 0.0f) {
        float next_speed = fmaxf(t->speed - TRACK_DECELERATION * dt,
                                 TRACK_MIN_SPEED);
        t->curv_radius = (t->mass * next_speed) /
                         (fabsf(t->charge) * MAGNETIC_FIELD);
        t->speed     = next_speed;
    }

    float step = t->speed * dt;
    Vector3 prevPos = t->pos;
    t->dist   += step;
    t->accum  += step;

    /* curve heading if charged (spiral in the XZ plane) */
    if (t->charge != 0.0f) {
        float dtheta = step / t->curv_radius;
        if (t->charge < 0.0f) dtheta = -dtheta;
        t->angle    += dtheta;
    }

    /* advance position */
    t->pos.x += cosf(angle) * step;
    t->pos.z += sinf(angle) * step;
    t->pos.y += t->dy * dt;

    /* spawn bubbles along the path if visible */
    if (t->visible) {
        while (t->accum >= BUBBLE_SPACING) {
            t->accum -= BUBBLE_SPACING;
            float bubbleT = (step > 0.0f) ? (step - t->accum) / step : 0.0f;
            Vector3 bubblePos = Vector3Lerp(prevPos, t->pos, bubbleT);
            /* jitter position slightly */
            Vector3 bp = {
                bubblePos.x + RandF(-1.5f, 1.5f),
                bubblePos.y + RandF(-1.5f, 1.5f),
                bubblePos.z + RandF(-1.5f, 1.5f)
            };
            SpawnBubble(bp);
        }
    }

    /* deactivate when max distance reached */
    if (t->dist >= t->max_dist) {
        t->active = false;
    }
}

/* ── event (vertex) creation ────────────────────────────────────── */

/*
 * Create a physics event at a random position inside the 3D box.
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

    /* origin: biased toward the camera side so new events are seen */
    Vector3 origin = {
        WrapX(RandF(WORLD_X0, WORLD_X0 + WORLD_DX)),
        RandF(60.0f, 280.0f),
        RandF(0.0f, WORLD_Z1)
    };

    /* decide topology: 2-4 visible tracks plus one neutral residual track */
    int nVisible  = GetRandomValue(2, 4);
    int nNeutral  = 1;
    ev->nTracks   = nVisible + nNeutral;
    if (ev->nTracks > 6) ev->nTracks = 6;

    /* first pass – assign random momenta to visible tracks */
    float totalPx = 0.0f, totalPz = 0.0f;

    for (int i = 0; i < nVisible; i++) {
        Track *t   = &ev->tracks[i];
        t->charge  = (GetRandomValue(0, 1) == 0) ? 1.0f : -1.0f;
        t->mass    = RandF(1.0f, 10.0f);
        t->speed   = RandF(120.0f, 280.0f);
        t->angle   = RandF(0, 2.0f * PI);   /* heading in XZ plane */
        t->dy      = RandF(-30.0f, 30.0f);  /* gentle vertical drift */
        t->pos     = origin;
        t->dist    = 0.0f;
        t->max_dist = RandF(TRACK_MIN_LENGTH, TRACK_MAX_LENGTH);
        t->accum   = 0.0f;
        t->active  = true;
        t->visible = true;

        /* curvature: R = m*v / (|q|*B); sign of charge controls direction */
        t->curv_radius = (t->mass * t->speed) /
                         (fabsf(t->charge) * MAGNETIC_FIELD);

        /* momentum in the XZ plane */
        float px = t->mass * t->speed * cosf(t->angle);
        float pz = t->mass * t->speed * sinf(t->angle);
        totalPx += px;
        totalPz += pz;
    }

    /* neutral track carries the leftover momentum so conservation holds */
    if (nNeutral > 0) {
        float neutralPx = -totalPx / nNeutral;
        float neutralPz = -totalPz / nNeutral;
        float neutralP  = sqrtf(neutralPx * neutralPx + neutralPz * neutralPz);
        for (int i = nVisible; i < ev->nTracks; i++) {
            Track *t   = &ev->tracks[i];
            t->charge  = 0.0f;
            t->speed   = RandF(120.0f, 280.0f);
            t->mass    = (neutralP > 0.0f) ? neutralP / t->speed : 0.0f;
            t->angle   = atan2f(neutralPz, neutralPx);
            t->dy      = RandF(-30.0f, 30.0f);
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

/* ── camera ─────────────────────────────────────────────────────── */

static void UpdateCameraPos(float dt) {
    camX += CAM_SPEED * dt;
    camX = WrapX(camX);   /* camera itself wraps, keeping it inside the box */

    cam = (Camera3D){
        .position = (Vector3){ camX, 180.0f, CAM_DISTANCE },
        .target   = (Vector3){ camX, 140.0f, 0.0f },
        .up       = (Vector3){ 0.0f, 1.0f, 0.0f },
        .projection = CAMERA_PERSPECTIVE,
        .fovy       = 50.0f
    };
}

/* ── main loop ──────────────────────────────────────────────────── */

#if defined(PLATFORM_WEB)
    #include <emscripten/emscripten.h>
#endif

static void UpdateDrawFrame(void);

int main(void) {
    SetConfigFlags(FLAG_VSYNC_HINT);
    InitWindow(SCREEN_W, SCREEN_H, "Bubble Chamber 3D");
    SetTargetFPS(60);

    UpdateCameraPos(0.0f);
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

    /* ── fixed-timestep physics ── */
    trackAccum += dt;
    if (trackAccum > MAX_FRAME_DT) trackAccum = MAX_FRAME_DT;
    while (trackAccum >= PHYSICS_DT) {
        /* periodic event spawning */
        nextEventTimer -= PHYSICS_DT;
        if (nextEventTimer <= 0.0f) {
            CreateEvent();
            nextEventTimer = RandF(EVENT_INTERVAL_MIN, EVENT_INTERVAL_MAX);
        }

        /* update tracks */
        for (int e = 0; e < eventCount; e++) {
            for (int t = 0; t < events[e].nTracks; t++) {
                StepTrack(&events[e].tracks[t], PHYSICS_DT);
            }
        }

        /* update bubbles */
        UpdateBubbles(PHYSICS_DT);

        trackAccum -= PHYSICS_DT;
    }

    /* ── update camera (slow horizontal drift => parallax) ── */
    UpdateCameraPos(dt);

    /* ── draw ── */
    BeginDrawing();
        ClearBackground((Color){ 10, 12, 18, 255 });   /* dark blue-black */
        BeginMode3D(cam);
            /* floor plane as a depth reference */
            DrawPlane((Vector3){ 0, -10, 300 }, (Vector2){ 2200, 1000 },
                      (Color){ 25, 30, 45, 255 });
        EndMode3D();

        DrawBubbles();
        DrawText("Bubble Chamber 3D", 10, 10, 16,
                 (Color){ 100, 110, 140, 180 });
    EndDrawing();
}
