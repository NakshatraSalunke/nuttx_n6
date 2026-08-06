/****************************************************************************
 * arch/arm/src/stm32n6/stm32_i2c.c
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
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <nuttx/debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/kmalloc.h>
#include <nuttx/clock.h>
#include <nuttx/cache.h>
#include <nuttx/i2c/i2c_master.h>

#include <arch/board/board.h>

#include "arm_internal.h"
#include "stm32_gpio.h"
#include "stm32_rcc.h"
#include "stm32_i2c.h"

#ifdef CONFIG_STM32N6_I2C_DMA
#include "stm32_dma.h"
#endif

/* At least one I2C peripheral must be enabled */

#if defined(CONFIG_STM32N6_I2C1) || defined(CONFIG_STM32N6_I2C2) || \
    defined(CONFIG_STM32N6_I2C3) || defined(CONFIG_STM32N6_I2C4)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Timeout config */

#define CONFIG_STM32N6_I2CTIMEOSEC 0
#define CONFIG_STM32N6_I2CTIMEOMS  500

#define CONFIG_STM32N6_I2CTIMEOTICKS \
    (SEC2TICK(CONFIG_STM32N6_I2CTIMEOSEC) + MSEC2TICK(CONFIG_STM32N6_I2CTIMEOMS))

/* GPIO output setup for recovery reset */

#define I2C_OUTPUT (GPIO_OUTPUT | GPIO_FLOAT | GPIO_OPENDRAIN |\
                      GPIO_SPEED_50MHZ | GPIO_OUTPUT_SET)

#define MKI2C_OUTPUT(p) (((p) & (GPIO_PORT_MASK | GPIO_PIN_MASK)) | I2C_OUTPUT)

#define I2C_CR1_TXRX    (I2C_CR1_RXIE | I2C_CR1_TXIE)
#define I2C_CR1_ALLINTS (I2C_CR1_TXRX | I2C_CR1_TCIE | I2C_CR1_ERRIE)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Interrupt state */

enum stm32_intstate_e
{
  INTSTATE_IDLE = 0,      /* No I2C activity */
  INTSTATE_WAITING,       /* Waiting for completion of interrupt activity */
  INTSTATE_DONE,          /* Interrupt activity complete */
};

/* I2C Device hardware configuration */

struct stm32_i2c_config_s
{
  uint32_t base;              /* I2C base address */
  uint32_t clk_bit;           /* Clock enable bit */
  uint32_t rcc_ensr;          /* RCC enable set register */
  uint32_t rcc_encr;          /* RCC enable clear register */
  uint32_t scl_pin;           /* SCL pin config */
  uint32_t sda_pin;           /* SDA pin config */
#ifndef CONFIG_I2C_POLLED
  uint32_t ev_irq;            /* Event IRQ */
  uint32_t er_irq;            /* Error IRQ */
#endif
};

/* I2C Device Private Data */

struct stm32_i2c_priv_s
{
  const struct stm32_i2c_config_s *config;
  int refs;                    /* Reference count */
  mutex_t lock;                /* Mutual exclusion mutex */
#ifndef CONFIG_I2C_POLLED
  sem_t sem_isr;               /* Interrupt wait semaphore */
#endif
  volatile uint8_t intstate;   /* Interrupt handshake */

  uint8_t msgc;                /* Message count */
  struct i2c_msg_s *msgv;      /* Message list */
  uint8_t *ptr;                /* Current message buffer */
  uint32_t frequency;          /* Current I2C frequency */
  int dcnt;                    /* Current message bytes remaining */
  uint16_t flags;              /* Current message flags */
  bool astart;                 /* START sent */

#ifdef CONFIG_STM32N6_I2C_DMA
  volatile uint8_t rxresult;   /* Result of the RX DMA */
  volatile uint8_t txresult;   /* Result of the TX DMA */
  uint16_t rxch;               /* The RX DMA request number */
  uint16_t txch;               /* The TX DMA request number */
  DMA_HANDLE rxdma;            /* DMA channel handle for RX transfers */
  DMA_HANDLE txdma;            /* DMA channel handle for TX transfers */
  sem_t rxsem;                 /* Wait for RX DMA to complete */
  sem_t txsem;                 /* Wait for TX DMA to complete */
#endif

  uint32_t status;             /* End of transfer status */
};

/* I2C Device, Instance */

struct stm32_i2c_inst_s
{
  const struct i2c_ops_s  *ops;  /* Standard I2C operations */
  struct stm32_i2c_priv_s *priv; /* Common driver private data */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t stm32_i2c_getreg32(struct stm32_i2c_priv_s *priv,
                                          uint8_t offset);
static inline void stm32_i2c_putreg32(struct stm32_i2c_priv_s *priv,
                                      uint8_t offset, uint32_t value);
static inline void stm32_i2c_modifyreg32(struct stm32_i2c_priv_s *priv,
                                         uint8_t offset, uint32_t clearbits,
                                         uint32_t setbits);
static inline int  stm32_i2c_sem_waitdone(struct stm32_i2c_priv_s *priv);
static inline void stm32_i2c_sem_waitstop(struct stm32_i2c_priv_s *priv);
static void stm32_i2c_setclock(struct stm32_i2c_priv_s *priv,
                               uint32_t frequency);
static inline void stm32_i2c_sendstart(struct stm32_i2c_priv_s *priv);
static inline void stm32_i2c_sendstop(struct stm32_i2c_priv_s *priv);
static int stm32_i2c_isr_process(struct stm32_i2c_priv_s *priv);
#ifndef CONFIG_I2C_POLLED
static int stm32_i2c_isr(int irq, void *context, void *arg);
#endif
static int stm32_i2c_init(struct stm32_i2c_priv_s *priv);
static int stm32_i2c_deinit(struct stm32_i2c_priv_s *priv);

static int stm32_i2c_transfer(struct i2c_master_s *dev,
                              struct i2c_msg_s *msgs, int count);
#ifdef CONFIG_I2C_RESET
static int stm32_i2c_reset(struct i2c_master_s *dev);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_STM32N6_I2C1
static const struct stm32_i2c_config_s stm32_i2c1_config =
{
  .base          = STM32_I2C1_BASE,
  .clk_bit       = RCC_APB1LENR_I2C1EN,
  .rcc_ensr      = STM32_RCC_APB1LENSR,
  .rcc_encr      = STM32_RCC_APB1LENCR,
  .scl_pin       = GPIO_I2C1_SCL,
  .sda_pin       = GPIO_I2C1_SDA,
#ifndef CONFIG_I2C_POLLED
  .ev_irq        = STM32_IRQ_I2C1_EV,
  .er_irq        = STM32_IRQ_I2C1_ER
#endif
};

static struct stm32_i2c_priv_s stm32_i2c1_priv =
{
  .config        = &stm32_i2c1_config,
  .refs          = 0,
  .lock          = NXMUTEX_INITIALIZER,
#ifndef CONFIG_I2C_POLLED
  .sem_isr       = SEM_INITIALIZER(0),
#endif
  .intstate      = INTSTATE_IDLE,
  .msgc          = 0,
  .msgv          = NULL,
  .ptr           = NULL,
  .frequency     = 0,
  .dcnt          = 0,
  .flags         = 0,
#ifdef CONFIG_STM32N6_I2C_DMA
  .rxch          = 95, /* I2C1_RX_DMA */
  .txch          = 96, /* I2C1_TX_DMA */
  .rxsem         = SEM_INITIALIZER(0),
  .txsem         = SEM_INITIALIZER(0),
#endif
  .status        = 0,
};
#endif

/* Device Operations */

static const struct i2c_ops_s stm32_i2c_ops =
{
  .transfer      = stm32_i2c_transfer,
#ifdef CONFIG_I2C_RESET
  .reset         = stm32_i2c_reset,
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t stm32_i2c_getreg32(struct stm32_i2c_priv_s *priv,
                                          uint8_t offset)
{
  return getreg32(priv->config->base + offset);
}

static inline void stm32_i2c_putreg32(struct stm32_i2c_priv_s *priv,
                                      uint8_t offset, uint32_t value)
{
  putreg32(value, priv->config->base + offset);
}

static inline void stm32_i2c_modifyreg32(struct stm32_i2c_priv_s *priv,
                                         uint8_t offset, uint32_t clearbits,
                                         uint32_t setbits)
{
  modifyreg32(priv->config->base + offset, clearbits, setbits);
}

/* Wait done */

#ifndef CONFIG_I2C_POLLED
static inline int stm32_i2c_sem_waitdone(struct stm32_i2c_priv_s *priv)
{
  irqstate_t flags;
  int ret;

  flags = enter_critical_section();

  /* Enable I2C interrupts including TXIE/RXIE so data can flow */

  stm32_i2c_modifyreg32(priv, STM32_I2C_CR1_OFFSET, 0,
                        I2C_CR1_ALLINTS);

  priv->intstate = INTSTATE_WAITING;
  do
    {
      ret = nxsem_tickwait(&priv->sem_isr, CONFIG_STM32N6_I2CTIMEOTICKS);
      if (ret < 0)
        {
          /* Timeout or interrupt signal */

          break;
        }
    }
  while (priv->intstate != INTSTATE_DONE);

  /* Disable interrupts */

  stm32_i2c_modifyreg32(priv, STM32_I2C_CR1_OFFSET, I2C_CR1_ALLINTS, 0);

  priv->intstate = INTSTATE_IDLE;
  leave_critical_section(flags);
  return ret;
}
#else
static inline int stm32_i2c_sem_waitdone(struct stm32_i2c_priv_s *priv)
{
  clock_t start;
  clock_t elapsed;
  int ret;

  start = clock_systime_ticks();
  do
    {
      /* Poll for status */

      stm32_i2c_isr_process(priv);
      if (priv->intstate == INTSTATE_DONE)
        {
          break;
        }

      elapsed = clock_systime_ticks() - start;
    }
  while (elapsed < CONFIG_STM32N6_I2CTIMEOTICKS);

  ret = (priv->intstate == INTSTATE_DONE) ? OK : -ETIMEDOUT;
  priv->intstate = INTSTATE_IDLE;
  return ret;
}
#endif

static inline void stm32_i2c_sem_waitstop(struct stm32_i2c_priv_s *priv)
{
  clock_t start;
  clock_t elapsed;
  uint32_t cr;
  uint32_t sr;

  start = clock_systime_ticks();
  do
    {
      elapsed = clock_systime_ticks() - start;
      cr = stm32_i2c_getreg32(priv, STM32_I2C_CR2_OFFSET);
      if ((cr & I2C_CR2_STOP) == 0)
        {
          return;
        }

      sr = stm32_i2c_getreg32(priv, STM32_I2C_ISR_OFFSET);
      if ((sr & I2C_INT_TIMEOUT) != 0)
        {
          return;
        }
    }
  while (elapsed < CONFIG_STM32N6_I2CTIMEOTICKS);
}

/* Set SCL / SDA clock */

static void stm32_i2c_setclock(struct stm32_i2c_priv_s *priv,
                               uint32_t frequency)
{
  uint8_t presc;
  uint8_t scl_delay;
  uint8_t sda_delay;
  uint8_t scl_h_period;
  uint8_t scl_l_period;

  /* Disable peripheral during clock update */

  stm32_i2c_modifyreg32(priv, STM32_I2C_CR1_OFFSET, I2C_CR1_PE, 0);

  if (frequency != priv->frequency)
    {
      /* Support standard clock configurations:
       * Since I2C clock defaults to PCLK1 = 50MHz, we calculate TIMINGR
       * values based on a 50 MHz input clock frequency.
       */

      if (frequency == 100000)
        {
          /* 100 kHz Standard Mode timings with clock 50 MHz */

          presc        = 9;   /* PRESC = 9 (+1) -> divide 50MHz by 10 to get 5MHz */
          scl_delay    = 4;   /* SCLDEL = 4 (+1) -> SCL setup = 1.0 us */
          sda_delay    = 2;   /* SDADEL = 2 -> SDA hold = 400 ns */
          scl_h_period = 23;  /* SCLH = 23 (+1) -> SCL high = 4.8 us */
          scl_l_period = 25;  /* SCLL = 25 (+1) -> SCL low = 5.2 us */
        }
      else if (frequency == 400000)
        {
          /* 400 kHz Fast Mode timings with clock 50 MHz */

          presc        = 4;   /* PRESC = 4 (+1) -> divide 50MHz by 5 to get 10MHz */
          scl_delay    = 3;   /* SCLDEL = 3 (+1) -> SCL setup = 400 ns */
          sda_delay    = 1;   /* SDADEL = 1 -> SDA hold = 100 ns */
          scl_h_period = 7;   /* SCLH = 7 (+1) -> SCL high = 800 ns */
          scl_l_period = 15;  /* SCLL = 15 (+1) -> SCL low = 1.6 us */
        }
      else
        {
          /* Quietly fallback to 100 kHz */

          presc        = 9;
          scl_delay    = 4;
          sda_delay    = 2;
          scl_h_period = 23;
          scl_l_period = 25;
        }

      uint32_t timingr =
        ((uint32_t)presc << I2C_TIMINGR_PRESC_SHIFT) |
        ((uint32_t)scl_delay << I2C_TIMINGR_SCLDEL_SHIFT) |
        ((uint32_t)sda_delay << I2C_TIMINGR_SDADEL_SHIFT) |
        ((uint32_t)scl_h_period << I2C_TIMINGR_SCLH_SHIFT) |
        ((uint32_t)scl_l_period << I2C_TIMINGR_SCLL_SHIFT);

      stm32_i2c_putreg32(priv, STM32_I2C_TIMINGR_OFFSET, timingr);
      priv->frequency = frequency;
    }

  /* Enable peripheral */

  stm32_i2c_modifyreg32(priv, STM32_I2C_CR1_OFFSET, 0, I2C_CR1_PE);
}

/* Send Start */

static inline void stm32_i2c_sendstart(struct stm32_i2c_priv_s *priv)
{
  priv->ptr   = priv->msgv->buffer;
  priv->dcnt  = priv->msgv->length;
  priv->flags = priv->msgv->flags;

  if ((priv->flags & I2C_M_NOSTART) == 0)
    {
      priv->astart = true;
    }

  /* Setup NBYTES */

  uint32_t cr2 = stm32_i2c_getreg32(priv, STM32_I2C_CR2_OFFSET);
  cr2 &= ~(I2C_CR2_SADD10_MASK | I2C_CR2_RD_WRN | I2C_CR2_START | I2C_CR2_STOP | I2C_CR2_NBYTES_MASK);

  /* Set address */

  cr2 |= ((uint32_t)priv->msgv->addr << I2C_CR2_SADD7_SHIFT) & I2C_CR2_SADD7_MASK;

  /* Set transfer size */

  if (priv->dcnt > 255)
    {
      cr2 |= (255 << I2C_CR2_NBYTES_SHIFT);
      cr2 |= I2C_CR2_RELOAD;
    }
  else
    {
      cr2 |= ((uint32_t)priv->dcnt << I2C_CR2_NBYTES_SHIFT);
    }

  /* Set direction */

  if (priv->flags & I2C_M_READ)
    {
      cr2 |= I2C_CR2_RD_WRN;
    }

  /* Send START */

  cr2 |= I2C_CR2_START;
  stm32_i2c_putreg32(priv, STM32_I2C_CR2_OFFSET, cr2);
}

/* Send Stop */

static inline void stm32_i2c_sendstop(struct stm32_i2c_priv_s *priv)
{
  stm32_i2c_modifyreg32(priv, STM32_I2C_CR2_OFFSET, 0, I2C_CR2_STOP);
}

/* Interrupt status processor */

static int stm32_i2c_isr_process(struct stm32_i2c_priv_s *priv)
{
  uint32_t status = stm32_i2c_getreg32(priv, STM32_I2C_ISR_OFFSET);
  priv->status = status;

  /* Error Handling */

  if (status & I2C_ISR_ERRORMASK)
    {
      /* Clear errors */

      stm32_i2c_putreg32(priv, STM32_I2C_ICR_OFFSET, status & I2C_ISR_ERRORMASK);
      priv->dcnt = -1;
      priv->msgc = 0;
    }

  /* NACK Handling */

  if (status & I2C_INT_NACK)
    {
      /* Clear NACK */

      stm32_i2c_putreg32(priv, STM32_I2C_ICR_OFFSET, I2C_INT_NACK);
      priv->dcnt = -1;
      priv->msgc = 0;
    }

  /* TXIS handler: Tx Buffer empty and ready for next byte */

  else if (((priv->flags & I2C_M_READ) == 0) && (status & I2C_ISR_TXIS))
    {
      if (priv->astart == true)
        {
          priv->astart = false;
        }

      if (priv->dcnt > 0)
        {
          /* Write next byte to TXDR */

          stm32_i2c_putreg32(priv, STM32_I2C_TXDR_OFFSET, *priv->ptr);
          priv->ptr++;
          priv->dcnt--;
        }
    }

  /* RXNE handler: Rx Buffer not empty */

  else if ((priv->flags & I2C_M_READ) && (status & I2C_ISR_RXNE))
    {
      if (priv->dcnt > 0)
        {
          /* Read byte from RXDR */

          *priv->ptr = stm32_i2c_getreg32(priv, STM32_I2C_RXDR_OFFSET) & I2C_RXDR_MASK;
          priv->ptr++;
          priv->dcnt--;
        }
    }

  /* Transfer Complete (TC) */

  if (status & I2C_ISR_TC)
    {
      if (priv->dcnt == 0)
        {
          /* Finished current message, check for next */

          priv->msgc--;
          if (priv->msgc > 0)
            {
              priv->msgv++;
              stm32_i2c_sendstart(priv);
            }
          else
            {
              /* No more messages, issue STOP */

              stm32_i2c_sendstop(priv);
              priv->intstate = INTSTATE_DONE;
#ifndef CONFIG_I2C_POLLED
              nxsem_post(&priv->sem_isr);
#endif
            }
        }
    }

  /* Transfer Complete Reload (TCR) */

  else if (status & I2C_ISR_TCR)
    {
      if (priv->dcnt > 0)
        {
          /* Update NBYTES for reload mode */

          uint32_t cr2 = stm32_i2c_getreg32(priv, STM32_I2C_CR2_OFFSET);
          cr2 &= ~I2C_CR2_NBYTES_MASK;
          if (priv->dcnt > 255)
            {
              cr2 |= (255 << I2C_CR2_NBYTES_SHIFT);
            }
          else
            {
              cr2 &= ~I2C_CR2_RELOAD;
              cr2 |= ((uint32_t)priv->dcnt << I2C_CR2_NBYTES_SHIFT);
            }
          stm32_i2c_putreg32(priv, STM32_I2C_CR2_OFFSET, cr2);
        }
    }

  /* Finish transaction on errors/NACK */

  if (priv->dcnt < 0)
    {
      stm32_i2c_sendstop(priv);
      priv->intstate = INTSTATE_DONE;
#ifndef CONFIG_I2C_POLLED
      nxsem_post(&priv->sem_isr);
#endif
    }

  return OK;
}

#ifndef CONFIG_I2C_POLLED
static int stm32_i2c_isr(int irq, void *context, void *arg)
{
  struct stm32_i2c_priv_s *priv = (struct stm32_i2c_priv_s *)arg;
  return stm32_i2c_isr_process(priv);
}
#endif

#ifdef CONFIG_STM32N6_I2C_DMA
static void stm32_i2c_dmacallback(DMA_HANDLE handle, uint8_t status, void *arg)
{
  struct stm32_i2c_priv_s *priv = (struct stm32_i2c_priv_s *)arg;

  if (handle == priv->rxdma)
    {
      priv->rxresult = status;
      nxsem_post(&priv->rxsem);
    }
  else if (handle == priv->txdma)
    {
      priv->txresult = status;
      nxsem_post(&priv->txsem);
    }
}
#endif


/* Initialize Device */

static int stm32_i2c_init(struct stm32_i2c_priv_s *priv)
{
  /* Configure GPIO pins */

  stm32_configgpio(priv->config->scl_pin);
  stm32_configgpio(priv->config->sda_pin);

  /* Enable peripheral clock */

  putreg32(priv->config->clk_bit, priv->config->rcc_ensr);

#ifndef CONFIG_I2C_POLLED
  /* Attach and enable interrupts */

  irq_attach(priv->config->ev_irq, stm32_i2c_isr, priv);
  irq_attach(priv->config->er_irq, stm32_i2c_isr, priv);
  up_enable_irq(priv->config->ev_irq);
  up_enable_irq(priv->config->er_irq);
#endif

  /* Set default timings (100 kHz) */

  stm32_i2c_setclock(priv, 100000);

#ifdef CONFIG_STM32N6_I2C_DMA
  if (priv->rxch && priv->txch)
    {
      priv->rxdma = stm32_dmachannel(GPDMA_TTYPE_P2M);
      priv->txdma = stm32_dmachannel(GPDMA_TTYPE_M2P);
    }
#endif

  return OK;
}

/* De-initialize Device */

static int stm32_i2c_deinit(struct stm32_i2c_priv_s *priv)
{
  /* Disable peripheral */

  stm32_i2c_modifyreg32(priv, STM32_I2C_CR1_OFFSET, I2C_CR1_PE, 0);

#ifndef CONFIG_I2C_POLLED
  /* Disable interrupts */

  up_disable_irq(priv->config->ev_irq);
  up_disable_irq(priv->config->er_irq);
  irq_detach(priv->config->ev_irq);
  irq_detach(priv->config->er_irq);
#endif

  /* Disable clock gating */

  putreg32(priv->config->clk_bit, priv->config->rcc_encr);

  /* Unconfigure GPIO pins */

  stm32_unconfiggpio(priv->config->scl_pin);
  stm32_unconfiggpio(priv->config->sda_pin);

  return OK;
}

/* Perform transaction transfer */

static int stm32_i2c_transfer(struct i2c_master_s *dev,
                              struct i2c_msg_s *msgs, int count)
{
  struct stm32_i2c_priv_s *priv = ((struct stm32_i2c_inst_s *)dev)->priv;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  priv->msgc = count;
  priv->msgv = msgs;
  priv->astart = false;

  /* Select bus clock frequency */

  stm32_i2c_setclock(priv, msgs->frequency);

  /* Clear sticky flags */

  stm32_i2c_putreg32(priv, STM32_I2C_ICR_OFFSET, I2C_ICR_CLEARMASK);

#ifdef CONFIG_STM32N6_I2C_DMA
  if (priv->rxdma != NULL && priv->txdma != NULL && msgs->length > 4)
    {
      struct stm32_gpdma_cfg_s dmacfg;
      
      /* Trigger START sequence */
      stm32_i2c_sendstart(priv);

      if (msgs->flags & I2C_M_READ)
        {
          dmacfg.src_addr   = priv->config->base + STM32_I2C_RXDR_OFFSET;
          dmacfg.dest_addr  = (uint32_t)msgs->buffer;
          dmacfg.tr1        = GPDMA_CXTR1_DDW_LOG2_BYTE | GPDMA_CXTR1_SDW_LOG2_BYTE | GPDMA_CXTR1_DINC;
          dmacfg.request    = priv->rxch;
          dmacfg.ntransfers = msgs->length;
          dmacfg.priority   = GPMDACFG_PRIO_LH;
          dmacfg.mode       = 0;

          stm32_dmasetup(priv->rxdma, &dmacfg);
          
          if (msgs->buffer)
            {
              up_clean_dcache((uintptr_t)msgs->buffer, (uintptr_t)msgs->buffer + msgs->length);
              up_invalidate_dcache((uintptr_t)msgs->buffer, (uintptr_t)msgs->buffer + msgs->length);
            }
            
          stm32_i2c_modifyreg32(priv, STM32_I2C_CR1_OFFSET, 0, I2C_CR1_RXDMAEN);
          stm32_dmastart(priv->rxdma, stm32_i2c_dmacallback, priv, false);
          
          nxsem_wait_uninterruptible(&priv->rxsem);
          stm32_i2c_modifyreg32(priv, STM32_I2C_CR1_OFFSET, I2C_CR1_RXDMAEN, 0);
          
          if (msgs->buffer)
            {
              up_invalidate_dcache((uintptr_t)msgs->buffer, (uintptr_t)msgs->buffer + msgs->length);
            }
        }
      else
        {
          dmacfg.src_addr   = (uint32_t)msgs->buffer;
          dmacfg.dest_addr  = priv->config->base + STM32_I2C_TXDR_OFFSET;
          dmacfg.tr1        = GPDMA_CXTR1_DDW_LOG2_BYTE | GPDMA_CXTR1_SDW_LOG2_BYTE | GPDMA_CXTR1_SINC;
          dmacfg.request    = priv->txch | GPDMA_CXTR2_DREQ;
          dmacfg.ntransfers = msgs->length;
          dmacfg.priority   = GPMDACFG_PRIO_LH;
          dmacfg.mode       = 0;

          stm32_dmasetup(priv->txdma, &dmacfg);
          
          if (msgs->buffer)
            {
              up_clean_dcache((uintptr_t)msgs->buffer, (uintptr_t)msgs->buffer + msgs->length);
            }
            
          stm32_i2c_modifyreg32(priv, STM32_I2C_CR1_OFFSET, 0, I2C_CR1_TXDMAEN);
          stm32_dmastart(priv->txdma, stm32_i2c_dmacallback, priv, false);
          
          nxsem_wait_uninterruptible(&priv->txsem);
          stm32_i2c_modifyreg32(priv, STM32_I2C_CR1_OFFSET, I2C_CR1_TXDMAEN, 0);
        }

      /* Wait for Transfer Complete */
      while ((stm32_i2c_getreg32(priv, STM32_I2C_ISR_OFFSET) & I2C_ISR_TC) == 0);

      /* Send STOP */
      stm32_i2c_sendstop(priv);
      while ((stm32_i2c_getreg32(priv, STM32_I2C_ISR_OFFSET) & I2C_INT_STOP) == 0);
      stm32_i2c_putreg32(priv, STM32_I2C_ICR_OFFSET, I2C_INT_STOP);
      
      ret = OK;
    }
  else
#endif
    {
      /* Trigger START sequence */

      stm32_i2c_sendstart(priv);

      /* Wait for transfer to complete */

      ret = stm32_i2c_sem_waitdone(priv);

      /* Wait for bus stop to complete */

      stm32_i2c_sem_waitstop(priv);
    }

  /* Handle results */

  if (ret == OK && priv->status & I2C_ISR_ERRORMASK)
    {
      ret = -EIO;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

#ifdef CONFIG_I2C_RESET
static int stm32_i2c_reset(struct i2c_master_s *dev)
{
  struct stm32_i2c_priv_s *priv = ((struct stm32_i2c_inst_s *)dev)->priv;
  int ret;

  /* Force bus recovery reset using bit bang SCL toggling */

  nxmutex_lock(&priv->lock);

  /* Deinitialize and release GPIO control */

  stm32_i2c_deinit(priv);

  /* Set SCL to GPIO output OD and toggle it */

  uint32_t scl = MKI2C_OUTPUT(priv->config->scl_pin);
  stm32_configgpio(scl);

  for (int i = 0; i < 9; i++)
    {
      stm32_gpiowrite(scl, false);
      up_udelay(5);
      stm32_gpiowrite(scl, true);
      up_udelay(5);
    }

  /* Reinitialize bus */

  stm32_i2c_init(priv);

  nxmutex_unlock(&priv->lock);
  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct i2c_master_s *stm32_i2cbus_initialize(int port)
{
  struct stm32_i2c_priv_s *priv = NULL;
  struct stm32_i2c_inst_s *inst = NULL;

  switch (port)
    {
#ifdef CONFIG_STM32N6_I2C1
    case 1:
      priv = &stm32_i2c1_priv;
      break;
#endif
    default:
      return NULL;
    }

  nxmutex_lock(&priv->lock);

  if (priv->refs == 0)
    {
      /* Allocate and initialize hardware */

      stm32_i2c_init(priv);
    }
  priv->refs++;

  nxmutex_unlock(&priv->lock);

  /* Instantiate bus user structure */

  inst = (struct stm32_i2c_inst_s *)kmm_malloc(sizeof(struct stm32_i2c_inst_s));
  if (inst == NULL)
    {
      return NULL;
    }

  inst->ops  = &stm32_i2c_ops;
  inst->priv = priv;

  return (struct i2c_master_s *)inst;
}

int stm32_i2cbus_uninitialize(struct i2c_master_s *dev)
{
  struct stm32_i2c_priv_s *priv = ((struct stm32_i2c_inst_s *)dev)->priv;

  nxmutex_lock(&priv->lock);

  priv->refs--;
  if (priv->refs == 0)
    {
      /* Disable hardware */

      stm32_i2c_deinit(priv);
    }

  nxmutex_unlock(&priv->lock);

  kmm_free(dev);
  return OK;
}

#endif /* CONFIG_STM32N6_I2C1 || CONFIG_STM32N6_I2C2 || ... */
