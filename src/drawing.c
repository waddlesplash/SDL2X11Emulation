#include <X11/Xlib.h>
#include "drawing.h"
#include "errors.h"
#include "window.h"
#include "display.h"
#include "util.h"
#include "gc.h"

/*
 * Flip all screen children and cause them to draw their content to the screen.
 */
void flipScreen() {
	Window* children = GET_CHILDREN(SCREEN_WINDOW);
	int i;
	for (i = 0; i < GET_WINDOW_STRUCT(SCREEN_WINDOW)->children.length; i++) {
		if (children[i] != NULL && GET_WINDOW_STRUCT(children[i])->sdlRenderer != NULL) {
			SDL_RenderPresent(GET_WINDOW_STRUCT(children[i])->sdlRenderer);
		}
	}
	#ifdef DEBUG_WINDOWS
	printWindowHierarchy();
//    drawDebugWindowSurfacePlanes();
//    drawWindowDebugView();
	#endif
}

SDL_Renderer* getWindowRenderer(Window window) {
	SDL_Rect viewPort;
	SDL_Renderer* renderer = NULL;
	int x = 0, y = 0;
	viewPort.x = 0;
	viewPort.y = 0;
	GET_WINDOW_DIMS(window, viewPort.w, viewPort.h);
	while (GET_PARENT(window) != NULL && GET_WINDOW_STRUCT(window)->sdlWindow == NULL
		   && GET_WINDOW_STRUCT(window)->mapState != UnMapped) {
		GET_WINDOW_POS(window, x, y);
		viewPort.x += x;
		viewPort.y += y;
		window = GET_PARENT(window);
	}
	renderer = GET_WINDOW_STRUCT(window)->sdlRenderer;
	if (renderer == NULL) {
		if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
			renderer = SDL_CreateRenderer(GET_WINDOW_STRUCT(window)->sdlWindow, -1,
										  SDL_RENDERER_ACCELERATED);
		}
		if (renderer == NULL) {
			renderer = GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
			SDL_Texture* texture = GET_WINDOW_STRUCT(window)->sdlTexture;
			if (texture == NULL) {
				int w, h;
				GET_WINDOW_DIMS(window, w, h);
				texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
											SDL_TEXTUREACCESS_TARGET, w, h);
				if (texture == NULL) {
					fprintf(stderr, "WTF: SDL_CreateTexture failed in %s for window %p: %s\n",
							__func__, window, SDL_GetError());
					#ifdef DEBUG_WINDOWS
					printWindowHierarchy();
					#endif
				} else {
					GET_WINDOW_STRUCT(window)->sdlTexture = texture;
				}
				SDL_SetRenderTarget(renderer, texture);
			}
		} else {
			GET_WINDOW_STRUCT(window)->sdlRenderer = renderer;
		}
	}
	#ifdef SDL_VIEWPORT_INCORRECT_COORDINATE_ORIGIN
	int w, h;
	GET_WINDOW_DIMS(window, w, h);
	viewPort.y = h - viewPort.y - viewPort.h;
	#endif
	//fprintf(stderr, "Setting viewport to {x = %d, y = %d, w = %d, h = %d}\n", viewPort.x, viewPort.y, viewPort.w, viewPort.h);
	if (SDL_RenderSetViewport(renderer, &viewPort)) {
		fprintf(stderr, "SDL_RenderSetViewport failed in %s: %s\n", __func__, SDL_GetError());
	}
	return renderer;
}

int XFillPolygon(Display* display, Drawable d, GC gc, XPoint *points, int npoints, int shape, int mode) {
    // https://tronche.com/gui/x/xlib/graphics/filling-areas/XFillPolygon.html
	SET_X_SERVER_REQUEST(display, X_PolyFillArc);
	WARN_UNIMPLEMENTED;
    return 1;
}

int XFillArc(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height, int angle1, int angle2) {
    // https://tronche.com/gui/x/xlib/graphics/filling-areas/XFillArc.html
    SET_X_SERVER_REQUEST(display, X_PolyFillArc);
    WARN_UNIMPLEMENTED;
    return 1;
}

int XDrawArc(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height, int angle1, int angle2) {
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawArc.html
    SET_X_SERVER_REQUEST(display, X_PolyArc);
    WARN_UNIMPLEMENTED;
    return 1;
}

int XCopyPlane(Display *display, Drawable src, Drawable dest, GC gc, int src_x, int src_y, unsigned int width, unsigned int height, int dest_x, int dest_y, unsigned long plane) {
    // https://tronche.com/gui/x/xlib/graphics/XCopyPlane.html
    SET_X_SERVER_REQUEST(display, X_CopyPlane);
    WARN_UNIMPLEMENTED;
    return 1;
}

int XDrawLine(Display* display, Drawable d, GC gc, int x1, int y1, int x2, int y2) {
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawLine.html
    WARN_UNIMPLEMENTED;
    return 1;
}

int XDrawLines(Display *display, Drawable d, GC gc, XPoint *points, int npoints, int mode) {
	// https://tronche.com/gui/x/xlib/graphics/drawing/XDrawLines.html
	SET_X_SERVER_REQUEST(display, X_CopyPlane);
	TYPE_CHECK(d, DRAWABLE, display, 0);
	fprintf(stderr, "%s: Drawing on %p\n", __func__, d);
	if (npoints <= 1) {
		fprintf(stderr, "Invalid number of points in %s: %d\n", __func__, npoints);
		handleError(0, display, None, 0, BadValue, 0);
		return 1;
	}
	SDL_Renderer* renderer = NULL;
	GET_RENDERER(d, renderer);
	if (renderer == NULL) {
		fprintf(stderr, "Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
		handleError(0, display, None, 0, BadValue, 0);
		return 1;
	}
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	SDL_Point sdlPoints[npoints];
	int i;
	if (mode == CoordModeOrigin) {
		for (i = 0; i < npoints; i++) {
			sdlPoints[i].x = (int) points[i].x;
			sdlPoints[i].y = (int) points[i].y;
		}
	} else if (mode == CoordModePrevious) {
		sdlPoints[0].x = (int) points[0].x;
		sdlPoints[0].y = (int) points[0].y;
		for (i = 1; i < npoints; i++) {
			sdlPoints[i].x = sdlPoints[i - 1].x + (int) points[i].x;
			sdlPoints[i].y = sdlPoints[i - 1].y + (int) points[i].y;
		}
	} else {
		handleError(0, display, None, 0, BadValue, 0);
		return 1;
	}
//    for (i = 0; i < npoints; i++) {
//        fprintf(stderr, " (x = %d, y = %d)\n", sdlPoints[i].x, sdlPoints[i].y);
//    }
	GraphicContext* gContext = GET_GC(gc);
	long color = gContext->foreground;
	SDL_SetRenderDrawColor(renderer, GET_RED_FROM_COLOR(color), GET_GREEN_FROM_COLOR(color),
		GET_BLUE_FROM_COLOR(color), GET_ALPHA_FROM_COLOR(color));
	if (SDL_RenderDrawLines(renderer, &sdlPoints[0], npoints)) {
		fprintf(stderr, "SDL_RenderDrawLines failed in %s: %s\n", __func__, SDL_GetError());
	}
	SDL_RenderPresent(renderer);
}

int XCopyArea(Display* display, Drawable src, Drawable dest, GC gc, int src_x, int src_y,
               unsigned int width, unsigned int height, int dest_x, int dest_y) {
    // https://tronche.com/gui/x/xlib/graphics/XCopyArea.html
	SET_X_SERVER_REQUEST(display, X_CopyArea);
	TYPE_CHECK(src, DRAWABLE, display, 0);
	TYPE_CHECK(dest, DRAWABLE, display, 0);
	LOG("%s: Copy area from %p to %p\n", __func__, src, dest);
	if (IS_TYPE(src, WINDOW)) {
		if (IS_INPUT_ONLY(src)) {
			LOG("BadMatch: Got input only window as the source in %s!\n", __func__);
			handleError(0, display, src, 0, BadMatch, 0);
			return 0;
		} else if (GET_WINDOW_STRUCT(src)->mapState == UnMapped && GET_WINDOW_STRUCT(src)->sdlTexture == NULL) {
			return 0;
		}
	}
	if (IS_TYPE(dest, WINDOW)) {
		if (IS_INPUT_ONLY(dest)) {
			LOG("BadMatch: Got input only window as the destination in %s!\n", __func__);
			handleError(0, display, dest, 0, BadMatch, 0);
			return 0;
		}
		SDL_Renderer* destRenderer = getWindowRenderer(dest);
		SDL_Renderer* srcRenderer;
		GET_RENDERER(src, srcRenderer);
		SDL_Surface* srcSurface = getRenderSurface(srcRenderer);
		if (srcSurface == NULL) {
			handleError(0, display, src, 0, BadMatch, 0);
		}
		SDL_Texture* srcTexture = SDL_CreateTextureFromSurface(destRenderer, srcSurface);
		SDL_FreeSurface(srcSurface);
		SDL_SetRenderDrawBlendMode(destRenderer, SDL_BLENDMODE_BLEND);
		SDL_SetTextureBlendMode(srcTexture, SDL_BLENDMODE_BLEND);
		SDL_Rect srcRect, destRect;
		srcRect.x = src_x;
		srcRect.y = src_y;
		destRect.x = dest_x;
		destRect.y = dest_y;
		srcRect.w = destRect.w = width;
		srcRect.h = destRect.h = height;
		if (SDL_RenderCopy(destRenderer, srcTexture, &srcRect, &destRect) != 0) {
			LOG("SDL_RenderCopy failed in %s: %s\n", __func__, SDL_GetError());
			handleError(0, display, src, 0, BadMatch, 0);
			return 0;
		}
		SDL_DestroyTexture(srcTexture);
		SDL_RenderPresent(destRenderer);
	} else {
		LOG("Hit unimplemented type in %s: %d\n", __func__, GET_XID_TYPE(dest));
	}

	// TODO: Events
	return 1;
}

int XDrawRectangle(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height) {
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawRectangle.html
	return 1;
}

int XFillRectangle(Display* display, Drawable d, GC gc, int x, int y,
                   unsigned int width, unsigned int height) {
    XRectangle rectangle; // TODO: Make this not depend on XFillRectangles
    rectangle.x = x;
    rectangle.y = y;
    rectangle.width = width;
    rectangle.height = height;
    return XFillRectangles(display, d, gc, &rectangle, 1);
}

int XFillRectangles(Display *display, Drawable d, GC gc, XRectangle *rectangles, int nrectangles) {
	// https://tronche.com/gui/x/xlib/graphics/filling-areas/XFillRectangles.html
	SET_X_SERVER_REQUEST(display, X_PolyFillRectangle);
	TYPE_CHECK(d, DRAWABLE, display, 0);
	LOG("%s: Drawing on %p\n", __func__, d);
	if (nrectangles < 1) {
		LOG("Invalid number of rectangles in %s: %d\n", __func__, nrectangles);
		handleError(0, display, None, 0, BadValue, 0);
		return 0;
	}
	SDL_Renderer* renderer = NULL;
	GET_RENDERER(d, renderer);
	if (renderer == NULL) {
		LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
		handleError(0, display, d, 0, BadDrawable, 0);
		return 0;
	}
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	SDL_Rect sdlRectangles[nrectangles];
	int i;
	for (i = 0; i < nrectangles; i++) {
		sdlRectangles[i].x = (int) rectangles[i].x;
		sdlRectangles[i].y = (int) rectangles[i].y;
		sdlRectangles[i].w = (int) rectangles[i].width;
		sdlRectangles[i].h = (int) rectangles[i].height;
		LOG("{x = %d, y = %d, w = %d, h = %d}\n", sdlRectangles[i].x,
				sdlRectangles[i].y, sdlRectangles[i].w, sdlRectangles[i].h);
	}
	GraphicContext* gContext = GET_GC(gc);
	LOG("bgColor: 0x%08lx, fgColor: 0x%08lx\n", gContext->background, gContext->foreground);
	if (gContext->fillStyle == FillSolid) {
		LOG("Fill_style is %s\n", "FillSolid");
		long color = gContext->foreground;
		SDL_SetRenderDrawColor(renderer,
							   GET_RED_FROM_COLOR(color),
							   GET_GREEN_FROM_COLOR(color),
							   GET_BLUE_FROM_COLOR(color),
							   GET_ALPHA_FROM_COLOR(color));
		SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
		if (SDL_RenderFillRects(renderer, &sdlRectangles[0], nrectangles)) {
			LOG("SDL_RenderFillRects failed in %s: %s\n", __func__, SDL_GetError());
		}
	} else if (gContext->fillStyle == FillTiled) {
		LOG("Fill_style is %s\n", "FillTiled");
	} else if (gContext->fillStyle == FillOpaqueStippled) {
		LOG("Fill_style is %s\n", "FillOpaqueStippled");
		SDL_Rect viewPort;
		SDL_RenderGetViewport(renderer, &viewPort);
		SDL_Surface* renderSurface = SDL_CreateRGBSurface(0, viewPort.w, viewPort.h, SDL_SURFACE_DEPTH, DEFAULT_RED_MASK,
														  DEFAULT_GREEN_MASK, DEFAULT_BLUE_MASK,
														  DEFAULT_ALPHA_MASK);
		if (renderSurface == NULL) {
			LOG("Failed to create rendering surface in %s: %s\n", __func__, SDL_GetError());
			return 0;
		}
		if (SDL_FillRects(renderSurface, &sdlRectangles[0], nrectangles, gContext->background)) {
			LOG("SDL_FillRects failed in %s: %s\n", __func__, SDL_GetError());
			SDL_FreeSurface(renderSurface);
			return 0;
		}
//        colorClipped(renderSurface, GET_SURFACE(gc->stipple), gc->ts_x_origin, gc->ts_y_origin, gc->foreground);
		SDL_Texture* renderTexture = SDL_CreateTextureFromSurface(renderer, renderSurface);
		if (renderTexture == NULL) {
			LOG("Failed to create texture in %s: %s\n", __func__, SDL_GetError());
		} else {
			SDL_RenderCopy(renderer, renderTexture, NULL, NULL);
		}
		SDL_DestroyTexture(renderTexture);
		SDL_FreeSurface(renderSurface);
	} else if (gContext->fillStyle == FillStippled) {
		LOG("Fill_style is %s\n", "FillStippled");
	}
	SDL_RenderPresent(renderer);
	return 1;
}
