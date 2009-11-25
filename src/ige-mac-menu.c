/* GTK+ Integration for the Mac OS X Menubar.
 *
 * Copyright (C) 2007 Pioneer Research Center USA, Inc.
 * Copyright (C) 2007, 2008 Imendio AB
 *
 * For further information, see:
 * http://developer.imendio.com/projects/gtk-macosx/menubar
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version 2.1
 * of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkquartz.h>
#include <glib.h>

#include <ApplicationServices/ApplicationServices.h>
#ifdef USE_CARBON
#include <Carbon/Carbon.h>
#endif
#import <Cocoa/Cocoa.h>

#include "ige-mac-menu.h"
#include "ige-mac-private.h"

/* TODO
 *
 * - Adding a standard Window menu (Minimize etc)?
 * - Sync reordering items? Does that work now?
 * - Create on demand? (can this be done with gtk+? ie fill in menu
     items when the menu is opened)
 *
 * - Deleting a menu item that is not the last one in a menu doesn't work
 */

#define IGE_QUARTZ_MENU_CREATOR 'IGEC'
#define IGE_QUARTZ_ITEM_WIDGET  'IWID'

#define IGE_MAC_KEY_HANDLER     "ige-mac-key-handler"

#define DEBUG TRUE //FALSE
#define DEBUG_SET TRUE //FALSE
#define DEBUG_SYNC TRUE //FALSE
#define DEBUG_SIGNAL FALSE
#define DEBUG_OSX FALSE
#define DEBUG_COCOA TRUE //FALSE

static gboolean global_key_handler_enabled = TRUE;

#ifdef USE_CARBON
static MenuID   last_menu_id;

static void   sync_menu_shell (GtkMenuShell *menu_shell, MenuRef carbon_menu,
			       gboolean toplevel, gboolean debug);
#else
static void   sync_menu_shell (GtkMenuShell *menu_shell, NSMenu *cocoa_menu,
			       gboolean toplevel, gboolean debug);
#endif

/* A category that exposes the protected carbon event for an NSEvent. */
@interface NSEvent (GdkQuartzNSEvent)
- (void *)gdk_quartz_event_ref;
@end

@implementation NSEvent (GdkQuartzNSEvent)
- (void *)gdk_quartz_event_ref {
    return _eventRef;
}
@end

#ifdef USE_CARBON
static gboolean
menu_flash_off_cb (gpointer data) {
    /* Disable flash by flashing a non-existing menu id. */
    FlashMenuBar (last_menu_id + 1);
    return FALSE;
}
#endif //FIXME: No Cocoa Implementation

/*
 * utility functions
 */

static GtkWidget *
find_menu_label (GtkWidget *widget) {
    GtkWidget *label = NULL;
    if (GTK_IS_LABEL (widget))
	return widget;
    if (GTK_IS_CONTAINER (widget)) {
	GList *children;
	GList *l;
	children = gtk_container_get_children (GTK_CONTAINER (widget));
	for (l = children; l; l = l->next) {
	    label = find_menu_label (l->data);
	    if (label)
		break;
	}
	g_list_free (children);
    }
    return label;
}

static const gchar *
get_menu_label_text (GtkWidget  *menu_item, GtkWidget **label) {
    GtkWidget *my_label;
    my_label = find_menu_label (menu_item);
    if (label)
	*label = my_label;
    if (my_label)
	return gtk_label_get_text (GTK_LABEL (my_label));
    return NULL;
}

static gboolean
accel_find_func (GtkAccelKey *key, GClosure *closure, gpointer data) {
    return (GClosure *) data == closure;
}


#ifdef USE_CARBON
typedef MenuRef OSXMenuRef;
#else //USE_COCOA
typedef NSMenu* OSXMenuRef;   
#endif
typedef struct {
    OSXMenuRef menu;
    guint   toplevel : 1;
} OSXMenu;

/*
 * OSXMenu functions
 */


static GQuark osx_menu_quark = 0;
static OSXMenu *
osx_menu_new (void) {
    return g_slice_new0 (OSXMenu);
}

static void
osx_menu_free (OSXMenu *menu) {
#ifdef USE_CARBON
    DisposeMenu(menu->menu);
#else
    if (menu->menu)
      [menu->menu release];
#endif
    g_slice_free (OSXMenu, menu);
}

static OSXMenu *
osx_menu_get (GtkWidget *widget) {
    return g_object_get_qdata (G_OBJECT (widget), osx_menu_quark);
}

static void
osx_menu_connect (GtkWidget *menu, OSXMenuRef menuRef, gboolean toplevel) {
    OSXMenu *osx_menu = osx_menu_get (menu);
    if (!osx_menu) {
	osx_menu = osx_menu_new ();
	g_object_set_qdata_full (G_OBJECT (menu), osx_menu_quark, 
				 osx_menu,
				 (GDestroyNotify) osx_menu_free);
    }
    osx_menu->menu     = menuRef;
    osx_menu->toplevel = toplevel;
}


/*
 * OSXMenuItem functions
 */
#ifdef USE_COCOA
typedef int MenuItemIndex;
#endif

typedef struct {
#ifdef USE_CARBON
    OSXMenuRef     menu;
    MenuItemIndex  index;
#else //USE_COCOA
    NSMenuItem    *menuitem;
#endif
    OSXMenuRef     submenu;
    GClosure      *accel_closure;
} OSXMenuItem;

static GQuark osx_menu_item_quark = 0;

static OSXMenuItem *
osx_menu_item_new (void) {
    return g_slice_new0 (OSXMenuItem);
}

static void
osx_menu_item_free (OSXMenuItem *menu_item) {
#ifdef USE_CARBON
    DeleteMenuItem(menu_item->menu, menu_item->index);  //Clean up the Carbon Menu
#else //USE_COCOA
    if (menu_item->menuitem)
        [[menu_item->menuitem menu] removeItem: menu_item->menuitem];
#endif
    if (menu_item->accel_closure)
	g_closure_unref (menu_item->accel_closure);
    g_slice_free (OSXMenuItem, menu_item);
}
#ifdef USE_CARBON
static const gchar *
carbon_menu_error_string(OSStatus err) {
    switch (err) {
    case 0:
	return "No Error";
    case -50:
	return "User Parameter List";
    case -5603:
	return "Menu Property Reserved Creator Type";
    case -5604:
	return "Menu Property Not Found";
    case -5620:
	return "Menu Not Found";
    case -5621:
	return "Menu uses system definition";
    case -5622:
	return "Menu Item Not Found";
    case -5623:
	return "Menu Reference Invalid";
    case -9860:
	return "Event Already Posted";
    case -9861:
	return "Event Target Busy";
    case -9862:
	return "Invalid Event Class";
    case -9864:
	return "Incorrect Event Class";
    case -9866:
	return "Event Handler Already Installed";
    case -9868:
	return "Internal Event Error";
    case -9869:
	return "Incorrect Event Kind";
    case -9870:
	return "Event Parameter Not Found";
    case -9874:
	return "Event Not Handled";
    case -9875:
	return "Event Loop Timeout";
    case -9876:
	return "Event Loop Quit";
    case -9877:
	return  "Event Not In Queue";
    case -9878:
	return "Hot Key Exists";
    case -9879:
	return "Invalid Hot Key";
    default:
	return "Invalid Error Code";
    }
    return "System Error: Unreachable";
}

#define carbon_menu_warn(err, msg) \
    if (err && DEBUG_OSX) \
	g_printerr("%s: %s %s\n", G_STRFUNC, msg, carbon_menu_error_string(err));

#define carbon_menu_warn_label(err, label, msg) \
    if (err && DEBUG_OSX) \
	g_printerr("%s: %s %s %s\n", G_STRFUNC, label, msg, carbon_menu_error_string(err));

#define carbon_menu_err_return(err, msg) \
    if (err) { \
    	if (DEBUG_OSX) \
	    g_printerr("%s: %s %s\n", G_STRFUNC, msg, carbon_menu_error_string(err)); \
	return;\
    }

#define carbon_menu_err_return_val(err, msg, val) \
    if (err) { \
    	if (DEBUG_OSX) \
	    g_printerr("%s: %s %s\n", G_STRFUNC, msg, carbon_menu_error_string(err)); \
	return val;\
    }

#define carbon_menu_err_return_label(err, label, msg)	\
    if (err) { \
    	if (DEBUG_OSX) \
	    g_printerr("%s: %s %s %s\n", G_STRFUNC, label, msg, carbon_menu_error_string(err)); \
	return;\
    }

#define carbon_menu_err_return_label_val(err, label, msg, val)	\
    if (err) { \
    	if (DEBUG_OSX) \
	    g_printerr("%s: %s %s %s\n", G_STRFUNC, label, msg, carbon_menu_error_string(err)); \
	return val;\
    }
#endif //USE_CARBON
/* FIXME: Implement Cocoa exception handlers */

static OSXMenuItem *
osx_menu_item_get (GtkWidget *widget) {
    return g_object_get_qdata (G_OBJECT (widget), osx_menu_item_quark);
}

static OSXMenuItem *
osx_menu_item_get_checked (GtkWidget *widget) {
    OSXMenuItem * osx_item = osx_menu_item_get(widget);
    GtkWidget *checkWidget = NULL;
#ifdef USE_CARBON
    OSStatus  err;
#endif
    const gchar *label = get_menu_label_text(GTK_WIDGET(widget), NULL);
    const gchar *name = gtk_widget_get_name(widget);

    if (!osx_item)
	return NULL;
#ifdef USE_CARBON
    /* Get any GtkWidget associated with the item. */
    err = GetMenuItemProperty (osx_item->menu, osx_item->index,
			       IGE_QUARTZ_MENU_CREATOR,
			       IGE_QUARTZ_ITEM_WIDGET,
			       sizeof (checkWidget), 0, &checkWidget);
    if (err) {
	if (DEBUG_OSX)
	    g_printerr("%s: Widget %s %s Cross-check error %s\n", G_STRFUNC,
		       name, label, carbon_menu_error_string(err));
	return NULL;
    }
#else //USE_COCOA
//FIXME: Implement cocoa widget storage & retrieval
#endif 
/* This could check the checkWidget, but that could turn into a
 * recursion nightmare, so worry about it when it we run
 * osx_menu_item_get on it.
 */
    if (widget != checkWidget) {
	const gchar *clabel = get_menu_label_text(GTK_WIDGET(checkWidget), 
						  NULL);
	const gchar *cname = gtk_widget_get_name(checkWidget);
	if (DEBUG_OSX)
	    g_printerr("%s: Widget mismatch, expected %s %s got %s %s\n", 
		       G_STRFUNC, name, label, cname, clabel);
	return NULL;
    }
    return osx_item;
}

static void
osx_menu_item_update_state (OSXMenuItem *osx_item, GtkWidget *widget) {
    gboolean sensitive;
    gboolean visible;
#ifdef USE_CARBON
    UInt32   set_attrs = 0;
    UInt32   clear_attrs = 0;
    OSStatus err;
#endif
    g_object_get (widget, "sensitive", &sensitive, "visible",   &visible, NULL);
#ifdef USE_CARBON
    if (!sensitive)
	set_attrs |= kMenuItemAttrDisabled;
    else
	clear_attrs |= kMenuItemAttrDisabled;
    if (!visible)
	set_attrs |= kMenuItemAttrHidden;
    else
	clear_attrs |= kMenuItemAttrHidden;
    err = ChangeMenuItemAttributes (osx_item->menu, osx_item->index,
				    set_attrs, clear_attrs);
    carbon_menu_warn(err, "Failed to update state");
#else //USE_COCOA
    if (!sensitive)
	[osx_item->menuitem setEnabled:NO];
    else
	[osx_item->menuitem setEnabled:YES];
    if (!visible)
	[osx_item->menuitem setHidden:YES];
    else
	[osx_item->menuitem setHidden:NO];
#endif
}

static void
osx_menu_item_update_active (OSXMenuItem *osx_item, 
				GtkWidget *widget) {
    gboolean active;
    g_object_get (widget, "active", &active, NULL);
#ifdef USE_CARBON
    CheckMenuItem (osx_item->menu, osx_item->index, active);
#else //USE_COCOA
    if (!active)
	[osx_item->menuitem setState:NSOffState];
    else
	[osx_item->menuitem setState:NSOnState];
#endif
}

static void
osx_menu_item_update_submenu (OSXMenuItem *osx_item, 
				 GtkWidget *widget, bool debug) {
    GtkWidget *submenu;
    const gchar *label_text;
#ifdef USE_CARBON
    CFStringRef  cfstr = NULL;
    OSStatus err;
#else //USE_COCOA
    NSString *nsstr = nil;
#endif
    label_text = get_menu_label_text (widget, NULL);
    submenu = gtk_menu_item_get_submenu (GTK_MENU_ITEM (widget));
    if (!submenu) {
#ifdef USE_CARBON
	err = SetMenuItemHierarchicalMenu (osx_item->menu, 
					   osx_item->index, NULL);
	carbon_menu_warn_label(err, label_text, "Failed to clear submenu");
	osx_item->submenu = NULL;
#else //USE_COCOA
	NS_DURING
		[osx_item->menuitem setSubmenu: nil];
	NS_HANDLER
	    //FIXME: Implement handler
	NS_ENDHANDLER
#endif
	return;
    }
#ifdef USE_CARBON
    err = CreateNewMenu (++last_menu_id, 0, &osx_item->submenu);
    carbon_menu_err_return_label(err, label_text, "Failed to create new menu");
    if (label_text)
	cfstr = CFStringCreateWithCString (NULL, label_text,
					   kCFStringEncodingUTF8);

    err = SetMenuTitleWithCFString (osx_item->submenu, cfstr);
    if (cfstr)
	CFRelease (cfstr);
    carbon_menu_err_return_label(err, label_text, "Failed to set menu title");
    err = SetMenuItemHierarchicalMenu (osx_item->menu, osx_item->index,
				       osx_item->submenu);
    carbon_menu_err_return_label(err, label_text, "Failed to set menu");
#else //USE_COCOA
    NS_DURING
    if (label_text)
	nsstr = [NSString stringWithUTF8String: label_text];
    osx_item->submenu = [[NSMenu alloc] initWithTitle: nsstr ? nsstr : @""];
    NS_HANDLER
	    //FIXME: Implement handler
    NS_ENDHANDLER
    if (nsstr)
	[nsstr release];
    [osx_item->menuitem setSubmenu: osx_item->submenu];
#endif
    sync_menu_shell (GTK_MENU_SHELL (submenu), osx_item->submenu, 
		     FALSE, debug);
}

static void
osx_menu_item_update_label (OSXMenuItem *osx_item, GtkWidget *widget) {
    const gchar *label_text;
#ifdef USE_CARBON
    CFStringRef  cfstr = NULL;
    OSStatus err;
#else //USE_COCOA
   NSString  *nsstr = nil;
#endif
    label_text = get_menu_label_text (widget, NULL);
#ifdef USE_CARBON
    if (label_text)
	cfstr = CFStringCreateWithCString (NULL, label_text, 
					   kCFStringEncodingUTF8);
    err = SetMenuItemTextWithCFString (osx_item->menu, osx_item->index, 
				       cfstr);
    carbon_menu_warn(err, "Failed to set menu text");
    if (cfstr)
	CFRelease (cfstr);
#else //USE_COCOA
    NS_DURING

    if (label_text)
	nsstr = [NSString stringWithUTF8String: label_text];
    [osx_item->menuitem setTitle: nsstr ? nsstr : @""];
    if (nsstr)
	[nsstr release];
    NS_HANDLER
	    //FIXME: Implement handler
    NS_ENDHANDLER
#endif
}

static void
osx_menu_item_update_accelerator (OSXMenuItem *osx_item,
				     GtkWidget *widget) {
    GtkAccelKey *key;
    GtkWidget *label = NULL;
    GdkDisplay *display = NULL;
    GdkKeymap *keymap = NULL;
    GdkKeymapKey *keys = NULL;
    gint n_keys = 0;
#ifdef USE_CARBON
    UInt8 modifiers = 0;
    OSStatus err;
    const gchar *label_txt = get_menu_label_text (widget, &label);
#else //USE_COCOA
    char keystr[2];
    unsigned int modifiers = 0;
#endif
    if (!(GTK_IS_ACCEL_LABEL (label) 
	  && GTK_ACCEL_LABEL (label)->accel_closure)) {
// Clear the menu shortcut
#ifdef USE_CARBON
	err = SetMenuItemModifiers (osx_item->menu, osx_item->index,
				    kMenuNoModifiers | kMenuNoCommandModifier);
	carbon_menu_warn_label(err, label_txt, "Failed to set modifiers");
	err = ChangeMenuItemAttributes (osx_item->menu, osx_item->index,
					0, kMenuItemAttrUseVirtualKey);
	carbon_menu_warn_label(err, label_txt, "Failed to change attributes");
	err = SetMenuItemCommandKey (osx_item->menu, osx_item->index,
				     false, 0);
	carbon_menu_warn_label(err, label_txt, "Failed to clear command key");
#else //USE_COCOA
    NS_DURING
        [osx_item->menuitem setKeyEquivalent: @""];
        [osx_item->menuitem setKeyEquivalentModifierMask: 0];
	NS_HANDLER
	    //FIXME: Implement handler
	NS_ENDHANDLER
#endif
	return;
    }
    key = gtk_accel_group_find (GTK_ACCEL_LABEL (label)->accel_group,
				    accel_find_func,
				    GTK_ACCEL_LABEL (label)->accel_closure);
    if (!(key && key->accel_key && key->accel_flags & GTK_ACCEL_VISIBLE))
	return;
    display = gtk_widget_get_display (widget);
    keymap  = gdk_keymap_get_for_display (display);

    if (!gdk_keymap_get_entries_for_keyval (keymap, key->accel_key,
					    &keys, &n_keys))
	return;
#ifdef USE_CARBON
    err = SetMenuItemCommandKey (osx_item->menu, osx_item->index,
				 true, keys[0].keycode);
    carbon_menu_warn_label(err, label_txt, "Set Command Key Failed");
#else //USE_COCOA
    NS_DURING
    keystr[0] = keys[0].keycode; keystr[1] = '\0';
    [osx_item->menuitem setKeyEquivalent: [NSString stringWithUTF8String: keystr]];
    NS_HANDLER
	    //FIXME: Implement handler
    NS_ENDHANDLER
    modifiers = NSCommandKeyMask;
#endif
    g_free (keys);
#ifdef USE_CARBON
    if (key->accel_mods) {
	if (key->accel_mods & GDK_SHIFT_MASK)
	    modifiers |= kMenuShiftModifier;
	if (key->accel_mods & GDK_MOD1_MASK)
	    modifiers |= kMenuOptionModifier;
    }
    if (!(key->accel_mods & GDK_CONTROL_MASK)) {
	modifiers |= kMenuNoCommandModifier;
    }
    err = SetMenuItemModifiers (osx_item->menu, osx_item->index,
				modifiers);
    carbon_menu_warn_label(err, label_txt, "Set Item Modifiers Failed");
#else //USE_COCOA
    NS_DURING
    if (key->accel_mods) {
	if (key->accel_mods & GDK_SHIFT_MASK)
	    modifiers |= NSShiftKeyMask;
	if (key->accel_mods & GDK_MOD1_MASK)
	    modifiers |= NSAlternateKeyMask;
    }
    if (!(key->accel_mods & GDK_CONTROL_MASK)) {
	modifiers &= ~NSCommandKeyMask; // NSControlKeyMask ?
    }
    [osx_item->menuitem setKeyEquivalentModifierMask: modifiers];
    NS_HANDLER
	    //FIXME: Implement handler
    NS_ENDHANDLER
#endif
    return;
}

static void
osx_menu_item_accel_changed (GtkAccelGroup *accel_group, guint keyval,
				GdkModifierType  modifier,
				GClosure *accel_closure, GtkWidget *widget) {
    OSXMenuItem *osx_item = osx_menu_item_get (widget);
    GtkWidget      *label;

    const gchar *label_text = get_menu_label_text (widget, &label);
    if (!osx_item) {
	if (DEBUG_OSX)
	    g_printerr("%s: Bad carbon item for %s\n", G_STRFUNC, label_text);
	return;
    }
    if (GTK_IS_ACCEL_LABEL (label) &&
	GTK_ACCEL_LABEL (label)->accel_closure == accel_closure)
	osx_menu_item_update_accelerator (osx_item, widget);
}

static void
osx_menu_item_update_accel_closure (OSXMenuItem *osx_item,
				       GtkWidget *widget) {
    GtkAccelGroup *group;
    GtkWidget     *label;
    get_menu_label_text (widget, &label);
    if (osx_item->accel_closure) {
	group = gtk_accel_group_from_accel_closure (osx_item->accel_closure);
	g_signal_handlers_disconnect_by_func (group,
					      osx_menu_item_accel_changed,
					      widget);
	g_closure_unref (osx_item->accel_closure);
	osx_item->accel_closure = NULL;
    }
    if (GTK_IS_ACCEL_LABEL (label))
	osx_item->accel_closure = GTK_ACCEL_LABEL (label)->accel_closure;
    if (osx_item->accel_closure) {
	g_closure_ref (osx_item->accel_closure);
	group = gtk_accel_group_from_accel_closure (osx_item->accel_closure);
	g_signal_connect_object (group, "accel-changed",
				 G_CALLBACK (osx_menu_item_accel_changed),
				 widget, 0);
    }
    osx_menu_item_update_accelerator (osx_item, widget);
}

static void
osx_menu_item_notify (GObject *object, GParamSpec *pspec,
			 OSXMenuItem *osx_item) {
    if (!strcmp (pspec->name, "sensitive") ||
	!strcmp (pspec->name, "visible")) {
	osx_menu_item_update_state (osx_item, GTK_WIDGET (object));
    }
    else if (!strcmp (pspec->name, "active")) {
	osx_menu_item_update_active (osx_item, GTK_WIDGET (object));
    }
    else if (!strcmp (pspec->name, "submenu")) {
	osx_menu_item_update_submenu (osx_item, GTK_WIDGET (object), 
DEBUG_SIGNAL);
    }
    else if (DEBUG)
	g_printerr("%s: Invalid parameter specification %s\n", G_STRFUNC, 
		   pspec->name);
}

static void
osx_menu_item_notify_label (GObject *object, GParamSpec *pspec,
			       gpointer data) {
    OSXMenuItem *osx_item = 
	osx_menu_item_get_checked (GTK_WIDGET (object));
    const gchar *label_text = get_menu_label_text(GTK_WIDGET(object), NULL);

    if (!osx_item) {
	if (DEBUG_OSX)
	    g_printerr("%s: Bad carbon item for %s\n", G_STRFUNC, label_text);
	return;
    }
   if (!strcmp (pspec->name, "label")) {
	osx_menu_item_update_label (osx_item, GTK_WIDGET (object));
    }
    else if (!strcmp (pspec->name, "accel-closure")) {
	osx_menu_item_update_accel_closure (osx_item, 
					       GTK_WIDGET (object));
    }
}

static OSXMenuItem*
osx_menu_item_get_or_create(GtkWidget *menu_item, GtkWidget *label) {
    OSXMenuItem *osx_item = 
	osx_menu_item_get_checked (menu_item);

    if (!osx_item) {
	osx_item = osx_menu_item_new ();
	g_object_set_qdata_full (G_OBJECT (menu_item), osx_menu_item_quark,
				 osx_item,
				 (GDestroyNotify) osx_menu_item_free);
	g_signal_connect (menu_item, "notify",
			  G_CALLBACK (osx_menu_item_notify), osx_item);
	if (label)
	    g_signal_connect_swapped(label, "notify::label",
				     G_CALLBACK (osx_menu_item_notify_label),
				     menu_item);
    }
    return osx_item;
}

#ifdef USE_CARBON
static OSXMenuItem *
osx_menu_item_connect (GtkWidget *menu_item, GtkWidget *label,
			  OSXMenuRef menu, MenuItemIndex index) {
    OSXMenuItem *osx_item = osx_menu_item_get_or_create(menu_item, label);
    osx_item->menu  = menu;
    osx_item->index = index;
    return osx_item;
}
#else //USE_COCOA
static OSXMenuItem *
osx_menu_item_connect (GtkWidget *menu_item, GtkWidget *label,
		       NSMenuItem *menuitem) {
    OSXMenuItem *osx_item = osx_menu_item_get_or_create(menu_item, label);
    osx_item->menuitem = menuitem;
    return osx_item;
}
#endif

static OSXMenuItem *
osx_menu_item_create (GtkWidget *menu_item, OSXMenuRef osx_menu,
			 MenuItemIndex index, bool debug) {
    GtkWidget          *label      = NULL;
    const gchar        *label_text;
    OSXMenuItem *osx_item;
#ifdef USE_CARBON
    CFStringRef         cfstr      = NULL;
    MenuItemAttributes  attributes = 0;
    OSStatus err;
#else //USE_COCOA
    NSString*         nsstr      = nil;
    NSMenuItem *cocoa_menuitem = nil;
#endif
    label_text = get_menu_label_text (menu_item, &label);
    if (debug)
	g_printerr ("%s:   -> creating new %s\n", G_STRFUNC, label_text);
    if (label_text)
#ifdef USE_CARBON
	cfstr = CFStringCreateWithCString (NULL, label_text,
					   kCFStringEncodingUTF8);
    if (GTK_IS_SEPARATOR_MENU_ITEM (menu_item))
	attributes |= kMenuItemAttrSeparator;
    if (!GTK_WIDGET_IS_SENSITIVE (menu_item))
	attributes |= kMenuItemAttrDisabled;
    if (!GTK_WIDGET_VISIBLE (menu_item))
	attributes |= kMenuItemAttrHidden;
    err = InsertMenuItemTextWithCFString (osx_menu, cfstr, index - 1,
					  attributes, 0);
    carbon_menu_err_return_label_val(err, label_text, 
				     "Failed to insert menu item", NULL);
    err = SetMenuItemProperty (osx_menu, index,
			       IGE_QUARTZ_MENU_CREATOR,
			       IGE_QUARTZ_ITEM_WIDGET,
			       sizeof (menu_item), &menu_item);

    if (cfstr)
	CFRelease (cfstr);
    if (err) {
	carbon_menu_warn_label(err, label_text,
				   "Failed to set menu property");
  	DeleteMenuItem(osx_menu, index); //Clean up the extra menu item
	return NULL;
    }
#else //USE_COCOA
    NS_DURING
	    if (label_text)
		    nsstr = [NSString stringWithUTF8String: label_text];
    
    if (GTK_IS_SEPARATOR_MENU_ITEM (menu_item))
	    cocoa_menuitem = [NSMenuItem separatorItem];
    else
	    cocoa_menuitem = [[NSMenuItem alloc] initWithTitle: nsstr ? nsstr : @""
						 action:NULL keyEquivalent: @""];
    [osx_menu insertItem: cocoa_menuitem atIndex: index-1];
    if (!GTK_WIDGET_IS_SENSITIVE (menu_item))
	    [cocoa_menuitem setEnabled: NO];
    else
	    [cocoa_menuitem setEnabled: YES];
    //if (!GTK_WIDGET_VISIBLE (menu_item))
    //	attributes |= kMenuItemAttrHidden;
#if 0  //FIXME: No Cocoa Implementation
    err = SetMenuItemProperty (cocoa_menu, index,
			       IGE_QUARTZ_MENU_CREATOR,
			       IGE_QUARTZ_ITEM_WIDGET,
			       sizeof (menu_item), &menu_item);
#endif //0
    if (nsstr)
	    [nsstr release];

#if 0 //FIXME: No Cocoa Implementation
    if (err) {
	    cocoa_menu_warn_label(err, label_text,
				  "Failed to set menu property");
	    DeleteMenuItem(cocoa_menu, index); //Clean up the extra menu item
	    return NULL;
    }
#endif //0
    NS_HANDLER
	    //FIXME: Implement handler
    NS_ENDHANDLER
#endif //CARBON?COCOA
#ifdef USE_CARBON
    osx_item = osx_menu_item_connect (menu_item, label, osx_menu, index);
#else //USE_COCOA
    osx_item = osx_menu_item_connect (menu_item, label, cocoa_menuitem);
#endif
    if (!osx_item) { //Got a bad osx_item, bail out
#ifdef USE_CARBON
	DeleteMenuItem(osx_menu, index); //Clean up the extra menu item
#else //USE_COCOA
	[cocoa_menuitem release]; //Clean up the extra menu item
#endif
	return osx_item;
    }
    if (GTK_IS_CHECK_MENU_ITEM (menu_item))
	osx_menu_item_update_active (osx_item, menu_item);
    osx_menu_item_update_accel_closure (osx_item, menu_item);
    if (gtk_menu_item_get_submenu (GTK_MENU_ITEM (menu_item)))
	osx_menu_item_update_submenu (osx_item, menu_item, debug);
    return osx_item;
}


typedef struct {
    GtkWidget *widget;
} ActivateIdleData;
#ifdef USE_CARBON
static void
activate_destroy_cb (gpointer user_data) {
    ActivateIdleData *data = user_data;

    if (data->widget)
	g_object_remove_weak_pointer (G_OBJECT (data->widget), 
				      (gpointer) &data->widget);
    g_free (data);
}

static gboolean
activate_idle_cb (gpointer user_data) {
    ActivateIdleData *data = user_data;

    if (data->widget)
	gtk_menu_item_activate (GTK_MENU_ITEM (data->widget));
    return FALSE;
}
#endif //Not used in Cocoa
static GList *app_menu_groups = NULL;

#ifdef USE_COCOA
static GQuark cocoa_target_quark = 0;

@class CocoaTarget;
@interface CocoaTarget : NSObject {
    @private
	GtkMenu *gtk_menu;
	GData   *menuActions;
}

-(CocoaTarget *) init: (GtkMenu *)menu;
-(void) addAction: (NSMenuItem *)ns_item with:(GtkMenuItem *)gtk_item;
-(void) setAction: (NSMenuItem *)ns_item;
-(void) resync;
-(void) handle: (NSMenuItem *)ns_item;
@end

static void
cocoa_target_set_action(GQuark item_id, GtkMenuItem *gtk_item, 
			gpointer target) {
    GtkMenu *menu = GTK_MENU(gtk_widget_get_parent(GTK_WIDGET (gtk_item)));
    NSMenuItem* ns_item = g_object_get_qdata(G_OBJECT (menu), 
					     cocoa_target_quark);
    [(CocoaTarget *)target setAction:ns_item];
}

@implementation CocoaTarget
-(CocoaTarget *) init: (GtkMenu*)menu {
    [super init];
    gtk_menu = menu;
    g_datalist_init(&menuActions);
    return self;
}

-(void) addAction: (NSMenuItem *)ns_item with:(GtkMenuItem *)gtk_item {
    const gchar *ns_item_title = [[ns_item title] UTF8String];
    printf("%s %s\n", G_STRFUNC, ns_item_title);

    gtk_widget_reparent (GTK_WIDGET (gtk_item), GTK_WIDGET (gtk_menu));
    g_datalist_set_data(&menuActions, ns_item_title, gtk_item);
    [self setAction:ns_item];
}

-(void) setAction: (NSMenuItem *)ns_item {
    [ns_item setTarget:self];
    [ns_item setAction:@selector(handle)];
}

-(void) resync {
	g_datalist_foreach(&menuActions, 
			   (GDataForeachFunc)cocoa_target_set_action, self);
}
    
-(void) handle: (NSMenuItem *)ns_item {
    const gchar *ns_item_title = [[ns_item title] UTF8String];
    GtkWidget *gtk_item = g_datalist_get_data(&menuActions, ns_item_title);
    ActivateIdleData *idleData;
    idleData = g_new0 (ActivateIdleData, 1);
    idleData->widget = gtk_item;
    g_object_add_weak_pointer (G_OBJECT (gtk_item), 
			       (gpointer) &idleData->widget);
    g_idle_add_full (G_PRIORITY_HIGH, activate_idle_cb,
		     idleData, activate_destroy_cb);

}
@end

static void
cocoa_target_unref(CocoaTarget *target) {
    [target release];
}

static GtkWidget *current_focussed_window = NULL;

static gint 
app_menu_group_last_index(IgeMacMenuGroup *group) {
        GList   *list;
	gint index = 0;
    for (list = app_menu_groups; list; list = g_list_next (list)) {
	IgeMacMenuGroup *list_group = list->data;

	index += g_list_length (list_group->items);
	/*  adjust index for the separator between groups, but not
	 *  before the first group
	 */
	if (list_group->items && list->prev)
	    index++;
	if (group == list_group) 
	    break;
    }
    return index;
}

static CocoaTarget*
cocoa_target_create(GtkWidget *toplevel) {
    GtkWidget* menu = gtk_menu_new();
    CocoaTarget *target = [[CocoaTarget alloc] init:GTK_MENU (menu)];
 
    gtk_widget_hide(menu);

    if (cocoa_target_quark  == 0)
	cocoa_target_quark = g_quark_from_static_string("CocoaTarget");
    g_object_set_qdata_full (G_OBJECT (menu), cocoa_target_quark,
			     target,
			     (GDestroyNotify)cocoa_target_unref);
    g_object_set_qdata (G_OBJECT(toplevel), cocoa_target_quark, target);
    return target;
}


static gboolean
app_menu_take_item(GtkMenuItem *menu_item, gint position, const gchar *label) {

    NSMenu *menu = [[[[NSApplication sharedApplication] mainMenu] itemAtIndex:0] submenu];
    NSString *itemTitle;
    NSMenuItem *cocoaMenuItem = [menu itemWithTitle:itemTitle];
    //OOPS! menus don't have targets!
    CocoaTarget *target = g_object_get_qdata(G_OBJECT (current_focussed_window),
					     cocoa_target_quark);
    gboolean new_item = FALSE;

    g_return_val_if_fail (GTK_IS_MENU_ITEM (menu_item), FALSE);

    if (!label || strlen(label) == 0)
       label = get_menu_label_text (GTK_WIDGET (menu_item), NULL);
    printf("%s %s\n", G_STRFUNC, label);
    itemTitle = [NSString stringWithUTF8String:label];
    printf("%s %s\n", G_STRFUNC, [itemTitle UTF8String]);
    if (!cocoaMenuItem) {
	cocoaMenuItem = [menu insertItemWithTitle:itemTitle action:nil 
			 keyEquivalent: @"" atIndex:(NSInteger)position];
	if (cocoaMenuItem) {
		printf ("%s Created item with title %s from %s\n", G_STRFUNC, [[cocoaMenuItem title] UTF8String], [itemTitle UTF8String]);
		new_item = TRUE;
	}
	else {
		printf ("%s New Item Creation Failed\n", G_STRFUNC);
		return FALSE;
	}
    }
    [target addAction:cocoaMenuItem with:menu_item];
    return new_item;
}

static void
app_menu_insert_separator(gint position) {
    NSMenu *menu = [[[[NSApplication sharedApplication] mainMenu] itemAtIndex:0] submenu];
    [menu insertItem:[NSMenuItem separatorItem] atIndex:position];
}
#endif //USE_COCOA

/*
 * carbon event handler
 */
#ifdef USE_CARBON

static OSStatus
menu_event_handler_func (EventHandlerCallRef  event_handler_call_ref,
			 EventRef event_ref, void *data) {
    UInt32 event_class = GetEventClass (event_ref);
    UInt32 event_kind = GetEventKind (event_ref);
    HICommand command;
    OSStatus  err;
    GtkWidget *widget = NULL;
    ActivateIdleData *idleData;

    switch (event_class) {
    case kEventClassCommand:
	/* This is called when activating (is that the right GTK+ term?)
	 * a menu item.
	 */
	if (event_kind != kEventCommandProcess)
	    break;

#if DEBUG
	g_printerr ("Menu: kEventClassCommand/kEventCommandProcess\n");
#endif
	err = GetEventParameter (event_ref, kEventParamDirectObject,
				 typeHICommand, 0,
				 sizeof (command), 0, &command);
	if (err != noErr) {
	    carbon_menu_warn(err, "Get Event Returned Error");
	    break;
	}
	/* Get any GtkWidget associated with the item. */
	err = GetMenuItemProperty (command.menu.menuRef,
				   command.menu.menuItemIndex,
				   IGE_QUARTZ_MENU_CREATOR,
				   IGE_QUARTZ_ITEM_WIDGET,
				   sizeof (widget), 0, &widget);
	if (err != noErr) {
	    carbon_menu_warn(err, "Failed to retrieve the widget associated with the menu item");
	    break;
	}
	if (! GTK_IS_WIDGET (widget)) {
	    g_printerr("The item associated with the menu item isn't a widget\n");
	    break;
	}
	/* Activate from an idle handler so that the event is
	 * emitted from the main loop instead of in the middle of
	 * handling quartz events.
	 */
	idleData = g_new0 (ActivateIdleData, 1);
	idleData->widget= widget;
	g_object_add_weak_pointer (G_OBJECT (widget), 
				   (gpointer) &idleData->widget);
	g_idle_add_full (G_PRIORITY_HIGH, activate_idle_cb,
			 idleData, activate_destroy_cb);
	return noErr;
	break;
    case kEventClassMenu:
	if (event_kind == kEventMenuEndTracking)
	    g_idle_add (menu_flash_off_cb, NULL);
	break;
    default:
	break;
    }
    return CallNextEventHandler (event_handler_call_ref, event_ref);
}
#endif //FIXME: No Cocoa Implementation

static gboolean
nsevent_handle_menu_key (NSEvent *nsevent) {
#ifdef USE_CARBON
    EventRef      event_ref;
    OSXMenuRef       menu_ref;
    MenuItemIndex index;
    MenuCommand menu_command;
    HICommand   hi_command;
   OSStatus err;

    if ([nsevent type] != NSKeyDown)
	return FALSE;
    event_ref = [nsevent gdk_quartz_event_ref];
    if (!IsMenuKeyEvent (NULL, event_ref, kMenuEventQueryOnly, 
			&menu_ref, &index)) 
	return FALSE;
    err = GetMenuItemCommandID (menu_ref, index, &menu_command);
    carbon_menu_err_return_val(err, "Failed to get command id", FALSE);
    hi_command.commandID = menu_command;
    hi_command.menu.menuRef = menu_ref;
    hi_command.menu.menuItemIndex = index;
    err = CreateEvent (NULL, kEventClassCommand, kEventCommandProcess,
		       0, kEventAttributeUserEvent, &event_ref);
    carbon_menu_err_return_val(err, "Failed to create event", FALSE);
    err = SetEventParameter (event_ref, kEventParamDirectObject, typeHICommand,
			     sizeof (HICommand), &hi_command);
    if (err != noErr)
	ReleaseEvent(event_ref); //We're about to bail, don't want to leak
     carbon_menu_err_return_val(err, "Failed to set event parm", FALSE);
    FlashMenuBar (GetMenuID (menu_ref));
    g_timeout_add (30, menu_flash_off_cb, NULL);
    err = SendEventToEventTarget (event_ref, GetMenuEventTarget (menu_ref));
    ReleaseEvent (event_ref);
    carbon_menu_err_return_val(err, "Failed to send event", FALSE);
    return TRUE;
#else //FIXME: No Cocoa Implementation
    return FALSE;
#endif
}

gboolean
ige_mac_menu_handle_menu_event (GdkEventKey *event) {
    NSEvent *nsevent;

    /* FIXME: If the event here is unallocated, we crash. */
    nsevent = gdk_quartz_event_get_nsevent ((GdkEvent *) event);
    if (nsevent)
	return nsevent_handle_menu_key (nsevent);
    return FALSE;
}

#ifdef USE_CARBON
static GdkFilterReturn
global_event_filter_func (gpointer  windowing_event, GdkEvent *event,
                          gpointer  user_data) {
    NSEvent *nsevent = windowing_event;

    /* Handle menu events with no window, since they won't go through the
     * regular event processing.
     */
    if ([nsevent window] == nil) {
	if (nsevent_handle_menu_key (nsevent))
	    return GDK_FILTER_REMOVE;
    }
    else if (global_key_handler_enabled && [nsevent type] == NSKeyDown) {
	GList *toplevels, *l;
	GtkWindow *focus = NULL;

	toplevels = gtk_window_list_toplevels ();
	for (l = toplevels; l; l = l->next) {
	    if (gtk_window_has_toplevel_focus (l->data)) {
		focus = l->data;
		break;
            }
        }
	g_list_free (toplevels);

	/* FIXME: We could do something to skip menu events if there is a
	 * modal dialog...
	 */
	if (!focus 
	    || !g_object_get_data (G_OBJECT (focus), IGE_MAC_KEY_HANDLER)) {
	    if (nsevent_handle_menu_key (nsevent))
		return GDK_FILTER_REMOVE;
        }
    }
    return GDK_FILTER_CONTINUE;
}

static gboolean
key_press_event (GtkWidget   *widget, GdkEventKey *event, gpointer user_data) {
    GtkWindow *window = GTK_WINDOW (widget);
    GtkWidget *focus = gtk_window_get_focus (window);
    gboolean handled = FALSE;

    /* Text widgets get all key events first. */
    if (GTK_IS_EDITABLE (focus) || GTK_IS_TEXT_VIEW (focus))
	handled = gtk_window_propagate_key_event (window, event);

    if (!handled)
	handled = ige_mac_menu_handle_menu_event (event);

    /* Invoke control/alt accelerators. */
    if (!handled && event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))
	handled = gtk_window_activate_key (window, event);

    /* Invoke focus widget handlers. */
    if (!handled)
	handled = gtk_window_propagate_key_event (window, event);

    /* Invoke non-(control/alt) accelerators. */
    if (!handled && !(event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)))
	handled = gtk_window_activate_key (window, event);

    return handled;
}
#endif //Not used in Cocoa

static void
setup_menu_event_handler (void) {
#ifdef USE_CARBON
    static gboolean is_setup = FALSE;
    EventHandlerUPP menu_event_handler_upp;
    EventHandlerRef menu_event_handler_ref;
    OSStatus err;
    const EventTypeSpec menu_events[] = {
	{ kEventClassCommand, kEventCommandProcess },
	{ kEventClassMenu, kEventMenuEndTracking }
    };

    if (is_setup)
	return;
    gdk_window_add_filter (NULL, global_event_filter_func, NULL);
    menu_event_handler_upp = NewEventHandlerUPP (menu_event_handler_func);
    err = InstallEventHandler (GetApplicationEventTarget (), 
			       menu_event_handler_upp,
			       GetEventTypeCount (menu_events), menu_events, 0,
			       &menu_event_handler_ref);
    carbon_menu_err_return(err, "Failed to install event handler");
#if 0
    /* Note: If we want to supporting shutting down, remove the handler
     * with:
     */
    err = RemoveEventHandler(menu_event_handler_ref);
    carbon_menu_warn(err, "Failed to remove handler");
    err = DisposeEventHandlerUPP(menu_event_handler_upp);
    carbon_menu_warn(err, "Failed to elete menu handler UPP");
#endif //
    is_setup = TRUE;
#endif //FIXME: No Cocoa Implementation
}

static void
sync_menu_shell (GtkMenuShell *menu_shell, OSXMenuRef osx_menu,
		 gboolean toplevel, gboolean debug) {
    GList         *children;
    GList         *l;
    MenuItemIndex  osx_index = 1;
#ifdef USE_CARBON
    OSStatus err;
#endif
    if (debug)
	g_printerr ("%s: syncing shell %s (%p)\n", G_STRFUNC, 
		    get_menu_label_text(GTK_WIDGET(menu_shell), NULL),
		    menu_shell);
    osx_menu_connect (GTK_WIDGET (menu_shell), osx_menu, toplevel);
    children = gtk_container_get_children (GTK_CONTAINER (menu_shell));
    for (l = children; l; l = l->next) {
	GtkWidget      *menu_item = l->data;
	OSXMenuItem *osx_item;
#ifdef USE_CARBON
	MenuAttributes attrs;
#endif
	const gchar *label = get_menu_label_text (menu_item, NULL);

	if (GTK_IS_TEAROFF_MENU_ITEM (menu_item))
	    continue;
	if (toplevel && (g_object_get_data (G_OBJECT (menu_item),
					    "gtk-empty-menu-item") 
			 || GTK_IS_SEPARATOR_MENU_ITEM (menu_item)))
	    continue;
	osx_item = osx_menu_item_get (menu_item);
	if (debug) {
#ifdef USE_CARBON
	    g_printerr ("%s: osx_item %d for menu_item %d (%s, %s)\n",
			G_STRFUNC, osx_item ? osx_item->index : -1,
			osx_index, label,
			g_type_name (G_TYPE_FROM_INSTANCE (menu_item)));

#else //USE_COCOA
#if __LP64__ || NS_BUILD_32_LIKE_64
#define FMT "%s: osx_item %ld for menu_item %d (%s, %s)\n"
#else
#define FMT "%s: osx_item %d for menu_item %d (%s, %s)\n"
#endif //__LP64__ etc.
	    g_printerr (FMT, G_STRFUNC, 
			osx_item ? [[osx_item->menuitem menu] indexOfItem: osx_item->menuitem] : -1,
			osx_index, label,
			g_type_name (G_TYPE_FROM_INSTANCE (menu_item)));
#endif //CARBON||COCOA
	}
#ifdef USE_CARBON 
	if (osx_item && osx_item->index != osx_index) {
	    if (osx_item->index == osx_index - 1) {
		if (debug)
		    g_printerr("%s: %s incrementing index\n", G_STRFUNC, label);
		++osx_item->index;
	    } 
	    else if (osx_item->index == osx_index + 1) {
		if (debug)
		    g_printerr("%s: %s decrementing index\n", G_STRFUNC, label);
		--osx_item->index;
	    } 
	    else {
		if (debug)
		    g_printerr ("%s: %s -> not matching, deleting\n",
				G_STRFUNC, label);
#ifdef USE_CARBON
		DeleteMenuItem (osx_item->menu, osx_index);
#else //USE_COCOA
		[[osx_item->menuitem menu] removeItemAtIndex: osx_index];
#endif
		osx_item = NULL;
	    }
	}
#endif //FIXME: No Cocoa Implementation
	if (!osx_item)
	    osx_item = osx_menu_item_create(menu_item, osx_menu,
						  osx_index, debug);
	if (!osx_item) //Bad osx_item, give up
	    continue;
	if (!osx_item->submenu) {
	    osx_index++;
	    continue;
	}
/*The rest only applies to submenus, not to items which should have
 * been fixed up in osx_menu_item_create
 */
#ifdef USE_CARBON
	err = GetMenuAttributes( osx_item->submenu, &attrs);
	carbon_menu_warn(err, "Failed to get menu attributes");
	if (!GTK_WIDGET_VISIBLE (menu_item)) {
	    if ((attrs & kMenuAttrHidden) == 0) {
		if (debug)
		    g_printerr("Hiding menu %s\n", label);
		err = ChangeMenuAttributes (osx_item->submenu, 
					    kMenuAttrHidden, 0);
		carbon_menu_warn_label(err, label, "Failed to set visible");
	    }
	}
	else if ((attrs & kMenuAttrHidden) != 0) {
	    if (debug) 
		g_printerr("Revealing menu %s\n", label);
	    err = ChangeMenuAttributes (osx_item->submenu, 0, 
					    kMenuAttrHidden);
	    carbon_menu_warn_label(err, label, "Failed to set Hidden");
	}
#endif //FIXME: No Cocoa Implementation
	osx_index++;
    }
    g_list_free (children);
}

#ifdef USE_CARBON
static gulong emission_hook_id    = 0;
static gint   emission_hook_count = 0;

static gboolean
parent_set_emission_hook (GSignalInvocationHint *ihint, guint n_param_values,
			  const GValue *param_values, gpointer data) {
    GtkWidget *instance = g_value_get_object (param_values);
    OSXMenu *osx_menu;
    GtkWidget *previous_parent  = NULL;
    GtkWidget *menu_shell = NULL;

    if (!GTK_IS_MENU_ITEM (instance)) 
	return TRUE;
    previous_parent = g_value_get_object (param_values + 1);
    if (GTK_IS_MENU_SHELL (previous_parent)) {
	menu_shell = previous_parent;
    }
    else if (GTK_IS_MENU_SHELL (instance->parent)) {
	menu_shell = instance->parent;
    }
    if (!menu_shell) 
	return TRUE;
    osx_menu = osx_menu_get (menu_shell);

    if (!osx_menu) 
	return TRUE;
#if DEBUG
    g_printerr ("%s: item %s (%s) %s %s (%p)\n", G_STRFUNC,
		get_menu_label_text (instance, NULL),
		g_type_name (G_TYPE_FROM_INSTANCE (instance)),
		previous_parent ? "removed from" : "added to",
		get_menu_label_text(menu_shell, NULL),
		menu_shell);
#endif
    sync_menu_shell (GTK_MENU_SHELL (menu_shell), osx_menu->menu,
		     osx_menu->toplevel, DEBUG_SIGNAL);
    return TRUE;
}

static void
parent_set_emission_hook_remove (GtkWidget *widget, gpointer data) {
#ifdef USE_CARBON
    OSXMenu *osx_menu = osx_menu_get(widget);
    if (osx_menu) {
	MenuID id = GetMenuID(osx_menu->menu);
	ClearMenuBar();
	DeleteMenu(id);
    }
    emission_hook_count--;
    if (emission_hook_count > 0)
	return;
    g_signal_remove_emission_hook (
	g_signal_lookup("parent-set", GTK_TYPE_WIDGET), emission_hook_id);
    emission_hook_id = 0;
#endif //FIXME: No Cocoa Implementation
}

static gboolean
window_focus(GtkWindow *window, GdkEventFocus *event, OSXMenu *menu) {
#ifdef USE_CARBON
    OSStatus err = SetRootMenu(menu->menu);
    if (err) {
	carbon_menu_warn(err, "Failed to transfer menu");
    }
    else if (DEBUG){
	g_printerr("%s: Switched Menu\n", G_STRFUNC);
    }
    return FALSE;
#endif //FIXME: No Cocoa Implementation
}
#endif //Not used by Cocoa

#ifdef USE_COCOA
/*
 * Cocoa Support Funcions:
 */

static NSString *
_get_application_name(void)
{
    NSString *appName = nil;
    NSDictionary *dict;

    dict = (NSDictionary *)CFBundleGetInfoDictionary(CFBundleGetMainBundle());
    if (dict)
        appName = [dict objectForKey: @"CFBundleName"];
    
    if (![appName length])
        appName = [[NSProcessInfo processInfo] processName];

    return appName;
}

static void
_create_application_menus(void)
{
    NSMenu * mainMenu;
    NSString * applicationName;
    NSMenu * menu;
    NSMenuItem *item;

    mainMenu = [[[NSMenu alloc] initWithTitle:@"MainMenu"] autorelease];
    [NSApp setMainMenu:mainMenu];
    
    // The titles of the menu items are for identification purposes only and shouldn't be localized.
    // The strings in the menu bar come from the submenu titles,
    // except for the application menu, whose title is ignored at runtime.
    item = [mainMenu addItemWithTitle:@"Apple" action:NULL keyEquivalent:@""];
    menu = [[[NSMenu alloc] initWithTitle:@"Apple"] autorelease];
    [NSApp performSelector:@selector(setAppleMenu:) withObject:menu];
    [mainMenu setSubmenu:menu forItem:item];

    applicationName = _get_application_name();
    
    item = [menu addItemWithTitle:[NSString stringWithFormat:@"%@ %@", NSLocalizedString(@"About", nil), applicationName]
		 action:@selector(orderFrontStandardAboutPanel:)
		 keyEquivalent:@""];
    [item setTarget:NSApp];
    
    [menu addItem:[NSMenuItem separatorItem]];
    
    item = [menu addItemWithTitle:NSLocalizedString(@"Services", nil)
		 action:NULL
		 keyEquivalent:@""];
    NSMenu * servicesMenu = [[[NSMenu alloc] initWithTitle:@"Services"] 
				    autorelease];
    [menu setSubmenu:servicesMenu forItem:item];
    [NSApp setServicesMenu:servicesMenu];
    
    [menu addItem:[NSMenuItem separatorItem]];
    
    item = [menu addItemWithTitle:[NSString stringWithFormat:@"%@ %@", NSLocalizedString(@"Hide", nil), applicationName]
		 action:@selector(hide:)
		 keyEquivalent:@"h"];
    [item setTarget:NSApp];
    
    item = [menu addItemWithTitle:NSLocalizedString(@"Hide Others", nil)
		 action:@selector(hideOtherApplications:)
		 keyEquivalent:@"h"];
    [item setKeyEquivalentModifierMask:NSCommandKeyMask | NSAlternateKeyMask];
    [item setTarget:NSApp];
    
    item = [menu addItemWithTitle:NSLocalizedString(@"Show All", nil)
		 action:@selector(unhideAllApplications:)
		 keyEquivalent:@""];
    [item setTarget:NSApp];
    
    [menu addItem:[NSMenuItem separatorItem]];
    
    item = [menu addItemWithTitle:[NSString stringWithFormat:@"%@ %@", NSLocalizedString(@"Quit", nil), applicationName]
    					   action:@selector(terminate:)
    				keyEquivalent:@"q"];
    [item setTarget:NSApp];
}
#endif

/*
 * public functions
 */

void
ige_mac_menu_set_menu_bar (GtkMenuShell *menu_shell) {
#ifdef USE_CARBON
    OSXMenu    *current_menu;
    OSXMenuRef 	osx_menubar;
    OSStatus    err;
    GtkWidget  *parent = gtk_widget_get_toplevel(GTK_WIDGET(menu_shell));
#else //USE_COCOA
    OSXMenuRef 	osx_menubar = nil;
#endif
    g_return_if_fail (GTK_IS_MENU_SHELL (menu_shell));
    if (osx_menu_quark == 0)
	osx_menu_quark = g_quark_from_static_string ("OSXMenu");
    if (osx_menu_item_quark == 0)
	osx_menu_item_quark = g_quark_from_static_string ("OSXMenuItem");
    current_menu = osx_menu_get (GTK_WIDGET (menu_shell));
#ifdef USE_CARBON
    if (current_menu) {
	err = SetRootMenu (current_menu->menu);
	carbon_menu_warn(err, "Failed to set root menu");
	return;
    }
    err = CreateNewMenu (++last_menu_id /*id*/, 0 /*options*/, &osx_menubar);
    carbon_menu_err_return(err, "Failed to create menu");
    err = SetRootMenu (osx_menubar);
    carbon_menu_err_return(err, "Failed to set root menu");
#else //USE_COCOA
    NS_DURING
    [NSApplication sharedApplication];
    if ([NSApp mainMenu] == nil)
	_create_application_menus();
    [NSApp finishLaunching];

    osx_menubar = [NSApp mainMenu];
    NS_HANDLER
	    //FIXME: Implement handler
    NS_ENDHANDLER
#endif
    setup_menu_event_handler ();
#ifdef USE_CARBON
    if (emission_hook_id == 0) {
	emission_hook_id =
	    g_signal_add_emission_hook(
		g_signal_lookup("parent-set", GTK_TYPE_WIDGET), 0,
		parent_set_emission_hook, NULL, NULL);
    }
    emission_hook_count++;
    g_signal_connect (menu_shell, "destroy",
		      G_CALLBACK (parent_set_emission_hook_remove), NULL);

#endif //FIXME: No Cocoa Implementation
#if DEBUG_SET
    g_printerr ("%s: syncing menubar\n", G_STRFUNC);
#endif
    sync_menu_shell (menu_shell, osx_menubar, TRUE, DEBUG_SET);
#ifdef USE_CARBON
    if (parent)
	g_signal_connect (parent, "focus-in-event",
			  G_CALLBACK(window_focus), 
			  osx_menu_get(GTK_WIDGET(menu_shell)));
#endif //FIXME: No Cocoa Implementation
}

void
ige_mac_menu_set_quit_menu_item (GtkMenuItem *menu_item) {
#ifdef USE_CARBON
    OSXMenuRef       appmenu;
    MenuItemIndex index;
    OSStatus err;

    g_return_if_fail (GTK_IS_MENU_ITEM (menu_item));
    setup_menu_event_handler ();
    err = GetIndMenuItemWithCommandID (NULL, kHICommandQuit, 1,
					&appmenu, &index);
    carbon_menu_err_return(err, "Failed to obtain Quit Menu");
    err = SetMenuItemCommandID (appmenu, index, 0);
    carbon_menu_err_return(err, 
			   "Failed to set Quit menu command id");
    err = SetMenuItemProperty (appmenu, index, IGE_QUARTZ_MENU_CREATOR,
			       IGE_QUARTZ_ITEM_WIDGET, sizeof (menu_item), 
			       &menu_item);
    carbon_menu_err_return(err, 
			   "Failed to associate Quit menu item");
    gtk_widget_hide (GTK_WIDGET (menu_item));
#elif defined USE_COCOA
    g_return_if_fail (GTK_IS_MENU_ITEM (menu_item));
    app_menu_take_item(menu_item, 0, "Quit");
#endif
    return;
}

void
ige_mac_menu_connect_window_key_handler (GtkWindow *window) {
    if (g_object_get_data (G_OBJECT (window), IGE_MAC_KEY_HANDLER)) {
	g_warning ("Window %p is already connected", window);
	return;
    }
#ifdef USE_CARBON
    g_signal_connect (window, "key-press-event", 
		      G_CALLBACK (key_press_event), NULL);
    g_object_set_data (G_OBJECT (window), IGE_MAC_KEY_HANDLER, 
		       GINT_TO_POINTER (1));
#endif //FIXME: No Cocoa Implementation
}

/* Most applications will want to have this enabled (which is the
 * defalt). For apps that need to deal with the events themselves, the
 * global handling can be disabled.
 */
void
ige_mac_menu_set_global_key_handler_enabled (gboolean enabled) {
    global_key_handler_enabled = enabled;
}

/* For internal use only. Returns TRUE if there is a GtkMenuItem assigned to
 * the Quit menu item.
 */
gboolean
_ige_mac_menu_is_quit_menu_item_handled (void) {
#ifdef USE_CARBON
    OSXMenuRef       appmenu;
    MenuItemIndex index;
    OSStatus err = GetIndMenuItemWithCommandID (NULL, kHICommandQuit, 1,
						&appmenu, &index);
    carbon_menu_warn(err, "failed with");
    return (err == noErr);
#elif defined USE_COCOA
       NSMenu *menu = [[[[NSApplication sharedApplication] mainMenu] itemAtIndex:0] submenu];
    NSString *itemTitle = [NSString stringWithUTF8String:"Quit"];
    NSMenuItem *cocoaMenuItem = [menu itemWithTitle:itemTitle];
    return ([cocoaMenuItem action] != nil);

#endif
}


void
ige_mac_menu_add_app_menu_item (IgeMacMenuGroup *group, GtkMenuItem *menu_item,
				const gchar *label) {
#ifdef USE_CARBON
    OSXMenuRef  appmenu;
    GList   *list;
    gint     index = 0;
    CFStringRef cfstr;
    OSStatus err;

    g_return_if_fail (group != NULL);
    g_return_if_fail (GTK_IS_MENU_ITEM (menu_item));
    setup_menu_event_handler ();
    err = GetIndMenuItemWithCommandID (NULL, kHICommandHide, 1,
				       &appmenu, NULL);
    carbon_menu_err_return(err, "retrieving app menu failed");
    for (list = app_menu_groups; list; list = g_list_next (list)) {
	IgeMacMenuGroup *list_group = list->data;

	index += g_list_length (list_group->items);
	/*  adjust index for the separator between groups, but not
	 *  before the first group
	 */
	if (list_group->items && list->prev)
	    index++;
	if (group != list_group) 
	    continue;

	/*  add a separator before adding the first item, but not
	 *  for the first group
	 */
	if (!group->items && list->prev) {
	    err = InsertMenuItemTextWithCFString (appmenu, NULL, index,
						  kMenuItemAttrSeparator, 0);
	    carbon_menu_err_return(err, "Failed to add separator");
	    index++;
	}
	if (!label)
	    label = get_menu_label_text (GTK_WIDGET (menu_item), NULL);
	cfstr = CFStringCreateWithCString (NULL, label,
					   kCFStringEncodingUTF8);
	err = InsertMenuItemTextWithCFString (appmenu, cfstr, index, 0, 0);
	carbon_menu_err_return(err, "Failed to add menu item");
	err = SetMenuItemProperty (appmenu, index + 1,
				   IGE_QUARTZ_MENU_CREATOR,
				   IGE_QUARTZ_ITEM_WIDGET,
				   sizeof (menu_item), &menu_item);
	CFRelease (cfstr);
	carbon_menu_err_return(err, "Failed to associate Gtk Widget");
	gtk_widget_hide (GTK_WIDGET (menu_item));
	group->items = g_list_append (group->items, menu_item);
	return;

    }
    if (!list)
	g_warning ("%s: app menu group %p does not exist", G_STRFUNC, group);
#elif defined USE_COC
    gint     index = app_menu_group_last_index(group);

    g_return_if_fail (group != NULL);
    g_return_if_fail (GTK_IS_MENU_ITEM (menu_item));
    if (app_menu_take_item(menu_item, index++, label))
	group->items = g_list_append (group->items, menu_item);
#endif
}

void
ige_mac_menu_sync(GtkMenuShell *menu_shell) {
    OSXMenu *osx_menu = osx_menu_get (GTK_WIDGET(menu_shell));
#if DEBUG_SYNC
    g_printerr ("%s: syncing menubar\n", G_STRFUNC);
#endif
    sync_menu_shell (menu_shell, osx_menu->menu,
		     osx_menu->toplevel, DEBUG_SYNC);
}
