/*******************************************************************************
*   SHOTCOIL - Recoil Shooter
*
*   There is no move button. You shoot, and the recoil throws you.
*   Aiming IS movement.
*
*   Single translation unit, zero asset files (shapes + synthesized audio only)
*   so the whole game stays far under the 1.44MB contest budget.
*******************************************************************************/

#include "raylib.h"
#include "raymath.h"

#include <math.h>
#include <stdio.h>      /* snprintf, for the popup labels */
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------*/
/* Configuration                                                              */
/*                                                                            */
/* The whole game is authored in a fixed 1280x720 (16:9) virtual space. Every  */
/* position, radius and font size below is in those units. At draw time the    */
/* virtual space is scaled to fill the real window and letterboxed, so the     */
/* layout is pixel-identical on a 16:10 laptop panel and a 16:9 monitor alike. */
/*----------------------------------------------------------------------------*/
#define SCREEN_W        1280
#define SCREEN_H        720
#define GROUND_Y        (SCREEN_H - 56)

#define MAX_BULLETS     256
#define MAX_ENEMIES     96
#define MAX_PARTICLES   1200
#define MAX_POPUPS      32
#define MAX_SPAWNQUEUE  64

#define PLAYER_RADIUS   16.0f
#define PLAYER_MAX_HP   5

/* Floor heat. The band is how high above the floor still counts as "down
   there"; skimming inside it bakes at SKIM_RATE, and heat only drains once you
   are above it - slower than it fills, so a quick hop never wipes the gauge. */
#define GROUND_HEAT_BAND  60.0f
#define GROUND_SKIM_RATE  0.60f
#define GROUND_COOL_RATE  0.75f

/* The waves never stop. There is no clear condition - a run ends when you do,
   and the only score that matters is how deep you got. */

#define SAVE_FILE       "shotcoil.sav"

/*----------------------------------------------------------------------------*/
/* Tunables - the five (six) numbers that decide whether the game feels good.  */
/*----------------------------------------------------------------------------*/
typedef struct Tunables {
    float recoilImpulse;    /* how hard one shot throws you            */
    float gravity;          /* how fast you fall                       */
    float airDrag;          /* per-60th-of-a-second velocity retention  */
    float fireCooldown;     /* seconds between shots - the flight throttle */
    float groundTime;       /* floor-heat capacity: seconds of standing to erupt */
    float mouseSens;        /* only used while the cursor is locked - see below */
} Tunables;

static const Tunables TUNE_DEFAULT = {
    460.0f,     /* recoilImpulse */
    900.0f,     /* gravity       */
    0.990f,     /* airDrag       */
    0.32f,      /* fireCooldown  */
    1.2f,       /* groundTime    */
    2.0f        /* mouseSens     */
};

static Tunables tune;

/*----------------------------------------------------------------------------*/
/* Weapons                                                                    */
/*                                                                            */
/* A weapon is an engine as much as a gun: the recoil multiplier is your       */
/* thrust per shot and the fire interval is your throttle. Picked up weapons   */
/* are kept until you die.                                                     */
/*----------------------------------------------------------------------------*/
typedef enum WeaponType {
    WP_PISTOL, WP_SMG, WP_SWORD, WP_SHOTGUN, WP_RAILGUN, WP_GRENADE, WP_BAZOOKA,
    WP_FLAMER, WP_RICOCHET, WP_HARPOON, WP_LASER, WP_COUNT
} WeaponType;

typedef struct WeaponDef {
    const char *name;
    float recoilMul;     /* thrust per shot, relative to RECOIL_IMPULSE  */
    float cooldownMul;   /* fire interval, relative to FIRE_COOLDOWN     */
    int   pellets;
    float spread;        /* radians, half-angle */
    float bulletSpeed;
    float bulletRadius;
    float damage;
    float life;          /* seconds the shot lives - doubles as a grenade fuse */
    float bulletGravity; /* grenades arc; everything else flies straight */
    int   bounces;
    int   pierce;        /* enemies a shot passes through before it dies */
    bool  explosive;
    bool  hitscan;       /* railgun: an instant piercing beam, no projectile */
    bool  slash;         /* sword: a wide, very short-lived crescent */
    float gunLen, gunThick;
    Color color;
} WeaponDef;

/* With infinite ammo the fire interval is the ONLY thing rationing flight, so
   every weapon is tuned to the same sustained thrust band: recoilMul divided by
   cooldownMul stays inside 1.04-1.12 on everything except the SWORD, which is
   deliberately higher. What differs is the texture - a 0.12s tap-hover on the
   SMG versus a 0.86s cannon shove on the BAZOOKA. Nobody is strictly better;
   they just fly differently, so keep that ratio when adding a weapon.        */
static const WeaponDef WEAPONS[WP_COUNT] = {
    /* Damage is set against enemy HP (chaser 2, dasher 4, splitter 4/2): the
       starting PISTOL one-shots the basic chaser, and nothing takes more than
       two hits from its natural counter. Bullets are drawn at their collision
       radius, so a bigger number is both easier to land and easier to read.  */
    /* name        recoil cool   pel spread  speed    rad   dmg  life  grav bnc prc boom hit slash len thick */
    { "권총",       0.92f, 0.86f, 1, 0.025f, 980.0f,  6.5f, 2.0f, 1.40f,   0.0f, 1, 0, false,false,false, 26.0f,10.0f,
      { 120, 240, 255, 255 } },
    { "기관단총",   0.40f, 0.36f, 1, 0.070f, 1020.0f, 4.5f, 1.0f, 1.40f,   0.0f, 0, 0, false,false,false, 30.0f, 8.0f,
      { 150, 255, 190, 255 } },
    /* The mobility weapon: full recoil at the shortest interval, so its thrust
       sits well above the others on purpose. The price is reach - the crescent
       dies after ~150px, so flying on it means diving into contact range.    */
    { "검",         1.00f, 0.72f, 1, 0.030f, 900.0f, 20.0f, 4.0f, 0.17f,   0.0f, 0, 0, false,false,true,  30.0f,13.0f,
      { 255, 250, 210, 255 } },
    { "산탄총",     1.85f, 1.65f, 5, 0.300f, 860.0f,  5.5f, 1.6f, 1.40f,   0.0f, 0, 0, false,false,false, 22.0f,15.0f,
      { 255, 205,  90, 255 } },
    { "레일건",     1.55f, 1.45f, 1, 0.000f,   0.0f,  0.0f, 5.0f, 0.00f,   0.0f, 0, 0, false,true, false, 36.0f,11.0f,
      { 210, 140, 255, 255 } },
    { "수류탄",     1.30f, 1.25f, 1, 0.040f, 620.0f,  8.0f, 4.0f, 1.30f, 900.0f, 3, 0, true, false,false, 24.0f,14.0f,
      { 170, 255, 120, 255 } },
    /* Retuned, because the usual way to die holding this thing was your own
       recoil. At 2.80 on a 0.86s interval one shot threw you 1290px/s and then
       took the controls away for most of a second: more than the width of the
       arena, with no chance to correct before you arrived. Both numbers come
       down by the same fraction, so the ratio - and with it the sustained
       thrust the whole roster is tuned against - is unchanged; what shrinks is
       the per-shot spike (943px/s) and the blind window (0.61s) with it. The
       rocket flies faster to buy back the reach the shorter fuse costs. */
    { "바주카포",   2.05f, 1.90f, 1, 0.010f, 660.0f, 10.0f, 4.2f, 2.00f,   0.0f, 0, 0, true, false,false, 32.0f,17.0f,
      { 255, 110, 200, 255 } },
    /* A cone that dies at ~120px. Enormous close-range output, and the tightest
       hover in the game - but you have to be in the crowd to use either.     */
    { "화염방사기", 0.55f, 0.50f, 5, 0.400f, 440.0f,  8.0f, 0.9f, 0.28f,   0.0f, 0, 1, false,false,false, 28.0f,12.0f,
      { 255, 150,  60, 255 } },
    /* Five wall bounces on a long fuse: the arena itself becomes the weapon,
       and shots you already forgot about keep working behind you.            */
    { "튕김총",     0.85f, 0.80f, 1, 0.050f, 900.0f,  5.5f, 2.0f, 2.60f,   0.0f, 5, 0, false,false,false, 27.0f, 9.0f,
      { 120, 255, 230, 255 } },
    /* One heavy spike, straight through a whole line of them.                */
    { "작살포",     1.20f, 1.14f, 1, 0.012f, 1350.0f, 7.0f, 3.5f, 1.40f,   0.0f, 0, 4, false,false,false, 38.0f,10.0f,
      { 255, 235, 140, 255 } },
    /* The railgun's little sibling: same instant pierce, a third of the punch,
       three times as often. Hitscan means range is never the problem.        */
    { "레이저",     0.50f, 0.47f, 1, 0.000f,   0.0f,  0.0f, 1.6f, 0.00f,   0.0f, 0, 0, false,true, false, 34.0f, 8.0f,
      { 140, 255, 255, 255 } }
};

/* How long a shot-linked augment ring stays up after firing. Set in FirePlayer,
   read in DrawAugmentRanges - which is most of the file apart, so it lives up
   here with the other cosmetic constants rather than beside either one. */
#define FIRE_RING_TIME 0.30f

#define SWORD_SWING    0.16f
#define BLAST_RADIUS   115.0f
#define BEAM_RADIUS    11.0f
#define BEAM_RANGE     2200.0f

/*----------------------------------------------------------------------------*/
/* Entities                                                                   */
/*----------------------------------------------------------------------------*/
typedef enum GameState { ST_TITLE, ST_TUTORIAL, ST_PLAY, ST_UPGRADE, ST_GAMEOVER } GameState;

typedef enum EnemyType {
    EN_CHASER, EN_DASHER, EN_RUSHER, EN_SPLITTER, EN_TURRET,
    EN_BOMBER, EN_BOMBARDIER, EN_SHIELDER, EN_BOSS
} EnemyType;

/* Bosses rotate through these, one per boss wave, so wave 20 is not simply
   wave 5 with more health. The kind is stashed in Enemy.tier. */
/* Seven laps before a boss ever repeats. The back three are the big versions
   of the three late enemies, and they sit at waves 25/30/35 - after the wave
   the enemy itself arrives, so the boss is always the thing you already know
   turned up to full size. */
typedef enum BossKind {
    BK_BARRAGE, BK_SUMMONER, BK_CHARGER, BK_VORTEX,
    BK_BULWARK, BK_LANCER, BK_MORTAR, BK_COUNT
} BossKind;

/* SHIELDER - the late-game answer to "every enemy dies to whatever you happen
   to be pointing at". It carries a shield that eats any direct hit arriving
   inside SHIELD_ARC of where the shield faces, and the shield tracks you - but
   only at SHIELD_TURN radians a second, which is slower than you fly. So it is
   not a health sponge, it is a positioning problem: get behind it. The recoil
   is what makes that a real decision, because the shots that carry you around
   it are the ones that cannot hit it.

   Area damage ignores the shield entirely (see UpdateBullets) - a rocket, a
   backblast or a death blast is the other answer, so an explosives build is not
   locked out of the enemy that punishes aim. */
#define SHIELD_ARC    1.15f     /* half-angle, ~66 degrees to each side */
#define SHIELD_TURN   1.1f      /* rad/s - a 180 turn takes about 2.9s   */
#define SHIELD_LUNGE  560.0f

/* The shield is a health pool of its own, so "get behind it" is an option
   rather than an order. Blocked hits are spent on the plate at full damage:
   about seven pistol shots to strip it, against four to kill the body once it
   is gone. Flying around is still the cheaper answer - breaking it is what you
   do when the room will not let you, and the enemy speeds up once it is bare,
   so the choice costs something either way. */
#define SHIELD_HP     14.0f
#define SHIELD_BROKEN_SPEED 175.0f
#define SHIELD_SPEED  100.0f

/* RUSHER - the dasher's bigger cousin, and deliberately the opposite kind of
   threat. A dasher commits to a lane the moment it winds up, so it is dodged
   once and then it is spent. A rusher KEEPS AIMING through its lock-on and
   then throws three dashes with a fresh lock between each, so one sidestep is
   not an answer - you have to keep moving, which is what this game is for. */
#define RUSH_LOCK     0.85f     /* first lock-on: long enough to read       */
#define RUSH_RELOCK   0.30f     /* between dashes: only just enough to dodge */
#define RUSH_TIME     0.36f
#define RUSH_SPEED    820.0f
#define RUSH_DASHES   3

/* BOMBARDIER - the first enemy that attacks a PLACE instead of a line. Turrets
   punish standing in their lane; this one lobs an arc that lands where you are
   now and detonates on a fuse, so hovering anywhere is what it punishes. The
   arc is solved for a fixed flight time (see ThrowBomb), which makes it read as
   a lob you can outrun rather than as a homing shot. */
#define BOMB_FLIGHT   1.15f     /* seconds from hand to landing point */
#define BOMB_GRAV     900.0f
#define BOMB_RADIUS   76.0f

/* How many times the summoner boss comes apart: two rounds of doubling, so
   1 -> 2 -> 4, seven bodies over the whole fight. One split was over the moment
   it broke; three went the other way and buried the arena. Two gives the fight
   a middle - it breaks, you deal with the halves, then it breaks AGAIN - while
   still ending before the quarters wear out their welcome. Each generation is
   cut HARD rather than halved (see KillEnemy), so the swarm gets flimsier as
   fast as it gets numerous. */
#define SUMMONER_SPLITS 2

/* Health and size a summoner fragment keeps from the body it broke off. 0.26
   is what keeps seven bodies an escalation instead of a slog:
   1 + 2x0.26 + 4x0.26^2 = about 1.8x the parent's health, which is what this
   fight has cost at every split count. */
#define SUMMONER_CHILD_HP 0.26f
#define SUMMONER_CHILD_R  0.68f

#define BOMBER_RADIUS 96.0f

/* Health a boss gains per lap through the roster, compounding (see SpawnEnemy).
   The single most sensitive number in the late game: 1.30 is a wall that still
   falls, 1.5 is a wall. */
#define BOSS_LAP_GROWTH 1.30f

/* Dasher state machine timings */
#define DASH_WINDUP   0.75f
#define DASH_TIME     0.45f
#define DASH_RECOVER  0.70f
#define DASH_SPEED    820.0f

typedef struct Player {
    Vector2 pos, vel;
    float   aim;            /* radians */
    float   cooldown;
    int     hp;
    float   invuln;
    bool    grounded;
    float   airTime;
    float   groundT;        /* how long we have been standing - the floor heats up */
    int     combo;
    float   comboFlash;
    float   muzzle;         /* muzzle flash timer */
    float   blinkTimer;     /* counts down; the eyes shut while it is negative */
    float   swingT;         /* katana swing animation, counts down */
    int     swingSide;      /* alternates so consecutive slashes mirror */
    float   shieldFlash;    /* AEGIS: counts down after a hit was negated */
    float   aegisCd;        /* AEGIS: locked out until this reaches zero  */
    float   stealCd;        /* LIFESTEAL lockout                          */
    float   thornCd;        /* THORNS lockout - separate on purpose       */
    float   blastT;         /* BACKBLAST ring throttle - purely cosmetic      */
    /* How long the shot-linked augment rings stay up after firing. `muzzle` is
       0.07s - long enough for a flash, far too short to read a range circle
       against - so the ring gets its own, slower timer. Only the challenge ring
       reads it now; DrawAugmentRanges says what happened to the others. */
    float   fireRingT;
    WeaponType weapon;      /* kept until death */
    float   deathT;
    bool    alive;
} Player;

typedef struct Bullet {
    Vector2 pos, vel;
    float   radius;
    float   life, maxLife;
    float   damage;
    float   grav;
    int     bounces;
    int     pierce;         /* enemies left to pass through before dying */
    int     lastHit;        /* enemy index already hit, so one pass = one hit */
    bool    fromPlayer;
    bool    explosive;
    bool    slash;
    bool    active;
    Color   color;
    /* Where this shot left the muzzle, for SNIPER's distance falloff. Appended
       rather than slotted in beside `pos` so the positional initialisers that
       build enemy fire keep working untouched - they simply leave it zeroed,
       and BulletDamage never reads it for a bullet that is not the player's. */
    Vector2 origin;
} Bullet;

typedef struct Enemy {
    EnemyType type;
    Vector2   pos, vel;
    Vector2   lockDir;      /* dasher: direction committed at wind-up */
    float     radius;
    float     hp, maxHp;
    float     timer;        /* attack timer         */
    float     timer2;       /* secondary behaviour  */
    float     rot, rotSpeed;
    float     hitFlash;
    float     spawnT;       /* telegraph countdown; > 0 == not yet solid */
    float     guardT;       /* shielder: glow left over from the last block */
    float     shield, shieldMax;  /* shielder: the plate's own health pool  */
    int       tier;         /* splitter generation, or BossKind on a boss */
    int       gen;          /* summoner: splits done. rusher: dashes left  */
    int       phase;        /* dasher: 0 approach 1 wind-up 2 dash 3 recover */
    bool      active;
} Enemy;

/* Dropped items. Kept to a handful so each one reads as an objective worth
   flying to rather than as clutter. */
/* PK_DAMAGE is the heal crate's twin: same box, same flight to reach it, but
   it pays into attack power instead of health - so a run at full HP still has
   a reason to break off and go get one. */
typedef enum PickupKind { PK_HEAL, PK_WEAPON, PK_DAMAGE } PickupKind;

typedef struct Pickup {
    PickupKind kind;
    WeaponType weapon;      /* meaningful when kind == PK_WEAPON */
    Vector2    pos;
    float      bob;
    float      life;
    bool       active;
} Pickup;

typedef struct Particle {
    Vector2 pos, vel;
    float   life, maxLife;
    float   size;
    float   grav;
    float   drag;
    Color   color;
} Particle;

/* An expanding ring that shows exactly how far an area effect reached. Purely
   visual - it is spawned with the same radius the damage loop used. */
typedef struct Shock {
    Vector2 pos;
    float   radius;
    float   life, maxLife;
    Color   color;
    bool    active;
} Shock;

/* Railgun trace: purely visual, the damage is applied the instant it fires.
   `hw` is the half-width that shot actually damaged with, carried over so the
   trace is drawn at the size it hit at rather than a fixed one - the size
   upgrade is invisible otherwise. It is stored per beam, not read from the
   upgrade at draw time, so a trace still on screen when the next upgrade lands
   keeps showing the shot that fired it. */
typedef struct Beam {
    Vector2 a, b;
    float   life;
    Color   color;
    float   hw;
    bool    active;
} Beam;

typedef struct Popup {
    Vector2 pos;
    float   life;
    int     value;
    /* Owned, not borrowed. This used to be a `const char *`, which is a trap
       for the one caller that formats its text: raylib's TextFormat hands back
       a slot from a ring of four static buffers, and the HUD alone burns that
       many every frame - so a popup that lives 0.8s was pointing at whatever
       had been formatted since. Copying costs 32 bytes a popup and cannot
       dangle. Empty means "show the number instead". */
    char    label[28];
    Color   color;
    bool    active;
} Popup;

/*----------------------------------------------------------------------------*/
/* Globals                                                                    */
/*----------------------------------------------------------------------------*/
static GameState state;

/* Capture mode's three hooks into the simulation. Declared up here because
   UpdatePlayer needs them and the rest of that machinery lives beside main().
   The reticle is pinned as well as the trigger - a capture that read the real
   cursor would render a different picture every time it ran.

   SHOTCOIL_CAPTURE is defined by the Debug|x64 configuration only, so the
   submitted Release binary contains none of this: no flags, no argv, no dead
   branches in the two hot functions below. See "Capture mode" near main(). */
#ifdef SHOTCOIL_CAPTURE
static bool    shotOn;
static bool    shotFire;
static Vector2 shotAim;
#endif

/* The help page is shown once on the way into the first run of the session and
   then never forced again - a player who died and wants back in should not have
   to dismiss a wall of text every time. Deliberately NOT saved to disk: the one
   thing worth guaranteeing is that a judge opening the exe for the first time
   sees it, and a stale save file from a test build must not be able to eat it. */
static bool seenTutorial;

static Player    player;
static Bullet    bullets[MAX_BULLETS];
static Enemy     enemies[MAX_ENEMIES];
static Particle  particles[MAX_PARTICLES];
static Popup     popups[MAX_POPUPS];
#define MAX_BEAMS 8
static Beam      beams[MAX_BEAMS];
#define MAX_SHOCKS 16
static Shock     shocks[MAX_SHOCKS];
#define MAX_PICKUPS 3
static Pickup    pickups[MAX_PICKUPS];
static float     healSpawnT, weaponSpawnT, dmgSpawnT;  /* > 0 == a drop is pending */
static int       lastHealWave, lastWeaponWave, lastDmgWave;

static int   wave;
static int   spawnQueue[MAX_SPAWNQUEUE];
static int   spawnCount, spawnIdx;
static float spawnTimer;
static float waveBannerT;
static float intermission;
static bool  waveCleared;       /* true during the pause after the last kill */

static long  score;
static long  bestScore;
static bool  newRecord;
static float runTime;

static float shake;
static float hitstop;
static float flashWhite;
static float gameOverT;

/*----------------------------------------------------------------------------*/
/* Small helpers                                                              */
/*----------------------------------------------------------------------------*/
static float RandF(float a, float b)
{
    return a + (b - a) * ((float)GetRandomValue(0, 10000) / 10000.0f);
}

static Vector2 FromAngle(float rad, float len)
{
    return (Vector2){ cosf(rad) * len, sinf(rad) * len };
}

/* Signed shortest way round from angle b to angle a, in (-PI, PI]. Everything
   that compares two headings needs this - a raw subtraction reports 350 degrees
   where the real answer is -10. */
static float AngleDelta(float a, float b)
{
    float d = fmodf(a - b + PI, 2.0f * PI);
    if (d < 0.0f) d += 2.0f * PI;
    return d - PI;
}

/* Shortest distance from point p to segment a-b. */
static float DistToSegment(Vector2 p, Vector2 a, Vector2 b)
{
    Vector2 ab = Vector2Subtract(b, a);
    float   len2 = Vector2LengthSqr(ab);
    if (len2 < 0.0001f) return Vector2Distance(p, a);

    float t = Vector2DotProduct(Vector2Subtract(p, a), ab) / len2;
    t = Clamp(t, 0.0f, 1.0f);
    return Vector2Distance(p, Vector2Add(a, Vector2Scale(ab, t)));
}

static void AddShake(float amount)
{
    shake += amount;
    if (shake > 26.0f) shake = 26.0f;
}

/*----------------------------------------------------------------------------*/
/* Viewport - maps the 1280x720 virtual space onto the real window            */
/*----------------------------------------------------------------------------*/
static float   viewScale  = 1.0f;           /* real pixels per virtual unit  */
static Vector2 viewOrigin = { 0.0f, 0.0f }; /* letterbox offset, real pixels */
static bool    fullscreen = true;

static void UpdateViewport(void)
{
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    if (sw < 1.0f || sh < 1.0f) return;     /* minimized */

    viewScale  = fminf(sw / SCREEN_W, sh / SCREEN_H);
    viewOrigin = (Vector2){ (sw - SCREEN_W * viewScale) * 0.5f,
                            (sh - SCREEN_H * viewScale) * 0.5f };
}

/* Camera that draws virtual units into the letterboxed area. */
static Camera2D ViewCamera(Vector2 shakeOffset)
{
    Camera2D cam = { 0 };
    cam.zoom     = viewScale;
    cam.rotation = 0.0f;
    cam.target   = (Vector2){ 0.0f, 0.0f };
    cam.offset   = Vector2Add(viewOrigin, shakeOffset);
    return cam;
}

/* The aiming reticle, in virtual units.
 *
 * Fullscreen locks the OS pointer to the window so it cannot wander onto a
 * second monitor mid-fight. A locked pointer has no meaningful absolute
 * position, so there we integrate its frame deltas into a reticle we own.
 *
 * That path needs MOUSE_SENS: raylib's DisableCursor() also switches GLFW into
 * GLFW_RAW_MOUSE_MOTION, which bypasses the OS pointer-speed slider and
 * acceleration entirely. Raw device counts are typically far slower than the
 * accelerated desktop cursor, so the multiplier exists to hand that feel back.
 *
 * Windowed mode leaves the pointer free - there is no reason to trap it - and
 * simply maps its absolute position, which keeps the OS mouse settings. */
static Vector2 aimCursor    = { SCREEN_W / 2.0f, SCREEN_H / 2.0f };
static bool    cursorLocked = false;

static void ApplyCursorMode(void)
{
    if (fullscreen && !cursorLocked)
    {
        DisableCursor();
        cursorLocked = true;
    }
    else if (!fullscreen && cursorLocked)
    {
        EnableCursor();     /* unlock... */
        HideCursor();       /* ...but still hide it, we draw our own crosshair */
        cursorLocked = false;
    }
}

/* Shared by the F11/Alt+Enter shortcut and the settings screen's toggle -
   one place that flips `fullscreen`, so the two can never disagree about
   which mode the window is actually in. */
static void ToggleFullscreenMode(void)
{
    ToggleBorderlessWindowed();
    fullscreen = !fullscreen;
    UpdateViewport();
    ApplyCursorMode();
}

static void UpdateAimCursor(void)
{
    if (viewScale <= 0.0f) return;

#ifdef SHOTCOIL_CAPTURE
    if (shotOn) { aimCursor = shotAim; return; }
#endif

    if (cursorLocked)
    {
        Vector2 d = GetMouseDelta();

        /* Regaining focus or re-locking can report one enormous jump; swallow
           it rather than teleporting the reticle across the arena. */
        if (fabsf(d.x) > 400.0f || fabsf(d.y) > 400.0f) d = (Vector2){ 0.0f, 0.0f };

        aimCursor = Vector2Add(aimCursor,
                               Vector2Scale(d, tune.mouseSens / viewScale));
    }
    else
    {
        Vector2 m = GetMousePosition();
        aimCursor = (Vector2){ (m.x - viewOrigin.x) / viewScale,
                               (m.y - viewOrigin.y) / viewScale };
    }

    aimCursor.x = Clamp(aimCursor.x, 0.0f, (float)SCREEN_W);
    aimCursor.y = Clamp(aimCursor.y, 0.0f, (float)SCREEN_H);
}

static Vector2 VirtualMouse(void)
{
    return aimCursor;
}

/*----------------------------------------------------------------------------*/
/* Text - Galmuri11, a Korean pixel font, embedded as a subset TTF            */
/*                                                                            */
/* Galmuri is drawn on a 100-unit grid inside a 1200-unit em, and raylib sizes */
/* a font by ascent-descent - 1400 units here. One design pixel is therefore   */
/* exactly one screen pixel at size 14, and stays exact at every multiple of   */
/* 14. At anything else the glyph edges land mid-pixel and the stems come out  */
/* uneven, which is the one way to make a pixel font look cheap.               */
/*                                                                            */
/* The arena is scaled to fill the window, so "a multiple of 14 virtual units" */
/* is not enough - at a 1.5x letterbox scale that is 21 real pixels. Both      */
/* helpers below therefore snap in REAL pixels and convert back, so the text   */
/* sits on the screen grid whatever the window size is.                        */
/*----------------------------------------------------------------------------*/
#include "galmuri.h"

#define FONT_GRID 14

typedef enum FontWeight { FW_REG, FW_BOLD } FontWeight;

static Font uiFont[2];

static Font LoadGalmuri(const unsigned char *data, int size)
{
    Font f = { 0 };
    f.baseSize     = FONT_GRID;
    f.glyphPadding = 1;         /* one transparent pixel is all POINT sampling needs */

    /* FONT_BITMAP is hard on/off pixels. Anti-aliasing has nothing to add to a
       font that is already exact at this size - it would only grey the edges. */
    f.glyphs = LoadFontData(data, size, FONT_GRID, GALMURI_CODEPOINTS,
                            GALMURI_CODEPOINT_COUNT, FONT_BITMAP, &f.glyphCount);
    if (f.glyphs == NULL) return GetFontDefault();

    Image atlas = GenImageFontAtlas(f.glyphs, &f.recs, f.glyphCount,
                                    f.baseSize, f.glyphPadding, 0);
    f.texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);
    SetTextureFilter(f.texture, TEXTURE_FILTER_POINT);
    return f;
}

static void LoadUIFonts(void)
{
    uiFont[FW_REG]  = LoadGalmuri(GALMURI_REGULAR, GALMURI_REGULAR_SIZE);
    uiFont[FW_BOLD] = LoadGalmuri(GALMURI_BOLD,    GALMURI_BOLD_SIZE);
}

static void UnloadUIFonts(void)
{
    for (int i = 0; i < 2; i++)
        if (uiFont[i].texture.id != GetFontDefault().texture.id) UnloadFont(uiFont[i]);
}

/* Nearest whole multiple of the pixel grid, expressed back in virtual units. */
static float UISize(float want)
{
    if (viewScale <= 0.0f) return want;
    float k = floorf(want * viewScale / FONT_GRID + 0.5f);
    if (k < 1.0f) k = 1.0f;
    return (FONT_GRID * k) / viewScale;
}

/* Virtual coordinate that maps onto a whole screen pixel. */
static float UISnap(float v, float origin)
{
    if (viewScale <= 0.0f) return v;
    return (floorf(origin + v * viewScale + 0.5f) - origin) / viewScale;
}

static Vector2 UIMeasure(FontWeight w, const char *txt, float size)
{
    return MeasureTextEx(uiFont[w], txt, UISize(size), 0.0f);
}

static float UIWidth(FontWeight w, const char *txt, float size)
{
    return UIMeasure(w, txt, size).x;
}

static void UIDraw(FontWeight w, const char *txt, float x, float y, float size, Color c)
{
    Vector2 p = { UISnap(x, viewOrigin.x), UISnap(y, viewOrigin.y) };
    DrawTextEx(uiFont[w], txt, p, UISize(size), 0.0f, c);
}

/* Centred horizontally on cx. '\n' starts a new line, and each line is centred
   on its own - raylib's own newline handling left-aligns every line against the
   widest one, which on a centred card reads as a layout mistake. */
static void UIDrawC(FontWeight w, const char *txt, float cx, float y, float size, Color c)
{
    if (strchr(txt, '\n') == NULL)      /* the common case: nothing to split */
    {
        UIDraw(w, txt, cx - UIWidth(w, txt, size) * 0.5f, y, size, c);
        return;
    }

    int          count = 0;
    const char **lines = TextSplit(txt, '\n', &count);
    float        step  = UISize(size) * 1.25f;

    for (int i = 0; i < count; i++)
        UIDraw(w, lines[i], cx - UIWidth(w, lines[i], size) * 0.5f,
               y + i * step, size, c);
}

/* Centred, and the size is taken literally instead of being snapped to the
   pixel grid.

   Only for text that ANIMATES its scale. A snapped size can only ever step
   between whole multiples of the grid, which turns a smooth pop into a
   stutter - so a growing-and-settling number has to give up the grid to move
   smoothly. Pass a snapped size as the base (UISize(...) * pop) and the text
   still lands exactly on the grid once the animation has come to rest.
   Everything static should stay on UIDrawC. */
static void UIDrawCFree(FontWeight w, const char *txt, float cx, float y,
                        float size, Color c)
{
    float half = MeasureTextEx(uiFont[w], txt, size, 0.0f).x * 0.5f;
    DrawTextEx(uiFont[w], txt, (Vector2){ cx - half, y }, size, 0.0f, c);
}

/*----------------------------------------------------------------------------*/
/* Roguelike upgrades                                                         */
/*                                                                            */
/* Every third wave you pick one of three. They stack, and they are wiped on  */
/* death along with your weapon - so a run builds an identity, and losing it  */
/* is what makes the next run interesting rather than identical.              */
/*----------------------------------------------------------------------------*/
typedef enum UpgradeId {
    UP_VITALITY, UP_RAPID, UP_CALIBER, UP_RECOIL, UP_BIGSHOT, UP_VELOCITY,
    UP_ASBESTOS, UP_DEATHBLAST, UP_LIFESTEAL, UP_THORNS, UP_GREED,
    /* The ones below change what a shot DOES rather than how big its number
       is. They are what a build ends up being remembered for. */
    UP_AEGIS, UP_PIERCE, UP_HOMING, UP_SCATTER, UP_BACKBLAST,
    /* ONE-TIME AUGMENTS. Everything above is a stat you can keep buying; these
       four are taken once and then gone from the pool. They do not scale a
       number, they attach a RULE to the bullet - which is why each one is
       worth a card slot even at wave 2, and why none of them is ever the
       obvious pick. Kept contiguous at the end of the enum on purpose: that is
       what lets the roll and the card art recognise them without a per-entry
       flag that could drift out of sync with the table. */
    UP_RICOCHET, UP_DEVOUR, UP_SNIPER, UP_UPDRAFT,
    UP_EXECUTE, UP_FRENZY, UP_FOCUS, UP_MAGNET, UP_COUNT
} UpgradeId;

#define UP_SPECIAL_FIRST  UP_RICOCHET

/* Two knobs decide how an upgrade feels over a long run:
 *
 *   maxStacks - 0 means it never runs out. The plain stat boosts are left
 *               uncapped so a deep run really does turn absurd; the ones that
 *               change a rule (extra pellets, a shield, homing) get a ceiling,
 *               because those stop being "stronger" and start being "different
 *               game" once they pile up.
 *
 *   weight    - relative odds of being offered. Bread-and-butter upgrades come
 *               up constantly, build-defining ones are a lucky roll.
 */
typedef struct UpgradeDef {
    const char *name;
    const char *desc;
    int         maxStacks;      /* 0 = unlimited */
    int         weight;         /* higher = offered more often */
    Color       color;
} UpgradeDef;

static const UpgradeDef UPGRADES[UP_COUNT] = {
    /* name        desc                                cap odds  colour */
    /* Every description is ONE line: each card also prints a live value line
       underneath it (UpgradeValue), and a second wrapped line would collide. */
    { "활력",      "최대 체력 +1 즉시 회복",              0, 100, { 120, 255, 170, 255 } },
    { "속사",      "발사 속도 12% 증가",                  8,  55, { 255, 225, 120, 255 } },
    { "고화력",    "공격력 25% 증가",                     0,  95, { 255, 140, 120, 255 } },
    { "강반동",    "반동 12% 증가",       0,  90, { 150, 200, 255, 255 } },
    { "대구경",    "탄환 크기 30% 증가",                  0,  80, { 200, 160, 255, 255 } },
    /* Speed is range as well as travel time - the fuse length is fixed, so a
       faster shot also reaches further before it expires. That is most of what
       makes it worth a card slot on the short-lived guns. */
    { "고속탄",    "탄환 속도 20% 증가",                  0,  75, { 140, 225, 255, 255 } },
    { "내화복",    "지면 허용 시간 +0.6초",               0,  70, { 255, 180,  90, 255 } },
    { "연쇄 폭발", "처치 시 주변 적 피해",                5,  40, { 255, 120,  90, 255 } },
    { "흡혈",      "처치 시 확률로 회복",                 4,  45, { 120, 255, 210, 255 } },
    { "가시",      "반격 폭발 + 확률 회복",               5,  45, { 255, 160, 220, 255 } },
    { "탐욕",      "점수 30% 증가",                       0,  85, { 255, 235, 140, 255 } },
    /* The rule-changers. Capped, and rare enough that landing one reads as the
       run turning a corner rather than as the default plan. */
    { "방어막",    "피격을 확률로 무력화",                5,  28, { 120, 210, 255, 255 } },
    { "관통",      "탄이 적을 관통",                      5,  32, { 255, 245, 190, 255 } },
    { "유도",      "탄이 적을 추적",                      3,  26, { 190, 255, 140, 255 } },
    /* The strongest thing in the game: one stack multiplies the output of
       every weapon at once, and four is already a wall of shot. */
    { "산탄화",    "탄 1발 추가 발사",                    4,  18, { 255, 190, 130, 255 } },
    { "반동 폭풍", "발사 시 뒤로 충격파",                 5,  26, { 180, 160, 255, 255 } },

    /* The one-time augments. Rare (16 against a pool that sums past 900, so
       roughly one upgrade screen in six shows one) and capped at a single
       stack, because a second copy of a rule is not a rule any more.
     *
     * Each one GRANTS the mechanic it feeds on - RICOCHET hands out bounces,
       DEVOUR hands out a pierce - so it is never a dead card in the hands of a
       gun that happened to lack it. That is the whole reason they can afford to
       be this rare: a rare card that might do nothing is just a wasted pick. */
    { "난반사",    "튕길수록 데미지 증가",                1,  16, { 130, 235, 255, 255 } },
    { "포식",      "관통할수록 크고 강해짐",              1,  16, { 255, 150, 210, 255 } },
    { "저격",      "멀리 날아갈수록 강해짐",              1,  16, { 200, 255, 160, 255 } },
    { "체공",      "오래 떠 있을수록 강해짐",             1,  16, { 255, 210, 140, 255 } },
    { "처형",      "빈사 상태의 적에게 치명타",           1,  16, { 255, 110, 130, 255 } },
    { "폭주",      "체력이 낮을수록 강해짐",              1,  16, { 255, 140, 100, 255 } },
    { "일점사",    "같은 적을 연속 명중할수록 강해짐",    1,  16, { 190, 170, 255, 255 } },
    { "자력탄",    "탄환이 근처 적을 끌어당김",           1,  16, { 140, 220, 200, 255 } }
};

/* ---- one-time augment tuning ---- */
/* Compounding per event, so the ceiling is set by how many events a shot can
   survive rather than by a cap: three bounces is 2.5x, four pierces is 2.9x. */
#define RICOCHET_GAIN    0.35f   /* damage gained per wall bounce             */
#define RICOCHET_BOUNCE  2       /* granted, so it works on guns with none    */
#define DEVOUR_DMG       0.30f   /* damage gained per enemy pierced           */
#define DEVOUR_SIZE      0.22f   /* and the shot visibly grows with it        */
#define DEVOUR_PIERCE    1
/* These two are capped instead: both feed off a quantity the player controls
   directly, and uncapped they would just be "stand still and win". */
#define SNIPER_PER_PX    0.0013f /* +13% per 100px of travel                  */
#define SNIPER_MAX       1.10f   /* caps around 850px - two thirds of the arena */
#define UPDRAFT_PER_SEC  0.14f
#define UPDRAFT_MAX      0.70f

/* EXECUTE is a threshold rather than a curve: a finisher has to be something
   the player can SEE coming (the enemy is nearly dead) or it is just a random
   damage roll. Bosses are excluded - a boss dying to a threshold at a third of
   its bar is not a boss fight. */
#define EXECUTE_BELOW    0.30f
#define EXECUTE_MUL      2.60f
/* FRENZY reads missing health, so it is strongest exactly when a run is about
   to end. Deliberately a comeback rather than a strategy: you cannot farm it
   without standing one hit from death. */
#define FRENZY_PER_HP    0.16f
/* FOCUS rewards staying on one target while everything else closes in, which
   is the opposite of what this game usually wants - that tension is the point. */
#define FOCUS_PER_HIT    0.22f
#define FOCUS_MAX_HITS   5
#define FOCUS_TIMEOUT    1.6f
/* MAGNET pulls enemies toward the shot rather than the shot toward enemies, so
   unlike HOMING it lands the crowd in one place for the NEXT shot. */
/* HOMING's search radius. Named because two places need to agree on it: the
   steering itself and the ring that promises where it will happen. */
#define HOMING_RANGE     560.0f
#define MAGNET_RADIUS    150.0f
#define MAGNET_PULL      520.0f

static int upStacks[UP_COUNT];

/* Earned attack power, in stacks. Unlike upStacks this is not a choice - it is
   the floor under every build, and three separate things feed it: clearing a
   wave, opening a damage crate, finishing a challenge. One shared counter so a
   player only ever has to read one number for "how hard do I hit". */
static int dmgStacks;

/* Rerolls left this run. A bad hand of three is the one way this game can make
   a pick feel like a punishment rather than a decision, and the fix has to be
   scarce or the choice stops mattering at all - so they are spent, not free,
   and a boss is the only thing that hands one back. */
static int rerolls;

/* FOCUS: which enemy the player has been hammering, and how many hits in a row
   have landed on it. Lives here rather than on the bullet because the streak
   is the PLAYER's, carried across separate shots - that is the whole mechanic. */
static int   focusEnemy = -1;
static int   focusHits;
static float focusTimer;

/* ---- wave challenges ---- */
/* A goal laid over a wave the player was going to fight anyway. It never costs
   anything to ignore, which is what lets it ask for something awkward: the
   whole point is to make a player fly differently for ninety seconds. */
typedef enum QuestKind {
    Q_AIRKILL,      /* N kills without touching the floor      */
    Q_NOHIT,        /* clear the wave untouched                */
    Q_SPEEDKILL,    /* N kills inside a time limit             */
    Q_LONGSHOT,     /* N kills from far away                   */
    Q_COUNT
} QuestKind;

typedef enum QuestState { QS_NONE, QS_ACTIVE, QS_DONE, QS_FAILED } QuestState;

static QuestKind  questKind;
static QuestState questState;
static int        questGoal, questProg;
/* Two separate numbers on purpose: `questLimit` is what the goal SAYS, and
   `questTimer` is what is left of it. Reading the countdown back into the goal
   text made the objective itself appear to shrink every frame. */
static float      questTimer, questLimit;   /* Q_SPEEDKILL only */
static float      questFlash;       /* counts down after a state change */
/* How long a FAILED challenge stays on the HUD before clearing itself. A
   success is a reward the player may want to look at; a failure is news for
   about two seconds and a permanent scold after that - nothing can change it,
   and it would otherwise sit pinned under the score for the rest of the wave. */
static float      questHold;
#define QUEST_FAIL_HOLD  2.0f
#define QUEST_FAIL_FADE  0.5f       /* the tail of the hold, spent fading out */

/* What failing one costs: attack power, for the REST OF THE CURRENT WAVE
   only. The wave scaling is why this is safe to impose at all - at wave 3
   it is -10% for ninety seconds, which is a wince; at wave 40 it is -26%,
   which is a wave you have to fight your way out of.
 *
 * It expires at the next StartWave rather than persisting, and that is the
 * whole design. A permanent debuff on Q_NOHIT would mean getting hit makes
 * you weaker makes you get hit - the run would enter a spiral it has no way
 * to climb out of, and the challenge would stop being something you can
 * decide to chase and start being a tax on playing badly. One wave is long
 * enough to hurt and short enough to answer. */
#define QUEST_FAIL_BASE  0.10f      /* -10% at the first challenge wave */
#define QUEST_FAIL_STEP  0.004f     /* +0.4 points a wave after that    */
#define QUEST_FAIL_CAP   0.30f
static float questPenalty;          /* 0.10 == attack power x0.90       */

#define QUEST_EVERY     3           /* one wave in three carries a challenge */
#define QUEST_LONGSHOT  520.0f      /* what counts as "far" for Q_LONGSHOT   */

/* ---- wave mutators (변칙) ----------------------------------------------- */
/* The problem these exist to solve: past the twenties the only thing still
   moving is enemy HEALTH (see SpawnEnemy), and health only makes a wave
   LONGER, never more dangerous. Enemy speed caps around wave 16, the spawn
   counts saturate by 12, and contact damage was a flat one heart forever.
   Meanwhile the player picks a compounding upgrade every other wave and is
   handed +2 HP on every boss. So wave 35 was wave 20 with more chewing.
 *
 * A mutator is a rule change laid over one wave. It is NOT optional: the run
 * gets harder on a schedule the player cannot decline, the same way the wave
 * number itself is not negotiable. Clearing one pays a reroll and a permanent
 * slice of attack power - the reward for surviving it, not a bribe for
 * accepting it.
 *
 * THE TABLE LEANS ENEMY-SIDE ON PURPOSE. A wave that is hard because the
 * enemies are stronger reads as a harder wave; a wave that is hard because
 * your own gun got worse reads as the game taking the controls away. The
 * player-side entries are kept anyway - and weighted well below the others -
 * because this game is about how you fly, and a table that only ever buffs
 * enemies never once asks you to fly differently. */
typedef enum MutatorId {
    /* ---- enemy-side (the default direction) ---- */
    MUT_SWIFT,          /* everything comes at you faster                     */
    MUT_BARRAGE,        /* every enemy clock runs fast - shots and telegraphs */
    MUT_WARD,           /* a flat share of your damage simply does not land   */
    MUT_SAVAGE,         /* contact costs two hearts                           */
    /* ---- player-side (rarer) ---- */
    MUT_GRAVITY,        /* fall harder - you cannot coast between shots       */
    MUT_RECOIL,         /* every shot overshoots; the walls do the rest       */
    MUT_SLOWBULLET,     /* shots crawl - lead your targets                    */
    MUT_HEAVYSHOT,      /* far fewer shots, so each one has to do both jobs   */
    MUT_SCORCH,         /* the floor stops being a place                      */
    MUT_HALVED,         /* the build you spent the run assembling, halved     */
    MUT_COUNT
} MutatorId;

typedef struct MutatorDef {
    const char *name;
    const char *desc;   /* one short measurement, see below */
    int         weight; /* relative odds of being rolled */
    Color       color;
} MutatorDef;

/* The description is ONE short measurement, not a sentence. UISize snaps text
   to whole multiples of a 14 real-pixel grid, so a size-20 line is 14 virtual
   units wide per step at window scale 1 and 21 at scale 2 - half again - and
   anything conversational here ran through the column divider at the larger
   rungs. The name carries the flavour; this line carries the number.

   Weights: the four enemy rules sum to 360 against 208 for the six player
   ones, so roughly three waves in five change the enemies rather than you.
   능력 반감 is the outlier at 18 - far and away the harshest entry, and at
   anything like an even share it would be the reason most runs end. */
static const MutatorDef MUTATORS[MUT_COUNT] = {
    /* name           desc                 odds  colour */
    { "적 가속",      "적 이동 x1.4",       100, { 255, 170,  90, 255 } },
    { "적 연사",      "적 공격속도 x1.5",    95, { 255, 120, 160, 255 } },
    { "적 방벽",      "피해 30% 무효",       85, { 140, 200, 255, 255 } },
    { "적 흉포화",    "접촉 피해 2배",       80, { 255,  90, 110, 255 } },
    { "중력 폭주",    "중력 x1.35",          45, { 150, 170, 255, 255 } },
    { "반동 폭주",    "반동 x1.75",          45, { 190, 150, 255, 255 } },
    { "느린 탄",      "탄속 x0.55",          40, { 120, 235, 255, 255 } },
    { "한 발의 무게", "연사 간격 x1.35",     40, { 255, 225, 120, 255 } },
    { "바닥 초토화",  "지면 인내 x0.4",      40, { 255, 140,  80, 255 } },
    { "능력 반감",    "공격력·탄속·크기 1/2", 18, { 200, 200, 215, 255 } }
};

#define MUT_SWIFT_MUL    1.40f
#define MUT_BARRAGE_MUL  1.50f
#define MUT_WARD_CHANCE  0.30f
#define MUT_SAVAGE_DMG   2
#define MUT_SAVAGE_MERCY 1.55f    /* longer i-frames, or two hearts go at once */

/* THE THRUST BUDGET IS WHY THESE TWO ARE NOT BIGGER NUMBERS.
 *
 * Sustained upward acceleration is one impulse per fire interval: on the
 * pistol that is 460 * 0.92 every 0.32 * 0.86 seconds, about 1540 px/s^2
 * against a gravity of 900. So the player only ever has ~640 px/s^2 of climb
 * to spend, and BOTH of these spend it - one by raising the floor, the other
 * by cutting how often you can push off it.
 *
 * At the 1.6 both started on, gravity became 1440 (climb: 98) and the fire
 * interval left 960 (climb: 60). That is not a hard wave, it is a wave where
 * the game stops answering the mouse - you sink whatever you do. At 1.35 each
 * leaves roughly half the climb rate, which reads as heavy rather than broken.
 * RollMutators additionally never rolls both at once; see MutThrustRule.
 *
 * The same budget is why 능력 반감 does NOT touch fire rate or recoil. Those
 * two ARE the flight controls - halving either grounds the player outright -
 * so that entry halves what the shots DO instead of how often they happen. */
#define MUT_GRAV_MUL     1.35f
#define MUT_RECOIL_MUL   1.75f
/* Recoil sends you further, and this lets it carry: airDrag is per-60th-of-a-
   second retention, so 0.990 -> 0.995 roughly doubles how long a shove lasts.
   Overshooting into a wall is the risk this one sells. */
#define MUT_RECOIL_DRAG  0.995f   /* replaces airDrag outright, not a factor  */
#define MUT_SLOW_MUL     0.55f
#define MUT_HEAVY_MUL    1.35f
#define MUT_SCORCH_MUL   0.40f
#define MUT_HALVED_MUL   0.50f

/* Not before 8: the opening waves are the tutorial, and a run that has not
   been handed any upgrades yet has no answer to any of these. From 25, two at
   once - one mutator is a texture change, two interact. From 40 they arrive
   every other wave instead of every third, which is where the roster and the
   spawn caps have nothing new left to say. Boss waves never carry one: a boss
   is already the wall of its five-wave block. */
#define MUT_FROM_WAVE    8
#define MUT_DOUBLE_WAVE  25
#define MUT_DENSE_WAVE   40
#define MUT_MAX_ACTIVE   2

/* What surviving one is worth, per rule in force. Two stacks is +6% attack
   power against the +3% a plain clear pays, and it is PERMANENT - which is
   what lets a run that keeps meeting mutators compound the way the upgrade
   cards do. The reroll is the other half; see the clear handler. */
#define DMG_MUTATOR      2

static bool mutOn[MUT_COUNT];        /* rules in force for the wave being played */
static bool mutArmed[MUT_COUNT];     /* rolled during the pause, live next wave  */
static int  mutNext[MUT_MAX_ACTIVE]; /* the same set in roll order, for the UI   */
static int  mutNextCount;
static int  mutCount;                /* how many of mutOn[] are true             */
static float mutBannerT;             /* announcement under the wave number       */

/* The physics reads these instead of `tune` directly. Kept as functions rather
   than a mutated copy of the Tunables so the tuning baseline stays the one
   thing on screen in a normal wave, and so nothing can leak a mutator into the
   next wave by forgetting to restore a field. */
static float MutGravity(void)    { return tune.gravity * (mutOn[MUT_GRAVITY] ? MUT_GRAV_MUL : 1.0f); }
static float MutAirDrag(void)    { return mutOn[MUT_RECOIL] ? MUT_RECOIL_DRAG : tune.airDrag; }
static float MutRecoilMul(void)  { return mutOn[MUT_RECOIL] ? MUT_RECOIL_MUL : 1.0f; }
static float MutFireMul(void)    { return mutOn[MUT_HEAVYSHOT] ? MUT_HEAVY_MUL : 1.0f; }
static float MutGroundMul(void)  { return mutOn[MUT_SCORCH] ? MUT_SCORCH_MUL : 1.0f; }
static int   MutContactDmg(void) { return mutOn[MUT_SAVAGE] ? MUT_SAVAGE_DMG : 1; }
static float MutMercy(void)      { return mutOn[MUT_SAVAGE] ? MUT_SAVAGE_MERCY : 1.3f; }

/* 능력 반감 rides along on the stat multipliers it halves; bullet speed is
   shared with 느린 탄, so the two compound if they ever land together. */
static float MutHalvedMul(void)  { return mutOn[MUT_HALVED] ? MUT_HALVED_MUL : 1.0f; }
static float MutBulletMul(void)  { return (mutOn[MUT_SLOWBULLET] ? MUT_SLOW_MUL : 1.0f)
                                        * MutHalvedMul(); }

/* Enemy-side. MutEnemySpeed scales the one place enemy position is integrated,
   MutEnemyRate the one place their clocks tick - so a new enemy kind picks
   both up for free instead of needing a per-behaviour patch. */
static float MutEnemySpeed(void) { return mutOn[MUT_SWIFT]   ? MUT_SWIFT_MUL   : 1.0f; }
static float MutEnemyRate(void)  { return mutOn[MUT_BARRAGE] ? MUT_BARRAGE_MUL : 1.0f; }

/* Whichever rule is lowest in the enum - only used to pick ONE colour for the
   shared banner, so any stable choice would do. */
static int MutFirst(void)
{
    for (int i = 0; i < MUT_COUNT; i++) if (mutOn[i]) return i;
    return 0;
}

#define REROLL_START      2
#define REROLL_PER_BOSS   1
#define REROLL_MAX        4
static int upChoices[3];
static int   pendingPicks;      /* boss clears grant two picks instead of one */
static float upgradeT;          /* seconds since the choice screen appeared */

/* The screen fades in, and ignores input until it has. Appearing instantly
   meant a shot fired on the last frame of a wave could pick a card by accident. */
#define UPGRADE_FADE 0.45f
static int blastDepth;          /* guards DEATH BLAST against runaway chains */

static int   PlayerMaxHp(void)  { return PLAYER_MAX_HP + upStacks[UP_VITALITY]; }
/* Compounding, and uncapped like everything else - but the fire interval is
   what rations flight, so it is kept off zero. At 0.88^n that floor is only
   reached somewhere past 30 stacks; it exists so the maths cannot degenerate,
   not to balance anything. */
/* Every effect is written once, as a function of a stack COUNT, and the live
   getters below just feed it the current count. The upgrade card prints the
   same functions at n and n+1 - so the number a card promises is by
   construction the number the game will use, and the two can never drift. */
static float FireMulAt(int n)
{
    float m = powf(0.88f, (float)n);
    return (m < 0.02f) ? 0.02f : m;
}
static float DamageMulAt(int n)  { return 1.0f + 0.25f * n; }
static float RecoilMulAt(int n)  { return 1.0f + 0.12f * n; }
static float SizeMulAt(int n)    { return 1.0f + 0.30f * n; }
static float SpeedMulAt(int n)   { return 1.0f + 0.20f * n; }
static float ScoreMulAt(int n)   { return 1.0f + 0.30f * n; }
static float GroundTimeAt(int n) { return (tune.groundTime + 0.6f * n) * MutGroundMul(); }
static float BlastRadiusAt(int n){ return 62.0f + 26.0f * n; }

static float UpFireMul(void)    { return FireMulAt(upStacks[UP_RAPID]); }
static float UpDamageMul(void)  { return DamageMulAt(upStacks[UP_CALIBER]); }
static float UpRecoilMul(void)  { return RecoilMulAt(upStacks[UP_RECOIL]); }
/* 능력 반감 (MUT_HALVED) rides these two as well as the damage multiplier
   above and the bullet speed inside MutBulletMul. Fire rate and recoil are
   pointedly absent: they are the flight controls, and halving either one
   drops sustained thrust below gravity - see the thrust-budget note by the
   mutator table. Halving what a shot DOES is a hard wave; halving how often
   you can shoot is a wave you fall through. */
static float UpSizeMul(void)    { return SizeMulAt(upStacks[UP_BIGSHOT]) * MutHalvedMul(); }
static float UpSpeedMul(void)   { return SpeedMulAt(upStacks[UP_VELOCITY]); }
static float UpScoreMul(void)   { return ScoreMulAt(upStacks[UP_GREED]) * MutHalvedMul(); }
static float UpGroundTime(void) { return GroundTimeAt(upStacks[UP_ASBESTOS]); }

/* Attack power earned by clearing waves, on top of whatever the build bought.
 *
 * 3% a wave, flat, is deliberately just under the rank-and-file health ramp
 * (7% a wave from 12, see SpawnEnemy). Through the teens the two nearly cancel:
 * enemies get tougher, you get stronger, and what actually grows is how many of
 * them there are - which is the pressure a new player can read. From wave 20 the
 * enemy side picks up a compounding term and this one does not, so the run
 * starts falling behind again on purpose. It is a floor, not a substitute for
 * the upgrade cards: by wave 30 it is worth 1.9x, while a single CALIBER stack
 * is 1.25x and they multiply. */
#define DMG_PER_STACK    0.03f
/* What each source is worth. A crate you had to fly to and a challenge you had
   to play around are both worth more than simply outliving a wave. */
#define DMG_WAVE_CLEAR   1
#define DMG_CRATE        2
#define DMG_QUEST        3
static float EarnedDamageMul(void) { return 1.0f + DMG_PER_STACK * dmgStacks; }

static bool HasAug(int id) { return upStacks[id] > 0; }

/* FOCUS bookkeeping, called once per DIRECT bullet hit. Kept off the shared
   damage path on purpose: an explosion touching five enemies would rewrite the
   target five times a frame and the streak would never survive a rocket. */
static float FocusOnHit(int enemyIndex)
{
    if (!HasAug(UP_FOCUS)) return 1.0f;

    if (enemyIndex == focusEnemy && focusTimer > 0.0f)
    {
        if (focusHits < FOCUS_MAX_HITS) focusHits++;
    }
    else
    {
        focusEnemy = enemyIndex;
        focusHits  = 0;         /* the first hit only opens the streak */
    }
    focusTimer = FOCUS_TIMEOUT;

    return 1.0f + FOCUS_PER_HIT * (float)focusHits;
}

/* True for the one-time augments, which the enum keeps contiguous at the end
   so this stays a comparison rather than a table lookup. */
static bool UpgradeIsSpecial(int id) { return id >= UP_SPECIAL_FIRST; }

/* UPDRAFT: the longer since you last touched the floor, the harder you hit.
   Deliberately reads player.airTime - the same gauge the no-touch combo runs
   on - so the augment rewards exactly what the game was already asking for and
   the player has one rule to learn instead of two. */
static float UpdraftMul(void)
{
    if (!HasAug(UP_UPDRAFT)) return 1.0f;
    float g = UPDRAFT_PER_SEC * player.airTime;
    return 1.0f + ((g > UPDRAFT_MAX) ? UPDRAFT_MAX : g);
}

/* FRENZY: every heart already lost is damage. Reads max-minus-current so a
   VITALITY stack does not silently hand out a bonus for health never taken. */
static float FrenzyMul(void)
{
    if (!HasAug(UP_FRENZY)) return 1.0f;
    int lost = PlayerMaxHp() - player.hp;
    return 1.0f + FRENZY_PER_HP * (float)(lost < 0 ? 0 : lost);
}

/* FOCUS: consecutive hits on one enemy. The streak is applied at the moment a
   shot lands rather than folded into PlayerDamageMul, because it depends on
   WHO is being hit - which the muzzle cannot know. */
static float FocusMul(void)
{
    if (!HasAug(UP_FOCUS)) return 1.0f;
    return 1.0f + FOCUS_PER_HIT * (float)focusHits;
}

/* Every shot the player fires goes through this - the build, the clears and
   the two player-state augments multiply rather than any replacing another. */
/* The debuff a failed challenge leaves behind, as a multiplier. Folded into
   PlayerDamageMul so every damage source in the game pays it at once - a
   beam, a blast and a stray pellet all weaken together. */
static float QuestPenaltyMul(void) { return 1.0f - questPenalty; }

static float PlayerDamageMul(void)
{
    return UpDamageMul() * EarnedDamageMul() * UpdraftMul() * FrenzyMul()
         * QuestPenaltyMul() * MutHalvedMul();
}

/* SNIPER: scales with how far THIS shot has actually flown. Resolved where the
   damage is spent rather than baked in at spawn, so one bullet is weak at the
   muzzle and lethal across the arena - which is the entire point of it. Every
   place that spends a player bullet's damage must go through here. */
static float BulletDamage(const Bullet *b)
{
    if (!b->fromPlayer || !HasAug(UP_SNIPER)) return b->damage;

    float g = Vector2Distance(b->origin, b->pos) * SNIPER_PER_PX;
    return b->damage * (1.0f + ((g > SNIPER_MAX) ? SNIPER_MAX : g));
}

/* Single source of truth: the ring drawn on screen and the circle the damage
   loop tests against must never be allowed to disagree. */
static float DeathBlastRadius(void) { return BlastRadiusAt(upStacks[UP_DEATHBLAST]); }

/* Stacking odds that approach a ceiling instead of marching past 100%: each
   stack removes the same FRACTION of what is left, so the tenth stack is worth
   much less than the first and no amount of them ever reaches certainty. That
   matters here because these three all cancel or undo damage - a linear roll
   would hit 100% and quietly turn the run invincible. */
static float StackChance(int stacks, float perStack)
{
    if (stacks <= 0) return 0.0f;
    return 1.0f - powf(1.0f - perStack, (float)stacks);
}

static bool RollChance(float p)
{
    return (p > 0.0f) && (RandF(0.0f, 1.0f) < p);
}

/* AEGIS negates the hit outright: 13% a stack, so 1/2/3/4/5 stacks read as
   13 / 24 / 34 / 43 / 50 percent.

   THORNS still blasts whoever landed the hit; its heal can only ever give back
   the point you just lost, never revive you. LIFESTEAL rolls on every kill, so
   its per-stack number has to stay small.

   These live as constants because the upgrade card prints them back to the
   player - a card promising odds the roll did not use would be worse than a
   card promising nothing. */
#define AEGIS_PER_STACK      0.13f
#define THORNS_HEAL_PER      0.14f
#define LIFESTEAL_PER_STACK  0.04f

static float AegisChance(void)      { return StackChance(upStacks[UP_AEGIS],     AEGIS_PER_STACK); }
static float ThornsHealChance(void) { return StackChance(upStacks[UP_THORNS],    THORNS_HEAL_PER); }
static float LifestealChance(void)  { return StackChance(upStacks[UP_LIFESTEAL], LIFESTEAL_PER_STACK); }

/* ---- cooldowns on the two things that undo damage ----
 *
 * The odds above approach a ceiling and never reach certainty, which was
 * supposed to be enough. It is not, because the ROLL RATE is unbounded: a
 * crowded late wave throws hits at you several times a second and a wide
 * weapon kills several enemies a second, so a 50% negate and a 15% heal
 * both fire constantly at exactly the moment they should be scarce. That is
 * how a deep run stops being able to die.
 *
 * A cooldown fixes the rate directly: the chance still decides WHETHER the
 * next eligible hit is negated, the cooldown decides HOW OFTEN there can be
 * a next one. Extra stacks buy the cooldown down as well as the odds up, so
 * a card is still worth taking twice.
 *
 * LIFESTEAL and THORNS get one clock each rather than a shared one. A build
 * that spends two of its card slots on healing should heal more often than
 * one that spent a single slot - and the alternative needed the cards to
 * SAY they shared a clock, which is a sentence neither card has room for at
 * a window scale of 1 (see the note on MUTATORS about UISize). */
#define AEGIS_CD_BASE   6.0f
#define AEGIS_CD_STEP   0.7f    /* per stack past the first */
#define AEGIS_CD_MIN    3.0f
#define HEAL_CD_BASE    9.0f
#define HEAL_CD_STEP    0.9f
#define HEAL_CD_MIN     4.5f

static float CooldownFor(int stacks, float base, float step, float floorSec)
{
    float cd = base - step * (float)(stacks > 0 ? stacks - 1 : 0);
    return (cd < floorSec) ? floorSec : cd;
}

static float AegisCooldown(void) { return CooldownFor(upStacks[UP_AEGIS],
                                                      AEGIS_CD_BASE, AEGIS_CD_STEP, AEGIS_CD_MIN); }
static float StealCooldown(void) { return CooldownFor(upStacks[UP_LIFESTEAL],
                                                      HEAL_CD_BASE, HEAL_CD_STEP, HEAL_CD_MIN); }
static float ThornCooldown(void) { return CooldownFor(upStacks[UP_THORNS],
                                                      HEAL_CD_BASE, HEAL_CD_STEP, HEAL_CD_MIN); }

/* BACKBLAST rides the recoil, so a heavier gun throws a heavier wave. That
   keeps it worth roughly the same on an SMG as on a BAZOOKA. */
static float BackblastRadius(const WeaponDef *w)
{
    return (58.0f + 22.0f * upStacks[UP_BACKBLAST]) * (0.7f + 0.3f * w->recoilMul);
}

/* The line every upgrade card carries: where the stat stands right now, and
   what this pick would move it to.
 *
 * The description above already says how much one stack is worth. What it
 * cannot say is where you actually are - five upgrades into a run nobody is
 * computing 1.25^n in their head, and "+25% damage" stops meaning anything.
 *
 * Both halves come from the same *At() functions the simulation runs on, so a
 * card can never advertise a number the game disagrees with. */
static const char *UpgradeValue(int id)
{
    int n = upStacks[id];

    switch (id)
    {
        case UP_VITALITY:
            return TextFormat("체력 %d → %d", PLAYER_MAX_HP + n, PLAYER_MAX_HP + n + 1);

        /* Named "간격" rather than "속도" because lower is better here, and the
           number shrinking while the card says "증가" would read as a bug. */
        case UP_RAPID:
            return TextFormat("연사 간격 %.0f%% → %.0f%%",
                              FireMulAt(n) * 100.0f, FireMulAt(n + 1) * 100.0f);

        /* Shows the damage you will actually deal, clears folded in - a card
           promising "100% -> 125%" while the HUD reads 157% would look like two
           different stats. The step between the two numbers is still exactly
           what this pick is worth. */
        case UP_CALIBER:
            return TextFormat("공격력 %.0f%% → %.0f%%",
                              DamageMulAt(n)     * EarnedDamageMul() * 100.0f,
                              DamageMulAt(n + 1) * EarnedDamageMul() * 100.0f);
        case UP_RECOIL:
            return TextFormat("반동 %.0f%% → %.0f%%",
                              RecoilMulAt(n) * 100.0f, RecoilMulAt(n + 1) * 100.0f);
        case UP_BIGSHOT:
            return TextFormat("탄 크기 %.0f%% → %.0f%%",
                              SizeMulAt(n) * 100.0f, SizeMulAt(n + 1) * 100.0f);

        /* Says so out loud when the gun in your hands has no projectile to
           speed up. The stat is still bought and still applies to every other
           weapon you might pick up - a card that quietly did nothing would read
           as a bug, and one that hid itself would break the weighted roll. */
        case UP_VELOCITY:
            if (WEAPONS[player.weapon].hitscan) return "즉발 무기에는 효과 없음";
            return TextFormat("탄속 %.0f%% → %.0f%%",
                              SpeedMulAt(n) * 100.0f, SpeedMulAt(n + 1) * 100.0f);
        case UP_GREED:
            return TextFormat("점수 %.0f%% → %.0f%%",
                              ScoreMulAt(n) * 100.0f, ScoreMulAt(n + 1) * 100.0f);

        case UP_ASBESTOS:
            return TextFormat("지면 %.1f초 → %.1f초",
                              GroundTimeAt(n), GroundTimeAt(n + 1));

        case UP_DEATHBLAST:
            return (n <= 0) ? TextFormat("반경 %.0f", BlastRadiusAt(1))
                            : TextFormat("반경 %.0f → %.0f",
                                         BlastRadiusAt(n), BlastRadiusAt(n + 1));

        /* Chance upgrades name what the odds measure - on THORNS the number is
           the heal, not the blast, and a bare "26%" would not say which.

           All three now carry a cooldown as well, and a stack buys both ends:
           better odds AND a shorter lockout. Both have to be on the card, which
           is why the arrow loses its spaces and its second "%" here - the long
           form ran past the card edge at a window scale of 2, where UISize
           renders this line half again as wide (see the note on MUTATORS). */
        case UP_AEGIS:
            return (n <= 0)
                ? TextFormat("무력화 %.0f%%  ·  %.1f초",
                             StackChance(1, AEGIS_PER_STACK) * 100.0f,
                             CooldownFor(1, AEGIS_CD_BASE, AEGIS_CD_STEP, AEGIS_CD_MIN))
                : TextFormat("무력화 %.0f→%.0f%%  ·  %.1f초",
                             StackChance(n,     AEGIS_PER_STACK) * 100.0f,
                             StackChance(n + 1, AEGIS_PER_STACK) * 100.0f,
                             CooldownFor(n + 1, AEGIS_CD_BASE, AEGIS_CD_STEP, AEGIS_CD_MIN));
        case UP_THORNS:
            return (n <= 0)
                ? TextFormat("회복 %.0f%%  ·  %.1f초",
                             StackChance(1, THORNS_HEAL_PER) * 100.0f,
                             CooldownFor(1, HEAL_CD_BASE, HEAL_CD_STEP, HEAL_CD_MIN))
                : TextFormat("회복 %.0f→%.0f%%  ·  %.1f초",
                             StackChance(n,     THORNS_HEAL_PER) * 100.0f,
                             StackChance(n + 1, THORNS_HEAL_PER) * 100.0f,
                             CooldownFor(n + 1, HEAL_CD_BASE, HEAL_CD_STEP, HEAL_CD_MIN));
        case UP_LIFESTEAL:
            return (n <= 0)
                ? TextFormat("회복 %.0f%%  ·  %.1f초",
                             StackChance(1, LIFESTEAL_PER_STACK) * 100.0f,
                             CooldownFor(1, HEAL_CD_BASE, HEAL_CD_STEP, HEAL_CD_MIN))
                : TextFormat("회복 %.0f→%.0f%%  ·  %.1f초",
                             StackChance(n,     LIFESTEAL_PER_STACK) * 100.0f,
                             StackChance(n + 1, LIFESTEAL_PER_STACK) * 100.0f,
                             CooldownFor(n + 1, HEAL_CD_BASE, HEAL_CD_STEP, HEAL_CD_MIN));

        case UP_PIERCE:  return TextFormat("관통 %d → %d",   n, n + 1);
        case UP_HOMING:  return TextFormat("추적 %d → %d",   n, n + 1);
        case UP_SCATTER: return TextFormat("추가 탄 %d → %d", n, n + 1);

        /* Scales with whatever is in your hands, so it is quoted for that gun. */
        case UP_BACKBLAST:
        {
            const WeaponDef *w = &WEAPONS[player.weapon];
            float k    = 0.7f + 0.3f * w->recoilMul;
            float now  = (58.0f + 22.0f * n)       * k;
            float next = (58.0f + 22.0f * (n + 1)) * k;
            return (n <= 0) ? TextFormat("반경 %.0f", next)
                            : TextFormat("반경 %.0f → %.0f", now, next);
        }

        /* One-time augments have no "now -> next": they are off or they are on.
           So the line states the rule's rate instead, which is the number the
           player actually has to weigh against a stack they could take again. */
        case UP_RICOCHET:
            return TextFormat("튕길 때마다 +%.0f%%   (튕김 +%d)",
                              RICOCHET_GAIN * 100.0f, RICOCHET_BOUNCE);
        case UP_DEVOUR:
            return TextFormat("관통마다 +%.0f%% / 크기 +%.0f%%",
                              DEVOUR_DMG * 100.0f, DEVOUR_SIZE * 100.0f);
        case UP_SNIPER:
            return TextFormat("거리에 비례   (최대 +%.0f%%)", SNIPER_MAX * 100.0f);
        case UP_UPDRAFT:
            return TextFormat("1초당 +%.0f%%   (최대 +%.0f%%)",
                              UPDRAFT_PER_SEC * 100.0f, UPDRAFT_MAX * 100.0f);
        case UP_EXECUTE:
            return TextFormat("체력 %.0f%% 이하   피해 %.1f배",
                              EXECUTE_BELOW * 100.0f, EXECUTE_MUL);
        case UP_FRENZY:
            return TextFormat("잃은 체력 1당 +%.0f%%", FRENZY_PER_HP * 100.0f);
        case UP_FOCUS:
            return TextFormat("연속 명중마다 +%.0f%%   (최대 %d회)",
                              FOCUS_PER_HIT * 100.0f, FOCUS_MAX_HITS);
        case UP_MAGNET:
            return TextFormat("반경 %.0f 안의 적을 끌어당김", MAGNET_RADIUS);
    }
    return NULL;
}

static bool UpgradeMaxed(int id)
{
    int cap = UPGRADES[id].maxStacks;
    return (cap > 0 && upStacks[id] >= cap);
}

/* Offer three distinct upgrades, drawn by weight so the plain stat boosts show
   up far more often than the rule-changers.
 *
 * Six upgrades have no ceiling at all, so the candidate list can never fall
 * below three and the screen can never run out of cards - but the -1 fallback
 * stays anyway, because the drawing code already handles it and a silently
 * empty card is a much worse failure than a missing one. */
static void RollUpgrades(void)
{
    int cand[UP_COUNT], n = 0;
    for (int i = 0; i < UP_COUNT; i++)
        if (!UpgradeMaxed(i)) cand[n++] = i;

    for (int slot = 0; slot < 3; slot++)
    {
        if (n == 0) { upChoices[slot] = -1; continue; }

        int total = 0;
        for (int i = 0; i < n; i++) total += UPGRADES[cand[i]].weight;

        int roll = GetRandomValue(0, total - 1);
        int pick = n - 1;                   /* also the guard if weights are odd */
        for (int i = 0; i < n; i++)
        {
            roll -= UPGRADES[cand[i]].weight;
            if (roll < 0) { pick = i; break; }
        }

        upChoices[slot] = cand[pick];
        cand[pick] = cand[--n];             /* drawn without replacement */
    }
}

/* The mutator offer for the wave AFTER the one just cleared. Called from the
   same place RollUpgrades is, and only from there: the offer belongs to the
   screen, so a re-roll of the cards must not quietly re-roll the gamble the
   player has already been reading.

   MUT_SLOWBULLET is withheld from a hitscan build. The railgun and the laser
   have no projectile to slow down, so on those two it is a free +12% attack
   power - an offer that is not a risk teaches the player to stop reading the
   offers at all. */
/* GRAVITY raises what you have to beat, HEAVYSHOT cuts how often you get to
   push against it, and they are drawn from the same budget - together they
   take the player's ability to stay airborne away entirely. Everything else
   in the table stacks fine, so this is a rule about these two rather than a
   general conflict system. */
static bool MutThrustRule(int id)
{
    return id == MUT_GRAVITY || id == MUT_HEAVYSHOT;
}

/* How many rules wave `n` carries. The whole schedule lives here so the roll,
   the forecast on the upgrade screen and the payout all read the same source
   instead of each re-deriving it. */
static int MutSlotsFor(int n)
{
    if (n < MUT_FROM_WAVE) return 0;
    if (n % 5 == 0)        return 0;    /* a boss is already that block's wall */

    /* Every third wave to begin with, every other wave from 40. `2 % every`
       is just the phase that keeps the first mutator wave on 8. */
    int  every = (n >= MUT_DENSE_WAVE) ? 2 : 3;
    int  phase = 2 % every;
    bool due   = (n % every == phase);

    /* A slot that lands on a boss wave SLIDES to the wave after it rather than
       being dropped. The two cycles collide every fifteen waves (20, 35, 50),
       and simply skipping left a five-wave lull each time - 33 through 37 with
       nothing, in exactly the stretch this system exists to fill. */
    if (!due && (n - 1) % 5 == 0 && (n - 1) % every == phase) due = true;
    if (!due) return 0;

    return (n >= MUT_DOUBLE_WAVE) ? MUT_MAX_ACTIVE : 1;
}

/* Rolls the rules for the wave AFTER the one just cleared and arms them.
   Called once from the pause between waves, so the upgrade screen (when there
   is one) can show the player what is coming while they pick a card.

   MUT_SLOWBULLET is withheld from a hitscan build. The railgun and the laser
   have no projectile to slow down, so on those two it is a wave that says it
   got harder and did not - and a rule the player learns to ignore is worse
   than no rule. */
static void RollMutators(void)
{
    int next = wave + 1;

    mutNextCount = 0;
    for (int i = 0; i < MUT_MAX_ACTIVE; i++) mutNext[i] = -1;
    memset(mutArmed, 0, sizeof(mutArmed));

    int want = MutSlotsFor(next);
    if (want <= 0) return;

    int cand[MUT_COUNT], n = 0;
    for (int i = 0; i < MUT_COUNT; i++)
    {
        if (i == MUT_SLOWBULLET && WEAPONS[player.weapon].hitscan) continue;
        cand[n++] = i;
    }

    for (int slot = 0; slot < want && n > 0; slot++)
    {
        int total = 0;
        for (int i = 0; i < n; i++) total += MUTATORS[cand[i]].weight;

        int roll = GetRandomValue(0, total - 1);
        int pick = n - 1;                   /* also the guard if weights are odd */
        for (int i = 0; i < n; i++)
        {
            roll -= MUTATORS[cand[i]].weight;
            if (roll < 0) { pick = i; break; }
        }

        int id = cand[pick];
        mutNext[mutNextCount++] = id;
        mutArmed[id] = true;
        cand[pick] = cand[--n];             /* drawn without replacement */

        /* Rolling one thrust rule closes the door on the other. */
        if (MutThrustRule(id))
            for (int i = 0; i < n; i++)
                if (MutThrustRule(cand[i])) { cand[i] = cand[--n]; i--; }
    }
}

/*----------------------------------------------------------------------------*/
/* Audio - every sound is generated in code, no WAV files on disk             */
/*----------------------------------------------------------------------------*/
#define SFX_VOICES 5

typedef struct SfxPool {
    Sound voice[SFX_VOICES];
    int   next;
} SfxPool;

typedef enum SfxWave { SW_SINE, SW_SQUARE, SW_SAW } SfxWave;

static SfxPool sfxShoot, sfxHit, sfxKill, sfxBoom, sfxHurt, sfxReload, sfxWave, sfxWarn;
static bool    audioReady = false;

static Sound SynthSound(SfxWave shape, float f0, float f1, float duration,
                        float noiseMix, float envPow, float volume)
{
    const int sampleRate = 22050;
    int   frames = (int)(sampleRate * duration);
    short *data  = (short *)MemAlloc(frames * sizeof(short));
    float phase  = 0.0f;
    unsigned int rng = 0x1234567u;
    float lowpass = 0.0f;

    for (int i = 0; i < frames; i++)
    {
        float t = (float)i / (float)frames;
        float f = f0 + (f1 - f0) * t;

        phase += f / sampleRate;
        if (phase >= 1.0f) phase -= 1.0f;

        float tone = 0.0f;
        switch (shape)
        {
            case SW_SINE:   tone = sinf(phase * 2.0f * PI);      break;
            case SW_SQUARE: tone = (phase < 0.5f) ? 1.0f : -1.0f; break;
            case SW_SAW:    tone = 2.0f * phase - 1.0f;           break;
        }

        rng = rng * 1664525u + 1013904223u;
        float noise = ((float)((rng >> 9) & 0xFFFF) / 32767.5f) - 1.0f;
        lowpass += (noise - lowpass) * 0.45f;   /* take the edge off the hiss */

        float s   = tone * (1.0f - noiseMix) + lowpass * noiseMix;
        float env = powf(1.0f - t, envPow);
        float atk = fminf(1.0f, (float)i / (sampleRate * 0.003f));
        float v   = s * env * atk * volume;

        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        data[i] = (short)(v * 32000.0f);
    }

    Wave w = { (unsigned int)frames, sampleRate, 16, 1, data };
    Sound snd = LoadSoundFromWave(w);
    MemFree(data);
    return snd;
}

static SfxPool MakePool(SfxWave shape, float f0, float f1, float dur,
                        float noiseMix, float envPow, float vol)
{
    SfxPool p = { 0 };
    p.voice[0] = SynthSound(shape, f0, f1, dur, noiseMix, envPow, vol);
    for (int i = 1; i < SFX_VOICES; i++) p.voice[i] = LoadSoundAlias(p.voice[0]);
    p.next = 0;
    return p;
}

static void UnloadPool(SfxPool *p)
{
    for (int i = SFX_VOICES - 1; i >= 1; i--) UnloadSoundAlias(p->voice[i]);
    UnloadSound(p->voice[0]);
}

static void PlaySfx(SfxPool *p, float pitch)
{
    if (!audioReady) return;
    Sound s = p->voice[p->next];
    p->next = (p->next + 1) % SFX_VOICES;
    SetSoundPitch(s, pitch);
    PlaySound(s);
}

static void InitSfx(void)
{
    InitAudioDevice();
    if (!IsAudioDeviceReady()) return;
    audioReady = true;
    SetMasterVolume(0.65f);

    sfxShoot  = MakePool(SW_SQUARE, 520.0f, 120.0f, 0.09f, 0.35f, 2.2f, 0.40f);
    sfxHit    = MakePool(SW_SQUARE, 900.0f, 420.0f, 0.06f, 0.15f, 2.5f, 0.30f);
    sfxKill   = MakePool(SW_SAW,    340.0f,  70.0f, 0.20f, 0.55f, 1.8f, 0.45f);
    sfxBoom   = MakePool(SW_SAW,    180.0f,  35.0f, 0.55f, 0.70f, 1.4f, 0.65f);
    sfxHurt   = MakePool(SW_SQUARE, 220.0f,  60.0f, 0.28f, 0.30f, 1.6f, 0.55f);
    sfxReload = MakePool(SW_SINE,   320.0f, 760.0f, 0.10f, 0.05f, 2.0f, 0.30f);
    sfxWave   = MakePool(SW_SINE,   180.0f, 620.0f, 0.35f, 0.00f, 1.2f, 0.35f);
    sfxWarn   = MakePool(SW_SQUARE, 130.0f, 330.0f, 0.22f, 0.10f, 0.9f, 0.30f);
}

static void ShutdownSfx(void)
{
    if (!audioReady) return;
    UnloadPool(&sfxShoot);  UnloadPool(&sfxHit);   UnloadPool(&sfxKill);
    UnloadPool(&sfxBoom);   UnloadPool(&sfxHurt);  UnloadPool(&sfxReload);
    UnloadPool(&sfxWave);   UnloadPool(&sfxWarn);
    CloseAudioDevice();
}

/*----------------------------------------------------------------------------*/
/* Music - two chiptune loops, rendered a buffer at a time                    */
/*----------------------------------------------------------------------------*/
/* The size limit rules out an audio file. Even a thin 30-second mono OGG eats
   the whole remaining headroom, and it would buy a loop the player hears forty
   times a run. So the music is synthesised like everything else here - except
   a song is far too long to hold as one Wave, so it is rendered a chunk at a
   time into a streaming buffer instead.
 *
 * The voice set is the NES's on purpose: two pulses, a triangle bass, and one
 * noise channel doing the drums. Four voices is not a restriction being worked
 * around - it is the sound. */
#define MUS_RATE    22050
#define MUS_CHUNK   1024        /* frames per refill - 46ms at this rate */
#define MUS_STEPS   128         /* sixteenth notes in one loop, either song */

/* Pattern bytes: 0 leaves the voice alone, 1 cuts it, anything else is a MIDI
   note. A rest is written as an explicit cut so that "hold" and "silence" can
   never be the same byte - that is the bug that makes a hand-typed pattern
   sound like it has a wrong note when it actually has a missing one. */

static const unsigned char TITLE_LEAD[MUS_STEPS] = {
    /* Am */ 69,0,0,0, 72,0,0,0, 76,0,0,0,  0,0,0,0,
    /* F  */ 74,0,0,0, 72,0,0,0, 69,0,0,0,  0,0,0,0,
    /* C  */ 76,0,0,0, 79,0,0,0, 76,0,0,0,  0,0,0,0,
    /* G  */ 74,0,0,0,  0,0,0,0, 71,0,0,0,  0,0,0,0,
    /* Am */ 69,0,0,0, 72,0,0,0, 76,0,0,0, 81,0,0,0,
    /* F  */ 79,0,0,0,  0,0,0,0, 77,0,0,0,  0,0,0,0,
    /* E  */ 76,0,0,0, 74,0,0,0, 72,0,0,0, 71,0,0,0,
    /* Am */ 69,0,0,0,  0,0,0,0,  0,0,0,0,  1,0,0,0
};
static const unsigned char TITLE_HARM[MUS_STEPS] = {
    57,0,60,0, 64,0,60,0, 57,0,60,0, 64,0,60,0,
    53,0,57,0, 60,0,57,0, 53,0,57,0, 60,0,57,0,
    52,0,55,0, 60,0,55,0, 52,0,55,0, 60,0,55,0,
    55,0,59,0, 62,0,59,0, 55,0,59,0, 62,0,59,0,
    57,0,60,0, 64,0,60,0, 57,0,60,0, 64,0,60,0,
    53,0,57,0, 60,0,57,0, 53,0,57,0, 60,0,57,0,
    52,0,56,0, 59,0,56,0, 52,0,56,0, 59,0,56,0,
    57,0,60,0, 64,0,60,0, 57,0,60,0, 64,0, 1,0
};
static const unsigned char TITLE_BASS[MUS_STEPS] = {
    45,0,0,0,0,0,0,0, 45,0,0,0,0,0,0,0,
    41,0,0,0,0,0,0,0, 41,0,0,0,0,0,0,0,
    48,0,0,0,0,0,0,0, 48,0,0,0,0,0,0,0,
    43,0,0,0,0,0,0,0, 43,0,0,0,0,0,0,0,
    45,0,0,0,0,0,0,0, 45,0,0,0,0,0,0,0,
    41,0,0,0,0,0,0,0, 41,0,0,0,0,0,0,0,
    40,0,0,0,0,0,0,0, 40,0,0,0,0,0,0,0,
    45,0,0,0,0,0,0,0, 45,0,0,0, 1,0,0,0
};

static const unsigned char BATTLE_LEAD[MUS_STEPS] = {
    /* Am */ 69,0,72,0, 76,0,72,0, 69,0,76,0, 72,0,69,0,
    /* Am */ 81,0,79,0, 76,0,72,0, 76,0,72,0, 69,0,67,0,
    /* F  */ 77,0,72,0, 69,0,72,0, 77,0,81,0, 77,0,72,0,
    /* G  */ 79,0,74,0, 71,0,74,0, 79,0,83,0, 79,0,74,0,
    /* Am */ 69,0,72,0, 76,0,72,0, 69,0,76,0, 72,0,69,0,
    /* Am */ 81,0,79,0, 77,0,76,0, 74,0,72,0, 71,0,69,0,
    /* F  */ 77,0,81,0, 84,0,81,0, 77,0,72,0, 77,0,81,0,
    /* E  */ 83,0,80,0, 76,0,71,0, 76,0,80,0, 83,0, 1,0
};
static const unsigned char BATTLE_HARM[MUS_STEPS] = {
    0,0,64,0, 0,0,64,0, 0,0,64,0, 0,0,64,0,
    0,0,64,0, 0,0,69,0, 0,0,64,0, 0,0,69,0,
    0,0,60,0, 0,0,60,0, 0,0,60,0, 0,0,65,0,
    0,0,62,0, 0,0,62,0, 0,0,62,0, 0,0,67,0,
    0,0,64,0, 0,0,64,0, 0,0,64,0, 0,0,64,0,
    0,0,64,0, 0,0,69,0, 0,0,64,0, 0,0,69,0,
    0,0,60,0, 0,0,65,0, 0,0,60,0, 0,0,65,0,
    0,0,59,0, 0,0,64,0, 0,0,59,0, 0,0, 1,0
};
static const unsigned char BATTLE_BASS[MUS_STEPS] = {
    45,0,45,0, 57,0,45,0, 45,0,45,0, 57,0,45,0,
    45,0,45,0, 57,0,45,0, 45,0,45,0, 57,0,45,0,
    41,0,41,0, 53,0,41,0, 41,0,41,0, 53,0,41,0,
    43,0,43,0, 55,0,43,0, 43,0,43,0, 55,0,43,0,
    45,0,45,0, 57,0,45,0, 45,0,45,0, 57,0,45,0,
    45,0,45,0, 57,0,45,0, 45,0,45,0, 57,0,45,0,
    41,0,41,0, 53,0,41,0, 41,0,41,0, 53,0,41,0,
    40,0,40,0, 52,0,40,0, 40,0,40,0, 52,0, 1,0
};

/* Drums repeat every bar, so they are stored as one bar rather than 128 bytes
   of the same four hits. 1 kick, 2 snare, 3 hat. The fill lands on the fourth
   and eighth bars - the two places the loop would otherwise start to be
   audible as a loop. */
static const unsigned char BATTLE_DRUM[16]     = { 1,3,3,3, 2,3,3,3, 1,3,1,3, 2,3,3,3 };
static const unsigned char BATTLE_DRUMFILL[16] = { 1,3,3,3, 2,3,3,3, 1,3,2,3, 2,2,2,2 };

typedef enum MusicTrack { MUS_OFF, MUS_TITLE, MUS_BATTLE, MUS_COUNT } MusicTrack;

typedef struct MusSong {
    float bpm;
    const unsigned char *lead, *harm, *bass, *drum, *drumFill;
} MusSong;

static const MusSong SONGS[MUS_COUNT] = {
    { 120.0f, NULL, NULL, NULL, NULL, NULL },
    /* The title sits under a screen nobody is being asked to react to, so it
       is slow and carries no drums at all. */
    { 100.0f, TITLE_LEAD,  TITLE_HARM,  TITLE_BASS,  NULL,        NULL },
    { 150.0f, BATTLE_LEAD, BATTLE_HARM, BATTLE_BASS, BATTLE_DRUM, BATTLE_DRUMFILL }
};

typedef struct MusVoice { float phase, freq, env, decay, duty; } MusVoice;

static AudioStream musStream;
static short       musBuf[MUS_CHUNK];
static bool        musReady   = false;
static bool        musEnabled = true;      /* M toggles it, from any screen */
static MusicTrack  musTrack   = MUS_OFF;   /* what is sounding right now    */
static MusicTrack  musWant    = MUS_OFF;   /* what the game state asks for  */
static float       musGain    = 0.0f;      /* the crossfade between the two */
static int         musStep    = 0;
static int         musSamp    = 0;         /* samples left in the step */

static MusVoice musP1, musP2, musTri;
static struct {
    float env, decay, phase, freq, lp, prev;
    int   type;
    unsigned int rng;
} musDrum;

static float MidiHz(unsigned char note)
{
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

static void MusNote(MusVoice *v, unsigned char code, float duty, float decay,
                    bool resetPhase)
{
    if (code == 0) return;
    if (code == 1) { v->env = 0.0f; return; }
    v->freq  = MidiHz(code);
    v->env   = 1.0f;
    v->decay = decay;
    v->duty  = duty;
    if (resetPhase) v->phase = 0.0f;   /* on a pulse the click is the attack */
}

static void MusDrumHit(int type)
{
    musDrum.type  = type;
    musDrum.env   = 1.0f;
    musDrum.phase = 0.0f;
    switch (type)
    {
        case 1:  musDrum.freq = 150.0f; musDrum.decay = 0.99970f; break;  /* kick  */
        case 2:  musDrum.decay = 0.99930f;                        break;  /* snare */
        default: musDrum.decay = 0.99750f;                        break;  /* hat   */
    }
}

/* One sixteenth note of whichever song is playing. */
static void MusAdvance(void)
{
    const MusSong *s = &SONGS[musTrack];
    if (s->lead == NULL) return;

    MusNote(&musP1,  s->lead[musStep], 0.50f, 0.99994f, true);
    MusNote(&musP2,  s->harm[musStep], 0.25f, 0.99985f, true);
    MusNote(&musTri, s->bass[musStep], 0.00f, 0.99992f, false);

    if (s->drum != NULL)
    {
        int bar = musStep / 16;
        const unsigned char *row = ((bar == 3 || bar == 7) && s->drumFill != NULL)
                                 ? s->drumFill : s->drum;
        int hit = row[musStep % 16];
        if (hit != 0) MusDrumHit(hit);
    }

    musStep = (musStep + 1) % MUS_STEPS;
}

static float MusPulse(MusVoice *v)
{
    v->phase += v->freq / MUS_RATE;
    if (v->phase >= 1.0f) v->phase -= 1.0f;
    float s = (v->phase < v->duty) ? 1.0f : -1.0f;
    v->env *= v->decay;
    return s * v->env;
}

/* Quantised to sixteen levels, which is what the NES triangle actually does
   and is why its bass has that faint buzz instead of sounding like a sine. */
static float MusTriangle(MusVoice *v)
{
    v->phase += v->freq / MUS_RATE;
    if (v->phase >= 1.0f) v->phase -= 1.0f;
    float s = (v->phase < 0.5f) ? (4.0f * v->phase - 1.0f)
                                : (3.0f - 4.0f * v->phase);
    s = floorf(s * 8.0f) / 8.0f;
    v->env *= v->decay;
    return s * v->env;
}

static float MusDrumSample(void)
{
    musDrum.rng = musDrum.rng * 1664525u + 1013904223u;
    float n = ((float)((musDrum.rng >> 9) & 0xFFFF) / 32767.5f) - 1.0f;
    float s = 0.0f;

    if (musDrum.type == 1)
    {
        musDrum.phase += musDrum.freq / MUS_RATE;
        if (musDrum.phase >= 1.0f) musDrum.phase -= 1.0f;
        musDrum.freq = fmaxf(42.0f, musDrum.freq * 0.99940f);
        s = sinf(musDrum.phase * 2.0f * PI);
    }
    else if (musDrum.type == 2)
    {
        musDrum.lp += (n - musDrum.lp) * 0.55f;   /* body under the hiss */
        s = musDrum.lp * 1.3f;
    }
    else if (musDrum.type == 3)
    {
        s = (n - musDrum.prev) * 0.5f;            /* first difference = bright */
    }

    musDrum.prev = n;
    s *= musDrum.env;
    musDrum.env *= musDrum.decay;
    return s;
}

static void MusSilenceVoices(void)
{
    musP1.env = musP2.env = musTri.env = 0.0f;
    musDrum.env  = 0.0f;
    musDrum.type = 0;
}

static void MusRender(short *out, int frames)
{
    const float fadeOut = 1.0f / (0.22f * MUS_RATE);
    const float fadeIn  = 1.0f / (0.45f * MUS_RATE);

    for (int i = 0; i < frames; i++)
    {
        /* The hand-off happens here rather than on the game thread, so a track
           change can never land in the middle of a sample. */
        if (musTrack != musWant)
        {
            musGain -= fadeOut;
            if (musGain <= 0.0f)
            {
                musGain  = 0.0f;
                musTrack = musWant;
                musStep  = 0;
                musSamp  = 0;
                MusSilenceVoices();
            }
        }
        else if (musTrack != MUS_OFF && musGain < 1.0f)
        {
            musGain = fminf(1.0f, musGain + fadeIn);
        }

        if (musTrack == MUS_OFF) { out[i] = 0; continue; }

        if (musSamp <= 0)
        {
            MusAdvance();
            musSamp = (int)(MUS_RATE * 15.0f / SONGS[musTrack].bpm);
        }
        musSamp--;

        float mix = MusPulse(&musP1)     * 0.20f
                  + MusPulse(&musP2)     * 0.13f
                  + MusTriangle(&musTri) * 0.26f
                  + MusDrumSample()      * 0.22f;

        mix *= musGain;
        if (mix >  1.0f) mix =  1.0f;
        if (mix < -1.0f) mix = -1.0f;
        out[i] = (short)(mix * 30000.0f);
    }
}

static void InitMusic(void)
{
    if (!audioReady) return;
    SetAudioStreamBufferSizeDefault(MUS_CHUNK);
    musStream = LoadAudioStream(MUS_RATE, 16, 1);
    if (!IsAudioStreamValid(musStream)) return;

    musReady    = true;
    musDrum.rng = 0x9E3779B9u;
    /* Under the effects, not beside them: the shot is the feedback the player
       is actually listening for. */
    SetAudioStreamVolume(musStream, 0.45f);
    PlayAudioStream(musStream);
}

static void ShutdownMusic(void)
{
    if (!musReady) return;
    StopAudioStream(musStream);
    UnloadAudioStream(musStream);
    musReady = false;
}

/* Which song the screen in front of the player should be playing. The help
   page and the game-over screen keep the title theme - both are places where
   the run is not happening, and both are read rather than played. */
static MusicTrack MusicForState(void)
{
    if (!musEnabled) return MUS_OFF;
    switch (state)
    {
        case ST_PLAY:
        case ST_UPGRADE: return MUS_BATTLE;
        default:         return MUS_TITLE;
    }
}

static void UpdateMusic(void)
{
    if (!musReady) return;
    musWant = MusicForState();
    while (IsAudioStreamProcessed(musStream))
    {
        MusRender(musBuf, MUS_CHUNK);
        UpdateAudioStream(musStream, musBuf, MUS_CHUNK);
    }
}

/*----------------------------------------------------------------------------*/
/* Particles & popups                                                         */
/*----------------------------------------------------------------------------*/
static void EmitBurst(Vector2 pos, float dir, float arc, int count,
                      float spdMin, float spdMax, float life, float size,
                      Color color, float grav)
{
    for (int n = 0; n < count; n++)
    {
        for (int i = 0; i < MAX_PARTICLES; i++)
        {
            Particle *p = &particles[i];
            if (p->life > 0.0f) continue;

            float a = dir + RandF(-arc, arc);
            p->pos     = pos;
            p->vel     = FromAngle(a, RandF(spdMin, spdMax));
            p->maxLife = life * RandF(0.65f, 1.35f);
            p->life    = p->maxLife;
            p->size    = size * RandF(0.6f, 1.4f);
            p->grav    = grav;
            p->drag    = 0.93f;
            p->color   = color;
            break;
        }
    }
}

static void AddShock(Vector2 pos, float radius, Color color)
{
    for (int i = 0; i < MAX_SHOCKS; i++)
    {
        if (shocks[i].active) continue;
        shocks[i] = (Shock){ pos, radius, 0.42f, 0.42f, color, true };
        return;
    }
}

static void UpdateShocks(float dt)
{
    for (int i = 0; i < MAX_SHOCKS; i++)
    {
        if (!shocks[i].active) continue;
        shocks[i].life -= dt;
        if (shocks[i].life <= 0.0f) shocks[i].active = false;
    }
}

static void DrawShocks(void)
{
    for (int i = 0; i < MAX_SHOCKS; i++)
    {
        const Shock *s = &shocks[i];
        if (!s->active) continue;

        float t = 1.0f - Clamp(s->life / s->maxLife, 0.0f, 1.0f);   /* 0 -> 1 */
        float r = s->radius * (1.0f - powf(1.0f - t, 3.0f));        /* eases out */
        float a = 1.0f - t;

        /* sweeping front */
        if (r > 2.0f)
            DrawRing(s->pos, r * 0.70f, r, 0.0f, 360.0f, 48, Fade(s->color, 0.30f * a));

        /* the boundary itself, held at the true damage radius the whole time so
           the player can actually learn the range */
        DrawRing(s->pos, s->radius - 2.0f, s->radius + 1.0f, 0.0f, 360.0f, 56,
                 Fade(s->color, 0.55f * a));
    }
}

static void AddPopupEx(Vector2 pos, int value, const char *label, Color color)
{
    for (int i = 0; i < MAX_POPUPS; i++)
    {
        if (popups[i].active) continue;
        popups[i].active = true;
        popups[i].pos    = pos;
        popups[i].life   = 0.8f;
        popups[i].value  = value;
        popups[i].color  = color;
        /* snprintf, not raylib's TextCopy - that one is an unbounded strcpy.
           28 bytes is roughly double the longest label in the game (the weapon
           names, at 15 bytes of UTF-8), so the bound is a guard that should
           never actually fire rather than a truncation anyone will see. */
        if (label) snprintf(popups[i].label, sizeof(popups[i].label), "%s", label);
        else       popups[i].label[0] = 0;
        return;
    }
}

static void AddPopup(Vector2 pos, int value, Color color)
{
    AddPopupEx(pos, value, NULL, color);
}

/* One door for every attack-power gain, so the popup, the sound and the number
   can never disagree about what a reward was worth. */
static void GrantDamage(int stacks, Vector2 at, const char *why)
{
    dmgStacks += stacks;
    AddPopupEx(at, 0, why, (Color){ 255, 170, 120, 255 });
    AddShock(at, 40.0f, (Color){ 255, 170, 120, 255 });
    PlaySfx(&sfxWave, 1.35f);
}

/* ---- wave challenges ---- */

static const char *QuestText(void)
{
    switch (questKind)
    {
        case Q_AIRKILL:   return TextFormat("착지 없이 %d킬", questGoal);
        case Q_NOHIT:     return "피격 없이 웨이브 클리어";
        case Q_SPEEDKILL: return TextFormat("%.0f초 안에 %d킬", questLimit, questGoal);
        /* Phrased against the ring DrawLongshotRange puts on screen, in the
           same colour. "far" on its own was a number only the source knew. */
        case Q_LONGSHOT:  return TextFormat("원 밖에서 %d킬", questGoal);
        default:          return "";
    }
}

/* Rolled at the top of a wave. Goals lean on the wave number so a challenge
   stays roughly as hard to hit at 25 as it was at 4 - the arena is fuller by
   then, so a flat "6 kills" would quietly turn into a freebie. */
static void StartQuest(int n)
{
    questKind  = (QuestKind)GetRandomValue(0, Q_COUNT - 1);
    questState = QS_ACTIVE;
    questProg  = 0;
    questFlash = 1.4f;

    switch (questKind)
    {
        case Q_AIRKILL:   questGoal = 5 + n / 6;  break;
        case Q_NOHIT:     questGoal = 1;          break;
        case Q_SPEEDKILL:
            questGoal  = 6 + n / 5;
            questLimit = 14.0f;
            questTimer = questLimit;
            break;
        case Q_LONGSHOT:  questGoal = 4 + n / 8;  break;
        default:          questState = QS_NONE;   break;
    }
}

static void QuestSucceed(void)
{
    if (questState != QS_ACTIVE) return;
    questState = QS_DONE;
    questFlash = 1.6f;

    GrantDamage(DMG_QUEST, (Vector2){ player.pos.x, player.pos.y - 52.0f },
                TextFormat("도전 성공  공격력 +%.0f%%",
                           DMG_PER_STACK * DMG_QUEST * 100.0f));
    flashWhite = 0.25f;
}

static void QuestFail(void)
{
    if (questState != QS_ACTIVE) return;
    questState = QS_FAILED;
    questFlash = 1.0f;
    questHold  = QUEST_FAIL_HOLD;

    questPenalty = QUEST_FAIL_BASE + QUEST_FAIL_STEP * (float)wave;
    if (questPenalty > QUEST_FAIL_CAP) questPenalty = QUEST_FAIL_CAP;

    AddPopupEx((Vector2){ player.pos.x, player.pos.y - 52.0f }, 0,
               TextFormat("도전 실패  공격력 -%.0f%%", questPenalty * 100.0f),
               (Color){ 255, 110, 110, 255 });
    AddShock(player.pos, 46.0f, (Color){ 255, 110, 110, 255 });
    PlaySfx(&sfxWarn, 0.6f);
}

/* Called from KillEnemy for every kill the player earns. `dist` is how far the
   killing blow landed from the player, which only Q_LONGSHOT reads. */
static void QuestOnKill(float dist)
{
    if (questState != QS_ACTIVE) return;

    switch (questKind)
    {
        /* Airborne only, and UpdatePlayer zeroes the progress on landing - the
           counter IS the streak rather than a running total. */
        case Q_AIRKILL:   if (!player.grounded) questProg++; break;
        case Q_SPEEDKILL: questProg++;                       break;
        case Q_LONGSHOT:  if (dist > QUEST_LONGSHOT) questProg++; break;
        default: return;
    }

    if (questProg >= questGoal) QuestSucceed();
}

static void UpdateQuest(float dt)
{
    if (questFlash > 0.0f) questFlash -= dt;

    /* Failure is self-clearing; QS_NONE is what the rest of the code already
       treats as "no challenge this wave", so nothing downstream has to learn a
       new state. Success deliberately does NOT expire. */
    if (questState == QS_FAILED)
    {
        questHold -= dt;
        if (questHold <= 0.0f) questState = QS_NONE;
        return;
    }

    if (questState != QS_ACTIVE) return;

    if (questKind == Q_SPEEDKILL)
    {
        questTimer -= dt;
        if (questTimer <= 0.0f) QuestFail();
    }
}

static void UpdateParticles(float dt)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        Particle *p = &particles[i];
        if (p->life <= 0.0f) continue;

        p->life -= dt;
        p->vel.y += p->grav * dt;
        float d = powf(p->drag, dt * 60.0f);
        p->vel = Vector2Scale(p->vel, d);
        p->pos = Vector2Add(p->pos, Vector2Scale(p->vel, dt));
    }

    for (int i = 0; i < MAX_POPUPS; i++)
    {
        if (!popups[i].active) continue;
        popups[i].life -= dt;
        popups[i].pos.y -= 40.0f * dt;
        if (popups[i].life <= 0.0f) popups[i].active = false;
    }
}

/*----------------------------------------------------------------------------*/
/* Bullets                                                                    */
/*----------------------------------------------------------------------------*/
static void SpawnBullet(Vector2 pos, Vector2 vel, float radius, float life,
                        bool fromPlayer, int bounces, Color color)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        if (bullets[i].active) continue;
        bullets[i] = (Bullet){ pos, vel, radius, life, life, 1.0f, 0.0f, bounces,
                               0, -1, fromPlayer, false, false, true, color };
        return;
    }
}

/* An enemy's thrown charge: falls, and goes off on a fuse rather than on
   contact. Marked explosive so UpdateBullets routes its death through
   BomberBlast instead of simply removing it. */
static void SpawnEnemyBomb(Vector2 pos, Vector2 vel, float fuse, Color color)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        if (bullets[i].active) continue;
        bullets[i] = (Bullet){ pos, vel, 8.0f, fuse, fuse, 1.0f, BOMB_GRAV, 0,
                               0, -1, false, true, false, true, color };
        return;
    }
}

/* Lob a charge so that it lands ON `target` after exactly BOMB_FLIGHT seconds.
   Solved rather than aimed: with the flight time fixed these two components are
   the only velocity that arrives there, which is what makes a thrown bomb read
   as a place to leave rather than as a shot to dodge. No lead is applied
   anywhere - it always aims at where you ARE, so moving is always right. */
static void ThrowBombAt(Vector2 from, Vector2 target, Color c)
{
    Vector2 d = Vector2Subtract(target, from);
    Vector2 v = { d.x / BOMB_FLIGHT,
                  (d.y - 0.5f * BOMB_GRAV * BOMB_FLIGHT * BOMB_FLIGHT) / BOMB_FLIGHT };

    SpawnEnemyBomb(from, v, BOMB_FLIGHT + 0.25f, c);
}

static void SpawnPlayerShot(Vector2 pos, Vector2 vel, const WeaponDef *w)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        if (bullets[i].active) continue;
        bullets[i] = (Bullet){ pos, vel, w->bulletRadius * UpSizeMul(), w->life, w->life,
                               w->damage * PlayerDamageMul(), w->bulletGravity,
                               w->bounces + (HasAug(UP_RICOCHET) ? RICOCHET_BOUNCE : 0),
                               w->pierce + upStacks[UP_PIERCE]
                                         + (HasAug(UP_DEVOUR) ? DEVOUR_PIERCE : 0), -1,
                               true, w->explosive, w->slash, true, w->color };
        bullets[i].origin = pos;
        return;
    }
}

/*----------------------------------------------------------------------------*/
/* Enemies                                                                    */
/*----------------------------------------------------------------------------*/
static Color EnemyColor(const Enemy *e);
static void  DamageEnemy(Enemy *e, float dmg, Vector2 from);
static void  HurtPlayer(Vector2 from);
static void  EndRun(void);

static Enemy *FreeEnemy(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (!enemies[i].active) return &enemies[i];
    return NULL;
}

/* Returns the enemy it placed, or NULL if the arena is full - callers that
   need to adjust the newcomer (the splitting boss) use that handle. */
static Enemy *SpawnEnemy(EnemyType type, Vector2 pos, int tier)
{
    Enemy *e = FreeEnemy();
    if (!e) return NULL;

    memset(e, 0, sizeof(Enemy));
    e->type   = type;
    e->pos    = pos;
    e->tier   = tier;
    e->active = true;
    e->spawnT = 0.65f;
    e->rot    = RandF(0.0f, 360.0f);

    switch (type)
    {
        case EN_CHASER:
            e->radius   = 15.0f;
            e->maxHp    = 2.0f;
            e->rotSpeed = RandF(-180.0f, 180.0f);
            break;

        case EN_DASHER:
            e->radius   = 18.0f;
            e->maxHp    = 4.0f;
            e->rotSpeed = 55.0f;
            e->phase    = 0;
            break;

        /* Heavier and slower off the mark than a dasher, and worth more, because
           surviving it is three dodges rather than one. */
        case EN_RUSHER:
            e->radius   = 22.0f;
            e->maxHp    = 6.0f;
            e->rotSpeed = 35.0f;
            e->phase    = 0;
            e->timer    = RandF(0.4f, 1.0f);
            e->gen      = RUSH_DASHES;
            break;

        case EN_SPLITTER:
            e->radius   = (tier == 0) ? 24.0f : 13.0f;
            e->maxHp    = (tier == 0) ? 4.0f : 2.0f;
            e->rotSpeed = RandF(-90.0f, 90.0f);
            e->timer2   = RandF(0.0f, 6.28f);
            break;

        /* Plants itself and shoots. It cannot chase you, so it punishes hanging
           still in its line instead - the one enemy that makes the arena itself
           feel dangerous rather than the things running at you. */
        case EN_TURRET:
            e->radius   = 20.0f;
            e->maxHp    = 6.0f;
            e->rotSpeed = 0.0f;
            e->timer    = RandF(1.0f, 2.0f);
            break;

        /* Slow, and lethal exactly where it dies. Killing one at range is free;
           letting it reach you costs a heart even if you never touched it. */
        case EN_BOMBER:
            e->radius   = 17.0f;
            e->maxHp    = 3.0f;
            e->rotSpeed = RandF(-70.0f, 70.0f);
            break;

        /* Fragile once the plate is gone - the shield is where its durability
           lives, not the body behind it. */
        case EN_SHIELDER:
            e->radius    = 21.0f;
            e->maxHp     = 7.0f;
            e->shieldMax = SHIELD_HP;
            e->rotSpeed  = 0.0f;        /* a shield that spun would be unreadable */
            e->timer2    = RandF(0.0f, 2.0f * PI);  /* shield starts anywhere */
            e->timer     = 1.0f;
            e->phase     = 0;
            break;

        /* Stays out at range and rains arcs in. Thin, because the counter is
           supposed to be flying at it - and flying at it is the one thing its
           own bombs make expensive. */
        case EN_BOMBARDIER:
            e->radius   = 19.0f;
            e->maxHp    = 4.0f;
            e->rotSpeed = RandF(-40.0f, 40.0f);
            e->timer    = RandF(1.2f, 2.2f);
            e->timer2   = RandF(0.0f, 6.28f);       /* drift phase */
            break;

        case EN_BOSS:
            e->radius   = (tier == BK_CHARGER) ? 40.0f :
                          (tier == BK_VORTEX)  ? 42.0f :
                          (tier == BK_LANCER)  ? 38.0f :
                          (tier == BK_MORTAR)  ? 44.0f : 46.0f;
            e->rotSpeed = 25.0f;
            e->timer    = 2.0f;
            e->timer2   = 6.0f;     /* first add is not immediate either */
            e->spawnT   = 1.2f;
            /* The charger closes distance for a living, so it gets less health
               to chew through. The summoner gets less than it looks like it
               should because it splits twice (see KillEnemy): seven bodies at
               0.26 per generation add up to about 1.8x this number, which lands
               the whole fight at roughly 1.3x a normal boss. */
            /* The vortex never stops applying pressure - there is no recovery
               window to punish the way the charger and the summoner have - so
               it is the shortest fight of the four on paper.
             *
             * The linear term sets the opening fights; the CYCLE term is what
             * keeps a boss a boss. Every fifth wave is another lap through the
             * roster, and by then the player has had four to six more upgrade
             * picks - compounding ones. So the boss compounds too, per lap
             * rather than per wave: lap 1 is untouched, lap 4 (wave 20) is
             * about 2.2x, lap 6 (wave 30) about 3.7x. Bosses are the only
             * enemy that should ever feel like a wall, which is why this is
             * far steeper than the rank-and-file ramp above. */
            {
                int   lap    = wave / 5;    /* 1 at wave 5, 6 at wave 30 */
                float growth = powf(BOSS_LAP_GROWTH, (float)(lap - 1));

                e->maxHp = (26.0f + wave * 5.0f) * growth *
                           ((tier == BK_CHARGER) ? 0.78f :
                            (tier == BK_SUMMONER) ? 0.72f :
                            (tier == BK_VORTEX) ? 0.85f :
                            (tier == BK_BULWARK) ? 0.70f :
                            (tier == BK_LANCER) ? 0.80f :
                            (tier == BK_MORTAR) ? 0.90f : 1.0f);
            }

            /* The bulwark keeps half its durability OUTSIDE its body, in a
               plate you are meant to go around rather than through. Flank it
               and the fight is the shortest of the seven; stand in front and
               you pay for both pools. `timer2` is the plate's angle here, not
               an add timer - each kind reads these fields its own way. */
            if (tier == BK_BULWARK)
            {
                e->shieldMax = e->maxHp * 0.5f;
                e->timer2    = RandF(0.0f, 2.0f * PI);
            }
            break;
    }
    /* Enemy counts hit their caps around wave 8-12, after which every wave
       would be identical. Rather than flooding the screen (which just gets
       unreadable), late waves keep escalating through health instead. Early
       waves are untouched, so the opening stays exactly as tuned.
     *
     * Past wave 16 a second, COMPOUNDING term joins the linear one. The linear
     * ramp alone loses: upgrades compound (each pick multiplies what the last
     * one built on), so a straight line falls further behind every wave, and
     * somewhere in the twenties the run stops being a fight. 3.5% a wave is
     * deliberately small - it is worth nothing at wave 17 and about +60% by
     * wave 30, which is meant to restore a difficulty curve, not a wall. */
    if (type != EN_BOSS)        /* the boss already scales its own health */
    {
        float over = (float)wave - 8.0f;
        float mul  = (over > 0.0f) ? 1.0f + 0.10f * over : 1.0f;

        float late = (float)wave - 16.0f;
        if (late > 0.0f) mul *= powf(1.035f, late);

        /* And a third, steeper term from 40. By then the player has taken
           twenty-odd upgrade picks, every one of them multiplying what the
           last built on, plus a permanent slice of attack power from every
           single wave clear - growth the 3.5% ramp above stopped tracking
           somewhere in the thirties. 5.5% a wave puts wave 50 at roughly
           27x a wave-8 body against the old 17x, which is what makes the
           forties a climb again rather than a victory lap. */
        float deep = (float)wave - 40.0f;
        if (deep > 0.0f) mul *= powf(1.055f, deep);

        e->maxHp     *= mul;
        e->shieldMax *= mul;    /* zero for everything without a plate */
    }

    e->hp     = e->maxHp;
    e->shield = e->shieldMax;

    /* Ground spawns burst upward out of the floor, so the telegraph reads as
       something climbing out rather than blinking into existence. */
    if (pos.y > GROUND_Y - 70.0f)
    {
        e->vel = (Vector2){ RandF(-70.0f, 70.0f), RandF(-300.0f, -180.0f) };
        EmitBurst((Vector2){ pos.x, (float)GROUND_Y }, -PI / 2.0f, 0.5f, 14,
                  90.0f, 300.0f, 0.45f, 4.0f, EnemyColor(e), 320.0f);
    }
    return e;
}

/* The player lives high up (the floor burns), so pressure has to come from
   below to mean anything. Most enemies climb out of the ground; the rest come
   in low from the sides. Nothing drops from above any more. */
static Vector2 RandomEdgeSpawn(void)
{
    int side = GetRandomValue(0, 4);
    if (side <= 2) return (Vector2){ RandF(90.0f, SCREEN_W - 90.0f), GROUND_Y - 26.0f };
    if (side == 3) return (Vector2){ -40.0f, RandF(GROUND_Y - 280.0f, GROUND_Y - 60.0f) };
    return (Vector2){ SCREEN_W + 40.0f, RandF(GROUND_Y - 280.0f, GROUND_Y - 60.0f) };
}

static int EnemyScore(const Enemy *e)
{
    switch (e->type)
    {
        case EN_CHASER:   return 100;
        case EN_DASHER:   return 200;
        case EN_RUSHER:   return 320;
        case EN_SPLITTER: return (e->tier == 0) ? 120 : 60;
        case EN_TURRET:   return 250;
        case EN_BOMBER:   return 150;
        case EN_BOMBARDIER: return 280;
        /* Paid for the flying you had to do, not for the health you chewed. */
        case EN_SHIELDER: return 300;
        /* A summoner pays out across the bodies it breaks into rather than all
           at once. Halving per generation while the count doubles means every
           generation is worth the same 900, so all three together still come to
           the 2700 the fight has always paid. */
        case EN_BOSS:     return (e->tier == BK_SUMMONER) ? (900 >> e->gen) : 2000;
    }
    return 0;
}

static Color EnemyColor(const Enemy *e)
{
    switch (e->type)
    {
        case EN_CHASER:   return (Color){ 255,  80,  90, 255 };
        case EN_DASHER:   return (Color){ 255, 170,  40, 255 };
        /* Reads as a hotter dasher, because that is what it is. */
        case EN_RUSHER:   return (Color){ 255,  95,  30, 255 };
        case EN_SPLITTER: return (Color){ 190, 100, 255, 255 };
        case EN_TURRET:   return (Color){ 255, 215,  90, 255 };
        case EN_BOMBER:   return (Color){ 255, 120,  55, 255 };
        /* Olive: close enough to the suicide bomber to say "explosives", far
           enough that you never mistake one for the other at a glance. */
        case EN_BOMBARDIER: return (Color){ 180, 200,  80, 255 };
        /* Steel, and the only cold colour in the roster - it is the one enemy
           you are meant to read as armour rather than as an animal. */
        case EN_SHIELDER: return (Color){ 130, 190, 220, 255 };
        case EN_BOSS:
            /* each boss kind gets its own colour - you should know what you are
               fighting before it has done anything */
            if (e->tier == BK_SUMMONER) return (Color){ 170,  95, 255, 255 };
            if (e->tier == BK_CHARGER)  return (Color){ 255, 130,  40, 255 };
            if (e->tier == BK_VORTEX)   return (Color){  90, 225, 235, 255 };
            /* The back three echo the enemy they are the big version of. */
            if (e->tier == BK_BULWARK)  return (Color){ 165, 205, 235, 255 };
            if (e->tier == BK_LANCER)   return (Color){ 255,  85,  45, 255 };
            if (e->tier == BK_MORTAR)   return (Color){ 205, 225,  95, 255 };
            return (Color){ 255, 60, 160, 255 };
    }
    return RED;
}

/* Does a direct hit arriving from `from` land on the shielder's shield rather
   than on the thing behind it? Only ever true for a SHIELDER, so callers can
   ask without checking the type first.

   Deliberately NOT folded into DamageEnemy: everything that routes through
   there without a meaningful direction - explosions, backblast, death blast,
   thorns - is supposed to go straight through the shield. Blocking belongs to
   the two places that fire something along a line, and nowhere else. */
/* The shielder and its boss-sized version share every rule about plates. */
static bool HasShield(const Enemy *e)
{
    return (e->type == EN_SHIELDER) ||
           (e->type == EN_BOSS && e->tier == BK_BULWARK);
}

static bool ShieldBlocks(const Enemy *e, Vector2 from)
{
    if (!HasShield(e) || e->shield <= 0.0f) return false;

    Vector2 d = Vector2Subtract(from, e->pos);
    if (Vector2LengthSqr(d) < 0.0001f) return false;    /* dead centre: no side */

    return fabsf(AngleDelta(atan2f(d.y, d.x), e->timer2)) < SHIELD_ARC;
}

/* The spark that tells you the shot did nothing. Loud on purpose - a hit that
   silently fails to damage reads as the game dropping your input.
 *
 * `from` is where the shot came from, not where it struck: a beam is tested
 * against its muzzle half a screen away, so the sparks are placed on the shield
 * surface here rather than by the caller. */
static void ShieldHit(Enemy *e, float dmg, Vector2 from, Color c)
{
    Vector2 d  = Vector2Subtract(from, e->pos);
    float   a  = (Vector2LengthSqr(d) > 0.0001f) ? atan2f(d.y, d.x) : e->timer2;
    Vector2 at = Vector2Add(e->pos, FromAngle(a, e->radius + 8.0f));

    e->guardT = 0.22f;
    e->shield -= dmg;

    if (e->shield > 0.0f)
    {
        EmitBurst(at, a, 0.9f, 7, 90.0f, 300.0f, 0.28f, 3.0f, c, 120.0f);
        PlaySfx(&sfxHit, 1.9f);
        return;
    }

    /* Gone for good - the plate does not come back, so the work you put into
       breaking it is banked. Announced loudly, because the enemy is a
       completely different proposition from this frame on. */
    e->shield = 0.0f;
    EmitBurst(at, a, 1.5f, 26, 120.0f, 460.0f, 0.55f, 4.5f,
              (Color){ 225, 245, 255, 255 }, 260.0f);
    AddShake(5.0f);
    PlaySfx(&sfxBoom, 1.6f);
}

/* Kills in one flight before the HUD bothers to say so - 4 is x3.0, the point
   where touching down actually costs the player something. */
#define COMBO_HUD_FROM 4

static float ComboMultiplier(void)
{
    float m = 1.0f + player.combo * 0.5f;
    return (m > 10.0f) ? 10.0f : m;
}

/* A bomber going off. Unlike the player's own explosions this one hurts, and
   it does not care whose side anything is on.
 *
 * `radius` is a parameter because the same blast serves the suicide bomber and
 * the bombardier's thrown charge, and those two have to be different sizes or
 * the thrown one - which arrives from off-screen and costs the enemy nothing -
 * would be strictly the better version of walking into you. */
static void BomberBlast(Vector2 pos, float radius)
{
    Color fire = { 255, 130, 60, 255 };

    if (player.alive &&
        Vector2Distance(player.pos, pos) < radius + PLAYER_RADIUS)
        HurtPlayer(pos);

    /* Friendly fire, one level deep only - a packed cluster should reward a
       well-placed shot, not erase the whole wave in a single frame. */
    if (blastDepth < 1)
    {
        blastDepth++;
        for (int i = 0; i < MAX_ENEMIES; i++)
        {
            Enemy *o = &enemies[i];
            if (!o->active || o->spawnT > 0.0f) continue;
            if (Vector2Distance(o->pos, pos) > radius + o->radius) continue;
            DamageEnemy(o, 3.0f, pos);
        }
        blastDepth--;
    }

    /* Everything visual is scaled off the radius, so the flash always tells the
       truth about how far the blast reached. */
    float k = radius / BOMBER_RADIUS;
    AddShock(pos, radius, fire);
    EmitBurst(pos, 0.0f, PI, (int)(38 * k), 90.0f, 460.0f * k, 0.5f, 6.0f, fire, 240.0f);
    EmitBurst(pos, 0.0f, PI, 12, 30.0f, 140.0f, 0.7f, 8.0f,
              (Color){ 255, 240, 200, 255 }, 30.0f);
    AddShake(9.0f * k);
    PlaySfx(&sfxBoom, 1.3f + 0.3f * (1.0f - k));
}

static void KillEnemy(Enemy *e, bool byPlayer)
{
    /* READ EVERYTHING OFF `e` FIRST, AND NEVER TOUCH IT AGAIN AFTER A SPAWN.
       This function deactivates the corpse and then spawns from its position,
       and FreeEnemy() hands out the first inactive slot - which is very often
       this one. The newcomer's memset then wipes `e` under our feet. That is
       not theoretical: it is what made the summoner split forever, because
       `ch->gen = e->gen + 1` read a freshly-zeroed gen and so every fragment
       came out as generation 1, no matter how deep the fight already was.
       Fragments were also born at radius 0 and health 0 for the same reason. */
    const EnemyType type   = e->type;
    const int       tier   = e->tier;
    const int       gen    = e->gen;
    const Vector2   pos    = e->pos;
    const float     radius = e->radius;
    const float     maxHp  = e->maxHp;
    const int       payout = EnemyScore(e);
    const Color     c      = EnemyColor(e);

    e->active = false;

    if (type == EN_BOMBER) BomberBlast(pos, BOMBER_RADIUS);

    EmitBurst(pos, 0.0f, PI, (type == EN_BOSS) ? 90 : 22,
              60.0f, (type == EN_BOSS) ? 520.0f : 300.0f,
              0.6f, radius * 0.35f, c, 260.0f);
    EmitBurst(pos, 0.0f, PI, 8, 20.0f, 90.0f, 0.9f, radius * 0.5f,
              (Color){ 255, 255, 255, 255 }, 40.0f);

    if (type == EN_BOSS)
    {
        /* Fragments announce themselves, but a full boss-death flash per body
           would be exhausting - so only the original gets the works. */
        float k = (gen > 0) ? 0.40f : 1.0f;
        AddShake(20.0f * k);
        flashWhite = 0.5f * k;
        PlaySfx(&sfxBoom, RandF(0.9f, 1.05f) + 0.25f * gen);
    }
    else
    {
        AddShake(4.0f);
        PlaySfx(&sfxKill, RandF(0.9f, 1.2f));
    }

    if (type == EN_SPLITTER && tier == 0)
    {
        for (int i = 0; i < 3; i++)
        {
            Vector2 p = Vector2Add(pos, FromAngle(RandF(0.0f, 2.0f * PI), 18.0f));
            SpawnEnemy(EN_SPLITTER, p, 1);
        }
    }

    /* The purple boss comes apart the way the purple ordinary enemy does, and
       does it twice - seven bodies. What keeps that readable is that the
       fragments LOSE something: only the original summons and fires its
       five-shot fan, everything it breaks into has no guns at all and simply
       charges. So the fight gets busier while each individual threat gets
       simpler and flimsier, down to quarters that die in a shot or two. */
    if (type == EN_BOSS && tier == BK_SUMMONER && gen < SUMMONER_SPLITS)
    {
        for (int i = 0; i < 2; i++)
        {
            Vector2 p = Vector2Add(pos,
                                   FromAngle(RandF(0.0f, 2.0f * PI), radius * 0.7f));
            Enemy  *ch = SpawnEnemy(EN_BOSS, p, BK_SUMMONER);
            if (ch == NULL) break;          /* arena full - just do not split */

            ch->gen    = gen + 1;
            ch->radius = radius * SUMMONER_CHILD_R;
            ch->maxHp  = maxHp  * SUMMONER_CHILD_HP;
            ch->hp     = ch->maxHp;
            /* Each generation telegraphs a little quicker than the last, so the
               later rounds feel like a burst rather than another slow reveal. */
            ch->spawnT = 0.45f - 0.10f * gen;
            ch->timer  = RandF(0.6f, 1.2f); /* staggered so they never fire in unison */
            ch->timer2 = RandF(1.2f, 2.4f);
            ch->vel    = FromAngle(RandF(0.0f, 2.0f * PI), RandF(150.0f, 260.0f));
        }
    }

    if (!byPlayer) return;

    /* No-touch combo: every airborne kill stacks the multiplier. */
    if (!player.grounded) { player.combo++; player.comboFlash = 0.45f; }

    QuestOnKill(Vector2Distance(pos, player.pos));

    int gained = (int)(payout * ComboMultiplier() * UpScoreMul());
    score += gained;
    AddPopup(pos, gained, c);

    hitstop = 0.045f;

    /* DEATH BLAST: chains, but only one level deep so a big wave cannot
       cascade into an instant screen wipe. */
    if (upStacks[UP_DEATHBLAST] > 0 && blastDepth < 1)
    {
        float   blastR = DeathBlastRadius();
        float   dmg    = 1.5f * upStacks[UP_DEATHBLAST];
        Vector2 at     = pos;
        Color   fire   = (Color){ 255, 150, 100, 255 };

        blastDepth++;
        for (int i = 0; i < MAX_ENEMIES; i++)
        {
            Enemy *o = &enemies[i];
            /* `o == e` is no longer a self-check - the corpse is inactive, and
               anything living in that slot now is a freshly spawned fragment,
               which the spawnT test skips anyway. */
            if (!o->active || o->spawnT > 0.0f) continue;
            if (Vector2Distance(o->pos, at) > blastR + o->radius) continue;
            DamageEnemy(o, dmg, at);
        }
        blastDepth--;

        AddShock(at, blastR, fire);
        EmitBurst(at, 0.0f, PI, 16, 80.0f, blastR * 3.0f, 0.35f, 4.0f, fire, 40.0f);
    }

    /* LIFESTEAL: every kill is its own roll. */
    {
        if (player.stealCd <= 0.0f && player.hp < PlayerMaxHp() &&
            RollChance(LifestealChance()))
        {
            player.hp++;
            player.stealCd = StealCooldown();
            AddPopupEx(player.pos, 0, "체력 +1", (Color){ 120, 255, 210, 255 });
            PlaySfx(&sfxReload, 1.5f);
        }
    }
}

static void DamageEnemy(Enemy *e, float dmg, Vector2 from)
{
    /* A corpse still has hp <= 0, so a second hit landing on it in the same
       frame would run KillEnemy again - and a summoner would split twice off
       one death. Every caller filters on active already; this just makes the
       invariant impossible to break from a new one. */
    if (!e->active || e->spawnT > 0.0f) return;

    /* MUT_WARD: a flat share of incoming damage simply does not land. Rolled
       per damage instance rather than per shot, so a piercing round gets a
       fresh roll on every body and a shotgun pattern is not all-or-nothing.
       Loud on purpose - a hit that silently does nothing reads as the game
       dropping the input, which is the same reason ShieldHit sparks. */
    if (mutOn[MUT_WARD] && RollChance(MUT_WARD_CHANCE))
    {
        Vector2 off = Vector2Subtract(e->pos, from);
        float   ang = (Vector2LengthSqr(off) > 0.0001f) ? atan2f(off.y, off.x) + PI
                                                        : 0.0f;
        e->guardT = 0.18f;
        EmitBurst(Vector2Add(e->pos, FromAngle(ang, e->radius)), ang, 1.0f, 6,
                  70.0f, 240.0f, 0.22f, 3.0f, MUTATORS[MUT_WARD].color, 90.0f);
        PlaySfx(&sfxHit, 2.1f);
        return;
    }

    /* EXECUTE: a finisher, applied here rather than at the muzzle so it reads
       every source - a rocket, a beam and a stray pellet all execute. Bosses
       are exempt: a boss folding at a third of its bar is not a boss fight. */
    if (HasAug(UP_EXECUTE) && e->type != EN_BOSS &&
        e->hp <= e->maxHp * EXECUTE_BELOW)
    {
        dmg *= EXECUTE_MUL;
        AddShock(e->pos, e->radius + 16.0f, (Color){ 255, 110, 130, 255 });
    }

    e->hp -= dmg;
    e->hitFlash = 0.12f;

    Vector2 kick = Vector2Subtract(e->pos, from);
    EmitBurst(e->pos, atan2f(kick.y, kick.x), 0.8f, 5, 60.0f, 220.0f, 0.25f, 3.0f,
              (Color){ 255, 230, 160, 255 }, 200.0f);

    if (e->hp <= 0.0f) KillEnemy(e, true);
    else               PlaySfx(&sfxHit, RandF(1.0f, 1.3f));
}

/* Bazooka blast: hits everything nearby and shoves the player, so a rocket
   fired at your own feet is a legitimate (and expensive) escape move.
 *
 * `tint` is the firing weapon's colour, carried in on the bullet. The blast
 * belongs to the gun that threw it - a pink rocket has no business bursting
 * orange - and it also means the radius ring reads as yours at a glance, even
 * when a bomber is going off somewhere else on screen. */
static void Explode(Vector2 pos, float damage, Color tint)
{
    Color fire = tint;

    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        Enemy *e = &enemies[i];
        if (!e->active || e->spawnT > 0.0f) continue;
        if (Vector2Distance(e->pos, pos) > BLAST_RADIUS + e->radius) continue;
        DamageEnemy(e, damage, pos);
    }

    if (player.alive)
    {
        Vector2 away = Vector2Subtract(player.pos, pos);
        float   d    = Vector2Length(away);
        if (d < BLAST_RADIUS && d > 0.001f)
        {
            float k = 1.0f - d / BLAST_RADIUS;
            player.vel = Vector2Add(player.vel,
                                    Vector2Scale(Vector2Scale(away, 1.0f / d), 620.0f * k));
        }
    }

    /* The ring is the same BLAST_RADIUS the loops above just tested against,
       so a rocket's reach can be learned instead of guessed from the spray. */
    AddShock(pos, BLAST_RADIUS, fire);

    EmitBurst(pos, 0.0f, PI, 46, 90.0f, 520.0f, 0.55f, 7.0f, fire, 260.0f);
    EmitBurst(pos, 0.0f, PI, 16, 30.0f, 160.0f, 0.8f, 9.0f,
              (Color){ 255, 240, 200, 255 }, 40.0f);
    AddShake(13.0f);
    flashWhite = 0.28f;
    PlaySfx(&sfxBoom, 1.15f);
}

/* Railgun: damages every enemy on the line at once, then leaves a fading trace. */
static void FireBeam(Vector2 from, Vector2 dir, const WeaponDef *w)
{
    Vector2 to = Vector2Add(from, Vector2Scale(dir, BEAM_RANGE));

    /* Single source of truth: the width the damage loop tests against and the
       width DrawBeams strokes are the same number. The bullet weapons get the
       size upgrade through Bullet.radius; hitscan has no projectile to carry
       it, so it comes through here instead. */
    float hw = BEAM_RADIUS * UpSizeMul();

    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        Enemy *e = &enemies[i];
        if (!e->active || e->spawnT > 0.0f) continue;
        if (DistToSegment(e->pos, from, to) > e->radius + hw) continue;

        /* Hitscan is still a shot fired down a line, so the shield eats it the
           same way it eats a bullet. It does not stop the beam for anyone
           standing behind it, though - the beam pierces, and a shield is not
           cover for the whole room. */
        if (ShieldBlocks(e, from))
        {
            ShieldHit(e, w->damage * PlayerDamageMul(), from, w->color);
            continue;
        }

        DamageEnemy(e, w->damage * PlayerDamageMul(), from);
    }

    for (int i = 0; i < MAX_BEAMS; i++)
    {
        if (beams[i].active) continue;
        beams[i] = (Beam){ from, to, 0.22f, w->color, hw, true };
        break;
    }
}

static void UpdateBeams(float dt)
{
    for (int i = 0; i < MAX_BEAMS; i++)
    {
        if (!beams[i].active) continue;
        beams[i].life -= dt;
        if (beams[i].life <= 0.0f) beams[i].active = false;
    }
}

static void DrawBeams(void)
{
    for (int i = 0; i < MAX_BEAMS; i++)
    {
        const Beam *b = &beams[i];
        if (!b->active) continue;
        float t = b->life / 0.22f;
        /* All three layers scale together, so an unupgraded shot is drawn
           exactly as it always was (s == 1) and every stack visibly fattens
           the whole beam rather than just its glow. */
        float s = b->hw / BEAM_RADIUS;
        DrawLineEx(b->a, b->b, 16.0f * s * t, Fade(b->color, 0.20f * t));
        DrawLineEx(b->a, b->b,  7.0f * s * t, Fade(b->color, 0.75f * t));
        DrawLineEx(b->a, b->b,  2.5f * s * t, Fade(WHITE, 0.9f * t));
    }
}

/*----------------------------------------------------------------------------*/
/* Pickups                                                                    */
/*                                                                            */
/* Drops high in the arena, so the reward for being hurt is a flight challenge */
/* rather than a free heal.                                                    */
/*----------------------------------------------------------------------------*/
#define PICKUP_LIFETIME  14.0f
#define PICKUP_RADIUS    17.0f

static Color PickupColor(const Pickup *p)
{
    if (p->kind == PK_HEAL)   return (Color){ 110, 255, 170, 255 };
    if (p->kind == PK_DAMAGE) return (Color){ 255, 170, 120, 255 };
    return WEAPONS[p->weapon].color;
}

static void SpawnPickup(PickupKind kind, WeaponType weapon)
{
    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        if (pickups[i].active) continue;

        pickups[i].kind   = kind;
        pickups[i].weapon = weapon;
        pickups[i].pos    = (Vector2){ RandF(140.0f, SCREEN_W - 140.0f),
                                       RandF(110.0f, GROUND_Y - 220.0f) };
        pickups[i].bob    = RandF(0.0f, 6.28f);
        pickups[i].life   = PICKUP_LIFETIME;
        pickups[i].active = true;

        EmitBurst(pickups[i].pos, 0.0f, PI, 18, 60.0f, 200.0f, 0.6f, 4.0f,
                  PickupColor(&pickups[i]), 0.0f);
        PlaySfx(&sfxReload, 0.7f);
        return;
    }
}

/* Never offer the weapon already in hand - a drop should always be a change. */
static WeaponType RandomOtherWeapon(void)
{
    WeaponType w;
    do { w = (WeaponType)GetRandomValue(1, WP_COUNT - 1); } while (w == player.weapon);
    return w;
}

static void GivePickup(const Pickup *p, Vector2 at)
{
    Color c = PickupColor(p);

    if (p->kind == PK_HEAL)
    {
        if (player.hp < PlayerMaxHp()) player.hp++;
        AddPopupEx(at, 0, "체력 +1", c);
    }
    else if (p->kind == PK_DAMAGE)
    {
        /* GrantDamage does its own popup and ring, so this branch only has to
           hand over the stacks - the shared burst below still fires. */
        GrantDamage(DMG_CRATE, at,
                    TextFormat("공격력 +%.0f%%", DMG_PER_STACK * DMG_CRATE * 100.0f));
    }
    else
    {
        player.weapon = p->weapon;      /* kept until you die */
        AddPopupEx(at, 0, WEAPONS[p->weapon].name, c);
    }

    EmitBurst(at, 0.0f, PI, 34, 70.0f, 340.0f, 0.7f, 5.0f, c, 60.0f);
    flashWhite = 0.22f;
    PlaySfx(&sfxReload, 1.4f);
}

static void UpdatePickups(float dt)
{
    if (healSpawnT > 0.0f)
    {
        healSpawnT -= dt;
        if (healSpawnT <= 0.0f) SpawnPickup(PK_HEAL, WP_PISTOL);
    }
    if (weaponSpawnT > 0.0f)
    {
        weaponSpawnT -= dt;
        if (weaponSpawnT <= 0.0f) SpawnPickup(PK_WEAPON, RandomOtherWeapon());
    }
    if (dmgSpawnT > 0.0f)
    {
        dmgSpawnT -= dt;
        if (dmgSpawnT <= 0.0f) SpawnPickup(PK_DAMAGE, WP_PISTOL);
    }

    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        Pickup *p = &pickups[i];
        if (!p->active) continue;

        p->bob  += dt * 3.0f;
        p->life -= dt;
        if (p->life <= 0.0f) { p->active = false; continue; }

        Vector2 at = { p->pos.x, p->pos.y + sinf(p->bob) * 5.0f };
        if (!player.alive ||
            !CheckCollisionCircles(at, PICKUP_RADIUS, player.pos, PLAYER_RADIUS)) continue;

        p->active = false;
        GivePickup(p, at);
    }
}

/*----------------------------------------------------------------------------*/
/* Waves                                                                      */
/*----------------------------------------------------------------------------*/
static void PushSpawn(EnemyType t, int count)
{
    for (int i = 0; i < count && spawnCount < MAX_SPAWNQUEUE; i++)
        spawnQueue[spawnCount++] = (int)t;
}

static void ShuffleQueue(void)
{
    for (int i = spawnCount - 1; i > 0; i--)
    {
        int j = GetRandomValue(0, i);
        int t = spawnQueue[i]; spawnQueue[i] = spawnQueue[j]; spawnQueue[j] = t;
    }
}

static void StartWave(int n)
{
    wave        = n;
    spawnCount  = 0;
    spawnIdx    = 0;
    spawnTimer  = (n == 1) ? 1.6f : 0.4f;   /* let the first wave settle in */
    waveBannerT = 1.6f;
    waveCleared = false;

    /* Mutators are armed on the upgrade screen and cashed in here, once,
       for exactly one wave. Promoting rather than reading `mutArmed` in
       place is what guarantees a rule cannot leak into the wave after:
       every StartWave overwrites the live set, including with nothing. */
    mutCount = 0;
    for (int i = 0; i < MUT_COUNT; i++)
    {
        mutOn[i]    = mutArmed[i];
        mutArmed[i] = false;
        if (mutOn[i]) mutCount++;
    }
    mutBannerT = (mutCount > 0) ? 2.6f : 0.0f;

    /* The pause that will follow THIS wave. Beating a boss earns a real
       breather rather than being thrown straight back into the grinder. */
    intermission = (n % 5 == 0) ? 5.0f : 1.2f;
    PlaySfx(&sfxWave, 1.0f);

    /* One wave in three carries a challenge, and never a boss wave - a boss is
       already asking for the player's whole attention, and a second goal laid
       over it would only be noise. */
    /* The failure debuff is scoped to one wave, and this is where that is
       enforced - not in UpdateQuest, which would have to know whether the
       wave had ended. */
    questPenalty = 0.0f;

    if (n >= 3 && n % QUEST_EVERY == 0 && n % 5 != 0) StartQuest(n);
    else                                              questState = QS_NONE;

    /* A pack is offered only when you are actually hurt, and never on
       consecutive waves - otherwise damage stops meaning anything. */
    /* Boss waves always offer one: surviving that far should extend the run,
       not just hand you a bigger enemy. */
    if (player.hp < PlayerMaxHp() && (n - lastHealWave >= 2 || n % 5 == 0))
    {
        healSpawnT   = 4.0f;
        lastHealWave = n;
    }

    /* A weapon shows up every other wave from wave 2 - often enough to shape a
       run, rare enough that the pistol is still the baseline you fly on. */
    if (n >= 2 && n - lastWeaponWave >= 2)
    {
        weaponSpawnT   = 7.0f;
        lastWeaponWave = n;
    }

    /* Every third wave. Unlike the heal crate this one is not conditional on
       how the run is going - it is the reward for engaging with the arena, and
       gating it behind being hurt would mean a clean run gets less for playing
       better. Dropped late in the wave so it is a detour under pressure. */
    if (n >= 4 && n - lastDmgWave >= 3)
    {
        dmgSpawnT   = 9.0f;
        lastDmgWave = n;
    }

    if (n % 5 == 0)
    {
        /* The escort grows with the run but stops well short of the queue, so
           a deep boss wave stays a boss fight rather than a swarm. */
        int escort = 3 + n / 5;
        if (escort > 12) escort = 12;

        PushSpawn(EN_BOSS, 1);
        PushSpawn(EN_CHASER, escort);
        /* Deep boss waves bring the arena furniture along too: turrets deny
           the easy hovering spots you would otherwise fight the boss from. */
        if (n >= 15) PushSpawn(EN_TURRET, 1 + n / 25);

        /* The back three bosses bring their own kind with them - the small
           version of the mechanic you are about to fight, which is the closest
           thing this game has to a tutorial for it. Must match the cycle in
           UpdateSpawning exactly or the escort turns up for the wrong boss. */
        {
            int kind = ((n / 5) - 1) % BK_COUNT;
            if (kind < 0) kind = 0;

            if      (kind == BK_BULWARK) PushSpawn(EN_SHIELDER,   2);
            else if (kind == BK_LANCER)  PushSpawn(EN_RUSHER,     2);
            else if (kind == BK_MORTAR)  PushSpawn(EN_BOMBARDIER, 2);
        }
        ShuffleQueue();
        /* boss first so the telegraph reads clearly */
        for (int i = 0; i < spawnCount; i++)
        {
            if (spawnQueue[i] == EN_BOSS)
            {
                spawnQueue[i] = spawnQueue[0];
                spawnQueue[0] = EN_BOSS;
                break;
            }
        }
        return;
    }

    /* The run never ends, so the caps cannot be flat any more - they creep up
       with the wave instead. They still creep rather than explode: a readable
       arena beats a crowded one, and health scaling (see SpawnEnemy) carries
       most of the late-game difficulty. Worst case with every cap saturated is
       51 spawns against MAX_SPAWNQUEUE's 64 - PushSpawn drops the overflow
       silently, and the kinds pushed last are the newest ones, so this margin
       is worth re-checking whenever a cap goes up.

       New kinds arrive on a schedule so each one gets a wave or two to be
       learned on its own before it becomes part of the mix. */
    int chasers   = 2 + (n * 2) / 3;
    int dashers   = (n >=  2) ? (n + 1) / 3   : 0;
    int splitters = (n >=  3) ? n / 3         : 0;
    int turrets   = (n >=  6) ? (n - 4) / 5   : 0;
    int bombers   = (n >=  8) ? (n - 6) / 4   : 0;
    /* THE LATE ROSTER STARTS AT 20, NOT IN THE TEENS. These three all punish a
       build rather than a reflex - a shielder wants a way around it, a lobber
       wants somewhere else to be, a rusher wants sustained thrust - and a run
       that has not been handed its upgrades yet has none of those answers. Met
       at wave 11 they were simply unfair; met at 20 they are the reason the
       twenties stop being the same wave over and over.
     *
     * One wave apart so each still gets its own introduction, and the counts
       start at one rather than ramping up from zero. */
    int shielders = (n >= 20) ? 1 + (n - 20) / 6 : 0;
    int lobbers   = (n >= 21) ? 1 + (n - 21) / 8 : 0;
    int rushers   = (n >= 22) ? 1 + (n - 22) / 6 : 0;

    int chaserCap = 10 + n / 8;  if (chaserCap > 20) chaserCap = 20;
    int dashCap   =  3 + n / 14; if (dashCap   >  7) dashCap   = 7;
    int splitCap  =  3 + n / 16; if (splitCap  >  6) splitCap  = 6;

    if (chasers   > chaserCap) chasers   = chaserCap;
    if (dashers   > dashCap)   dashers   = dashCap;
    if (splitters > splitCap)  splitters = splitCap;
    if (turrets   > 4)         turrets   = 4;
    if (bombers   > 6)         bombers   = 6;
    /* Three is a wall you have to fly around, four is a room you cannot shoot
       into - and unlike the others these do not die to a stray shot, so the
       cap has to be tighter than the arithmetic suggests. */
    if (shielders > 3)         shielders = 3;
    /* Two lobbers already means the arena has nowhere quiet in it; a third
       would just be an unreadable carpet of blast rings. */
    if (lobbers   > 2)         lobbers   = 2;
    if (rushers   > 3)         rushers   = 3;

    PushSpawn(EN_CHASER, chasers);
    PushSpawn(EN_DASHER, dashers);
    PushSpawn(EN_RUSHER, rushers);
    PushSpawn(EN_SPLITTER, splitters);
    PushSpawn(EN_TURRET, turrets);
    PushSpawn(EN_BOMBER, bombers);
    PushSpawn(EN_BOMBARDIER, lobbers);
    PushSpawn(EN_SHIELDER, shielders);
    ShuffleQueue();
}

static int LiveEnemies(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) if (enemies[i].active) n++;
    return n;
}

static void UpdateSpawning(float dt)
{
    if (spawnIdx < spawnCount)
    {
        spawnTimer -= dt;
        if (spawnTimer <= 0.0f)
        {
            EnemyType t = (EnemyType)spawnQueue[spawnIdx++];
            Vector2 pos;
            int     tier = 0;

            if (t == EN_BOSS)
            {
                pos = (Vector2){ RandF(300.0f, SCREEN_W - 300.0f), GROUND_Y - 70.0f };
                /* One kind per boss wave, cycling - wave 5 barrage, 10
                   summoner, 15 charger, 20 vortex, 25 barrage again. */
                tier = ((wave / 5) - 1) % BK_COUNT;
                if (tier < 0) tier = 0;
            }
            /* A turret is an emplacement: it belongs on the floor, spread out,
               never dropped into the middle of the air. */
            else if (t == EN_TURRET)
                pos = (Vector2){ RandF(120.0f, SCREEN_W - 120.0f), GROUND_Y - 30.0f };
            else pos = RandomEdgeSpawn();

            SpawnEnemy(t, pos, tier);

            /* Early waves breathe. The first couple of minutes are for learning
               to fly on recoil, not for dodging a crowd - so the gap between
               spawns starts long and decays toward a constant stream. */
            spawnTimer = 0.28f + 1.35f * powf(0.80f, (float)(wave - 1));
        }
    }
    else if (LiveEnemies() == 0)
    {
        bool bossWave = (wave % 5 == 0);

        /* the moment the last enemy dies */
        if (!waveCleared)
        {
            waveCleared = true;

            /* Every wave pays out, boss or not. The cards are a choice you can
               get wrong; this is the one reward that just accrues, so a run
               that keeps drawing upgrades it cannot use still gets stronger. */
            /* Resolved before the clear bonus so the two popups stack in the
               order they were earned. */
            if (questState == QS_ACTIVE && questKind == Q_NOHIT) QuestSucceed();

            GrantDamage(DMG_WAVE_CLEAR,
                        (Vector2){ player.pos.x, player.pos.y - 30.0f },
                        TextFormat("공격력 +%.0f%%",
                                   DMG_PER_STACK * DMG_WAVE_CLEAR * 100.0f));

            /* A mutated wave pays on top of the ordinary clear: permanent
               attack power per rule survived, and one reroll. The reroll
               is the half that lands immediately - it is spendable on the
               very next card screen, which is what makes a hard wave feel
               like it bought something rather than merely cost time. */
            if (mutCount > 0)
            {
                bool gotReroll = (rerolls < REROLL_MAX);
                if (gotReroll) rerolls++;

                GrantDamage(DMG_MUTATOR * mutCount,
                            (Vector2){ player.pos.x, player.pos.y - 62.0f },
                            gotReroll
                                ? TextFormat("변칙 돌파  공격력 +%.0f%%  새로고침 +1",
                                             DMG_PER_STACK * DMG_MUTATOR * mutCount * 100.0f)
                                : TextFormat("변칙 돌파  공격력 +%.0f%%",
                                             DMG_PER_STACK * DMG_MUTATOR * mutCount * 100.0f));
                flashWhite = 0.3f;
                AddShake(8.0f);
            }

            if (bossWave)
            {
                if (rerolls < REROLL_MAX)
                {
                    rerolls += REROLL_PER_BOSS;
                    if (rerolls > REROLL_MAX) rerolls = REROLL_MAX;
                }

                int before = player.hp;
                player.hp += 2;
                if (player.hp > PlayerMaxHp()) player.hp = PlayerMaxHp();
                if (player.hp > before)
                    AddPopupEx(player.pos, 0, TextFormat("체력 +%d", player.hp - before),
                               (Color){ 120, 255, 170, 255 });

                EmitBurst(player.pos, 0.0f, PI, 40, 60.0f, 320.0f, 0.9f, 5.0f,
                          (Color){ 120, 255, 170, 255 }, 20.0f);
                PlaySfx(&sfxWave, 0.8f);
            }
        }

        intermission -= dt;
        if (intermission <= 0.0f)
        {
            score += (long)(250.0f * wave * UpScoreMul() * (bossWave ? 3.0f : 1.0f)
                                  * (1.0f + 0.6f * mutCount));

            /* The next wave's rules are rolled here, before either branch,
               because they are not a choice any more - the card screen only
               gets to REPORT them, and a wave with no card screen still has
               to arrive carrying them. */
            RollMutators();

            /* Upgrades every other wave, and always after a boss. */
            if (wave % 2 == 0 || bossWave)
            {
                RollUpgrades();
                pendingPicks = bossWave ? 2 : 1;
                upgradeT     = 0.0f;
                state        = ST_UPGRADE;
            }
            else StartWave(wave + 1);
        }
    }
}

/*----------------------------------------------------------------------------*/
/* Player                                                                     */
/*----------------------------------------------------------------------------*/
static void HurtPlayer(Vector2 from)
{
    if (player.invuln > 0.0f || !player.alive) return;

    /* AEGIS: rolled fresh on every incoming hit. When it lands the hit simply
       never happened - no health, no combo broken - and the short mercy window
       stops a crowd from rolling against you several times in one frame. */
    if (player.aegisCd <= 0.0f && RollChance(AegisChance()))
    {
        player.shieldFlash = 0.35f;
        player.invuln      = 0.35f;
        player.aegisCd     = AegisCooldown();

        Vector2 push = Vector2Subtract(player.pos, from);
        if (Vector2LengthSqr(push) < 0.001f) push = (Vector2){ 0, -1 };
        player.vel = Vector2Add(player.vel,
                                Vector2Scale(Vector2Normalize(push), 240.0f));

        AddShock(player.pos, PLAYER_RADIUS + 22.0f, (Color){ 120, 210, 255, 255 });
        EmitBurst(player.pos, 0.0f, PI, 22, 90.0f, 300.0f, 0.4f, 4.0f,
                  (Color){ 120, 210, 255, 255 }, 40.0f);
        AddPopupEx(player.pos, 0, "무력화!", (Color){ 120, 210, 255, 255 });
        AddShake(6.0f);
        PlaySfx(&sfxReload, 0.55f);
        return;
    }

    /* MUT_SAVAGE takes two hearts a hit. The mercy window widens with it -
       at the old 1.3s a crowd could take four of five hearts before you had
       any say in it, which is a coin flip rather than a harder wave. */
    player.hp -= MutContactDmg();
    if (player.hp < 0) player.hp = 0;
    player.invuln = MutMercy();
    player.combo  = 0;

    /* Only a hit that actually landed breaks it - an AEGIS negation returns
       above this line, and it is supposed to mean the hit never happened. */
    if (questState == QS_ACTIVE && questKind == Q_NOHIT) QuestFail();

    Vector2 push = Vector2Subtract(player.pos, from);
    if (Vector2LengthSqr(push) < 0.001f) push = (Vector2){ 0, -1 };
    player.vel = Vector2Add(player.vel, Vector2Scale(Vector2Normalize(push), 320.0f));

    AddShake(12.0f);
    flashWhite = 0.35f;
    PlaySfx(&sfxHurt, 1.0f);
    EmitBurst(player.pos, 0.0f, PI, 24, 80.0f, 320.0f, 0.5f, 4.0f,
              (Color){ 255, 90, 90, 255 }, 200.0f);

    /* THORNS: punish whatever just touched you */
    if (upStacks[UP_THORNS] > 0)
    {
        float radius = 90.0f + 30.0f * upStacks[UP_THORNS];
        float dmg    = 2.0f * upStacks[UP_THORNS];

        for (int i = 0; i < MAX_ENEMIES; i++)
        {
            Enemy *e = &enemies[i];
            if (!e->active || e->spawnT > 0.0f) continue;
            if (Vector2Distance(e->pos, player.pos) > radius + e->radius) continue;
            DamageEnemy(e, dmg, player.pos);
        }
        AddShock(player.pos, radius, (Color){ 255, 160, 220, 255 });
        EmitBurst(player.pos, 0.0f, PI, 30, 120.0f, radius * 3.5f, 0.45f, 5.0f,
                  (Color){ 255, 160, 220, 255 }, 30.0f);

        /* ...and sometimes the blast gives the point back. Guarded on hp > 0
           so it can heal a wound but never undo a death. */
        if (player.thornCd <= 0.0f && player.hp > 0 && player.hp < PlayerMaxHp() &&
            RollChance(ThornsHealChance()))
        {
            player.hp++;
            player.thornCd = ThornCooldown();
            AddPopupEx(player.pos, 0, "체력 +1", (Color){ 255, 160, 220, 255 });
            PlaySfx(&sfxReload, 1.5f);
        }
    }

    if (player.hp <= 0)
    {
        player.alive  = false;
        player.deathT = 0.0f;
        AddShake(24.0f);
        flashWhite = 0.7f;
        PlaySfx(&sfxBoom, 0.8f);
        EmitBurst(player.pos, 0.0f, PI, 70, 60.0f, 560.0f, 0.9f, 5.0f,
                  (Color){ 120, 230, 255, 255 }, 300.0f);
    }
}

/* 0 while the floor is safe, ramping to 1 at the moment it erupts.
   Deliberately reads the gauge alone rather than also demanding `grounded`:
   heat now builds while skimming, and a warning you cannot see while it is
   filling would be worse than no warning at all. */
static float GroundDanger(void)
{
    float total = UpGroundTime();
    float warn  = total * 0.5f;
    if (player.groundT <= warn) return 0.0f;
    return Clamp((player.groundT - warn) / (total - warn), 0.0f, 1.0f);
}

/* The floor is a pit stop, not a home. Stand too long and it throws you off. */
static void GroundErupt(void)
{
    Vector2 base = { player.pos.x, (float)GROUND_Y };

    EmitBurst(base, -PI / 2.0f, 0.45f, 44, 220.0f, 760.0f, 0.7f, 6.0f,
              (Color){ 255, 150, 50, 255 }, 520.0f);
    EmitBurst(base, -PI / 2.0f, 1.3f, 22, 80.0f, 320.0f, 0.5f, 5.0f,
              (Color){ 255, 235, 160, 255 }, 420.0f);

    AddShake(15.0f);
    flashWhite = 0.3f;
    PlaySfx(&sfxBoom, 1.35f);

    HurtPlayer(base);               /* damage still respects invulnerability... */
    player.vel.y    = -620.0f;      /* ...but the launch always happens */
    player.grounded = false;
    player.groundT  = 0.0f;
}

static void FirePlayer(Vector2 target)
{
    Vector2 dir = Vector2Subtract(target, player.pos);
    if (Vector2LengthSqr(dir) < 1.0f) dir = (Vector2){ 1, 0 };
    dir = Vector2Normalize(dir);

    const WeaponDef *w = &WEAPONS[player.weapon];

    float   aimA    = atan2f(dir.y, dir.x);
    Vector2 shotDir = FromAngle(aimA, 1.0f);
    Vector2 muzzle  = Vector2Add(player.pos, Vector2Scale(shotDir, PLAYER_RADIUS + 8.0f));

    /* SCATTER adds shots to whatever the weapon already fires, and widens the
       cone with them - piling extra pellets into the same narrow angle would
       just be a damage multiplier wearing a shotgun costume. */
    int   pellets = w->pellets + upStacks[UP_SCATTER];
    float spread  = w->spread + 0.075f * upStacks[UP_SCATTER];

    /* Pellets are fanned evenly across the cone rather than each one rolling
       its own angle. Independent rolls clump toward the middle, which is why a
       random shotgun reads as a slightly wider pistol; a fan actually spreads.
       The jitter is only there to keep the pattern from looking stamped. */
    for (int i = 0; i < pellets; i++)
    {
        float t = (pellets > 1) ? ((float)i / (float)(pellets - 1)) * 2.0f - 1.0f
                                : 0.0f;
        float a = aimA + spread * t + RandF(-spread, spread) * 0.16f;

        /* The whole game in two lines: shots go forward, you go backward. */
        if (w->hitscan) FireBeam(muzzle, FromAngle(a, 1.0f), w);
        else SpawnPlayerShot(muzzle,
                             Vector2Scale(FromAngle(a, 1.0f),
                                          w->bulletSpeed * UpSpeedMul() * MutBulletMul()
                                                         * RandF(0.92f, 1.08f)), w);
    }

    player.vel = Vector2Add(player.vel,
                            Vector2Scale(shotDir,
                                         -tune.recoilImpulse * w->recoilMul * UpRecoilMul()
                                                             * MutRecoilMul()));

    /* BACKBLAST: the recoil itself hurts whatever is behind you, which is
       exactly where you are about to fly away from. */
    if (upStacks[UP_BACKBLAST] > 0)
    {
        float   radius = BackblastRadius(w);
        float   dmg    = 0.6f * upStacks[UP_BACKBLAST] * w->recoilMul;
        Vector2 at     = Vector2Add(player.pos, Vector2Scale(shotDir, -PLAYER_RADIUS));

        for (int i = 0; i < MAX_ENEMIES; i++)
        {
            Enemy *e = &enemies[i];
            if (!e->active || e->spawnT > 0.0f) continue;
            if (Vector2Distance(e->pos, at) > radius + e->radius) continue;
            DamageEnemy(e, dmg, at);
        }

        /* The ring is how the player learns the range, but an SMG would strobe
           it - so the damage is every shot and the ring is not. */
        if (player.blastT <= 0.0f)
        {
            AddShock(at, radius, (Color){ 180, 160, 255, 255 });
            player.blastT = 0.16f;
        }
    }

    player.cooldown  = tune.fireCooldown * w->cooldownMul * UpFireMul() * MutFireMul();
    player.muzzle    = 0.07f;
    player.fireRingT = FIRE_RING_TIME;
    player.grounded = false;

    if (w->slash)   /* mirror each swing so repeated slashes read as a combo */
    {
        player.swingT    = SWORD_SWING;
        player.swingSide = -player.swingSide;
    }

    float back = aimA + PI;
    EmitBurst(muzzle, aimA, 0.35f + w->spread, 6 + w->pellets, 120.0f, 420.0f,
              0.28f, 4.0f, (Color){ 255, 220, 150, 255 }, 120.0f);
    EmitBurst(player.pos, back, 0.7f, 9, 40.0f, 190.0f * w->recoilMul, 0.5f, 6.0f,
              (Color){ 150, 150, 170, 255 }, -30.0f);

    AddShake(3.0f * w->recoilMul);
    PlaySfx(&sfxShoot, RandF(0.92f, 1.10f) / w->recoilMul);
}

static void UpdatePlayer(float dt)
{
    if (!player.alive)
    {
        player.deathT += dt;
        return;
    }

    Vector2 mouse = VirtualMouse();
    Vector2 aimV  = Vector2Subtract(mouse, player.pos);
    if (Vector2LengthSqr(aimV) > 1.0f) player.aim = atan2f(aimV.y, aimV.x);

    if (player.cooldown > 0.0f) player.cooldown -= dt;
    if (player.invuln   > 0.0f) player.invuln   -= dt;
    if (player.muzzle   > 0.0f) player.muzzle   -= dt;
    if (player.swingT   > 0.0f) player.swingT   -= dt;
    if (player.shieldFlash > 0.0f) player.shieldFlash -= dt;
    if (player.aegisCd  > 0.0f) player.aegisCd  -= dt;
    if (player.stealCd  > 0.0f) player.stealCd  -= dt;
    if (player.thornCd  > 0.0f) player.thornCd  -= dt;
    if (player.blastT   > 0.0f) player.blastT   -= dt;
    if (player.fireRingT > 0.0f) player.fireRingT -= dt;
    if (player.comboFlash > 0.0f) player.comboFlash -= dt;

    /* Let go of the target if nothing has landed on it recently. */
    if (focusTimer > 0.0f)
    {
        focusTimer -= dt;
        if (focusTimer <= 0.0f) { focusEnemy = -1; focusHits = 0; }
    }

    player.blinkTimer -= dt;
    if (player.blinkTimer < -0.12f) player.blinkTimer = RandF(2.0f, 5.0f);

    /* Ammo is infinite - the fire interval alone rations how much you can fly. */
#ifdef SHOTCOIL_CAPTURE
    if ((IsMouseButtonDown(MOUSE_BUTTON_LEFT) || shotFire) && player.cooldown <= 0.0f)
#else
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && player.cooldown <= 0.0f)
#endif
        FirePlayer(mouse);

    /* Integration */
    player.vel.y += MutGravity() * dt;
    player.vel = Vector2Scale(player.vel, powf(MutAirDrag(), dt * 60.0f));
    player.pos = Vector2Add(player.pos, Vector2Scale(player.vel, dt));

    /* Arena collision - walls bounce, which is what makes trick shots exist. */
    bool wasGrounded = player.grounded;
    player.grounded = false;

    if (player.pos.x < PLAYER_RADIUS)
    {
        player.pos.x = PLAYER_RADIUS;
        if (player.vel.x < -60.0f)
        {
            EmitBurst(player.pos, 0.0f, 1.0f, 6, 60.0f, 200.0f, 0.3f, 3.0f, GRAY, 200.0f);
            AddShake(2.0f);
        }
        player.vel.x = -player.vel.x * 0.5f;
    }
    if (player.pos.x > SCREEN_W - PLAYER_RADIUS)
    {
        player.pos.x = SCREEN_W - PLAYER_RADIUS;
        if (player.vel.x > 60.0f)
        {
            EmitBurst(player.pos, PI, 1.0f, 6, 60.0f, 200.0f, 0.3f, 3.0f, GRAY, 200.0f);
            AddShake(2.0f);
        }
        player.vel.x = -player.vel.x * 0.5f;
    }
    if (player.pos.y < PLAYER_RADIUS)
    {
        player.pos.y = PLAYER_RADIUS;
        player.vel.y = -player.vel.y * 0.5f;
    }
    if (player.pos.y > GROUND_Y - PLAYER_RADIUS)
    {
        player.pos.y = GROUND_Y - PLAYER_RADIUS;
        if (player.vel.y > 260.0f)
        {
            EmitBurst((Vector2){ player.pos.x, GROUND_Y }, -PI / 2, 1.1f, 10,
                      80.0f, 260.0f, 0.35f, 4.0f, (Color){ 120, 130, 150, 255 }, 400.0f);
            AddShake(fminf(6.0f, player.vel.y * 0.012f));
        }
        player.vel.y = -player.vel.y * 0.22f;
        player.vel.x *= 0.86f;
        if (fabsf(player.vel.y) < 90.0f)
        {
            player.vel.y    = 0.0f;
            player.grounded = true;
        }
    }

    if (player.grounded)
    {
        player.airTime = 0.0f;
        if (!wasGrounded && player.combo > 0) player.combo = 0;   /* touching down burns the combo */

        /* Same rule for the airborne challenge: the counter is a streak, and
           the floor is what ends it. Not a failure though - you can start the
           streak again as long as the wave is still running. */
        if (!wasGrounded && questState == QS_ACTIVE && questKind == Q_AIRKILL)
            questProg = 0;
    }
    else player.airTime += dt;

    /* Floor heat.
     *
     * This used to be a plain stopwatch that ran while grounded and snapped to
     * zero the instant you were not - which made the floor the safest place in
     * the game. Lifting a few pixels wiped the whole timer, so you could skim
     * the ground indefinitely and never pay for it, and the escape hatch cost
     * one downward shot.
     *
     * Now it is a gauge. It fills anywhere in the low band (fastest while
     * actually standing, slower while skimming just above it) and only drains
     * once you are properly airborne - and drains slower than it fills, so a
     * hop buys a little relief but never a reset. Living on the floor is
     * finally a decision with a cost. */
    {
        float total = UpGroundTime();
        float prev  = player.groundT;

        if (player.grounded)
            player.groundT += dt;
        else if (player.pos.y > GROUND_Y - GROUND_HEAT_BAND)
            player.groundT += dt * GROUND_SKIM_RATE;
        else
        {
            player.groundT -= dt * GROUND_COOL_RATE;
            if (player.groundT < 0.0f) player.groundT = 0.0f;
        }

        if (prev <= total * 0.5f && player.groundT > total * 0.5f) PlaySfx(&sfxWarn, 1.0f);
        if (player.groundT >= total) GroundErupt();
    }

    /* speed trail */
    if (Vector2Length(player.vel) > 420.0f && GetRandomValue(0, 1) == 0)
        EmitBurst(player.pos, 0.0f, PI, 1, 0.0f, 25.0f, 0.28f, 5.0f,
                  (Color){ 90, 200, 255, 255 }, 0.0f);
}

/*----------------------------------------------------------------------------*/
/* Enemy behaviour                                                            */
/*----------------------------------------------------------------------------*/
static void UpdateEnemies(float dt)
{
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        Enemy *e = &enemies[i];
        if (!e->active) continue;

        e->rot += e->rotSpeed * dt;
        if (e->hitFlash > 0.0f) e->hitFlash -= dt;

        if (e->spawnT > 0.0f)
        {
            e->spawnT -= dt;
            continue;
        }

        /* MUT_BARRAGE. Every `e->timer` / `e->timer2` countdown in the
           behaviour switch below runs on this instead of dt, so one
           constant speeds up every attack cadence, wind-up and recovery
           in the game at once. Deliberately NOT applied to spawnT: the
           telegraph that says a body is about to become solid is the
           one clock the player must always get in full. */
        float edt = dt * MutEnemyRate();

        Vector2 toPlayer = Vector2Subtract(player.pos, e->pos);
        float   dist     = Vector2Length(toPlayer);
        Vector2 dir      = (dist > 0.001f) ? Vector2Scale(toPlayer, 1.0f / dist)
                                           : (Vector2){ 0, 0 };

        switch (e->type)
        {
            case EN_CHASER:
            {
                /* Aim where the player is going, not where they are - otherwise
                   simply drifting sideways dodges every chaser for free. */
                Vector2 lead  = Vector2Add(player.pos, Vector2Scale(player.vel, 0.22f));
                Vector2 ldir  = Vector2Normalize(Vector2Subtract(lead, e->pos));
                float   speed = fminf(340.0f, 145.0f + wave * 12.0f);

                /* The cap is reached at wave 16, and from there the basic enemy
                   stopped changing at all. A small amount of extra closing
                   speed past that point is worth more than the same difficulty
                   spent on health - a faster chaser is a shorter reaction
                   window, a tougher one is just a longer trigger pull. */
                if (wave > 16) speed = fminf(400.0f, speed + (wave - 16) * 2.5f);

                e->vel = Vector2Add(e->vel, Vector2Scale(ldir, 1000.0f * dt));
                if (Vector2Length(e->vel) > speed)
                    e->vel = Vector2Scale(Vector2Normalize(e->vel), speed);
            } break;

            case EN_SPLITTER:
            {
                e->timer2 += dt * 2.0f;
                float speed = (e->tier == 0) ? 70.0f : 130.0f;
                Vector2 drift = { cosf(e->timer2) * 60.0f, sinf(e->timer2 * 1.3f) * 40.0f };
                Vector2 want  = Vector2Add(Vector2Scale(dir, speed), drift);
                e->vel = Vector2Lerp(e->vel, want, 1.0f - powf(0.02f, dt));
            } break;

            case EN_DASHER:
            {
                /* Commits to a direction, shows it, then charges. It threatens
                   space instead of shooting, so you dodge with movement rather
                   than by out-aiming a projectile. */
                e->timer -= edt;
                switch (e->phase)
                {
                    case 0:     /* close in until it is worth charging */
                        e->vel = Vector2Lerp(e->vel, Vector2Scale(dir, 115.0f),
                                             1.0f - powf(0.05f, dt));
                        if (dist < 300.0f)
                        {
                            e->phase = 1;
                            e->timer = DASH_WINDUP;
                            e->lockDir = dir;      /* committed - you can dodge it */
                            PlaySfx(&sfxWarn, 1.5f);
                        }
                        break;

                    case 1:     /* wind-up: brakes hard and telegraphs the lane */
                        e->vel = Vector2Scale(e->vel, powf(0.02f, dt));
                        if (e->timer <= 0.0f)
                        {
                            e->phase = 2;
                            e->timer = DASH_TIME;
                            e->vel   = Vector2Scale(e->lockDir, DASH_SPEED);
                            EmitBurst(e->pos, atan2f(-e->lockDir.y, -e->lockDir.x), 0.6f,
                                      12, 90.0f, 300.0f, 0.35f, 4.0f, EnemyColor(e), 120.0f);
                            PlaySfx(&sfxShoot, 0.5f);
                        }
                        break;

                    case 2:     /* the charge itself, overshooting past the player */
                        if (GetRandomValue(0, 1) == 0)
                            EmitBurst(e->pos, 0.0f, PI, 1, 0.0f, 40.0f, 0.3f, 4.0f,
                                      EnemyColor(e), 0.0f);
                        if (e->timer <= 0.0f) { e->phase = 3; e->timer = DASH_RECOVER; }
                        break;

                    default:    /* recovery: slow and open to punishment */
                        e->vel = Vector2Scale(e->vel, powf(0.08f, dt));
                        if (e->timer <= 0.0f) e->phase = 0;
                        break;
                }
            } break;

            /* Locks on, charges, and does it three times without letting go.
               The lock TRACKS you while it winds up - unlike the dasher, whose
               lane is fixed the moment it commits - so the dodge has to come
               late. Between dashes it re-locks in a third of the time, which is
               where the pressure is: you get one beat to have already moved. */
            case EN_RUSHER:
            {
                e->timer -= edt;
                switch (e->phase)
                {
                    case 0:     /* close to a range worth charging from */
                        e->vel = Vector2Lerp(e->vel, Vector2Scale(dir, 130.0f),
                                             1.0f - powf(0.06f, dt));
                        if (dist < 420.0f && e->timer <= 0.0f)
                        {
                            e->phase   = 1;
                            e->timer   = RUSH_LOCK;
                            e->gen     = RUSH_DASHES;
                            e->lockDir = dir;
                            PlaySfx(&sfxWarn, 1.1f);
                        }
                        break;

                    case 1:     /* lock-on: still aiming, nearly stationary */
                        e->vel     = Vector2Scale(e->vel, powf(0.05f, dt));
                        e->lockDir = Vector2Lerp(e->lockDir, dir,
                                                 1.0f - powf(0.02f, dt));
                        if (Vector2LengthSqr(e->lockDir) > 0.0001f)
                            e->lockDir = Vector2Normalize(e->lockDir);

                        if (e->timer <= 0.0f)
                        {
                            e->phase = 2;
                            e->timer = RUSH_TIME;
                            e->vel   = Vector2Scale(e->lockDir, RUSH_SPEED);
                            EmitBurst(e->pos, atan2f(-e->lockDir.y, -e->lockDir.x), 0.6f,
                                      14, 100.0f, 340.0f, 0.35f, 4.5f, EnemyColor(e), 130.0f);
                            AddShake(3.0f);
                            PlaySfx(&sfxShoot, 0.45f);
                        }
                        break;

                    case 2:     /* the dash - straight, committed, fast */
                        EmitBurst(e->pos, 0.0f, PI, 1, 0.0f, 60.0f, 0.3f, 4.0f,
                                  EnemyColor(e), 0.0f);
                        if (e->timer <= 0.0f)
                        {
                            /* Out of dashes it has to rest, and that rest is
                               the whole window you get to kill it in. */
                            if (--e->gen > 0) { e->phase = 1; e->timer = RUSH_RELOCK; }
                            else              { e->phase = 3; e->timer = 1.15f;       }
                        }
                        break;

                    default:    /* spent */
                        e->vel = Vector2Scale(e->vel, powf(0.06f, dt));
                        if (e->timer <= 0.0f) { e->phase = 0; e->timer = 0.5f; }
                        break;
                }
            } break;

            /* Hangs back and lobs. It is the only enemy that makes a PLACE
               dangerous rather than a line or a body, so the answer is to stop
               hovering - or to close the distance it is trying to keep. */
            case EN_BOMBARDIER:
            {
                /* Backs off when you come at it, drifts closer when you are far,
                   and always sways - a stationary lobber is free target practice. */
                e->timer2 += dt;
                float want  = (dist < 300.0f) ? -150.0f : ((dist > 460.0f) ? 110.0f : 0.0f);
                Vector2 sway = { cosf(e->timer2 * 1.6f) * 55.0f,
                                 sinf(e->timer2 * 2.1f) * 45.0f };

                e->vel = Vector2Lerp(e->vel,
                                     Vector2Add(Vector2Scale(dir, want), sway),
                                     1.0f - powf(0.08f, dt));

                e->timer -= edt;
                if (e->timer <= 0.0f)
                {
                    float gap = 2.9f - wave * 0.04f;
                    e->timer  = (gap < 1.5f) ? 1.5f : gap;

                    ThrowBombAt(Vector2Add(e->pos, (Vector2){ 0.0f, -e->radius }),
                                player.pos, (Color){ 210, 235, 110, 255 });
                    EmitBurst(e->pos, -PI / 2.0f, 0.7f, 5, 40.0f, 130.0f, 0.3f, 3.0f,
                              EnemyColor(e), 90.0f);
                    PlaySfx(&sfxShoot, 0.9f);
                }
            } break;

            /* Fixed gun. Never moves, never closes - it just makes one lane of
               the arena expensive and dares you to keep flying through it. */
            case EN_TURRET:
            {
                e->vel = Vector2Scale(e->vel, powf(0.02f, dt));  /* plants itself */

                e->timer -= edt;
                if (e->timer <= 0.0f)
                {
                    float gap = 2.6f - wave * 0.05f;
                    e->timer = (gap < 1.1f) ? 1.1f : gap;

                    float base = atan2f(dir.y, dir.x);
                    for (int k = -1; k <= 1; k++)
                    {
                        float a = base + k * 0.17f;
                        SpawnBullet(Vector2Add(e->pos, FromAngle(a, e->radius)),
                                    FromAngle(a, 430.0f), 6.0f, 3.2f, false, 0,
                                    (Color){ 255, 215, 90, 255 });
                    }
                    AddShake(2.0f);
                    PlaySfx(&sfxShoot, 0.72f);
                }
            } break;

            /* A walking wall. It has no gun and no reach worth speaking of -
               what it does is make one direction of approach worthless, and
               then walk that direction at you.
             *
             * The shield lags on purpose. Turning at SHIELD_TURN means it can
             * always catch a player who hovers and trades, and can never catch
             * one who commits to going around - which is the exact skill the
             * recoil movement is for. */
            case EN_SHIELDER:
            {
                if (e->guardT > 0.0f) e->guardT -= dt;

                bool bare = (e->shield <= 0.0f);

                /* Committed and winded both cost it the ability to keep up with
                   you. That is the punish window: the bash is not just an
                   attack to dodge, it is the moment its back becomes reachable
                   for anyone who dodged sideways instead of backwards. */
                float want = atan2f(dir.y, dir.x);
                float turn = SHIELD_TURN * ((e->phase >= 2) ? 0.35f : 1.0f) * dt;
                float diff = AngleDelta(want, e->timer2);
                e->timer2 += (fabsf(diff) < turn) ? diff : ((diff > 0.0f) ? turn : -turn);

                /* Facing is measured against the SHIELD, not the body: being in
                   front is what makes the lunge fair. It only ever charges the
                   side it can defend, so it cannot open with its back to you.
                   Stripped of the plate it stops caring and comes at you from
                   wherever it happens to be. */
                bool infront = bare ||
                               fabsf(AngleDelta(want, e->timer2)) < SHIELD_ARC;

                e->timer -= edt;
                switch (e->phase)
                {
                    case 0:     /* advance, slowly, like something heavy */
                        e->vel = Vector2Lerp(e->vel,
                                             Vector2Scale(dir, bare ? SHIELD_BROKEN_SPEED
                                                                    : SHIELD_SPEED),
                                             1.0f - powf(0.06f, dt));
                        if (infront && dist < 260.0f && e->timer <= 0.0f)
                        {
                            e->phase   = 1;
                            e->timer   = 0.55f;
                            e->lockDir = dir;
                            PlaySfx(&sfxWarn, 1.25f);
                        }
                        break;

                    case 1:     /* plant the feet - the tell that it is coming */
                        e->vel = Vector2Scale(e->vel, powf(0.03f, dt));
                        if (e->timer <= 0.0f)
                        {
                            e->phase = 2;
                            e->timer = 0.30f;
                            e->vel   = Vector2Scale(e->lockDir, SHIELD_LUNGE);
                            EmitBurst(e->pos, atan2f(e->lockDir.y, e->lockDir.x), 0.5f,
                                      10, 80.0f, 260.0f, 0.3f, 4.0f, EnemyColor(e), 100.0f);
                            PlaySfx(&sfxShoot, 0.65f);
                        }
                        break;

                    case 2:     /* the bash itself - shield first, so still safe */
                        if (e->timer <= 0.0f) { e->phase = 3; e->timer = 0.75f; }
                        break;

                    default:    /* winded, and the shield drifts - go around now */
                        e->vel = Vector2Scale(e->vel, powf(0.05f, dt));
                        if (e->timer <= 0.0f) { e->phase = 0; e->timer = 1.4f; }
                        break;
                }
            } break;

            /* Walks it in. The threat is not the contact, it is the crater. */
            case EN_BOMBER:
            {
                e->timer += dt;      /* drives the fuse blink in DrawEnemy */
                Vector2 want = Vector2Scale(dir, fminf(190.0f, 90.0f + wave * 6.0f));
                e->vel = Vector2Lerp(e->vel, want, 1.0f - powf(0.08f, dt));
            } break;

            case EN_BOSS:
            {
                switch (e->tier)
                {
                    /* Ring barrage: fills the room and forces you to keep
                       flying rather than hover and trade. */
                    case BK_BARRAGE:
                    default:
                    {
                        e->vel = Vector2Lerp(e->vel, Vector2Scale(dir, 80.0f),
                                             1.0f - powf(0.2f, dt));

                        e->timer -= edt;
                        if (e->timer <= 0.0f)
                        {
                            e->timer = 2.4f;
                            int   n    = 14;
                            float base = RandF(0.0f, 2.0f * PI);
                            for (int k = 0; k < n; k++)
                            {
                                float a = base + (2.0f * PI * k) / n;
                                SpawnBullet(Vector2Add(e->pos, FromAngle(a, e->radius)),
                                            FromAngle(a, 260.0f), 6.0f, 4.0f, false, 0,
                                            (Color){ 255, 110, 190, 255 });
                            }
                            AddShake(6.0f);
                            PlaySfx(&sfxShoot, 0.45f);
                        }

                        /* Adds are seasoning, not the meal - the barrage is what
                           this fight is about, and a stream of chasers on top of
                           it just buries the pattern you are supposed to read. */
                        e->timer2 -= edt;
                        if (e->timer2 <= 0.0f)
                        {
                            e->timer2 = 10.0f;
                            SpawnEnemy(EN_CHASER,
                                       Vector2Add(e->pos, (Vector2){ RandF(-60, 60), 60 }), 0);
                        }
                    } break;

                    /* Summoner: keeps its distance and buries you in adds, so
                       the real question is whether you can reach it at all. */
                    case BK_SUMMONER:
                    {
                        /* Generation decides the whole personality here. The
                           original keeps its distance and hides behind adds;
                           everything it breaks into has no guns left and comes
                           straight at you like oversized splitters, faster the
                           smaller it gets. Seven-plus shooters would be an
                           unreadable bullet storm, so the gate is gen 0 only -
                           it does not loosen as the split count grows. */
                        float want = (e->gen > 0)
                                         ? 200.0f + 60.0f * e->gen
                                         : ((dist < 320.0f) ? -110.0f : 55.0f);
                        e->vel = Vector2Lerp(e->vel, Vector2Scale(dir, want),
                                             1.0f - powf(0.25f, dt));

                        e->timer -= edt;
                        if (e->timer <= 0.0f && e->gen == 0)
                        {
                            e->timer = 1.7f;

                            int   fan  = 2;      /* 5 shots - only gen 0 fires */
                            float base = atan2f(dir.y, dir.x);
                            for (int k = -fan; k <= fan; k++)
                                SpawnBullet(Vector2Add(e->pos, FromAngle(base + k * 0.20f, e->radius)),
                                            FromAngle(base + k * 0.20f, 300.0f), 6.0f, 3.5f, false, 0,
                                            (Color){ 200, 140, 255, 255 });
                            PlaySfx(&sfxShoot, 0.55f);
                        }

                        /* Only the original summons - the halves doing it as
                           well would bury the arena the moment it broke apart. */
                        e->timer2 -= edt;
                        if (e->timer2 <= 0.0f && e->gen == 0)
                        {
                            /* One add per call instead of two, and less often.
                               The splitting fight already supplies plenty of
                               bodies on its own once it starts coming apart. */
                            e->timer2 = 5.5f;
                            Vector2 at = Vector2Add(e->pos, (Vector2){ RandF(-80, 80), 70 });
                            SpawnEnemy((GetRandomValue(0, 2) == 0) ? EN_SPLITTER : EN_DASHER,
                                       at, 0);
                            PlaySfx(&sfxWarn, 1.1f);
                        }
                    } break;

                    /* Vortex: the only boss that attacks your MOVEMENT. It drags
                       you toward itself continuously, which turns the core loop
                       inside out - shooting at it shoves you away from it, so
                       the pull and your own gun are pushing you in opposite
                       directions and you have to pick one. Firing away from it
                       is now how you close the distance.
                     *
                     * Nothing here has a safe hovering answer, which is why it
                       carries the least health of the four. */
                    case BK_VORTEX:
                    {
                        /* Drifts, rather than chases - it does not need to
                           close, the arena comes to it. */
                        e->vel = Vector2Lerp(e->vel, Vector2Scale(dir, 34.0f),
                                             1.0f - powf(0.4f, dt));

                        e->timer  -= edt;
                        e->timer2 -= edt;

                        if (e->phase == 0)
                        {
                            /* The pull. Eased off inside 130px so point-blank
                               is a place you can still fly out of, and capped
                               by distance so the far side of the arena is not
                               a free ride either. */
                            if (player.alive)
                            {
                                float d    = fmaxf(dist, 1.0f);
                                float grip = (d < 130.0f) ? (d / 130.0f) : 1.0f;
                                player.vel = Vector2Subtract(
                                    player.vel, Vector2Scale(dir, 480.0f * grip * dt));
                            }

                            /* A rotating spiral rather than the barrage boss's
                               concentric rings: the gaps travel, so standing in
                               one is temporary by construction. */
                            if (e->timer <= 0.0f)
                            {
                                e->timer = 0.24f;
                                e->rot  += 26.0f;   /* also spins the drawn body */

                                float base = e->rot * DEG2RAD;
                                for (int k = 0; k < 2; k++)
                                {
                                    float a = base + k * PI;
                                    SpawnBullet(Vector2Add(e->pos, FromAngle(a, e->radius)),
                                                FromAngle(a, 235.0f), 6.0f, 5.0f, false, 0,
                                                (Color){ 120, 240, 255, 255 });
                                }
                                PlaySfx(&sfxShoot, 1.25f);
                            }

                            if (e->timer2 <= 0.0f)      /* wind up the reversal */
                            {
                                e->phase  = 1;
                                e->timer2 = 0.9f;
                                PlaySfx(&sfxWarn, 0.6f);
                            }
                        }
                        else
                        {
                            /* Wind-up: the pull nearly doubles, so the last
                               second before the blast drags you INTO it. Then
                               it reverses and flings you at a wall.
                             *
                               Same near-taper as the idle pull. Without it the
                               wind-up would deliver you into the body and the
                               contact damage would be scripted rather than
                               something you flew into. */
                            if (player.alive)
                            {
                                float d    = fmaxf(dist, 1.0f);
                                float grip = (d < 130.0f) ? (d / 130.0f) : 1.0f;
                                player.vel = Vector2Subtract(
                                    player.vel, Vector2Scale(dir, 900.0f * grip * dt));
                            }

                            if (e->timer2 <= 0.0f)
                            {
                                e->phase  = 0;
                                e->timer2 = RandF(4.5f, 6.0f);

                                /* ...and now outward: `dir` points from the boss
                                   to you, so this is the one place that ADDS
                                   it. Closer means flung harder, which is what
                                   makes the wind-up's extra pull a trap. */
                                if (player.alive && dist < 460.0f)
                                {
                                    float k = 1.0f - dist / 460.0f;
                                    player.vel = Vector2Add(
                                        player.vel, Vector2Scale(dir, 900.0f * k));
                                }

                                /* The blast is push, not damage - being flung is
                                   punishment enough, and the ring of shot it
                                   leaves behind is what you actually dodge. */
                                float base = RandF(0.0f, 2.0f * PI);
                                for (int k = 0; k < 12; k++)
                                {
                                    float a = base + (2.0f * PI * k) / 12.0f;
                                    SpawnBullet(Vector2Add(e->pos, FromAngle(a, e->radius)),
                                                FromAngle(a, 330.0f), 6.0f, 3.2f, false, 0,
                                                (Color){ 160, 255, 255, 255 });
                                }
                                AddShock(e->pos, 240.0f, (Color){ 120, 240, 255, 255 });
                                AddShake(9.0f);
                                PlaySfx(&sfxBoom, 1.3f);
                            }
                        }
                    } break;

                    /* Bulwark: the shielder, boss-sized, with one change that
                       makes it a fight instead of a chore - the plate SWEEPS at
                       a fixed rate rather than tracking you. There is always an
                       opening and it is always moving, so the fight is about
                       orbiting against the rotation. Break the plate and it
                       stops defending and starts firing in earnest. */
                    case BK_BULWARK:
                    {
                        bool bare = (e->shield <= 0.0f);
                        if (e->guardT > 0.0f) e->guardT -= dt;
                        if (!bare) e->timer2 += 0.8f * dt;

                        /* Holds the middle distance: close enough that going
                           around it is a real trip, far enough that it is not
                           simply a body-block. */
                        float want = (dist < 250.0f) ? -70.0f : 75.0f;
                        e->vel = Vector2Lerp(e->vel, Vector2Scale(dir, want),
                                             1.0f - powf(0.25f, dt));

                        e->timer -= edt;
                        if (e->timer <= 0.0f)
                        {
                            /* Guarded it fires a fan out of the shield face -
                               the front is dangerous as well as useless. Bare
                               it fires everywhere, faster: nothing left to hide
                               behind, so it stops trying. */
                            if (bare)
                            {
                                e->timer   = 1.5f;
                                float base = RandF(0.0f, 2.0f * PI);
                                for (int k = 0; k < 10; k++)
                                {
                                    float a = base + (2.0f * PI * k) / 10.0f;
                                    SpawnBullet(Vector2Add(e->pos, FromAngle(a, e->radius)),
                                                FromAngle(a, 300.0f), 6.0f, 4.0f, false, 0,
                                                (Color){ 200, 230, 255, 255 });
                                }
                                AddShake(6.0f);
                            }
                            else
                            {
                                e->timer = 2.3f;
                                for (int k = -2; k <= 2; k++)
                                {
                                    float a = e->timer2 + k * 0.22f;
                                    SpawnBullet(Vector2Add(e->pos, FromAngle(a, e->radius + 16.0f)),
                                                FromAngle(a, 340.0f), 6.0f, 4.0f, false, 0,
                                                (Color){ 200, 230, 255, 255 });
                                }
                                AddShake(4.0f);
                            }
                            PlaySfx(&sfxShoot, 0.6f);
                        }
                    } break;

                    /* Lancer: the rusher grown up. Four tracking dashes in a
                       row, and this one KEEPS ITS SPEED off the walls, so a
                       missed dash comes back through the room instead of
                       ending. The charger throws one lane at you; this fills
                       the arena with itself and then stands still, exhausted,
                       for a second and a half. */
                    case BK_LANCER:
                    {
                        e->timer -= edt;
                        switch (e->phase)
                        {
                            case 0:     /* stalk */
                                e->vel = Vector2Lerp(e->vel, Vector2Scale(dir, 120.0f),
                                                     1.0f - powf(0.1f, dt));
                                if (e->timer <= 0.0f)
                                {
                                    e->phase   = 1;
                                    e->timer   = 0.75f;
                                    e->gen     = 4;         /* dashes in the chain */
                                    e->lockDir = dir;
                                    PlaySfx(&sfxWarn, 0.95f);
                                }
                                break;

                            case 1:     /* lock-on, still tracking you */
                                e->vel     = Vector2Scale(e->vel, powf(0.02f, dt));
                                e->lockDir = Vector2Lerp(e->lockDir, dir,
                                                         1.0f - powf(0.03f, dt));
                                if (Vector2LengthSqr(e->lockDir) > 0.0001f)
                                    e->lockDir = Vector2Normalize(e->lockDir);

                                if (e->timer <= 0.0f)
                                {
                                    e->phase = 2;
                                    e->timer = 0.55f;
                                    e->vel   = Vector2Scale(e->lockDir, 1000.0f);
                                    EmitBurst(e->pos,
                                              atan2f(-e->lockDir.y, -e->lockDir.x), 0.7f,
                                              18, 110.0f, 400.0f, 0.4f, 5.0f,
                                              EnemyColor(e), 130.0f);
                                    AddShake(7.0f);
                                    PlaySfx(&sfxShoot, 0.4f);
                                }
                                break;

                            case 2:     /* the dash, bouncing at full speed */
                            {
                                /* Reflected here rather than by the generic
                                   arena clamp below, which damps to half speed
                                   - that would turn a ricochet into a stumble.
                                   Setting the velocity away from the wall also
                                   stops the clamp from firing at all. */
                                if ((e->pos.x < e->radius          && e->vel.x < 0.0f) ||
                                    (e->pos.x > SCREEN_W - e->radius && e->vel.x > 0.0f))
                                {
                                    e->vel.x = -e->vel.x;
                                    AddShake(4.0f);
                                    EmitBurst(e->pos, 0.0f, PI, 8, 90.0f, 300.0f, 0.3f,
                                              4.0f, EnemyColor(e), 120.0f);
                                }
                                if ((e->pos.y < e->radius          && e->vel.y < 0.0f) ||
                                    (e->pos.y > GROUND_Y - e->radius && e->vel.y > 0.0f))
                                {
                                    e->vel.y = -e->vel.y;
                                    AddShake(4.0f);
                                    EmitBurst(e->pos, 0.0f, PI, 8, 90.0f, 300.0f, 0.3f,
                                              4.0f, EnemyColor(e), 120.0f);
                                }

                                EmitBurst(e->pos, 0.0f, PI, 2, 0.0f, 80.0f, 0.35f, 5.0f,
                                          EnemyColor(e), 0.0f);

                                if (e->timer <= 0.0f)
                                {
                                    if (--e->gen > 0) { e->phase = 1; e->timer = 0.28f; }
                                    else              { e->phase = 3; e->timer = 1.5f;  }
                                }
                            } break;

                            default:    /* spent - the whole window to hurt it */
                                e->vel = Vector2Scale(e->vel, powf(0.05f, dt));
                                if (e->timer <= 0.0f) { e->phase = 0; e->timer = 1.1f; }
                                break;
                        }
                    } break;

                    /* Mortar: the bombardier at siege scale. It never comes to
                       you - it takes the floor away instead, first in salvos
                       aimed where you are standing, then in a carpet that walks
                       across the whole arena toward you. The only boss you
                       fight by choosing where to be rather than what to dodge. */
                    case BK_MORTAR:
                    {
                        Color bomb = (Color){ 225, 245, 120, 255 };

                        /* Keeps its distance and keeps its altitude. */
                        float want = (dist < 400.0f) ? -130.0f : 45.0f;
                        e->vel = Vector2Lerp(e->vel, Vector2Scale(dir, want),
                                             1.0f - powf(0.2f, dt));
                        if (e->pos.y > GROUND_Y - 260.0f) e->vel.y -= 190.0f * dt;

                        e->timer -= edt;

                        if (e->phase == 0)          /* aimed salvos */
                        {
                            e->timer2 -= edt;

                            if (e->timer <= 0.0f)
                            {
                                e->timer = 2.5f;

                                /* Three charges: one on you, two bracketing -
                                   so the dodge cannot simply be "step aside". */
                                Vector2 muzzle = Vector2Add(e->pos, (Vector2){ 0.0f, -e->radius });
                                for (int k = -1; k <= 1; k++)
                                {
                                    Vector2 at = { player.pos.x + k * 130.0f, player.pos.y };
                                    ThrowBombAt(muzzle, at, bomb);
                                }
                                PlaySfx(&sfxShoot, 0.85f);
                            }

                            if (e->timer2 <= 0.0f)  /* time to walk the carpet */
                            {
                                e->phase  = 1;
                                e->timer  = 0.0f;
                                /* Starts on the far side and sweeps TOWARD you,
                                   so standing still is the one losing move. */
                                e->gen    = (player.pos.x < SCREEN_W * 0.5f) ? 9 : 0;
                                e->lockDir = (Vector2){ (e->gen == 0) ? 1.0f : -1.0f, 0.0f };
                                PlaySfx(&sfxWarn, 0.55f);
                            }
                        }
                        else                        /* the carpet itself */
                        {
                            if (e->timer <= 0.0f)
                            {
                                e->timer = 0.2f;

                                float x  = 110.0f + e->gen * ((SCREEN_W - 220.0f) / 9.0f);
                                Vector2 muzzle = Vector2Add(e->pos, (Vector2){ 0.0f, -e->radius });
                                ThrowBombAt(muzzle, (Vector2){ x, GROUND_Y - 24.0f }, bomb);

                                e->gen += (e->lockDir.x > 0.0f) ? 1 : -1;
                                if (e->gen < 0 || e->gen > 9)
                                {
                                    e->phase  = 0;
                                    e->timer  = 1.6f;
                                    e->timer2 = RandF(9.0f, 12.0f);
                                }
                            }
                        }
                    } break;

                    /* Charger: no bullets at all. It winds up, telegraphs, and
                       throws its whole mass down the lane you are standing in. */
                    case BK_CHARGER:
                    {
                        e->timer -= edt;
                        switch (e->phase)
                        {
                            case 0:     /* stalk */
                                e->vel = Vector2Lerp(e->vel, Vector2Scale(dir, 150.0f),
                                                     1.0f - powf(0.1f, dt));
                                if (e->timer <= 0.0f)
                                {
                                    e->phase   = 1;
                                    e->timer   = 0.85f;
                                    e->lockDir = dir;
                                    PlaySfx(&sfxWarn, 0.8f);
                                }
                                break;

                            case 1:     /* wind-up, lane shown in DrawEnemy */
                                e->vel = Vector2Scale(e->vel, powf(0.02f, dt));
                                if (e->timer <= 0.0f)
                                {
                                    e->phase = 2;
                                    e->timer = 0.7f;
                                    e->vel   = Vector2Scale(e->lockDir, 900.0f);
                                    EmitBurst(e->pos,
                                              atan2f(-e->lockDir.y, -e->lockDir.x), 0.7f,
                                              22, 120.0f, 420.0f, 0.4f, 6.0f,
                                              EnemyColor(e), 140.0f);
                                    AddShake(8.0f);
                                    PlaySfx(&sfxShoot, 0.35f);
                                }
                                break;

                            case 2:     /* the charge */
                                EmitBurst(e->pos, 0.0f, PI, 2, 0.0f, 70.0f, 0.35f, 5.0f,
                                          EnemyColor(e), 0.0f);
                                if (e->timer <= 0.0f) { e->phase = 3; e->timer = 1.0f; }
                                break;

                            default:    /* recovery - the window to punish it */
                                e->vel = Vector2Scale(e->vel, powf(0.06f, dt));
                                if (e->timer <= 0.0f) { e->phase = 0; e->timer = 1.3f; }
                                break;
                        }
                    } break;
                }
            } break;
        }

        /* MUT_SWIFT lives here, on the one line that turns an enemy's
           velocity into travel. Scaling the displacement rather than every
           `speed` constant in the behaviour switch means a new enemy kind
           inherits it for free, and the steering above stays exactly as
           tuned - they aim the same, they just cover ground faster. */
        e->pos = Vector2Add(e->pos, Vector2Scale(e->vel, dt * MutEnemySpeed()));

        /* keep them inside the arena once they have entered it */
        {
            float r = e->radius;
            if (e->pos.x < r && e->vel.x < 0 && e->pos.x > -r * 3) { e->pos.x = r; e->vel.x *= -0.5f; }
            if (e->pos.x > SCREEN_W - r && e->vel.x > 0 && e->pos.x < SCREEN_W + r * 3)
                { e->pos.x = SCREEN_W - r; e->vel.x *= -0.5f; }
            if (e->pos.y > GROUND_Y - r) { e->pos.y = GROUND_Y - r; e->vel.y *= -0.4f; }
            if (e->pos.y < r && e->vel.y < 0 && e->pos.y > -r * 3) { e->pos.y = r; e->vel.y *= -0.5f; }
        }

        /* contact damage */
        if (player.alive && CheckCollisionCircles(e->pos, e->radius, player.pos, PLAYER_RADIUS))
        {
            /* A bomber that reaches you spends itself doing it - the blast is
               the attack, so it never also lands a contact hit on top. */
            if (e->type == EN_BOMBER) { KillEnemy(e, false); continue; }

            HurtPlayer(e->pos);
            if (e->type == EN_CHASER) DamageEnemy(e, 1.0f, player.pos);
        }
    }
}

/*----------------------------------------------------------------------------*/
/* Bullet update                                                              */
/*----------------------------------------------------------------------------*/
/* Closest live enemy within range, or NULL. Used by homing shots. */
static Enemy *NearestEnemy(Vector2 p, float maxDist)
{
    Enemy *best = NULL;
    float  bestD = maxDist * maxDist;

    for (int i = 0; i < MAX_ENEMIES; i++)
    {
        Enemy *e = &enemies[i];
        if (!e->active || e->spawnT > 0.0f) continue;

        float d = Vector2LengthSqr(Vector2Subtract(e->pos, p));
        if (d < bestD) { bestD = d; best = e; }
    }
    return best;
}

static void UpdateBullets(float dt)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        Bullet *b = &bullets[i];
        if (!b->active) continue;

        b->life -= dt;
        if (b->life <= 0.0f)
        {
            b->active = false;
            /* Both sides have fuses now, and they are not the same explosion:
               Explode hurts enemies and shoves the player, BomberBlast hurts
               the player. Routing an enemy bomb through Explode would have it
               damaging its own side and healing nobody. */
            if (b->explosive)
            {
                if (b->fromPlayer) Explode(b->pos, BulletDamage(b), b->color);
                else               BomberBlast(b->pos, BOMB_RADIUS);
            }
            continue;
        }

        /* HOMING bends the shot before it moves. A slash is a swing rather than
           a projectile, so it keeps going wherever the swing went. */
        if (b->fromPlayer && !b->slash && upStacks[UP_HOMING] > 0)
        {
            float  speed = Vector2Length(b->vel);
            Enemy *t     = (speed > 1.0f) ? NearestEnemy(b->pos, HOMING_RANGE) : NULL;

            if (t != NULL)
            {
                Vector2 cur  = Vector2Scale(b->vel, 1.0f / speed);
                Vector2 want = Vector2Normalize(Vector2Subtract(t->pos, b->pos));
                float   k    = 3.4f * upStacks[UP_HOMING] * dt;
                if (k > 1.0f) k = 1.0f;

                Vector2 mix = Vector2Lerp(cur, want, k);
                if (Vector2LengthSqr(mix) > 0.0001f)
                    b->vel = Vector2Scale(Vector2Normalize(mix), speed);
            }
        }

        /* MAGNET: the shot drags the crowd toward itself. Unlike HOMING this
           does not improve the shot in flight - it rearranges the arena for the
           NEXT one, which is what makes it worth a slot next to a damage card.
           Nudges position rather than velocity because half the roster steers
           by writing pos directly and would simply undo a velocity change. */
        if (b->fromPlayer && !b->slash && HasAug(UP_MAGNET))
        {
            for (int j = 0; j < MAX_ENEMIES; j++)
            {
                Enemy *e = &enemies[j];
                if (!e->active || e->spawnT > 0.0f || e->type == EN_BOSS) continue;

                Vector2 d   = Vector2Subtract(b->pos, e->pos);
                float   len = Vector2Length(d);
                if (len > MAGNET_RADIUS || len < 1.0f) continue;

                /* Strongest at the centre, nothing at the rim, so a bullet
                   passing by nudges rather than snatches. */
                float pull = (1.0f - len / MAGNET_RADIUS) * MAGNET_PULL * dt;
                e->pos = Vector2Add(e->pos, Vector2Scale(d, pull / len));
            }
        }

        b->vel.y += b->grav * dt;

        /* Where the shot was before this step. Collision is swept along that
           segment rather than tested at the new point, because a point test
           misses whatever is thinner than one frame of travel: a harpoon at
           1350 covers 22px a frame and a chaser is only 15 across, so the
           fastest gun in the game could already shoot straight through things.
           The velocity upgrade would have made that the normal experience. */
        Vector2 from = b->pos;
        b->pos = Vector2Add(b->pos, Vector2Scale(b->vel, dt));

        if (b->fromPlayer)
        {
            /* one wall bounce per bullet -> trick shots (rockets detonate instead) */
            bool bounced = false;
            if (b->pos.x < b->radius)              { b->pos.x = b->radius;              b->vel.x = -b->vel.x; bounced = true; }
            if (b->pos.x > SCREEN_W - b->radius)   { b->pos.x = SCREEN_W - b->radius;   b->vel.x = -b->vel.x; bounced = true; }
            if (b->pos.y < b->radius)              { b->pos.y = b->radius;              b->vel.y = -b->vel.y; bounced = true; }
            if (b->pos.y > GROUND_Y - b->radius)   { b->pos.y = GROUND_Y - b->radius;   b->vel.y = -b->vel.y; bounced = true; }

            if (bounced)
            {
                /* Out of bounces: a rocket detonates on the wall, everything
                   else simply expires. Grenades keep their bounces and lose
                   energy, so they settle into a lob rather than pinging away. */
                if (b->bounces-- <= 0)
                {
                    b->active = false;
                    if (b->explosive) Explode(b->pos, BulletDamage(b), b->color);
                    continue;
                }
                if (b->grav > 0.0f) b->vel = Vector2Scale(b->vel, 0.62f);
                EmitBurst(b->pos, 0.0f, PI, 4, 40.0f, 150.0f, 0.2f, 2.5f, b->color, 100.0f);

                /* RICOCHET: every wall the shot survives makes it hit harder.
                   The ring is not decoration - a bullet that got stronger has
                   to LOOK different off the wall, or the player has no way to
                   tell a live trick shot from a stray one. */
                if (HasAug(UP_RICOCHET))
                {
                    b->damage *= 1.0f + RICOCHET_GAIN;
                    AddShock(b->pos, 26.0f, b->color);
                }

                /* The path bent this frame, so the straight segment no longer
                   describes where the shot went. Collapse the sweep to a point
                   test rather than let it cut a corner it never travelled. */
                from = b->pos;
            }

            if (b->explosive)     /* smoke trail so a slow rocket still reads fast */
                EmitBurst(b->pos, 0.0f, PI, 1, 0.0f, 50.0f, 0.4f, 5.0f,
                          (Color){ 190, 190, 200, 255 }, -20.0f);

            for (int j = 0; j < MAX_ENEMIES; j++)
            {
                Enemy *e = &enemies[j];
                if (!e->active || e->spawnT > 0.0f || j == b->lastHit) continue;
                if (DistToSegment(e->pos, from, b->pos) > e->radius + b->radius) continue;

                if (b->explosive)
                {
                    b->active = false;
                    Explode(b->pos, BulletDamage(b), b->color);
                    break;
                }

                /* Stopped dead by the shield, whatever it was carrying - a
                   piercing shot does not get to spend a pierce and continue,
                   or the enemy would be a speed bump for half the guns. */
                if (ShieldBlocks(e, b->pos))
                {
                    ShieldHit(e, BulletDamage(b), b->pos, b->color);
                    b->active = false;
                    break;
                }

                DamageEnemy(e, BulletDamage(b) * FocusOnHit(j), b->pos);

                /* A piercing shot carries on, but remembers who it just went
                   through so one pass can never land as two hits. */
                if (b->pierce > 0)
                {
                    b->pierce--;
                    b->lastHit = j;

                    /* DEVOUR: the shot eats what it passes through. Growing the
                       radius as well as the number matters twice over - it is
                       the only readout the player gets mid-flight, and a wider
                       shot genuinely catches more, so the augment compounds
                       into itself instead of just printing bigger figures. */
                    if (HasAug(UP_DEVOUR))
                    {
                        b->damage *= 1.0f + DEVOUR_DMG;
                        b->radius *= 1.0f + DEVOUR_SIZE;
                        AddShock(b->pos, b->radius * 2.4f, b->color);
                    }
                }
                else b->active = false;
                break;
            }
        }
        else
        {
            /* A thrown charge lands - it does not fall through the world and it
               does not wait out its fuse in mid-air on the way down. Landing is
               the common case, so this is where most of them go off. */
            if (b->explosive && b->pos.y > GROUND_Y - b->radius)
            {
                b->active = false;
                BomberBlast((Vector2){ b->pos.x, GROUND_Y - b->radius }, BOMB_RADIUS);
                continue;
            }

            if (b->pos.x < -40 || b->pos.x > SCREEN_W + 40 ||
                b->pos.y < -40 || b->pos.y > GROUND_Y + 40) { b->active = false; continue; }

            if (player.alive &&
                CheckCollisionCircles(b->pos, b->radius, player.pos, PLAYER_RADIUS))
            {
                b->active = false;
                if (b->explosive) BomberBlast(b->pos, BOMB_RADIUS);
                else              HurtPlayer(b->pos);
            }
        }
    }
}

/*----------------------------------------------------------------------------*/
/* Save file                                                                  */
/*----------------------------------------------------------------------------*/
static long LoadBest(void)
{
    if (!FileExists(SAVE_FILE)) return 0;

    int size = 0;
    unsigned char *data = LoadFileData(SAVE_FILE, &size);
    long v = 0;
    if (data)
    {
        if (size == (int)sizeof(long)) memcpy(&v, data, sizeof(long));
        UnloadFileData(data);
    }
    if (v < 0) v = 0;
    return v;
}

static void SaveBest(long v)
{
    SaveFileData(SAVE_FILE, &v, (int)sizeof(long));
}

/*----------------------------------------------------------------------------*/
/* Game lifecycle                                                             */
/*----------------------------------------------------------------------------*/
static void ResetGame(void)
{
    memset(bullets,   0, sizeof(bullets));
    memset(enemies,   0, sizeof(enemies));
    memset(particles, 0, sizeof(particles));
    memset(popups,    0, sizeof(popups));
    memset(pickups, 0, sizeof(pickups));
    memset(beams,   0, sizeof(beams));
    memset(shocks,  0, sizeof(shocks));

    /* upgrades are run-scoped: dying wipes the build, same as the weapon */
    memset(upStacks, 0, sizeof(upStacks));
    dmgStacks    = 0;
    rerolls      = REROLL_START;
    questState   = QS_NONE;
    questFlash   = 0.0f;
    questHold    = 0.0f;
    questPenalty = 0.0f;
    focusEnemy   = -1;
    focusHits    = 0;
    focusTimer   = 0.0f;
    blastDepth   = 0;
    pendingPicks = 0;

    /* Run-scoped like the build: a rule the last run agreed to must not be
       waiting for the next one. StartWave(1) below clears mutOn[] from
       mutArmed[] anyway, but leaving that as the only guard would make a
       new entry point into this file one line away from a very confusing
       bug report. */
    memset(mutOn,    0, sizeof(mutOn));
    memset(mutArmed, 0, sizeof(mutArmed));
    mutNextCount = 0;
    mutCount     = 0;
    mutBannerT   = 0.0f;
    healSpawnT     = 0.0f;
    weaponSpawnT   = 0.0f;
    dmgSpawnT      = 0.0f;
    lastHealWave   = -1;
    lastWeaponWave = -1;
    lastDmgWave    = -1;

    memset(&player, 0, sizeof(player));
    player.pos     = (Vector2){ SCREEN_W / 2.0f, GROUND_Y - 120.0f };
    player.hp      = PlayerMaxHp();
    player.alive   = true;
    player.aim     = -PI / 2.0f;
    player.weapon  = WP_PISTOL;     /* dying always costs you your weapon */
    player.swingSide = 1;
    aimCursor        = (Vector2){ SCREEN_W / 2.0f, SCREEN_H / 2.0f };

    score        = 0;
    runTime      = 0.0f;
    newRecord    = false;
    shake        = 0.0f;
    hitstop      = 0.0f;
    flashWhite   = 0.0f;
    gameOverT    = 0.0f;
    intermission = 1.2f;
    spawnCount   = 0;
    spawnIdx     = 0;

    StartWave(1);
}

static void EndRun(void)
{
    state     = ST_GAMEOVER;
    gameOverT = 0.0f;
    score += (long)(runTime * 5.0f);
    if (score > bestScore)
    {
        bestScore = score;
        newRecord = true;
        SaveBest(bestScore);
    }
}

/*----------------------------------------------------------------------------*/
/* Rendering                                                                  */
/*----------------------------------------------------------------------------*/
static void DrawBackground(void)
{
    DrawRectangleGradientV(0, 0, SCREEN_W, GROUND_Y,
                           (Color){ 12, 14, 30, 255 }, (Color){ 26, 20, 48, 255 });

    Color grid = (Color){ 255, 255, 255, 10 };
    for (int x = 0; x < SCREEN_W; x += 64) DrawLine(x, 0, x, GROUND_Y, grid);
    for (int y = 0; y < GROUND_Y; y += 64) DrawLine(0, y, SCREEN_W, y, grid);

    /* ambient glow behind the player so the arena reads as a stage */
    DrawCircleGradient(player.pos, 220.0f,
                       (Color){ 70, 140, 255, 22 }, (Color){ 0, 0, 0, 0 });

    DrawRectangle(0, GROUND_Y, SCREEN_W, SCREEN_H - GROUND_Y, (Color){ 20, 22, 40, 255 });
    DrawRectangleGradientV(0, GROUND_Y, SCREEN_W, 10,
                           (Color){ 90, 200, 255, 120 }, (Color){ 90, 200, 255, 0 });
    DrawLineEx((Vector2){ 0, (float)GROUND_Y }, (Vector2){ SCREEN_W, (float)GROUND_Y }, 2.5f,
               (Color){ 120, 220, 255, 200 });
}

/* Telegraph for the floor eruption: the player gets a full half-second of
   glowing, escalating warning before anything actually hurts them. */
static void DrawGroundHazard(void)
{
    float k = GroundDanger();
    if (k <= 0.0f || !player.alive) return;

    float pulse = 0.55f + 0.45f * sinf((float)GetTime() * (12.0f + 34.0f * k));
    float w     = 130.0f * (0.55f + 0.45f * k);
    float x     = player.pos.x;
    Color hot   = { 255, (unsigned char)(170 - 120 * k), 50, 255 };

    DrawRectangle((int)(x - w / 2), GROUND_Y, (int)w, SCREEN_H - GROUND_Y,
                  Fade(hot, 0.45f * k * pulse));
    DrawRectangleGradientV((int)(x - w / 2), (int)(GROUND_Y - 70.0f * k), (int)w,
                           (int)(70.0f * k), Fade(hot, 0.0f), Fade(hot, 0.55f * pulse));
    DrawLineEx((Vector2){ x - w / 2, (float)GROUND_Y }, (Vector2){ x + w / 2, (float)GROUND_Y },
               3.0f, Fade(hot, 0.9f * pulse));

    /* ring closing in on the player = time left */
    DrawRingLines(player.pos, PLAYER_RADIUS + 34.0f * (1.0f - k),
                  PLAYER_RADIUS + 34.0f * (1.0f - k) + 2.0f, 0.0f, 360.0f, 32,
                  Fade(hot, 0.8f * pulse));

    if (GetRandomValue(0, 2) == 0)
        EmitBurst((Vector2){ x + RandF(-w / 2, w / 2), (float)GROUND_Y }, -PI / 2.0f, 0.3f,
                  1, 60.0f, 220.0f * k, 0.35f, 3.0f, hot, 260.0f);

    if (k > 0.25f)
    {
        UIDrawC(FW_BOLD, "!위험!", x, GROUND_Y - 116.0f, 28.0f,
                Fade(hot, 0.85f * pulse));
    }
}

static void DrawParticles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        Particle *p = &particles[i];
        if (p->life <= 0.0f) continue;
        float t = p->life / p->maxLife;
        DrawCircleV(p->pos, p->size * t, Fade(p->color, t));
    }
}

/* Stick eyes that stare along `look`. `angry` slants the inner edges down,
   which is the whole difference between "cute" and "hostile". */
static void DrawStareEyes(Vector2 pos, Vector2 look, float forward, float spacing,
                          float ew, float eh, float angry, Color ink)
{
    if (Vector2LengthSqr(look) < 0.0001f) look = (Vector2){ 0.0f, 1.0f };
    look = Vector2Normalize(look);

    /* A bar drawn unrotated is long along +Y, and we want its long axis along
       `perp` - which is exactly a rotation by the look angle, with no offset. */
    float   baseRot = atan2f(look.y, look.x) * RAD2DEG;
    Vector2 perp    = { -look.y, look.x };

    for (int i = 0; i < 2; i++)
    {
        float   s = (i == 0) ? 1.0f : -1.0f;
        Vector2 e = Vector2Add(pos, Vector2Add(Vector2Scale(look, forward),
                                               Vector2Scale(perp, spacing * s)));
        DrawRectanglePro((Rectangle){ e.x, e.y, ew + 2.0f, eh + 2.0f },
                         (Vector2){ (ew + 2.0f) * 0.5f, (eh + 2.0f) * 0.5f },
                         baseRot + angry * s, Fade(WHITE, 0.85f));
        DrawRectanglePro((Rectangle){ e.x, e.y, ew, eh },
                         (Vector2){ ew * 0.5f, eh * 0.5f },
                         baseRot + angry * s, ink);
    }
}

/* One crate, drawn from its centre, with no state of its own.
 *
 * Split out of DrawPickups because the help screen shows the same three boxes:
 * a hand-drawn second copy would be a lie waiting to happen the first time an
 * icon changes, and the whole value of the page is that the shape a player
 * memorised there is the shape that drops into the arena. */
static void DrawCrate(Vector2 p, float rot, PickupKind kind, WeaponType weapon, Color c)
{
    Color ink = (Color){ 22, 28, 34, 255 };

    DrawRectanglePro((Rectangle){ p.x, p.y, 32.0f, 32.0f }, (Vector2){ 16.0f, 16.0f },
                     rot, WHITE);
    DrawRectanglePro((Rectangle){ p.x, p.y, 27.0f, 27.0f }, (Vector2){ 13.5f, 13.5f },
                     rot, c);

    if (kind == PK_HEAL)            /* a cross */
    {
        DrawRectanglePro((Rectangle){ p.x, p.y, 15.0f, 5.0f },
                         (Vector2){ 7.5f, 2.5f }, rot, ink);
        DrawRectanglePro((Rectangle){ p.x, p.y, 5.0f, 15.0f },
                         (Vector2){ 2.5f, 7.5f }, rot, ink);
        return;
    }

    if (kind == PK_DAMAGE)          /* two stacked chevrons, pointing up */
    {
        for (int k = 0; k < 2; k++)
        {
            float oy = p.y + 4.0f + k * 7.0f;
            DrawRectanglePro((Rectangle){ p.x - 4.0f, oy, 11.0f, 4.0f },
                             (Vector2){ 5.5f, 2.0f }, rot - 40.0f, ink);
            DrawRectanglePro((Rectangle){ p.x + 4.0f, oy, 11.0f, 4.0f },
                             (Vector2){ 5.5f, 2.0f }, rot + 40.0f, ink);
        }
        return;
    }

    {
        switch (weapon)
        {
            case WP_SMG:        /* thin barrel over a stubby magazine */
                DrawRectanglePro((Rectangle){ p.x, p.y - 3.0f, 18.0f, 4.0f },
                                 (Vector2){ 9.0f, 2.0f }, rot, ink);
                DrawRectanglePro((Rectangle){ p.x - 3.0f, p.y + 4.0f, 6.0f, 8.0f },
                                 (Vector2){ 3.0f, 4.0f }, rot, ink);
                break;

            case WP_SHOTGUN:    /* double barrel */
                DrawRectanglePro((Rectangle){ p.x, p.y - 4.0f, 17.0f, 4.5f },
                                 (Vector2){ 8.5f, 2.25f }, rot, ink);
                DrawRectanglePro((Rectangle){ p.x, p.y + 4.0f, 17.0f, 4.5f },
                                 (Vector2){ 8.5f, 2.25f }, rot, ink);
                break;

            case WP_SWORD:      /* an angled blade with a crossguard */
                DrawRectanglePro((Rectangle){ p.x + 1.0f, p.y - 1.0f, 20.0f, 4.5f },
                                 (Vector2){ 10.0f, 2.25f }, rot - 45.0f, ink);
                DrawRectanglePro((Rectangle){ p.x - 5.0f, p.y + 5.0f, 11.0f, 3.5f },
                                 (Vector2){ 5.5f, 1.75f }, rot + 45.0f, ink);
                break;

            case WP_GRENADE:    /* a round shell with a lit fuse */
                DrawCircleV((Vector2){ p.x, p.y + 2.0f }, 7.0f, ink);
                DrawRectanglePro((Rectangle){ p.x + 4.0f, p.y - 6.0f, 3.0f, 8.0f },
                                 (Vector2){ 1.5f, 4.0f }, rot + 30.0f, ink);
                break;

            case WP_RAILGUN:    /* one long lance */
                DrawRectanglePro((Rectangle){ p.x, p.y, 20.0f, 3.6f },
                                 (Vector2){ 10.0f, 1.8f }, rot, ink);
                DrawRectanglePro((Rectangle){ p.x - 6.0f, p.y, 5.0f, 12.0f },
                                 (Vector2){ 2.5f, 6.0f }, rot, ink);
                break;

            case WP_BAZOOKA:    /* fat tube with a nose */
                DrawRectanglePro((Rectangle){ p.x - 2.0f, p.y, 15.0f, 9.0f },
                                 (Vector2){ 7.5f, 4.5f }, rot, ink);
                DrawPoly((Vector2){ p.x + 8.0f, p.y }, 3, 6.0f, rot, ink);
                break;

            case WP_FLAMER:     /* stubby nozzle spitting a flame */
                DrawRectanglePro((Rectangle){ p.x - 4.0f, p.y, 13.0f, 7.0f },
                                 (Vector2){ 6.5f, 3.5f }, rot, ink);
                DrawPoly((Vector2){ p.x + 6.0f, p.y - 1.0f }, 3, 5.5f, rot, ink);
                DrawPoly((Vector2){ p.x + 9.0f, p.y + 3.0f }, 3, 3.5f, rot + 20.0f, ink);
                break;

            case WP_RICOCHET:   /* a shot bouncing off two walls */
                DrawRectanglePro((Rectangle){ p.x - 1.0f, p.y - 5.0f, 14.0f, 3.0f },
                                 (Vector2){ 7.0f, 1.5f }, rot + 32.0f, ink);
                DrawRectanglePro((Rectangle){ p.x + 1.0f, p.y + 4.0f, 14.0f, 3.0f },
                                 (Vector2){ 7.0f, 1.5f }, rot - 32.0f, ink);
                DrawCircleV((Vector2){ p.x + 8.0f, p.y - 1.0f }, 3.0f, ink);
                break;

            case WP_HARPOON:    /* one long spike with barbs */
                DrawRectanglePro((Rectangle){ p.x - 2.0f, p.y, 22.0f, 3.4f },
                                 (Vector2){ 11.0f, 1.7f }, rot, ink);
                DrawPoly((Vector2){ p.x + 10.0f, p.y }, 3, 5.5f, rot, ink);
                DrawRectanglePro((Rectangle){ p.x - 7.0f, p.y, 3.0f, 11.0f },
                                 (Vector2){ 1.5f, 5.5f }, rot, ink);
                break;

            case WP_LASER:      /* emitter lens with a thin beam */
                DrawCircleV((Vector2){ p.x - 6.0f, p.y }, 5.5f, ink);
                DrawRectanglePro((Rectangle){ p.x + 3.0f, p.y, 18.0f, 2.6f },
                                 (Vector2){ 9.0f, 1.3f }, rot, ink);
                break;

            default: break;
        }
    }
}

static void DrawPickups(void)
{
    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        const Pickup *pk = &pickups[i];
        if (!pk->active) continue;

        /* blink out the last three seconds so vanishing is never a surprise */
        if (pk->life < 3.0f && fmodf(pk->life, 0.3f) < 0.15f) continue;

        Vector2 p    = { pk->pos.x, pk->pos.y + sinf(pk->bob) * 5.0f };
        Color   c    = PickupColor(pk);
        float   glow = 0.85f + 0.15f * sinf(pk->bob * 2.0f);

        DrawCircleGradient(p, 46.0f * glow, Fade(c, 0.28f), Fade(c, 0.0f));
        DrawCrate(p, sinf(pk->bob) * 6.0f, pk->kind, pk->weapon, c);
    }
}

static void DrawEnemy(const Enemy *e)
{
    Color   c    = EnemyColor(e);
    Vector2 look = Vector2Subtract(player.pos, e->pos);
    Color   ink  = (Color){ 26, 16, 30, 255 };

    if (e->spawnT > 0.0f)
    {
        float t = 1.0f - (e->spawnT / ((e->type == EN_BOSS) ? 1.2f : 0.65f));
        DrawPolyLinesEx(e->pos, 6, e->radius * (2.4f - 1.4f * t), e->rot, 2.0f, Fade(c, 0.35f + 0.4f * t));
        DrawPolyLinesEx(e->pos, 6, e->radius * t, e->rot, 2.0f, Fade(c, 0.6f));
        return;
    }

    Color body = (e->hitFlash > 0.0f) ? WHITE : c;

    switch (e->type)
    {
        case EN_CHASER:
            DrawCircleGradient(e->pos, e->radius * 2.2f, Fade(c, 0.20f), Fade(c, 0.0f));
            DrawPoly(e->pos, 3, e->radius, e->rot, body);
            DrawPolyLinesEx(e->pos, 3, e->radius + 3.0f, -e->rot * 0.5f, 2.0f, Fade(WHITE, 0.55f));
            DrawStareEyes(e->pos, look, 3.0f, 5.0f, 3.0f, 7.5f, 22.0f, ink);
            break;

        case EN_DASHER:
        {
            DrawCircleGradient(e->pos, e->radius * 2.4f, Fade(c, 0.18f), Fade(c, 0.0f));

            /* the committed lane, drawn while winding up so it can be dodged */
            if (e->phase == 1)
            {
                /* Visible from the very first frame - a warning that fades in
                   is weakest exactly when there is still time to dodge it. */
                float t = 1.0f - Clamp(e->timer / DASH_WINDUP, 0.0f, 1.0f);
                float a = 0.45f + 0.55f * t;
                for (int i = 1; i < 20; i++)
                    DrawCircleV(Vector2Add(e->pos, Vector2Scale(e->lockDir, i * 28.0f)),
                                2.6f, Fade(c, (0.75f - i * 0.033f) * a));
            }

            Vector2 bp = e->pos;
            if (e->phase == 1) { bp.x += RandF(-2.0f, 2.0f); bp.y += RandF(-2.0f, 2.0f); }

            DrawPoly(bp, 4, e->radius, e->rot, body);
            DrawPolyLinesEx(bp, 4, e->radius + 3.0f, e->rot, 2.0f, Fade(WHITE, 0.55f));

            /* one big eye: it snaps wide open and red the instant it commits */
            Vector2 eyeLook = (e->phase >= 1 && e->phase <= 2) ? e->lockDir : look;
            bool    hot     = (e->phase == 1 || e->phase == 2);
            float   ew      = hot ? 8.5f : 6.0f;
            float   eh      = hot ? 14.0f : 10.0f;
            DrawStareEyes(bp, eyeLook, 3.0f, 0.0f, ew, eh, 0.0f,
                          hot ? (Color){ 255, 60, 60, 255 } : ink);
        } break;

        /* A dasher's shape with the corners knocked off and a lock-on cone
           instead of a lane. The cone follows you while it winds up, which is
           the whole difference between the two enemies, so it has to be the
           loudest thing about it. */
        case EN_RUSHER:
        {
            DrawCircleGradient(e->pos, e->radius * 2.6f, Fade(c, 0.22f), Fade(c, 0.0f));

            if (e->phase == 1)
            {
                float t   = 1.0f - Clamp(e->timer / RUSH_LOCK, 0.0f, 1.0f);
                float deg = atan2f(e->lockDir.y, e->lockDir.x) * RAD2DEG;

                /* The cone tightens as the lock completes - open means there is
                   still time to leave, closed means it is already coming. */
                float spread = 26.0f - 20.0f * t;
                DrawRing(e->pos, e->radius + 10.0f, e->radius + 640.0f,
                         deg - spread, deg + spread, 12, Fade(c, 0.10f + 0.10f * t));

                for (int i = 1; i < 22; i++)
                    DrawCircleV(Vector2Add(e->pos, Vector2Scale(e->lockDir, i * 30.0f)),
                                3.0f, Fade(c, (0.8f - i * 0.03f) * (0.4f + 0.6f * t)));

                /* the reticle itself, sitting on the player */
                float r = 26.0f - 12.0f * t;
                DrawCircleLines((int)player.pos.x, (int)player.pos.y, r,
                                Fade(c, 0.35f + 0.5f * t));
            }

            Vector2 bp = e->pos;
            if (e->phase == 1) { bp.x += RandF(-3.0f, 3.0f); bp.y += RandF(-3.0f, 3.0f); }

            DrawPoly(bp, 5, e->radius, e->rot, body);
            DrawPolyLinesEx(bp, 5, e->radius + 4.0f, -e->rot * 0.6f, 2.5f, Fade(WHITE, 0.6f));

            /* One pip per dash still in the volley: the count is the threat. */
            for (int i = 0; i < e->gen; i++)
                DrawCircleV((Vector2){ bp.x - 9.0f + i * 9.0f, bp.y - e->radius - 9.0f },
                            2.6f, Fade(WHITE, 0.8f));

            {
                bool hot = (e->phase == 1 || e->phase == 2);
                DrawStareEyes(bp, (e->phase >= 1 && e->phase <= 2) ? e->lockDir : look,
                              3.0f, 4.0f, hot ? 4.0f : 3.0f, hot ? 13.0f : 9.0f, 26.0f,
                              hot ? (Color){ 255, 70, 50, 255 } : ink);
            }
        } break;

        /* A hunched thrower with a live charge held over its head - the charge
           is the tell, and it brightens as the throw comes due. */
        case EN_BOMBARDIER:
        {
            DrawCircleGradient(e->pos, e->radius * 2.4f, Fade(c, 0.18f), Fade(c, 0.0f));

            DrawPoly(e->pos, 5, e->radius, e->rot, body);
            DrawPolyLinesEx(e->pos, 5, e->radius + 3.0f, -e->rot * 0.5f, 2.0f,
                            Fade(WHITE, 0.5f));

            float ready = 1.0f - Clamp(e->timer / 1.5f, 0.0f, 1.0f);
            Vector2 hold = { e->pos.x, e->pos.y - e->radius - 9.0f };
            DrawCircleV(hold, 5.5f, Fade((Color){ 240, 255, 160, 255 },
                                         0.45f + 0.55f * ready));
            DrawCircleLines((int)hold.x, (int)hold.y, 8.0f + 3.0f * ready,
                            Fade(WHITE, 0.25f + 0.45f * ready));

            DrawStareEyes(e->pos, look, 2.0f, 5.0f, 3.0f, 7.0f, 14.0f, ink);
        } break;

        case EN_SPLITTER:
            DrawCircleGradient(e->pos, e->radius * 2.2f, Fade(c, 0.20f), Fade(c, 0.0f));
            DrawPoly(e->pos, 6, e->radius, e->rot, body);
            DrawPolyLinesEx(e->pos, 6, e->radius + 4.0f, -e->rot, 2.0f, Fade(WHITE, 0.45f));
            DrawStareEyes(e->pos, look, 2.0f, (e->tier == 0) ? 8.0f : 4.5f,
                          (e->tier == 0) ? 4.0f : 2.6f, (e->tier == 0) ? 10.0f : 6.5f,
                          0.0f, ink);
            break;

        /* A squat emplacement with a barrel that tracks you. It sits flat -
           no spin - so it reads as furniture rather than something alive. */
        case EN_TURRET:
        {
            Vector2 aim = Vector2Normalize(look);
            if (Vector2LengthSqr(aim) < 0.001f) aim = (Vector2){ 0.0f, -1.0f };

            DrawCircleGradient(e->pos, e->radius * 2.2f, Fade(c, 0.18f), Fade(c, 0.0f));

            /* barrel first, so the housing sits on top of it */
            float   deg = atan2f(aim.y, aim.x) * RAD2DEG;
            Vector2 bp  = Vector2Add(e->pos, Vector2Scale(aim, 6.0f));
            DrawRectanglePro((Rectangle){ bp.x, bp.y, 26.0f, 9.0f },
                             (Vector2){ 0.0f, 4.5f }, deg, Fade(WHITE, 0.55f));
            DrawRectanglePro((Rectangle){ bp.x, bp.y, 24.0f, 6.0f },
                             (Vector2){ 0.0f, 3.0f }, deg, body);

            DrawPoly(e->pos, 4, e->radius, 45.0f, body);
            DrawPolyLinesEx(e->pos, 4, e->radius + 3.0f, 45.0f, 2.0f, Fade(WHITE, 0.5f));

            /* the muzzle glows as the next volley comes due */
            float heat = 1.0f - Clamp(e->timer / 1.1f, 0.0f, 1.0f);
            if (heat > 0.0f)
                DrawCircleV(Vector2Add(e->pos, Vector2Scale(aim, 30.0f)),
                            2.0f + 3.0f * heat, Fade(WHITE, 0.35f + 0.55f * heat));

            DrawStareEyes(e->pos, aim, 0.0f, 5.0f, 3.0f, 6.0f, 18.0f, ink);
        } break;

        /* Small body, big shield. The shield is drawn as a solid arc at the
           exact half-angle the damage test uses, so "am I behind it yet" is a
           question you answer by looking rather than by guessing. */
        case EN_SHIELDER:
        {
            float   sdeg = e->timer2 * RAD2DEG;
            Vector2 face = FromAngle(e->timer2, 1.0f);
            float   lit  = Clamp(e->guardT / 0.22f, 0.0f, 1.0f);

            DrawCircleGradient(e->pos, e->radius * 2.2f, Fade(c, 0.16f), Fade(c, 0.0f));

            DrawPoly(e->pos, 6, e->radius, e->rot, body);
            DrawPolyLinesEx(e->pos, 6, e->radius + 3.0f, e->rot, 2.0f, Fade(WHITE, 0.45f));
            DrawStareEyes(e->pos, look, 2.0f, 5.0f, 3.0f, 7.0f, 20.0f, ink);

            /* The plate. DrawRing takes degrees and sweeps clockwise from
               `start`, so the arc is centred by opening SHIELD_ARC either side
               of the facing - the same number ShieldBlocks compares against.
               Nothing is drawn once it is broken: the enemy has to LOOK
               harmless from every angle the instant it actually is. */
            if (e->shield > 0.0f)
            {
                float half  = SHIELD_ARC * RAD2DEG;
                float left  = Clamp(e->shield / e->shieldMax, 0.0f, 1.0f);
                Color plate = (lit > 0.0f) ? WHITE : (Color){ 225, 245, 255, 255 };

                DrawRing(e->pos, e->radius + 5.0f, e->radius + 15.0f,
                         sdeg - half, sdeg + half, 30, Fade(plate, 0.30f + 0.55f * left));
                DrawRing(e->pos, e->radius + 15.0f, e->radius + 18.0f,
                         sdeg - half, sdeg + half, 30, Fade(c, 0.9f));

                /* How much plate is left, drawn as plate: the arc shortens from
                   both ends, so a nearly-broken shield is visibly a smaller
                   thing to get around as well as a weaker one. */
                DrawRing(e->pos, e->radius + 6.0f, e->radius + 14.0f,
                         sdeg - half * left, sdeg + half * left, 30,
                         Fade(WHITE, 0.75f));

                /* the flare of a block, thrown out past the plate */
                if (lit > 0.0f)
                    DrawRing(e->pos, e->radius + 18.0f, e->radius + 26.0f + 10.0f * lit,
                             sdeg - half, sdeg + half, 30, Fade(WHITE, 0.55f * lit));

                /* A brace from the body to the plate: without it the arc reads
                   as a free-floating decoration rather than as something it is
                   holding. */
                DrawLineEx(Vector2Add(e->pos, Vector2Scale(face, e->radius * 0.4f)),
                           Vector2Add(e->pos, Vector2Scale(face, e->radius + 8.0f)),
                           4.0f, Fade(plate, 0.7f));
            }
            else
            {
                /* Two broken stubs where the brace used to be. */
                Vector2 perp = { -face.y, face.x };
                for (int s = -1; s <= 1; s += 2)
                {
                    Vector2 a = Vector2Add(e->pos, Vector2Scale(perp, s * e->radius * 0.6f));
                    DrawLineEx(a, Vector2Add(a, Vector2Scale(face, 7.0f)), 3.0f,
                               Fade((Color){ 225, 245, 255, 255 }, 0.35f));
                }
            }

            /* winding up a bash: the lane it will cover, same as a dasher */
            if (e->phase == 1)
            {
                float t = 1.0f - Clamp(e->timer / 0.55f, 0.0f, 1.0f);
                for (int i = 1; i < 14; i++)
                    DrawCircleV(Vector2Add(e->pos, Vector2Scale(e->lockDir, i * 26.0f)),
                                2.4f, Fade(c, (0.7f - i * 0.045f) * (0.5f + 0.5f * t)));
            }
        } break;

        /* Round, and visibly counting down. The fuse blink is the only warning
           you get, so it speeds up the closer it is to reaching you. */
        case EN_BOMBER:
        {
            float near  = 1.0f - Clamp(Vector2Length(look) / 320.0f, 0.0f, 1.0f);
            float blink = 0.5f + 0.5f * sinf(e->timer * (7.0f + 16.0f * near));

            DrawCircleGradient(e->pos, e->radius * 2.6f,
                               Fade(c, 0.18f + 0.20f * blink), Fade(c, 0.0f));
            DrawCircleV(e->pos, e->radius + 2.5f, Fade(WHITE, 0.5f));
            DrawCircleV(e->pos, e->radius, body);

            /* the charge inside, pulsing brighter as it closes */
            DrawCircleV(e->pos, e->radius * 0.45f,
                        Fade((Color){ 255, 245, 200, 255 }, 0.35f + 0.65f * blink));

            DrawStareEyes(e->pos, look, 2.0f, 5.0f, 3.0f, 7.0f, 26.0f, ink);
        } break;

        case EN_BOSS:
        {
            DrawCircleGradient(e->pos, e->radius * 2.6f, Fade(c, 0.25f), Fade(c, 0.0f));

            /* the charger shows its lane the whole wind-up, same as a dasher */
            if (e->tier == BK_CHARGER && e->phase == 1)
            {
                float t = 1.0f - Clamp(e->timer / 0.85f, 0.0f, 1.0f);
                for (int i = 1; i < 26; i++)
                    DrawCircleV(Vector2Add(e->pos, Vector2Scale(e->lockDir, i * 32.0f)),
                                4.0f, Fade(c, (0.8f - i * 0.028f) * (0.45f + 0.55f * t)));
            }

            /* The vortex wears its mechanic: rings falling inward while it
               pulls, and a bright ring collapsing onto the body during the
               wind-up so the reversal is something you can see coming rather
               than something that happens to you. */
            if (e->tier == BK_VORTEX)
            {
                float spin = e->rot * DEG2RAD;
                for (int i = 0; i < 3; i++)
                {
                    float phase = fmodf((float)GetTime() * 0.9f + i * 0.333f, 1.0f);
                    float rr    = e->radius + 150.0f * (1.0f - phase);
                    DrawCircleLines((int)e->pos.x, (int)e->pos.y, rr,
                                    Fade(c, 0.42f * phase));
                }
                for (int i = 0; i < 10; i++)
                {
                    float a = spin * 0.6f + (2.0f * PI * i) / 10.0f;
                    float d = e->radius + 26.0f + 24.0f * sinf((float)GetTime() * 3.0f + i);
                    DrawCircleV(Vector2Add(e->pos, FromAngle(a, d)), 2.6f, Fade(c, 0.7f));
                }
                if (e->phase == 1)
                {
                    float t = 1.0f - Clamp(e->timer2 / 0.9f, 0.0f, 1.0f);
                    DrawCircleLines((int)e->pos.x, (int)e->pos.y,
                                    e->radius + 210.0f * (1.0f - t), Fade(WHITE, 0.35f + 0.5f * t));
                }
            }

            /* The lancer shows its lane exactly like the charger does, but the
               lane keeps moving while it locks - that difference is the fight,
               so it has to be visible before the first dash lands. */
            if (e->tier == BK_LANCER && e->phase == 1)
            {
                float t = 1.0f - Clamp(e->timer / 0.75f, 0.0f, 1.0f);
                for (int i = 1; i < 30; i++)
                    DrawCircleV(Vector2Add(e->pos, Vector2Scale(e->lockDir, i * 30.0f)),
                                3.4f, Fade(c, (0.8f - i * 0.025f) * (0.45f + 0.55f * t)));
                DrawCircleLines((int)player.pos.x, (int)player.pos.y,
                                34.0f - 16.0f * t, Fade(c, 0.4f + 0.5f * t));
            }

            /* The mortar telegraphs the carpet by where it is pointing next. */
            if (e->tier == BK_MORTAR && e->phase == 1)
            {
                float x = 110.0f + Clamp((float)e->gen, 0.0f, 9.0f)
                                       * ((SCREEN_W - 220.0f) / 9.0f);
                DrawLineEx((Vector2){ x, (float)GROUND_Y - 90.0f },
                           (Vector2){ x, (float)GROUND_Y }, 3.0f, Fade(c, 0.5f));
                DrawCircleLines((int)x, GROUND_Y - 24, BOMB_RADIUS, Fade(c, 0.35f));
            }

            int sides = (e->tier == BK_SUMMONER) ? 6 :
                        (e->tier == BK_CHARGER)  ? 3 :
                        (e->tier == BK_VORTEX)   ? 8 :
                        (e->tier == BK_BULWARK)  ? 6 :
                        (e->tier == BK_LANCER)   ? 4 :
                        (e->tier == BK_MORTAR)   ? 7 : 5;
            DrawPoly(e->pos, sides, e->radius, e->rot, body);
            DrawPolyLinesEx(e->pos, sides, e->radius + 6.0f, -e->rot * 0.7f, 3.0f,
                            Fade(WHITE, 0.6f));

            Vector2 bossLook = ((e->tier == BK_CHARGER || e->tier == BK_LANCER) &&
                                e->phase >= 1 && e->phase <= 2) ? e->lockDir : look;
            DrawStareEyes(e->pos, bossLook, 6.0f, 15.0f, 7.0f, 20.0f, 20.0f, ink);

            /* The bulwark's plate, drawn over the body: same arc the damage
               test uses, sweeping at its own rate. What is left of it shows as
               the length of the bright inner band, so the opening you have to
               get to visibly widens as you wear it down. */
            if (e->tier == BK_BULWARK && e->shield > 0.0f)
            {
                float sdeg = e->timer2 * RAD2DEG;
                float half = SHIELD_ARC * RAD2DEG;
                float left = Clamp(e->shield / e->shieldMax, 0.0f, 1.0f);
                float lit  = Clamp(e->guardT / 0.22f, 0.0f, 1.0f);

                DrawRing(e->pos, e->radius + 8.0f, e->radius + 24.0f,
                         sdeg - half, sdeg + half, 36,
                         Fade((lit > 0.0f) ? WHITE : (Color){ 225, 245, 255, 255 },
                              0.30f + 0.55f * left));
                DrawRing(e->pos, e->radius + 10.0f, e->radius + 21.0f,
                         sdeg - half * left, sdeg + half * left, 36, Fade(WHITE, 0.75f));
                DrawRing(e->pos, e->radius + 24.0f, e->radius + 28.0f,
                         sdeg - half, sdeg + half, 36, Fade(c, 0.9f));
            }

            /* Scaled off the body rather than fixed, so a shoal of fragments
               does not turn into a stack of overlapping full-width bars. */
            float w = e->radius * 3.4f, hpT = e->hp / e->maxHp;
            DrawRectangle((int)(e->pos.x - w / 2), (int)(e->pos.y - e->radius - 22), (int)w, 7,
                          (Color){ 0, 0, 0, 160 });
            DrawRectangle((int)(e->pos.x - w / 2), (int)(e->pos.y - e->radius - 22), (int)(w * hpT), 7, c);
        } break;
    }
}

/*----------------------------------------------------------------------------*/
/* Augment ranges                                                             */
/*                                                                            */
/* Several augments are worth exactly as much as the player's ability to judge */
/* a distance by eye, which nobody can do from a number on a card. So the ones */
/* with a radius draw it. Dashed rather than solid, and faint until you fire - */
/* the arena has enough solid circles in it already, and a permanent bright    */
/* ring would read as a hazard.                                               */
/*----------------------------------------------------------------------------*/

/* A dashed circle. Segments are drawn as short arcs with gaps between them,
   which keeps a 846-radius ring legible without it turning into a wall. */
static void DrawDashedRing(Vector2 c, float r, Color col, int dashes, float thick)
{
    if (r < 4.0f) return;

    float step = 360.0f / (float)dashes;
    for (int i = 0; i < dashes; i++)
    {
        float a0 = i * step;
        DrawRing(c, r - thick * 0.5f, r + thick * 0.5f, a0, a0 + step * 0.5f, 6, col);
    }
}

/* Which augment ranges are worth drawing, and when.
 *
 * Every one of these used to be a faint circle pinned to the player at all
 * times. Four augments deep that is four concentric dashed rings and four
 * labels following you around a fight you are supposed to be reading. Almost
 * none survived the only question that matters here - what does the player DO
 * differently for having seen it?
 *
 *   BACKBLAST, DEATHBLAST - both already throw a shock ring of exactly the
 *      right size at exactly the right place the instant they fire. A standing
 *      copy on the player is the same fact twice, and the wrong one is the one
 *      that is always on.
 *
 *   HOMING - the shot visibly bends. That IS the feedback. The ring answered
 *      "will this curve?" with a 560px circle, which from anywhere near the
 *      middle of the arena means yes to nearly everything on screen - and the
 *      answer never moved the crosshair, because you were aiming at the enemy
 *      either way.
 *
 *   SNIPER - worse. 846px is further than the screen's own corner is from the
 *      middle, so the ring was either entirely invisible or a giant arc cutting
 *      across the fight. And the bonus is a smooth gradient: the circle marked
 *      the one distance where the reward STOPS growing, which is not a decision
 *      anybody makes.
 *
 * What is left is the two that still answer something - a reach small enough to
 * aim with, and a target you can kill right now.
 */
static void DrawAugmentRanges(void)
{
    if (!player.alive) return;

    /* MAGNET travels with the shot, so its ring does too. One per live bullet
       was a carpet of circles on anything with an SMG's fire rate, so a ring is
       drawn only while its bullet actually has something to pull: the moment it
       means anything is the moment it is doing something. */
    if (HasAug(UP_MAGNET))
    {
        for (int i = 0; i < MAX_BULLETS; i++)
        {
            const Bullet *b = &bullets[i];
            if (!b->active || !b->fromPlayer || b->slash) continue;

            bool pulling = false;
            for (int j = 0; j < MAX_ENEMIES && !pulling; j++)
            {
                const Enemy *e = &enemies[j];
                if (!e->active || e->spawnT > 0.0f) continue;
                if (Vector2Distance(e->pos, b->pos) <= MAGNET_RADIUS + e->radius)
                    pulling = true;
            }
            if (!pulling) continue;

            DrawDashedRing(b->pos, MAGNET_RADIUS,
                           Fade((Color){ 140, 220, 200, 255 }, 0.30f), 18, 1.5f);
        }
    }

    /* EXECUTE is a threshold, not a radius - so it marks the enemies that are
       inside it. Kept always-on: unlike the others this one is not describing
       the player, it is answering "which of these dies to the next shot", and
       that is a question the player is asking constantly. */
    if (HasAug(UP_EXECUTE))
    {
        float pulse = 0.45f + 0.25f * sinf((float)GetTime() * 7.0f);
        for (int i = 0; i < MAX_ENEMIES; i++)
        {
            const Enemy *e = &enemies[i];
            if (!e->active || e->spawnT > 0.0f || e->type == EN_BOSS) continue;
            if (e->hp > e->maxHp * EXECUTE_BELOW) continue;

            DrawDashedRing(e->pos, e->radius + 9.0f,
                           Fade((Color){ 255, 110, 130, 255 }, pulse), 10, 2.0f);
        }
    }
}

/* The reach a "먼 거리" challenge is actually asking for.
 *
 * Q_LONGSHOT is the only goal in the game phrased as a distance, and nothing on
 * screen ever said what "far" meant - QUEST_LONGSHOT is 520px, which a player
 * can only discover by killing things and watching whether the counter moves.
 * So the threshold gets drawn, in the HUD challenge block's own colour so the
 * ring and the objective read as one thing.
 *
 * Inverted against every other ring in this game: here the kills that count are
 * the ones OUTSIDE it. That is what the outward ticks and the label are for - a
 * plain circle would be read as "get inside this", which is exactly backwards.
 *
 * Only while the challenge is live. Once it is won or lost the distance stops
 * meaning anything, and a ring this size is far too big to leave lying around. */
static void DrawLongshotRange(void)
{
    if (!player.alive) return;
    if (questState != QS_ACTIVE || questKind != Q_LONGSHOT) return;

    const Color c = { 150, 210, 255, 255 };
    const float r = QUEST_LONGSHOT;

    /* Brighter than the augment rings on purpose: those are passive stats, this
       is a goal the player is actively chasing. Still lifts while firing. */
    float a = 0.34f + 0.18f * Clamp(player.fireRingT / FIRE_RING_TIME, 0.0f, 1.0f);

    DrawDashedRing(player.pos, r, Fade(c, a), 44, 2.0f);

    /* Ticks on the far side of the line, pointing away. Cheap, and it is the
       whole reason the ring cannot be mistaken for a containment circle. */
    for (int i = 0; i < 22; i++)
    {
        float   ang = (float)i * (2.0f * PI / 22.0f);
        Vector2 in  = Vector2Add(player.pos, FromAngle(ang, r + 3.0f));
        Vector2 out = Vector2Add(player.pos, FromAngle(ang, r + 11.0f));
        DrawLineEx(in, out, 2.0f, Fade(c, a * 0.85f));
    }

    /* No label on the ring itself. A 520px circle is wider than the arena from
       most positions, so the only spots a label fits are the screen edges -
       which is exactly where the score, the hearts and the weapon readout all
       live, and it landed on one of them depending on where the player happened
       to be floating. The objective line in the HUD names the ring instead. */
}

static void DrawPlayer(void)
{
    if (!player.alive) return;

    bool blink = (player.invuln > 0.0f) && (fmodf(player.invuln, 0.16f) < 0.08f);
    Color body = blink ? (Color){ 255, 120, 120, 255 } : (Color){ 120, 230, 255, 255 };
    float aimDeg = player.aim * RAD2DEG;

    /* aim guide + recoil direction preview */
    Vector2 aimDir = FromAngle(player.aim, 1.0f);
    for (int i = 2; i < 14; i++)
    {
        float d = PLAYER_RADIUS + 10.0f + i * 14.0f;
        DrawCircleV(Vector2Add(player.pos, Vector2Scale(aimDir, d)), 1.6f,
                    Fade((Color){ 150, 230, 255, 255 }, 0.35f - i * 0.02f));
    }
    Vector2 back = Vector2Add(player.pos, Vector2Scale(aimDir, -(PLAYER_RADIUS + 26.0f)));
    DrawPoly(back, 3, 7.0f, aimDeg + 180.0f, Fade((Color){ 255, 190, 90, 255 }, 0.55f));

    DrawCircleGradient(player.pos, 46.0f, Fade(body, 0.28f), Fade(body, 0.0f));

    /* --- katana: carried out to the side, swept through the aim on a slash --- */
    const WeaponDef *w = &WEAPONS[player.weapon];
    if (w->slash)
    {
        float side  = (float)((player.swingSide >= 0) ? 1 : -1);
        float rest  = aimDeg + 104.0f * side;             /* held at the hip   */
        float from  = aimDeg + 118.0f * side;             /* wind-up           */
        float to    = aimDeg -  62.0f * side;             /* follow-through    */
        float bladeDeg = rest;

        if (player.swingT > 0.0f)
        {
            float p = 1.0f - Clamp(player.swingT / SWORD_SWING, 0.0f, 1.0f);
            float e = 1.0f - powf(1.0f - p, 3.0f);        /* fast out, settling */
            bladeDeg = from + (to - from) * e;

            /* the arc the blade has already carved */
            float a0 = fminf(from, bladeDeg), a1 = fmaxf(from, bladeDeg);
            DrawRing(player.pos, 24.0f, 44.0f, a0, a1, 28,
                     Fade(w->color, 0.30f * (1.0f - p)));
            DrawRing(player.pos, 36.0f, 42.0f, a0, a1, 28,
                     Fade(WHITE, 0.45f * (1.0f - p)));
        }

        Vector2 dir  = FromAngle(bladeDeg * DEG2RAD, 1.0f);
        Vector2 grip = Vector2Add(player.pos, Vector2Scale(dir, 4.0f));

        /* handle behind the hand, then guard, then the blade itself */
        DrawRectanglePro((Rectangle){ grip.x, grip.y, 13.0f, 6.0f },
                         (Vector2){ 13.0f, 3.0f }, bladeDeg, (Color){ 60, 48, 44, 255 });
        DrawRectanglePro((Rectangle){ grip.x, grip.y, 4.0f, 16.0f },
                         (Vector2){ 2.0f, 8.0f }, bladeDeg, (Color){ 220, 195, 120, 255 });

        Vector2 base = Vector2Add(grip, Vector2Scale(dir, 3.0f));
        DrawRectanglePro((Rectangle){ base.x, base.y, 38.0f, 7.0f },
                         (Vector2){ 0.0f, 3.5f }, bladeDeg, (Color){ 240, 246, 255, 255 });
        DrawRectanglePro((Rectangle){ base.x, base.y, 36.0f, 3.0f },
                         (Vector2){ 0.0f, 2.6f }, bladeDeg, w->color);
    }
    else
    {
        /* --- gun, drawn first so the box reads as holding it --- */
        float   kick = (player.muzzle > 0.0f)
                           ? (player.muzzle / 0.07f) * 5.0f * w->recoilMul : 0.0f;
        Vector2 gunBase = Vector2Add(player.pos, Vector2Scale(aimDir, 9.0f - kick));
        Color   metal   = (player.weapon == WP_PISTOL) ? (Color){ 58, 74, 104, 255 }
                                                       : w->color;

        DrawRectanglePro((Rectangle){ gunBase.x, gunBase.y,
                                      w->gunLen + 2.0f, w->gunThick + 4.0f },
                         (Vector2){ 1.0f, (w->gunThick + 4.0f) * 0.5f }, aimDeg,
                         (Color){ 235, 245, 255, 255 });
        DrawRectanglePro((Rectangle){ gunBase.x, gunBase.y, w->gunLen, w->gunThick },
                         (Vector2){ 0.0f, w->gunThick * 0.5f }, aimDeg, metal);

        /* muzzle block, so it reads as a weapon rather than an antenna */
        Vector2 tip = Vector2Add(gunBase, Vector2Scale(aimDir, w->gunLen - 4.0f));
        DrawRectanglePro((Rectangle){ tip.x, tip.y, 9.0f, w->gunThick + 8.0f },
                         (Vector2){ 4.5f, (w->gunThick + 8.0f) * 0.5f }, aimDeg,
                         (Color){ 235, 245, 255, 255 });
        DrawRectanglePro((Rectangle){ tip.x, tip.y, 6.0f, w->gunThick + 4.0f },
                         (Vector2){ 3.0f, (w->gunThick + 4.0f) * 0.5f }, aimDeg,
                         (player.weapon == WP_PISTOL) ? (Color){ 78, 98, 134, 255 } : metal);

        if (player.muzzle > 0.0f)
        {
            Vector2 m = Vector2Add(player.pos, Vector2Scale(aimDir, PLAYER_RADIUS + 12.0f));
            DrawCircleV(m, 14.0f * (player.muzzle / 0.07f),
                        Fade((Color){ 255, 240, 180, 255 }, 0.85f));
        }
    }

    /* --- body: an upright box that leans into its motion and stretches when
           it is moving fast vertically --- */
    float tilt = Clamp(player.vel.x * 0.020f, -18.0f, 18.0f);
    float sq   = Clamp(fabsf(player.vel.y) / 900.0f, 0.0f, 0.35f);
    float bw   = 26.0f * (1.0f - sq * 0.45f);
    float bh   = 26.0f * (1.0f + sq * 0.45f);

    DrawRectanglePro((Rectangle){ player.pos.x, player.pos.y, bw + 4.0f, bh + 4.0f },
                     (Vector2){ (bw + 4.0f) * 0.5f, (bh + 4.0f) * 0.5f }, tilt, WHITE);
    DrawRectanglePro((Rectangle){ player.pos.x, player.pos.y, bw, bh },
                     (Vector2){ bw * 0.5f, bh * 0.5f }, tilt, body);

    /* --- stick eyes: they lean with the body and glance toward the aim --- */
    {
        float   rad = tilt * DEG2RAD, cs = cosf(rad), sn = sinf(rad);
        Vector2 look = Vector2Scale(aimDir, 2.6f);
        bool    shut = (player.blinkTimer <= 0.0f) || (player.invuln > 0.0f);
        Color   ink  = (Color){ 18, 26, 46, 255 };

        for (int i = 0; i < 2; i++)
        {
            float lx = (i == 0) ? -5.6f : 5.6f;
            float ly = -1.5f;
            Vector2 e = { player.pos.x + lx * cs - ly * sn + look.x,
                          player.pos.y + lx * sn + ly * cs + look.y };

            if (shut)
                DrawRectanglePro((Rectangle){ e.x, e.y, 8.2f, 2.8f },
                                 (Vector2){ 4.1f, 1.4f }, tilt, ink);
            else
                DrawRectanglePro((Rectangle){ e.x, e.y, 3.4f, 9.4f },
                                 (Vector2){ 1.7f, 4.7f }, tilt, ink);
        }
    }

    /* AEGIS. There is no bubble to hold up any more - the roll happens on the
       hit - so a faint ring says "you own this" and it flares hard on a save. */
    if (upStacks[UP_AEGIS] > 0)
    {
        Color sc = { 120, 210, 255, 255 };
        float idle = 0.10f + 0.05f * sinf((float)GetTime() * 2.4f);

        DrawRing(player.pos, PLAYER_RADIUS + 11.0f, PLAYER_RADIUS + 12.5f,
                 0.0f, 360.0f, 40, Fade(sc, idle));

        if (player.shieldFlash > 0.0f)
        {
            float k = Clamp(player.shieldFlash / 0.35f, 0.0f, 1.0f);
            float r = PLAYER_RADIUS + 12.0f + 10.0f * (1.0f - k);

            DrawCircleGradient(player.pos, r * 1.6f, Fade(sc, 0.22f * k), Fade(sc, 0.0f));
            DrawRing(player.pos, r - 3.0f, r, 0.0f, 360.0f, 40, Fade(sc, 0.35f + 0.6f * k));
        }
    }

    /* Fire-ready ring. With infinite ammo this is the only throttle the player
       has to read, so it lives right on the character. */
    {
        float interval = tune.fireCooldown * w->cooldownMul * UpFireMul() * MutFireMul();
        float t = (player.cooldown > 0.0f && interval > 0.0f)
                      ? 1.0f - Clamp(player.cooldown / interval, 0.0f, 1.0f)
                      : 1.0f;
        Color ring = w->color;

        DrawRing(player.pos, PLAYER_RADIUS + 13.0f, PLAYER_RADIUS + 16.5f,
                 -90.0f, -90.0f + 360.0f * t, 40, Fade(ring, (t >= 1.0f) ? 0.85f : 0.45f));
    }

    if (player.combo > 0)
    {
        /* Grows on every kill and eases back down, so it needs the unsnapped
           draw - and rests exactly on the grid because the base is snapped. */
        float pop = 1.0f + player.comboFlash * 1.4f;
        UIDrawCFree(FW_BOLD, TextFormat("x%.1f", ComboMultiplier()),
                    player.pos.x, player.pos.y - 58.0f, UISize(24.0f) * pop,
                    (Color){ 255, 220, 100, 220 });
    }
}

static void DrawBullets(void)
{
    for (int i = 0; i < MAX_BULLETS; i++)
    {
        Bullet *b = &bullets[i];
        if (!b->active) continue;

        if (b->slash)
        {
            /* a crescent of sword energy, thinning as it dissipates */
            float t   = Clamp(b->life / b->maxLife, 0.0f, 1.0f);
            float ang = atan2f(b->vel.y, b->vel.x) * RAD2DEG;
            float r   = b->radius * (1.35f - 0.35f * t);

            DrawCircleGradient(b->pos, r * 2.0f, Fade(b->color, 0.22f * t),
                               Fade(b->color, 0.0f));
            DrawRing(b->pos, r * 0.62f, r, ang - 74.0f, ang + 74.0f, 24,
                     Fade(b->color, 0.85f * t));
            DrawRing(b->pos, r * 0.80f, r * 0.95f, ang - 66.0f, ang + 66.0f, 24,
                     Fade(WHITE, 0.9f * t));
            continue;
        }

        /* A thrown charge is not a bullet you dodge, it is a timer you leave -
           so it is drawn as an object with a fuse, blinking faster as it runs
           out, and it shows the blast it is about to make. */
        if (b->explosive && !b->fromPlayer)
        {
            float t     = Clamp(b->life / b->maxLife, 0.0f, 1.0f);
            float blink = 0.5f + 0.5f * sinf(b->life * (9.0f + 26.0f * (1.0f - t)));

            DrawCircleLines((int)b->pos.x, (int)b->pos.y, BOMB_RADIUS,
                            Fade(b->color, 0.10f + 0.16f * blink));
            DrawCircleGradient(b->pos, b->radius * 3.0f,
                               Fade(b->color, 0.25f + 0.25f * blink),
                               Fade(b->color, 0.0f));
            DrawCircleV(b->pos, b->radius, (Color){ 60, 62, 40, 255 });
            DrawCircleV(b->pos, b->radius * 0.62f,
                        Fade((Color){ 255, 255, 210, 255 }, 0.4f + 0.6f * blink));
            DrawCircleLines((int)b->pos.x, (int)b->pos.y, b->radius + 2.0f,
                            Fade(b->color, 0.9f));
            continue;
        }

        Vector2 tail = Vector2Subtract(b->pos, Vector2Scale(b->vel, 0.016f));
        DrawCircleGradient(b->pos, b->radius * 3.5f,
                           Fade(b->color, 0.30f), Fade(b->color, 0.0f));
        DrawLineEx(tail, b->pos, b->radius * 1.4f, Fade(b->color, 0.8f));
        DrawCircleV(b->pos, b->radius, WHITE);
        DrawCircleV(b->pos, b->radius * 0.7f, b->color);
    }
}

static void DrawCrosshair(void)
{
    Vector2 m = VirtualMouse();
    Color   c = (player.alive && state == ST_PLAY) ? (Color){ 255, 235, 180, 230 }
                                                   : (Color){ 200, 215, 240, 200 };

    DrawRing(m, 9.0f, 10.5f, 0.0f, 360.0f, 28, Fade(c, 0.55f));
    DrawLineEx((Vector2){ m.x - 17, m.y }, (Vector2){ m.x - 6, m.y }, 2.0f, c);
    DrawLineEx((Vector2){ m.x + 6,  m.y }, (Vector2){ m.x + 17, m.y }, 2.0f, c);
    DrawLineEx((Vector2){ m.x, m.y - 17 }, (Vector2){ m.x, m.y - 6 }, 2.0f, c);
    DrawLineEx((Vector2){ m.x, m.y + 6  }, (Vector2){ m.x, m.y + 17 }, 2.0f, c);
    DrawCircleV(m, 1.8f, c);
}

static void DrawPopups(void)
{
    for (int i = 0; i < MAX_POPUPS; i++)
    {
        if (!popups[i].active) continue;
        float a = popups[i].life / 0.8f;
        const char *t = popups[i].label[0] ? popups[i].label
                                           : TextFormat("%d", popups[i].value);
        UIDrawC(FW_BOLD, t, popups[i].pos.x, popups[i].pos.y, 20.0f,
                Fade(popups[i].color, a));
    }
}

static void DrawHud(void)
{
    UIDraw(FW_BOLD, TextFormat("%ld", score), 24.0f, 14.0f, 44.0f, RAYWHITE);
    UIDraw(FW_REG, TextFormat("최고 %ld", bestScore), 26.0f, 64.0f, 20.0f,
           (Color){ 160, 170, 200, 255 });

    /* ---- wave challenge ---- */
    /* Under the score, because it is the other number the run is chasing. A
       finished one stays on screen for the rest of the wave rather than
       vanishing on completion - the reward popup is easy to miss mid-fight, and
       a goal that disappears the moment you hit it reads as a bug. */
    if (questState != QS_NONE)
    {
        /* A state change pulses the whole block once, which is what carries
           "you just did that" without needing a second banner. */
        float pulse = (questFlash > 0.0f) ? 0.35f * sinf(questFlash * 18.0f) : 0.0f;

        /* A failed block dims away instead of blinking out - vanishing on a
           frame boundary reads as a glitch rather than as a timer expiring. */
        float gone = (questState == QS_FAILED)
                   ? Clamp(questHold / QUEST_FAIL_FADE, 0.0f, 1.0f) : 1.0f;

        Color c = (questState == QS_DONE)   ? (Color){ 255, 190, 130, 255 }
                : (questState == QS_FAILED) ? (Color){ 130, 138, 158, 255 }
                                            : (Color){ 150, 210, 255, 255 };

        const char *tag = (questState == QS_DONE)   ? "도전 성공"
                        : (questState == QS_FAILED) ? "도전 실패"
                                                    : "도전";

        UIDraw(FW_BOLD, tag, 26.0f, 100.0f, 20.0f, Fade(c, (0.85f + pulse) * gone));

        /* What the challenge is WORTH, stated next to what it is. The payout
           used to exist only as a popup at the moment it landed - one second,
           mid-fight, over the player's own explosion - so the honest reading of
           an active challenge was "some reward, probably". A goal you can see
           the price tag on is a goal you can decide to chase or ignore. */
        {
            const char *verb = (questState == QS_DONE)   ? "획득"
                             : (questState == QS_FAILED) ? "놓침"
                                                         : "보상";
            const char *pay  = TextFormat("%s  공격력 +%.0f%%", verb,
                                          DMG_PER_STACK * DMG_QUEST * 100.0f);
            UIDraw(FW_REG, pay, 26.0f + UIWidth(FW_BOLD, tag, 20.0f) + 14.0f,
                   100.0f, 20.0f, Fade(c, (0.7f + pulse) * gone));
        }

        UIDraw(FW_REG, QuestText(), 26.0f, 124.0f, 22.0f,
               Fade(questState == QS_ACTIVE ? (Color){ 205, 215, 235, 255 }
                                            : c, (0.9f + pulse) * gone));

        /* Progress only means something while it can still change, and only for
           the goals that actually count something. */
        if (questState == QS_ACTIVE && questKind != Q_NOHIT)
        {
            float f = (float)questProg / (float)questGoal;
            if (f > 1.0f) f = 1.0f;

            DrawRectangle(26, 154, 190, 6, Fade((Color){ 70, 80, 105, 255 }, 0.9f));
            DrawRectangle(26, 154, (int)(190.0f * f), 6, Fade(c, 0.95f));
            UIDraw(FW_BOLD, TextFormat("%d / %d", questProg, questGoal),
                   224.0f, 146.0f, 20.0f, Fade(c, 0.95f));
        }

        /* The clock is the threat on this one, so it gets its own line. */
        if (questState == QS_ACTIVE && questKind == Q_SPEEDKILL)
            UIDraw(FW_BOLD, TextFormat("%.1f초", questTimer), 26.0f, 168.0f, 20.0f,
                   Fade(questTimer < 4.0f ? (Color){ 255, 120, 120, 255 } : c, 0.95f));
    }

    UIDrawC(FW_BOLD, TextFormat("웨이브 %d", wave), SCREEN_W / 2.0f, 18.0f, 28.0f,
            (Color){ 200, 215, 255, 255 });

    /* hp */
    for (int i = 0; i < PlayerMaxHp(); i++)
    {
        /* little boxes, so the life icons read as "one more of you" */
        int x = (int)(SCREEN_W - 44.0f - i * 30.0f), y = 24;
        if (i < player.hp)
        {
            DrawRectangle(x, y, 20, 20, (Color){ 120, 230, 255, 255 });
            DrawRectangle(x + 5,  y + 6, 3, 8, (Color){ 18, 26, 46, 255 });
            DrawRectangle(x + 12, y + 6, 3, 8, (Color){ 18, 26, 46, 255 });
        }
        else DrawRectangleLinesEx((Rectangle){ (float)x, (float)y, 20.0f, 20.0f }, 2.0f,
                                  (Color){ 120, 230, 255, 70 });
    }

    /* Which rules are in force, under the hearts. Right-aligned because the
       hearts already anchor that corner, and standing rather than flashed: a
       mutator changes how the whole wave has to be flown, so "why am I falling
       this fast" must be answerable at any moment, not only at the banner.

       Only while the wave is actually being played: the rules stay live
       through the intermission (you are still in the air under them), but
       the HUD shows through the upgrade overlay, and the wave you just left
       listed beside the offer for the next one reads as one list. */
    if (mutCount > 0 && state == ST_PLAY)
    {
        float y = 56.0f;
        for (int i = 0; i < MUT_COUNT; i++)
        {
            if (!mutOn[i]) continue;

            const MutatorDef *md = &MUTATORS[i];
            float w = UIWidth(FW_BOLD, md->name, 22.0f);

            DrawRectangle((int)(SCREEN_W - 30.0f - w - 8.0f), (int)y - 2, 4, 24,
                          Fade(md->color, 0.9f));
            UIDraw(FW_BOLD, md->name, SCREEN_W - 24.0f - w, y, 22.0f,
                   Fade(md->color, 0.92f));
            y += 30.0f;
        }
    }

    /* current weapon - permanent, so it is simply stated rather than timed */
    {
        const WeaponDef *w = &WEAPONS[player.weapon];
        UIDraw(FW_REG, "무기", 24.0f, SCREEN_H - 126.0f, 20.0f,
               (Color){ 160, 170, 200, 200 });
        UIDraw(FW_BOLD, w->name, 24.0f, SCREEN_H - 102.0f, 28.0f, w->color);

        /* Under the weapon because it multiplies whatever is above it. Stated
           as one total: the player has no way to act on the split between what
           the clears gave and what the cards did. */
        /* Turns red while a failed challenge is being paid off, and says by
           how much. The number itself already includes the penalty, so
           without the tag the player would only see a figure that dropped
           for no stated reason. */
        bool  debuff = (questPenalty > 0.0f);
        Color dmgCol = debuff ? (Color){ 255, 110, 110, 255 }
                              : (Color){ 255, 190, 130, 255 };

        UIDraw(FW_REG, debuff ? TextFormat("공격력  도전 실패 -%.0f%%",
                                           questPenalty * 100.0f)
                              : "공격력",
               24.0f, SCREEN_H - 62.0f, 20.0f,
               debuff ? dmgCol : (Color){ 160, 170, 200, 200 });
        UIDraw(FW_BOLD, TextFormat("%.0f%%", PlayerDamageMul() * 100.0f),
               24.0f, SCREEN_H - 38.0f, 28.0f, dmgCol);
    }

    /* The multiplier already floats over the player's head, popping on every
       kill - that is the feedback. This line is the standing reminder not to
       land, so it only earns its place once there is something worth losing.
       Below the threshold the two readouts were the same number twice, and one
       of them was on screen for most of the game. */
    if (player.combo >= COMBO_HUD_FROM)
    {
        UIDrawC(FW_BOLD, TextFormat("무착지 콤보  x%.1f", ComboMultiplier()),
                SCREEN_W / 2.0f, SCREEN_H - 44.0f, 24.0f,
                (Color){ 255, 220, 100, 220 });
    }

    /* post-boss breather */
    if (state == ST_PLAY && waveCleared && wave % 5 == 0)
    {
        UIDrawC(FW_BOLD, "보스 격파", SCREEN_W / 2.0f, SCREEN_H / 2.0f - 132.0f, 56.0f,
                (Color){ 255, 200, 120, 255 });
        UIDrawC(FW_REG, TextFormat("휴식 시간  -  %.0f", ceilf(intermission)),
                SCREEN_W / 2.0f, SCREEN_H / 2.0f - 60.0f, 24.0f,
                (Color){ 200, 215, 245, 220 });
    }

    if (waveBannerT > 0.0f && state == ST_PLAY)
    {
        float a = (waveBannerT > 1.2f) ? (1.6f - waveBannerT) / 0.4f : waveBannerT / 1.2f;
        const char *t = (wave % 5 == 0) ? TextFormat("웨이브 %d  -  보스", wave)
                                        : TextFormat("웨이브 %d", wave);
        UIDrawC(FW_BOLD, t, SCREEN_W / 2.0f, SCREEN_H / 2.0f - 120.0f, 70.0f,
                Fade(mutCount > 0 ? MUTATORS[MutFirst()].color : RAYWHITE, a * 0.9f));
    }

    /* Named under the wave number for the first couple of seconds. The standing
       readout in the corner is the reference; this is the announcement, and it
       has to arrive before the first enemy does. */
    if (mutBannerT > 0.0f && state == ST_PLAY && mutCount > 0)
    {
        float a = (mutBannerT > 2.2f) ? (2.6f - mutBannerT) / 0.4f
                                      : fminf(1.0f, mutBannerT / 0.7f);
        /* Built into a local buffer rather than chained TextFormat calls:
           that helper hands back slots from a ring of four, and the pieces
           have to stay alive together long enough to be measured. */
        char   line[128];
        size_t at = 0;
        line[0] = 0;

        for (int i = 0, k = 0; i < MUT_COUNT && at < sizeof(line) - 1; i++)
        {
            if (!mutOn[i]) continue;
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%s",
                                   (k++ > 0) ? "  +  " : "", MUTATORS[i].name);
        }

        UIDrawC(FW_BOLD, line, SCREEN_W / 2.0f, SCREEN_H / 2.0f - 40.0f, 34.0f,
                Fade(MUTATORS[MutFirst()].color, a));
        UIDrawC(FW_REG, "변칙 웨이브", SCREEN_W / 2.0f, SCREEN_H / 2.0f + 4.0f, 22.0f,
                Fade((Color){ 210, 218, 240, 255 }, a * 0.8f));
    }
}

/*----------------------------------------------------------------------------*/
/* Screens                                                                    */
/*----------------------------------------------------------------------------*/
static void DrawTitle(void)
{
    /* The name stays Latin - it is the logo, and the pixel face wears it well. */
    const char *title = "SHOTCOIL";
    float fs = 98.0f;
    float tx = SCREEN_W / 2.0f - UIWidth(FW_BOLD, title, fs) * 0.5f;
    UIDraw(FW_BOLD, title, tx + 5.0f, 145.0f, fs, (Color){ 255, 70, 140, 120 });
    UIDraw(FW_BOLD, title, tx, 140.0f, fs, RAYWHITE);

    UIDrawC(FW_BOLD, "\"Recoil is The Movement.\"", SCREEN_W / 2.0f, 258.0f, 26.0f,
            (Color){ 150, 210, 255, 255 });

    /* Three lines, centred. Everything else the game teaches by playing it. */
    const char *lines[] = {
        "클릭해서 쏘면 반동으로 날아갑니다!",
        "바닥이 불타오릅니다. 착지하지 마세요!",
        "웨이브는 끝나지 않습니다. 끝까지 살아남아보세요"
    };
    for (int i = 0; i < 3; i++)
        UIDrawC(FW_REG, lines[i], SCREEN_W / 2.0f, 340.0f + i * 40.0f, 24.0f,
                (Color){ 195, 205, 230, 255 });

    float pulse = 0.6f + 0.4f * sinf((float)GetTime() * 4.0f);
    UIDrawC(FW_BOLD, "클릭  또는  ENTER  로 시작", SCREEN_W / 2.0f, 518.0f, 30.0f,
            Fade(RAYWHITE, pulse));

    if (bestScore > 0)
        UIDrawC(FW_BOLD, TextFormat("최고 기록  %ld", bestScore), SCREEN_W / 2.0f, 580.0f,
                24.0f, (Color){ 255, 225, 120, 255 });

    UIDrawC(FW_REG, TextFormat("T  게임 방법     M  음악 %s     F11  창 모드     ESC  종료",
                               musEnabled ? "켜짐" : "꺼짐"),
            SCREEN_W / 2.0f, SCREEN_H - 52.0f, 20.0f, (Color){ 120, 132, 160, 255 });
}

/* The help page.
 *
 * Only two things in this game cannot be learned by playing it. The crates are
 * gone in fourteen seconds, so guessing wrong about one costs a detour under
 * fire; and the screen between waves asks for a decision using words the fight
 * never said out loud. Recoil, the floor and the enemies teach themselves in
 * the first ten seconds, so they are deliberately not on this page - a wall of
 * text that explains what the player is about to feel is worse than nothing.
 *
 * Everything here is drawn at sizes that survive UISize's 14px grid at a 1.0
 * window scale, where 20 and 22 land two whole steps apart. Lines are kept
 * short for the same reason: at that scale a body line renders at 28 virtual
 * pixels, and a sentence that fits the panel in fullscreen may not fit here. */
static void DrawTutorial(void)
{
    const Color dim  = (Color){ 150, 162, 190, 255 };
    const Color body = (Color){ 200, 210, 232, 255 };
    const Color cyan = (Color){ 150, 210, 255, 255 };

    /* Same scrim the upgrade screen uses. Without it the arena's floor line
       runs straight through the bottom hint - the background is drawn under
       every state, and this page is the only one whose text reaches that low. */
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade((Color){ 6, 8, 18, 255 }, 0.88f));

    UIDrawC(FW_BOLD, "게임 방법", SCREEN_W / 2.0f, 34.0f, 44.0f, RAYWHITE);
    UIDrawC(FW_REG, "좌클릭으로 발사  -  조준한 반대 방향으로 날아갑니다",
            SCREEN_W / 2.0f, 96.0f, 24.0f, cyan);
    UIDrawC(FW_REG, "바닥에 오래 머무르면 불타오릅니다",
            SCREEN_W / 2.0f, 130.0f, 24.0f, body);

    /* ---- crates ---- */
    Rectangle a = { 140.0f, 178.0f, 1000.0f, 204.0f };
    DrawRectangleRec(a, Fade((Color){ 14, 17, 32, 255 }, 0.92f));
    DrawRectangleLinesEx(a, 2.0f, Fade((Color){ 255, 205, 120, 255 }, 0.55f));

    UIDraw(FW_BOLD, "상자", a.x + 32.0f, a.y + 18.0f, 30.0f,
           (Color){ 255, 205, 120, 255 });
    {
        const char *note = "잠시 뒤 사라집니다  -  몸으로 부딪혀 줍시다";
        UIDraw(FW_REG, note, a.x + a.width - 32.0f - UIWidth(FW_REG, note, 20.0f),
               a.y + 26.0f, 20.0f, dim);
    }

    /* Drawn through the same DrawCrate the arena uses, at the same 32px, so
       what is memorised here is literally the sprite that will drop. */
    {
        const PickupKind kinds[3] = { PK_HEAL, PK_WEAPON, PK_DAMAGE };
        /* The shotgun stands in for "a weapon": its double barrel is the most
           legible of the eleven icons at a glance. */
        const WeaponType gun      = WP_SHOTGUN;
        const char      *names[3] = { "체력 상자", "무기 상자", "공격력 상자" };
        const char      *desc[3]  = {
            "체력을 1 회복합니다",
            "무기가 바뀝니다  -  죽을 때까지 유지됩니다",
            "공격력이 영구히 오릅니다"
        };
        Color colors[3] = { (Color){ 110, 255, 170, 255 },
                            WEAPONS[gun].color,
                            (Color){ 255, 170, 120, 255 } };

        for (int i = 0; i < 3; i++)
        {
            float y = a.y + 66.0f + i * 46.0f;

            DrawCrate((Vector2){ a.x + 58.0f, y + 14.0f }, 0.0f, kinds[i], gun, colors[i]);

            UIDraw(FW_BOLD, names[i], a.x + 96.0f, y, 22.0f, colors[i]);
            UIDraw(FW_REG, desc[i],
                   a.x + 96.0f + UIWidth(FW_BOLD, names[i], 22.0f) + 20.0f, y, 22.0f,
                   body);
        }
    }

    /* ---- upgrades ---- */
    Rectangle b = { 140.0f, 398.0f, 1000.0f, 208.0f };
    DrawRectangleRec(b, Fade((Color){ 14, 17, 32, 255 }, 0.92f));
    DrawRectangleLinesEx(b, 2.0f, Fade(cyan, 0.55f));

    UIDraw(FW_BOLD, "강화와 새로고침", b.x + 32.0f, b.y + 18.0f, 30.0f, cyan);

    {
        const char *lines[4] = {
            "웨이브를 클리어하면 강화 3장 중 1장을 고릅니다  (보스는 2장)",
            "'특수 증강'은 한 번만 얻는 특별한 강화입니다",
            "새로고침  -  R 키로 3장을 다시 뽑습니다",
            "새로고침 횟수  -  시작 2회, 보스 처치마다 +1  (최대 4회)"
        };
        for (int i = 0; i < 4; i++)
            UIDraw(FW_REG, lines[i], b.x + 40.0f, b.y + 64.0f + i * 34.0f, 22.0f, body);
    }

    float pulse = 0.6f + 0.4f * sinf((float)GetTime() * 4.0f);
    UIDrawC(FW_BOLD, "클릭  또는  ENTER  로 시작", SCREEN_W / 2.0f, 630.0f, 28.0f,
            Fade(RAYWHITE, pulse));
    UIDrawC(FW_REG, "ESC  타이틀로", SCREEN_W / 2.0f, 676.0f, 20.0f, dim);
}

/* Button geometry, shared by the drawing and the hit-testing so they cannot
   drift. Sits under the middle card, where the eye already is. */
static Rectangle RerollButtonRect(void)
{
    const float w = 300.0f, h = 52.0f;
    return (Rectangle){ SCREEN_W / 2.0f - w / 2.0f, SCREEN_H - 122.0f, w, h };
}

/* The forecast of what the next wave is bringing. Lives in the band between
   the cards and the reroll button - the one place on this screen with
   nothing in it. Nothing here is interactive: the rules are not a choice,
   and the only decision this screen still asks for is which card. What it
   buys is that the pick can be made KNOWING what is coming - taking 활력
   over 고화력 because the next wave says 적 흉포화 is the whole point of
   showing it before the wave rather than at the start of it. */
static Rectangle MutBannerRect(void)
{
    const float w = 900.0f, h = 64.0f;
    return (Rectangle){ SCREEN_W / 2.0f - w / 2.0f, 518.0f, w, h };
}

/* Card geometry, shared by the drawing and the hit-testing so they cannot drift. */
static Rectangle UpgradeCardRect(int i)
{
    const float w = 320.0f, h = 250.0f, gap = 34.0f;
    float total = 3.0f * w + 2.0f * gap;
    float x = (SCREEN_W - total) * 0.5f + i * (w + gap);
    return (Rectangle){ x, SCREEN_H * 0.5f - h * 0.5f + 20.0f, w, h };
}

static void ApplyUpgrade(int id)
{
    if (id < 0 || id >= UP_COUNT) return;

    upStacks[id]++;
    if (id == UP_VITALITY && player.hp < PlayerMaxHp()) player.hp++;

    EmitBurst(player.pos, 0.0f, PI, 40, 80.0f, 400.0f, 0.8f, 5.0f,
              UPGRADES[id].color, 40.0f);
    flashWhite = 0.3f;
    PlaySfx(&sfxWave, 1.2f);
}

static void UpdateUpgradeScreen(float dt)
{
    upgradeT += dt;
    if (upgradeT < UPGRADE_FADE) return;    /* not interactive yet */

    Vector2 m = VirtualMouse();
    int picked = -1;

    for (int i = 0; i < 3; i++)
    {
        if (upChoices[i] < 0) continue;
        if (CheckCollisionPointRec(m, UpgradeCardRect(i)) &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) picked = i;
    }
    if (IsKeyPressed(KEY_ONE))   picked = 0;
    if (IsKeyPressed(KEY_TWO))   picked = 1;
    if (IsKeyPressed(KEY_THREE)) picked = 2;

    /* Reroll before the pick is resolved, so a hand can be thrown away without
       spending the pick it came with. Does NOT reset upgradeT: the cards should
       swap in place rather than replay the whole entrance animation, which at
       four rerolls in a row would be most of the time spent here. */
    bool wantReroll = IsKeyPressed(KEY_R) ||
                      (CheckCollisionPointRec(m, RerollButtonRect()) &&
                       IsMouseButtonPressed(MOUSE_BUTTON_LEFT));

    if (wantReroll && rerolls > 0)
    {
        rerolls--;
        RollUpgrades();
        EmitBurst(player.pos, 0.0f, PI, 22, 60.0f, 260.0f, 0.5f, 4.0f,
                  (Color){ 150, 210, 255, 255 }, 30.0f);
        PlaySfx(&sfxReload, 1.1f);
        return;
    }

    if (picked < 0 || upChoices[picked] < 0) return;

    ApplyUpgrade(upChoices[picked]);

    /* a boss grants two picks - re-roll and stay put for the second */
    if (--pendingPicks > 0) { RollUpgrades(); upgradeT = 0.0f; return; }

    pendingPicks = 0;
    StartWave(wave + 1);
    state = ST_PLAY;
}

static void DrawUpgradeScreen(void)
{
    float a    = Clamp(upgradeT / UPGRADE_FADE, 0.0f, 1.0f);
    float ease = 1.0f - powf(1.0f - a, 3.0f);
    float rise = (1.0f - ease) * 34.0f;     /* cards settle upward as they appear */
    bool  live = (upgradeT >= UPGRADE_FADE);

    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade((Color){ 6, 8, 18, 255 }, 0.84f * a));

    bool boss = (wave % 5 == 0);

    const char *t = boss ? TextFormat("보스 격파  -  웨이브 %d", wave)
                         : TextFormat("웨이브 %d 클리어", wave);
    UIDrawC(FW_BOLD, t, SCREEN_W / 2.0f, 108.0f + rise, 48.0f,
            Fade(boss ? (Color){ 255, 200, 120, 255 } : RAYWHITE, a));

    const char *s = (pendingPicks > 1) ? TextFormat("강화 선택  (%d회 남음)", pendingPicks)
                                       : "강화 선택";
    UIDrawC(FW_REG, s, SCREEN_W / 2.0f, 172.0f + rise, 26.0f,
            Fade((Color){ 150, 210, 255, 255 }, a));

    Vector2 m = VirtualMouse();

    for (int i = 0; i < 3; i++)
    {
        int id = upChoices[i];
        if (id < 0) continue;

        Rectangle r = UpgradeCardRect(i);
        r.y += rise;

        const UpgradeDef *u = &UPGRADES[id];
        bool hot = live && CheckCollisionPointRec(m, r);

        DrawRectangleRec(r, Fade((Color){ 14, 17, 32, 255 }, 0.94f * a));
        DrawRectangleLinesEx(r, hot ? 3.0f : 2.0f,
                             Fade(hot ? WHITE : u->color, (hot ? 1.0f : 0.75f) * a));
        DrawRectangle((int)r.x, (int)r.y, (int)r.width, 6, Fade(u->color, a));

        /* A one-time augment is a different KIND of decision from a stat stack,
           so it gets flagged above the card rather than dressed up inside it -
           the player has to be able to spot it before reading three cards. The
           tag sits in the gap under the header, which is empty by construction. */
        if (UpgradeIsSpecial(id))
        {
            Rectangle tag = { r.x, r.y - 36.0f, r.width, 28.0f };
            DrawRectangleRec(tag, Fade(u->color, 0.16f * a));
            DrawRectangleLinesEx(tag, 1.0f, Fade(u->color, 0.5f * a));
            UIDrawC(FW_BOLD, "특수 증강  -  1회 한정", r.x + r.width / 2.0f,
                    r.y - 32.0f, 20.0f, Fade(u->color, a));

            /* Inset frame: the tag names it, this makes the whole card read as
               different out of the corner of the eye. */
            DrawRectangleLinesEx((Rectangle){ r.x + 6.0f, r.y + 12.0f,
                                              r.width - 12.0f, r.height - 18.0f },
                                 1.0f, Fade(u->color, 0.32f * a));
        }

        UIDraw(FW_BOLD, TextFormat("%d", i + 1), r.x + 16.0f, r.y + 20.0f, 24.0f,
               Fade(WHITE, 0.5f * a));
        UIDrawC(FW_BOLD, u->name, r.x + r.width / 2.0f, r.y + 62.0f, 36.0f,
                Fade(u->color, a));

        /* No wrapping: every description is authored to one short line, which
           is what leaves room for the value line under it. */
        UIDrawC(FW_REG, u->desc, r.x + r.width / 2.0f, r.y + 108.0f, 22.0f,
                Fade((Color){ 195, 205, 225, 255 }, a));

        /* Where the stat stands now, and where this card would take it.
           Same 22 as the description on purpose: sizes snap to whole multiples
           of the 14px font grid, and 20 falls on the wrong side of that
           rounding at a 1.0 window scale - it would render at half the size of
           the line directly above it. */
        const char *value = UpgradeValue(id);
        if (value != NULL)
            UIDrawC(FW_BOLD, value, r.x + r.width / 2.0f, r.y + 142.0f, 22.0f,
                    Fade(u->color, 0.9f * a));

        /* A capped upgrade counts down to its ceiling, so the hollow pips are
           the room left. An uncapped one has nothing to count down to - its row
           just grows with the build, and the number below carries the reading
           once it runs past what fits on the card. */
        int owned = upStacks[id];
        int cap   = u->maxStacks;
        int pips  = (cap > 0) ? cap : ((owned > 12) ? 12 : owned);

        for (int s2 = 0; s2 < pips; s2++)
        {
            int px = (int)(r.x + r.width / 2 - (pips * 16) / 2 + s2 * 16);
            if (s2 < owned)
                DrawRectangle(px, (int)r.y + 176, 11, 11, Fade(u->color, a));
            else
                DrawRectangleLines(px, (int)r.y + 176, 11, 11,
                                   Fade(u->color, 0.35f * a));
        }

        UIDrawC(FW_REG,
                (cap > 0) ? TextFormat("%d / %d", owned, cap)
                          : (owned ? TextFormat("보유 %d", owned) : "새 강화"),
                r.x + r.width / 2.0f, r.y + 200.0f, 20.0f,
                Fade((Color){ 140, 150, 175, 255 }, a));
    }

    /* ---- what the next wave is bringing ---- */
    if (mutNextCount > 0)
    {
        Rectangle r = MutBannerRect();
        r.y += rise;

        Color key  = MUTATORS[mutNext[0]].color;
        /* A slow pulse rather than a static panel. This is a warning, and it
           has to pull the eye once on the way past the cards without competing
           with them for the whole time the screen is up. */
        float glow = 0.78f + 0.22f * sinf((float)GetTime() * 3.2f);

        DrawRectangleRec(r, Fade(key, 0.13f * a));
        DrawRectangleLinesEx(r, 2.0f, Fade(key, 0.75f * glow * a));

        /* ---- left column: the label and what clearing it pays ---- */
        {
            const float lx = r.x + 18.0f;

            UIDraw(FW_BOLD, "다음 웨이브", lx, r.y + 10.0f, 22.0f,
                   Fade(key, glow * a));
            UIDraw(FW_REG,
                   TextFormat("돌파 시  공격력 +%.0f%%   새로고침 +1",
                              DMG_PER_STACK * DMG_MUTATOR * mutNextCount * 100.0f),
                   lx, r.y + 38.0f, 20.0f,
                   Fade((Color){ 255, 190, 130, 255 }, 0.9f * a));
        }

        DrawRectangle((int)(r.x + 284.0f), (int)(r.y + 12.0f), 1, 40,
                      Fade((Color){ 110, 120, 150, 255 }, 0.7f * a));

        /* ---- right: one tile per rule, split evenly ---- */
        {
            const float tx = r.x + 296.0f;
            const float tw = (r.x + r.width - 14.0f - tx) / (float)mutNextCount;

            for (int i = 0; i < mutNextCount; i++)
            {
                const MutatorDef *md = &MUTATORS[mutNext[i]];
                float cx = tx + tw * ((float)i + 0.5f);

                if (i > 0)
                    DrawRectangle((int)(tx + tw * (float)i - 6.0f), (int)(r.y + 16.0f),
                                  1, 34, Fade((Color){ 110, 120, 150, 255 }, 0.5f * a));

                UIDrawC(FW_BOLD, md->name, cx, r.y + 6.0f, 24.0f, Fade(md->color, a));
                UIDrawC(FW_REG, md->desc, cx, r.y + 38.0f, 20.0f,
                        Fade((Color){ 205, 214, 235, 255 }, 0.85f * a));
            }
        }
    }

    if (live)
    {
        /* ---- reroll ---- */
        Rectangle rb  = RerollButtonRect();
        bool      can = (rerolls > 0);
        bool      hot = can && CheckCollisionPointRec(m, rb);
        Color     rc  = can ? (Color){ 150, 210, 255, 255 } : (Color){ 110, 118, 140, 255 };

        DrawRectangleRec(rb, Fade(hot ? (Color){ 30, 48, 74, 255 }
                                      : (Color){ 14, 17, 32, 255 }, 0.94f * a));
        DrawRectangleLinesEx(rb, hot ? 3.0f : 2.0f, Fade(rc, (hot ? 1.0f : 0.6f) * a));

        /* The count is the whole decision here, so it is stated rather than
           implied by a greyed-out button - "R  새로고침  2" reads at a glance. */
        UIDrawC(FW_BOLD, can ? TextFormat("R   새로고침   %d", rerolls) : "R   새로고침   없음",
                rb.x + rb.width / 2.0f, rb.y + 13.0f, 26.0f, Fade(rc, a));

        UIDrawC(FW_BOLD, "클릭  또는  1 / 2 / 3", SCREEN_W / 2.0f, SCREEN_H - 56.0f, 24.0f,
                Fade(RAYWHITE, 0.6f + 0.4f * sinf((float)GetTime() * 4.0f)));
    }
}

/* The build the run died with. Everything here is formatted into local buffers
   rather than held as TextFormat pointers: that helper hands back slots from a
   ring of four, and this needs a dozen strings alive at once to measure them
   before it can centre anything (AddPopupEx documents the same trap).

   Returns the y it finished at, so the caller can put the prompt under it. */
static float DrawFinalSpec(float y, float alpha)
{
    const WeaponDef *w = &WEAPONS[player.weapon];

    /* ---- headline numbers ---- */
    /* The five the player was actually feeling by the end. Fire rate keeps the
       card's "간격" framing on purpose - lower is better, and a number that
       shrank under a label saying 속도 would read as a bug. */
    enum { CORE = 5 };
    const char *lab[CORE] = { "무기", "공격력", "연사 간격", "반동", "최대 체력" };
    char        val[CORE][32];
    Color       col[CORE] = { w->color,
                              { 255, 190, 130, 255 },
                              { 255, 225, 120, 255 },
                              { 150, 200, 255, 255 },
                              { 120, 255, 170, 255 } };

    snprintf(val[0], sizeof(val[0]), "%s",     w->name);
    /* Build only - UpdraftMul is deliberately left out. It reads player.airTime,
       which is frozen at whatever it happened to be when the run ended, so
       folding it in would make the same build print a different final number
       depending on whether you died in the air. The augment itself still shows
       up, in the chip row below. */
    snprintf(val[1], sizeof(val[1]), "%.0f%%", UpDamageMul() * EarnedDamageMul() * 100.0f);
    snprintf(val[2], sizeof(val[2]), "%.0f%%", UpFireMul()       * 100.0f);
    snprintf(val[3], sizeof(val[3]), "%.0f%%", UpRecoilMul()     * 100.0f);
    snprintf(val[4], sizeof(val[4]), "%d",     PlayerMaxHp());

    const float GAP = 54.0f;
    float colW[CORE], total = 0.0f;
    for (int i = 0; i < CORE; i++)
    {
        float a = UIWidth(FW_REG,  lab[i], 20.0f);
        float b = UIWidth(FW_BOLD, val[i], 30.0f);
        colW[i] = (a > b) ? a : b;
        total  += colW[i] + (i ? GAP : 0.0f);
    }

    float x = SCREEN_W / 2.0f - total / 2.0f;
    for (int i = 0; i < CORE; i++)
    {
        float cx = x + colW[i] / 2.0f;
        UIDrawC(FW_REG,  lab[i], cx, y,          20.0f,
                Fade((Color){ 150, 162, 190, 255 }, alpha));
        UIDrawC(FW_BOLD, val[i], cx, y + 26.0f,  30.0f, Fade(col[i], alpha));
        x += colW[i] + GAP;
    }
    y += 84.0f;

    /* ---- the cards that were picked ---- */
    char  chip[UP_COUNT][40];
    float chipW[UP_COUNT];
    int   idx[UP_COUNT];
    int   count = 0;

    for (int i = 0; i < UP_COUNT; i++)
    {
        if (upStacks[i] <= 0) continue;
        idx[count] = i;
        snprintf(chip[count], sizeof(chip[count]), "%s x%d", UPGRADES[i].name, upStacks[i]);
        chipW[count] = UIWidth(FW_BOLD, chip[count], 22.0f);
        count++;
    }

    if (count == 0)
    {
        UIDrawC(FW_REG, "획득한 강화 없음", SCREEN_W / 2.0f, y, 22.0f,
                Fade((Color){ 130, 140, 165, 255 }, alpha));
        return y + 34.0f;
    }

    /* Greedy wrap. Rows are centred one at a time, so a trailing short row sits
       under the middle of the block rather than hanging off the left. */
    const float CGAP = 26.0f, MAXW = 1000.0f;
    int row = 0;
    while (row < count)
    {
        int   end = row;
        float rw  = 0.0f;
        while (end < count)
        {
            float next = rw + chipW[end] + (end > row ? CGAP : 0.0f);
            if (end > row && next > MAXW) break;
            rw = next;
            end++;
        }

        float cx = SCREEN_W / 2.0f - rw / 2.0f;
        for (int i = row; i < end; i++)
        {
            UIDraw(FW_BOLD, chip[i], cx, y, 22.0f, Fade(UPGRADES[idx[i]].color, alpha));
            cx += chipW[i] + CGAP;
        }
        y   += 32.0f;
        row  = end;
    }
    return y + 2.0f;
}

static void DrawGameOver(void)
{
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, (Color){ 0, 0, 0, 150 });

    UIDrawC(FW_BOLD, "패배", SCREEN_W / 2.0f, 92.0f, 84.0f,
            (Color){ 255, 90, 110, 255 });

    UIDrawC(FW_BOLD, TextFormat("점수  %ld", score), SCREEN_W / 2.0f, 208.0f, 42.0f,
            RAYWHITE);

    UIDrawC(FW_REG, TextFormat("웨이브 %d 도달   -   %.0f초", wave, runTime),
            SCREEN_W / 2.0f, 264.0f, 24.0f, (Color){ 170, 185, 215, 255 });

    if (newRecord)
    {
        float pulse = 0.6f + 0.4f * sinf((float)GetTime() * 6.0f);
        UIDrawC(FW_BOLD, "신기록!", SCREEN_W / 2.0f, 302.0f, 36.0f,
                Fade((Color){ 255, 225, 120, 255 }, pulse));
    }
    else
    {
        UIDrawC(FW_BOLD, TextFormat("최고 기록  %ld", bestScore), SCREEN_W / 2.0f, 304.0f,
                28.0f, (Color){ 255, 225, 120, 255 });
    }

    /* The spec sheet fades in a beat after the score so the eye reads the run's
       result first and its build second, rather than meeting a wall of numbers
       at the same instant the player died. */
    float promptY = 622.0f;
    float specA   = (gameOverT - 0.35f) / 0.4f;
    if (specA > 0.0f)
    {
        if (specA > 1.0f) specA = 1.0f;

        DrawRectangle(SCREEN_W / 2 - 470, 366, 940, 1,
                      Fade((Color){ 110, 125, 160, 255 }, 0.5f * specA));
        UIDrawC(FW_REG, "최종 스펙", SCREEN_W / 2.0f, 382.0f, 20.0f,
                Fade((Color){ 130, 142, 172, 255 }, specA));

        /* The chip rows wrap, so the block's height depends on how many cards
           the run picked. Let the prompt follow it rather than sitting at a
           fixed y a tall build would collide with - floored so a short build
           does not leave it stranded halfway up the screen. */
        promptY = DrawFinalSpec(420.0f, specA) + 46.0f;
        if (promptY < 622.0f) promptY = 622.0f;
    }

    if (gameOverT > 0.5f)
        UIDrawC(FW_BOLD, "R  다시 시작        ESC  종료", SCREEN_W / 2.0f, promptY, 28.0f,
                Fade(RAYWHITE, 0.6f + 0.4f * sinf((float)GetTime() * 4.0f)));
}

/*----------------------------------------------------------------------------*/
/* Main                                                                       */
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
/* Capture mode - DEBUG BUILDS ONLY                                           */
/*                                                                            */
/* Compiled only when SHOTCOIL_CAPTURE is defined, which the Debug|x64         */
/* configuration does and Release does not. The contest is judged on a         */
/* finished game inside 1.44MB; a hidden developer flag in the submitted       */
/* binary is weight and surface area that the judge did not ask for, so the    */
/* shipped exe simply does not have it.                                        */
/*                                                                            */
/*   msbuild Project1.sln /p:Configuration=Debug /p:Platform=x64               */
/*   Project1\\x64\\Debug\\Shotcoil.exe --shot=help --scale=2                     */
/*                                                                            */
/* `Shotcoil.exe --shot=SCENE` sets up one screen, simulates a fixed number of */
/* frames, writes a PNG and exits. It exists because this game cannot be       */
/* driven from the outside: the cursor is captured in fullscreen, so           */
/* synthesized clicks and keystrokes go nowhere, and half the UI worth         */
/* checking - a wave-12 challenge, the upgrade cards, an augment's range ring  */
/* - is a two-minute run away from the title screen.                           */
/*                                                                            */
/* Deliberately inert during a normal launch: one branch in main(), taken only */
/* when --shot is actually on the command line.                                */
/*----------------------------------------------------------------------------*/

#ifdef SHOTCOIL_CAPTURE

typedef enum ShotScene {
    SHOT_OFF, SHOT_TITLE, SHOT_HELP, SHOT_PLAY, SHOT_UPGRADE, SHOT_GAMEOVER
} ShotScene;

typedef enum ShotQuest { SQ_DEFAULT, SQ_ACTIVE, SQ_FAIL, SQ_DONE } ShotQuest;

typedef struct ShotOpts {
    ShotScene   scene;
    const char *out;
    int         scale;      /* window scale. 1 and 2 land on different rungs of */
                            /* UISize's 14px grid, which is the single most     */
                            /* common way a layout in this game breaks          */
    int         wave;
    float       at;         /* seconds of simulation before the capture         */
    unsigned    seed;
    ShotQuest   quest;
    int         questKind;  /* QuestKind, or -1 for whatever the roll gives   */
    int         weapon;     /* -1 = leave whatever the run started with         */
    bool        fire;       /* hold the trigger - the shot-linked rings, the    */
                            /* muzzle, the recoil trail all need it             */
    Vector2     aim;        /* where the reticle sits, in virtual units         */
    int         aug[UP_COUNT];
    bool        mut[MUT_COUNT];  /* pins the forecast (upgrade) or the rules (play) */
    int         mutCount;
    int         cards[3];   /* pins the three offered upgrades, -1 = leave the roll */
    int         cardCount;
    MusicTrack  music;      /* --music: write the song to a wav and exit */
} ShotOpts;

static ShotOpts shot;
static float    shotT;

/* ASCII handles for the two Korean-named tables. Kept here rather than as a
   field on WeaponDef/UpgradeDef so those tables stay in their tight column
   layout. The typedefs underneath are the classic C static assert: a table
   that grows a row without gaining a key becomes a compile error instead of an
   off-by-one that silently grants the wrong augment. */
static const char *const SHOT_WEAPON_KEYS[] = {
    "pistol", "smg", "sword", "shotgun", "railgun", "grenade", "bazooka",
    "flamer", "ricochet", "harpoon", "laser"
};
static const char *const SHOT_MUT_KEYS[] = {
    "swift", "barrage", "ward", "savage",
    "gravity", "recoil", "slowbullet", "heavyshot", "scorch", "halved"
};
static const char *const SHOT_AUG_KEYS[] = {
    "vitality", "rapid", "caliber", "recoil", "bigshot", "velocity",
    "asbestos", "deathblast", "lifesteal", "thorns", "greed",
    "aegis", "pierce", "homing", "scatter", "backblast",
    "ricochet-aug", "devour", "sniper", "updraft",
    "execute", "frenzy", "focus", "magnet"
};
typedef char shot_weapon_keys_match[
    (sizeof(SHOT_WEAPON_KEYS) / sizeof(SHOT_WEAPON_KEYS[0]) == WP_COUNT) ? 1 : -1];
typedef char shot_aug_keys_match[
    (sizeof(SHOT_AUG_KEYS) / sizeof(SHOT_AUG_KEYS[0]) == UP_COUNT) ? 1 : -1];
typedef char shot_mut_keys_match[
    (sizeof(SHOT_MUT_KEYS) / sizeof(SHOT_MUT_KEYS[0]) == MUT_COUNT) ? 1 : -1];

static void ShotUsage(void)
{
    printf(
    "SHOTCOIL capture mode\n"
    "\n"
    "  Shotcoil.exe --shot=SCENE [options]\n"
    "\n"
    "  --shot=SCENE   title | help | play | upgrade | gameover\n"
    "  --out=FILE     output png, relative to the exe or absolute\n"
    "                 (default shot-SCENE.png)\n"
    "  --scale=N      window scale, 1..4    (default 1)\n"
    "  --wave=N       wave to set up        (default 1)\n"
    "  --at=SEC       seconds to simulate   (default 1.0)\n"
    "  --seed=N       rng seed              (default 1)\n"
    "  --weapon=KEY   see --list\n"
    "  --aug=K,K,...  grant one stack each; repeat a key to stack it\n"
    "  --cards=K,K,K  pin the three offered upgrades (upgrade only)\n"
    "  --mut=K,K      wave mutators - the forecast on --shot=upgrade,\n"
    "                 the rules in force on --shot=play\n"
    "  --quest=STATE  active | fail | done  (play only)\n"
    "  --qkind=KIND   airkill | nohit | speedkill | longshot  (pins the roll)\n"
    "  --fire         hold the trigger down for the whole capture\n"
    "  --aim=X,Y      reticle position in 1280x720 units (default 640,300)\n"
    "  --music=TRACK  title | battle - render the song to a wav and exit\n"
    "                 --at sets the length, --out the file\n"
    "  --list         print the weapon and augment keys, then exit\n"
    "\n"
    "Frames run at a fixed 1/60s, so a given command always renders the same\n"
    "picture. Vary --scale: font sizes snap to a 14 real-pixel grid, so a line\n"
    "that fits its panel at one scale can overflow at another.\n");
}

static void ShotList(void)
{
    printf("weapons:\n ");
    for (int i = 0; i < WP_COUNT; i++) printf(" %s", SHOT_WEAPON_KEYS[i]);

    printf("\n\nmutators:\n ");
    for (int i = 0; i < MUT_COUNT; i++) printf(" %s", SHOT_MUT_KEYS[i]);

    printf("\n\naugments:\n ");
    for (int i = 0; i < UP_COUNT; i++)
    {
        printf(" %s", SHOT_AUG_KEYS[i]);
        if (i % 6 == 5 && i != UP_COUNT - 1) printf("\n ");
    }
    printf("\n");
}

/* Matches a --name= prefix and hands back the tail, or NULL. */
static const char *ShotArg(const char *arg, const char *name)
{
    size_t n = strlen(name);
    if (strncmp(arg, name, n) != 0) return NULL;
    return arg + n;
}

static int ShotLookup(const char *const *keys, int count, const char *v, size_t len)
{
    for (int i = 0; i < count; i++)
        if (strlen(keys[i]) == len && strncmp(keys[i], v, len) == 0) return i;
    return -1;
}

/* False means the process should stop right here - bad usage, or --list. */
static bool ShotParse(int argc, char **argv)
{
    shot.scene  = SHOT_OFF;
    shot.scale  = 1;
    shot.wave   = 1;
    shot.at     = 1.0f;
    shot.seed   = 1;
    shot.quest     = SQ_DEFAULT;
    for (int i = 0; i < 3; i++) shot.cards[i] = -1;
    shot.questKind = -1;
    shot.weapon    = -1;
    shot.aim    = (Vector2){ SCREEN_W / 2.0f, 300.0f };

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        const char *v;

        if (strcmp(a, "--fire") == 0) { shot.fire = true; continue; }
        if (strcmp(a, "--list") == 0) { ShotList();  return false; }
        if (strcmp(a, "--help") == 0) { ShotUsage(); return false; }

        if ((v = ShotArg(a, "--shot=")) != NULL)
        {
            if      (strcmp(v, "title")    == 0) shot.scene = SHOT_TITLE;
            else if (strcmp(v, "help")     == 0) shot.scene = SHOT_HELP;
            else if (strcmp(v, "play")     == 0) shot.scene = SHOT_PLAY;
            else if (strcmp(v, "upgrade")  == 0) shot.scene = SHOT_UPGRADE;
            else if (strcmp(v, "gameover") == 0) shot.scene = SHOT_GAMEOVER;
            else { printf("unknown scene: %s\n\n", v); ShotUsage(); return false; }
            continue;
        }

        if ((v = ShotArg(a, "--music=")) != NULL)
        {
            if      (strcmp(v, "title")  == 0) shot.music = MUS_TITLE;
            else if (strcmp(v, "battle") == 0) shot.music = MUS_BATTLE;
            else { printf("unknown track: %s\n", v); return false; }
            continue;
        }
        if ((v = ShotArg(a, "--out="))   != NULL) { shot.out   = v;               continue; }
        if ((v = ShotArg(a, "--scale=")) != NULL) { shot.scale = atoi(v);         continue; }
        if ((v = ShotArg(a, "--wave="))  != NULL) { shot.wave  = atoi(v);         continue; }
        if ((v = ShotArg(a, "--at="))    != NULL) { shot.at    = (float)atof(v);  continue; }
        if ((v = ShotArg(a, "--seed="))  != NULL) { shot.seed  = (unsigned)atoi(v); continue; }

        if ((v = ShotArg(a, "--aim=")) != NULL)
        {
            const char *comma = strchr(v, ',');
            if (comma == NULL) { printf("--aim wants X,Y\n"); return false; }
            shot.aim = (Vector2){ (float)atof(v), (float)atof(comma + 1) };
            continue;
        }

        if ((v = ShotArg(a, "--qkind=")) != NULL)
        {
            if      (strcmp(v, "airkill")   == 0) shot.questKind = Q_AIRKILL;
            else if (strcmp(v, "nohit")     == 0) shot.questKind = Q_NOHIT;
            else if (strcmp(v, "speedkill") == 0) shot.questKind = Q_SPEEDKILL;
            else if (strcmp(v, "longshot")  == 0) shot.questKind = Q_LONGSHOT;
            else { printf("unknown challenge kind: %s\n", v); return false; }
            if (shot.quest == SQ_DEFAULT) shot.quest = SQ_ACTIVE;
            continue;
        }

        if ((v = ShotArg(a, "--quest=")) != NULL)
        {
            if      (strcmp(v, "active") == 0) shot.quest = SQ_ACTIVE;
            else if (strcmp(v, "fail")   == 0) shot.quest = SQ_FAIL;
            else if (strcmp(v, "done")   == 0) shot.quest = SQ_DONE;
            else { printf("unknown quest state: %s\n", v); return false; }
            continue;
        }

        if ((v = ShotArg(a, "--weapon=")) != NULL)
        {
            int w = ShotLookup(SHOT_WEAPON_KEYS, WP_COUNT, v, strlen(v));
            if (w < 0) { printf("unknown weapon: %s  (try --list)\n", v); return false; }
            shot.weapon = w;
            continue;
        }

        /* The card roll is weighted, so a rarely-offered upgrade can take a
           dozen seeds to turn up - which makes checking that its value line
           fits the card a matter of luck rather than a check. */
        if ((v = ShotArg(a, "--cards=")) != NULL)
        {
            while (*v != 0 && shot.cardCount < 3)
            {
                const char *comma = strchr(v, ',');
                size_t      len   = comma ? (size_t)(comma - v) : strlen(v);
                int         id    = ShotLookup(SHOT_AUG_KEYS, UP_COUNT, v, len);

                if (id < 0)
                {
                    printf("unknown upgrade: %.*s  (try --list)\n", (int)len, v);
                    return false;
                }
                shot.cards[shot.cardCount++] = id;
                v = comma ? comma + 1 : v + len;
            }
            continue;
        }

        if ((v = ShotArg(a, "--mut=")) != NULL)
        {
            while (*v != 0)
            {
                const char *comma = strchr(v, ',');
                size_t      len   = comma ? (size_t)(comma - v) : strlen(v);
                int         id    = ShotLookup(SHOT_MUT_KEYS, MUT_COUNT, v, len);

                if (id < 0)
                {
                    printf("unknown mutator: %.*s  (try --list)\n", (int)len, v);
                    return false;
                }
                if (!shot.mut[id]) { shot.mut[id] = true; shot.mutCount++; }
                v = comma ? comma + 1 : v + len;
            }
            continue;
        }

        if ((v = ShotArg(a, "--aug=")) != NULL)
        {
            while (*v != 0)
            {
                const char *comma = strchr(v, ',');
                size_t      len   = comma ? (size_t)(comma - v) : strlen(v);
                int         id    = ShotLookup(SHOT_AUG_KEYS, UP_COUNT, v, len);

                if (id < 0)
                {
                    printf("unknown augment: %.*s  (try --list)\n", (int)len, v);
                    return false;
                }
                shot.aug[id]++;
                v = comma ? comma + 1 : v + len;
            }
            continue;
        }

        printf("unknown option: %s\n\n", a);
        ShotUsage();
        return false;
    }

    if (shot.scene == SHOT_OFF) return true;    /* no --shot: just play */

    if (shot.scale < 1) shot.scale = 1;
    if (shot.scale > 4) shot.scale = 4;
    if (shot.wave  < 1) shot.wave  = 1;
    if (shot.at  < 0.0f) shot.at   = 0.0f;
    return true;
}

/* Everything that has to happen after the game is initialised but before the
   first frame. Built out of the same ResetGame / StartWave / RollUpgrades the
   game itself runs on, so a captured screen is a real one rather than a mock. */
static void ShotSetup(void)
{
    shotOn   = true;
    shotFire = shot.fire;
    shotAim  = shot.aim;

    SetRandomSeed(shot.seed);
    ResetGame();

    for (int i = 0; i < UP_COUNT; i++)
        for (int k = 0; k < shot.aug[i]; k++) ApplyUpgrade(i);

    if (shot.weapon >= 0) player.weapon = (WeaponType)shot.weapon;

    switch (shot.scene)
    {
        case SHOT_TITLE:    state = ST_TITLE;    return;
        case SHOT_HELP:     state = ST_TUTORIAL; return;

        case SHOT_UPGRADE:
            /* The screen a wave clear leads to, minus the wave. */
            StartWave(shot.wave);
            RollUpgrades();
            for (int i = 0; i < shot.cardCount; i++) upChoices[i] = shot.cards[i];
            RollMutators();

            /* --mut pins the forecast, so the banner can be checked against
               a chosen pair (widest names, longest descriptions) instead of
               whatever the seed happened to draw. */
            if (shot.mutCount > 0)
            {
                mutNextCount = 0;
                for (int i = 0; i < MUT_COUNT && mutNextCount < MUT_MAX_ACTIVE; i++)
                    if (shot.mut[i]) mutNext[mutNextCount++] = i;
            }

            pendingPicks = (shot.wave % 5 == 0) ? 2 : 1;
            upgradeT     = 0.0f;
            state        = ST_UPGRADE;
            return;

        case SHOT_GAMEOVER:
            StartWave(shot.wave);
            player.alive = false;
            EndRun();
            return;

        default:
            break;
    }

    /* Armed before StartWave, which is the one place that promotes them -
       exactly the path a real run takes out of the upgrade screen. */
    for (int i = 0; i < MUT_COUNT; i++) mutArmed[i] = shot.mut[i];

    StartWave(shot.wave);
    state = ST_PLAY;

    /* StartWave already rolls a challenge on the waves that carry one; this
       overrides it so a capture never depends on which wave number you picked. */
    if (shot.quest != SQ_DEFAULT)
    {
        StartQuest(shot.wave);

        /* StartQuest rolls its kind. Re-roll until the requested one comes up
           rather than reaching into questKind directly - the goal numbers are
           set inside that switch, and a hand-poked kind would carry the wrong
           target. Bounded so a future kind that stops being reachable cannot
           hang the capture. */
        for (int guard = 0; shot.questKind >= 0 &&
                            questKind != (QuestKind)shot.questKind &&
                            guard < 500; guard++)
            StartQuest(shot.wave);

        if (shot.quest == SQ_FAIL) QuestFail();
        if (shot.quest == SQ_DONE) QuestSucceed();
    }
}

static const char *ShotOutPath(void)
{
    if (shot.out != NULL) return shot.out;
    if (shot.music != MUS_OFF) return "shot-music.wav";

    switch (shot.scene)
    {
        case SHOT_TITLE:    return "shot-title.png";
        case SHOT_HELP:     return "shot-help.png";
        case SHOT_UPGRADE:  return "shot-upgrade.png";
        case SHOT_GAMEOVER: return "shot-gameover.png";
        default:            return "shot-play.png";
    }
}

/* Renders the song straight through MusRender - the same function the audio
   thread calls - so what lands in the wav is what the game plays, and not a
   second implementation of it that can drift. No audio device is opened:
   this runs before the window exists. */
static int ShotWriteMusic(void)
{
    int    frames = (int)(MUS_RATE * (shot.at > 0.0f ? shot.at : 1.0f));
    short *data   = (short *)MemAlloc(frames * sizeof(short));

    musTrack    = shot.music;
    musWant     = shot.music;
    musGain     = 1.0f;          /* no fade-in - the file starts on beat 1 */
    musDrum.rng = 0x9E3779B9u;

    for (int i = 0; i < frames; i += MUS_CHUNK)
    {
        int n = (frames - i < MUS_CHUNK) ? (frames - i) : MUS_CHUNK;
        MusRender(data + i, n);
    }

    Wave w  = { (unsigned int)frames, MUS_RATE, 16, 1, data };
    bool ok = ExportWave(w, ShotOutPath());
    MemFree(data);

    printf(ok ? "wrote %s (%.1fs)\n" : "could not write %s (%.1fs)\n",
           ShotOutPath(), (double)frames / MUS_RATE);
    return ok ? 0 : 1;
}

#endif  /* SHOTCOIL_CAPTURE */

/*----------------------------------------------------------------------------*/

#ifdef SHOTCOIL_CAPTURE
int main(int argc, char **argv)
#else
int main(void)
#endif
{
#ifdef SHOTCOIL_CAPTURE
    /* Parsed before anything is created: --list and a bad flag have to be able
       to answer without opening a window, and --scale decides how big it is. */
    if (!ShotParse(argc, argv)) return 0;
    if (shot.music != MUS_OFF) return ShotWriteMusic();

    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(SCREEN_W * shot.scale, SCREEN_H * shot.scale, "SHOTCOIL");
#else
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(SCREEN_W, SCREEN_H, "SHOTCOIL");
#endif

    /* The best-score file is opened by a bare relative name, so it lands in
       whatever the working directory happens to be. Launched from a shortcut or
       from Explorer's search that is not the folder the exe sits in - the record
       would be written somewhere else, or nowhere if the spot is read-only.
       Pinning the working directory to the executable keeps the save beside it. */
    ChangeDirectory(GetApplicationDirectory());
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);          /* ESC is handled per-screen */

#ifdef SHOTCOIL_CAPTURE
    /* Capture mode stays windowed at an exact multiple of the virtual
       resolution - no letterbox bars in the PNG, and viewScale is exactly
       --scale - and never grabs the pointer, so a failed run cannot leave the
       cursor trapped. */
    bool goFullscreen = (shot.scene == SHOT_OFF);
#else
    const bool goFullscreen = true;
#endif

    if (goFullscreen)
    {
        ToggleBorderlessWindowed();    /* start fullscreen, alt-tab friendly */
        UpdateViewport();
        ApplyCursorMode();             /* fullscreen -> lock the pointer in */
    }
    else
    {
        fullscreen = false;
        UpdateViewport();
    }

    LoadUIFonts();
    InitSfx();
    InitMusic();

    tune = TUNE_DEFAULT;
    bestScore = LoadBest();
    ResetGame();
    state = ST_TITLE;

#ifdef SHOTCOIL_CAPTURE
    if (shot.scene != SHOT_OFF) ShotSetup();
#endif

    while (!WindowShouldClose())
    {
        float rawDt = GetFrameTime();
        if (rawDt > 1.0f / 30.0f) rawDt = 1.0f / 30.0f;

#ifdef SHOTCOIL_CAPTURE
        /* A capture must not depend on how fast the machine drew the last
           frame, or two runs of the same command disagree. */
        if (shot.scene != SHOT_OFF) rawDt = 1.0f / 60.0f;
#endif

        UpdateViewport();

        bool toggledView = IsKeyPressed(KEY_F11) ||
                           (IsKeyDown(KEY_LEFT_ALT) && IsKeyPressed(KEY_ENTER));
        if (toggledView) ToggleFullscreenMode();

        /* Music is deliberately not a per-screen key: a player who wants it
           off wants it off, and there is no settings page to bury it in. */
        if (IsKeyPressed(KEY_M)) musEnabled = !musEnabled;
        UpdateMusic();

        UpdateAimCursor();

        float dt = rawDt;
        if (hitstop > 0.0f) { hitstop -= rawDt; dt = 0.0f; }

        /* ---- update ---- */
        switch (state)
        {
            case ST_TITLE:
                if (IsKeyPressed(KEY_T)) { state = ST_TUTORIAL; break; }
                if ((IsKeyPressed(KEY_ENTER) && !toggledView) ||
                    IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                {
                    /* The first run of the session goes through the help page.
                       After that it is on T only: a player who just died wants
                       to be back in the arena, not reading, and a page that
                       gets dismissed on reflex teaches nobody anything. */
                    if (!seenTutorial) { seenTutorial = true; state = ST_TUTORIAL; }
                    else               { ResetGame();         state = ST_PLAY;    }
                }
                if (IsKeyPressed(KEY_ESCAPE)) goto quit;
                break;

            case ST_TUTORIAL:
                if ((IsKeyPressed(KEY_ENTER) && !toggledView) ||
                    IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                {
                    ResetGame();
                    state = ST_PLAY;
                }
                if (IsKeyPressed(KEY_ESCAPE)) state = ST_TITLE;
                break;

            case ST_PLAY:
                runTime += dt;
                if (waveBannerT > 0.0f) waveBannerT -= rawDt;
                if (mutBannerT  > 0.0f) mutBannerT  -= rawDt;

                UpdateQuest(dt);
                UpdatePlayer(dt);
                UpdateEnemies(dt);
                UpdateBullets(dt);
                UpdateBeams(rawDt);
                UpdateShocks(rawDt);
                UpdatePickups(dt);
                UpdateSpawning(dt);
                UpdateParticles(rawDt);

                if (!player.alive && player.deathT > 1.1f) EndRun();
                if (IsKeyPressed(KEY_ESCAPE)) state = ST_TITLE;
                break;

            case ST_UPGRADE:
                UpdateParticles(rawDt);
                UpdateUpgradeScreen(rawDt);
                break;

            case ST_GAMEOVER:
                gameOverT += rawDt;
                UpdateParticles(rawDt);
                if (gameOverT > 0.4f && IsKeyPressed(KEY_R)) { ResetGame(); state = ST_PLAY; }
                if (IsKeyPressed(KEY_ESCAPE)) state = ST_TITLE;
                break;
        }

        shake      = fmaxf(0.0f, shake - 60.0f * rawDt);
        shake     *= powf(0.05f, rawDt);
        flashWhite = fmaxf(0.0f, flashWhite - 2.5f * rawDt);

        /* ---- draw ---- */
        BeginDrawing();
        ClearBackground(BLACK);        /* letterbox bars */

        /* shake lives in real pixels so it stays consistent at any resolution */
        Vector2 shakeOff = { RandF(-shake, shake) * viewScale,
                             RandF(-shake, shake) * viewScale };

        BeginScissorMode((int)viewOrigin.x, (int)viewOrigin.y,
                         (int)(SCREEN_W * viewScale), (int)(SCREEN_H * viewScale));

            BeginMode2D(ViewCamera(shakeOff));
                DrawBackground();

                /* The arena exists behind the pause screens but not behind the
                   two menus - a help page over a live-looking fight is asking
                   the reader to watch something instead of read. */
                if (state != ST_TITLE && state != ST_TUTORIAL)
                {
                    DrawGroundHazard();
                    DrawPickups();
                    for (int i = 0; i < MAX_ENEMIES; i++)
                        if (enemies[i].active) DrawEnemy(&enemies[i]);
                    /* Under the particles and the shots: these are a reference
                       grid, not something that should ever sit on top of what
                       is actually happening. */
                    DrawAugmentRanges();
                    DrawLongshotRange();
                    DrawParticles();
                    DrawShocks();
                    DrawBeams();
                    DrawBullets();
                    DrawPlayer();
                    DrawPopups();
                }
            EndMode2D();

            /* overlays are not shaken - they must stay readable */
            BeginMode2D(ViewCamera((Vector2){ 0.0f, 0.0f }));
                if (flashWhite > 0.0f)
                    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, Fade(RAYWHITE, flashWhite * 0.5f));

                if (state != ST_TITLE && state != ST_TUTORIAL) DrawHud();
                if (state == ST_TITLE)    DrawTitle();
                if (state == ST_TUTORIAL) DrawTutorial();
                if (state == ST_UPGRADE)  DrawUpgradeScreen();
                if (state == ST_GAMEOVER) DrawGameOver();
                DrawCrosshair();
            EndMode2D();

        EndScissorMode();

        EndDrawing();

#ifdef SHOTCOIL_CAPTURE
        if (shot.scene != SHOT_OFF)
        {
            shotT += rawDt;
            if (shotT >= shot.at)
            {
                /* Not TakeScreenshot: that one glues the working directory onto
                   whatever it is handed, so an absolute --out came back as
                   "...\\Project1/C:\\Users\\..." and silently failed to save.
                   Exporting the frame directly takes the path as given - a bare
                   name lands next to the exe, a full path lands where it says. */
                Image frame = LoadImageFromScreen();
                bool  saved = ExportImage(frame, ShotOutPath());
                UnloadImage(frame);

                if (!saved) printf("could not write %s\n", ShotOutPath());
                goto quit;
            }
        }
#endif
    }

quit:
    ShutdownMusic();
    ShutdownSfx();
    UnloadUIFonts();
    CloseWindow();
    return 0;
}
