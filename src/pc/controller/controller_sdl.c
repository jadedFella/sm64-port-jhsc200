#if !defined(_WIN32) && !defined(_WIN64)

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include <SDL2/SDL.h>

#include <ultra64.h>

#include "controller_api.h"

#define DEADZONE 4960

static bool init_ok;
static SDL_GameController *sdl_cntrl;

/* TV-side pseudo-controllers that would happily win the enumeration lottery
   and eat the real pad's input (BT remotes expose dpad/motion axes). */
static bool controller_sdl_is_real_pad(const char *name) {
    if (name == NULL) {
        return false;
    }
    if (strstr(name, "Remote") != NULL || strstr(name, "remote") != NULL ||
        strstr(name, "CEC") != NULL || strstr(name, "cec") != NULL ||
        strstr(name, "Touch") != NULL || strstr(name, "touch") != NULL) {
        return false;
    }
    return true;
}

/* Dedup so hotplug rescans don't spam the same mapping twice. */
static bool controller_sdl_guid_seen(const char *guid_str) {
    static char seen[8][33];
    static int n_seen = 0;
    for (int i = 0; i < n_seen; i++) {
        if (strcmp(seen[i], guid_str) == 0) {
            return true;
        }
    }
    if (n_seen < 8) {
        snprintf(seen[n_seen], sizeof(seen[n_seen]), "%s", guid_str);
        n_seen++;
    }
    return false;
}

static void controller_sdl_remap_xbox_pads(void) {
    /* Xbox-compatible pads under Android get a GUID synthesized as
       BLUETOOTH/USB bus + vendor + product + this device's button/axis
       masks, and SDL 2.0's built-in mappings have no 045e/028e row, so
       the pad is never recognised as an SDL game controller on TV
       boxes and gets skipped in favour of whatever pseudo-controller
       enumerates first (e.g. the BT remote). Also handles hotplug:
       rescanned after init inside the read loop. */
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        const char *name = SDL_JoystickNameForIndex(i);
        if (name == NULL ||
            (strstr(name, "X-Box") == NULL && strstr(name, "Xbox") == NULL)) {
            continue;
        }
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
        char guid_str[33];
        for (int b = 0; b < (int)sizeof(guid.data); b++) {
            sprintf(guid_str + 2 * b, "%02x", guid.data[b]);
        }
        guid_str[32] = '\0';
        if (controller_sdl_guid_seen(guid_str)) {
            continue;
        }
        char row[512];
        snprintf(row, sizeof(row),
            "%s,%s,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
            "leftstick:b7,rightstick:b8,"
            "leftshoulder:b9,rightshoulder:b10,"
            "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
            "lefttrigger:a4,righttrigger:a5,",
            guid_str, name);
        int rc_map = SDL_GameControllerAddMapping(row);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[jhs] idx=%d name=\"%s\" guid=%s IsGameCtl=%d map_rc=%d",
                    i, name, guid_str, SDL_IsGameController(i), rc_map);
    }
}

static void controller_sdl_init(void) {
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL init error: %s\n", SDL_GetError());
        return;
    }

    controller_sdl_remap_xbox_pads();
    init_ok = true;
}

static void controller_sdl_read(OSContPad *pad) {
    if (!init_ok) {
        return;
    }

    SDL_GameControllerUpdate();

    if (sdl_cntrl != NULL && !SDL_GameControllerGetAttached(sdl_cntrl)) {
        SDL_GameControllerClose(sdl_cntrl);
        sdl_cntrl = NULL;
    }
    if (sdl_cntrl == NULL) {
        controller_sdl_remap_xbox_pads();
        /* Pass 1: only accept an explicitly-named real gamepad. Other
           pseudo-controllers (e.g. the Amlogic IR keypad, which also
           passes SDL_IsGameController) enumerate at index 0 and would
           otherwise win the scan. */
        static const char *const real_names[] = { "Xbox", "X-Box", "Microsoft", "Gamepad" };
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < SDL_NumJoysticks(); i++) {
                if (!SDL_IsGameController(i)) {
                    continue;
                }
                const char *name = SDL_GameControllerNameForIndex(i);
                if (pass == 0) {
                    bool hit = false;
                    for (size_t n = 0; n < sizeof(real_names) / sizeof(real_names[0]); n++) {
                        if (name != NULL && strstr(name, real_names[n]) != NULL) {
                            hit = true;
                            break;
                        }
                    }
                    if (!hit) {
                        continue;
                    }
                } else if (!controller_sdl_is_real_pad(name)) {
                    continue;
                }
                sdl_cntrl = SDL_GameControllerOpen(i);
                if (sdl_cntrl != NULL) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[jhs] opened controller idx=%d name=\"%s\"",
                                i, name);
                    break;
                }
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[jhs] open failed idx=%d: %s", i, SDL_GetError());
            }
            if (sdl_cntrl != NULL) {
                break;
            }
        }
        if (sdl_cntrl == NULL) {
            return;
        }
    }

    if (SDL_GameControllerGetButton(sdl_cntrl, SDL_CONTROLLER_BUTTON_START)) pad->button |= START_BUTTON;
    if (SDL_GameControllerGetButton(sdl_cntrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) pad->button |= Z_TRIG;
    if (SDL_GameControllerGetButton(sdl_cntrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) pad->button |= R_TRIG;
    if (SDL_GameControllerGetButton(sdl_cntrl, SDL_CONTROLLER_BUTTON_A)) pad->button |= A_BUTTON;
    if (SDL_GameControllerGetButton(sdl_cntrl, SDL_CONTROLLER_BUTTON_X)) pad->button |= B_BUTTON;

    int16_t leftx = SDL_GameControllerGetAxis(sdl_cntrl, SDL_CONTROLLER_AXIS_LEFTX);
    int16_t lefty = SDL_GameControllerGetAxis(sdl_cntrl, SDL_CONTROLLER_AXIS_LEFTY);
    int16_t rightx = SDL_GameControllerGetAxis(sdl_cntrl, SDL_CONTROLLER_AXIS_RIGHTX);
    int16_t righty = SDL_GameControllerGetAxis(sdl_cntrl, SDL_CONTROLLER_AXIS_RIGHTY);

    int16_t ltrig = SDL_GameControllerGetAxis(sdl_cntrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int16_t rtrig = SDL_GameControllerGetAxis(sdl_cntrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

#ifdef TARGET_WEB
    // Firefox has a bug: https://bugzilla.mozilla.org/show_bug.cgi?id=1606562
    // It sets down y to 32768.0f / 32767.0f, which is greater than the allowed 1.0f,
    // which SDL then converts to a int16_t by multiplying by 32767.0f, which overflows into -32768.
    // Maximum up will hence never become -32768 with the current version of SDL2,
    // so this workaround should be safe in compliant browsers.
    if (lefty == -32768) {
        lefty = 32767;
    }
    if (righty == -32768) {
        righty = 32767;
    }
#endif

    if (rightx < -0x4000) pad->button |= L_CBUTTONS;
    if (rightx > 0x4000) pad->button |= R_CBUTTONS;
    if (righty < -0x4000) pad->button |= U_CBUTTONS;
    if (righty > 0x4000) pad->button |= D_CBUTTONS;

    if (ltrig > 30 * 256) pad->button |= Z_TRIG;
    if (rtrig > 30 * 256) pad->button |= R_TRIG;

    uint32_t magnitude_sq = (uint32_t)(leftx * leftx) + (uint32_t)(lefty * lefty);
    if (magnitude_sq > (uint32_t)(DEADZONE * DEADZONE)) {
        // Game expects stick coordinates within -80..80
        // 32768 / 409 = ~80
        pad->stick_x = leftx / 409;
        pad->stick_y = -lefty / 409;
    }
}

struct ControllerAPI controller_sdl = {
    controller_sdl_init,
    controller_sdl_read
};

#endif
