/*
 * Cinnamon Power Guard Integration Hooks
 * Provides integration points for display, WiFi, and sensor drivers
 * to communicate with the Cinnamon Power Guard system
 *
 * Copyright (C) 2026 Cinnamon Kernel Project
 */

#ifndef _CINNAMON_POWER_GUARD_H_
#define _CINNAMON_POWER_GUARD_H_

#include <linux/types.h>

/* Power Guard Events */
enum cinnamon_power_guard_event {
	CINNAMON_DISPLAY_ON,
	CINNAMON_DISPLAY_OFF,
	CINNAMON_DEEP_SLEEP_ENTER,
	CINNAMON_DEEP_SLEEP_EXIT,
};

/* Power Guard Functions */
extern void cinnamon_power_guard_notify_event(enum cinnamon_power_guard_event event);
extern bool cinnamon_power_guard_is_deep_sleep_enabled(void);
extern bool cinnamon_power_guard_is_display_on(void);

/* Integration Macros for Drivers */
#define CINNAMON_POWER_GUARD_DISPLAY_ON() \
	cinnamon_power_guard_notify_event(CINNAMON_DISPLAY_ON)

#define CINNAMON_POWER_GUARD_DISPLAY_OFF() \
	cinnamon_power_guard_notify_event(CINNAMON_DISPLAY_OFF)

#define CINNAMON_POWER_GUARD_DEEP_SLEEP_ENTER() \
	cinnamon_power_guard_notify_event(CINNAMON_DEEP_SLEEP_ENTER)

#define CINNAMON_POWER_GUARD_DEEP_SLEEP_EXIT() \
	cinnamon_power_guard_notify_event(CINNAMON_DEEP_SLEEP_EXIT)

/* Power Guard Configuration */
#define CINNAMON_POWER_GUARD_SUSPEND_INPUT() \
	do { \
		if (cinnamon_power_guard_is_deep_sleep_enabled()) { \
			/* Suspend input device when entering deep sleep */ \
		} \
	} while(0)

#define CINNAMON_POWER_GUARD_RESUME_INPUT() \
	do { \
		if (cinnamon_power_guard_is_deep_sleep_enabled()) { \
			/* Resume input device when exiting deep sleep */ \
		} \
	} while(0)

#endif /* _CINNAMON_POWER_GUARD_H_ */
