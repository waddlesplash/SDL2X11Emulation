#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "colors.h"
#include "stdColors.h"
#include "errors.h"
#include "display.h"
#include "util.h"

SDL_Color uLongToColor(SDL_PixelFormat* pixelFormat, unsigned long color) {
    SDL_Color res;
    SDL_GetRGBA(color, pixelFormat, &res.r, &res.g, &res.b, &res.a);
    return res;
}

SDL_Color uLongToColorFromVisual(Visual* visual, unsigned long color) {
    SDL_Color res;
    res.r = (visual->red_mask & color) >> 24;
    res.g = (visual->green_mask & color) >> 16;
    res.b = (visual->blue_mask & color) >> 8;
    res.a = (~(visual->red_mask | visual->green_mask | visual->blue_mask)) & color;
    return res;
}

int XFreeColormap(Display* display, Colormap colormap) {
    // https://tronche.com/gui/x/xlib/color/XFreeColormap.html
    SET_X_SERVER_REQUEST(display, X_FreeColormap);
	// Nothing to do.
	return Success;
}

Colormap XCreateColormap(Display* display, Window window, Visual* visual, int allocate) {
    // https://tronche.com/gui/x/xlib/color/XCreateColormap.html
    SET_X_SERVER_REQUEST(display, X_CreateColormap);
    int visualClass;
	visualClass = visual ? visual->CLASS_ATTRIBUTE : TrueColor;
    Colormap colormap;
    switch (visualClass) {
        case StaticGray:
        case GrayScale:
            colormap = GREY_SCALE_COLORMAP;
            break;
        case PseudoColor:
        case DirectColor:
        case StaticColor:
        case TrueColor:
            colormap = REAL_COLOR_COLORMAP;
            break;
        default:
            LOG("Bad parameter: got an unknown visual class in XCreateColormap: %d\n", visualClass);
            handleError(0, display, None, 0, BadMatch, 0);
            return None;
    }
    return colormap;
}

int XQueryColors(Display *display, Colormap colormap, XColor* defs_in_out, int ncolors) {
    // https://tronche.com/gui/x/xlib/color/XQueryColors.html
    SET_X_SERVER_REQUEST(display, X_QueryColors);
    int i;
    for (i = 0; i < ncolors; ++i) {
        XColor color = defs_in_out[i];
        color.red   = (unsigned int) (color.pixel & 0xFF000000) >> 24;
        color.green = (unsigned int) (color.pixel & 0x00FF0000) >> 16;
        color.blue  = (unsigned int) (color.pixel & 0x0000FF00) >>  8;
    }
	return Success;
}

Status XLookupColor(Display* display, Colormap colormap, _Xconst char* color_name,
                    XColor* exact_def_return, XColor* screen_def_return) {
    // https://tronche.com/gui/x/xlib/color/XLookupColor.html
    SET_X_SERVER_REQUEST(display, X_LookupColor);
    int nameLength = strlen(color_name);
    int i, strIndex, offset;
    StdColorEntry entry;
    int maxLength;
    Bool doesMatch;
    for (i = 0; i < NUM_STANDARD_COLORS; i++) {
        doesMatch = True;
        offset = 0;
        entry = STANDARD_COLORS[i];
        maxLength = strlen(entry.name);
        if (nameLength <= maxLength) {
            for (strIndex = 0; strIndex < maxLength; strIndex++) {
                if (entry.name[strIndex] == ' ') {
                    if (color_name[strIndex - offset] != ' ') {
                        offset++;
                    }
                } else if (entry.name[strIndex] != tolower(color_name[strIndex - offset])) {
                    doesMatch = False;
                    break;
                }
            }
            if (doesMatch) {
                // TODO: Should I use the colormap?
                XColor colorList[] = {*exact_def_return, *screen_def_return};
                exact_def_return->pixel = entry.pixelValue;
                screen_def_return->pixel = entry.pixelValue;
                XQueryColors(display, colormap, colorList, 2);
                return 1;
            }
        }
    }
    return 0;
}

Status XAllocNamedColor(Display* display, Colormap colormap, _Xconst char* color_name,
                        XColor* screen_def_return, XColor* exact_def_return) {
    // https://tronche.com/gui/x/xlib/color/XAllocNamedColor.html
    SET_X_SERVER_REQUEST(display, X_AllocNamedColor);
    return XLookupColor(display, colormap, color_name, screen_def_return, exact_def_return);
}

int XFreeColors(Display* display, Colormap colormap, unsigned long pixels[], int npixels,
                 unsigned long planes) {
    // https://tronche.com/gui/x/xlib/color/XFreeColors.html
    SET_X_SERVER_REQUEST(display, X_FreeColors);
    // We have no real colormap, so we don't need to free colors
	return Success;
}

Status XAllocColor(Display* display, Colormap colormap, XColor* screen_in_out) {
    // https://tronche.com/gui/x/xlib/color/XAllocColor.html
    SET_X_SERVER_REQUEST(display, X_AllocColor);
    // Since our "colormap" has as many colors as there are pixel, this call always succeeds.
    // screen_in_out is always the same.
	return Success;
}

Status XParseColor(Display* display, Colormap colormap, _Xconst char *spec, XColor *exact_def_return) {
    // https://tronche.com/gui/x/xlib/color/XParseColor.html
    fprintf(stderr, "Hit unimplemented function %s.\n", __func__);
	return BadValue;
}
