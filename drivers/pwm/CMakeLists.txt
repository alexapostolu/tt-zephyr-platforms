# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()

# zephyr-keep-sorted-start
zephyr_library_sources_ifdef(CONFIG_PWM_MAX6639 pwm_max6639.c)
# zephyr-keep-sorted-stop
