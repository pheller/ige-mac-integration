/* GTK+ Integration for the Mac OS X Dock.
 *
 * Copyright (C) 2007, 2008 Imendio AB
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

/* FIXME: Add example like this to docs for the open documents stuff:

    <key>CFBundleDocumentTypes</key>
    <array>
      <dict>
        <key>CFBundleTypeExtensions</key>
        <array>
          <string>txt</string>
        </array>
      </dict>
    </array>

*/

#include <config.h>
#include <ApplicationServices/ApplicationServices.h>
#ifdef USE_CARBON
#include <Carbon/Carbon.h>
#endif
#ifdef USE_COCOA
#import <Cocoa/Cocoa.h>
#endif
#include <sys/param.h>
#include <gtk/gtk.h>

#include "ige-mac-dock.h"
#include "ige-mac-bundle.h"
#include "ige-mac-image-utils.h"
#include "ige-mac-private.h"

enum {
  CLICKED,
  QUIT_ACTIVATE,
  OPEN_DOCUMENTS,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef struct IgeMacDockPriv IgeMacDockPriv;

struct IgeMacDockPriv {
  glong id;
};

static void  mac_dock_finalize                  (GObject          *object);
#ifdef USE_CARBON
static OSErr mac_dock_handle_quit               (const AppleEvent *inAppleEvent,
                                                 AppleEvent       *outAppleEvent,
                                                 long              inHandlerRefcon);
static OSErr mac_dock_handle_open_documents     (const AppleEvent *inAppleEvent,
                                                 AppleEvent       *outAppleEvent,
                                                 long              inHandlerRefcon);
static OSErr mac_dock_handle_open_application   (const AppleEvent *inAppleEvent,
                                                 AppleEvent       *outAppleEvent,
                                                 long              inHandlerRefcon);
static OSErr mac_dock_handle_reopen_application (const AppleEvent *inAppleEvent,
                                                 AppleEvent       *outAppleEvent,
                                                 long              inHandlerRefcon);
#endif

G_DEFINE_TYPE (IgeMacDock, ige_mac_dock, G_TYPE_OBJECT)

#define GET_PRIV(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), IGE_TYPE_MAC_DOCK, IgeMacDockPriv))

static GList      *handlers;
static IgeMacDock *global_dock;

static void
ige_mac_dock_class_init (IgeMacDockClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = mac_dock_finalize;

  signals[CLICKED] =
    g_signal_new ("clicked",
                  IGE_TYPE_MAC_DOCK,
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  /* FIXME: Need marshaller. */
  signals[OPEN_DOCUMENTS] =
    g_signal_new ("open-documents",
                  IGE_TYPE_MAC_DOCK,
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[QUIT_ACTIVATE] =
    g_signal_new ("quit-activate",
                  IGE_TYPE_MAC_DOCK,
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  g_type_class_add_private (object_class, sizeof (IgeMacDockPriv));

#ifdef USE_CARBON
  /* FIXME: Just testing with triggering Carbon to take control over
   * the dock menu events instead of Cocoa (which happens when the
   * sharedApplication is created) to get custom dock menu working
   * with carbon menu code. However, doing this makes the dock icon
   * not get a "running triangle".
   */
#if 0
  EventTypeSpec kFakeEventList[] = { { INT_MAX, INT_MAX } };
  EventRef event;
  
  ReceiveNextEvent (GetEventTypeCount (kFakeEventList),
                    kFakeEventList,
                    kEventDurationNoWait, false, 
                    &event);
#endif
#endif /* USE_CARBON */
}

static void
ige_mac_dock_init (IgeMacDock *dock)
{
  IgeMacDockPriv *priv = GET_PRIV (dock);
  static glong    id;

  priv->id = ++id;

  handlers = g_list_prepend (handlers, dock);

#ifdef USE_CARBON
  AEInstallEventHandler (kCoreEventClass, kAEQuitApplication, 
                         mac_dock_handle_quit,
                         priv->id, true);
  AEInstallEventHandler (kCoreEventClass, kAEOpenApplication,
                         mac_dock_handle_open_application,
                         priv->id, true);
  AEInstallEventHandler (kCoreEventClass, kAEReopenApplication, 
                         mac_dock_handle_reopen_application,
                         priv->id, true);
  AEInstallEventHandler (kCoreEventClass, kAEOpenDocuments,
                         mac_dock_handle_open_documents,
                         priv->id, true);
#endif
}

static void
mac_dock_finalize (GObject *object)
{
  IgeMacDockPriv *priv;

  priv = GET_PRIV (object);

#ifdef USE_CARBON
  AERemoveEventHandler (kCoreEventClass, kAEQuitApplication,
                        mac_dock_handle_quit, false);
  AERemoveEventHandler (kCoreEventClass, kAEReopenApplication,
                        mac_dock_handle_reopen_application, false);
  AERemoveEventHandler (kCoreEventClass, kAEOpenApplication,
                        mac_dock_handle_open_application, false);
  AERemoveEventHandler (kCoreEventClass, kAEOpenDocuments,
                        mac_dock_handle_open_documents, false);
#endif

  handlers = g_list_remove (handlers, object);

  G_OBJECT_CLASS (ige_mac_dock_parent_class)->finalize (object);
}

IgeMacDock *
ige_mac_dock_new (void)
{
  return g_object_new (IGE_TYPE_MAC_DOCK, NULL);
}

IgeMacDock *
ige_mac_dock_get_default (void)
{
  if (!global_dock)
    global_dock = g_object_new (IGE_TYPE_MAC_DOCK, NULL);

  return global_dock;
}

/* For internal use only. Returns TRUE if there is a handled setup for the
 * Quit dock menu item (i.e. if there is a dock instance alive).
 */
gboolean
_ige_mac_dock_is_quit_menu_item_handled (void)
{
  return handlers != NULL;
}

#ifdef USE_CARBON
static IgeMacDock *
mac_dock_get_from_id (gulong id)
{
  GList      *l;
  IgeMacDock *dock = NULL;

  for (l = handlers; l; l = l->next)
    {
      dock = l->data;
      if (GET_PRIV (dock)->id == id)
        break;

      dock = NULL;
  }

  return dock;
}

static OSErr
mac_dock_handle_quit (const AppleEvent *inAppleEvent, 
                      AppleEvent       *outAppleEvent, 
                      long              inHandlerRefcon)
{
  IgeMacDock *dock;

  dock = mac_dock_get_from_id (inHandlerRefcon);

  if (dock)
    g_signal_emit (dock, signals[QUIT_ACTIVATE], 0);

  return noErr;
}

static OSErr
mac_dock_handle_open_application (const AppleEvent *inAppleEvent,
                                  AppleEvent       *outAppleEvent,
                                  long              inHandlerRefCon)
{
  /*g_print ("FIXME: mac_dock_handle_open_application\n");*/

  return noErr;
}

static OSErr
mac_dock_handle_reopen_application (const AppleEvent *inAppleEvent, 
                                    AppleEvent       *outAppleEvent, 
                                    long              inHandlerRefcon)
{
  IgeMacDock *dock;

  dock = mac_dock_get_from_id (inHandlerRefcon);

  if (dock)
    g_signal_emit (dock, signals[CLICKED], 0);
  
  return noErr;
}

static OSErr
mac_dock_handle_open_documents (const AppleEvent *inAppleEvent,
                                AppleEvent       *outAppleEvent,
                                long              inHandlerRefCon)
{
  IgeMacDock *dock;
  OSStatus    status;
  AEDescList  documents;
  gchar       path[MAXPATHLEN];

  /*g_print ("FIXME: mac_dock_handle_open_documents\n");*/

  dock = mac_dock_get_from_id (inHandlerRefCon);

  status = AEGetParamDesc (inAppleEvent,
                           keyDirectObject, typeAEList,
                           &documents);
  if (status == noErr)
    {
      long count = 0;
      int  i;

      AECountItems (&documents, &count);

      for (i = 0; i < count; i++)
        {
          FSRef ref;

          status = AEGetNthPtr (&documents, i + 1, typeFSRef, 
                                0, 0, &ref, sizeof (ref),
                                0);
          if (status != noErr)
            continue;

          FSRefMakePath (&ref, (char *) path, MAXPATHLEN);

          /* FIXME: Add to a list, then emit the open-documents
           * signal.
           */
          /*g_print ("  %s\n", path);*/
        }
    }
        
    return status;
}

void
ige_mac_dock_set_icon_from_pixbuf (IgeMacDock *dock,
                                   GdkPixbuf  *pixbuf)
{
  if (!pixbuf)
    RestoreApplicationDockTileImage ();
  else
    {
      CGImageRef image;

      image = ige_mac_image_from_pixbuf (pixbuf);
      SetApplicationDockTileImage (image);
      CGImageRelease (image);
    }
}

void
ige_mac_dock_set_icon_from_resource (IgeMacDock   *dock,
                                     IgeMacBundle *bundle,
                                     const gchar  *name,
                                     const gchar  *type,
                                     const gchar  *subdir)
{
  gchar *path;

  g_return_if_fail (IGE_IS_MAC_DOCK (dock));
  g_return_if_fail (name != NULL);

  path = ige_mac_bundle_get_resource_path (bundle, name, type, subdir);
  if (path)
    {
      GdkPixbuf *pixbuf;

      pixbuf = gdk_pixbuf_new_from_file (path, NULL);
      if (pixbuf)
        {
          ige_mac_dock_set_icon_from_pixbuf (dock, pixbuf);
          g_object_unref (pixbuf);
        }

      g_free (path);
    }
}

void
ige_mac_dock_set_overlay_from_pixbuf (IgeMacDock  *dock,
                                      GdkPixbuf   *pixbuf)
{
  CGImageRef image;

  g_return_if_fail (IGE_IS_MAC_DOCK (dock));
  g_return_if_fail (pixbuf == NULL || GDK_IS_PIXBUF (pixbuf));

  if (pixbuf)
    {
      image = ige_mac_image_from_pixbuf (pixbuf);
      OverlayApplicationDockTileImage (image);
      CGImageRelease (image);
    }
  else
    RestoreApplicationDockTileImage ();
}

void
ige_mac_dock_set_overlay_from_resource (IgeMacDock   *dock,
                                        IgeMacBundle *bundle,
                                        const gchar  *name,
                                        const gchar  *type,
                                        const gchar  *subdir)
{
  gchar *path;

  g_return_if_fail (IGE_IS_MAC_DOCK (dock));
  g_return_if_fail (name != NULL);

  path = ige_mac_bundle_get_resource_path (bundle, name, type, subdir);
  if (path)
    {
      GdkPixbuf *pixbuf;

      pixbuf = gdk_pixbuf_new_from_file (path, NULL);
      if (pixbuf)
        {
          ige_mac_dock_set_overlay_from_pixbuf (dock, pixbuf);
          g_object_unref (pixbuf);
        }

      g_free (path);
    }
}

struct _IgeMacAttentionRequest {
  NMRec    nm_request;
  guint    timeout_id;
  gboolean is_cancelled;
};

static gboolean
mac_dock_attention_cb (IgeMacAttentionRequest *request)
{
  request->timeout_id = 0;
  request->is_cancelled = TRUE;

  NMRemove (&request->nm_request);

  return FALSE;
}


/* FIXME: Add listener for "application activated" and cancel any
 * requests.
 */
IgeMacAttentionRequest *
ige_mac_dock_attention_request (IgeMacDock          *dock,
                                IgeMacAttentionType  type)
{
  IgeMacAttentionRequest *request;

  request = g_new0 (IgeMacAttentionRequest, 1);

  request->nm_request.nmMark = 1;
  request->nm_request.qType = nmType;
  
  if (NMInstall (&request->nm_request) != noErr)
    {
      g_free (request);
      return NULL;
    }

  if (type == IGE_MAC_ATTENTION_INFO)
    request->timeout_id = gdk_threads_add_timeout (
            1000,
            (GSourceFunc) mac_dock_attention_cb,
            request);

  return request;
}

void
ige_mac_dock_attention_cancel (IgeMacDock             *dock,
                               IgeMacAttentionRequest *request)
{
  if (request->timeout_id)
    g_source_remove (request->timeout_id);

  if (!request->is_cancelled)
    NMRemove (&request->nm_request);

  g_free (request);
}

GType
ige_mac_attention_type_get_type (void)
{
  /* FIXME */
  return 0;
}
#else /* USE_COCOA */

#include <AvailabilityMacros.h>

static NSImage* _NSImageFromCGImage(CGImageRef image)
{
    NSImage* newImage = nil;
#if MAC_OS_X_VERSION_MIN_REQUIRED > MAC_OS_X_VERSION_10_4 /* 10_5_AND_LATER */
    NSBitmapImageRep *bitmapRep = [[NSBitmapImageRep alloc] initWithCGImage:image];

    newImage = [[NSImage alloc] init];
    [newImage addRepresentation:bitmapRep];
    [bitmapRep release];
#else
    NSRect imageRect = NSMakeRect(0.0, 0.0, 0.0, 0.0);
    CGContextRef imageContext = nil;
    imageRect.size.height = CGImageGetHeight(image);
    imageRect.size.width = CGImageGetWidth(image);
 
    newImage = [[[NSImage alloc] initWithSize:imageRect.size] autorelease];
    [newImage lockFocus];
    imageContext = (CGContextRef)[[NSGraphicsContext currentContext] graphicsPort];
    CGContextDrawImage(imageContext, *(CGRect*)&imageRect, image);
    [newImage unlockFocus];
#endif
     return newImage;
}

void
ige_mac_dock_set_icon_from_pixbuf (IgeMacDock *dock,
                                   GdkPixbuf  *pixbuf)
{
  if (!pixbuf)
    [NSApp setApplicationIconImage: nil];
  else
    {
      CGImageRef image;

      image = ige_mac_image_from_pixbuf (pixbuf);
      [NSApp setApplicationIconImage: _NSImageFromCGImage(image)];
      CGImageRelease (image);
    }
}

void
ige_mac_dock_set_icon_from_resource (IgeMacDock   *dock,
                                     IgeMacBundle *bundle,
                                     const gchar  *name,
                                     const gchar  *type,
                                     const gchar  *subdir)
{
  gchar *path;

  g_return_if_fail (IGE_IS_MAC_DOCK (dock));
  g_return_if_fail (name != NULL);

  path = ige_mac_bundle_get_resource_path (bundle, name, type, subdir);
  if (path)
    {
      GdkPixbuf *pixbuf;

      pixbuf = gdk_pixbuf_new_from_file (path, NULL);
      if (pixbuf)
        {
          ige_mac_dock_set_icon_from_pixbuf (dock, pixbuf);
          g_object_unref (pixbuf);
        }

      g_free (path);
    }
}

#if MAC_OS_X_VERSION_MIN_REQUIRED <= MAC_OS_X_VERSION_10_4
/* TODO: load these at runtime, instead of linking */

extern OSStatus OverlayApplicationDockTileImage(CGImageRef inImage);
extern OSStatus RestoreApplicationDockTileImage(void);
#endif

void
ige_mac_dock_set_overlay_from_pixbuf (IgeMacDock  *dock,
                                      GdkPixbuf   *pixbuf)
{
  CGImageRef image;

  g_return_if_fail (IGE_IS_MAC_DOCK (dock));
  g_return_if_fail (pixbuf == NULL || GDK_IS_PIXBUF (pixbuf));

#if MAC_OS_X_VERSION_MIN_REQUIRED > MAC_OS_X_VERSION_10_4 /* 10_5_AND_LATER */
  if (pixbuf)
    {
      NSDockTile* dockTile = [NSApp dockTile];
      NSRect frame = NSMakeRect(0, 0, dockTile.size.width, dockTile.size.height);
      NSImageView* dockImageView = [[NSImageView alloc] initWithFrame: frame];
      image = ige_mac_image_from_pixbuf (pixbuf);
      [dockImageView setImage: _NSImageFromCGImage(image)];
      [dockTile setContentView: dockImageView];
      [dockTile display];
      CGImageRelease (image);
    }
  else
    {
      NSDockTile* dockTile = [NSApp dockTile];
      [dockTile setContentView: nil]; /* XXXX */
      [dockTile display];
    }
#else /* need to use Carbon on earlier Mac OS X */
  if (pixbuf)
    {
      image = ige_mac_image_from_pixbuf (pixbuf);
      OverlayApplicationDockTileImage (image);
      CGImageRelease (image);
    }
  else
    RestoreApplicationDockTileImage ();
#endif
}

void
ige_mac_dock_set_overlay_from_resource (IgeMacDock   *dock,
                                        IgeMacBundle *bundle,
                                        const gchar  *name,
                                        const gchar  *type,
                                        const gchar  *subdir)
{
  gchar *path;

  g_return_if_fail (IGE_IS_MAC_DOCK (dock));
  g_return_if_fail (name != NULL);

  path = ige_mac_bundle_get_resource_path (bundle, name, type, subdir);
  if (path)
    {
      GdkPixbuf *pixbuf;

      pixbuf = gdk_pixbuf_new_from_file (path, NULL);
      if (pixbuf)
        {
          ige_mac_dock_set_overlay_from_pixbuf (dock, pixbuf);
          g_object_unref (pixbuf);
        }

      g_free (path);
    }
}

#define USE_CFUSERNOTIFICATION	0

struct _IgeMacAttentionRequest {
#if USE_CFUSERNOTIFICATION
  CFUserNotificationRef un_ref;
#else
  int identifier;
#endif
  guint    timeout_id;
  gboolean is_cancelled;
};

static gboolean
mac_dock_attention_cb (IgeMacAttentionRequest *request)
{
  request->timeout_id = 0;
  request->is_cancelled = TRUE;

#if USE_CFUSERNOTIFICATION
  CFUserNotificationCancel(request->un_ref);
#else
  [NSApp cancelUserAttentionRequest: request->identifier];
#endif
  
  return FALSE;
}

IgeMacAttentionRequest *
ige_mac_dock_attention_request (IgeMacDock          *dock,
                                IgeMacAttentionType  type)
{
  IgeMacAttentionRequest *request;
#if USE_CFUSERNOTIFICATION
  CFMutableDictionaryRef elements;
  CFOptionFlags flags;
  SInt32 error;
#else
  NSRequestUserAttentionType requestType;
#endif
  
  request = g_new0 (IgeMacAttentionRequest, 1);

#if USE_CFUSERNOTIFICATION
  if (type == IGE_MAC_ATTENTION_INFO)
    flags = kCFUserNotificationPlainAlertLevel | kCFUserNotificationNoDefaultButtonFlag;
  else /* IGE_MAC_ATTENTION_CRITICAL */
    flags = kCFUserNotificationStopAlertLevel;

  elements = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
      /* keyCallBacks */ NULL, /* valueCallBacks */ NULL);
  if (elements == NULL)
    {
      return NULL;
    }

  CFDictionaryAddValue(elements, kCFUserNotificationAlertHeaderKey, CFSTR(""));

  request->un_ref = CFUserNotificationCreate(kCFAllocatorDefault,
      0, flags, &error, elements);
  if (error != noErr)
    {
      g_free (request);
      return NULL;
    }

  CFRelease(elements);
#else
  if (type == IGE_MAC_ATTENTION_INFO)
    requestType = NSInformationalRequest;
  else /* IGE_MAC_ATTENTION_CRITICAL */
    requestType = NSCriticalRequest;

  request->identifier = [NSApp requestUserAttention:requestType];
#endif

  if (type == IGE_MAC_ATTENTION_INFO)
    request->timeout_id = gdk_threads_add_timeout (
            1000,
            (GSourceFunc) mac_dock_attention_cb,
            request);

  return request;
}

void
ige_mac_dock_attention_cancel (IgeMacDock             *dock,
                               IgeMacAttentionRequest *request)
{
  if (request->timeout_id)
    g_source_remove (request->timeout_id);

  if (!request->is_cancelled)
#if USE_CFUSERNOTIFICATION
    CFUserNotificationCancel(request->un_ref);
#else
    [NSApp cancelUserAttentionRequest: request->identifier];
#endif

  g_free (request);
}
#endif
