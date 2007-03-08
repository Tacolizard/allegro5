#include <X11/Xlib.h>
#include <GL/glx.h>

typedef struct AL_SYSTEM_XDUMMY AL_SYSTEM_XDUMMY;
typedef struct AL_DISPLAY_XDUMMY AL_DISPLAY_XDUMMY;
typedef struct AL_BITMAP_XDUMMY AL_BITMAP_XDUMMY;

/* This is our version of AL_SYSTEM with driver specific extra data. */
struct AL_SYSTEM_XDUMMY
{
   AL_SYSTEM system; /* This must be the first member, we "derive" from it. */

   /* Driver specifics. */

   Display *xdisplay; /* The X11 display. */
   pthread_t thread; /* background thread. */
};

/* This is our version of AL_DISPLAY with driver specific extra data. */
struct AL_DISPLAY_XDUMMY
{
   AL_DISPLAY display; /* This must be the first member. */
   
   /* Driver specifics. */

   Window window;
   GLXWindow glxwindow;
   GLXContext context;
   int is_initialized;
   Atom wm_delete_window_atom;
};

struct AL_BITMAP_XDUMMY
{
   AL_BITMAP bitmap; /* This must be the first member. */
   
   /* Driver specifics. */
   
   GLuint texture; /* 0 means, not uploaded yet. */
   float left, top, right, bottom; /* Texture coordinates. */
};

void _al_display_xdummy_configure(AL_DISPLAY *d, XEvent *event);
void _al_xwin_keyboard_handler(XKeyEvent *event, bool dga2_hack);
void _al_display_xdummy_closebutton(AL_DISPLAY *d, XEvent *xevent);

