#ifndef LIB_HEARTBEAT_H_
#define LIB_HEARTBEAT_H_

#include <zephyr/kernel.h>

#ifdef CONFIG_HEARTBEAT

/**
 * @defgroup lib_heartbeat Heartbeat timer library
 * @ingroup lib
 * @{
 */

/**
 * @brief Initialises and starts the heartbeat timer.
 *
 * This function initialises the heartbeat GPIO and starts a timer with the
 * specified duration. If the timer has already been initialised, it will
 * simply restart the timer with a new duration. The heartbeat GPIO must be
 * aliased in devicetree as "heartbeat-gpio".
 *
 * @param duration The timeout duration for the heartbeat timer.
 * @return 0 on success, or a negative error code on failure.
 */
int heartbeat_init_start(k_timeout_t duration);

/**
 * @brief Stops the heartbeat timer.
 *
 * This function stops the heartbeat timer if it is running.
 */
void heartbeat_stop(void);

/** @} */

#endif /* CONFIG_HEARTBEAT */

#endif /* LIB_HEARTBEAT_H_ */
