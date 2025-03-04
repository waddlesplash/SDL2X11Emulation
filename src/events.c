#include <errno.h>
#include <fcntl.h>
#include <X11/Xlib.h>
#include "events.h"
#include "errors.h"
#include "input.h"
#include "inputMethod.h"
#include "display.h"
#include "atoms.h"
#include "util.h"

int eventFds[2];
#define READ_EVENT_FD eventFds[0]
#define WRITE_EVENT_FD eventFds[1]
SDL_Event waitingEvent;
Bool eventWaiting = False;
Bool tmpVar = False;
unsigned long lastEventSerial = 1;

void updateWindowRenderTargets(Display* display);

#define ENQUEUE_EVENT_IN_PIPE(display) { char buffer = 'e'; write(WRITE_EVENT_FD, &buffer, sizeof(buffer)); GET_DISPLAY(display)->qlen++; }
#define READ_EVENT_IN_PIPE(display) if (GET_DISPLAY(display)->qlen > 0) { char buffer; read(READ_EVENT_FD, &buffer, sizeof(buffer)); GET_DISPLAY(display)->qlen--; }

// TODO: Generate Enter & Leave events on MouseButton down and MouseMotion
// TODO: prioritize events like RENDER_TARGETS_RESET

Bool getRectIntersection(const SDL_Rect* rect1, const SDL_Rect* rect2, SDL_Rect *rectOut) { // TODO: Refine this
    if (rect2->x < rect1->x && rect2->x > rect1->x + rect1->w
        && rect2->y < rect1->y && rect2->y > rect1->y + rect1->h
        && rect2->x + rect2->w < rect1->x && rect2->x + rect2->w > rect1->y + rect1->w
        && rect2->y + rect2->h < rect1->y && rect2->y + rect2->h > rect1->y + rect1->h) {
        return False;
    }
    SDL_UnionRect(rect1, rect2, rectOut);
    rectOut->x -= rect2->x;
    rectOut->y -= rect2->y;
    return True;
}

void postExposeEvent(Display* display, Window window, const SDL_Rect* damagedAreaList, size_t numAreas) {
    size_t i = numAreas, j;
    while (i-- > 0) {
        postEvent(display, window, Expose, &damagedAreaList[i], i);
    }
    SDL_Rect* childDamagedAreaList = malloc(sizeof(SDL_Rect) * numAreas);
    if (childDamagedAreaList == NULL) return;
    Window* children = GET_CHILDREN(window);
    for (i = 0; i < GET_WINDOW_STRUCT(window)->children.length; i++) {
        if (!IS_INPUT_ONLY(children[i])
            && GET_WINDOW_STRUCT(children[i])->mapState == Mapped) {
            WindowStruct* childWindowStruct = GET_WINDOW_STRUCT(children[i]);
            SDL_Rect childWindowRect = {
                    childWindowStruct->x,
                    childWindowStruct->y,
                    childWindowStruct->w,
                    childWindowStruct->h,
            };
            size_t numChildAreas = 0;
            for (j = 0; j < numAreas; j++) {
                if (getRectIntersection(&damagedAreaList[j], &childWindowRect, &childDamagedAreaList[numChildAreas])) {
                    numChildAreas++;
                }
            }
            if (numChildAreas > 0) {
                postExposeEvent(display, children[i], childDamagedAreaList, numChildAreas);
            }
        }
    }
    free(childDamagedAreaList);
}

int onSdlEvent(void* userdata, SDL_Event* event) {
    switch (event->type) {
//        case SDL_QUIT:
        case SDL_WINDOWEVENT:
            if (GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlWindow == NULL || 
                    event->window.windowID == SDL_GetWindowID(GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlWindow)) {
                return 0;
            } // else fall trough to default
        default:
            ENQUEUE_EVENT_IN_PIPE((Display *) userdata);
    }
    return 1;
}

Bool getEventQueueLength(int* qlen) {
    SDL_Event tmp[25];
    *qlen = SDL_PeepEvents((SDL_Event*) &tmp, 25, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
    if (*qlen < 0) {
        LOG("Failed to get the length of the input queue: %s\n", SDL_GetError());
        return False;
    }
    return True;
}

int initEventPipe(Display* display) {
    lastEventSerial = 1;
    int flags;
    if (pipe(eventFds) == -1) {
        LOG("Could not create the event pipe: %s", strerror(errno));
        return -1;
    }
    flags = fcntl(READ_EVENT_FD, F_GETFL);
    fcntl(READ_EVENT_FD, F_SETFD, flags | O_NONBLOCK);
    FILE* event_write = fdopen(WRITE_EVENT_FD, "w");
    if (event_write == NULL) {
        LOG("Could not create the write input of the event pipe: %s", strerror(errno));
        return -1;
    }
    FILE* event_read = fdopen(READ_EVENT_FD, "r");
    if (event_read == NULL) {
        LOG("Could not create the read output of the event pipe: %s", strerror(errno));
        return -1;
    }
    int qlen;
    getEventQueueLength(&qlen);
    LOG("Events in queue = %d at initialisation\n", qlen);
    for (; qlen > 0; qlen--) {
        ENQUEUE_EVENT_IN_PIPE(display);
    }
    SDL_SetEventFilter(onSdlEvent, display);
    return READ_EVENT_FD;
}

unsigned int convertModifierState(Uint16 mod) {
    unsigned int state = 0;
    if (HAS_VALUE(mod, KMOD_SHIFT)) {
        state |= ShiftMask;
    }
    if (HAS_VALUE(mod, KMOD_CTRL)) {
        state |= ControlMask;
    }
    if (HAS_VALUE(mod, KMOD_CAPS)) {
        state |= LockMask;
    }
    if (HAS_VALUE(mod, KMOD_NUM)) {
        state |= Mod1Mask;
    }
    return state;
}

int convertEvent(Display* display, SDL_Event* sdlEvent, XEvent* xEvent) {
    Bool sendEvent = False;
    Window eventWindow = None;
    int type = -1;
#define FILL_STANDARD_VALUES(eventStruct) xEvent->eventStruct.type = type;\
    xEvent->eventStruct.serial = lastEventSerial;\
    xEvent->eventStruct.send_event = sendEvent;\
    xEvent->eventStruct.display = display
    //TODO: this needs to update the window attributes
    switch (sdlEvent->type) {
        case SDL_KEYDOWN:
            type = KeyPress;
            LOG("SDL_KEYDOWN\n");
        case SDL_KEYUP:
            if (sdlEvent->type == SDL_KEYUP) {
                LOG("SDL_KEYUP\n");
                type = KeyRelease;
            }
            FILL_STANDARD_VALUES(xkey);
            xEvent->xkey.root = getWindowFromId(sdlEvent->key.windowID);
            eventWindow = getKeyboardFocus();
            eventWindow = eventWindow == None ? xEvent->xkey.root : eventWindow;
            xEvent->xkey.window = eventWindow;
            xEvent->xkey.subwindow = None;
            xEvent->xkey.time = sdlEvent->key.timestamp;
            SDL_GetMouseState(&xEvent->xkey.x, &xEvent->xkey.y);
            xEvent->xkey.x_root = xEvent->xkey.x; // Because root and window are the same.
            xEvent->xkey.y_root = xEvent->xkey.y;
            xEvent->xkey.state = convertModifierState(sdlEvent->key.keysym.mod);
            xEvent->xkey.keycode = (unsigned int) sdlEvent->key.keysym.sym;
            xEvent->xkey.same_screen = True;
            break;
        case SDL_MOUSEBUTTONDOWN:
            LOG("SDL_MOUSEBUTTONDOWN\n");
            type = ButtonPress;
            if (!tmpVar) { // TODO: propper implementation
                ENQUEUE_EVENT_IN_PIPE(display);
                eventWaiting = True;
                memcpy(&waitingEvent, sdlEvent, sizeof(SDL_Event));
                type = EnterNotify;
                xEvent->xcrossing.root = getWindowFromId(sdlEvent->button.windowID);
                if (xEvent->xbutton.root == None) {
                    xEvent->xbutton.root = SCREEN_WINDOW;
                }
                eventWindow = getContainingWindow(xEvent->xcrossing.root, sdlEvent->button.x, sdlEvent->button.y);
                FILL_STANDARD_VALUES(xcrossing);
                xEvent->xcrossing.window = eventWindow;
                // TODO: Check if pointer is in child
                xEvent->xcrossing.subwindow = None;
                xEvent->xcrossing.time = sdlEvent->button.timestamp;
                SDL_GetMouseState(&xEvent->xcrossing.x_root, &xEvent->xcrossing.y_root);
                xEvent->xcrossing.x = xEvent->xcrossing.x_root;
                xEvent->xcrossing.y = xEvent->xcrossing.y_root;
                xEvent->xcrossing.mode = NotifyNormal;
                // TODO: What is this?
                xEvent->xcrossing.detail = NotifyAncestor;
                /*
                 * NotifyAncestor, NotifyVirtual, NotifyInferior,
                 * NotifyNonlinear,NotifyNonlinearVirtual
                 */
                xEvent->xcrossing.same_screen = True;
                xEvent->xcrossing.focus = SDL_GetWindowFlags(SDL_GetWindowFromID(
                        sdlEvent->button.windowID)) & SDL_WINDOW_MOUSE_FOCUS;
                xEvent->xcrossing.state = convertModifierState(SDL_GetModState());
                break;
            }
        case SDL_MOUSEBUTTONUP:
            if (sdlEvent->type == SDL_MOUSEBUTTONUP) {
                LOG("SDL_MOUSEBUTTONUP\n");
                type = ButtonRelease;
            }
            FILL_STANDARD_VALUES(xbutton);
            xEvent->xbutton.root = getWindowFromId(sdlEvent->button.windowID);
            if (xEvent->xbutton.root == None) {
                xEvent->xbutton.root = SCREEN_WINDOW;
            }
            xEvent->xbutton.subwindow = getContainingWindow(xEvent->xbutton.root, sdlEvent->button.x, sdlEvent->button.y); // The event window is always the SDL Window.
            eventWindow = xEvent->xbutton.subwindow;
//            while (eventWindow != SCREEN_WINDOW && 0 && !HAS_VALUE(GET_WINDOW_STRUCT(eventWindow)->eventMask, ButtonPressMask)) {
//                eventWindow = GET_PARENT(eventWindow);
//            }
            xEvent->xbutton.window = eventWindow;
            xEvent->xbutton.time = sdlEvent->button.timestamp;
            xEvent->xbutton.x = sdlEvent->button.x;
            xEvent->xbutton.y = sdlEvent->button.y;
            xEvent->xbutton.x_root = xEvent->xbutton.x; // Because root and window are the same.
            xEvent->xbutton.y_root = xEvent->xbutton.y;
            xEvent->xbutton.state = convertModifierState(SDL_GetModState());
            if (sdlEvent->button.button == SDL_BUTTON_LEFT) {
                xEvent->xbutton.button = Button1;
            } else if (sdlEvent->button.button == SDL_BUTTON_MIDDLE) {
                xEvent->xbutton.button = Button2;
            } else if (sdlEvent->button.button == SDL_BUTTON_RIGHT) {
                xEvent->xbutton.button = Button3;
            } else if (sdlEvent->button.button == SDL_BUTTON_X1) {
                xEvent->xbutton.button = Button4;
            } else {
                xEvent->xbutton.button = Button5;
            }
            xEvent->xbutton.same_screen = True;
            break;
        case SDL_MOUSEMOTION:
            LOG("SDL_MOUSEMOTION\n");
            type = MotionNotify;
            FILL_STANDARD_VALUES(xmotion);
            xEvent->xmotion.root = getWindowFromId(sdlEvent->motion.windowID);
            if (xEvent->xbutton.root == None) {
                xEvent->xbutton.root = SCREEN_WINDOW;
            }
            eventWindow = getContainingWindow(xEvent->xbutton.root, sdlEvent->motion.x, sdlEvent->motion.y);
            xEvent->xmotion.window = eventWindow; // The event window is always the window the mouse is in.
            if (xEvent->xmotion.window == None) {
                xEvent->xmotion.window = SCREEN_WINDOW;
            }
            xEvent->xmotion.subwindow = None;
            xEvent->xmotion.time = sdlEvent->motion.timestamp;
            xEvent->xmotion.x = sdlEvent->motion.x;
            xEvent->xmotion.y = sdlEvent->motion.y;
            xEvent->xmotion.x_root = xEvent->xbutton.x; // Because root and window are the same.
            xEvent->xmotion.y_root = xEvent->xbutton.y;
            xEvent->xmotion.state = convertModifierState(SDL_GetModState());
            xEvent->xmotion.is_hint = NotifyNormal; // TODO: When is this a hint?
            xEvent->xmotion.same_screen = True;
            break;
        case SDL_WINDOWEVENT:
            eventWindow = getWindowFromId(sdlEvent->window.windowID);
            switch (sdlEvent->window.event) {
                case SDL_WINDOWEVENT_SHOWN:
                    LOG("Window %d shown\n", sdlEvent->window.windowID);
                    type = MapNotify;
                    FILL_STANDARD_VALUES(xmap);
                    xEvent->xmap.window = eventWindow;
                    xEvent->xmap.event = xEvent->xmap.window;
                    xEvent->xmap.override_redirect = False;
                    break;
                case SDL_WINDOWEVENT_HIDDEN:
                    LOG("Window %d hidden\n", sdlEvent->window.windowID);
                    type = UnmapNotify;
                    FILL_STANDARD_VALUES(xunmap);
                    xEvent->xunmap.window = eventWindow;
                    xEvent->xunmap.event = xEvent->xunmap.window;
                    xEvent->xunmap.from_configure = False;
                    break;
                case SDL_WINDOWEVENT_EXPOSED:
                    LOG("Window %d exposed\n", sdlEvent->window.windowID);
                    return -1;
                    break;
                case SDL_WINDOWEVENT_MOVED:
                    LOG("Window %d moved to %d,%d\n",
                        sdlEvent->window.windowID, sdlEvent->window.data1, sdlEvent->window.data2);
                case SDL_WINDOWEVENT_RESIZED:
                    if (sdlEvent->window.event == SDL_WINDOWEVENT_RESIZED) {
                        LOG("Window %d resized to %dx%d\n", sdlEvent->window.windowID,
                            sdlEvent->window.data1, sdlEvent->window.data2);
                    }
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    if (sdlEvent->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        LOG("Window %d size changed to %dx%d\n", sdlEvent->window.windowID,
                            sdlEvent->window.data1, sdlEvent->window.data2);
                    }
                    type = ConfigureNotify;
                    FILL_STANDARD_VALUES(xconfigure);
					xEvent->xconfigure.event = eventWindow;
					xEvent->xconfigure.window = xEvent->xconfigure.event;
					if (sdlEvent->window.event == SDL_WINDOWEVENT_MOVED) {
						xEvent->xconfigure.x = sdlEvent->window.data1;
						xEvent->xconfigure.y = sdlEvent->window.data2;
					} else {
						SDL_GetWindowPosition(SDL_GetWindowFromID(sdlEvent->window.windowID),
											  &xEvent->xconfigure.x, &xEvent->xconfigure.y);
					}
					if (sdlEvent->window.event == SDL_WINDOWEVENT_RESIZED
						|| sdlEvent->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
						xEvent->xconfigure.width  = sdlEvent->window.data1;
						xEvent->xconfigure.height = sdlEvent->window.data2;
					} else {
						SDL_GetWindowSize(SDL_GetWindowFromID(sdlEvent->window.windowID),
										  &xEvent->xconfigure.width, &xEvent->xconfigure.height);
					}
					xEvent->xconfigure.border_width = GET_WINDOW_STRUCT(eventWindow)->borderWidth;
					xEvent->xconfigure.above = None;
					xEvent->xconfigure.override_redirect = GET_WINDOW_STRUCT(eventWindow)->overrideRedirect;
					break;
				case SDL_WINDOWEVENT_MINIMIZED:
                    LOG("Window %d minimized\n", sdlEvent->window.windowID);
                    return -1;
                    break;
                case SDL_WINDOWEVENT_MAXIMIZED:
                    LOG("Window %d maximized\n", sdlEvent->window.windowID);
                    return -1;
                    break;
                case SDL_WINDOWEVENT_RESTORED:
                    LOG("Window %d restored\n", sdlEvent->window.windowID);
                    SDL_Rect windowArea = {0, 0, 0, 0};
                    GET_WINDOW_DIMS(eventWindow, windowArea.w, windowArea.h);
                    postExposeEvent(display, eventWindow, &windowArea, 1);
                    return -1;
                    break;
                case SDL_WINDOWEVENT_ENTER:
                    LOG("Mouse entered window %d\n", sdlEvent->window.windowID);
                    type = EnterNotify;
                case SDL_WINDOWEVENT_LEAVE:
                    if (sdlEvent->window.event == SDL_WINDOWEVENT_LEAVE) {
                        LOG("Mouse left window %d\n", sdlEvent->window.windowID);
                        type = LeaveNotify;
                    }
                    FILL_STANDARD_VALUES(xcrossing);
                    xEvent->xcrossing.root = eventWindow;
                    xEvent->xcrossing.window = xEvent->xcrossing.root;
                    // TODO: Check if pointer is in child
                    xEvent->xcrossing.subwindow = None;
                    xEvent->xcrossing.time = sdlEvent->window.timestamp;
                    SDL_GetMouseState(&xEvent->xcrossing.x_root, &xEvent->xcrossing.y_root);
                    xEvent->xcrossing.x = xEvent->xcrossing.x_root;
                    xEvent->xcrossing.y = xEvent->xcrossing.y_root;
                    xEvent->xcrossing.mode = NotifyNormal;
                    // TODO: What is this?
                    xEvent->xcrossing.detail = NotifyAncestor;
                    /*
                     * NotifyAncestor, NotifyVirtual, NotifyInferior,
                     * NotifyNonlinear,NotifyNonlinearVirtual
                     */
                    xEvent->xcrossing.same_screen = True;
                    xEvent->xcrossing.focus = SDL_GetWindowFlags(SDL_GetWindowFromID(
                            sdlEvent->window.windowID)) & SDL_WINDOW_MOUSE_FOCUS;
                    xEvent->xcrossing.state = convertModifierState(SDL_GetModState());
                    break;
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    LOG("Window %d gained keyboard focus\n", sdlEvent->window.windowID);
                    type = FocusIn;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    if (sdlEvent->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        LOG("Window %d lost keyboard focus\n", sdlEvent->window.windowID);
                        type = FocusOut;
                    }
                    FILL_STANDARD_VALUES(xfocus);
                    xEvent->xfocus.window = eventWindow;
                    xEvent->xfocus.mode = NotifyNormal;
                    xEvent->xfocus.detail = NotifyAncestor;
                    break;
                case SDL_WINDOWEVENT_CLOSE:
                    LOG("Window %d closed\n", sdlEvent->window.windowID);
                    static Atom WM_PROTOCOLS = None;
                    static Atom WM_DELETE_WINDOW = None;
                    if (WM_PROTOCOLS == None) {
                        WM_PROTOCOLS = internalInternAtom("WM_PROTOCOLS");
                        WM_DELETE_WINDOW = internalInternAtom("WM_DELETE_WINDOW");
                    }
                    WindowProperty *windowProperty = findProperty(&GET_WINDOW_STRUCT(eventWindow)->properties,
                                                                  WM_PROTOCOLS, NULL);
                    if (windowProperty != NULL && windowProperty->type == XA_ATOM) {
                        size_t i;
                        for (i = 0; i < windowProperty->dataLength; i++) {
                            if (((Atom*) windowProperty->data)[i] == WM_DELETE_WINDOW) {
                                postEvent(display, eventWindow, ClientMessage, 32,
                                          WM_PROTOCOLS, WM_DELETE_WINDOW);
                                break;
                            }
                        }
                    } // TODO: If window has not the atom, destroy it and Close Display if it is the last
                    return -1;
                    break;
                default:
                    LOG("Window %d got unknown event %d\n",
                        sdlEvent->window.windowID, sdlEvent->window.event);
                    return -1;
            }
            break;
        case SDL_QUIT:/**< User-requested quit */
            LOG("SDL_QUIT\n");
            return -1;
        case SDL_APP_TERMINATING: /**< The application is being terminated by the OS
                                     Called on iOS in applicationWillTerminate()
                                     Called on Android in onDestroy()
                                */
            LOG("SDL_APP_TERMINATING\n");
            return -1;
        case SDL_APP_LOWMEMORY: /**< The application is low on memory, free memory if possible.
                                     Called on iOS in applicationDidReceiveMemoryWarning()
                                     Called on Android in onLowMemory()
                                */
            LOG("SDL_APP_LOWMEMORY\n");
            return -1;
        case SDL_APP_WILLENTERBACKGROUND: /**< The application is about to enter the background
                                     Called on iOS in applicationWillResignActive()
                                     Called on Android in onPause()
                                */
            LOG("SDL_APP_WILLENTERBACKGROUND\n");
            return -1;
        case SDL_APP_DIDENTERBACKGROUND: /**< The application did enter the background and may not get CPU for some time
                                     Called on iOS in applicationDidEnterBackground()
                                     Called on Android in onPause()
                                */
            LOG("SDL_APP_DIDENTERBACKGROUND\n");
            return -1;
        case SDL_APP_WILLENTERFOREGROUND: /**< The application is about to enter the foreground
                                     Called on iOS in applicationWillEnterForeground()
                                     Called on Android in onResume()
                                */
            LOG("SDL_APP_WILLENTERFOREGROUND\n");
            return -1;
        case SDL_APP_DIDENTERFOREGROUND: /**< The application is now interactive
                                     Called on iOS in applicationDidBecomeActive()
                                     Called on Android in onResume()
                                */
            LOG("SDL_APP_DIDENTERFOREGROUND\n");
            return -1;
        case SDL_SYSWMEVENT: /**< System specific event */
            LOG("SDL_SYSWMEVENT\n");
            return -1;
        case SDL_TEXTEDITING:            /**< Keyboard text editing (composition) */
            LOG("SDL_TEXTEDITING\n");
            return -1;
        case SDL_TEXTINPUT:              /**< Keyboard text input */
            LOG("SDL_TEXTINPUT\n");
            inputMethodSetCurrentText(sdlEvent->text.text);
            // Send synthetic key down and up events with keycode = 0
            type = KeyPress;
            FILL_STANDARD_VALUES(xkey);
            xEvent->xkey.root = getWindowFromId(sdlEvent->text.windowID);
            eventWindow = getKeyboardFocus();
            eventWindow = eventWindow == None ? xEvent->xkey.root : eventWindow;
            xEvent->xkey.window = eventWindow;
            xEvent->xkey.subwindow = None;
            xEvent->xkey.time = sdlEvent->text.timestamp;
            SDL_GetMouseState(&xEvent->xkey.x, &xEvent->xkey.y);
            xEvent->xkey.x_root = xEvent->xkey.x; // Because root and window are the same.
            xEvent->xkey.y_root = xEvent->xkey.y;
            xEvent->xkey.state = convertModifierState(SDL_GetModState());
            xEvent->xkey.keycode = 0;
            xEvent->xkey.same_screen = True;
            ENQUEUE_EVENT_IN_PIPE(display);
            eventWaiting = True;
            waitingEvent.type = SDL_KEYUP;
            waitingEvent.key.windowID = sdlEvent->text.windowID;
            waitingEvent.key.timestamp = sdlEvent->text.timestamp;
            waitingEvent.key.keysym.mod = SDL_GetModState();
            // Because Xutf8LookupString is not called for KeyUp events, we need to fill in the keycode here.
            // TODO: This will not work for UTF-8 characters bigger than one byte 
            LOG("Enqueuing keyup for char %d = '%c'\n",
                sdlEvent->text.text[strlen(sdlEvent->text.text) - 1],
                sdlEvent->text.text[strlen(sdlEvent->text.text) - 1]);
            waitingEvent.key.keysym.sym = sdlEvent->text.text[strlen(sdlEvent->text.text) - 1];
            break;
        case SDL_MOUSEWHEEL:             /**< Mouse wheel motion */
            LOG("SDL_MOUSEWHEEL\n");
            return -1;
        case SDL_JOYAXISMOTION: /**< Joystick axis motion */
            LOG("SDL_JOYAXISMOTION\n");
            return -1;
        case SDL_JOYBALLMOTION:          /**< Joystick trackball motion */
            LOG("SDL_JOYBALLMOTION\n");
            return -1;
        case SDL_JOYHATMOTION:           /**< Joystick hat position change */
            LOG("SDL_JOYHATMOTION\n");
            return -1;
        case SDL_JOYBUTTONDOWN:          /**< Joystick button pressed */
            LOG("SDL_JOYBUTTONDOWN\n");
            return -1;
        case SDL_JOYBUTTONUP:            /**< Joystick button released */
            LOG("SDL_JOYBUTTONUP\n");
            return -1;
        case SDL_JOYDEVICEADDED:         /**< A new joystick has been inserted into the system */
            LOG("SDL_JOYDEVICEADDED\n");
            return -1;
        case SDL_JOYDEVICEREMOVED:       /**< An opened joystick has been removed */
            LOG("SDL_JOYDEVICEREMOVED\n");
            return -1;
        case SDL_CONTROLLERAXISMOTION: /**< Game controller axis motion */
            LOG("SDL_CONTROLLERAXISMOTION\n");
            return -1;
        case SDL_CONTROLLERBUTTONDOWN:          /**< Game controller button pressed */
            LOG("SDL_CONTROLLERBUTTONDOWN\n");
            return -1;
        case SDL_CONTROLLERBUTTONUP:            /**< Game controller button released */
            LOG("SDL_CONTROLLERBUTTONUP\n");
            return -1;
        case SDL_CONTROLLERDEVICEADDED:         /**< A new Game controller has been inserted into the system */
            LOG("SDL_CONTROLLERDEVICEADDED\n");
            return -1;
        case SDL_CONTROLLERDEVICEREMOVED:       /**< An opened Game controller has been removed */
            LOG("SDL_CONTROLLERDEVICEREMOVED\n");
            return -1;
        case SDL_CONTROLLERDEVICEREMAPPED:      /**< The controller mapping was updated */
            LOG("SDL_CONTROLLERDEVICEREMAPPED\n");
            return -1;
        case SDL_FINGERDOWN: // Should not happen
            LOG("FINGER BUTTONDOWN\n");
            return -1;
            type = ButtonPress;
        case SDL_FINGERUP: // Should not happen
            LOG("SDL_FINGERUP\n");
            return -1;
            if (sdlEvent->type == SDL_FINGERUP) {
                type = ButtonRelease;
            }
            FILL_STANDARD_VALUES(xbutton);
            eventWindow = SCREEN_WINDOW;
            xEvent->xbutton.root = eventWindow;
            xEvent->xbutton.window = xEvent->xbutton.root; // The event window is always the SDL Window.
            xEvent->xbutton.subwindow = None;
            xEvent->xbutton.time = sdlEvent->tfinger.timestamp;
            xEvent->xbutton.x = sdlEvent->tfinger.x;
            xEvent->xbutton.y = sdlEvent->tfinger.y;
            xEvent->xbutton.x_root = xEvent->xbutton.x; // Because root and window are the same.
            xEvent->xbutton.y_root = xEvent->xbutton.y;
            xEvent->xbutton.state = convertModifierState(SDL_GetModState());
            xEvent->xbutton.button = Button1;
            xEvent->xbutton.same_screen = True;
            break;
        case SDL_FINGERMOTION: // Should not happen
            LOG("SDL_FINGERMOTION\n");
            return -1;
            type = MotionNotify;
            FILL_STANDARD_VALUES(xmotion);
            eventWindow = SCREEN_WINDOW;
            xEvent->xmotion.root = eventWindow;
            xEvent->xmotion.window = xEvent->xmotion.root; // The event window is always the SDL Window.
            xEvent->xmotion.subwindow = None;
            xEvent->xmotion.time = sdlEvent->tfinger.timestamp;
            xEvent->xmotion.x = sdlEvent->tfinger.x;
            xEvent->xmotion.y = sdlEvent->tfinger.y;
            xEvent->xmotion.x_root = xEvent->xbutton.x; // Because root and window are the same.
            xEvent->xmotion.y_root = xEvent->xbutton.y;
            xEvent->xmotion.state = convertModifierState(SDL_GetModState());
            xEvent->xmotion.is_hint = NotifyNormal; // TODO: When is this a hint?
            xEvent->xmotion.same_screen = True;
            break;
        case SDL_DOLLARGESTURE:
            LOG("SDL_DOLLARGESTURE\n");
            return -1;
        case SDL_DOLLARRECORD:
            LOG("SDL_DOLLARRECORD\n");
            return -1;
        case SDL_MULTIGESTURE:
            LOG("SDL_MULTIGESTURE\n");
            return -1;
        case SDL_CLIPBOARDUPDATE: /**< The clipboard changed */
            LOG("SDL_CLIPBOARDUPDATE\n");
            return -1;
        case SDL_DROPFILE: /**< The system requests a file open */
            LOG("SDL_DROPFILE\n");
            return -1;
        case SDL_RENDER_TARGETS_RESET:
            LOG("SDL_RENDER_TARGETS_RESET\n");
            updateWindowRenderTargets(display);
            type = Expose;
            eventWindow = *GET_CHILDREN(SCREEN_WINDOW);
            FILL_STANDARD_VALUES(xexpose);
            xEvent->xexpose.window = eventWindow;
            GET_WINDOW_POS(eventWindow, xEvent->xexpose.x, xEvent->xexpose.y);
            GET_WINDOW_DIMS(eventWindow, xEvent->xexpose.width, xEvent->xexpose.height);
            xEvent->xexpose.count = 0;
            break;
        case SDL_RENDER_DEVICE_RESET: // TODO: This is WIP
            LOG("SDL_RENDER_DEVICE_RESET\n");
            updateWindowRenderTargets(display);
            type = Expose;
            eventWindow = *GET_CHILDREN(SCREEN_WINDOW);
            FILL_STANDARD_VALUES(xexpose);
            xEvent->xexpose.window = eventWindow;
            GET_WINDOW_POS(eventWindow, xEvent->xexpose.x, xEvent->xexpose.y);
            GET_WINDOW_DIMS(eventWindow, xEvent->xexpose.width, xEvent->xexpose.height);
            xEvent->xexpose.count = 0;
            break;
        default:
            if (sdlEvent->type >= SDL_USEREVENT && sdlEvent->type <= SDL_LASTEVENT) {
                if (sdlEvent->user.code == INTERNAL_EVENT_CODE) {
                    XAnyEvent* allocEvent = sdlEvent->user.data1;
                    eventWindow = (Window) sdlEvent->user.data2;
                    type = allocEvent->type;
                    sendEvent = allocEvent->send_event;
                    allocEvent->serial = lastEventSerial;
                    switch(type) {
                        case KeyRelease:
                        case KeyPress:
                            memcpy(&xEvent->xkey, allocEvent, sizeof(XKeyEvent)); break;
                        case ButtonPress:
                        case ButtonRelease:
                            memcpy(&xEvent->xbutton, allocEvent, sizeof(XButtonEvent)); break;
                        case MotionNotify:
                            memcpy(&xEvent->xmotion, allocEvent, sizeof(XMotionEvent)); break;
                        case EnterNotify:
                        case LeaveNotify:
                            memcpy(&xEvent->xcrossing, allocEvent, sizeof(XCrossingEvent)); break;
                        case FocusIn:
                        case FocusOut:
                            memcpy(&xEvent->xfocus, allocEvent, sizeof(XFocusChangeEvent)); break;
                        case Expose:
                            memcpy(&xEvent->xexpose, allocEvent, sizeof(XExposeEvent)); break;
                        case KeymapNotify:
                            memcpy(&xEvent->xkeymap, allocEvent, sizeof(XKeymapEvent)); break;
                        case GraphicsExpose:
                            memcpy(&xEvent->xgraphicsexpose, allocEvent, sizeof(XGraphicsExposeEvent)); break;
                        case NoExpose:
                            memcpy(&xEvent->xnoexpose, allocEvent, sizeof(XNoExposeEvent)); break;
                        case VisibilityNotify:
                            memcpy(&xEvent->xvisibility, allocEvent, sizeof(XVisibilityEvent)); break;
                        case CreateNotify:
                            memcpy(&xEvent->xcreatewindow, allocEvent, sizeof(XCreateWindowEvent)); break;
                        case DestroyNotify:
                            memcpy(&xEvent->xdestroywindow, allocEvent, sizeof(XDestroyWindowEvent)); break;
                        case UnmapNotify:
                            memcpy(&xEvent->xunmap, allocEvent, sizeof(XUnmapEvent)); break;
                        case MapNotify:
                            memcpy(&xEvent->xmap, allocEvent, sizeof(XMapEvent)); break;
                        case MapRequest:
                            memcpy(&xEvent->xmaprequest, allocEvent, sizeof(XMapRequestEvent)); break;
                        case ReparentNotify:
                            memcpy(&xEvent->xreparent, allocEvent, sizeof(XReparentEvent)); break;
                        case ConfigureNotify:
                            memcpy(&xEvent->xconfigure, allocEvent, sizeof(XConfigureEvent)); break;
                        case ConfigureRequest:
                            memcpy(&xEvent->xconfigurerequest, allocEvent, sizeof(XConfigureRequestEvent)); break;
                        case GravityNotify:
                            memcpy(&xEvent->xgravity, allocEvent, sizeof(XGravityEvent)); break;
                        case ResizeRequest:
                            memcpy(&xEvent->xresizerequest, allocEvent, sizeof(XResizeRequestEvent)); break;
                        case CirculateNotify:
                            memcpy(&xEvent->xcirculate, allocEvent, sizeof(XCirculateEvent)); break;
                        case CirculateRequest:
                            memcpy(&xEvent->xcirculaterequest, allocEvent, sizeof(XCirculateRequestEvent)); break;
                        case PropertyNotify:
                            memcpy(&xEvent->xproperty, allocEvent, sizeof(XPropertyEvent)); break;
                        case SelectionClear:
                            memcpy(&xEvent->xselectionclear, allocEvent, sizeof(XSelectionClearEvent)); break;
                        case SelectionRequest:
                            memcpy(&xEvent->xselectionrequest, allocEvent, sizeof(XSelectionRequestEvent)); break;
                        case SelectionNotify:
                            memcpy(&xEvent->xselection, allocEvent, sizeof(XSelectionEvent)); break;
                        case ColormapNotify:
                            memcpy(&xEvent->xcolormap, allocEvent, sizeof(XColormapEvent)); break;
                        case ClientMessage:
                            memcpy(&xEvent->xclient, allocEvent, sizeof(XClientMessageEvent)); break;
                        case MappingNotify:
                            memcpy(&xEvent->xmapping, allocEvent, sizeof(XMappingEvent)); break;
                        default: break;
                    }
                    free(allocEvent);
                    break;
                } else if (sdlEvent->user.code == SEND_EVENT_CODE) {
                    memcpy(xEvent, sdlEvent->user.data1, sizeof(XEvent));
                    free(sdlEvent->user.data1);
                    return 0;
                }
            }
            return -1;
    }
    FILL_STANDARD_VALUES(xany);
    xEvent->xany.window = eventWindow;
    xEvent->type = type;
    return 0;
#undef FILL_STANDARD_VALUES
}

void updateWindowRenderTargets(Display* display) {
    size_t i;
    LOG("Resetting window render targets\n");
    Window* children = GET_CHILDREN(SCREEN_WINDOW);
    for (i = 0; i < GET_WINDOW_STRUCT(SCREEN_WINDOW)->children.length; i++) {
        if (GET_WINDOW_STRUCT(children[i])->sdlWindow != NULL) {
            WindowStruct* windowStruct = GET_WINDOW_STRUCT(children[i]);
            LOG("Resetting render target of window %lu\n", children[i]);
			SDL_DestroyRenderer(windowStruct->sdlRenderer);
			windowStruct->sdlRenderer = SDL_CreateRenderer(windowStruct->sdlWindow, -1, 0);
            SDL_Rect exposeRect;
            exposeRect.x = 0;
            exposeRect.y = 0;
            GET_WINDOW_DIMS(children[i], exposeRect.w, exposeRect.h);
            postExposeEvent(display, children[i], &exposeRect, 1);
        }
    }
}

void printEventInfo(XEvent* event) {
    char* eventType;
    char msg[100];
#define CASE_TYPE(type, format, ...) case type: eventType = #type; snprintf(msg, 100, format, ##__VA_ARGS__); break
    switch (event->type) {
        CASE_TYPE(KeyPress, "");
        CASE_TYPE(KeyRelease, "");
        CASE_TYPE(ButtonPress, "");
        CASE_TYPE(ButtonRelease, "");
        CASE_TYPE(MotionNotify, "");
        CASE_TYPE(EnterNotify, "");
        CASE_TYPE(LeaveNotify, "");
        CASE_TYPE(FocusIn, "");
        CASE_TYPE(FocusOut, "");
        CASE_TYPE(KeymapNotify, "");
        CASE_TYPE(Expose, "window %lu, rect (x = %d, y = %d, %dx%d), count = %d",
                  event->xexpose.window, event->xexpose.x, event->xexpose.y,
                  event->xexpose.width, event->xexpose.height, event->xexpose.count);
        CASE_TYPE(GraphicsExpose, "");
        CASE_TYPE(NoExpose, "");
        CASE_TYPE(VisibilityNotify, "");
        CASE_TYPE(CreateNotify, "");
        CASE_TYPE(DestroyNotify, "");
        CASE_TYPE(UnmapNotify, "");
        CASE_TYPE(MapNotify, "");
        CASE_TYPE(MapRequest, "");
        CASE_TYPE(ReparentNotify, "");
        CASE_TYPE(ConfigureNotify, "window %lu (x = %d, y = %d, w = %d, h = %d), borderW = %d, above %lu, overrideRedirect = %d",
                  event->xconfigure.window, event->xconfigure.x, event->xconfigure.y, event->xconfigure.width,
                  event->xconfigure.height, event->xconfigure.border_width, event->xconfigure.above,
                  event->xconfigure.override_redirect);
        CASE_TYPE(ConfigureRequest, "");
        CASE_TYPE(GravityNotify, "");
        CASE_TYPE(ResizeRequest, "");
        CASE_TYPE(CirculateNotify, "");
        CASE_TYPE(CirculateRequest, "");
        CASE_TYPE(PropertyNotify, "");
        CASE_TYPE(SelectionClear, "");
        CASE_TYPE(SelectionRequest, "");
        CASE_TYPE(SelectionNotify, "");
        CASE_TYPE(ColormapNotify, "");
        CASE_TYPE(ClientMessage, "");
        CASE_TYPE(MappingNotify, "");
        default:
            eventType = "Unknown";
    }
#undef CASE_TYPE
    LOG("Got %s event\n", eventType);
    LOG("%s\n", msg);
}

int XNextEvent(Display* display, XEvent* event_return) {
    // https://tronche.com/gui/x/xlib/event-handling/manipulating-event-queue/XNextEvent.html
    SDL_Event event;
    Bool done = False;
    while (!done) {
        int qlen;
        getEventQueueLength(&qlen);
        LOG("Events in queue = %d, qlen = %d\n", qlen, GET_DISPLAY(display)->qlen);
        if (qlen == 0 && GET_DISPLAY(display)->qlen > 0 && !eventWaiting) {
            READ_EVENT_IN_PIPE(display);
            LOG("PREVENTING HANGING!!!\n");
            event_return->type = Expose;
            event_return->xany.serial = lastEventSerial;
            event_return->xany.display = display;
            event_return->xany.send_event = False;
            event_return->xany.type = Expose;
            event_return->xany.window = *GET_CHILDREN(GET_DISPLAY(display)->screens[0].root);
            event_return->xexpose.type = Expose;
            event_return->xexpose.serial = 0;
            event_return->xexpose.send_event = False;
            event_return->xexpose.display = display;
            event_return->xexpose.window = event_return->xany.window;
            event_return->xexpose.x = 0;
            event_return->xexpose.y = 0;
            event_return->xexpose.width = 0;
            event_return->xexpose.height = 0;
            event_return->xexpose.count = 0;
            break;
        }
        if (eventWaiting || SDL_WaitEvent(&event) == 1) {
            tmpVar = False;
            if (eventWaiting) {
                event = waitingEvent;
                eventWaiting = False;
                tmpVar = True;
            }
            // Clear the event from the pipe;
            READ_EVENT_IN_PIPE(display);
            if (convertEvent(display, &event, event_return) == 0) {
                printEventInfo(event_return);
                done = True;
            } else {
//                #ifdef DEBUG_WINDOWS
//                printWindowsHierarchy();
//                #endif
                LOG("Got unknown SDL event %d!\n", event.type);
                event_return->type = Expose;
                event_return->xany.serial = lastEventSerial;
                event_return->xany.display = display;
                event_return->xany.send_event = False;
                event_return->xany.type = Expose;
                event_return->xany.window = *GET_CHILDREN(GET_DISPLAY(display)->screens[0].root);
                event_return->xexpose.type = Expose;
                event_return->xexpose.serial = 0;
                event_return->xexpose.send_event = False;
                event_return->xexpose.display = display;
                event_return->xexpose.window = event_return->xany.window;
                event_return->xexpose.x = 0;
                event_return->xexpose.y = 0;
                event_return->xexpose.width = 0;
                event_return->xexpose.height = 0;
                event_return->xexpose.count = 0;
                done = True;
            }
            lastEventSerial++;
            tmpVar = False;
        } else {
            LOG("SDL_WaitEvent failed: %s, retrying...\n", SDL_GetError());
        }
        fflush(stderr);
    }
    LOG("Leaving XNextEvent\n");
    return 0;
}

Bool enqueueEvent(Display* display, Window eventWindow, void* event) {
    static Uint32 sendEventType = (Uint32) -1;
    if (sendEventType == ((Uint32) -1)) {
        sendEventType = SDL_RegisterEvents(1);
    }
    if (sendEventType != ((Uint32) -1)) {
        SDL_Event sdlEvent;
        SDL_zero(sdlEvent);
        sdlEvent.type = sendEventType;
        sdlEvent.user.code = INTERNAL_EVENT_CODE;
        sdlEvent.user.data1 = event;
        sdlEvent.user.data2 = (void *) eventWindow;
        LOG("Enqueuing event\n");
        SDL_PushEvent(&sdlEvent);
        return True;
    }
    LOG("Failed to send event: SDL_RegisterEvents failed!");
    return False;
}

Status XSendEvent(Display* display, Window window, Bool propagate, long event_mask, XEvent* event_send) {
    // https://tronche.com/gui/x/xlib/event-handling/XSendEvent.html
    SET_X_SERVER_REQUEST(display, X_SendEvent);
    // TODO: propagate, event_mask
    //  We have to assume that window is our window
    TYPE_CHECK(window, WINDOW, display, 0);
    event_send->xany.send_event = True;
    event_send->xany.serial = lastEventSerial;
    lastEventSerial++;
    static Uint32 sendEventType = (Uint32) -1;
    if (sendEventType == ((Uint32) -1)) {
        sendEventType = SDL_RegisterEvents(1);
    }
    if (sendEventType != ((Uint32) -1)) {
        XEvent* copy = malloc(sizeof(XEvent));
        if (copy == NULL) {
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        memcpy(copy, event_send, sizeof(XEvent));
        SDL_Event sdlEvent;
        SDL_zero(sdlEvent);
        sdlEvent.type = sendEventType;
        sdlEvent.user.code = SEND_EVENT_CODE;
        sdlEvent.user.data1 = copy;
        LOG("SEND event\n");
        SDL_PushEvent(&sdlEvent);
        return 1;
    }
    return 0;
}

Bool XFilterEvent(XEvent *event, Window w) {
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XFilterEvent.3.xhtml
    // We don't get an event from sdl, if an IM gets it before us.
    return False;
}

int XEventsQueued(Display *display, int mode) {
    // https://tronche.com/gui/x/xlib/event-handling/XEventsQueued.html
//    SET_X_SERVER_REQUEST(display, XCB_);
    if (GET_DISPLAY(display)->qlen == 0 && mode != QueuedAlready) {
        SDL_PumpEvents();
    }
    return GET_DISPLAY(display)->qlen;
}

int XFlush(Display *display) {
    // https://tronche.com/gui/x/xlib/event-handling/XFlush.html
//    SET_X_SERVER_REQUEST(display, XCB_);
//    SDL_PumpEvents(); // TODO: This locks up the main thread
    return 1;
}

Window getSiblingBelow(Window window) {
    Window parent = GET_PARENT(window);
    Window* children = GET_CHILDREN(parent);
    size_t i;
    for (i = 0; i < GET_WINDOW_STRUCT(parent)->children.length; i++) {
        if (children[i] == window && i + 1 < GET_WINDOW_STRUCT(parent)->children.length) {
            return children[i + 1];
        }
    }
    return None;
}

Bool postEvent(Display* display, Window eventWindow, unsigned int eventId, ...) {
#define SKIP {eventNeeded = False; break;}
    void* eventData = NULL;
    Bool eventNeeded = True;
    va_list args;
    va_start(args, eventId);
    switch (eventId) {
        case CreateNotify: {
            if (!HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask)) SKIP
            XCreateWindowEvent* event = malloc(sizeof(XCreateWindowEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            WindowStruct *windowStruct = GET_WINDOW_STRUCT(eventWindow);
            event->parent = windowStruct->parent;
            event->window = eventWindow;
            event->x = windowStruct->x;
            event->y = windowStruct->y;
            event->width = windowStruct->w;
            event->height = windowStruct->h;
            event->border_width = windowStruct->borderWidth;
            event->override_redirect = windowStruct->overrideRedirect;
            eventData = event;
            break;
        }
        case DestroyNotify: {
            if (!(HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask) ||
                  HAS_EVENT_MASK(eventWindow, StructureNotifyMask))) SKIP
            XDestroyWindowEvent* event = malloc(sizeof(XDestroyWindowEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            event->window = eventWindow;
            if (HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask)) {
                // Enqueue the event for the parent first
                event->event = GET_PARENT(eventWindow);
                if (!enqueueEvent(display, GET_PARENT(eventWindow), event)) {
                    break; // Break out and return False
                }
            }
            if (HAS_EVENT_MASK(eventWindow, StructureNotifyMask)) {
                // Now enqueue the event for the destroyed window
                event->event = eventWindow;
            } else SKIP
            eventData = event;
            break;
        }
        case Expose: {
            if (!HAS_EVENT_MASK(eventWindow, ExposureMask) || IS_INPUT_ONLY(eventWindow)
                || GET_WINDOW_STRUCT(eventWindow)->mapState != Mapped) SKIP
            XExposeEvent* event = malloc(sizeof(XExposeEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            event->window = eventWindow;
            SDL_Rect* exposeRect = va_arg(args, SDL_Rect*);
            event->x = exposeRect->x;
            event->y = exposeRect->y;
            event->width = exposeRect->w;
            event->height = exposeRect->h;
            event->count = va_arg(args, size_t);
            eventData = event;
            break;
        }
        case ConfigureRequest: {
            if (GET_WINDOW_STRUCT(eventWindow)->overrideRedirect
                || !HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureRedirectMask)) SKIP
            XConfigureRequestEvent* event = malloc(sizeof(XConfigureRequestEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            event->window = eventWindow;
            event->parent = GET_PARENT(eventWindow);
            event->value_mask = va_arg(args, unsigned long);
            XWindowChanges *windowChanges = va_arg(args, XWindowChanges*);
            GET_WINDOW_POS(eventWindow, event->x, event->y);
            if (HAS_VALUE(event->value_mask, CWX)) {
                event->x = windowChanges->x;
            }
            if (HAS_VALUE(event->value_mask, CWY)) {
                event->y = windowChanges->y;
            }
            GET_WINDOW_DIMS(eventWindow, event->width, event->height);
            if (HAS_VALUE(event->value_mask, CWWidth)) {
                event->width = windowChanges->width;
            }
            if (HAS_VALUE(event->value_mask, CWHeight)) {
                event->height = windowChanges->height;
            }
            if (HAS_VALUE(event->value_mask, CWBorderWidth)) {
                event->border_width = windowChanges->border_width;
            } else {
                event->border_width = GET_WINDOW_STRUCT(eventWindow)->borderWidth;
            }
            event->above = HAS_VALUE(event->value_mask, CWSibling)
                                           ? windowChanges->sibling : None;
            event->detail = HAS_VALUE(event->value_mask, CWStackMode)
                                            ? windowChanges->stack_mode : Above;
            eventData = event;
            break;
        }
        case ConfigureNotify: {
            if (!(HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask) ||
                  HAS_EVENT_MASK(eventWindow, StructureNotifyMask))) SKIP
            XConfigureEvent* event = malloc(sizeof(XConfigureEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            event->window = eventWindow;
            GET_WINDOW_POS(eventWindow, event->x, event->y);
            GET_WINDOW_DIMS(eventWindow, event->width, event->height);
            event->border_width = GET_WINDOW_STRUCT(eventWindow)->borderWidth;
            event->above = getSiblingBelow(eventWindow);
            event->override_redirect = GET_WINDOW_STRUCT(eventWindow)->overrideRedirect;
            if (HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask)) {
                // Enqueue the event for the parent first
                event->event = GET_PARENT(eventWindow);
                if (!enqueueEvent(display, GET_PARENT(eventWindow), event)) {
                    break; // Break out and return False
                }
            }
            if (HAS_EVENT_MASK(eventWindow, StructureNotifyMask)) {
                event->event = eventWindow;
            } else SKIP
            eventData = event;
            break;
        }
        case ReparentNotify: {
            Window oldParent = va_arg(args, Window);
            if (!(HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask) ||
                  HAS_EVENT_MASK(oldParent, SubstructureNotifyMask) ||
                  HAS_EVENT_MASK(eventWindow, StructureNotifyMask))) SKIP
            XReparentEvent* event = malloc(sizeof(XReparentEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            event->window = eventWindow;
            event->parent = GET_PARENT(eventWindow);
            GET_WINDOW_POS(eventWindow, event->x, event->y);
            event->override_redirect = GET_WINDOW_STRUCT(eventWindow)->overrideRedirect;
            if (HAS_EVENT_MASK(oldParent, SubstructureNotifyMask)) {
                // Enqueue the event for the old parent first
                event->event = oldParent;
                if (!enqueueEvent(display, oldParent, event)) {
                    break; // Break out and return False
                }
            }
            if (HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask)) {
                // Enqueue the event for the parent second
                event->event = GET_PARENT(eventWindow);
                if (!enqueueEvent(display, GET_PARENT(eventWindow), event)) {
                    break; // Break out and return False
                }
            }
            if (HAS_EVENT_MASK(eventWindow, StructureNotifyMask)) {
                event->event = eventWindow;
            } else SKIP
            eventData = event;
            break;
        }
        case MapRequest: {
            if (!HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureRedirectMask)
                || GET_WINDOW_STRUCT(eventWindow)->overrideRedirect
                || GET_WINDOW_STRUCT(eventWindow)->mapState != UnMapped) SKIP
            XMapRequestEvent* event = malloc(sizeof(XMapRequestEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            event->window = eventWindow;
            event->parent = GET_PARENT(eventWindow);
            eventData = event;
            break;
        }
        case MapNotify: {
            if (!HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask)
                && !HAS_EVENT_MASK(eventWindow, StructureNotifyMask)) SKIP
            XMapEvent* event = malloc(sizeof(XMapEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            event->window = eventWindow;
            event->override_redirect = GET_WINDOW_STRUCT(eventWindow)->overrideRedirect;
            if (HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask)) {
                // Enqueue the event for the parent second
                event->event = GET_PARENT(eventWindow);
                if (!enqueueEvent(display, GET_PARENT(eventWindow), event)) {
                    break; // Break out and return False
                }
            }
            if (HAS_EVENT_MASK(eventWindow, StructureNotifyMask)) {
                event->event = eventWindow;
            } else SKIP
            eventData = event;
            break;
        }
        case UnmapNotify: {
            if (!HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask)
                && !HAS_EVENT_MASK(eventWindow, StructureNotifyMask)) SKIP
            XUnmapEvent* event = malloc(sizeof(XUnmapEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            event->window = eventWindow;
            event->from_configure = va_arg(args, Bool);
            if (HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask)) {
                // Enqueue the event for the parent second
                event->event = GET_PARENT(eventWindow);
                if (!enqueueEvent(display, GET_PARENT(eventWindow), event)) {
                    break; // Break out and return False
                }
            }
            if (HAS_EVENT_MASK(eventWindow, StructureNotifyMask)) {
                event->event = eventWindow;
            } else SKIP
            eventData = event;
            break;
        }
        case ClientMessage: {
            XClientMessageEvent* event = malloc(sizeof(XClientMessageEvent));
            if (event == NULL) break;
            event->type = eventId;
            event->send_event = False;
            event->display = display;
            event->window = eventWindow;
            event->format = va_arg(args, int);
            event->message_type = va_arg(args, Atom);
            event->data.l[0] = va_arg(args, Atom);
            eventData = event;
            break;
        }
        case KeyRelease:
        case KeyPress:
//            memcpy(&xEvent->xkey, allocEvent, sizeof(XKeyEvent)); break;
        case ButtonPress:
        case ButtonRelease:
//            memcpy(&xEvent->xbutton, allocEvent, sizeof(XButtonEvent)); break;
        case MotionNotify:
//            memcpy(&xEvent->xmotion, allocEvent, sizeof(XMotionEvent)); break;
        case EnterNotify:
        case LeaveNotify:
//            memcpy(&xEvent->xcrossing, allocEvent, sizeof(XCrossingEvent)); break;
        case FocusIn:
        case FocusOut:
//            memcpy(&xEvent->xfocus, allocEvent, sizeof(XFocusChangeEvent)); break;
        case KeymapNotify:
//            memcpy(&xEvent->xexpose, allocEvent, sizeof(XExposeEvent)); break;
        case GraphicsExpose:
//            memcpy(&xEvent->xgraphicsexpose, allocEvent, sizeof(XGraphicsExposeEvent)); break;
        case NoExpose:
            /*memcpy(&xEvent->xnoexpose, allocEvent, sizeof(XNoExposeEvent)); */break; // TODO
        case VisibilityNotify:
//            memcpy(&xEvent->xvisibility, allocEvent, sizeof(XVisibilityEvent)); break;
        case GravityNotify:
            /*memcpy(&xEvent->xgravity, allocEvent, sizeof(XGravityEvent)); */break; // TODO
        case ResizeRequest:
//            memcpy(&xEvent->xresizerequest, allocEvent, sizeof(XResizeRequestEvent)); break;
        case CirculateNotify:
//            memcpy(&xEvent->xcirculate, allocEvent, sizeof(XCirculateEvent)); break;
        case CirculateRequest:
//            memcpy(&xEvent->xconfigurerequest, allocEvent, sizeof(XConfigureRequestEvent)); break;
        case PropertyNotify:
//            memcpy(&xEvent->xproperty, allocEvent, sizeof(XPropertyEvent)); break;
        case SelectionClear:
//            memcpy(&xEvent->xselectionclear, allocEvent, sizeof(XSelectionClearEvent)); break;
        case SelectionRequest:
//            memcpy(&xEvent->xselectionrequest, allocEvent, sizeof(XSelectionRequestEvent)); break;
        case SelectionNotify:
//            memcpy(&xEvent->xselection, allocEvent, sizeof(XSelectionEvent)); break;
        case ColormapNotify:
            /*memcpy(&xEvent->xcolormap, allocEvent, sizeof(XColormapEvent)); */break; // TODO
        case MappingNotify:
//            memcpy(&xEvent->xmapping, allocEvent, sizeof(XMappingEvent)); break;
        default:
            break;
    }
    va_end(args);
    if (eventData == NULL) return !eventNeeded;
    return enqueueEvent(display, eventWindow, eventData);
#undef SKIP
}

#undef ENQUEUE_EVENT_IN_PIPE
#undef READ_EVENT_IN_PIPE
