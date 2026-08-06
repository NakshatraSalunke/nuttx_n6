/****************************************************************************
 * arch/arm/src/stm32n6/stm32_dma.h
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

#ifndef __ARCH_ARM_SRC_STM32N6_STM32_DMA_H
#define __ARCH_ARM_SRC_STM32N6_STM32_DMA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/types.h>
#include <stdint.h>

#include "hardware/stm32n6xxx_gpdma.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* These definitions provide the bit encoding of the 'status' parameter
 * passed to the DMA callback function (see dma_callback_t).
 */

#define DMA_STATUS_TCF        (1 << 0) /* Transfer Complete */
#define DMA_STATUS_HTF        (1 << 1) /* Half Transfer */
#define DMA_STATUS_DTEF       (1 << 2) /* Data transfer error */
#define DMA_STATUS_ULEF       (1 << 3) /* Update link transfer error */
#define DMA_STATUS_USEF       (1 << 4) /* User setting error */
#define DMA_STATUS_SUSPF      (1 << 5) /* Completed suspension flag */
#define DMA_STATUS_TOF        (1 << 6) /* Trigger overrun flag */

#define DMA_STATUS_FATAL      (DMA_STATUS_DTEF | DMA_STATUS_ULEF | DMA_STATUS_USEF)
#define DMA_STATUS_SUCCESS    (DMA_STATUS_TCF | DMA_STATUS_HTF)

/* GPDMA Mode Flags */

#define GPDMACFG_MODE_CIRC    (1 << 0)  /* Enable Circular mode */
#define GPDMACFG_MODE_PFC     (1 << 1)  /* Enable Peripheral flow control */
#define GPDMACFG_MODE_DB      (1 << 2)  /* Enable Double buffer mode */

/* Channel priority level
 * Refer to PRIO field in GPDMA_CxCR register description
 */

#define GPDMACFG_PRIO_LL      (0)   /* Low priority, low weight */
#define GPDMACFG_PRIO_LM      (1)   /* Low priority, mid weight */
#define GPMDACFG_PRIO_LH      (2)   /* Low priority, high weight */
#define GPDMACFG_PRIO_H       (3)   /* High priority */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* GPDMA transfer type enumeration */

enum gpdma_ttype_e
{
  /* Peripheral-to-memory transfer */
  GPDMA_TTYPE_P2M = 0,

  /* Memory-to-peripheral transfer */
  GPDMA_TTYPE_M2P,

  /* Memory-to-memory transfer, linear addressing */
  GPDMA_TTYPE_M2M_LINEAR,

  /* 2D Addressing needed (NOT IMPLEMENTED YET) */
  GPDMA_TTYPE_2D
};

struct stm32_gpdma_cfg_s
{
  uint32_t src_addr;
  uint32_t dest_addr;

  /* CxTR1 register for specified channel. */
  uint32_t tr1;

  /* request: Accepts GPDMA_CXTR2_SWREQ, GPDMA_CXTR2_DREQ, and
   * GPDMA_CXTR2_REQSEL(r)
   */
  uint16_t request;

  /* Number of transfers, in units of the data width
   * specified in tr1.
   */
  uint16_t ntransfers;

  /* Priority level: refer to GPDMACFG_PRIO defines above */
  uint8_t  priority;

  /* mode flags, refer to GPDMACFG_MODE_X defines above. */
  uint8_t  mode;
};

/* DMA_HANDLE Provides an opaque reference that can be used to represent a
 * DMA stream.
 */
typedef void *DMA_HANDLE;

/* Description:
 *   This is the type of the callback that is used to inform the user of the
 *   completion of the DMA.
 */
typedef void (*dma_callback_t)(DMA_HANDLE handle, uint8_t status, void *arg);

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: stm32_dmainitialize
 *
 * Description:
 *   Initialize the DMA subsystem.
 *
 ****************************************************************************/

void stm32_dmainitialize(void);

DMA_HANDLE stm32_dmachannel(enum gpdma_ttype_e type);
void stm32_dmafree(DMA_HANDLE handle);
void stm32_dmasetup(DMA_HANDLE handle, struct stm32_gpdma_cfg_s *cfg);
void stm32_dmastart(DMA_HANDLE handle, dma_callback_t callback, void *arg, bool half);
void stm32_dmastop(DMA_HANDLE handle);
size_t stm32_dmaresidual(DMA_HANDLE handle);
void stm32_dmadump(DMA_HANDLE handle, const char *msg);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* !__ASSEMBLY__*/
#endif /* __ARCH_ARM_SRC_STM32N6_STM32_DMA_H*/
