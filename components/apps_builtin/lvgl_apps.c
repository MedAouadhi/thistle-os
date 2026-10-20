/*
 * ThistleOS — LVGL built-in app registration
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One list, three builds. See lvgl_apps.h for why it must stay shared.
 */

#include "lvgl_apps.h"

/* The firmware builds every app and sets none of these flags. The simulators
 * set each one from whether the app's sources are present, so an absent app
 * compiles out instead of failing to link. Notes, Weather, WiFi Scanner and
 * Flashlight are not here: they install from the App Store
 * (cmake/thistle_apps.cmake, THISTLE_STORE_APPS). */
#ifndef THISTLE_HAVE_LAUNCHER
#define THISTLE_HAVE_LAUNCHER 1
#endif
#ifndef THISTLE_HAVE_SETTINGS
#define THISTLE_HAVE_SETTINGS 1
#endif
#ifndef THISTLE_HAVE_ASSISTANT
#define THISTLE_HAVE_ASSISTANT 1
#endif
#ifndef THISTLE_HAVE_TERMINAL
#define THISTLE_HAVE_TERMINAL 1
#endif
#ifndef THISTLE_HAVE_FILEMGR
#define THISTLE_HAVE_FILEMGR 1
#endif
#ifndef THISTLE_HAVE_FILEXFER
#define THISTLE_HAVE_FILEXFER 1
#endif
#ifndef THISTLE_HAVE_READER
#define THISTLE_HAVE_READER 1
#endif
#ifndef THISTLE_HAVE_PODCASTS
#define THISTLE_HAVE_PODCASTS 1
#endif
#ifndef THISTLE_HAVE_PHONE
#define THISTLE_HAVE_PHONE 1
#endif
#ifndef THISTLE_HAVE_MESSENGER
#define THISTLE_HAVE_MESSENGER 1
#endif
#ifndef THISTLE_HAVE_NAVIGATOR
#define THISTLE_HAVE_NAVIGATOR 1
#endif
#ifndef THISTLE_HAVE_APPSTORE
#define THISTLE_HAVE_APPSTORE 1
#endif
#ifndef THISTLE_HAVE_VAULT
#define THISTLE_HAVE_VAULT 1
#endif
#ifndef THISTLE_HAVE_TAILSCALE
#define THISTLE_HAVE_TAILSCALE 1
#endif
#ifndef THISTLE_HAVE_HERMES
#define THISTLE_HAVE_HERMES 1
#endif
#ifndef THISTLE_HAVE_TODO
#define THISTLE_HAVE_TODO 1
#endif

#if THISTLE_HAVE_LAUNCHER
#include "launcher/launcher_app.h"
#endif
#if THISTLE_HAVE_SETTINGS
#include "settings/settings_app.h"
#endif
#if THISTLE_HAVE_ASSISTANT
#include "assistant/assistant_app.h"
#endif
#if THISTLE_HAVE_TERMINAL
#include "terminal/terminal_app.h"
#endif
#if THISTLE_HAVE_FILEMGR
#include "file_manager/filemgr_app.h"
#endif
#if THISTLE_HAVE_FILEXFER
#include "filexfer/filexfer_app.h"
#endif
#if THISTLE_HAVE_READER
#include "reader/reader_app.h"
#endif
#if THISTLE_HAVE_PODCASTS
#include "podcasts/podcasts_app.h"
#endif
#if THISTLE_HAVE_PHONE
#include "phone/phone_app.h"
#endif
#if THISTLE_HAVE_MESSENGER
#include "messenger/messenger_app.h"
#endif
#if THISTLE_HAVE_NAVIGATOR
#include "navigator/navigator_app.h"
#endif
#if THISTLE_HAVE_TODO
#include "todo/todo_app.h"
#endif
#if THISTLE_HAVE_APPSTORE
#include "appstore/appstore_app.h"
#endif
#if THISTLE_HAVE_VAULT
#include "vault/vault_app.h"
#endif
#if THISTLE_HAVE_TAILSCALE
#include "tailscale/tailscale_app.h"
#endif
#if THISTLE_HAVE_HERMES
#include "hermes/hermes_app.h"
#endif

/* Keep the first failure and carry on: one app failing to register is no
 * reason to leave the rest of the launcher empty. */
#define REGISTER(fn)                       \
    do {                                   \
        esp_err_t _err = (fn)();           \
        if (first_err == ESP_OK) {         \
            first_err = _err;              \
        }                                  \
    } while (0)

esp_err_t lvgl_apps_register_builtin(bool has_radio, bool has_gps)
{
    /* Both are unused in a build with no capability-gated app compiled in. */
    (void)has_radio;
    (void)has_gps;

    esp_err_t first_err = ESP_OK;

#if THISTLE_HAVE_LAUNCHER
    REGISTER(launcher_app_register);
#endif
#if THISTLE_HAVE_SETTINGS
    REGISTER(settings_app_register);
#endif
#if THISTLE_HAVE_FILEMGR
    REGISTER(filemgr_app_register);
#endif
#if THISTLE_HAVE_FILEXFER
    REGISTER(filexfer_app_register);
#endif
#if THISTLE_HAVE_TAILSCALE
    /* On every board: on one without the client it says so. */
    REGISTER(tailscale_app_register);
#endif
#if THISTLE_HAVE_HERMES
    /* On every board: on one without the tailnet it says so. */
    REGISTER(hermes_app_register);
#endif
#if THISTLE_HAVE_READER
    REGISTER(reader_app_register);
#endif
#if THISTLE_HAVE_PODCASTS
    REGISTER(podcasts_app_register);
#endif
#if THISTLE_HAVE_PHONE
    /* On every board: contacts are worth having without a modem, and the
     * app says where it cannot call. */
    REGISTER(phone_app_register);
#endif
#if THISTLE_HAVE_MESSENGER
    if (has_radio) {
        REGISTER(messenger_app_register);
    }
#endif
#if THISTLE_HAVE_NAVIGATOR
    if (has_gps) {
        REGISTER(navigator_app_register);
    }
#endif
#if THISTLE_HAVE_TODO
    REGISTER(todo_app_register);
#endif
#if THISTLE_HAVE_APPSTORE
    REGISTER(appstore_app_register);
#endif
#if THISTLE_HAVE_ASSISTANT
    REGISTER(assistant_app_register);
#endif
#if THISTLE_HAVE_TERMINAL
    REGISTER(terminal_app_register);
#endif
#if THISTLE_HAVE_VAULT
    REGISTER(vault_app_register);
#endif

    return first_err;
}

#undef REGISTER
