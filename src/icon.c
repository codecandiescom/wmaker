/* icon.c - window icon and dock and appicon parent
 *
 *  Window Maker window manager
 *
 *  Copyright (c) 1997-2003 Alfredo K. Kojima
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "wconfig.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <wraster.h>
#include <sys/stat.h>

#include "WindowMaker.h"
#include "wcore.h"
#include "texture.h"
#include "window.h"
#include "icon.h"
#include "actions.h"
#include "funcs.h"
#include "stacking.h"
#include "application.h"
#include "defaults.h"
#include "appicon.h"
#include "wmspec.h"

/**** Global varianebles ****/
extern WPreferences wPreferences;

#define MOD_MASK wPreferences.modifier_mask
#define CACHE_ICON_PATH "/Library/WindowMaker/CachedPixmaps"
#define ICON_BORDER 3

extern Cursor wCursor[WCUR_LAST];

static void miniwindowExpose(WObjDescriptor * desc, XEvent * event);
static void miniwindowMouseDown(WObjDescriptor * desc, XEvent * event);
static void miniwindowDblClick(WObjDescriptor * desc, XEvent * event);

static WIcon *icon_create_core(WScreen *scr, int coord_x, int coord_y);

static void get_pixmap_icon_from_icon_win(WIcon *icon);
static int get_pixmap_icon_from_wm_hints(WIcon *icon);
static int get_rimage_icon_from_wm_hints(WIcon *icon);
static void get_pixmap_icon_from_user_icon(WIcon *icon);
static void get_rimage_icon_from_user_icon(WIcon *icon);
static void get_pixmap_icon_from_default_icon(WIcon *icon);
static void get_rimage_icon_from_default_icon(WIcon *icon);
static void get_pixmap_icon_from_x11(WIcon *icon);
static void get_rimage_icon_from_x11(WIcon *icon);

static void icon_update_pixmap(WIcon *icon, RImage *image);
static void unset_icon_image(WIcon *icon);

/****** Notification Observers ******/

static void appearanceObserver(void *self, WMNotification * notif)
{
	WIcon *icon = (WIcon *) self;
	uintptr_t flags = (uintptr_t)WMGetNotificationClientData(notif);

	if ((flags & WTextureSettings) || (flags & WFontSettings))
		icon->force_paint = 1;

	wIconPaint(icon);

	/* so that the appicon expose handlers will paint the appicon specific
	 * stuff */
	XClearArea(dpy, icon->core->window, 0, 0, icon->core->width, icon->core->height, True);
}

static void tileObserver(void *self, WMNotification * notif)
{
	WIcon *icon = (WIcon *) self;

	icon->force_paint = 1;
	wIconPaint(icon);

	XClearArea(dpy, icon->core->window, 0, 0, 1, 1, True);
}

/************************************/

static int getSize(Drawable d, unsigned int *w, unsigned int *h, unsigned int *dep)
{
	Window rjunk;
	int xjunk, yjunk;
	unsigned int bjunk;

	return XGetGeometry(dpy, d, &rjunk, &xjunk, &yjunk, w, h, &bjunk, dep);
}

WIcon *icon_create_for_wwindow(WWindow *wwin)
{
	WScreen *scr = wwin->screen_ptr;
	WIcon *icon;
	char *file;

	icon = icon_create_core(scr, wwin->icon_x, wwin->icon_y);

	icon->owner = wwin;
	if (wwin->wm_hints && (wwin->wm_hints->flags & IconWindowHint)) {
		if (wwin->client_win == wwin->main_window) {
			WApplication *wapp;
			/* do not let miniwindow steal app-icon's icon window */
			wapp = wApplicationOf(wwin->client_win);
			if (!wapp || wapp->app_icon == NULL)
				icon->icon_win = wwin->wm_hints->icon_window;
		} else {
			icon->icon_win = wwin->wm_hints->icon_window;
		}
	}
#ifdef NO_MINIWINDOW_TITLES
	icon->show_title = 0;
#else
	icon->show_title = 1;
#endif

	icon->icon_name = wNETWMGetIconName(wwin->client_win);
	if (icon->icon_name)
		wwin->flags.net_has_icon_title = 1;
	else
		wGetIconName(dpy, wwin->client_win, &icon->icon_name);

	/* Get the application icon, default included */
	file = get_default_icon_filename(scr, wwin->wm_instance, wwin->wm_class, NULL, True);
	if (file) {
		icon->file = wstrdup(file);
		icon->file_image = get_rimage_from_file(scr, icon->file, wPreferences.icon_size);
		wfree(file);
	}

	icon->tile_type = TILE_NORMAL;

	wIconUpdate(icon);

	WMAddNotificationObserver(appearanceObserver, icon, WNIconAppearanceSettingsChanged, icon);
	WMAddNotificationObserver(tileObserver, icon, WNIconTileSettingsChanged, icon);

	return icon;
}

WIcon *icon_create_for_dock(WScreen *scr, char *command, char *wm_instance, char *wm_class, int tile)
{
	WIcon *icon;
	char *file = NULL;

	icon = icon_create_core(scr, 0, 0);

	/* Search the icon using instance and class, without default icon */
	file = get_default_icon_filename(scr, wm_instance, wm_class, command, False);
	if (file) {
		icon->file = wstrdup(file);
		icon->file_image = get_rimage_from_file(scr, icon->file, wPreferences.icon_size);
		wfree(file);
	}

	icon->tile_type = tile;

	wIconUpdate(icon);

	WMAddNotificationObserver(appearanceObserver, icon, WNIconAppearanceSettingsChanged, icon);
	WMAddNotificationObserver(tileObserver, icon, WNIconTileSettingsChanged, icon);

	return icon;
}

static WIcon *icon_create_core(WScreen *scr, int coord_x, int coord_y)
{
	WIcon *icon;
	unsigned long vmask = 0;
	XSetWindowAttributes attribs;

	icon = wmalloc(sizeof(WIcon));
	icon->core = wCoreCreateTopLevel(scr,
					 coord_x,
					 coord_y,
					 wPreferences.icon_size,
					 wPreferences.icon_size,
					 0, scr->w_depth, scr->w_visual, scr->w_colormap);

	if (wPreferences.use_saveunders) {
		vmask = CWSaveUnder;
		attribs.save_under = True;
	}

	/* a white border for selecting it */
	vmask |= CWBorderPixel;
	attribs.border_pixel = scr->white_pixel;

	XChangeWindowAttributes(dpy, icon->core->window, vmask, &attribs);

	/* will be overriden if this is a application icon */
	icon->core->descriptor.handle_mousedown = miniwindowMouseDown;
	icon->core->descriptor.handle_expose = miniwindowExpose;
	icon->core->descriptor.parent_type = WCLASS_MINIWINDOW;
	icon->core->descriptor.parent = icon;

	icon->core->stacking = wmalloc(sizeof(WStacking));
	icon->core->stacking->above = NULL;
	icon->core->stacking->under = NULL;
	icon->core->stacking->window_level = NORMAL_ICON_LEVEL;
	icon->core->stacking->child_of = NULL;

	/* Icon image */
	icon->file = NULL;
	icon->file_image = NULL;

	return icon;
}

void wIconDestroy(WIcon * icon)
{
	WCoreWindow *core = icon->core;
	WScreen *scr = core->screen_ptr;

	WMRemoveNotificationObserver(icon);

	if (icon->handlerID)
		WMDeleteTimerHandler(icon->handlerID);

	if (icon->icon_win) {
		int x = 0, y = 0;

		if (icon->owner) {
			x = icon->owner->icon_x;
			y = icon->owner->icon_y;
		}
		XUnmapWindow(dpy, icon->icon_win);
		XReparentWindow(dpy, icon->icon_win, scr->root_win, x, y);
	}
	if (icon->icon_name)
		XFree(icon->icon_name);

	if (icon->pixmap)
		XFreePixmap(dpy, icon->pixmap);

	unset_icon_image(icon);

	wCoreDestroy(icon->core);
	wfree(icon);
}

static void drawIconTitle(WScreen * scr, Pixmap pixmap, int height)
{
	XFillRectangle(dpy, pixmap, scr->icon_title_texture->normal_gc, 0, 0, wPreferences.icon_size, height + 1);
	XDrawLine(dpy, pixmap, scr->icon_title_texture->light_gc, 0, 0, wPreferences.icon_size, 0);
	XDrawLine(dpy, pixmap, scr->icon_title_texture->light_gc, 0, 0, 0, height + 1);
	XDrawLine(dpy, pixmap, scr->icon_title_texture->dim_gc,
		  wPreferences.icon_size - 1, 0, wPreferences.icon_size - 1, height + 1);
}

static void icon_update_pixmap(WIcon *icon, RImage *image)
{
	RImage *tile;
	Pixmap pixmap;
	int x, y, sx, sy;
	unsigned w, h;
	int theight = 0;
	WScreen *scr = icon->core->screen_ptr;
	int titled = icon->show_title;

	if (icon->tile_type == TILE_NORMAL) {
		tile = RCloneImage(scr->icon_tile);
	} else {
		assert(scr->clip_tile);
		tile = RCloneImage(scr->clip_tile);
	}

	if (image) {
		w = (image->width > wPreferences.icon_size)
		    ? wPreferences.icon_size : image->width;
		x = (wPreferences.icon_size - w) / 2;
		sx = (image->width - w) / 2;

		if (titled)
			theight = WMFontHeight(scr->icon_title_font);

		h = (image->height + theight > wPreferences.icon_size
		     ? wPreferences.icon_size - theight : image->height);
		y = theight + (wPreferences.icon_size - theight - h) / 2;
		sy = (image->height - h) / 2;

		RCombineArea(tile, image, sx, sy, w, h, x, y);
	}

	if (icon->shadowed) {
		RColor color;

		color.red = scr->icon_back_texture->light.red >> 8;
		color.green = scr->icon_back_texture->light.green >> 8;
		color.blue = scr->icon_back_texture->light.blue >> 8;
		color.alpha = 150;	/* about 60% */
		RClearImage(tile, &color);
	}

	if (icon->highlighted) {
		RColor color;

		color.red = color.green = color.blue = 0;
		color.alpha = 160;
		RLightImage(tile, &color);
	}

	if (!RConvertImage(scr->rcontext, tile, &pixmap))
		wwarning(_("error rendering image:%s"), RMessageForError(RErrorCode));

	RReleaseImage(tile);

	if (titled)
		drawIconTitle(scr, pixmap, theight);

	icon->pixmap = pixmap;
}

void wIconChangeTitle(WIcon * icon, char *new_title)
{
	int changed;

	changed = (new_title == NULL && icon->icon_name != NULL)
	    || (new_title != NULL && icon->icon_name == NULL);

	if (icon->icon_name != NULL)
		XFree(icon->icon_name);

	icon->icon_name = new_title;

	if (changed)
		icon->force_paint = 1;
	wIconPaint(icon);
}

RImage *wIconValidateIconSize(RImage *icon, int max_size)
{
	RImage *nimage;

	if (!icon)
		return NULL;

	/* We should hold "ICON_BORDER" (~2) pixels to include the icon border */
	if ((icon->width - max_size) > -ICON_BORDER ||
	    (icon->height - max_size) > -ICON_BORDER) {
		nimage = RScaleImage(icon, max_size - ICON_BORDER,
				     (icon->height * (max_size - ICON_BORDER) / icon->width));
		RReleaseImage(icon);
		icon = nimage;
	}

	return icon;
}

Bool wIconChangeImageFile(WIcon *icon, char *file)
{
	WScreen *scr = icon->core->screen_ptr;
	char *path;
	RImage *image = NULL;
	int error = 0;

	/* If no new image, don't do nothing */
	if (!file)
		return True;

	/* Find the new image */
	path = FindImage(wPreferences.icon_path, file);
	if (path)
		image = get_rimage_from_file(scr, path, wPreferences.icon_size);
	else
		error = 1;

	/* New image! */
	if (!error && image) {
		/* Remove the old one */
		unset_icon_image(icon);

		/* Set the new image */
		icon->file_image = image;
		icon->file = wstrdup(path);
		wIconUpdate(icon);
	} else {
		error = 1;
	}

	if (path)
		wfree(path);

	return !error;
}

static char *get_name_for_wwin(WWindow *wwin)
{
	return get_name_for_instance_class(wwin->wm_instance, wwin->wm_class);
}

char *get_name_for_instance_class(char *wm_instance, char *wm_class)
{
	char *suffix;
	int len;

	if (wm_class && wm_instance) {
		len = strlen(wm_class) + strlen(wm_instance) + 2;
		suffix = wmalloc(len);
		snprintf(suffix, len, "%s.%s", wm_instance, wm_class);
	} else if (wm_class) {
		len = strlen(wm_class) + 1;
		suffix = wmalloc(len);
		snprintf(suffix, len, "%s", wm_class);
	} else if (wm_instance) {
		len = strlen(wm_instance) + 1;
		suffix = wmalloc(len);
		snprintf(suffix, len, "%s", wm_instance);
	} else {
		return NULL;
	}

	return suffix;
}

static char *get_icon_cache_path(void)
{
	char *prefix, *path;
	int len, ret;

	prefix = wusergnusteppath();
	len = strlen(prefix) + strlen(CACHE_ICON_PATH) + 2;
	path = wmalloc(len);
	snprintf(path, len, "%s%s/", prefix, CACHE_ICON_PATH);

	/* If the folder exists, exit */
	if (access(path, F_OK) == 0)
		return path;

	/* Create the folder */
	ret = wmkdirhier((const char *) path);

	/* Exit 1 on success, 0 on failure */
	if (ret == 1)
		return path;

	/* Fail */
	wfree(path);
	return NULL;
}

static RImage *get_wwindow_image_from_wmhints(WWindow *wwin, WIcon *icon)
{
	RImage *image = NULL;
	XWMHints *hints = wwin->wm_hints;

	if (hints && (hints->flags & IconPixmapHint) && hints->icon_pixmap != None)
		image = RCreateImageFromDrawable(icon->core->screen_ptr->rcontext,
						 hints->icon_pixmap,
						 (hints->flags & IconMaskHint)
						 ? hints->icon_mask : None);

	return image;
}

/*
 * wIconStore--
 * 	Stores the client supplied icon at CACHE_ICON_PATH
 * and returns the path for that icon. Returns NULL if there is no
 * client supplied icon or on failure.
 *
 * Side effects:
 * 	New directories might be created.
 */
char *wIconStore(WIcon * icon)
{
	char *path, *dir_path, *file;
	int len = 0;
	RImage *image = NULL;
	WWindow *wwin = icon->owner;

	if (!wwin)
		return NULL;

	dir_path = get_icon_cache_path();
	if (!dir_path)
		return NULL;

	file = get_name_for_wwin(wwin);
	if (!file) {
		wfree(dir_path);
		return NULL;
	}

	len = strlen(dir_path) + strlen(file) + 5;
	path = wmalloc(len);
	snprintf(path, len, "%s%s.xpm", dir_path, file);
	wfree(dir_path);
	wfree(file);

	/* If icon exists, exit */
	if (access(path, F_OK) == 0)
		return path;

	if (wwin->net_icon_image)
		image = RRetainImage(wwin->net_icon_image);
	else
		image = get_wwindow_image_from_wmhints(wwin, icon);

	if (!image) {
		wfree(path);
		return NULL;
	}

	if (!RSaveImage(image, path, "XPM")) {
		wfree(path);
		path = NULL;
	}

	RReleaseImage(image);

	return path;
}

static void cycleColor(void *data)
{
	WIcon *icon = (WIcon *) data;
	WScreen *scr = icon->core->screen_ptr;
	XGCValues gcv;

	icon->step--;
	gcv.dash_offset = icon->step;
	XChangeGC(dpy, scr->icon_select_gc, GCDashOffset, &gcv);

	XDrawRectangle(dpy, icon->core->window, scr->icon_select_gc, 0, 0,
		       icon->core->width - 1, icon->core->height - 1);
	icon->handlerID = WMAddTimerHandler(COLOR_CYCLE_DELAY, cycleColor, icon);
}

#ifdef NEWAPPICON
void wIconSetHighlited(WIcon *icon, Bool flag)
{
	if (icon->highlighted == flag)
		return;

	icon->highlighted = flag;
	icon->force_paint = True;
	wIconPaint(icon);
}
#endif

void wIconSelect(WIcon * icon)
{
	WScreen *scr = icon->core->screen_ptr;
	icon->selected = !icon->selected;

	if (icon->selected) {
		icon->step = 0;
		if (!wPreferences.dont_blink)
			icon->handlerID = WMAddTimerHandler(10, cycleColor, icon);
		else
			XDrawRectangle(dpy, icon->core->window, scr->icon_select_gc, 0, 0,
				       icon->core->width - 1, icon->core->height - 1);
	} else {
		if (icon->handlerID) {
			WMDeleteTimerHandler(icon->handlerID);
			icon->handlerID = NULL;
		}
		XClearArea(dpy, icon->core->window, 0, 0, icon->core->width, icon->core->height, True);
	}
}

static void unset_icon_image(WIcon *icon)
{
	if (icon->file)
		wfree(icon->file);

	if (icon->file_image)
		RReleaseImage(icon->file_image);
}

void wIconUpdate(WIcon *icon)
{
	WScreen *scr = icon->core->screen_ptr;
	WWindow *wwin = icon->owner;

	assert(scr->icon_tile != NULL);

	if (icon->pixmap != None)
		XFreePixmap(dpy, icon->pixmap);

	icon->pixmap = None;

	if (wwin && WFLAGP(wwin, always_user_icon)) {
		/* Forced use user_icon */
		get_pixmap_icon_from_user_icon(icon);
	} else if (icon->icon_win != None) {
		/* Get the Pixmap from the WIcon */
		get_pixmap_icon_from_icon_win(icon);
	} else if (wwin && wwin->net_icon_image) {
		/* Use _NET_WM_ICON icon */
		get_pixmap_icon_from_x11(icon);
	} else if (wwin && wwin->wm_hints && (wwin->wm_hints->flags & IconPixmapHint)) {
		/* Get the Pixmap from the wm_hints, else, from the user */
		if (get_pixmap_icon_from_wm_hints(icon))
			get_pixmap_icon_from_user_icon(icon);
	} else {
		/* Get the Pixmap from the user */
		get_pixmap_icon_from_user_icon(icon);
	}

	/* No pixmap, set default background */
	if (icon->pixmap != None)
		XSetWindowBackgroundPixmap(dpy, icon->core->window, icon->pixmap);

	/* Paint it */
	XClearWindow(dpy, icon->core->window);
	wIconPaint(icon);
}

static void get_pixmap_icon_from_x11(WIcon *icon)
{
	/* Set the icon->file_image */
	get_rimage_icon_from_x11(icon);

	/* Update icon->pixmap */
	icon_update_pixmap(icon, icon->file_image);
}

static void get_rimage_icon_from_x11(WIcon *icon)
{
	/* Remove the icon image */
	unset_icon_image(icon);

	/* Set the new icon image */
	icon->file = NULL;
	icon->file_image = RRetainImage(icon->owner->net_icon_image);
}

static void get_rimage_icon_from_user_icon(WIcon *icon)
{
	if (icon->file_image)
		return;

	get_rimage_icon_from_default_icon(icon);
}

static void get_pixmap_icon_from_user_icon(WIcon *icon)
{
	/* Set the icon->file_image */
	get_rimage_icon_from_user_icon(icon);

	/* Update icon->pixmap */
	icon_update_pixmap(icon, icon->file_image);
}

static void get_rimage_icon_from_default_icon(WIcon *icon)
{
	WScreen *scr = icon->core->screen_ptr;

	/* If the icon don't have image, we should use the default image. */
	if (!scr->def_icon_rimage)
		scr->def_icon_rimage = get_default_image(scr);

	/* Remove the icon image */
	unset_icon_image(icon);

	/* Set the new icon image */
	icon->file = NULL;
	icon->file_image = RRetainImage(scr->def_icon_rimage);
}

static void get_pixmap_icon_from_default_icon(WIcon *icon)
{
	/* Update icon->file image */
	get_rimage_icon_from_default_icon(icon);

	/* Now, create the pixmap using the default (saved) image */
	icon_update_pixmap(icon, icon->file_image);
}

/* Get the Pixmap from the WIcon of the WWindow */
static void get_pixmap_icon_from_icon_win(WIcon *icon)
{
	XWindowAttributes attr;
	RImage *image;
	WScreen *scr = icon->core->screen_ptr;
	int title_height = WMFontHeight(scr->icon_title_font);
	unsigned int w, h, d;
	int theight = 0;

	/* Create the new RImage */
	image = get_window_image_from_x11(icon->icon_win);

	/* Free the icon info */
	unset_icon_image(icon);

	/* Set the new info */
	icon->file = NULL;
	icon->file_image = image;

	/* Paint the image at the icon */
	icon_update_pixmap(icon, image);

	/* Reparent the dock application to the icon */

	/* We need the application size to center it
	 * and show in the correct position */
	getSize(icon->icon_win, &w, &h, &d);

	/* Set extra space for title */
	if (icon->show_title && (h + title_height < wPreferences.icon_size)) {
		theight = title_height;
		drawIconTitle(scr, icon->pixmap, theight);
	} else {
                XSetWindowBackgroundPixmap(dpy, icon->core->window, scr->icon_tile_pixmap);
        }

	/* Set the icon border */
	XSetWindowBorderWidth(dpy, icon->icon_win, 0);

	/* Put the dock application in the icon */
	XReparentWindow(dpy, icon->icon_win, icon->core->window,
			(wPreferences.icon_size - w) / 2,
			theight + (wPreferences.icon_size - h - theight) / 2);

	/* Show it and save */
	XMapWindow(dpy, icon->icon_win);
	XAddToSaveSet(dpy, icon->icon_win);

	/* Needed to move the icon clicking on the application part */
	if ((XGetWindowAttributes(dpy, icon->icon_win, &attr)) &&
	    (attr.all_event_masks & ButtonPressMask))
		wHackedGrabButton(Button1, MOD_MASK, icon->core->window, True,
				  ButtonPressMask, GrabModeSync, GrabModeAsync,
				  None, wCursor[WCUR_ARROW]);
}

/* Get the RImage from the XWindow wm_hints */
static int get_rimage_icon_from_wm_hints(WIcon *icon)
{
	RImage *image = NULL;
	unsigned int w, h, d;
	WWindow *wwin = icon->owner;

	if (!getSize(wwin->wm_hints->icon_pixmap, &w, &h, &d)) {
		icon->owner->wm_hints->flags &= ~IconPixmapHint;
		return 1;
	}

	image = get_wwindow_image_from_wmhints(wwin, icon);
	if (!image)
		return 1;

	/* FIXME: If unset_icon_image, pointer double free then crash 
	unset_icon_image(icon); */
	icon->file_image = image;

	return 0;
}

/* Get the Pixmap from the XWindow wm_hints */
static int get_pixmap_icon_from_wm_hints(WIcon *icon)
{
	int ret;

	ret = get_rimage_icon_from_wm_hints(icon);
	if (ret == 0)
		icon_update_pixmap(icon, icon->file_image);

	return ret;
}

void wIconPaint(WIcon * icon)
{
	WScreen *scr = icon->core->screen_ptr;
	int x;
	char *tmp;

	if (icon->force_paint) {
		icon->force_paint = 0;
		wIconUpdate(icon);
		return;
	}

	XClearWindow(dpy, icon->core->window);

	/* draw the icon title */
	if (icon->show_title && icon->icon_name != NULL) {
		int l;
		int w;

		tmp = ShrinkString(scr->icon_title_font, icon->icon_name, wPreferences.icon_size - 4);
		w = WMWidthOfString(scr->icon_title_font, tmp, l = strlen(tmp));

		if (w > icon->core->width - 4)
			x = (icon->core->width - 4) - w;
		else
			x = (icon->core->width - w) / 2;

		WMDrawString(scr->wmscreen, icon->core->window, scr->icon_title_color,
			     scr->icon_title_font, x, 1, tmp, l);
		wfree(tmp);
	}

	if (icon->selected)
		XDrawRectangle(dpy, icon->core->window, scr->icon_select_gc, 0, 0,
			       icon->core->width - 1, icon->core->height - 1);
}

/******************************************************************/

static void miniwindowExpose(WObjDescriptor * desc, XEvent * event)
{
	wIconPaint(desc->parent);
}

static void miniwindowDblClick(WObjDescriptor * desc, XEvent * event)
{
	WIcon *icon = desc->parent;

	assert(icon->owner != NULL);

	wDeiconifyWindow(icon->owner);
}

static void miniwindowMouseDown(WObjDescriptor * desc, XEvent * event)
{
	WIcon *icon = desc->parent;
	WWindow *wwin = icon->owner;
	XEvent ev;
	int x = wwin->icon_x, y = wwin->icon_y;
	int dx = event->xbutton.x, dy = event->xbutton.y;
	int grabbed = 0;
	int clickButton = event->xbutton.button;
	Bool hasMoved = False;

	if (WCHECK_STATE(WSTATE_MODAL))
		return;

	if (IsDoubleClick(icon->core->screen_ptr, event)) {
		miniwindowDblClick(desc, event);
		return;
	}

	if (event->xbutton.button == Button1) {
		if (event->xbutton.state & MOD_MASK)
			wLowerFrame(icon->core);
		else
			wRaiseFrame(icon->core);
		if (event->xbutton.state & ShiftMask) {
			wIconSelect(icon);
			wSelectWindow(icon->owner, !wwin->flags.selected);
		}
	} else if (event->xbutton.button == Button3) {
		WObjDescriptor *desc;

		OpenMiniwindowMenu(wwin, event->xbutton.x_root, event->xbutton.y_root);

		/* allow drag select of menu */
		desc = &wwin->screen_ptr->window_menu->menu->descriptor;
		event->xbutton.send_event = True;
		(*desc->handle_mousedown) (desc, event);

		return;
	}

	if (XGrabPointer(dpy, icon->core->window, False, ButtonMotionMask
			 | ButtonReleaseMask | ButtonPressMask, GrabModeAsync,
			 GrabModeAsync, None, None, CurrentTime) != GrabSuccess) {
	}
	while (1) {
		WMMaskEvent(dpy, PointerMotionMask | ButtonReleaseMask | ButtonPressMask
			    | ButtonMotionMask | ExposureMask, &ev);
		switch (ev.type) {
		case Expose:
			WMHandleEvent(&ev);
			break;

		case MotionNotify:
			hasMoved = True;
			if (!grabbed) {
				if (abs(dx - ev.xmotion.x) >= MOVE_THRESHOLD
				    || abs(dy - ev.xmotion.y) >= MOVE_THRESHOLD) {
					XChangeActivePointerGrab(dpy, ButtonMotionMask
								 | ButtonReleaseMask | ButtonPressMask,
								 wCursor[WCUR_MOVE], CurrentTime);
					grabbed = 1;
				} else {
					break;
				}
			}
			x = ev.xmotion.x_root - dx;
			y = ev.xmotion.y_root - dy;
			XMoveWindow(dpy, icon->core->window, x, y);
			break;

		case ButtonPress:
			break;

		case ButtonRelease:
			if (ev.xbutton.button != clickButton)
				break;

			if (wwin->icon_x != x || wwin->icon_y != y)
				wwin->flags.icon_moved = 1;

			XMoveWindow(dpy, icon->core->window, x, y);

			wwin->icon_x = x;
			wwin->icon_y = y;
			XUngrabPointer(dpy, CurrentTime);

			if (wPreferences.auto_arrange_icons)
				wArrangeIcons(wwin->screen_ptr, True);
			if (wPreferences.single_click && !hasMoved)
				miniwindowDblClick(desc, event);
			return;

		}
	}
}
