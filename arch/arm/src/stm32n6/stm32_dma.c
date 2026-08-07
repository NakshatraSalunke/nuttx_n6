/****************************************************************************
 * arch/arm/src/stm32n6/stm32_dma.c
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

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <nuttx/debug.h>
#include <errno.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/signal.h>

#include "arm_internal.h"
#include "sched/sched.h"
#include "stm32_dma.h"
#include "hardware/stm32n6xxx_gpdma.h"
#include "hardware/stm32n6xxx_memorymap.h"
#include "hardware/stm32n6xxx_rcc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* For GPDMA peripheral, each channel has channel specific addresses that are
 * at a base offset based on channel. Channel reg references will be based
 * off this in this file.
 */

#define CH_BASE_OFFSET(ch)  (0x80*(ch))
#define CH_CXLBAR_OFFSET     0x50
#define CH_CXFCR_OFFSET      0x5C
#define CH_CXSR_OFFSET       0x60
#define CH_CXCR_OFFSET       0x64
#define CH_CXTR1_OFFSET      0x90
#define CH_CXTR2_OFFSET      0x94
#define CH_CXBR1_OFFSET      0x98
#define CH_CXSAR_OFFSET      0x9C
#define CH_CXDAR_OFFSET      0xA0
#define CH_CXTR3_OFFSET      0xA4
#define CH_CXBR2_OFFSET      0xA8
#define CH_CXLLR_OFFSET      0xCC

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct stm32_gpdma_lli_s
{
  uint32_t tr1;   /* GPDMA_CxTR1 value */
  uint32_t tr2;   /* GPDMA_CxTR2 value */
  uint32_t br1;   /* GPDMA_CxBR1 value (block size in bytes) */
  uint32_t sar;   /* GPDMA_CxSAR (source address) */
  uint32_t dar;   /* GPDMA_CxDAR (dest address) */
  uint32_t llr;   /* GPDMA_CxLLR (pointer+update bits) */
}
__attribute__ ((aligned(32)));

struct gpdma_ch_s
{
  uint8_t            dma_instance; /* GPDMA1 or HPDMA1 */
  uint8_t            channel;
  uint8_t            irq;
  enum gpdma_ttype_e type;
  bool               free;         /* Is this channel free to use. */
  uint32_t           base;         /* Channel base address */
  dma_callback_t     callback;
  void              *arg;
  struct stm32_gpdma_cfg_s cfg;   /* Configuration passed at channel setup */
  struct stm32_gpdma_lli_s lli[2];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t gpdmach_getreg(struct gpdma_ch_s *chan,
                                      uint32_t offset);
static inline void gpdmach_putreg(struct gpdma_ch_s *chan, uint32_t offset,
                                  uint32_t value);
static inline void gpdmach_modifyreg32(struct gpdma_ch_s *chan,
                                       uint32_t offset, uint32_t clrbits,
                                       uint32_t setbits);
static void gpdma_ch_abort(struct gpdma_ch_s *chan);
static void gpdma_ch_disable(struct gpdma_ch_s *chan);

static int gpdma_setup(struct gpdma_ch_s *chan,
                       struct stm32_gpdma_cfg_s *cfg);
static int gpdma_setup_circular(struct gpdma_ch_s *chan,
                                struct stm32_gpdma_cfg_s *cfg);
static int gpdma_dmainterrupt(int irq, void *context, void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_STM32N6_GPDMA1
static struct gpdma_ch_s g_chan[] =
{
  {
    .dma_instance = 1,
    .channel = 0,
    .irq = STM32_IRQ_GPDMA1_CH0,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(0)
  },
  {
    .dma_instance = 1,
    .channel = 1,
    .irq = STM32_IRQ_GPDMA1_CH1,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(1)
  },
  {
    .dma_instance = 1,
    .channel = 2,
    .irq = STM32_IRQ_GPDMA1_CH2,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(2)
  },
  {
    .dma_instance = 1,
    .channel = 3,
    .irq = STM32_IRQ_GPDMA1_CH3,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(3)
  },
  {
    .dma_instance = 1,
    .channel = 4,
    .irq = STM32_IRQ_GPDMA1_CH4,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(4)
  },
  {
    .dma_instance = 1,
    .channel = 5,
    .irq = STM32_IRQ_GPDMA1_CH5,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(5)
  },
  {
    .dma_instance = 1,
    .channel = 6,
    .irq = STM32_IRQ_GPDMA1_CH6,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(6)
  },
  {
    .dma_instance = 1,
    .channel = 7,
    .irq = STM32_IRQ_GPDMA1_CH7,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(7)
  },
  {
    .dma_instance = 1,
    .channel = 8,
    .irq = STM32_IRQ_GPDMA1_CH8,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(8)
  },
  {
    .dma_instance = 1,
    .channel = 9,
    .irq = STM32_IRQ_GPDMA1_CH9,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(9)
  },
  {
    .dma_instance = 1,
    .channel = 10,
    .irq = STM32_IRQ_GPDMA1_CH10,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(10)
  },
  {
    .dma_instance = 1,
    .channel = 11,
    .irq = STM32_IRQ_GPDMA1_CH11,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(11)
  },
  {
    .dma_instance = 1,
    .channel = 12,
    .irq = STM32_IRQ_GPDMA1_CH12,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(12)
  },
  {
    .dma_instance = 1,
    .channel = 13,
    .irq = STM32_IRQ_GPDMA1_CH13,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(13)
  },
  {
    .dma_instance = 1,
    .channel = 14,
    .irq = STM32_IRQ_GPDMA1_CH14,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(14)
  },
  {
    .dma_instance = 1,
    .channel = 15,
    .irq = STM32_IRQ_GPDMA1_CH15,
    .free = true,
    .base = STM32_GPDMA1_BASE + CH_BASE_OFFSET(15)
  }
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t gpdmach_getreg(struct gpdma_ch_s *chan,
                                      uint32_t offset)
{
  return getreg32(chan->base + offset);
}

static inline void gpdmach_putreg(struct gpdma_ch_s *chan, uint32_t offset,
                                  uint32_t value)
{
  putreg32(value, chan->base + offset);
}

static inline void gpdmach_modifyreg32(struct gpdma_ch_s *chan,
                                       uint32_t offset, uint32_t clrbits,
                                       uint32_t setbits)
{
  modifyreg32(chan->base + offset, clrbits, setbits);
}

/****************************************************************************
 * Name: gpdma_dmainterrupt
 *
 * Description:
 *   DMA interrupt handler.
 *
 ****************************************************************************/

static int gpdma_dmainterrupt(int irq, void *context, void *arg)
{
  struct gpdma_ch_s *chan = NULL;
  uint32_t status;

  /* Get the channel that generated the interrupt */

#ifdef CONFIG_STM32N6_GPDMA1
  if (irq >= STM32_IRQ_GPDMA1_CH0 && irq <= STM32_IRQ_GPDMA1_CH15)
    {
      chan = &g_chan[irq - STM32_IRQ_GPDMA1_CH0];
    }
  else
#endif
    {
      DEBUGPANIC();
    }

  /* Get the interrupt status for this channel */
  status = (gpdmach_getreg(chan, CH_CXSR_OFFSET) >> 8) & 0x7f;

  /* Clear the fetched channel interrupts by setting bits in the flag
   * clear register
   */
  gpdmach_putreg(chan, CH_CXFCR_OFFSET, ~0);

  /* Invoke the callback */
  if (chan->callback)
    {
      chan->callback(chan, (uint8_t)status, chan->arg);
    }

  return 0;
}

/****************************************************************************
 * Name: gpdma_ch_abort
 *
 * Description:
 *   For the given channel, suspend and abort any ongoing channel transfers.
 *   Returns after the abort has complete and taken effect.
 *
 ****************************************************************************/

static void gpdma_ch_abort(struct gpdma_ch_s *chan)
{
  if ((gpdmach_getreg(chan, CH_CXCR_OFFSET) & GPDMA_CXCR_EN) == 0)
    {
      return;
    }

  /* 1. Software writes 1 to the GPDMA_CxCR.SUSP bit */
  gpdmach_putreg(chan, CH_CXCR_OFFSET, GPDMA_CXCR_SUSP);

  /* 2. Polls suspend flag GPDMA_CxSR.SUSPF until SUSPF = 1, or waits for an
   *    interrupt previously enabled by writing 1 to GPDMA_CxCR.SUSPIE.
   */
  while ((gpdmach_getreg(chan, CH_CXSR_OFFSET) & GPDMA_CXSR_SUSPF) == 0)
    {
    }

  /* 3. Reset chan by writing 1 to GPDMA_CxCR.RESET */
  gpdmach_putreg(chan, CH_CXCR_OFFSET, GPDMA_CXCR_RESET);

  /* 4. Wait for GPDMA_CxCR.EN and GPDMA_CxCR.SUSP bits to be reset */
  while ((gpdmach_getreg(chan, CH_CXCR_OFFSET) &
         (GPDMA_CXCR_EN | GPDMA_CXCR_SUSP)) != 0)
    {
    }
}

/****************************************************************************
 * Name: gpdma_ch_disable
 *
 * Description:
 *   Disable the DMA channel.
 *
 ****************************************************************************/

static void gpdma_ch_disable(struct gpdma_ch_s *chan)
{
  DEBUGASSERT(chan != NULL);

  gpdma_ch_abort(chan);

  /* Disable and clear all interrupts. */
  gpdmach_modifyreg32(chan, CH_CXCR_OFFSET, GPDMA_CXCR_ALLINTS, 0);
  gpdmach_modifyreg32(chan, CH_CXFCR_OFFSET, 0, ~0);
}

/****************************************************************************
 * Name: gpdma_setup
 *
 * Assumptions:
 *   - EN bit not set. Channel must have been aborted before this is called.
 *
 ****************************************************************************/

static int gpdma_setup(struct gpdma_ch_s *chan,
                       struct stm32_gpdma_cfg_s *cfg)
{
  uint32_t reg;

  /* Make sure not to use linked list mode. */
  gpdmach_modifyreg32(chan, CH_CXLLR_OFFSET, ~0, 0);
  gpdmach_putreg(chan, CH_CXLBAR_OFFSET, 0);

  /* Set source and destination addresses. */
  gpdmach_putreg(chan, CH_CXSAR_OFFSET, cfg->src_addr);
  gpdmach_putreg(chan, CH_CXDAR_OFFSET, cfg->dest_addr);

  /* Set the channel priority according to configuration. */
  gpdmach_modifyreg32(chan, CH_CXCR_OFFSET, GPDMA_CXCR_PRIO_MASK,
                      cfg->priority << GPDMA_CXCR_PRIO_SHIFT);

  /* Set channels TR1 register based on configuration provided. */
  gpdmach_putreg(chan, CH_CXTR1_OFFSET, cfg->tr1);

  /* Assemble the required config for TR2 */
  reg = (uint32_t)cfg->request &
        (GPDMA_CXTR2_REQSEL_MASK | GPDMA_CXTR2_DREQ | GPDMA_CXTR2_SWREQ);
  gpdmach_putreg(chan, CH_CXTR2_OFFSET, reg);

  /* Calculate block number of data bytes to transfer, update BR1 */
  reg = cfg->ntransfers;
  gpdmach_putreg(chan, CH_CXBR1_OFFSET, reg);

  return 0;
}

/****************************************************************************
 * Name: gpdma_setup_circular
 *
 * Description:
 *   Circular DMA requires linked list items (LLI) to setup properly. This
 *   function handles LLI allocation and handling necessary to implement
 *   circular DMA.
 *
 ****************************************************************************/

static int gpdma_setup_circular(struct gpdma_ch_s *chan,
                                struct stm32_gpdma_cfg_s *cfg)
{
  struct stm32_gpdma_lli_s *lli = chan->lli;

  lli[0].tr1 = cfg->tr1;
  lli[0].tr2 = (2U << GPDMA_CXTR2_TCEM_SHIFT)
             | (cfg->request & GPDMA_CXTR2_REQSEL_MASK);
  lli[0].br1 = cfg->ntransfers;
  lli[0].sar = cfg->src_addr;
  lli[0].dar = cfg->dest_addr;
  lli[0].llr = (GPDMA_CXLLR_UT1  /* reload TR1 */
             | GPDMA_CXLLR_UT2   /* reload TR2 */
             | GPDMA_CXLLR_UB1   /* reload BR1 */
             | GPDMA_CXLLR_USA   /* reload SAR */
             | GPDMA_CXLLR_UDA   /* reload DAR */
             | GPDMA_CXLLR_ULL)  /* reload LLR */
             | (((uint32_t)&lli[1]) & GPDMA_CXLLR_LA_MASK);

  lli[1].tr1 = lli[0].tr1;
  lli[1].tr2 = lli[0].tr2;
  lli[1].br1 = lli[0].br1;
  lli[1].sar = lli[0].sar;
  lli[1].dar = lli[0].dar;
  lli[1].llr = (lli[0].llr & ~GPDMA_CXLLR_LA_MASK)
             | (((uint32_t)&lli[0]) & GPDMA_CXLLR_LA_MASK);

  gpdmach_putreg(chan, CH_CXSAR_OFFSET,   lli[0].sar);
  gpdmach_putreg(chan, CH_CXDAR_OFFSET,   lli[0].dar);
  gpdmach_putreg(chan, CH_CXTR1_OFFSET,   lli[0].tr1);
  gpdmach_putreg(chan, CH_CXTR2_OFFSET,   lli[0].tr2);
  gpdmach_putreg(chan, CH_CXBR1_OFFSET,   lli[0].br1);
  gpdmach_putreg(chan, CH_CXLBAR_OFFSET,  (uint32_t)&lli[0]);
  gpdmach_putreg(chan, CH_CXLLR_OFFSET,   lli[0].llr);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_dma_initialize
 *
 * Description:
 *   Initialize the DMA subsystem
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void weak_function arm_dma_initialize(void)
{
  struct gpdma_ch_s *chan;
  int i;

  /* Initialize each DMA stream */
#ifdef CONFIG_STM32N6_GPDMA1
  for (i = 0; i < sizeof(g_chan) / sizeof(struct gpdma_ch_s); i++)
    {
      chan = &g_chan[i];

      /* Attach DMA interrupt vectors */
      irq_attach(chan->irq, gpdma_dmainterrupt, chan);

      /* Disable the DMA channel */
      gpdma_ch_disable(chan);

      /* Enable the IRQ at the NVIC (still disabled at the DMA controller) */
      up_enable_irq(chan->irq);
    }
#endif
}

/****************************************************************************
 * Name: stm32_dmachannel
 *
 ****************************************************************************/

DMA_HANDLE stm32_dmachannel(enum gpdma_ttype_e type)
{
  DMA_HANDLE handle = NULL;
  irqstate_t flags;
  int i;

  /* Currently no support for M2M or 2D addressing modes. */
  DEBUGASSERT(type != GPDMA_TTYPE_M2M_LINEAR);
  DEBUGASSERT(type != GPDMA_TTYPE_2D);

  flags = enter_critical_section();

  if (type == GPDMA_TTYPE_M2P || type == GPDMA_TTYPE_P2M)
    {
#ifdef CONFIG_STM32N6_GPDMA1
      for (i = 0; i < (sizeof(g_chan) / sizeof(struct gpdma_ch_s)); i++)
        {
          struct gpdma_ch_s *chan = &g_chan[i];

          if (chan->free)
            {
              chan->free = false;
              chan->type = type;
              handle = (DMA_HANDLE)chan;
              break;
            }
        }
#endif
    }

  leave_critical_section(flags);

  if (handle == NULL)
    {
      /* Failed to allocate channel. */
      dmainfo("No available DMA chan for transfer type=%" PRIu8 "\n", type);
    }

  return handle;
}

/****************************************************************************
 * Name: stm32_dmafree
 *
 * Description:
 *   Release a DMA channel.
 *
 ****************************************************************************/

void stm32_dmafree(DMA_HANDLE handle)
{
  struct gpdma_ch_s *chan = (struct gpdma_ch_s *)handle;

  DEBUGASSERT(handle != NULL);

  chan->free = true;
}

/****************************************************************************
 * Name: stm32_dmasetup
 *
 * Description:
 *   Configure DMA before using
 *
 ****************************************************************************/

void stm32_dmasetup(DMA_HANDLE handle, struct stm32_gpdma_cfg_s *cfg)
{
  struct gpdma_ch_s *chan = (struct gpdma_ch_s *)handle;

  DEBUGASSERT(handle != NULL);

  /* Store the configuration so it can be referenced later by start. */
  chan->cfg = *cfg;

  gpdma_ch_disable(chan);

  /* Clear any unhandled flags from previous transactions */
  gpdmach_putreg(chan, CH_CXFCR_OFFSET, ~0);

  if (cfg->mode & GPDMACFG_MODE_CIRC)
    {
      gpdma_setup_circular(chan, cfg);
    }
  else
    {
      gpdma_setup(chan, cfg);
    }
}

/****************************************************************************
 * Name: stm32_dmastart
 *
 * Description:
 *   Start the DMA transfer.
 *
 ****************************************************************************/

void stm32_dmastart(DMA_HANDLE handle, dma_callback_t callback, void *arg,
                    bool half)
{
  struct gpdma_ch_s *chan = (struct gpdma_ch_s *)handle;
  uint32_t cr;

  DEBUGASSERT(handle != NULL);

  /* Save the callback info. This will be invoked when the DMA completes */
  chan->callback = callback;
  chan->arg = arg;

  /* Activate channel by setting ENABLE bin in the GPDMA_CXCR register. */
  cr = gpdmach_getreg(chan, CH_CXCR_OFFSET);
  cr |= GPDMA_CXCR_EN;

  if (chan->cfg.mode & (GPDMACFG_MODE_CIRC))
    {
      cr |= ((half ? GPDMA_CXCR_HTIE : 0) | (GPDMA_CXCR_ALLINTS & ~GPDMA_CXCR_HTIE));
    }
  else
    {
      cr |= (half ? GPDMA_CXCR_ALLINTS : (GPDMA_CXCR_ALLINTS & ~GPDMA_CXCR_HTIE));
    }

  gpdmach_putreg(chan, CH_CXCR_OFFSET, cr);
}

/****************************************************************************
 * Name: stm32_dmastop
 *
 * Description:
 *   Cancel the DMA.
 *
 ****************************************************************************/

void stm32_dmastop(DMA_HANDLE handle)
{
  struct gpdma_ch_s *chan = (struct gpdma_ch_s *)handle;
  gpdma_ch_disable(chan);
}

/****************************************************************************
 * Name: stm32_dmaresidual
 *
 * Description:
 *   Returns the number of data beats remaining to transfer.
 *
 ****************************************************************************/

size_t stm32_dmaresidual(DMA_HANDLE handle)
{
  struct gpdma_ch_s *chan = (struct gpdma_ch_s *)handle;
  uint32_t           br1  = getreg32(chan->base + CH_CXBR1_OFFSET);

  /* BNDT[15:0] = beats remaining in current block transfer */
  return (size_t)(br1 & GPDMA_CXBR1_BNDT_MASK);
}

/****************************************************************************
 * Name: stm32_dmastatus
 *
 * Description:
 *   Returns the GPDMA channel status register (CH_CXSR).
 *
 ****************************************************************************/

uint32_t stm32_dmastatus(DMA_HANDLE handle)
{
  struct gpdma_ch_s *chan = (struct gpdma_ch_s *)handle;
  return getreg32(chan->base + CH_CXSR_OFFSET);
}

/****************************************************************************
 * Name: stm32_dmainitialize
 *
 * Description:
 *   Initialize the DMA subsystem.
 *
 ****************************************************************************/

void stm32_dmainitialize(void)
{
  int i;

  /* Enable GPDMA1 clock */

  modifyreg32(STM32_RCC_AHB1ENR, 0, RCC_AHB1ENR_GPDMA1EN);

  /* Enable NVIC interrupts for GPDMA1 channels 0-15 */

  for (i = 0; i < 16; i++)
    {
      up_enable_irq(STM32_IRQ_GPDMA1_CH0 + i);
    }
}

void stm32_dmadump(DMA_HANDLE handle, const char *msg)
{
  struct gpdma_ch_s *chan = (struct gpdma_ch_s *)handle;
  if (!chan)
    {
      return;
    }

  syslog(LOG_ERR, "GPDMA Channel Dump: %s\n", msg);
  syslog(LOG_ERR, "  Instance: %d, Channel: %d, IRQ: %d, Base: 0x%08lx\n",
         chan->dma_instance, chan->channel, chan->irq, (unsigned long)chan->base);
  syslog(LOG_ERR, "  CR: 0x%08lx, SR: 0x%08lx, TR1: 0x%08lx, TR2: 0x%08lx\n",
         (unsigned long)getreg32(chan->base + CH_CXCR_OFFSET),
         (unsigned long)getreg32(chan->base + CH_CXSR_OFFSET),
         (unsigned long)getreg32(chan->base + CH_CXTR1_OFFSET),
         (unsigned long)getreg32(chan->base + CH_CXTR2_OFFSET));
  syslog(LOG_ERR, "  BR1: 0x%08lx, SAR: 0x%08lx, DAR: 0x%08lx, LLR: 0x%08lx\n",
         (unsigned long)getreg32(chan->base + CH_CXBR1_OFFSET),
         (unsigned long)getreg32(chan->base + CH_CXSAR_OFFSET),
         (unsigned long)getreg32(chan->base + CH_CXDAR_OFFSET),
         (unsigned long)getreg32(chan->base + CH_CXLLR_OFFSET));
}
