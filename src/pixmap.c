#include "X11/Xlib.h"
#include "drawing.h"
#include "errors.h"
#include "resourceTypes.h"
#include "display.h"

Pixmap XCreatePixmap(Display* display, Drawable drawable, unsigned int width, unsigned int height,
                     unsigned int depth) {
	// https://tronche.com/gui/x/xlib/pixmap-and-cursor/XCreatePixmap.html
	SET_X_SERVER_REQUEST(display, X_CreatePixmap);
	(void) drawable;
	// TODO: Adjust masks for depth
	if (width == 0 || height == 0) {
		LOG("Width and/or height are 0 in XCreatePixmap: w = %u, h = %u\n", width, height);
		handleError(0, display, None, 0, BadValue, 0);
		return None;
	}
	if (depth < 1) {
		LOG("Got unsupported depth (%u) in XCreatePixmap\n", depth);
		handleError(0, display, None, 0, BadValue, 0);
		return None;
	}
	XID pixmap = ALLOC_XID();
	if (pixmap == None) {
		LOG("Out of memory: Could not allocate XID in XCreatePixmap!\n");
		handleOutOfMemory(0, display, 0, 0);
		return None;
	}
	LOG("%s: addr= %lu, w = %d, h = %d\n", __func__, pixmap, width, height);
	SDL_Texture* texture = SDL_CreateTexture(GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer,
											 SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
											 (int) width, (int) height);
	if (texture == NULL) {
		fprintf(stderr, "SDL_CreateTexture failed in XCreatePixmap: %s\n", SDL_GetError());
		FREE_XID(pixmap);
		handleOutOfMemory(0, display, 0, 0);
		return NULL;
	}
	SET_XID_TYPE(pixmap, PIXMAP);
	SET_XID_VALUE(pixmap, texture);

	SDL_Renderer* renderer;
	GET_RENDERER(pixmap, renderer);
	SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
	SDL_RenderClear(renderer);
	return pixmap;
}

int XFreePixmap(Display* display, Pixmap pixmap) {
	// https://tronche.com/gui/x/xlib/pixmap-and-cursor/XFreePixmap.html
	SET_X_SERVER_REQUEST(display, X_FreePixmap);
	TYPE_CHECK(pixmap, PIXMAP, display, 0);
	SDL_Texture* texture = GET_PIXMAP_TEXTURE(pixmap);
	free(pixmap);
	SDL_DestroyTexture(texture);
	return 1;
}

Pixmap XCreateBitmapFromData(Display* display, Drawable d, _Xconst char* data,
							 unsigned int width, unsigned int height) {
	// https://tronche.com/gui/x/xlib/utilities/XCreateBitmapFromData.html
	SET_X_SERVER_REQUEST(display, X_CreatePixmap);
	XID pixmap = ALLOC_XID();
	if (pixmap == None) {
		LOG("Out of memory: Could not allocate XID in %s!\n", __func__);
		handleOutOfMemory(0, display, BadAlloc, 0);
		return None;
	}

	SDL_Renderer* renderer = NULL;
	GET_RENDERER(d, renderer);
	if (renderer == NULL) {
		LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
		handleError(0, display, d, 0, BadDrawable, 0);
		return 0;
	}

	SDL_Texture *image = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
	if (image == NULL) {
		LOG("SDL_CreateTextureFromSurface failed in %s: %s\n", __func__, SDL_GetError());
		FREE_XID(pixmap);
		handleOutOfMemory(0, display, BadAlloc, 0);
		return None;
	}

	if (SDL_SetRenderTarget(renderer, image) < 0) {
		LOG("SDL_SetRenderTarget failed in %s: %s\n", __func__, SDL_GetError());
		FREE_XID(pixmap);
		SDL_DestroyTexture(image);
		handleOutOfMemory(0, display, BadAlloc, 0);
		return None;
	}
	SET_XID_TYPE(pixmap, PIXMAP);
	SET_XID_VALUE(pixmap, image);
	return pixmap;
}
