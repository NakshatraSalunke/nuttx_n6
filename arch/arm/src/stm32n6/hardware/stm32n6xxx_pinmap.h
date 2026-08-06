/****************************************************************************
 * arch/arm/src/stm32n6/hardware/stm32n6xxx_pinmap.h
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

#ifndef __ARCH_ARM_SRC_STM32N6_HARDWARE_STM32N6XXX_PINMAP_H
#define __ARCH_ARM_SRC_STM32N6_HARDWARE_STM32N6XXX_PINMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Alternate Pin Functions.
 *
 * Alternative pin selections are provided with a numeric suffix like _1, _2,
 * etc.  Drivers, however, will use the pin selection without the numeric
 * suffix.  Additional definitions are required in the board.h file.  For
 * example, if USART1_TX connects via PE5 on some board, then the following
 * definition should appear in the board.h header file for that board:
 *
 * #define GPIO_USART1_TX GPIO_USART1_TX_1
 *
 * The driver will then automatically configure PE5 as the USART1 TX pin.
 */

/* USART1: PE5=TX (AF7), PE6=RX (AF7) - ST-Link Virtual COM Port */

#define GPIO_USART1_TX_1   (GPIO_ALT | GPIO_AF7 | GPIO_SPEED_50MHZ | GPIO_PUSHPULL | GPIO_PORTE | GPIO_PIN5)
#define GPIO_USART1_RX_1   (GPIO_ALT | GPIO_AF7 | GPIO_SPEED_50MHZ | GPIO_PORTE | GPIO_PIN6)

/* USART3: PD8=TX (AF7), PD9=RX (AF7) */
#define GPIO_USART3_TX_1   (GPIO_ALT | GPIO_AF7 | GPIO_SPEED_50MHZ | GPIO_PUSHPULL | GPIO_PORTD | GPIO_PIN8)
#define GPIO_USART3_RX_1   (GPIO_ALT | GPIO_AF7 | GPIO_PORTD | GPIO_PIN9)


/* TIM2 Channel 1: PA0 (AF1) */
#define GPIO_TIM2_CH1OUT_1   (GPIO_ALT | GPIO_AF1 | GPIO_SPEED_50MHZ | GPIO_PUSHPULL | GPIO_PORTA | GPIO_PIN0)

/* TIM2 Channel 2: PA1 (AF1) */
#define GPIO_TIM2_CH2OUT_1   (GPIO_ALT | GPIO_AF1 | GPIO_SPEED_50MHZ | GPIO_PUSHPULL | GPIO_PORTA | GPIO_PIN1)

/* I2C1: PC1=SDA (AF4), PH9=SCL (AF4) */
#define GPIO_I2C1_SDA_1      (GPIO_ALT | GPIO_AF4 | GPIO_SPEED_50MHZ | GPIO_OPENDRAIN | GPIO_PORTC | GPIO_PIN1)
#define GPIO_I2C1_SCL_1      (GPIO_ALT | GPIO_AF4 | GPIO_SPEED_50MHZ | GPIO_OPENDRAIN | GPIO_PORTH | GPIO_PIN9)

/* SPI1: PB8=MISO (AF5), PB9=SCK (AF5), PC3=MOSI (AF5), PC5=NSS (AF5) */


/* SPI5 (TEST_SPI / Instinct-master pinout): PE15=SCK (AF5), PH7=MOSI (AF5), PH8=MISO (AF5), PH6=NSS (AF5) */


#endif /* __ARCH_ARM_SRC_STM32N6_HARDWARE_STM32N6XXX_PINMAP_H */

