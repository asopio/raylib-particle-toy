/*
 * Toy Bubble Chamber Simulation (3D)
 * A minimal C/Raylib particle simulation that renders charged particle
 * tracks as strings of bubbles spiralling through a magnetic field.
 *
 * The simulation takes place in a 3D box.  The screen is the x-z plane (x
 * horizontal, z vertical) and the y-axis points into/out of the screen.  A
 * uniform magnetic field points along the y-axis - perpendicular to the
 * screen plane.  Charged particles feel the real Lorentz force F = q (v x B),
 * so their tracks curve in the x-y plane with radius R = m v / (|q| B) -
 * evaluated from the cross product each physics step, not baked in.
 *
 * The camera sits above the box (y = 0) and looks down +y into it, and slowly
 * drifts along x,
 * so the parallax between near and far tracks is visible.  The world wraps
 * horizontally (x): tracks and their bubbles leaving the left edge reappear
 * on the right.  Bubbles fade with their distance from the viewer
 * (perspective depth cue).
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

/* simulation box (world units).  Screen plane is x-z; +y goes into the box */
#define WORLD_X0      -400.0f     /* left wrapping edge  */
#define WORLD_X1       400.0f     /* right wrapping edge */
#define WORLD_Y0        40.0f     /* near plane (close to camera) */
#define WORLD_Y1       600.0f     /* far plane (depth)           */
#define WORLD_Z0      -300.0f     /* bottom of screen            */
#define WORLD_Z1       300.0f     /* top of screen               */

/* camera: sits above the box at y=0 and looks down +y; the x-z plane (y=0)
   is the screen, x scrolls horizontally */
#define CAM_Y          0.0f      /* camera height (above the box)        */
#define CAM_SPEED      20.0f     /* px/s horizontal (x) scroll speed     */

/* perspective depth fading:  alpha = 1/(1 + k * (dist/CAM_DEPTH)^2) */
#define CAM_DEPTH     500.0f     /* reference distance for the fade curve */
#define DEPTH_FADE_K      1.2f
#define MIN_FADE          0.12f   /* floor for very far bubbles        */
#define EVENT_INTERVAL_MIN 2.0f
#define EVENT_INTERVAL_MAX 5.0f
/* uniform magnetic field: B = (0, +B, 0) - along the y axis */
#define B_MAGNETIC      1.0f
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
    float   charge;    /* sign: +1 or -1 (0 = neutral/invisible) */
    float   mass;      /* controls radius of curvature          */
    Vector3 vel;       /* velocity (px/s); |vel| = speed        */

    /* state */
    Vector3 pos;
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
static float   camX = 0.0f;   /* camera's world-x position (scrolls linearly, never wraps) */

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
 * Camera-relative x: the world is periodic in x with period WORLD_DX, so for
 * any effect (culling, the reference plane, ...) we only need each object's
 * x offset from the camera, folded into one wrapping cell.  Positive result
 * means the object is to the camera's right.
 */
static float CamRelX(float worldX) {
    float d = WrapX(worldX - camX) - WORLD_X0;   /* 0 .. WORLD_DX           */
    d -= 0.5f * WORLD_DX;                        /* -DX/2 .. +DX/2          */
    return (d > 0.5f * WORLD_DX) ? d - WORLD_DX : d;
}

/*
 * Perspective distance fading.
 * dist is the bubble's straight-line distance from the camera.
 * Returns 1.0 at the reference plane (dist = CAM_DEPTH), falling off with
 * distance squared.
 */
static float DepthFade(float dist) {
    /* dist = bubble's distance from the camera.  1.0 near the reference
       plane (dist = CAM_DEPTH), falling off with distance squared. */
    float d = fabsf(dist) / CAM_DEPTH;
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

        /* depth fading: straight-line distance from the camera */
        Vector3 toCam = Vector3Subtract(b->pos, cam.position);
        float distCam = Vector3Length(toCam);
        float alpha = lifeAlpha * DepthFade(distCam);

        Vector2 sp = GetWorldToScreen(b->pos, cam);
        DrawCircleV(sp, b->radius, BubbleColor(alpha));
    }
}

/* ── track stepping ─────────────────────────────────────────────── */

static void StepTrack(Track *t, float dt) {
    if (!t->active) return;

    Vector3 prevPos = t->pos;
    float speed = Vector3Length(t->vel);
    float step  = speed * dt;
    t->dist   += step;
    t->accum  += step;

    if (t->charge != 0.0f) {
        /*
         * Lorentz force:  F = q (v x B).  With B = (0, B, 0):
         *     v x B = | i  j  k |
         *             |vx vy vz|
         *             | 0 B  0 |
         *           = (-vy*B, 0, vx*B)
         *     F/m   = (qB/m) * (-vy, 0, vx)
         *
         * The force is perpendicular to v (it only bends the track, it
         * never changes the speed), so we integrate semi-implicitly: add
         * the force to the velocity, then keep the magnitude constant.
         */
        Vector3 B = { 0.0f, B_MAGNETIC, 0.0f };
        Vector3 cross = Vector3CrossProduct(t->vel, B);      /* v x B     */
        Vector3 acc   = Vector3Scale(cross, t->charge / t->mass); /* F/m = a */
        t->vel        = Vector3Add(t->vel, Vector3Scale(acc, dt));
        t->vel        = Vector3Normalize(t->vel);           /* |v| = 1   */
        t->vel        = Vector3Scale(t->vel, speed);        /* |v| = speed */
    }

    /* advance position along (possibly bent) velocity */
    t->pos = Vector3Add(t->pos, Vector3Scale(t->vel, dt));
    WrapPos(&t->pos);

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

    /* origin: anywhere in the box */
    Vector3 origin = {
        WrapX(RandF(WORLD_X0, WORLD_X0 + WORLD_DX)),   /* x: horizontal, wraps */
        RandF(WORLD_Y0, WORLD_Y1),                     /* y: depth into box */
        RandF(WORLD_Z0, WORLD_Z1)                      /* z: vertical on screen */
    };

    /* decide topology: 2-4 visible tracks plus one neutral residual track */
    int nVisible  = GetRandomValue(2, 4);
    int nNeutral  = 1;
    ev->nTracks   = nVisible + nNeutral;
    if (ev->nTracks > 6) ev->nTracks = 6;

    /* first pass – assign random 3D momenta to the visible tracks */
    Vector3 totalP = { 0.0f, 0.0f, 0.0f };

    for (int i = 0; i < nVisible; i++) {
        Track *t   = &ev->tracks[i];
        t->charge  = (GetRandomValue(0, 1) == 0) ? 1.0f : -1.0f;
        t->mass    = RandF(1.0f, 10.0f);
        float speed = RandF(120.0f, 280.0f);

        /* random unit direction */
        Vector3 dir = Vector3Normalize((Vector3){ RandF(-1, 1), RandF(-1, 1), RandF(-1, 1) });
        if (Vector3Length(dir) < 0.5f) dir = (Vector3){ 1.0f, 0.0f, 0.0f };
        t->vel = Vector3Scale(dir, speed);

        t->pos      = origin;
        t->dist     = 0.0f;
        t->max_dist = RandF(TRACK_MIN_LENGTH, TRACK_MAX_LENGTH);
        t->accum    = 0.0f;
        t->active   = true;
        t->visible  = true;

        /* momentum p = m v; accumulate for conservation */
        Vector3 p = Vector3Scale(t->vel, t->mass);
        totalP   = Vector3Add(totalP, p);
    }

    /* neutral track carries the leftover momentum so conservation holds */
    if (nNeutral > 0) {
        Vector3 neutralP = Vector3Scale(totalP, -1.0f / nNeutral);
        float neutralSpeed = Vector3Length(neutralP);
        for (int i = nVisible; i < ev->nTracks; i++) {
            Track *t   = &ev->tracks[i];
            t->charge  = 0.0f;
            t->mass    = 1.0f;
            t->vel     = neutralP;                 /* |vel| = speed (m = 1) */
            t->pos     = origin;
            t->dist    = 0.0f;
            t->max_dist = (neutralSpeed > 0.0f) ? RandF(TRACK_MIN_LENGTH, TRACK_MAX_LENGTH) : 0.0f;
            t->accum   = 0.0f;
            t->active  = (neutralSpeed > 0.0f);
            t->visible = false;                    /* invisible neutral track */
        }
    }
}

/* ── camera ─────────────────────────────────────────────────────── */

static void UpdateCameraPos(float dt) {
    camX += CAM_SPEED * dt;   /* scroll linearly: the camera never wraps, so the
                                 periodic world keeps scrolling past it forever */

    /* looks down +y into the box: x is screen-right, z is screen-up */
    cam = (Camera3D){
        .position = (Vector3){ camX, CAM_Y, 0.0f },
        .target   = (Vector3){ camX, 300.0f, 0.0f },   /* look down +y */
        .up       = (Vector3){ 0.0f, 0.0f, 1.0f },     /* world +z = screen up */
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
            /* far reference plane (depth cue): one copy per wrapping cell that
               can be on screen, placed at the camera-relative x so the world
               looks seamless as it scrolls past */
            float halfH = (float)SCREEN_H * tanf(Radians(25.0f)) * 300.0f;
            float halfW = (float)SCREEN_W * tanf(Radians(25.0f)) * 300.0f;
            for (int k = -2; k <= 2; k++) {
                float px = WORLD_X0 + k * WORLD_DX + CamRelX(0.0f);
                if (px < -halfW || px > halfW + WORLD_DX) continue;
                DrawPlane((Vector3){ px, WORLD_Y1, 0 },
                          (Vector2){ WORLD_DX + 400.0f, WORLD_Z1 - WORLD_Z0 + 400.0f },
                          (Color){ 20, 24, 36, 255 });
            }
        EndMode3D();

        DrawBubbles();
        DrawText("Bubble Chamber 3D", 10, 10, 16,
                 (Color){ 100, 110, 140, 180 });
    EndDrawing();
}
