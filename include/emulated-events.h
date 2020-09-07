/*
 * Copyright © 2015 Red Hat, Inc.
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

#ifndef _EMULATED_PROPERTIES_H_
#define _EMULATED_PROPERTIES_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define EMULATED_DRIVER_VERSION 0x00000001
#define EMULATED_SYNC_RESPONSE 's'

enum EmulatedEventType
{
    EmulatedEvent_Unknown,
    EmulatedEvent_WaitForSync,
    EmulatedEvent_Motion,
    EmulatedEvent_Proximity,
    EmulatedEvent_Button,
    EmulatedEvent_Key,
    EmulatedEvent_Touch,
    EmulatedEvent_GestureSwipe,
    EmulatedEvent_GesturePinch
};

/* We care more about preserving the protocol than the size of the messages, so hardcode larger
   valuator count than the Xorg has */
#define EMULATED_MAX_VALUATORS 50

typedef struct {
    uint8_t has_unaccelerated;
    uint8_t mask[EMULATED_MAX_VALUATORS + 7 / 8];
    double valuators[EMULATED_MAX_VALUATORS];
    double unaccelerated[EMULATED_MAX_VALUATORS];
} EmulatedValuatorData;

typedef struct {
    enum EmulatedEventType event;
    int32_t is_absolute;
    EmulatedValuatorData valuators;
} EmulatedEventMotionEvent;

typedef struct {
    enum EmulatedEventType event;
    int32_t is_in;
    EmulatedValuatorData valuators;
} EmulatedEventProximityEvent;

typedef struct {
    enum EmulatedEventType event;
    int32_t is_absolute;
    int32_t button;
    int32_t is_down;
    EmulatedValuatorData valuators;
} EmulatedEventButton;

typedef struct {
    enum EmulatedEventType event;
    int32_t key_code;
    int32_t is_down;
} EmulatedEventKey;

typedef struct {
    enum EmulatedEventType event;
    uint32_t touchid;
    uint16_t type;
    uint32_t flags;
    EmulatedValuatorData valuators;
} EmulatedEventTouch;

typedef struct {
    enum EmulatedEventType event;
    uint16_t type;
    uint16_t num_touches;
    uint32_t flags;
    double delta_x;
    double delta_y;
    double delta_unaccel_x;
    double delta_unaccel_y;
    double scale;
    double delta_angle;
} EmulatedEventGesturePinch;

typedef struct {
    enum EmulatedEventType event;
    uint16_t type;
    uint16_t num_touches;
    uint32_t flags;
    double delta_x;
    double delta_y;
    double delta_unaccel_x;
    double delta_unaccel_y;
} EmulatedEventGestureSwipe;

typedef union
{
    struct {
        enum EmulatedEventType event;
    } any;
    EmulatedEventMotionEvent motion;
    EmulatedEventProximityEvent proximity;
    EmulatedEventButton button;
    EmulatedEventKey key;
    EmulatedEventTouch touch;
    EmulatedEventGesturePinch pinch;
    EmulatedEventGestureSwipe swipe;
} EmulatedEvent;

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* _EMULATED_PROPERTIES_H_ */
