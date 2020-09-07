/*
 * Copyright © 2013-2017 Red Hat, Inc.
 * Copyright © 2020 Povilas Kanapickas <povilas@radix.lt>
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <xorg-server.h>
#include <list.h>
#include <exevents.h>
#include <inputstr.h>
#include <xkbsrv.h>
#include <xf86.h>
#include <xf86Xinput.h>
#include <xf86_OSproc.h>
#include <xserver-properties.h>
#include <os.h>

#include <linux/input.h>
#include <sys/stat.h>

#include <X11/Xatom.h>

#include "emulated-events.h"

#define TOUCHPAD_NUM_AXES 4 /* x, y, hscroll, vscroll */
#define TABLET_NUM_BUTTONS 7 /* we need scroll buttons */
#define TOUCH_MAX_SLOTS 15

/*
   libinput does not provide axis information for absolute devices, instead
   it scales into the screen dimensions provided. So we set up the axes with
   a fixed range, let libinput scale into that range and then the server
   do the scaling it usually does.
 */
#define TOUCH_AXIS_MAX 0xffff
#define TABLET_AXIS_MAX 0xffffff
#define TABLET_PRESSURE_AXIS_MAX 2047
#define TABLET_TILT_AXIS_MAX 64
#define TABLET_STRIP_AXIS_MAX 4096
#define TABLET_RING_AXIS_MAX 71

#define TYPE_KEYBOARD 1
#define TYPE_POINTER 2
#define TYPE_POINTER_GESTURE 3
#define TYPE_POINTER_ABS 4
#define TYPE_POINTER_ABS_PROXIMITY 5
#define TYPE_TOUCH 6

typedef struct {
    InputInfoPtr pInfo;

    int pipe_write_fd;
    int pipe_read_fd;

    int events_in_fd;
    int events_out_fd;

    char* events_in_path;
    char* events_out_path;

    EmulatedEvent* events_buffer;
    int events_buffer_size;

    uint32_t device_type;

    int waiting_for_drain;

    ValuatorMask *valuators;
    ValuatorMask *valuators_unaccelerated;
} EmulatedDevice, *EmulatedDevicePtr;

static void
xf86emulated_read_input_from_test(InputInfoPtr pInfo);

static Bool
xf86emulated_input_drain_write(ClientPtr ptr, void* closure)
{
    int fd = (int)(intptr_t) closure;
    char send_data = EMULATED_SYNC_RESPONSE;
    input_lock();
    /*  we don't really care whether the write succeeds. It may fail in the case if the device is
        already shut down and the descriptor is closed.
    */
    if (write(fd, &send_data, 1) < 0) {
        LogMessageVerbSigSafe(X_ERROR, 0, "emulated: Could not write sync byte: %s\n",
                              strerror(errno));
    }
    input_unlock();
    return TRUE;
}

static void
xf86emulated_input_drain_callback(void* data)
{
    void* send_data;
    InputInfoPtr pInfo = data;
    EmulatedDevicePtr driver_data = pInfo->private;


    if (driver_data->waiting_for_drain) {
        driver_data->waiting_for_drain = 0;
        send_data = (void*)(intptr_t) driver_data->events_out_fd;
        /* We must wait until input processing is done, because in some cases the events are
           added to the queue manually. Only after input lock is unlocked we can be sure that
           the processing of that parcicular event set has been finished.

           To make sure input lock is unlocked we must lock it ourselves from a separate task.
         */
        xf86IDrvMsg(pInfo, X_DEBUG, "Synchronization finished\n");
        QueueWorkProc(xf86emulated_input_drain_write, NULL, send_data);
    }
}

static void xf86emulated_got_data_to_read(int fd, int ready, void* data)
{
    DeviceIntPtr dev = (DeviceIntPtr) data;
    InputInfoPtr pInfo = dev->public.devicePrivate;

    xf86emulated_read_input_from_test(pInfo);
}

static int
xf86emulated_on(DeviceIntPtr dev)
{
    InputInfoPtr pInfo;
    EmulatedDevicePtr driver_data;

    pInfo = dev->public.devicePrivate;
    driver_data = pInfo->private;

    xf86IDrvMsg(pInfo, X_DEBUG, "Device turned on\n");

    xf86AddInputEventDrainCallback(xf86emulated_input_drain_callback, pInfo);
    SetNotifyFd(driver_data->events_in_fd, xf86emulated_got_data_to_read, X_NOTIFY_READ, dev);

    xf86AddEnabledDevice(pInfo);
    dev->public.on = TRUE;

    return Success;
}

static int
xf86emulated_off(DeviceIntPtr dev)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    EmulatedDevicePtr driver_data = pInfo->private;

    xf86IDrvMsg(pInfo, X_DEBUG, "Device turned off\n");

    if (dev->public.on) {
        RemoveNotifyFd(driver_data->events_in_fd);
        xf86RemoveEnabledDevice(pInfo);
        xf86RemoveInputEventDrainCallback(xf86emulated_input_drain_callback, pInfo);
    }
    dev->public.on = FALSE;
    return Success;
}

static void
xf86emulated_ptr_ctl(DeviceIntPtr dev, PtrCtrl *ctl)
{
}

static void
init_button_map(unsigned char *btnmap, size_t size)
{
    int i;

    memset(btnmap, 0, size);
    for (i = 0; i < size; i++)
        btnmap[i] = i;
}

static void
init_button_labels(Atom *labels, size_t size)
{
    assert(size > 10);

    memset(labels, 0, size * sizeof(Atom));
    labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
    labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
    labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
    labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
    labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
    labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
    labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
    labels[7] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_SIDE);
    labels[8] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_EXTRA);
    labels[9] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_FORWARD);
    labels[10] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_BACK);
}

static void
init_axis_labels(Atom *labels, size_t size)
{
    memset(labels, 0, size * sizeof(Atom));
    labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
    labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);
    labels[2] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_HSCROLL);
    labels[3] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_VSCROLL);
}

static void
xf86emulated_init_pointer(InputInfoPtr pInfo)
{
    DeviceIntPtr dev= pInfo->dev;
    int min, max, res;
    int nbuttons = 7;

    unsigned char btnmap[MAX_BUTTONS + 1];
    Atom btnlabels[MAX_BUTTONS];
    Atom axislabels[TOUCHPAD_NUM_AXES];

    nbuttons = xf86SetIntOption(pInfo->options, "PointerButtonCount", 7);

    init_button_map(btnmap, ARRAY_SIZE(btnmap));
    init_button_labels(btnlabels, ARRAY_SIZE(btnlabels));
    init_axis_labels(axislabels, ARRAY_SIZE(axislabels));

    InitPointerDeviceStruct((DevicePtr)dev,
                            btnmap,
                            nbuttons,
                            btnlabels,
                            xf86emulated_ptr_ctl,
                            GetMotionHistorySize(),
                            TOUCHPAD_NUM_AXES,
                            axislabels);
    min = -1;
    max = -1;
    res = 0;

    xf86InitValuatorAxisStruct(dev, 0, XIGetKnownProperty(AXIS_LABEL_PROP_REL_X),
                               min, max, res * 1000, 0, res * 1000, Relative);
    xf86InitValuatorAxisStruct(dev, 1, XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y),
                               min, max, res * 1000, 0, res * 1000, Relative);

    SetScrollValuator(dev, 2, SCROLL_TYPE_HORIZONTAL, 15, 0);
    SetScrollValuator(dev, 3, SCROLL_TYPE_VERTICAL, 15, 0);
}

static void
xf86emulated_init_pointer_absolute(InputInfoPtr pInfo, Bool proximity)
{
    DeviceIntPtr dev= pInfo->dev;
    int min, max, res;
    int nbuttons = 7;

    unsigned char btnmap[MAX_BUTTONS + 1];
    Atom btnlabels[MAX_BUTTONS];
    Atom axislabels[TOUCHPAD_NUM_AXES];

    nbuttons = xf86SetIntOption(pInfo->options, "PointerButtonCount", 7);

    init_button_map(btnmap, ARRAY_SIZE(btnmap));
    init_button_labels(btnlabels, ARRAY_SIZE(btnlabels));
    init_axis_labels(axislabels, ARRAY_SIZE(axislabels));

    InitPointerDeviceStruct((DevicePtr)dev,
                            btnmap,
                            nbuttons,
                            btnlabels,
                            xf86emulated_ptr_ctl,
                            GetMotionHistorySize(),
                            TOUCHPAD_NUM_AXES,
                            axislabels);
    min = 0;
    max = TOUCH_AXIS_MAX;
    res = 0;

    xf86InitValuatorAxisStruct(dev, 0, XIGetKnownProperty(AXIS_LABEL_PROP_ABS_X),
                               min, max, res * 1000, 0, res * 1000, Absolute);
    xf86InitValuatorAxisStruct(dev, 1, XIGetKnownProperty(AXIS_LABEL_PROP_ABS_Y),
                               min, max, res * 1000, 0, res * 1000, Absolute);

    SetScrollValuator(dev, 2, SCROLL_TYPE_HORIZONTAL, 15, 0);
    SetScrollValuator(dev, 3, SCROLL_TYPE_VERTICAL, 15, 0);

    if (proximity)
        InitProximityClassDeviceStruct(dev);
}

static void
xf86emulated_init_keyboard(InputInfoPtr pInfo)
{
    DeviceIntPtr dev= pInfo->dev;
    XkbRMLVOSet rmlvo = {0};
    XkbRMLVOSet defaults = {0};

    XkbGetRulesDflts(&defaults);

    rmlvo.rules = xf86SetStrOption(pInfo->options, "xkb_rules", defaults.rules);
    rmlvo.model = xf86SetStrOption(pInfo->options, "xkb_model", defaults.model);
    rmlvo.layout = xf86SetStrOption(pInfo->options, "xkb_layout", defaults.layout);
    rmlvo.variant = xf86SetStrOption(pInfo->options, "xkb_variant", defaults.variant);
    rmlvo.options = xf86SetStrOption(pInfo->options, "xkb_options", defaults.options);

    InitKeyboardDeviceStruct(dev, &rmlvo, NULL, NULL);
    XkbFreeRMLVOSet(&rmlvo, FALSE);
    XkbFreeRMLVOSet(&defaults, FALSE);
}

static void
xf86emulated_init_touch(InputInfoPtr pInfo)
{
    DeviceIntPtr dev = pInfo->dev;
    EmulatedDevicePtr driver_data = pInfo->private;
    int min, max, res;
    unsigned char btnmap[MAX_BUTTONS + 1];
    Atom btnlabels[MAX_BUTTONS];
    Atom axislabels[TOUCHPAD_NUM_AXES];
    int nbuttons = 7;
    int ntouches = TOUCH_MAX_SLOTS;

    init_button_map(btnmap, ARRAY_SIZE(btnmap));
    init_button_labels(btnlabels, ARRAY_SIZE(btnlabels));
    init_axis_labels(axislabels, ARRAY_SIZE(axislabels));

    InitPointerDeviceStruct((DevicePtr)dev,
                            btnmap,
                            nbuttons,
                            btnlabels,
                            xf86emulated_ptr_ctl,
                            GetMotionHistorySize(),
                            TOUCHPAD_NUM_AXES,
                            axislabels);
    min = 0;
    max = TOUCH_AXIS_MAX;
    res = 0;

    xf86InitValuatorAxisStruct(dev, 0,
                               XIGetKnownProperty(AXIS_LABEL_PROP_ABS_MT_POSITION_X),
                               min, max, res * 1000, 0, res * 1000, Absolute);
    xf86InitValuatorAxisStruct(dev, 1,
                               XIGetKnownProperty(AXIS_LABEL_PROP_ABS_MT_POSITION_Y),
                               min, max, res * 1000, 0, res * 1000, Absolute);
    xf86InitValuatorAxisStruct(dev, 2,
                               XIGetKnownProperty(AXIS_LABEL_PROP_ABS_MT_PRESSURE),
                               min, TABLET_PRESSURE_AXIS_MAX, res * 1000, 0, res * 1000, Absolute);

    ntouches = xf86SetIntOption(pInfo->options, "TouchCount", TOUCH_MAX_SLOTS);
    if (ntouches == 0) /* unknown */
        ntouches = TOUCH_MAX_SLOTS;
    InitTouchClassDeviceStruct(dev, ntouches, XIDirectTouch, 2);
}

static void
xf86emulated_init_gesture(InputInfoPtr pInfo)
{
    DeviceIntPtr dev = pInfo->dev;
    int ntouches = TOUCH_MAX_SLOTS;
    InitGestureClassDeviceStruct(dev, ntouches);
}

static void
xf86emulated_init(DeviceIntPtr dev)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    EmulatedDevicePtr driver_data = pInfo->private;

    dev->public.on = FALSE;

    if (driver_data->device_type == TYPE_KEYBOARD) {
        xf86emulated_init_keyboard(pInfo);
    } else if (driver_data->device_type == TYPE_POINTER) {
        xf86emulated_init_pointer(pInfo);
    } else if (driver_data->device_type == TYPE_POINTER_GESTURE) {
        xf86emulated_init_pointer(pInfo);
        xf86emulated_init_gesture(pInfo);
    } else if (driver_data->device_type == TYPE_POINTER_ABS) {
        xf86emulated_init_pointer_absolute(pInfo, FALSE);
    } else if (driver_data->device_type == TYPE_POINTER_ABS_PROXIMITY) {
        xf86emulated_init_pointer_absolute(pInfo, TRUE);
    } else if (driver_data->device_type == TYPE_TOUCH) {
        xf86emulated_init_touch(pInfo);
    }
}

static void
xf86emulated_destroy(DeviceIntPtr dev)
{
    InputInfoPtr pInfo = dev->public.devicePrivate;
    xf86IDrvMsg(pInfo, X_INFO, "Close\n");
}

static int
xf86emulated_device_control(DeviceIntPtr dev, int mode)
{
    switch(mode) {
        case DEVICE_INIT:
            xf86emulated_init(dev);
            break;
        case DEVICE_ON:
            xf86emulated_on(dev);
            break;
        case DEVICE_OFF:
            xf86emulated_off(dev);
            break;
        case DEVICE_CLOSE:
            xf86emulated_destroy(dev);
            break;
    }

    return Success;
}

static void
xf86emulated_convert_to_valuators(EmulatedValuatorData * event, ValuatorMask* mask)
{
    valuator_mask_zero(mask);
    for (int i = 0; i < EMULATED_MAX_VALUATORS && i < MAX_VALUATORS; ++i) {
        if (BitIsOn(event->mask, i)) {
            if (event->has_unaccelerated) {
                valuator_mask_set_unaccelerated(mask, i, event->valuators[i],
                                                event->unaccelerated[i]);
            } else {
                valuator_mask_set_double(mask, i, event->valuators[i]);
            }
        }
    }
}

static void
xf86emulated_handle_wait_for_sync(InputInfoPtr pInfo)
{
    EmulatedDevicePtr driver_data = pInfo->private;

    xf86IDrvMsg(pInfo, X_DEBUG, "Handling sync event\n");

    driver_data->waiting_for_drain = 1;
}

static void
xf86emulated_handle_motion(InputInfoPtr pInfo, EmulatedEventMotionEvent *event)
{
    DeviceIntPtr dev = pInfo->dev;
    EmulatedDevicePtr driver_data = pInfo->private;
    ValuatorMask* mask = driver_data->valuators;

    xf86IDrvMsg(pInfo, X_DEBUG, "Handling motion event\n");

    xf86emulated_convert_to_valuators(&(event->valuators), mask);
    xf86PostMotionEventM(dev, event->is_absolute ? Absolute : Relative, mask);
}

static void
xf86emulated_handle_proximity(InputInfoPtr pInfo, EmulatedEventProximityEvent *event)
{
    DeviceIntPtr dev = pInfo->dev;
    EmulatedDevicePtr driver_data = pInfo->private;
    ValuatorMask* mask = driver_data->valuators;

    xf86IDrvMsg(pInfo, X_DEBUG, "Handling proximity event\n");

    xf86emulated_convert_to_valuators(&(event->valuators), mask);
    xf86PostProximityEventM(dev, event->is_in, mask);
}

static void
xf86emulated_handle_button(InputInfoPtr pInfo, EmulatedEventButton *event)
{
    DeviceIntPtr dev = pInfo->dev;
    EmulatedDevicePtr driver_data = pInfo->private;
    ValuatorMask* mask = driver_data->valuators;

    xf86IDrvMsg(pInfo, X_DEBUG, "Handling button event\n");

    xf86emulated_convert_to_valuators(&(event->valuators), mask);
    xf86PostButtonEventM(dev, event->is_absolute ? Absolute : Relative, event->button,
                         event->is_down, mask);
}

static void
xf86emulated_handle_key(InputInfoPtr pInfo, EmulatedEventKey *event)
{
    DeviceIntPtr dev = pInfo->dev;

    xf86IDrvMsg(pInfo, X_DEBUG, "Handling key event\n");

    xf86PostKeyboardEvent(dev, event->key_code, event->is_down);
}

static void
xf86emulated_handle_touch(InputInfoPtr pInfo, EmulatedEventTouch *event)
{
    DeviceIntPtr dev = pInfo->dev;
    EmulatedDevicePtr driver_data = pInfo->private;
    ValuatorMask* mask = driver_data->valuators;

    xf86IDrvMsg(pInfo, X_DEBUG, "Handling touch event\n");

    xf86emulated_convert_to_valuators(&(event->valuators), mask);
    xf86PostTouchEvent(dev, event->touchid, event->type, event->flags, mask);
}

static void
xf86emulated_handle_gesture_swipe(InputInfoPtr pInfo, EmulatedEventGestureSwipe *event)
{
    DeviceIntPtr dev = pInfo->dev;

    xf86IDrvMsg(pInfo, X_DEBUG, "Handling gesture swipe event\n");

    xf86PostGestureSwipeEvent(dev, event->type, event->num_touches, event->flags,
                              event->delta_x, event->delta_y,
                              event->delta_unaccel_x, event->delta_unaccel_y);
}

static void
xf86emulated_handle_gesture_pinch(InputInfoPtr pInfo, EmulatedEventGesturePinch *event)
{
    DeviceIntPtr dev = pInfo->dev;

    xf86IDrvMsg(pInfo, X_DEBUG, "Handling gesture pinch event\n");

    xf86PostGesturePinchEvent(dev, event->type, event->num_touches, event->flags,
                              event->delta_x, event->delta_y,
                              event->delta_unaccel_x, event->delta_unaccel_y,
                              event->scale, event->delta_angle);
}

static void
xf86emulated_handle_event(InputInfoPtr pInfo, EmulatedEvent *event)
{
    if (!pInfo->dev->public.on)
        return;

    switch (event->any.event) {
        case EmulatedEvent_Unknown:
            break;
        case EmulatedEvent_WaitForSync:
            xf86emulated_handle_wait_for_sync(pInfo);
            break;
        case EmulatedEvent_Motion:
            xf86emulated_handle_motion(pInfo, &(event->motion));
            break;
        case EmulatedEvent_Proximity:
            xf86emulated_handle_proximity(pInfo, &(event->proximity));
            break;
        case EmulatedEvent_Button:
            xf86emulated_handle_button(pInfo, &(event->button));
            break;
        case EmulatedEvent_Key:
            xf86emulated_handle_key(pInfo, &(event->key));
            break;
        case EmulatedEvent_Touch:
            xf86emulated_handle_touch(pInfo, &(event->touch));
            break;
        case EmulatedEvent_GesturePinch:
            xf86emulated_handle_gesture_pinch(pInfo, &(event->pinch));
            break;
        case EmulatedEvent_GestureSwipe:
            xf86emulated_handle_gesture_swipe(pInfo, &(event->swipe));
            break;
    }
}

static void
xf86emulated_read_input_from_test(InputInfoPtr pInfo)
{
    int read_size = 0;
    int i;
    int events_count;
    EmulatedDevicePtr driver_data = pInfo->private;

    while (1) {
        read_size = read(driver_data->events_in_fd, driver_data->events_buffer,
                         driver_data->events_buffer_size * sizeof(EmulatedEvent));
        if (read_size < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;

            xf86IDrvMsg(pInfo, X_ERROR, "Error reading events: %s\n", strerror(errno));
            return;
        }

        if (read_size == 0)
            return;

        if (read_size % sizeof(EmulatedEvent) != 0) {

            xf86IDrvMsg(pInfo, X_ERROR, "Unexpected read size: got %d remaining bytes, "
                        "expected %d (total read size: %d)\n",
                        (int) (read_size % sizeof(EmulatedEvent)),
                        (int) sizeof(EmulatedEvent),
                        (int) read_size);
            return;
        }

        events_count = read_size / sizeof(EmulatedEvent);
        for (i = 0; i < events_count; ++i) {
            xf86emulated_handle_event(pInfo, driver_data->events_buffer + i);
        }
    }
}

static void
xf86emulated_read_input(InputInfoPtr pInfo)
{
}

static const char*
xf86emulated_get_type_name(EmulatedDevicePtr driver_data)
{
    switch (driver_data->device_type) {
        case TYPE_POINTER_GESTURE: return XI_TOUCHPAD;
        case TYPE_TOUCH: return XI_TOUCHSCREEN;
        case TYPE_POINTER:
        case TYPE_POINTER_ABS: return XI_MOUSE;
        case TYPE_POINTER_ABS_PROXIMITY: return XI_TABLET;
        case TYPE_KEYBOARD: return XI_KEYBOARD;
    }
    return XI_KEYBOARD;
}

static EmulatedDevicePtr
xf86emulated_device_alloc(void)
{
    EmulatedDevicePtr driver_data = calloc(sizeof(EmulatedDevice), 1);

    if (!driver_data)
        return NULL;

    driver_data->events_in_fd = -1;
    driver_data->events_out_fd = -1;

    return driver_data;
}


static int
xf86emulated_pre_init(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    EmulatedDevicePtr driver_data = NULL;
    int pipefds[2];
    char *device_type_option;

    pInfo->type_name = 0;
    pInfo->device_control = xf86emulated_device_control;
    pInfo->read_input = xf86emulated_read_input;
    pInfo->control_proc = NULL;
    pInfo->switch_mode = NULL;

    driver_data = xf86emulated_device_alloc();
    if (!driver_data)
        goto fail;

    driver_data->events_buffer_size = 32;
    driver_data->events_buffer = calloc(sizeof(EmulatedEvent), driver_data->events_buffer_size);
    if (!driver_data->events_buffer)
        goto fail;

    driver_data->valuators = valuator_mask_new(6);
    if (!driver_data->valuators)
        goto fail;

    driver_data->valuators_unaccelerated = valuator_mask_new(2);
    if (!driver_data->valuators_unaccelerated)
        goto fail;

    driver_data->events_in_path = xf86SetStrOption(pInfo->options, "EventsInPath", NULL);
    if (!driver_data->events_in_path){
        xf86IDrvMsg(pInfo, X_ERROR, "EventsInPath must be specified\n");
        goto fail;
    }

    driver_data->events_out_path = xf86SetStrOption(pInfo->options, "EventsOutPath", NULL);
    if (!driver_data->events_out_path) {
        xf86IDrvMsg(pInfo, X_ERROR, "EventsOutPath must be specified\n");
        goto fail;
    }

    unlink(driver_data->events_in_path);
    unlink(driver_data->events_out_path);
    if (mkfifo(driver_data->events_in_path, 0777) != 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "Could not create fifo for EventsInPath\n");
        goto fail;
    }
    if (mkfifo(driver_data->events_out_path, 0777) != 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "Could not create fifo for EventsOutPath\n");
        goto fail;
    }

    do {
        driver_data->events_in_fd = open(driver_data->events_in_path, O_RDONLY | O_NONBLOCK, 0);
    } while (driver_data->events_in_fd < 0 && errno == EINTR);

    if (driver_data->events_in_fd < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "Could not open EventsInPath\n");
        goto fail;
    }

    do {
        driver_data->events_out_fd = open(driver_data->events_out_path, O_RDWR, 0);
    } while (driver_data->events_out_fd < 0 && errno == EINTR);

    if (driver_data->events_out_fd < 0){
        xf86IDrvMsg(pInfo, X_ERROR, "Could not open EventsOutPath\n");
        goto fail;
    }

    if (pipe(pipefds) < 0)
        goto fail;
    driver_data->pipe_read_fd = pipefds[0];
    driver_data->pipe_write_fd = pipefds[1];

    device_type_option = xf86SetStrOption(pInfo->options, "EmulatedType", NULL);
    if (device_type_option == NULL) {
        xf86IDrvMsg(pInfo, X_ERROR, "EmulatedType option must be specified\n");
        goto fail;
    }

    if (strcmp(device_type_option, "Keyboard") == 0) {
        driver_data->device_type = TYPE_KEYBOARD;
    } else if (strcmp(device_type_option, "Pointer") == 0) {
        driver_data->device_type = TYPE_POINTER;
    } else if (strcmp(device_type_option, "PointerGesture") == 0) {
        driver_data->device_type = TYPE_POINTER_GESTURE;
    } else if (strcmp(device_type_option, "PointerAbsolute") == 0) {
        driver_data->device_type = TYPE_POINTER_ABS;
    } else if (strcmp(device_type_option, "PointerAbsoluteProximity") == 0) {
        driver_data->device_type = TYPE_POINTER_ABS_PROXIMITY;
    } else if (strcmp(device_type_option, "Touch") == 0) {
        driver_data->device_type = TYPE_TOUCH;
    } else {
        xf86IDrvMsg(pInfo, X_ERROR, "Unsupported EmulatedType option.\n");
        goto fail;
    }
    free(device_type_option);

    pInfo->private = driver_data;
    driver_data->pInfo = pInfo;

    /* FIXME:
    pInfo->options = xf86ReplaceIntOption(pInfo->options, "AccelerationProfile", -1);
    pInfo->options = xf86ReplaceStrOption(pInfo->options, "AccelerationScheme", "none");
    */

    pInfo->type_name = xf86emulated_get_type_name(driver_data);

    return Success;
fail:
    if (driver_data) {
        free(driver_data->events_buffer);
        close(driver_data->events_in_fd);
        close(driver_data->events_out_fd);
        close(driver_data->pipe_read_fd);
        close(driver_data->pipe_write_fd);
        if (driver_data->events_in_path)
            unlink(driver_data->events_in_path);
        if (driver_data->events_out_path)
            unlink(driver_data->events_out_path);
        free(driver_data->events_in_path);
        free(driver_data->events_out_path);

        if (driver_data->valuators)
            valuator_mask_free(&driver_data->valuators);
        if (driver_data->valuators_unaccelerated)
            valuator_mask_free(&driver_data->valuators_unaccelerated);
    }

    free(driver_data);
    return BadValue;
}

static void
xf86emulated_uninit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    EmulatedDevicePtr driver_data = pInfo->private;
    if (driver_data) {
        close(driver_data->events_in_fd);
        close(driver_data->events_out_fd);
        close(driver_data->pipe_read_fd);
        close(driver_data->pipe_write_fd);
        unlink(driver_data->events_in_path);
        unlink(driver_data->events_out_path);

        valuator_mask_free(&driver_data->valuators);
        valuator_mask_free(&driver_data->valuators_unaccelerated);
        free(driver_data);

        pInfo->private = NULL;
    }
    xf86DeleteInput(pInfo, flags);
}

InputDriverRec xf86emulated_driver = {
    .driverVersion = 1,
    .driverName = "emulated",
    .PreInit = xf86emulated_pre_init,
    .UnInit = xf86emulated_uninit,
    .module = NULL,
    .default_options = NULL,
    .capabilities = XI86_DRV_CAP_SERVER_FD
};

static XF86ModuleVersionInfo xf86emulated_version_info = {
    "emulated",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    {0, 0, 0, 0}
};

static pointer
xf86emulated_setup_proc(pointer module, pointer options, int *errmaj, int *errmin)
{
    xf86AddInputDriver(&xf86emulated_driver, module, 0);
    return module;
}

_X_EXPORT XF86ModuleData emulatedModuleData = {
    .vers = &xf86emulated_version_info,
    .setup = &xf86emulated_setup_proc,
    .teardown = NULL
};

