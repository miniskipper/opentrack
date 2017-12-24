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
#include <XPLMMenus.h>


#ifndef PLUGIN_API
#define PLUGIN_API
#endif

#pragma GCC diagnostic ignored "-Wunused-parameter"

/* using Wine name to ease things */
#define WINE_SHM_NAME "facetracknoir-wine-shm"
#define WINE_MTX_NAME "facetracknoir-wine-mtx"

#define MAX_LASTCHANGE 5

#define BUILD_compat
#include "compat/export.hpp"

void menuhandler(void *mRef, void *iRef);


enum Axis {
    TX = 0, TY, TZ, Yaw, Pitch, Roll
};

enum Menu {
    SAVE_OFFSET = 0, DISABLE, ENABLE, TOGGLE_TRANSLATION, RESET_VIEW
};


#ifdef DEBUG
#define DEBUG_LOG(...) mylog(__VA_ARGS__ );
#else
#define DEBUG_LOG(...)
#endif


typedef struct shmdata
{
    void* mem;
    int fd;
    int size;
    bool opened;
    int lastchange;
    char *name;
} shmdata;

typedef struct winedata
{
    double data[6];
    int gameid, gameid2;
    unsigned char table[8];
    bool stop;
} winedata;

static shmdata *shm = NULL;
static winedata *data = NULL;
static double olddata[6];

static void *view_x, *view_y, *view_z, *view_heading, *view_pitch, *view_roll;
static float offset_x, offset_y, offset_z, offset_yaw, offset_pitch, offset_roll, offset_cockpit_x, offset_cockpit_y, offset_cockpit_z;

static XPLMCommandRef track_toggle = NULL, translation_disable_toggle = NULL, reset_view_toggle = NULL;

static int track_disabled = 1;
static int translation_disabled;
static float interval = 2.0;

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


void registerMenus(){
    DEBUG_LOG("Registering menus\n");
    int menuitem = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "opentrack", NULL, 1);
    XPLMMenuID menuid = XPLMCreateMenu("opentrack", XPLMFindPluginsMenu(), menuitem, menuhandler, NULL);
    XPLMAppendMenuItem(menuid, "Enable plugin", (void*)ENABLE, 1);
    XPLMAppendMenuItem(menuid, "Disable plugin", (void*)DISABLE, 1);
    XPLMAppendMenuItem(menuid, "Toggle translation movement", (void*)TOGGLE_TRANSLATION, 1);
    XPLMAppendMenuItem(menuid, "Save View Offset", (void*)SAVE_OFFSET, 1);
    XPLMAppendMenuItem(menuid, "Reset View", (void*)RESET_VIEW, 1);
}

static void save_view() {
    DEBUG_LOG("Save View\n");
    offset_cockpit_x = XPLMGetDataf(view_x);
    offset_cockpit_y = XPLMGetDataf(view_y);
    offset_cockpit_z = XPLMGetDataf(view_z);
    DEBUG_LOG("View saved: %f %f %f\n", offset_cockpit_x, offset_cockpit_y, offset_cockpit_z);
}

static void reinit_translational_offset() {
    DEBUG_LOG("Reinit Offset\n");
    offset_x = data->data[TX];
    offset_y = data->data[TY];
    offset_z = data->data[TZ];
    DEBUG_LOG("Offset Values: %f %f %f\n", offset_x, offset_y, offset_z);
}

static void reinit_rotational_offset() {
    DEBUG_LOG("Reinit View\n");
    offset_yaw = data->data[Yaw];
    offset_pitch = data->data[Pitch];
    offset_roll = data->data[Roll];
    reinit_translational_offset();
    DEBUG_LOG("View Values: %f %f %f\n", offset_yaw, offset_pitch, offset_roll);
}


bool openshm(char *shm_name){
    DEBUG_LOG("openshm %s\n", shm_name);
    shm->name = shm_name;
    shm->fd = shm_open(shm->name, O_RDONLY, 0600);
    if(shm->fd < 0){
        if(interval < 0){
            mylog("shm_open failed: %s\n", strerror(errno));
        }
        interval = 1.0;
        return false;
    }
    shm->mem = mmap(0, sizeof(winedata), PROT_READ, MAP_SHARED, shm->fd, 0);
    if(shm->mem == MAP_FAILED){
        mylog("mmap failed: %s\n", strerror(errno));
        interval = 2.0;
        return false;
    }
    shm->opened = true;
    data = (winedata *)shm->mem;
    DEBUG_LOG("openshm success\n");

    return true;
}

void closeshm(){
    DEBUG_LOG("closeshm %s\n", shm->name);
    munmap(shm->mem, sizeof(winedata));
    close(shm->fd);
    shm->opened = false;
    DEBUG_LOG("shm closed\n");
}

void init(){
    DEBUG_LOG("init\n");
    shm = calloc(1, sizeof(shmdata));
    shm->opened = false;
    DEBUG_LOG("init done\n");
}

#ifdef __GNUC__
#   define OT_UNUSED(varname) varname __attribute__((__unused__))
#else
#   define OT_UNUSED(varname) varname
#endif


float write_head_position(
        float                OT_UNUSED(inElapsedSinceLastCall),
        float                OT_UNUSED(inElapsedTimeSinceLastFlightLoop),
        int                  OT_UNUSED(inCounter),
        void *               OT_UNUSED(inRefcon) )
{

    DEBUG_LOG("frameloop %i  lastchange=%i\n", inCounter, shm->lastchange);
    if(!shm->opened){
        if(!openshm(WINE_SHM_NAME)){
            DEBUG_LOG("openshm failed\n");
        }
    }

    if(shm->opened){
        DEBUG_LOG("compare data\n");
        if(memcmp(olddata, data->data, sizeof(olddata)) != 0){
            DEBUG_LOG("data changed, setting view\n");
            shm->lastchange=0;
            interval = -1.0;            
        } else {
            shm->lastchange++;
            if(shm->lastchange > MAX_LASTCHANGE && interval < 0){
                mylog("Tracking stopped\n");
                interval = 2.0;
                closeshm();
            }
        }
    }

    if(shm->opened && shm->lastchange <= MAX_LASTCHANGE){
        if (!translation_disabled)
        {
            DEBUG_LOG("Setting new translation data: %f %f %f\n", data->data[TX], data->data[TY], data->data[TZ]);
            XPLMSetDataf(view_x, (data->data[TX] - offset_x) * 1e-3 * -1 + offset_cockpit_x);
            XPLMSetDataf(view_y, (data->data[TY] - offset_y) * 1e-3 + offset_cockpit_y);
            XPLMSetDataf(view_z, (data->data[TZ] - offset_z) * 1e-3 + offset_cockpit_z);
        }
        DEBUG_LOG("Setting new rotation data: %f %f %f\n", data->data[Yaw], data->data[Pitch], data->data[Roll]);
        XPLMSetDataf(view_heading, (data->data[Yaw] - offset_yaw) * 180 / M_PI);
        XPLMSetDataf(view_pitch, (data->data[Pitch] - offset_pitch ) * 180 / M_PI);
        XPLMSetDataf(view_roll, (data->data[Roll] - offset_roll) * 180 * -1 / M_PI);

        DEBUG_LOG("Save current data\n");
        memcpy(&olddata, &data->data, sizeof(olddata));
    } else {
        //reset roll, otherwise it would be stuck at last angle
        XPLMSetDataf(view_roll, 0);        
    }
    
    return interval;
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
            reinit_translational_offset();
    }
    else
    {
        //Disable
        XPLMUnregisterFlightLoopCallback(write_head_position, NULL);
        XPLMSetDataf(view_roll, 0);
        closeshm();
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
        reinit_translational_offset();
    }
    return 0;
}

static int ResetViewToggleHandler( XPLMCommandRef inCommand,
                                     XPLMCommandPhase inPhase,
                                     void * inRefCon )
{
    if (!track_disabled)
    {
        // Reinit the offsets when we re-enable the translations so that we can "move around"
        reinit_rotational_offset();
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
    reset_view_toggle = XPLMCreateCommand("opentrack/reset_view", "Reset the 3D View");

    DEBUG_LOG("Registering commands\n");
    XPLMRegisterCommandHandler( track_toggle,
                                TrackToggleHandler,
                                1,
                                (void*)0);

    XPLMRegisterCommandHandler( translation_disable_toggle,
                                TranslationToggleHandler,
                                1,
                                (void*)0);
                                
    XPLMRegisterCommandHandler( reset_view_toggle,
                                ResetViewToggleHandler,
                                1,
                                (void*)0);

    if (view_x && view_y && view_z && view_heading && view_pitch && view_roll && track_toggle && translation_disable_toggle) {
        init();
        registerMenus(); 
        strcpy(outName, "opentrack");
        strcpy(outSignature, "opentrack - freetrack lives!");
        strcpy(outDescription, "head tracking view control");
        mylog("opentrack init complete\n");
        return 1;
    }
    return 0;
}

PLUGIN_API OTR_COMPAT_EXPORT void XPluginStop ( void ) {
    if (shm->opened)
    {
        mylog("Stop plugin\n");
        closeshm();
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
    closeshm();
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
        save_view();
        break;
    default:
        break;
    }
}


void menuhandler(void *mRef, void *iRef){
    unsigned int ref = (uintptr_t) iRef;
    DEBUG_LOG("menuitem %i\n", ref);
    switch(ref){
    case RESET_VIEW:
        reinit_rotational_offset();
        break;
    case SAVE_OFFSET:
        save_view();
        break;
    case ENABLE:
        XPluginEnable();
        break;
    case DISABLE:
        XPluginDisable();
        break;
    case TOGGLE_TRANSLATION:
        mylog("toggle translation\n");
        translation_disabled = !translation_disabled;
        break;
    default:
        break;
    }
}
