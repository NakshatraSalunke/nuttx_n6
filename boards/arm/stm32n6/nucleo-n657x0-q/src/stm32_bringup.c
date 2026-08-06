/****************************************************************************
 * boards/arm/stm32n6/nucleo-n657x0-q/src/stm32_bringup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <syslog.h>
#include <debug.h>

#include <nuttx/board.h>
#include <nuttx/leds/userled.h>

#include "nucleo-n657x0-q.h"

#include <arch/board/board.h>

#ifdef CONFIG_I2C
#  include <nuttx/i2c/i2c_master.h>
#  include "stm32_i2c.h"
#endif

#ifdef CONFIG_SENSORS_MPU60X0
#  include <nuttx/sensors/mpu60x0.h>
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stm32_bringup
 *
 * Description:
 *   Perform architecture-specific initialization
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y :
 *     Called from board_late_initialize().
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=n && CONFIG_BOARDCTL=y :
 *     Called from the NSH library
 *
 ****************************************************************************/

int stm32_bringup(void)
{
#if !defined(CONFIG_ARCH_LEDS) && defined(CONFIG_USERLED_LOWER)
  int ret;

  /* Register the LED driver */

  ret = userled_lower_initialize("/dev/userleds");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: userled_lower_initialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_I2C
  {
    FAR struct i2c_master_s *i2c;
    int ret;

    /* Initialize I2C1 */
    i2c = stm32_i2cbus_initialize(1);
    if (!i2c)
      {
        syslog(LOG_ERR, "ERROR: Failed to initialize I2C1\n");
      }
    else
      {
        /* Register /dev/i2c1 */
        ret = i2c_register(i2c, 1);
        if (ret < 0)
          {
            syslog(LOG_ERR, "ERROR: i2c_register for I2C1 failed: %d\n", ret);
          }
      }
  }
#endif

#ifdef CONFIG_SENSORS_MPU60X0
  {
    FAR struct i2c_master_s *i2c;
    struct mpu_config_s config;
    int ret;

    i2c = stm32_i2cbus_initialize(1);
    if (!i2c)
      {
        syslog(LOG_ERR, "ERROR: Failed to initialize I2C1 for MPU60X0\n");
      }
    else
      {
        memset(&config, 0, sizeof(config));
        config.i2c = i2c;
        config.addr = 0x68; /* Default MPU6050 address */

        ret = mpu60x0_register("/dev/imu0", &config);
        if (ret < 0)
          {
            syslog(LOG_ERR, "ERROR: mpu60x0_register failed: %d\n", ret);
          }
      }
  }
#endif

  return OK;
}
