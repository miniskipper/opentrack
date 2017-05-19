/* Copyright (c) 2013, Stanislaw Halik <sthalik@misaki.pl>

 * Permission to use, copy, modify, and/or distribute this
 * software for any purpose with or without fee is hereby granted,
 * provided that the above copyright notice and this permission
 * notice appear in all copies.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <limits.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>

#include <XPLMPlugin.h>
#include <XPLMDataAccess.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#ifndef PLUGIN_API
#define PLUGIN_API
#endif

#pragma GCC diagnostic ignored "-Wunused-parameter"

/* using Wine name to ease things */
#define WINE_SHM_NAME "facetracknoir-wine-shm"
#define WINE_MTX_NAME "facetracknoir-wine-mtx"

#define BUILD_compat
#include "compat/export.hpp"

enum Axis {
    TX = 0, TY, TZ, Yaw, Pitch, Roll
};



#ifdef DEBUG
#define DEBUG_LOG(...) mylog(__VA_ARGS__ );
#else
#define DEBUG_LOG(...)
#endif





typedef struct PortableLockedShm
{
    void* mem;
    int fd, size;
} PortableLockedShm;

typedef struct WineSHM
{
    double data[6];
    int gameid, gameid2;
    unsigned char table[8];
    bool stop;
} WineSHM;

static PortableLockedShm* lck_posix = NULL;
static WineSHM* shm_posix = NULL;
static double data_last[6];
static void *view_x, *view_y, *view_z, *view_heading, *view_pitch, *view_roll;
static float offset_x, offset_y, offset_z;
static XPLMCommandRef track_toggle = NULL, translation_disable_toggle = NULL;
static int track_disabled = 1;
static int translation_disabled;

static float nextrun = -1.0;

void mylog(char *format, ...){
    char buffer[1024];
    memset(buffer, 0, 1024);
    strcpy(buffer, "opentrack: ");
    va_list args;
    va_start(args, format);
    vsprintf(buffer+11, format, args);
    XPLMDebugString(buffer);
    va_end(args);
}

static void reinit_offset() {
    DEBUG_LOG("Reinit Offset\n");
    offset_x = XPLMGetDataf(view_x);
    offset_y = XPLMGetDataf(view_y);
    offset_z = XPLMGetDataf(view_z);
    DEBUG_LOG("Offset Values: %f %f %f\n", offset_x, offset_y, offset_z);
}

#ifdef __GNUC__
#   define OT_UNUSED(varname) varname __attribute__((__unused__))
#else
#   define OT_UNUSED(varname) varname
#endif



PortableLockedShm* PortableLockedShm_init(const char *shmName, const char *OT_UNUSED(mutexName), int mapSize)
{
    DEBUG_LOG("Initializing SHM\n");
    PortableLockedShm* self = malloc(sizeof(PortableLockedShm));
    char shm_filename[NAME_MAX];
    shm_filename[0] = '/';
    strncpy(shm_filename+1, shmName, NAME_MAX-2);
    shm_filename[NAME_MAX-1] = '\0';
    /* (void) shm_unlink(shm_filename); */
    self->fd = shm_open(shm_filename, O_RDWR | O_CREAT, 0600);
    if(self->fd < 0){
        mylog("shm_open failed: %s\n", strerror(errno));
        return NULL;
    }
    if(ftruncate(self->fd, mapSize) != 0){
        mylog("ftruncate failed: %s\n", strerror(errno));
        return NULL;
    }
    self->mem = mmap(NULL, mapSize, PROT_READ|PROT_WRITE, MAP_SHARED, self->fd, (off_t)0);
    if(self->mem == (void *)-1){
        mylog("mmap failed: %s\n", strerror(errno));
        return NULL;
    }
    DEBUG_LOG("SHM Initialized\n");
    return self;
}

void PortableLockedShm_free(PortableLockedShm* self)
{
    DEBUG_LOG("Freeing SHM\n");
    (void) munmap(self->mem, self->size);
    (void) close(self->fd);
    free(self);
    DEBUG_LOG("SHM freed\n");
}

void PortableLockedShm_lock(PortableLockedShm* self)
{
    DEBUG_LOG("Locking SHM\n");
    flock(self->fd, LOCK_SH);
    DEBUG_LOG("Locked SHM\n");
}

void PortableLockedShm_unlock(PortableLockedShm* self)
{
    DEBUG_LOG("Unlocking SHM\n");
    flock(self->fd, LOCK_UN);
    DEBUG_LOG("Unlocked SHM\n");
}

float write_head_position(
        float                OT_UNUSED(inElapsedSinceLastCall),
        float                OT_UNUSED(inElapsedTimeSinceLastFlightLoop),
        int                  OT_UNUSED(inCounter),
        void *               OT_UNUSED(inRefcon) )
{
    if (lck_posix != NULL && shm_posix != NULL) {

        PortableLockedShm_lock(lck_posix);
        //only set the view if tracking is running
        if(memcmp(shm_posix, data_last, sizeof(shm_posix->data)) != 0){
            DEBUG_LOG("View changed, setting new data\n");
            //we received data, from now on run every frame
            nextrun = -1.0;
            if (!translation_disabled)
            {
                DEBUG_LOG("Setting new translation data: %f %f %f\n", shm_posix->data[TX], shm_posix->data[TY], shm_posix->data[TZ]);
                XPLMSetDataf(view_x, shm_posix->data[TX] * 1e-3 + offset_x);
                XPLMSetDataf(view_y, shm_posix->data[TY] * 1e-3 + offset_y);
                XPLMSetDataf(view_z, shm_posix->data[TZ] * 1e-3 + offset_z);
            }
            DEBUG_LOG("Setting new rotation data: %f %f %f\n", shm_posix->data[Yaw], shm_posix->data[Pitch], shm_posix->data[Roll]);
            XPLMSetDataf(view_heading, shm_posix->data[Yaw] * 180 / M_PI);
            XPLMSetDataf(view_pitch, shm_posix->data[Pitch] * 180 / M_PI);
            XPLMSetDataf(view_roll, shm_posix->data[Roll] * 180 / M_PI);
        } else {
            //reset roll, otherwise it would be stuck at last angle
            if(nextrun < 0){
                mylog("Tracking stopped\n");
            }
            XPLMSetDataf(view_roll, 0);
            //from now on only run every 2 seconds
            nextrun = 2.0;
        }
        
        DEBUG_LOG("Save current data\n");
        memcpy(&data_last, &shm_posix->data, sizeof(data_last));

        PortableLockedShm_unlock(lck_posix);
    }
    return nextrun;
}

static int TrackToggleHandler( XPLMCommandRef inCommand,
                               XPLMCommandPhase inPhase,
                               void * inRefCon )
{
    if ( track_disabled )
    {
        //Enable
        XPLMRegisterFlightLoopCallback(write_head_position, -1.0, NULL);

        // Reinit the offsets when we re-enable the plugin
        if ( !translation_disabled )
            reinit_offset();
    }
    else
    {
        //Disable
        XPLMUnregisterFlightLoopCallback(write_head_position, NULL);
    }
    track_disabled = !track_disabled;
    return 0;
}

static int TranslationToggleHandler( XPLMCommandRef inCommand,
                                     XPLMCommandPhase inPhase,
                                     void * inRefCon )
{
    translation_disabled = !translation_disabled;
    if (!translation_disabled)
    {
        // Reinit the offsets when we re-enable the translations so that we can "move around"
        reinit_offset();
    }
    return 0;
}

PLUGIN_API OTR_COMPAT_EXPORT int XPluginStart ( char * outName, char * outSignature, char * outDescription ) {
    mylog("Start plugin\n");
    DEBUG_LOG("Searching DataRefs\n");
    view_x = XPLMFindDataRef("sim/graphics/view/pilots_head_x");
    view_y = XPLMFindDataRef("sim/graphics/view/pilots_head_y");
    view_z = XPLMFindDataRef("sim/graphics/view/pilots_head_z");
    view_heading = XPLMFindDataRef("sim/graphics/view/pilots_head_psi");
    view_pitch = XPLMFindDataRef("sim/graphics/view/pilots_head_the");
    view_roll = XPLMFindDataRef("sim/graphics/view/field_of_view_roll_deg");


    DEBUG_LOG("Creating commands\n");
    track_toggle = XPLMCreateCommand("opentrack/toggle", "Disable/Enable head tracking");
    translation_disable_toggle = XPLMCreateCommand("opentrack/toggle_translation", "Disable/Enable input translation from opentrack");

    DEBUG_LOG("Registering commands\n");
    XPLMRegisterCommandHandler( track_toggle,
                                TrackToggleHandler,
                                1,
                                (void*)0);

    XPLMRegisterCommandHandler( translation_disable_toggle,
                                TranslationToggleHandler,
                                1,
                                (void*)0);


    if (view_x && view_y && view_z && view_heading && view_pitch && view_roll && track_toggle && translation_disable_toggle) {
        lck_posix = PortableLockedShm_init(WINE_SHM_NAME, WINE_MTX_NAME, sizeof(WineSHM));
        if (lck_posix == NULL || lck_posix->mem == (void*)-1) {
            fprintf(stderr, "opentrack failed to init SHM!\n");
            return 0;
        }
        shm_posix = (WineSHM*) lck_posix->mem;
        memset(shm_posix, 0, sizeof(WineSHM));
        strcpy(outName, "opentrack");
        strcpy(outSignature, "opentrack - freetrack lives!");
        strcpy(outDescription, "head tracking view control");
        mylog("opentrack init complete\n");
        return 1;
    }
    return 0;
}

PLUGIN_API OTR_COMPAT_EXPORT void XPluginStop ( void ) {
    if (lck_posix)
    {
        mylog("Stop plugin\n");
        PortableLockedShm_free(lck_posix);
        lck_posix = NULL;
        shm_posix = NULL;
    }
}

PLUGIN_API OTR_COMPAT_EXPORT void XPluginEnable ( void ) {
    mylog("Enable plugin\n");
    XPLMRegisterFlightLoopCallback(write_head_position, -1.0, NULL);
    track_disabled = 0;
}

PLUGIN_API OTR_COMPAT_EXPORT void XPluginDisable ( void ) {
    mylog("Disable plugin\n");
    XPLMUnregisterFlightLoopCallback(write_head_position, NULL);
    track_disabled = 1;
}

PLUGIN_API OTR_COMPAT_EXPORT void XPluginReceiveMessage(
        XPLMPluginID    OT_UNUSED(inFromWho),
        int             OT_UNUSED(inMessage),
        void *          OT_UNUSED(inParam))
{
    DEBUG_LOG("Message received: %d\n", inMessage);
    switch (inMessage) {
    case XPLM_MSG_PLANE_LOADED:
    case XPLM_MSG_AIRPORT_LOADED:
        reinit_offset();
        break;
    default:
        break;
    }
}
