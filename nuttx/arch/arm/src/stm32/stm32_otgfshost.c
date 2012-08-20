/*******************************************************************************
 * arch/arm/src/stm32/stm32_otgfshost.c
 *
 *   Copyright (C) 2012 Gregory Nutt. All rights reserved.
 *   Authors: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *******************************************************************************/

/*******************************************************************************
 * Included Files
 *******************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <semaphore.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbhost.h>

#include <arch/irq.h>

#include "chip.h"             /* Includes default GPIO settings */
#include <arch/board/board.h> /* May redefine GPIO settings */

#include "up_arch.h"
#include "up_internal.h"

#include "stm32_otgfs.h"

#if defined(CONFIG_USBHOST) && defined(CONFIG_STM32_OTGFS)

/*******************************************************************************
 * Definitions
 *******************************************************************************/

/* Configuration ***************************************************************/
/* Pre-requistites (partial) */

#ifndef CONFIG_STM32_SYSCFG
#  error "CONFIG_STM32_SYSCFG is required"
#endif

/* Default RxFIFO size */

#ifndef CONFIG_STM32_OTGFS_RXFIFO_SIZE
#  define CONFIG_STM32_OTGFS_RXFIFO_SIZE 128
#endif

/* Default host non-periodic transmit FIFO size */

#ifndef CONFIG_STM32_OTGFS_RXFIFO_SIZE
#  define CONFIG_STM32_OTGFS_RXFIFO_SIZE 128
#endif

/* Default host non-periodic transmit FIFO size */

#ifndef CONFIG_STM32_OTGFS_NPTXFIFO_SIZE
#  define CONFIG_STM32_OTGFS_NPTXFIFO_SIZE 96
#endif

/* Default the host periodic Tx fifo size register (HPTXFSIZ) */

#ifndef CONFIG_STM32_OTGFS_PTXFIFO_SIZE
#  define CONFIG_STM32_OTGFS_PTXFIFO_SIZE 96
#endif

/* HCD Setup *******************************************************************/
/* Hardware capabilities */

#define STM32_NHOST_CHANNELS      8   /* Number of host channels */
#define STM32_MAX_PACKET_SIZE     64  /* Full speed max packet size */
#define STM32_EP0_DEF_PACKET_SIZE 8   /* EP0 default packet size */
#define STM32_EP0_MAX_PACKET_SIZE 64  /* EP0 FS max packet size */
#define STM32_MAX_TX_FIFOS        15  /* Max number of TX FIFOs */
#define STM32_MAX_PKTCOUNT        256 /* Max packet count */
#define STM32_RETRY_COUNT         3   /* Number of retries */
#define STM32_DEF_DEVADDR         0   /* Default device address */

/* Delays **********************************************************************/

#define STM32_READY_DELAY            200000 /* In loop counts */
#define STM32_FLUSH_DELAY            200000 /* In loop counts */
#define STM32_NOTREADY_DELAY         5000   /* In frames */

/* Ever-present MIN/MAX macros */

#ifndef MIN
#  define  MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#  define  MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

/*******************************************************************************
 * Private Types
 *******************************************************************************/

/* The following enumeration represents the various states of the USB host
 * state machine (for debug purposes only)
 */

enum stm32_smstate_e
{
  SMSTATE_DETACHED = 0,  /* Not attached to a device */
  SMSTATE_ATTACHED,      /* Attached to a device */
  SMSTATE_ENUM,          /* Attached, enumerating */
  SMSTATE_CLASS_BOUND,   /* Enumeration complete, class bound */
};

/* This enumeration represents the state of one TX channel */

enum stm32_chstate_e
{
  CHSTATE_IDLE = 0,      /* Inactive (initial state) */
  CHSTATE_XFRC,          /* Transfer complete */
  CHSTATE_NAK,           /* NAK received */
  CHSTATE_NYET,          /* NotYet received */
  CHSTATE_STALL,         /* Endpoint stalled */
  CHSTATE_TXERR,         /* Transfer error received */
  CHSTATE_DTERR          /* Data error received */
};

/* This structure retains the state of one host channel */

struct stm32_chan_s
{
  sem_t             waitsem;   /* Channel wait semaphore */
  volatile uint8_t  result;    /* The result of the transfer */
  volatile uint8_t  chstate;   /* See enum stm32_chstate_e */
  uint8_t           epno;      /* Device endpoint number (0-127) */
  uint8_t           eptype;    /* See OTGFS_EPTYPE_* definitions */
  uint8_t           pid;       /* Data PID */
  bool              inuse;     /* True: This channel is "in use" */
  bool              indata1;   /* IN data toggle. True: DATA01  */
  bool              outdata1;  /* OUT data toggle.  True: DATA01 */
  bool              isin;      /* True: IN endpoint */
  volatile bool     waiter;    /* True: Thread is waiting for a channel event */
  volatile uint16_t nerrors;   /* Number of errors detecgted */
  volatile uint16_t xfrd;      /* Number of bytes transferred */
  uint16_t          maxpacket; /* Max packet size */
  uint16_t          buflen;    /* Buffer length (remaining) */
  uint16_t          xfrlen;    /* Number of bytes transferrred */
  FAR uint8_t      *buffer;    /* Transfer buffer pointer */
};

/* This structure retains the state of the USB host controller */

struct stm32_usbhost_s
{
  /* Common device fields.  This must be the first thing defined in the
   * structure so that it is possible to simply cast from struct usbhost_s
   * to structstm32_usbhost_s.
   */

  struct usbhost_driver_s drvr;

  /* The bound device class driver */

  struct usbhost_class_s *class;

  /* Overall driver status */

  uint8_t           smstate;   /* The state of the USB host state machine */
  uint8_t           devaddr;   /* Device address */
  uint8_t           ep0in;     /* EP0 IN control channel index */
  uint8_t           ep0out;    /* EP0 OUT control channel index */
  uint8_t           ep0size;   /* EP0 max packet size */
  bool              lowspeed;  /* True: low speed device */
  volatile bool     connected; /* Connected to device */
  volatile bool     eventwait; /* True: Thread is waiting for a port event */
  sem_t             exclsem;   /* Support mutually exclusive access */
  sem_t             eventsem;  /* Semaphore to wait for a port event */

  /* The state of each host channel */

  struct stm32_chan_s chan[STM32_MAX_TX_FIFOS];
};

/*******************************************************************************
 * Private Function Prototypes
 *******************************************************************************/

/* Register operations ********************************************************/

#ifdef CONFIG_STM32_USBHOST_REGDEBUG
static void stm32_printreg(uint32_t addr, uint32_t val, bool iswrite);
static void stm32_checkreg(uint32_t addr, uint32_t val, bool iswrite);
static uint32_t stm32_getreg(uint32_t addr);
static void stm32_putreg(uint32_t addr, uint32_t value);
#else
# define stm32_getreg(addr)     getreg32(addr)
# define stm32_putreg(val,addr) putreg32(val,addr)
#endif

static inline void stm32_modifyreg(uint32_t addr, uint32_t clrbits,
                                   uint32_t setbits);

/* Semaphores ******************************************************************/

static void stm32_takesem(sem_t *sem);
#define stm32_givesem(s) sem_post(s);

/* Byte stream access helper functions *****************************************/

static inline uint16_t stm32_getle16(const uint8_t *val);
static void stm32_putle16(uint8_t *dest, uint16_t val);

/* Channel management **********************************************************/

static int stm32_chan_alloc(FAR struct stm32_usbhost_s *priv);
static void stm32_chan_free(FAR struct stm32_usbhost_s *priv, int chidx);
static inline void stm32_chan_freeall(FAR struct stm32_usbhost_s *priv);
static void stm32_chan_configure(FAR struct stm32_usbhost_s *priv, int chidx);
static int stm32_chan_waitsetup(FAR struct stm32_usbhost_s *priv,
                                FAR struct stm32_chan_s *chan);
static void stm32_chan_wait(FAR struct stm32_usbhost_s *priv,
                            FAR struct stm32_chan_s *chan);
static void stm32_chan_wakeup(FAR struct stm32_usbhost_s *priv,
                              FAR struct stm32_chan_s *chan);

/* Control/data transfer logic *************************************************/

static void stm32_transfer_start(FAR struct stm32_usbhost_s *priv, int chidx);
static inline uint32_t stm32_getframe(void);
static int stm32_ctrl_sendsetup(FAR struct stm32_usbhost_s *priv,
                                FAR const struct usb_ctrlreq_s *req);
static int stm32_ctrl_senddata(FAR struct stm32_usbhost_s *priv,
                               FAR uint8_t *buffer, unsigned int buflen);
static int stm32_ctrl_recvdata(FAR struct stm32_usbhost_s *priv,
                               FAR uint8_t *buffer, unsigned int buflen);

/* Interrupt handling **********************************************************/
/* Lower level interrupt handlers */

static void stm32_gint_wrpacket(FAR struct stm32_usbhost_s *priv,
                                FAR uint8_t *buffer, int chidx, int buflen);
static void stm32_gint_halttxchan(FAR struct stm32_usbhost_s *priv, int chidx);
static inline void stm32_gint_hcinisr(FAR struct stm32_usbhost_s *priv,
                                      int chidx);
static inline void stm32_gint_hcoutisr(FAR struct stm32_usbhost_s *priv,
                                       int chidx);
static void stm32_gint_connected(FAR struct stm32_usbhost_s *priv);
static void stm32_gint_disconnected(FAR struct stm32_usbhost_s *priv);

/* Second level interrupt handlers */

#ifdef CONFIG_STM32_OTGFS_SOFINTR
static inline void stm32_gint_sofisr(FAR struct stm32_usbhost_s *priv);
#endif
static inline void stm32_gint_rxflvlisr(FAR struct stm32_usbhost_s *priv);
static inline void stm32_gint_nptxfeisr(FAR struct stm32_usbhost_s *priv);
static inline void stm32_gint_ptxfeisr(FAR struct stm32_usbhost_s *priv);
static inline void stm32_gint_hcisr(FAR struct stm32_usbhost_s *priv);
static inline void stm32_gint_hprtisr(FAR struct stm32_usbhost_s *priv);
static inline void stm32_gint_discisr(FAR struct stm32_usbhost_s *priv);
static inline void stm32_gint_iisooxfrisr(FAR struct stm32_usbhost_s *priv);

/* First level, global interrupt handler */

static int stm32_gint_isr(int irq, FAR void *context);

/* Interrupt controls */

static void stm32_gint_enable(void);
static void stm32_gint_disable(void);
static inline int stm32_hostinit_enable(void);

/* USB host controller operations **********************************************/

static int stm32_wait(FAR struct usbhost_driver_s *drvr, bool connected);
static int stm32_enumerate(FAR struct usbhost_driver_s *drvr);
static int stm32_ep0configure(FAR struct usbhost_driver_s *drvr, uint8_t funcaddr,
                              uint16_t maxpacketsize);
static int stm32_epalloc(FAR struct usbhost_driver_s *drvr,
                         const FAR struct usbhost_epdesc_s *epdesc, usbhost_ep_t *ep);
static int stm32_epfree(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep);
static int stm32_alloc(FAR struct usbhost_driver_s *drvr,
                       FAR uint8_t **buffer, FAR size_t *maxlen);
static int stm32_free(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer);
static int stm32_ioalloc(FAR struct usbhost_driver_s *drvr,
                         FAR uint8_t **buffer, size_t buflen);
static int stm32_iofree(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer);
static int stm32_ctrlin(FAR struct usbhost_driver_s *drvr,
                        FAR const struct usb_ctrlreq_s *req,
                        FAR uint8_t *buffer);
static int stm32_ctrlout(FAR struct usbhost_driver_s *drvr,
                         FAR const struct usb_ctrlreq_s *req,
                         FAR const uint8_t *buffer);
static int stm32_transfer(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep,
                          FAR uint8_t *buffer, size_t buflen);
static void stm32_disconnect(FAR struct usbhost_driver_s *drvr);

/* Initialization **************************************************************/

static inline void stm32_ep0init(FAR struct stm32_usbhost_s *priv);
static void stm32_portreset(FAR struct stm32_usbhost_s *priv);
static inline void stm32_flush_txfifos(uint32_t txfnum);
static inline void stm32_flush_rxfifo(void);
static void stm32_vbusdrive(FAR struct stm32_usbhost_s *priv, bool state);
static void stm32_host_initialize(FAR struct stm32_usbhost_s *priv);
static inline void stm32_sw_initialize(FAR struct stm32_usbhost_s *priv);
static inline int stm32_hw_initialize(FAR struct stm32_usbhost_s *priv);

/*******************************************************************************
 * Private Data
 *******************************************************************************/

/* In this driver implementation, support is provided for only a single a single
 * USB device.  All status information can be simply retained in a single global
 * instance.
 */

static struct stm32_usbhost_s g_usbhost =
{
  .drvr             =
    {
      .wait         = stm32_wait,
      .enumerate    = stm32_enumerate,
      .ep0configure = stm32_ep0configure,
      .epalloc      = stm32_epalloc,
      .epfree       = stm32_epfree,
      .alloc        = stm32_alloc,
      .free         = stm32_free,
      .ioalloc      = stm32_ioalloc,
      .iofree       = stm32_iofree,
      .ctrlin       = stm32_ctrlin,
      .ctrlout      = stm32_ctrlout,
      .transfer     = stm32_transfer,
      .disconnect   = stm32_disconnect,
    },
  .class            = NULL,
};

/* This is a free list of EDs and TD buffers */

static struct stm32_list_s *g_edfree; /* List of unused EDs */
static struct stm32_list_s *g_tdfree; /* List of unused TDs */
static struct stm32_list_s *g_tbfree; /* List of unused transfer buffers */
#if STM32_IOBUFFERS > 0
static struct stm32_list_s *g_iofree; /* List of unused I/O buffers */
#endif

/*******************************************************************************
 * Public Data
 *******************************************************************************/

/*******************************************************************************
 * Private Functions
 *******************************************************************************/

/*******************************************************************************
 * Name: stm32_printreg
 *
 * Description:
 *   Print the contents of an STM32xx register operation
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_USBHOST_REGDEBUG
static void stm32_printreg(uint32_t addr, uint32_t val, bool iswrite)
{
  lldbg("%08x%s%08x\n", addr, iswrite ? "<-" : "->", val);
}
#endif

/*******************************************************************************
 * Name: stm32_checkreg
 *
 * Description:
 *   Get the contents of an STM32 register
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_USBHOST_REGDEBUG
static void stm32_checkreg(uint32_t addr, uint32_t val, bool iswrite)
{
  static uint32_t prevaddr = 0;
  static uint32_t preval = 0;
  static uint32_t count = 0;
  static bool     prevwrite = false;

  /* Is this the same value that we read from/wrote to the same register last time?
   * Are we polling the register?  If so, suppress the output.
   */

  if (addr == prevaddr && val == preval && prevwrite == iswrite)
    {
      /* Yes.. Just increment the count */

      count++;
    }
  else
    {
      /* No this is a new address or value or operation. Were there any
       * duplicate accesses before this one?
       */

      if (count > 0)
        {
          /* Yes.. Just one? */

          if (count == 1)
            {
              /* Yes.. Just one */

              stm32_printreg(prevaddr, preval, prevwrite);
            }
          else
            {
              /* No.. More than one. */

              lldbg("[repeats %d more times]\n", count);
            }
        }

      /* Save the new address, value, count, and operation for next time */

      prevaddr  = addr;
      preval    = val;
      count     = 0;
      prevwrite = iswrite;

      /* Show the new regisgter access */

      stm32_printreg(addr, val, iswrite);
    }
}
#endif

/*******************************************************************************
 * Name: stm32_getreg
 *
 * Description:
 *   Get the contents of an STM32 register
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_USBHOST_REGDEBUG
static uint32_t stm32_getreg(uint32_t addr)
{
  /* Read the value from the register */

  uint32_t val = getreg32(addr);

  /* Check if we need to print this value */

  stm32_checkreg(addr, val, false);
  return val;
}
#endif

/*******************************************************************************
 * Name: stm32_putreg
 *
 * Description:
 *   Set the contents of an STM32 register to a value
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_USBHOST_REGDEBUG
static void stm32_putreg(uint32_t val, uint32_t addr)
{
  /* Check if we need to print this value */

  stm32_checkreg(addr, val, true);

  /* Write the value */

  putreg32(val, addr);
}
#endif

/*******************************************************************************
 * Name: stm32_modifyreg
 *
 * Description:
 *   Modify selected bits of an STM32 register.
 *
 *******************************************************************************/

static inline void stm32_modifyreg(uint32_t addr, uint32_t clrbits, uint32_t setbits)
{
  stm32_putreg(addr, (((stm32_getreg(addr)) & ~clrbits) | setbits));
}

/****************************************************************************
 * Name: stm32_takesem
 *
 * Description:
 *   This is just a wrapper to handle the annoying behavior of semaphore
 *   waits that return due to the receipt of a signal.
 *
 *******************************************************************************/

static void stm32_takesem(sem_t *sem)
{
  /* Take the semaphore (perhaps waiting) */

  while (sem_wait(sem) != 0)
    {
      /* The only case that an error should occr here is if the wait was
       * awakened by a signal.
       */

      ASSERT(errno == EINTR);
    }
}

/****************************************************************************
 * Name: stm32_getle16
 *
 * Description:
 *   Get a (possibly unaligned) 16-bit little endian value.
 *
 *******************************************************************************/

static inline uint16_t stm32_getle16(const uint8_t *val)
{
  return (uint16_t)val[1] << 8 | (uint16_t)val[0];
}

/****************************************************************************
 * Name: stm32_putle16
 *
 * Description:
 *   Put a (possibly unaligned) 16-bit little endian value.
 *
 *******************************************************************************/

static void stm32_putle16(uint8_t *dest, uint16_t val)
{
  dest[0] = val & 0xff; /* Little endian means LS byte first in byte stream */
  dest[1] = val >> 8;
}

/*******************************************************************************
 * Name: stm32_chan_alloc
 *
 * Description:
 *   Allocate a channel.
 *
 *******************************************************************************/

static int stm32_chan_alloc(FAR struct stm32_usbhost_s *priv)
{
  int chidx;

  /* Search the table of channels */

  for (chidx = 0 ; chidx < STM32_NHOST_CHANNELS ; chidx++)
    {
      /* Is this channel available? */

      if (!priv->chan[chidx].inuse)
        {
          /* Yes... make it "in use" and return the index */

          priv->chan[chidx].inuse = true;
          return chidx;
        }
    }

  /* All of the channels are "in-use" */

  return -EBUSY;
}

/*******************************************************************************
 * Name: stm32_chan_free
 *
 * Description:
 *   Free a previoiusly allocated channel.
 *
 *******************************************************************************/

static void stm32_chan_free(FAR struct stm32_usbhost_s *priv, int chidx)
{
  DEBUGASSERT((unsigned)chidx < STM32_NHOST_CHANNELS);
  priv-priv->chan[chidx].inuse = false;
}

/*******************************************************************************
 * Name: stm32_chan_freeall
 *
 * Description:
 *   Free all channels.
 *
 *******************************************************************************/

static inline void stm32_chan_freeall(FAR struct stm32_usbhost_s *priv)
{
   uint8_t chidx;

   /* Clear address an "in-use" flag for all host channels */

   for (chidx = 2; chidx < STM32_NHOST_CHANNELS ; chidx ++)
     {
       priv->chan[chidx].inuse = false;
     }
}

/*******************************************************************************
 * Name: stm32_chan_configure
 *
 * Description:
 *   Configure or re-configure a host channel.  Host channels are configured
 *   when endpoint is allocated and EP0 (only) is re-configured with the
 *   max packet size or device address changes.
 *
 *******************************************************************************/

static void stm32_chan_configure(FAR struct stm32_usbhost_s *priv, int chidx)
{
  uint32_t regval;

  /* Clear any old pending interrupts for this host channel. */

  stm32_putreg(STM32_OTGFS_HCINT(chidx), 0xffffffff);

  /* Enable channel interrupts required for transfers on this channel. */

  regval = 0;

  switch (priv->chan[chidx].eptype)
    {
    case OTGFS_EPTYPE_CTRL:
    case OTGFS_EPTYPE_BULK:
      {
        /* Interrupts required for CTRL and BULK endpoints */

        regval |= (OTGFS_HCINT_XFRC  | OTGFS_HCINT_STALL | OTGFS_HCINT_NAK |
                   OTGFS_HCINT_TXERR | OTGFS_HCINT_DTERR);

        /* Additional setting for IN/OUT endpoints */

        if (priv->chan[chidx].isin)
          {
            regval |= OTGFS_HCINT_BBERR;
          }
        else
          {
            regval |= OTGFS_HCINT_NYET;
          }
      }
      break;

    case OTGFS_EPTYPE_INTR:
      {
        /* Interrupts required for INTR endpoints */

        regval |= (OTGFS_HCINT_XFRC | OTGFS_HCINT_STALL | OTGFS_HCINT_NAK |
                   OTGFS_HCINT_TXERR | OTGFS_HCINT_FRMOR | OTGFS_HCINT_DTERR);

        /* Additional setting for IN endpoints */

        if (priv->chan[chidx].isin)
          {
            regval |= OTGFS_HCINT_BBERR;
          }
      }
      break;

    case OTGFS_EPTYPE_ISOC:
      {
        /* Interrupts required for ISOC endpoints */

        regval |= (OTGFS_HCINT_XFRC | OTGFS_HCINT_ACK | OTGFS_HCINT_FRMOR);

        /* Additional setting for IN endpoints */

        if (priv->chan[chidx].isin)
          {
            regval |= (OTGFS_HCINT_TXERR | OTGFS_HCINT_BBERR);
          }
      }
      break;
    }

  stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

  /* Enable the top level host channel interrupt. */
 
  stm32_modifyreg(STM32_OTGFS_HAINTMSK, 0, OTGFS_HAINT(chidx));

  /* Make sure host channel interrupts are enabled. */

  stm32_modifyreg(STM32_OTGFS_GINTMSK, 0, OTGFS_GINT_HC);

  /* Program the HCCHAR register */

  regval = ((uint32_t)priv->chan[chidx].maxpacket << OTGFS_HCCHAR_MPSIZ_SHIFT) |
           ((uint32_t)priv->chan[chidx].epno << OTGFS_HCCHAR_EPNUM_SHIFT) |
           ((uint32_t)priv->chan[chidx].eptype << OTGFS_HCCHAR_EPTYP_SHIFT) |
           ((uint32_t)priv->devaddr << OTGFS_HCCHAR_DAD_SHIFT) |

  /* Special case settings for low speed devices */

  if (priv->lowspeed)
    {
      regval |= OTGFS_HCCHAR_LSDEV;
    }

  /* Special case settings for IN endpoints */

  if (priv->chan[chidx].isin)
    {
      regval |= OTGFS_HCCHAR_EPDIR_IN;
    }

  /* Special case settings for INTR endpoints */

  if (priv->chan[chidx].eptype == OTGFS_EPTYPE_INTR)
    {
      regval |= OTGFS_HCCHAR_ODDFRM;
    }

  /* Write the channel configuration */

  stm32_putreg(STM32_OTGFS_HCCHAR(chidx), regval);
}

/*******************************************************************************
 * Name: stm32_chan_waitsetup
 *
 * Description:
 *   Set the request for the transfer complete event well BEFORE enabling the
 *   transfer (as soon as we are absolutely committed to the to avoid transfer).
 *   We do this to minimize race conditions.  This logic would have to be expanded
 *   if we want to have more than one packet in flight at a time!
 *
 * Assumptions:
 *   Called from a normal thread context BEFORE the transfer has been started.
 *
 *******************************************************************************/

static int stm32_chan_waitsetup(FAR struct stm32_usbhost_s *priv,
                                FAR struct stm32_chan_s *chan)
{
  irqstate_t flags = irqsave();
  int        ret   = -ENODEV;

  /* Is the device still connected? */

  if (priv->connected)
    {
      /* Yes.. then set waiter to indicate that we expect to be informed when
       * either (1) the device is disconnected, or (2) the transfer completed.
       */

      chan->waiter = true;
      ret          = OK;
    }

  irqrestore(flags);
  return ret;
}

/*******************************************************************************
 * Name: stm32_chan_wait
 *
 * Description:
 *   Wait for a transfer on a channel to complete.
 *
 * Assumptions:
 *   Called from a normal thread context
 *
 *******************************************************************************/

static void stm32_chan_wait(FAR struct stm32_usbhost_s *priv,
                            FAR struct stm32_chan_s *chan)
{
  irqstate_t flags;
  int ret;

  /* Disable interrupts so that the following operations will be atomic.  On
   * the OTG FS global interrupt needs to be disabled.  However, here we disable
   * all interrupts to exploit that fact that interrupts will be re-enabled
   * while we wait.
   */

  flags = irqsave();

  /* Loop, testing for an end of transfer conditino.  The channel 'result'
   * was set to EBUSY and 'waiter' was set to true before the transfer; 'waiter'
   * will be set to false and 'result' will be set appropriately when the
   * tranfer is completed.
   */

  do
    {
      /* Wait for the transfer to complete.  NOTE the transfer may already
       * completed before we get here or the transfer may complete while we
       * wait here.
       */

      ret = sem_wait(chan->waitsem);

      /* sem_wait should succeeed.  But it is possible that we could be
       * awakened by a signal too.
       */

      DEBUGASSERT(ret == OK || get_errno() == EINTR);
    }
  while (chan->waiter);

  /* The transfer is complete */
  
  irqrestore(flags);
}

/*******************************************************************************
 * Name: stm32_chan_wakeup
 *
 * Description:
 *   A channel transfer has completed... wakeup any threads waiting for the
 *   transfer to complete.
 *
 * Assumptions:
 *   This function is called from the transfer complete interrupt handler for
 *   the channel.  Interrupts are disabled.
 *
 *******************************************************************************/

static void stm32_chan_wakeup(FAR struct stm32_usbhost_s *priv,
                              FAR struct stm32_chan_s *chan)
{
  /* Is there a thread waiting for this transfer to complete? */

  if (priv->chan[chidx].result != EBUSY && chan->waiter)
    {
      stm32_givesem(&chan->waitsem);
      chan->waiter = false;
    }
}

/*******************************************************************************
 * Name: stm32_transfer_start
 *
 * Description:
 *   Start at transfer on the select IN or OUT channel.
 *
 *******************************************************************************/

static void stm32_transfer_start(FAR struct stm32_usbhost_s *priv, int chidx)
{
  uint32_t regval;
  unsigned int npackets;
  unsigned int maxpacket;
  unsigned int buflen32;
  unsigned int avail32;

  /* Set up the initial state of the transfer */

  priv->chan[chidx].result = EBUSY;
  priv->chan[chidx].xfrlen = 0;

  /* Compute the expected number of packets associated to the transfer.
   * If it is a IN endpoint, then we need to round down to the number
   * of full, maximally sized packets that can fit in the read buffer.
   */

  mxpacket = priv->chan[chidx].maxpacket;
  if (priv->chan[chidx].isin)
    {
      npackets = priv->chan[chidx].buflen / maxpacket;

      /* A read buffer must at least one max packet size in length */

      DEBUGASSERT(npackets > 0);

      /* For the buffer length to an even multiple of maxpacket */

      priv->chan[chidx].buflen = npackets * maxpacket;
    }

  /* If it is an OUT endpoint, then we need round up to include any
   * possible partial, final packet.
   */

  else if (priv->chan[chidx].buflen > 0)
    {
      npackets = (priv->chan[chidx].buflen + maxpacket - 1) / maxpacket;
    }

  /* Otherwise, we are sending a single zero-length packet */

  else
    {
      npackets = 1;
    }

  /* Clip it the buffer length if it exceeds the maximum number of
   * packets that can be transferred (this should not happen).
   */

  if (npackets > STM32_MAX_PKTCOUNT)
    {
      npackets = STM32_MAX_PKTCOUNT;
      priv->chan[chidx].buflen = STM32_MAX_PKTCOUNT * maxpacket;
    }

  /* Setup the HCTSIZn register */

  regval = ((uint32_t)priv->chan[chidx].buflen << OTGFS_HCTSIZ_XFRSIZ_SHIFT) |
           ((uint32_t)npackets << OTGFS_HCTSIZ_PKTCNT_SHIFT) |
           ((uint32_t)priv->chan[chidx].pid << OTGFS_HCTSIZ_DPID_SHIFT);
  stm32_putreg(STM32_OTGFS_HCTSIZ(chidx), regval);

  /* Setup the HCCHAR register: Frame oddness and host channel enable */

  regval = stm32_getreg(STM32_OTGFS_HCCHAR(chidx));

  /* Check for an even frame */

  if ((stm32_getreg(STM32_OTGFS_HFNUM) & 1) == 0)
    {
      regval |= OTGFS_HCCHAR_ODDFRM;
    }

  regval &= ~OTGFS_HCCHAR_CHDIS;
  regval |= OTGFS_HCCHAR_CHENA;
  stm32_putreg(STM32_OTGFS_HCCHAR(chidx), regval);

  /* If this is an out transfer, then we need to do more.. we need to copy the
   * outgoing data into the correct TxFIFO.
   */

  if (!priv->chan[chidx].isin && priv->chan[chidx].buflen > 0)
    {
      /* Handle non-periodic (CTRL and BULK) OUT transfers differently than
       * perioci (INTR and ISOC) OUT transfers.
       */

      buflen32 = (priv->chan[chidx].buflen + 3) >> 2;
      switch (priv->chan[chidx].eptype)
        {
        case OTGFS_EPTYPE_CTRL: /* Non periodic transfer */
        case OTGFS_EPTYPE_BULK:
          {
            /* Read the Non-periodic Tx FIFO status register */

            regval  = stm32_getreg(STM32_OTGFS_HNPTXSTS);
            avail32 = (regval & OTGFS_HNPTXSTS_NPTXFSAV_MASK) >> OTGFS_HNPTXSTS_NPTXFSAV_SHIFT;

            /* Check if there is enough space in the Tx FIFO space */

            if (buflen32 > avail32)
              {
                /* Insufficient space... Enable the Non-periodic Tx FIFO
                 * interrupt to handle the transfer when the Tx FIFO is empty.
                 */

                stm32_modifyreg(STM32_OTGFS_GINTMSK, 0, OTGFS_GINT_NPTXFE);
            }
          }
          break;

          /* Periodic transfer */

          case OTGFS_EPTYPE_INTR:
          case OTGFS_EPTYPE_ISOC:
            /* Read the Non-periodic Tx FIFO status register */

            regval  = stm32_getreg(STM32_OTGFS_HPTXSTS);
            avail32 = (regval & OTGFS_HPTXSTS_PTXFSAVL_MASK) >> OTGFS_HPTXSTS_PTXFSAVL_SHIFT;

            /* Check if there is enough space in FIFO space */

            if (buflen32 > avail32)
              {
                /* Need to process data in OTGFS_GINT_PTXFE interrupt */

                stm32_modifyreg(STM32_OTGFS_GINTMSK, 0, OTGFS_GINT_PTXFE);
              }
            break;

          default:
            break;
          }

        /* Write packet into the Tx FIFO. */

        stm32_gint_wrpacket(priv, priv->chan[chidx].buffer, chidx, priv->chan[chidx].buflen);
    }
}


/*******************************************************************************
 * Name: stm32_getframe
 *
 * Description:
 *   Get the current frame number.
 *
 *******************************************************************************/

static inline uint16_t stm32_getframe(void)
{
  return (stm32_getreg(STM32_OTGFS_HFNUM) & OTGFS_HFNUM_FRNUM_MASK);
}

/*******************************************************************************
 * Name: stm32_ctrl_sendsetup
 *
 * Description:
 *   Send an IN/OUT SETUP packet.
 *
 *******************************************************************************/

static int stm32_ctrl_sendsetup(FAR struct stm32_usbhost_s *priv,
                                FAR const struct usb_ctrlreq_s *req)
{
  FAR struct stm32_chan_s *chan;
  uint16_t start = stm32_getframe();
  uint16_t elapsed;
  int ret;

  chan = &priv->chan[priv->ep0out];

  /* Loop while the device reports NAK (and a timeout is not exceeded */

  do
    {
      /* Send the  SETUP packet */

      chan->pid    = OTGFS_PID_SETUP;
      chan->buffer = (FAR uint8_t *)req;
      chan->buflen = USB_SIZEOF_CTRLREQ;

      /* Set up for the wait BEFORE starting the transfer */

      ret = stm32_chan_waitsetup(priv, chan)
      if (ret != OK)
        {
          udbg("ERROR: Device disconnected\n");
          return ret;
        }

      /* Start the transfer */

      stm32_transfer_start(priv, priv->ep0out);

      /* Wait for the transfer to complete */

      stm32_chan_wait(priv, chan);

      /* Get the result of the transfer */

      ret = -(int)chan->result;
      if (ret != -EGAIN)
        {
          return ret;
        }

     /* Get the elpased time (in frames) */

     elapsed = stm32_getframe() - start;
    }
  while (elapsed < STM32_NOTREADY_DELAY);

  return -ETIMEDOUT;
}

/*******************************************************************************
 * Name: stm32_ctrl_senddata
 *
 * Description:
 *   Send data in the data phase of an OUT control transfer.  Or send status
 *   in the status phase of an IN control transfer
 *
 *******************************************************************************/

static int stm32_ctrl_senddata(FAR struct stm32_usbhost_s *priv,
                               FAR uint8_t *buffer, unsigned int buflen)
{
  FAR struct stm32_chan_s *chan = &priv->chan[priv->ep0out];

  /* Save buffer information */

  chan->buffer = buffer;
  chan->buflen = buflen;

  /* Set the DATA PID */

  if (buflen == 0)
    {
      /* For status OUT stage with buflen == 0, set PID DATA1 */

      chan->outdata1 = true;
    }

  /* Set the Data PID as per the outdata1 boolean */

  if (!chan->outdata1)
    {
      /* Use PID == DATA0 */

      chan->pid = OTGFS_PID_DATA0;
    }
  else
    {
      /* Put the PID 1 */

      chan->pid = OTGFS_PID_DATA1 ;
    }

  /* Set up for the wait BEFORE starting the transfer */

  ret = stm32_chan_waitsetup(priv, chan)
  if (ret != OK)
    {
      udbg("ERROR: Device disconnected\n");
      return ret;
    }

  /* Start the transfer */

  stm32_transfer_start(priv, priv->ep0out);

  /* Wait for the transfer to complete */

  stm32_chan_wait(priv, chan);

  /* Return the result of the transfer */

  return -(int)chan->result;
}

/*******************************************************************************
 * Name: stm32_ctrl_recvdata
 *
 * Description:
 *   Receive data in the data phase of an IN control transfer.  Or receive status
 *   in the status phase of an OUT control transfer
 *
 *******************************************************************************/

static int stm32_ctrl_recvdata(FAR struct stm32_usbhost_s *priv,
                               FAR uint8_t *buffer, unsigned int buflen);
{
  FAR struct stm32_chan_s *chan = &priv->chan[pric->ep0in];

  /* Save buffer information */

  chan->pid    = OTGFS_PID_DATA1;
  chan->buffer = buffer;
  chan->buflen = buflen;

  /* Set up for the wait BEFORE starting the transfer */

  ret = stm32_chan_waitsetup(priv, chan)
  if (ret != OK)
    {
      udbg("ERROR: Device disconnected\n");
      return ret;
    }

  /* Start the transfer */

  stm32_transfer_start(priv, pric->ep0in);

  /* Wait for the transfer to complete */

  stm32_chan_wait(priv, chan);

  /* Return the result of the transfer */

  return -(int)chan->result;
}

/*******************************************************************************
 * Name: stm32_gint_wrpacket
 *
 * Description:
 *   Transfer the 'buflen' bytes in 'buffer' to the Tx FIFO associated with
 *   'chidx' (non-DMA).
 *
 *******************************************************************************/

static void stm32_gint_wrpacket(FAR struct stm32_usbhost_s *priv,
                                FAR uint8_t *buffer, int chidx, int buflen)
{
  FAR volatile uint32_t *fifo;
  FAR uint32_t *src;
  int buflen32;

  /* Get the number of 32-byte words associated with this byte size */

  buflen32 = (buflen + 3) >> 2;

  /* Get the address of the Tx FIFO associated with this channel */

  fifo = STM32_OTGFS_DFIFO_HCH(chidx);

  /* Transfer all of the data into the Tx FIFO */

  src = (FAR uint32_t *)buffer;
  for (; buflen32 > 0; buflen32--)
    {
      uint32_t data = *src++;
      stm32_putreg(fifo, data);
    }
}

/*******************************************************************************
 * Name: stm32_gint_halttxchan
 *
 * Description:
 *   Halt the Tx channel associated with 'chidx' by setting the  CHannel DISable
 *   (CHDIS) bit in in the HCCHAR register.
 *
 *******************************************************************************/

static void stm32_gint_halttxchan(FAR struct stm32_usbhost_s *priv, int chidx)
{
  uint32_t hcchar;
  uint32_t eptype;
  uint32_t txsts;

  /* Prepare to set the CHannel DISable and the CHannel ENAble bits in the
   * HCCHAR register.
   */

  hcchar = stm32_getreg(STM32_OTGFS_HCCHAR(chidx));
  hcchar |= (OTGFS_HCCHAR_CHDIS | OTGFS_HCCHAR_CHENA);

  /* Get the endpoint type from the HCCHAR register */

  eptype = (hcchar & OTGFS_HCCHAR_EPTYP_MASK);

  if (eptype == OTGFS_HCCHAR_EPTYP_CTRL || eptype == OTGFS_HCCHAR_EPTYP_BULK)
    {
      /* Check if the non-periodic Tx FIFO is empty. */

      txsts = stm32_getreg(STM32_OTGFS_HNPTXSTS);
      if ((txsts & OTGFS_HNPTXSTS_NPTXFSAV_MASK) == 0)
        {
          /* The Tx FIFO is empty... disable the channel */

          hcchar &= ~OTGFS_HCCHAR_CHENA;
        }
    }
  else /* if (eptype == OTGFS_HCCHAR_EPTYP_ISOC || eptype == OTGFS_HCCHAR_EPTYP_INTR) */
    {
      /* Check if the periodic Tx FIFO is empty. */

      txsts = stm32_getreg(STM32_OTGFS_HPTXSTS);
      if ((txsts & OTGFS_HPTXSTS_PTXFSAVL_MASK) == 0)
        {
          /* The Tx FIFO is empty... disable the channel */

          hcchar &= ~OTGFS_HCCHAR_CHENA;
        }
    }

  /* Halt the channel by setting CHDIS in the HCCHAR */

  stm32_putreg(STM32_OTGFS_HCCHAR(chidx), hcchar);
}

/*******************************************************************************
 * Name: stm32_gint_hcinisr
 *
 * Description:
 *   USB OTG FS host IN channels interrupt handler
 *
 *******************************************************************************/

static inline void stm32_gint_hcinisr(FAR struct stm32_usbhost_s *priv,
                                      int chidx)
{
  FAR struct stm32_chan_s *chan = &priv->chan[chidx];
  uint32_t hcint;
  uint32_t hcintmsk;
  uint32_t hcchar;
  uint32_t hctsiz;
  unsigned int eptype;

  /* Read the HCINT register to get the pending HC interrupts.  Read the
   * HCINTMSK register to get the set of enabled HC interrupts.
   */

  hcint    = stm32_getreg(STM32_OTGFS_HCINT(chidx));
  hcintmsk = stm32_getreg(STM32_OTGFS_HCINTMSK(chidx));

  /* AND the two to get the set of enabled, pending HC interrupts */

  pending = hcint & hcintmsk;

  /* Pre-fetch the HCCHAR register and extract the endpoint type.  Those
   * values are used in several cases.
   */

  hcchar = stm32_getreg(STM32_OTGFS_HCCHAR(chidx));
  eptype = (hcchar & OTGFS_HCCHAR_EPTYP_MASK) >> OTGFS_HCCHAR_EPTYP_SHIFT;

  /* Check for a pending ACK response received/transmitted (ACK) interrrupt */

  if ((pending & OTGFS_HCINT_ACK) != 0)
    {
      /* Clear the pending the ACK response received/transmitted (ACK) interrupt */

      hcint &= ~OTGFS_HCINT_ACK;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a pending STALL response receive (STALL) interrrupt */

  else if ((pending & OTGFS_HCINT_STALL) != 0)
    {
      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Set the stall state */

      chan->chstate = CHSTATE_STALL;

      /* Clear the NAK and STALL Conditions. */

      hcint &= ~(OTGFS_HCINT_NAK | OTGFS_HCINT_STALL);
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* When there is a STALL, clear any pending NAK so that it is nor
       * processed below.
       */

      pending &= ~OTGFS_HCINT_NAK;
    }

  /* Check for a pending Data Toggle ERRor (DTERR) interrrupt */

  else if ((pending & OTGFS_HCINT_DTERR) != 0)
    {
      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Clear the NAK and data toggle error conditions */

      hcint &= ~(OTGFS_HCINT_NAK | OTGFS_HCINT_DTERR);
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);

      /* Set the Data Toggle ERRor (DTERR) state */

      chan->chstate = CHSTATE_DTERR;
    }

  /* Check for a pending FRaMe OverRun (FRMOR) interrrupt */

  if ((pending & OTGFS_HCINT_FRMOR) != 0)
    {
      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Clear the FRaMe OverRun (FRMOR) condition */

      hcint &= ~OTGFS_HCINT_FRMOR;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a pending TransFeR Completed (XFRC) interrrupt */

  else if ((pending & OTGFS_HCINT_XFRC) != 0)
    {
      /* Set the trnansfer complete state and reset the error count */

      chan->chstate = CHSTATE_XFRC;
      chan->nerrors = 0;

      /* Clear the TransFeR Completed (XFRC) condition */

      hcint &= ~OTGFS_HCINT_XFRC;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);

      if ((eptype == OTGFS_HCCHAR_EPTYP_CTRL) ||
          (eptype == OTGFS_HCCHAR_EPTYP_BULK))
        {
          /* Unmask the CHannel Halted (CHH) interrupt */

          hcintmsk |= OTGFS_HCINT_CHH;
          stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

          /* Halt the Tx channel */

          stm32_gint_halttxchan(priv, chidx);

          /* Clear any pending NAK condition */

          hcint &= ~OTGFS_HCINT_NAK;
          stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);

          /* Toggle the IN data state */

          chan->indata1 ^= true;
        }
      else if (eptype == OTGFS_HCCHAR_EPTYP_INTR)
        {
          /* Force the next transfer on an ODD frame */

          hcchar |= OTGFS_HCCHAR_ODDFRM;
          stm32_putreg(STM32_OTGFS_HCCHAR(chidx), hcchar);

          /* Set the request done state */

          chan->result = OK;
        }
    }

  /* Check for a pending CHannel Halted (CHH) interrrupt */

  else if ((pending & OTGFS_HCINT_CHH) != 0)
    {
      /* Mask the CHannel Halted (CHH) interrupt */

      hcintmsk &= ~OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Update the request state based on the host state machine state */

      if (chan->chstate == CHSTATE_XFRC)
        {
          /* Set the request done state */

          chan->result = OK;
        }
      else if (chan->chstate == CHSTATE_STALL)
        {
          /* Set the request stall state */

          chan->result = EPERM;
        }
      else if ((chan->chstate == CHSTATE_TXERR) ||
               (chan->chstate == CHSTATE_DTERR))
        {
          /* Set the request error state */

          chan->nerrors = 0;
          chan->result = EIO;

        }
      else if (eptype == OTGFS_HCCHAR_EPTYP_INTR)
        {
          /* Toggle the IN data toggle */

          chan->indata1 ^= true;
        }

      /* Clear the CHannel Halted (CHH) condition */

      hcint &= ~OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a pending Transaction ERror (TXERR) interrrupt */

  else if ((pending & OTGFS_HCINT_TXERR) != 0)
    {
      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Increment the error count and set the transaction error state */

      chan->nerrors++;
      chan->chstate = CHSTATE_TXERR;

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Clear the Transaction ERror (TXERR) condition */

      hcint &= ~OTGFS_HCINT_TXERR;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a pending NAK response received (NAK) interrrupt */

  else if ((pending & OTGFS_HCINT_NAK) != 0)
    {
      /* Handle the NAK based on the endpoint type */

      if (eptype == OTGFS_HCCHAR_EPTYP_INTR)
        {
          /* Unmask the CHannel Halted (CHH) interrupt */

          hcintmsk |= OTGFS_HCINT_CHH;
          stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

          /* Halt the Tx channel */

          stm32_gint_halttxchan(priv, chidx);
        }
      else if ((eptype == OTGFS_HCCHAR_EPTYP_CTRL) ||
               (eptype == OTGFS_HCCHAR_EPTYP_BULK))
        {
          /* Re-activate the channel by clearing CHDIS and assuring that
           * CHENA is set
           */

          hcchar |= OTGFS_HCCHAR_CHENA;
          hcchar &= ~OTGFS_HCCHAR_CHDIS;
          stm32_putreg(STM32_OTGFS_HCCHAR(chidx), hcchar);
        }

      /* Set the NAK state */

      chan->chstate = CHSTATE_NAK;

      /* Clear the NAK condition */

      hcint &= ~OTGFS_HCINT_NAK;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a transfer complete event */

  stm32_chan_wakeup(priv, chan);
}

/*******************************************************************************
 * Name: stm32_gint_hcoutisr
 *
 * Description:
 *   USB OTG FS host OUT channels interrupt handler
 *
 *******************************************************************************/

static inline void stm32_gint_hcoutisr(FAR struct stm32_usbhost_s *priv,
                                       int chidx)
{
  FAR struct stm32_chan_s *chan = &priv->chan[chidx];
  uint32_t hcint;
  uint32_t hcintmsk;
  uint32_t pending;
  uint32_t hcchar;

  /* Read the HCINT register to get the pending HC interrupts.  Read the
   * HCINTMSK register to get the set of enabled HC interrupts.
   */

  hcint    = stm32_getreg(STM32_OTGFS_HCINT(chidx));
  hcintmsk = stm32_getreg(STM32_OTGFS_HCINTMSK(chidx));

  /* AND the two to get the set of enabled, pending HC interrupts */

  pending = hcint & hcintmsk;

  /* Check for a pending ACK response received/transmitted (ACK) interrrupt */

  if ((pending & OTGFS_HCINT_ACK) != 0)
    {
      /* Clear the pending the ACK response received/transmitted (ACK) interrupt */

      hcint &= ~OTGFS_HCINT_ACK;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a pending FRaMe OverRun (FRMOR) interrrupt */

  else if ((pending & OTGFS_HCINT_FRMOR) != 0)
    {
      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Clear the pending the FRaMe OverRun (FRMOR) interrupt */

      hcint &= ~OTGFS_HCINT_FRMOR;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a pending TransFeR Completed (XFRC) interrrupt */

  else if ((pending & OTGFS_HCINT_XFRC) != 0)
    {
      chan->nerrors = 0;

      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Clear the pending the TransFeR Completed (XFRC) interrupt */

      hcint &= ~OTGFS_HCINT_XFRC;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);

      /* Set the transfer completed state */

      chan->chstate = CHSTATE_XFRC;
    }

  /* Check for a pending STALL response receive (STALL) interrrupt */

  else if ((pending & OTGFS_HCINT_STALL) != 0)
    {
      /* Clear the pending the STALL response receiv (STALL) interrupt */

      hcint &= ~OTGFS_HCINT_STALL;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);

      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Set the stall state */

      chan->chstate = CHSTATE_STALL;
    }

  /* Check for a pending NAK response received (NAK) interrrupt */

  else if ((pending & OTGFS_HCINT_NAK) != 0)
    {
      uint32_t regval;

      /* Clear the error count */

      chan->nerrors = 0;

      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Clear the pending the NAK response received (NAK) interrupt */

      hcint &= ~OTGFS_HCINT_NAK;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);

      /* Set the NAK state */

      chan->chstate = CHSTATE_NAK;
    }

  /* Check for a pending Transaction ERror (TXERR) interrrupt */

  else if ((pending & OTGFS_HCINT_TXERR) != 0)
    {
      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Increment the number of errors */

      chan->nerrors++;

      /* Set the transaction error state */

      chan->chstate = CHSTATE_TXERR;

      /* Clear the pending the Transaction ERror (TXERR) interrupt */

      hcint &= ~OTGFS_HCINT_TXERR;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a pending response received (xxx) interrrupt */

#if 0 /* NYET is a reserved bit in the HCINT register */
  else if ((pending & OTGFS_HCINT_NYET) != 0)
    {
      chan->nerrors = 0;

      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Clear the pending the response received (xxx) interrupt */

      hcint &= ~OTGFS_HCINT_NYET;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);

      /* Set the NYET state */

      chan->chstate = CHSTATE_NYET;
    }
#endif

  /* Check for a pending Data Toggle ERRor (DTERR) interrrupt */

  else if (pending & OTGFS_HCINT_DTERR)
    {
      /* Unmask the CHannel Halted (CHH) interrupt */

      hcintmsk |= OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      /* Halt the Tx channel */

      stm32_gint_halttxchan(priv, chidx);

      /* Set the data toggle error state */

      chan->chstate = CHSTATE_DTERR;

      /* Clear the pending the Data Toggle ERRor (DTERR) and NAK interrupts */

      hcint &= ~(OTGFS_HCINT_DTERR | OTGFS_HCINT_NAK);
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a pending CHannel Halted (CHH) interrrupt */

  else if ((pending & OTGFS_HCINT_CHH) != 0)
    {
      /* Mask the CHannel Halted (CHH) interrupt */

      hcintmsk &= ~OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINTMSK(chidx), hcintmsk);

      if (chan->chstate == CHSTATE_XFRC)
        {
          /* Set the request done state */

          chan->result = OK;

          /* Read the HCCHAR register to get the HCCHAR register to get
           * the endpoint type.
           */

          hcchar = stm32_getreg(STM32_OTGFS_HCCHAR(chidx));

          /* Is it a bulk endpoint */

          if ((hcchar & OTGFS_HCCHAR_EPTYP_MASK) == OTGFS_HCCHAR_EPTYP_BULK)
            {
              /* Yes... toggle the data out PID */

              chan->outdata1 ^= true;
            }
        }
      else if (chan->chstate == CHSTATE_NAK ||
               chan->chstate == CHSTATE_NYET)
        {
          chan->result = EGAIN;
        }
      else if (chan->chstate == CHSTATE_STALL)
        {
          /* Set the request stall state */

          chan->result = EPERM;
        }
      else if (chan->chstate == CHSTATE_TXERR)
        {
          /* Check the error count */

          if (chan->nerrors == 3)
            {
              /* If the error count exceeds a threshold, then set the request error state */

              chan->result  = EIO;
              chan->nerrors = 0;
            }
        }

      /* Clear the pending the CHannel Halted (CHH) interrupt */

      hcint &= ~OTGFS_HCINT_CHH;
      stm32_putreg(STM32_OTGFS_HCINT(chidx), hcint);
    }

  /* Check for a transfer complete event */

  stm32_chan_wakeup(priv, chan);
}

/*******************************************************************************
 * Name: stm32_gint_connected
 *
 * Description:
 *   Handle a connection event.
 *
 *******************************************************************************/

static void stm32_gint_connected(FAR struct stm32_usbhost_s *priv)
{
  /* We we previously disconnected? */

  if (!priv->connected)
    {
      /* Yes.. then now we are connected */
 
      ullvdbg("Connected\n");
      priv->connected = true;
      DEBUGASSERT(priv->smstate == SMTATE_IDLE);

      /* Notify any waiters */

      priv->smstate = SMSTATE_ATTACHED;
      if (priv->eventwait)
        {
          stm32_givesem(&priv->eventsem);
          priv->eventwait = false;
        }
    }
}

/*******************************************************************************
 * Name: stm32_gint_disconnected
 *
 * Description:
 *   Handle a disconnection event.
 *
 *******************************************************************************/

static void stm32_gint_disconnected(FAR struct stm32_usbhost_s *priv)
{
  /* Were we previously connected? */

  if !priv->connected)
    {
      /* Yes.. then we no longer connected */
 
      ullvdbg("Disconnected\n");
      priv->connected = false;

      /* Are we bound to a class driver? */

      if (priv->class)
        {
          /* Yes.. Disconnect the class driver */

          CLASS_DISCONNECTED(priv->class);
          priv->class = NULL;
        }

      /* Re-Initilaize Host for new Enumeration */

      stm32_swreset(priv);
      stm32_chan_freeall(priv);
      priv->smstate = SMSTATE_DETACHED;

    /* Notify any waiters that there is a change in the connection state */

     if (priv->eventwait)
        {
          stm32_givesem(&priv->eventsem);
          priv->eventwait = false;
        }
    }
}

/*******************************************************************************
 * Name: stm32_gint_sofisr
 *
 * Description:
 *   USB OTG FS start-of-frame interrupt handler
 *
 *******************************************************************************/

#ifdef CONFIG_STM32_OTGFS_SOFINTR
static inline void stm32_gint_sofisr(FAR struct stm32_usbhost_s *priv)
{
  /* Handle SOF interrupt */
#warning "Do what?"

  /* Clear pending SOF interrupt */

  stm32_putreg(STM32_OTGFS_GINTSTS, OTGFS_GINT_SOF);
}
#endif

/*******************************************************************************
 * Name: stm32_gint_rxflvlisr
 *
 * Description:
 *   USB OTG FS RxFIFO non-empty interrupt handler
 *
 *******************************************************************************/

static inline void stm32_gint_rxflvlisr(FAR struct stm32_usbhost_s *priv)
{
  FAR uint32_t *dest;
  uint32_t grxsts;
  uint32_t intmsk;
  uint32_t hcchar;
  uint32_t hctsiz;
  uint32_t fifo;
  int bcnt;
  int bcnt32;
  int chidx;
  int i;

  /* Disable the RxFIFO non-empty interrupt */

  intmsk  = stm32_getreg(STM32_OTGFS_GINTMSK)
  intmsk &= ~OTGFS_GINT_RXFLVL;
  stm32_putreg(STM32_OTGFS_GINTMSK, intmsk);

  /* Read and pop the next status from the Rx FIFO */

  grxsts = stm32_getreg(STM32_OTGFS_GRXSTSP);

  /* Isolate the channel number/index in the status word */

  chidx = (grxsts & OTGFS_GRXSTSH_CHNUM_MASK) >> OTGFS_GRXSTSH_CHNUM_SHIFT;

  /* Get the host channel characteristics register (HCCHAR) for this channel */

  hcchar = stm32_getreg(STM32_OTGFS_HCCHAR(chidx));

  /* The process the interrupt according to the packet status */

  switch (grxsts & OTGFS_GRXSTSH_PKTSTS_MASK)
    {
    case OTGFS_GRXSTSH_PKTSTS_INRECVD: /* IN data packet received */
      {
        /* Read the data into the host buffer. */

        int bcnt = (grxsts & OTGFS_GRXSTSH_BCNT_MASK) >> OTGFS_GRXSTSH_BCNT_SHIFT;

        if (bcnt > 0 && priv->chan[chidx].buffer != NULL)
          {
            /* Transfer the packet from the Rx FIFO into the user buffer */

            FAR uint32_t *dest = (FAR uint32_t *)priv->chan[chidx].buffer;
            uint32_t fifo = STM32_OTGFS_DFIFO_HCH(0);
            uint32_t hctsiz;
            int bcnt32 = (bcnt + 3) >> 2;

            for (i = 0; i < count32b; i++, dest += 4)
              {
                *dest++ = stm32_getreg(fifo);
              }

            /* Manage multiple packet transfers */

            priv->chan[chidx].buffer += bcnt;
            priv->chan[chidx].xfrlen += bcnt;
            priv->chan[chidx].xfrd    = priv->chan[chidx].xfrlen;

            /* Check if more packets are expected */

            hctsiz = stm32_getreg(STM32_OTGFS_HCTSIZ(chidx));
            if ((hctsiz & OTGFS_HCTSIZ_PKTCNT_MASK) != 0)
              {
                /* Re-activate the channel when more packets are expected */

                hcchar |= OTGFS_HCCHAR_CHENA;
                hcchar &= ~OTGFS_HCCHAR_CHDIS;
                stm32_putreg(STM32_OTGFS_HCCHAR(chidx), hcchar);
              }
          }
      }
      break;

    case OTGFS_GRXSTSH_PKTSTS_INDONE:  /* IN transfer completed */
    case OTGFS_GRXSTSH_PKTSTS_DTOGERR: /* Data toggle error */
    case OTGFS_GRXSTSH_PKTSTS_HALTED:  /* Channel halted */
    default:
      break;
    }

  /* Re-enable the RxFIFO non-empty interrupt */

  intmsk |= OTGFS_GINT_RXFLVL;
  stm32_putreg(STM32_OTGFS_GINTMSK, intmsk);
}

/*******************************************************************************
 * Name: stm32_gint_nptxfeisr
 *
 * Description:
 *   USB OTG FS non-periodic TxFIFO empty interrupt handler
 *
 *******************************************************************************/

static inline void stm32_gint_nptxfeisr(FAR struct stm32_usbhost_s *priv)
{
  uint32_t     regval;
  unsigned int buflen;
  unsigned int buflen32;
  unsigned int wrsize;
  unsigned int avail32;
  unsigned int chidx;

  /* Loop while there is data to be sent and where there is space available
   * in the non-periodic Tx FIFO.
   */

  for (;;)
    {
      /* Read the status from the top of the non-periodic TxFIFO */

      regval   = stm32_getreg(STM32_OTGFS_HNPTXSTS);

      /* Extract the channel number and the number of 32-bit words available in
       * the non-periodic Tx FIFO.
       */

      chidx    = (regval & OTGFS_HNPTXSTS_CHNUM_MASK) >> OTGFS_HNPTXSTS_CHNUM_SHIFT;
      avail32  = (regval & OTGFS_HNPTXSTS_NPTXFSAV_MASK) >> OTGFS_HNPTXSTS_NPTXFSAV_SHIFT;

      /* Get the number of words remaining to be sent */

      buflen   = priv->chan[chidx].buflen
      buflen32 = (buflen + 3) >> 2;

      /* Break out of the loop if either (a) there is nothing more to be
       * sent, or (2) there is insufficent space availabe in the non-periodic
       * Tx FIFO to hold the next packet.
       */

      if (buflen == 0 || avail32 <= buflen32)
        {
          return;
        }
 
      /* Get the number of bytes available in the non-periodic Tx FIFO. That
       * is the maximum write size.
       */

      wrsize = avail32 << 2;

      /* Clip the actual write size to the number of bytes actually available
       * to be sent.
       */

      if (wrsize > buflen)
        {
          /* This is the last packet to be sent.  Clip to the amount of
           * data to send in the last packet.
           */

          wrsize = buflen;
          stm32_modifyreg(STM32_OTGFS_GINTMSK, OTGFS_GINT_NPTXFE, 0);
        }

      stm32_gint_wrpacket(priv, priv->chan[chidx].buffer, chidx, wrsize);

      priv->chan[chidx].buffer += wrsize;
      priv->chan[chidx].buflen -= wrsize;
      priv->chan[chidx].xfrlen += wrsize;
    }
}

/*******************************************************************************
 * Name: stm32_gint_ptxfeisr
 *
 * Description:
 *   USB OTG FS periodic TxFIFO empty interrupt handler
 *
 *******************************************************************************/

static inline void stm32_gint_ptxfeisr(FAR struct stm32_usbhost_s *priv)
{
  uint32_t     regval;
  unsigned int buflen;
  unsigned int buflen32;
  unsigned int wrsize;
  unsigned int avail32;
  unsigned int chidx;

  /* Loop while there is data to be sent and where there is space available
   * in the periodic Tx FIFO.
   */

  for (;;)
    {
      /* Read the status from the top of the periodic TxFIFO */

      regval   = stm32_getreg(STM32_OTGFS_HPTXSTS);

      /* Extract the channel number and the number of 32-bit words available in
       * the periodic Tx FIFO.
       */

      chidx    = (regval & OTGFS_HPTXSTS_CHNUM_MASK) >> OTGFS_HPTXSTS_CHNUM_SHIFT;
      avail32  = (regval & OTGFS_HPTXSTS_PTXFSAVL_MASK) >> OTGFS_HPTXSTS_PTXFSAVL_SHIFT;

      /* Get the number of words remaining to be sent */

      buflen   = priv->chan[chidx].buflen
      buflen32 = (buflen + 3) >> 2;

      /* Break out of the loop if either (a) there is nothing more to be
       * sent, or (2) there is insufficent space availabe in the periodic
       * Tx FIFO to hold the next packet.
       */

      if (buflen == 0 || avail32 <= buflen32)
        {
          return;
        }
 
      /* Get the number of bytes available in the periodic Tx FIFO. That is
       * the maximum write size.
       */

      wrsize = avail32 << 2;

      /* Clip the actual write size to the number of bytes actually available
       * to be sent.
       */

      if (wrsize > buflen)
        {
          /* This is the last packet to be sent.  Clip to the amount of
           * data to send in the last packet.
           */

          wrsize = buflen;
          stm32_modifyreg(STM32_OTGFS_GINTMSK, OTGFS_GINT_PTXFE, 0);
        }

      stm32_gint_wrpacket(priv, priv->chan[chidx].buffer, chidx, wrsize);

      priv->chan[chidx].buffer += wrsize;
      priv->chan[chidx].buflen -= wrsize;
      priv->chan[chidx].xfrlen += wrsize;
    }
}

/*******************************************************************************
 * Name: stm32_gint_hcisr
 *
 * Description:
 *   USB OTG FS host channels interrupt handler
 *
 *******************************************************************************/

static inline void stm32_gint_hcisr(FAR struct stm32_usbhost_s *priv)
{
  uint32_t haint;
  uint32_t hcchar;
  int i = 0;

  /* Read the Host all channels interrupt register and test each bit in the
   * register. Each bit i, i=0...(STM32_NHOST_CHANNELS-1), corresponds to
   * a pending interrupt on channel i.
   */

  haint = stm32_getreg(STM32_OTGFS_HAINT);
  for (i = 0; i < STM32_NHOST_CHANNELS; i++)
    {
      /* Is an interrupt pending on this channel? */

      if ((haint & OTGFS_HAINT(i)) != 0)
        {
          /* Yes... read the HCCHAR register to get the direction bit */

          hcchar = stm32_getreg(STM32_OTGFS_HCCHAR(i));

          /* Was this an interrupt on an IN or an OUT channel? */

          if ((hcchar & OTGFS_HCCHAR_EPDIR) != 0)
            {
              /* Handle the HC IN channel interrupt */

              stm32_gint_hcinisr(priv, i);
            }
          else
            {
              /* Handle the HC OUT channel interrupt */

              stm32_gint_hcoutisr(priv, i);
            }
        }
    }
}

/*******************************************************************************
 * Name: stm32_gint_hprtisr
 *
 * Description:
 *   USB OTG FS host port interrupt handler
 *
 *******************************************************************************/

static inline void stm32_gint_hprtisr(FAR struct stm32_usbhost_s *priv)
{
  uint32_t hprt;
  uint32_t newhprt;
  uint32_t hcfg;

  /* Read the port status and control register (HPRT) */

  hprt = stm32_getreg(STM32_OTGFS_HPRT);

  /* Setup to clear the interrupt bits in GINTSTS by setting the corresponding
   * bits in the HPRT.
   */

  newhprt = hprt & ~(OTGFS_HPRT_PENA    | OTGFS_HPRT_PCDET  |
                     OTGFS_HPRT_PENCHNG | OTGFS_HPRT_POCCHNG);

  /* Check for Port Overcurrent CHaNGe (POCCHNG) */

  if ((hprt & OTGFS_HPRT_POCCHNG) != 0)
    {
      /* Set up to clear the POCCHNG status in the new HPRT contents. */

      newhprt |= OTGFS_HPRT_POCCHNG;
    }

  /* Check for Port Connect DETected (PCDET).  The core sets this bit when a
   * device connection is detected.
   */

  if ((hprt & OTGFS_HPRT_PCDET) != 0)
    {
      /* Set up to clear the PCDET status in the new HPRT contents. Then
       * process the new connection event.
       */

      newhprt |= OTGFS_HPRT_PCDET;
      stm32_gint_connected(priv);
    }

  /* Check for Port Enable CHaNGed (PENCHNG) */

  if ((hprt & OTGFS_HPRT_PENCHNG) != 0)
    {
      /* Set up to clear the PENCHNG status in the new HPRT contents. */

      newhprt |= OTGFS_HPRT_PENCHNG;

      /* Was the port enabled? */

      if ((hprt & OTGFS_HPRT_PENA) != 0)
        {
          uint32_t hcfg;

          /* Yes.. handle the new connection event */

          stm32_gint_connected(priv);

          /* Check the Host ConFiGuration register (HCFG) */

          hcfg = stm32_getreg(STM32_OTGFS_HCFG);

          /* Is this a low speed or full speed connection (OTG FS does not
           * support high speed)
           */

          if ((hprt & OTGFS_HPRT_PSPD_MASK) == OTGFS_HPRT_PSPD_LS)
            {
              /* Set the Host Frame Interval Register for the 6KHz speed */

              stm32_putreg(STM32_OTGFS_HFIR, 6000);
 
              if ((hcfg & OTGFS_HCFG_FSLSPCS_MASK) != OTGFS_HCFG_FSLSPCS_LS6MHz)
                {
                  hcfg &= ~OTGFS_HCFG_FSLSPCS_MASK;
                  hcfg |= OTGFS_HCFG_FSLSPCS_LS6MHz;
                  stm32_putreg(STM32_OTGFS_HCFG, hcfg);
                }
            }
          else /* if ((hprt & OTGFS_HPRT_PSPD_MASK) == OTGFS_HPRT_PSPD_FS) */
            {
              stm32_putreg(STM32_OTGFS_HFIR, 48000);
              if ((hcfg & OTGFS_HCFG_FSLSPCS_MASK) != OTGFS_HCFG_FSLSPCS_FS48MHz)
                {
                  hcfg &= ~OTGFS_HCFG_FSLSPCS_MASK;
                  hcfg |= OTGFS_HCFG_FSLSPCS_FS48MHz;
                  stm32_putreg(STM32_OTGFS_HCFG, hcfg);
                }
            }

          /* Reset the port */

          stm32_portreset(priv);
        }
    }

  /* Clear port interrupts by setting bits in the HPRT */

  stm32_putreg(STM32_OTGFS_HPRT, newhprt);
}

/*******************************************************************************
 * Name: stm32_gint_discisr
 *
 * Description:
 *   USB OTG FS disconnect detected interrupt handler
 *
 *******************************************************************************/

static inline void stm32_gint_discisr(FAR struct stm32_usbhost_s *priv)
{
  uint32_t regval;

  /* Handle the disconnection event */

  stm32_gint_disconnected(priv);

  /* Clear the dicsonnect interrupt */

  stm32_putreg(STM32_OTGFS_GINTSTS, OTGFS_GINT_DISC);
}

/*******************************************************************************
 * Name: stm32_gint_iisooxfrisr
 *
 * Description:
 *   USB OTG FS incomplete isochronous interrupt handler
 *
 *******************************************************************************/

static inline void stm32_gint_iisooxfrisr(FAR struct stm32_usbhost_s *priv)
{
  uint32_t regval;

  /* CHENA : Set to enable the channel
   * CHDIS : Set to stop transmitting/receiving data on a channel
   */
 
  regval = stm32_getreg(STM32_OTGFS_HCCHAR(0));
  regval |= (OTGFS_HCCHAR_CHDIS | OTGFS_HCCHAR_CHENA);
  stm32_putreg(STM32_OTGFS_HCCHAR(0), regval);

  /* Clear the incomplete isochronous OUT interrupt */

  stm32_putreg(STM32_OTGFS_GINTSTS, OTGFS_GINT_IISOOXFR);
}

/*******************************************************************************
 * Name: stm32_gint_isr
 *
 * Description:
 *   USB OTG FS global interrupt handler
 *
 *******************************************************************************/

static int stm32_gint_isr(int irq, FAR void *context)
{
  /* At present, there is only support for a single OTG FS host. Hence it is
   * pre-allocated as g_usbhost.  However, in most code, the private data
   * structure will be referenced using the 'priv' pointer (rather than the
   * global data) in order to simplify any future support for multiple devices.
   */

  FAR struct stm32_usbhost_s *priv = &g_usbhost;
  uint32_t pending;

  /* If OTG were supported, we would need to check if we are in host or
   * device mode when the global interrupt occurs.  Here we support only
   * host mode
   */

  /* Get the unmasked bits in the GINT status */

  pending = stm32_getreg(STM32_OTGFS_GINTSTS);
  pending &= stm32_getreg(STM32_OTGFS_GINTMSK);

  /* The process each pending, unmasked GINT interrupts */

  if (pending != 0)
    {
      ullvdbg("GINTSTS: %08x\n", pending);

      /* Handle the start of frame interrupt */

#ifdef CONFIG_STM32_OTGFS_SOFINTR
      if ((pending & OTGFS_GINT_SOF) != 0)
        {
          stm32_gint_sofisr(priv);
        }
#endif

      /* Handle the RxFIFO non-empty interrupt */

      if ((pending & OTGFS_GINT_RXFLVL)) != 0)
        {
          stm32_gint_rxflvlisr(priv);
        }

      /* Handle the non-periodic TxFIFO empty interrupt */

      if ((pending & OTGFS_GINT_NPTXFE)) != 0)
        {
          stm32_gint_nptxfeisr(priv);
        }

      /* Handle the periodic TxFIFO empty interrupt */

      if ((pending & OTGFS_GINT_PTXFE)) != 0)
        {
          stm32_gint_ptxfeisr(priv);
        }

      /* Handle the host channels interrupt */

      if ((pending & OTGFS_GINT_HC)) != 0)
        {
          stm32_gint_hcisr(priv);
        }

      /* Handle the host port interrupt */

      if ((pending & OTGFS_GINT_HPRT)) != 0)
        {
          stm32_gint_hprtisr(priv);
        }

      /* Handle the disconnect detected interrupt */

      if ((pending & OTGFS_GINT_DISC)) != 0)
        {
          stm32_gint_discisr(priv);
        }

      /* Handle the incomplete isochronous OUT transfer */

      if ((pending & OTGFS_GINT_IISOOXFR)) != 0)
        {
          stm32_gint_iisooxfrisr(priv);
        }
    }

  return OK;
}

/*******************************************************************************
 * Name: stm32_gint_enable and stm32_gint_disable
 *
 * Description:
 *   Respectively enable or disable the global OTG FS interrupt.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 *******************************************************************************/

static void stm32_gint_enable(void)
{
  uint32_t regval;

  /* Set the GINTMSK bit to unmask the interrupt */

  regval  = stm32_getreg(STM32_OTGFS_GAHBCFG)
  regval |= OTGFS_GAHBCFG_GINTMSK;
  stm32_putreg(OTGFS_GAHBCFG_GINTMSK, regval);
}

static void stm32_gint_disable(void)
{
  uint32_t regval;

  /* Clear the GINTMSK bit to mask the interrupt */

  regval  = stm32_getreg(STM32_OTGFS_GAHBCFG)
  regval &= ~OTGFS_GAHBCFG_GINTMSK;
  stm32_putreg(OTGFS_GAHBCFG_GINTMSK, regval);
}

/*******************************************************************************
 * Name: stm32_hostinit_enable
 *
 * Description:
 *   Enable host interrupts.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 *******************************************************************************/

static inline void stm32_hostinit_enable(void)
{
  /* Disable all interrupts. */

  stm32_putreg(STM32_OTGFS_GINTMSK, 0);

  /* Clear any pending interrupts. */

  stm32_putreg(STM32_OTGFS_GINTSTS, 0xffffffff);

  /* Clear any pending USB OTG Interrupts (should be done elsewhere if OTG is supported) */

  stm32_putreg(STM32_OTGFS_GOTGINT, 0xffffffff);

  /* Clear any pending USB OTG interrupts */

  stm32_putreg(STM32_OTGFS_GINTSTS, 0xbfffffff);

  /* Enable the host interrupts */
  /* Common interrupts:
   *
   *   OTGFS_GINT_WKUP     : Resume/remote wakeup detected interrupt
   *   OTGFS_GINT_USBSUSP  : USB suspend
   */

  regval = (OTGFS_GINT_WKUP | OTGFS_GINT_USBSUSP);

  /* If OTG were supported, we would need to enable the following as well:
   *
   *   OTGFS_GINT_OTG      : OTG interrupt
   *   OTGFS_GINT_SRQ      : Session request/new session detected interrupt
   *   OTGFS_GINT_CIDSCHG  : Connector ID status change
   */

  /* Host-specific interrupts
   *
   *   OTGFS_GINT_SOF      : Start of frame
   *   OTGFS_GINT_RXFLVL   : RxFIFO non-empty
   *   OTGFS_GINT_IISOOXFR : Incomplete isochronous OUT transfer
   *   OTGFS_GINT_HPRT     : Host port interrupt
   *   OTGFS_GINT_HC       : Host channels interrupt
   *   OTGFS_GINT_DISC     : Disconnect detected interrupt
   */

#ifdef CONFIG_STM32_OTGFS_SOFINTR
  regval |= (OTGFS_GINT_SOF    | OTGFS_GINT_RXFLVL   | OTGFS_GINT_IISOOXFR |
             OTGFS_GINT_HPRT   | OTGFS_GINT_HC       | OTGFS_GINT_DISC);
#else
  regval |= (OTGFS_GINT_RXFLVL | OTGFS_GINT_IISOOXFR | OTGFS_GINT_HPRT     |
             OTGFS_GINT_HC     | OTGFS_GINT_DISC);
#endif
  stm32_putreg(STM32_OTGFS_GINTMSK, regval);
}

/*******************************************************************************
 * USB Host Controller Operations
 *******************************************************************************/

/*******************************************************************************
 * Name: stm32_wait
 *
 * Description:
 *   Wait for a device to be connected or disconneced.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   connected - TRUE: Wait for device to be connected; FALSE: wait for device
 *      to be disconnected
 *
 * Returned Values:
 *   Zero (OK) is returned when a device in connected. This function will not
 *   return until either (1) a device is connected or (2) some failure occurs.
 *   On a failure, a negated errno value is returned indicating the nature of
 *   the failure
 *
 * Assumptions:
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int stm32_wait(FAR struct usbhost_driver_s *drvr, bool connected)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;
  irqstate_t flags;

  /* Are we already connected? */

  flags = irqsave();
  while (priv->connected == connected)
    {
      /* No... wait for the connection/disconnection */

      priv->eventwait = true;
      stm32_takesem(&priv->eventsem);
    }
  irqrestore(flags);

  udbg("Connected:%s\n", priv->connected ? "YES" : "NO");
  return OK;
}

/*******************************************************************************
 * Name: stm32_enumerate
 *
 * Description:
 *   Enumerate the connected device.  As part of this enumeration process,
 *   the driver will (1) get the device's configuration descriptor, (2)
 *   extract the class ID info from the configuration descriptor, (3) call
 *   usbhost_findclass() to find the class that supports this device, (4)
 *   call the create() method on the struct usbhost_registry_s interface
 *   to get a class instance, and finally (5) call the configdesc() method
 *   of the struct usbhost_class_s interface.  After that, the class is in
 *   charge of the sequence of operations.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int stm32_enumerate(FAR struct usbhost_driver_s *drvr)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;
  uint32_t regval;
  int chidx;
  int ret;

  /* Are we connected to a device?  The caller should have called the wait()
   * method first to be assured that a device is connected.
   */

  while (!priv->connected)
    {
      /* No, return an error */

      udbg("Not connected\n");
      return -ENODEV;
    }
  DEBUGASSERT(priv->smstate == SMSTATE_ATTACHED);
 
  /* Allocate and initialize the control OUT channel */

  chidx = stm32_chan_alloc(priv);
  DEBUGASSERT(chidx >= 0);

  priv->ep0out                = chidx;
  priv->chan[chidx].epno      = 0;
  priv->chan[chidx].isin      = false;
  priv->chan[chidx].eptype    = OTGFS_EPTYPE_CTRL;
  priv->chan[chidx].maxpacket = STM32_EP0_DEF_PACKET_SIZE;
  priv->chan[chidx].indata1   = false;
  priv->chan[chidx].outdata1  = false;

  /* Allocate and initialize the control IN channel */

  chidx = stm32_chan_alloc(priv);
  DEBUGASSERT(chidx >= 0);

  priv->ep0in                 = chidx;
  priv->chan[chidx].epno      = 0;
  priv->chan[chidx].isin      = true;
  priv->chan[chidx].eptype    = OTGFS_EPTYPE_CTRL;
  priv->chan[chidx].maxpacket = STM32_EP0_DEF_PACKET_SIZE;
  priv->chan[chidx].indata1   = false;
  priv->chan[chidx].outdata1  = false;

  /* USB 2.0 spec says at least 50ms delay before port reset */

  up_mdelay(100);

  /* Reset the host port */

  stm32_portreset(priv);

  /* Get the current device speed */

  regval = stm32_getreg(STM32_OTGFS_HPRT);
  priv->lowspeed = ((regval & OTGFS_HPRT_PSPD_MASK) == OTGFS_HPRT_PSPD_LS);

  /* Configure control channels */

  stm32_chan_configure(priv, priv->ep0out) ;
  stm32_chan_configure(priv, priv->ep0in) ;

  /* Let the common usbhost_enumerate do all of the real work.  Note that the
   * FunctionAddress (USB address) is hardcoded to one.
   */

  uvdbg("Enumerate the device\n");
  priv->smstate = SMSTATE_ENUM
  ret = usbhost_enumerate(drvr, 1, &priv->class);

  /* The enumeration may fail either because of some HCD interfaces failure
   * or because the device class is not supported.  In either case, we just
   * need to perform the disconnection operation and make ready for a new
   * enumeration.
   */

  if (ret < 0)
    {
      /* Return to the disconnected state */

      stm32_gint_disconnected(priv);
    }

  return ret;
}

/************************************************************************************
 * Name: stm32_ep0configure
 *
 * Description:
 *   Configure endpoint 0.  This method is normally used internally by the
 *   enumerate() method but is made available at the interface to support an
 *   external implementation of the enumeration logic.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   funcaddr - The USB address of the function containing the endpoint that EP0
 *     controls
 *   maxpacketsize - The maximum number of bytes that can be sent to or
 *    received from the endpoint in a single data packet
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int stm32_ep0configure(FAR struct usbhost_driver_s *drvr, uint8_t funcaddr,
                              uint16_t maxpacketsize)
{
  FAR struct stm32_usbhost_s *priv = (FAR struct stm32_usbhost_s *)drvr;

  DEBUGASSERT(drvr && funcaddr < 128 && maxpacketsize < 2048);

  /* We must have exclusive access to the USB host hardware and state structures */

  stm32_takesem(&priv->exclsem);

  /* Save the device address and EP0 max packet size */

  priv->devaddr = funcaddr;
  priv->ep0size = maxpacketsize;

  /* Configure the EP0 OUT channel */

  priv->chan[priv->ep0out].maxpacket = maxpacketsize;
  stm32_chan_configure(priv, priv->ep0out);
  
  /* Configure the EP0 IN channel */

  priv->chan[priv->ep0in].maxpacket = maxpacketsize;
  stm32_chan_configure(priv, priv->ep0in);
  
  stm32_givesem(&priv->exclsem);
  return OK;
}

/************************************************************************************
 * Name: stm32_epalloc
 *
 * Description:
 *   Allocate and configure one endpoint.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   epdesc - Describes the endpoint to be allocated.
 *   ep - A memory location provided by the caller in which to receive the
 *      allocated endpoint desciptor.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int stm32_epalloc(FAR struct usbhost_driver_s *drvr,
                         const FAR struct usbhost_epdesc_s *epdesc, usbhost_ep_t *ep)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;
  int                     ret  = -ENOMEM;

  /* Sanity check.  NOTE that this method should only be called if a device is
   * connected (because we need a valid low speed indication).
   */

  DEBUGASSERT(priv && epdesc && ep && priv->connected);

  /* We must have exclusive access to the USB host hardware and state structures */

  stm32_takesem(&priv->exclsem);

  /* Get the direction of the endpoint */

  if (epdesc->in)
    {
    }
  else
    {
    }

  /* Set the transfer type */

  /* Special Case isochronous transfer types */

  stm32_givesem(&priv->exclsem);
  return ret;
}

/************************************************************************************
 * Name: stm32_epfree
 *
 * Description:
 *   Free and endpoint previously allocated by DRVR_EPALLOC.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   ep - The endpint to be freed.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int stm32_epfree(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;
  int                     ret;

  /* There should not be any pending, real TDs linked to this ED */

  DEBUGASSERT(drvr);

  /* We must have exclusive access to the USB host hardware and state structures */

  stm32_takesem(&priv->exclsem);
#warning "Missing logic"

  stm32_givesem(&priv->exclsem);
  return ret;
}

/*******************************************************************************
 * Name: stm32_alloc
 *
 * Description:
 *   Some hardware supports special memory in which request and descriptor data can
 *   be accessed more efficiently.  This method provides a mechanism to allocate
 *   the request/descriptor memory.  If the underlying hardware does not support
 *   such "special" memory, this functions may simply map to malloc.
 *
 *   This interface was optimized under a particular assumption.  It was assumed
 *   that the driver maintains a pool of small, pre-allocated buffers for descriptor
 *   traffic.  NOTE that size is not an input, but an output:  The size of the
 *   pre-allocated buffer is returned.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   buffer - The address of a memory location provided by the caller in which to
 *     return the allocated buffer memory address.
 *   maxlen - The address of a memory location provided by the caller in which to
 *     return the maximum size of the allocated buffer memory.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int stm32_alloc(FAR struct usbhost_driver_s *drvr,
                       FAR uint8_t **buffer, FAR size_t *maxlen)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;
  int ret = -ENOMEM;

  DEBUGASSERT(priv && buffer && maxlen);
  /* We must have exclusive access to the USB host hardware and state structures */

  stm32_takesem(&priv->exclsem);
#warning "Missing logic"
  stm32_givesem(&priv->exclsem);
  return ret;
}

/*******************************************************************************
 * Name: stm32_free
 *
 * Description:
 *   Some hardware supports special memory in which request and descriptor data can
 *   be accessed more efficiently.  This method provides a mechanism to free that
 *   request/descriptor memory.  If the underlying hardware does not support
 *   such "special" memory, this functions may simply map to free().
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   buffer - The address of the allocated buffer memory to be freed.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int stm32_free(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;

  DEBUGASSERT(drvr && buffer);

  /* We must have exclusive access to the USB host hardware and state structures */

  stm32_takesem(&priv->exclsem);
#warning "Missing logic"
  stm32_givesem(&priv->exclsem);
  return OK;
}

/************************************************************************************
 * Name: stm32_ioalloc
 *
 * Description:
 *   Some hardware supports special memory in which larger IO buffers can
 *   be accessed more efficiently.  This method provides a mechanism to allocate
 *   the request/descriptor memory.  If the underlying hardware does not support
 *   such "special" memory, this functions may simply map to malloc.
 *
 *   This interface differs from DRVR_ALLOC in that the buffers are variable-sized.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   buffer - The address of a memory location provided by the caller in which to
 *     return the allocated buffer memory address.
 *   buflen - The size of the buffer required.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int stm32_ioalloc(FAR struct usbhost_driver_s *drvr,
                         FAR uint8_t **buffer, size_t buflen)
{
  DEBUGASSERT(drvr && buffer);
  return -ENOSYS;
}

/************************************************************************************
 * Name: stm32_iofree
 *
 * Description:
 *   Some hardware supports special memory in which IO data can  be accessed more
 *   efficiently.  This method provides a mechanism to free that IO buffer
 *   memory.  If the underlying hardware does not support such "special" memory,
 *   this functions may simply map to free().
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   buffer - The address of the allocated buffer memory to be freed.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int stm32_iofree(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer)
{
  DEBUGASSERT(drvr && buffer);
  return -ENOSYS;
}

/*******************************************************************************
 * Name: stm32_ctrlin and stm32_ctrlout
 *
 * Description:
 *   Process a IN or OUT request on the control endpoint.  These methods
 *   will enqueue the request and wait for it to complete.  Only one transfer may be
 *   queued; Neither these methods nor the transfer() method can be called again
 *   until the control transfer functions returns.
 *
 *   These are blocking methods; these functions will not return until the
 *   control transfer has completed.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   req - Describes the request to be sent.  This request must lie in memory
 *      created by DRVR_ALLOC.
 *   buffer - A buffer used for sending the request and for returning any
 *     responses.  This buffer must be large enough to hold the length value
 *     in the request description. buffer must have been allocated using DRVR_ALLOC
 *
 *   NOTE: On an IN transaction, req and buffer may refer to the same allocated
 *   memory.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int stm32_ctrlin(FAR struct usbhost_driver_s *drvr,
                        FAR const struct usb_ctrlreq_s *req,
                        FAR uint8_t *buffer)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;
  uint16_t buflen;
  int  ret;

  DEBUGASSERT(drvr && req);
  uvdbg("type:%02x req:%02x value:%02x%02x index:%02x%02x len:%02x%02x\n",
        req->type, req->req, req->value[1], req->value[0],
        req->index[1], req->index[0], req->len[1], req->len[0]);

  /* Extract values from the request */

  buflen = stm32_getle16(req->len);

  /* We must have exclusive access to the USB host hardware and state structures */

  stm32_takesem(&priv->exclsem);

  /* Loop, retrying until the retry count expires */

  for (retries = 0; retries < STM32_RETRY_COUNT; retries++)
    {
      /* Send the SETUP request */

      ret = stm32_ctrl_sendsetup(priv, req);
      if (ret < 0)
        {
          udbg("stm32_ctrl_sendsetup failed: %d\n", ret);
          return ret;
        }

      /* Handle the IN data phase (if any) */

      if (buflen > 0)
        {
          ret = stm32_ctrl_recvdata(priv, buffer, buflen);
          if (ret < 0)
            {
              udbg("stm32_ctrl_recvdata failed: %d\n", ret);
              continue;
            }
        }

      /* Handle the status OUT phase */

      priv->chan[priv->ep0out].outdata1 ^= true;
      ret = stm32_ctrl_senddata(priv, buffer, buflen);
      if (ret == OK)
        {
          break;
        }

      udbg("stm32_ctrl_senddata failed: %d\n", ret);
      ret = -ETIMEDOUT;
    }

  stm32_givesem(&priv->exclsem);
  return ret;
}

static int stm32_ctrlout(FAR struct usbhost_driver_s *drvr,
                         FAR const struct usb_ctrlreq_s *req,
                         FAR const uint8_t *buffer)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;
  uint16_t buflen;
  int retries;
  int  ret;

  DEBUGASSERT(drvr && req);
  uvdbg("type:%02x req:%02x value:%02x%02x index:%02x%02x len:%02x%02x\n",
        req->type, req->req, req->value[1], req->value[0],
        req->index[1], req->index[0], req->len[1], req->len[0]);

  /* Extract values from the request */

  buflen = stm32_getle16(req->len);

  /* We must have exclusive access to the USB host hardware and state structures */

  stm32_takesem(&priv->exclsem);

  /* Loop, retrying until the retry count expires */

  for (retries = 0; retries < STM32_RETRY_COUNT; retries++)
    {
      /* Send the SETUP request */

      ret = stm32_ctrl_sendsetup(priv, req);
      if (ret < 0)
        {
          udbg("stm32_ctrl_sendsetup failed: %d\n", ret);
          continue;
        }

      /* Handle the data OUT phase (if any) */

      if (buflen > 0)
        {
          /* Start DATA out transfer (only one DATA packet) */

          priv->chan[priv->ep0out].outdata1 = true;
          ret = stm32_ctrl_senddata(priv, NULL, 0);
          if (ret < 0)
            {
              udbg("stm32_ctrl_senddata failed: %d\n", ret);
              continue;
            }
        }

      /* Handle the status IN phase */
 
      ret = stm32_ctrl_recvdata(priv, NULL, 0);
      if (ret == OK)
        {
          break;
        }

      udbg("stm32_ctrl_recvdata failed: %d\n", ret);
      ret = -ETIMEDOUT;
    }

  stm32_givesem(&priv->exclsem);
  return ret;
}

/*******************************************************************************
 * Name: stm32_transfer
 *
 * Description:
 *   Process a request to handle a transfer descriptor.  This method will
 *   enqueue the transfer request and return immediately.  Only one transfer may be
 *   queued; Neither this method nor the ctrlin or ctrlout methods can be called
 *   again until this function returns.
 *
 *   This is a blocking method; this functions will not return until the
 *   transfer has completed.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   ep - The IN or OUT endpoint descriptor for the device endpoint on which to
 *      perform the transfer.
 *   buffer - A buffer containing the data to be sent (OUT endpoint) or received
 *     (IN endpoint).  buffer must have been allocated using DRVR_ALLOC
 *   buflen - The length of the data to be sent or received.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/
 
static int stm32_transfer(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep,
                          FAR uint8_t *buffer, size_t buflen)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;
  uint32_t dirpid;
  uint32_t regval;
#if STM32_IOBUFFERS > 0
  uint8_t *origbuf = NULL;
#endif
  bool in;
  int ret;

  DEBUGASSERT(priv && chan && buffer && buflen > 0);

  /* We must have exclusive access to the USB host hardware and state structures */

  stm32_takesem(&priv->exclsem);

  /* Set up for the wait BEFORE starting the transfer */

  ret = stm32_chan_waitsetup(priv, chan);
  if (ret != OK)
    {
      udbg("ERROR: Device disconnected\n");
      goto errout;
    }

  /* Start the transfer */
#warning "Missing logic"

  /* Wait for the transfer to complete */

  stm32_chan_wait(priv, chan);

  /* Get the result of the transfer */


  /* Check the transfer completion status */
#warning "Missing Logic"

errout:
  /* Make sure that there is no outstanding request on this endpoint */
#warning "Missing Logic"

  stm32_givesem(&priv->exclsem);
  return ret;
}

/*******************************************************************************
 * Name: stm32_disconnect
 *
 * Description:
 *   Called by the class when an error occurs and driver has been disconnected.
 *   The USB host driver should discard the handle to the class instance (it is
 *   stale) and not attempt any further interaction with the class driver instance
 *   (until a new instance is received from the create() method).  The driver
 *   should not called the class' disconnected() method.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *
 * Returned Values:
 *   None
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static void stm32_disconnect(FAR struct usbhost_driver_s *drvr)
{
  struct stm32_usbhost_s *priv = (struct stm32_usbhost_s *)drvr;
  priv->class = NULL;
}
  
/*******************************************************************************
 * Initialization
 *******************************************************************************/
/*******************************************************************************
 * Name: stm32_ep0init
 *
 * Description:
 *   Initialize ED for EP0, add it to the control ED list, and enable control
 *   transfers.
 *
 * Input Parameters:
 *   priv - private driver state instance.
 *
 * Returned Values:
 *   None
 *
 *******************************************************************************/

static inline void stm32_ep0init(struct stm32_usbhost_s *priv)
{
  uint32_t regval;

  /* Set up some default values */

  (void)stm32_ep0configure(&priv->drvr, 1, 8);

#warning "Missing logic"
}

/*******************************************************************************
 * Name: stm32_portreset
 *
 * Description:
 *   Reset the USB host port.
 *
 *   NOTE: "Before starting to drive a USB reset, the application waits for the
 *   OTG interrupt triggered by the debounce done bit (DBCDNE bit in
 *   OTG_FS_GOTGINT), which indicates that the bus is stable again after the
 *   electrical debounce caused by the attachment of a pull-up resistor on DP
 *   (FS) or DM (LS).
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   None
 *
 *******************************************************************************/

static void stm32_portreset(FAR struct stm32_usbhost_s *priv)
{
  uint32_t regval;

  regval  = stm32_getreg(STM32_OTGFS_HPRT);
  retval &= ~(OTGFS_HPRT_PENA|OTGFS_HPRT_PCDET|OTGFS_HPRT_PENCHNG|OTGFS_HPRT_POCCHNG);
  regval |= OTGFS_HPRT_PRST;
  stm32_putreg(STM32_OTGFS_HPRT, regval);

  up_mdelay(10);

  regval &= ~OTGFS_HPRT_PRST;
  stm32_putreg(STM32_OTGFS_HPRT, regval);

  up_mdelay(20);
}

/*******************************************************************************
 * Name: stm32_flush_txfifos
 *
 * Description:
 *   Flush the selected Tx FIFO.
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   None.
 *
 *******************************************************************************/

static inline void stm32_flush_txfifos(uint32_t txfnum)
{
  uint32_t regval;
  uint32_t timeout;

  /* Initiate the TX FIFO flush operation */

  regval = OTGFS_GRSTCTL_TXFFLSH | txfnum;
  stm32_putreg(regval, STM32_OTGFS_GRSTCTL);

  /* Wait for the FLUSH to complete */

  for (timeout = 0; timeout < STM32_FLUSH_DELAY; timeout++)
    {
      regval = stm32_getreg(STM32_OTGFS_GRSTCTL);
      if ((regval & OTGFS_GRSTCTL_TXFFLSH) == 0)
        {
          break;
        }
    }

  /* Wait for 3 PHY Clocks */

  up_udelay(3);
}

/*******************************************************************************
 * Name: stm32_flush_rxfifo
 *
 * Description:
 *   Flush the Rx FIFO.
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   None.
 *
 *******************************************************************************/

static inline void stm32_flush_rxfifo(void)
{
  uint32_t regval;
  uint32_t timeout;

  /* Initiate the RX FIFO flush operation */

  stm32_putreg(OTGFS_GRSTCTL_RXFFLSH, STM32_OTGFS_GRSTCTL);

  /* Wait for the FLUSH to complete */

  for (timeout = 0; timeout < STM32_FLUSH_DELAY; timeout++)
    {
      regval = stm32_getreg(STM32_OTGFS_GRSTCTL);
      if ((regval & OTGFS_GRSTCTL_RXFFLSH) == 0)
        {
          break;
        }
    }

  /* Wait for 3 PHY Clocks */

  up_udelay(3);
}

/*******************************************************************************
 * Name: stm32_vbusdrive
 *
 * Description:
 *   Drive the Vbus +5V.
 *
 * Input Parameters:
 *   priv  - USB host driver private data structure.
 *   state - True: Drive, False: Don't drive
 *
 * Returned Value:
 *   None.
 *
 *******************************************************************************/

static void stm32_vbusdrive(FAR struct stm32_usbhost_s *priv, bool state)
{
  uint32_t regval;

  /* Enable/disable the external charge pump */

  stm32_usbhost_vbusdrive(state);

  /* Turn on the Host port power. */

  regval = stm32_getreg(STM32_OTGFS_HPRT);
  regval &= ~(OTGFS_HPRT_PENA|OTGFS_HPRT_PCDET|OTGFS_HPRT_PENCHNG|OTGFS_HPRT_POCCHNG);

  if (((regval & OTGFS_HPRT_PPWR) == 0) && state)
    {
      regval |= OTGFS_HPRT_PPWR;
      stm32_putreg(STM32_OTGFS_HPRT, regval);
    }

  if (((regval & OTGFS_HPRT_PPWR) != 0) && !state)
    {
      regval &= ~OTGFS_HPRT_PPWR;
      stm32_putreg(STM32_OTGFS_HPRT, regval);
    }

  up_mdelay(200);
}

/*******************************************************************************
 * Name: stm32_host_initialize
 *
 * Description:
 *   Initialize/re-initialize hardware for host mode operation.  At present,
 *   this function is called only from stm32_hw_initialize().  But if OTG mode
 *   were supported, this function would also be called to swtich between
 *   host and device modes on a connector ID change interrupt.
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   None.
 *
 *******************************************************************************/

static void stm32_host_initialize(FAR struct stm32_usbhost_s *priv)
{
  uint32_t regval;
  uint32_t offset;
  int ret;
  int i;

  /* Restart the PHY Clock */

  stm32_putreg(STM32_OTGFS_PCGCCTL, 0);

  /* Initialize Host Configuration (HCFG) register */

  regval  = stm32_getreg(STM32_OTGFS_HCFG);
  regval &= ~OTGFS_HCFG_FSLSPCS_MASK;
  regval |= OTGFS_HCFG_FSLSPCS_FS48MHz;
  stm32_putreg(STM32_OTGFS_HCFG, regval);

  /* Reset the host port */

  stm32_portreset(priv);

  /* Clear the FS-/LS-only support bit in the HCFG register */

  regval = stm32_getreg(STM32_OTGFS_HCFG);
  regval &= ~OTGFS_HCFG_FSLSS;
  stm32_putreg(STM32_OTGFS_HCFG, regval);

  /* Carve up FIFO memory for the Rx FIFO and the periodic and non-periodic Tx FIFOs */
  /* Configure Rx FIFO size (GRXFSIZ) */

  stm32_putreg(STM32_OTGFS_GRXFSIZ, CONFIG_STM32_OTGFS_RXFIFO_SIZE);
  offset = CONFIG_STM32_OTGFS_RXFIFO_SIZE;

  /* Setup the host non-periodic Tx FIFO size (HNPTXFSIZ) */

  regval = (offset | (CONFIG_STM32_OTGFS_NPTXFIFO_SIZE << OTGFS_HNPTXFSIZ_NPTXFD_MASK);
  stm32_putreg(STM32_OTGFS_DIEPTXF0_HNPTXFSIZ, regval);
  offset += CONFIG_STM32_OTGFS_NPTXFIFO_SIZE

  /* Set up the host periodic Tx fifo size register (HPTXFSIZ) */

  regval = (offset | (CONFIG_STM32_OTGFS_PTXFIFO_SIZE << OTGFS_HPTXFSIZ_PTXFD_SHIFT);
  stm32_putreg(STM32_OTGFS_HPTXFSIZ, regval);

  /* If OTG were supported, we sould need to clear HNP enable bit in the
   * USB_OTG control register about here.
   */

  /* Flush all FIFOs */

  stm32_flush_txfifos(OTGFS_GRSTCTL_TXFNUM_HALL);
  stm32_flush_rxfifo();

  /* Clear all pending HC Interrupts */

  for (i = 0; i < STM32_NHOST_CHANNELS; i++)
    {
      stm32_putreg(STM32_OTGFS_HCINT(i), 0xffffffff);
      stm32_putreg(STM32_OTGFS_HCINTMSK(i), 0);
    }

  /* Driver Vbus +5V (the smoke test).  Should be done elsewhere in OTG
   * mode.
   */

  stm32_vbusdrive(priv, true);

  /* Enable host interrupts */

  stm32_hostinit_enable(priv);
  return OK;
}

/*******************************************************************************
 * Name: stm32_sw_initialize
 *
 * Description:
 *   One-time setup of the host driver state structure.
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   None.
 *
 *******************************************************************************/

static inline void stm32_sw_initialize(FAR struct stm32_usbhost_s *priv)
{
  int i;

  /* Initialize the state data structure */

  sem_init(&priv->eventsem,  0, 0);
  sem_init(&priv->exclsem, 0, 1);

  priv->smstate   = SMSTATE_DETACHED;
  priv->ep0size   = STM32_EP0_MAX_PACKET_SIZE;
  priv->devaddr   = STM32_DEF_DEVADDR;
  priv->connected = false;
  priv->lowspeed  = false;

  /* Put all of the channels back in their initial, allocated state */

  memset(priv-chan, 0, STM32_MAX_TX_FIFOS * sizeof(struct stm32_chan_s));

  /* Initialize each channel */

  for (i = 0; i < STM32_MAX_TX_FIFOS; i++)
    {
      FAR struct stm32_chan_s *chan = &priv->chan[i];
      sem_init(&chan->waitsem,  0, 0);
    }
}

/*******************************************************************************
 * Name: stm32_hw_initialize
 *
 * Description:
 *   One-time setup of the host controller harware for normal operations.
 *
 * Input Parameters:
 *   priv -- USB host driver private data structure.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 *******************************************************************************/

static inline int stm32_hw_initialize(FAR struct stm32_usbhost_s *priv)
{
  uint32_t regval;
  int ret;

  /* Set the PHYSEL bit in the GUSBCFG register to select the OTG FS serial
   * transceiver: "This bit is always 1 with write-only access"
   */
  
  regval = stm32_getreg(STM32_OTGFS_GUSBCFG);;
  regval |= OTGFS_GUSBCFG_PHYSEL;
  stm32_putreg(STM32_OTGFS_GUSBCFG, regval);

  /* Reset after a PHY select and set Host mode.  First, wait for AHB master
   * IDLE state.
   */

  for (timeout = 0; timeout < STM32_READY_DELAY; timeout++)
    {
      up_udelay(3);
      regval = stm32_getreg(STM32_OTGFS_GRSTCTL);
      if ((regval & OTGFS_GRSTCTL_AHBIDL) != 0)
        {
          break;
        }
    }

  /* Then perform the core soft reset. */

  stm32_putreg(STM32_OTGFS_GRSTCTL, OTGFS_GRSTCTL_CSRST);
  for (timeout = 0; timeout < STM32_READY_DELAY; timeout++)
    {
      regval = stm32_getreg(STM32_OTGFS_GRSTCTL);
      if ((regval & OTGFS_GRSTCTL_CSRST) == 0)
        {
          break;
        }
    }

  /* Wait for 3 PHY Clocks */

  up_udelay(3);

  /* Deactivate the power down */

  regval  = (OTGFS_GCCFG_PWRDWN | OTGFS_GCCFG_VBUSASEN | OTGFS_GCCFG_VBUSBSEN);
#ifndef CONFIG_USBDEV_VBUSSENSING
  regval |= OTGFS_GCCFG_NOVBUSSENS;
#endif
#ifdef CONFIG_STM32_OTGFS_SOFOUTPUT
  regval |= OTGFS_GCCFG_SOFOUTEN;
#endif
  stm32_putreg(STM32_OTGFS_GCCFG, regval);
  up_mdelay(20);

  /* Initialize OTG features:  In order to support OTP, the HNPCAP and SRPCAP
   * bits would need to be set in the GUSBCFG register about here.
   */

  /* Force Host Mode */

  regval  = stm32_getreg(STM32_OTGFS_GUSBCFG);
  regval &= ~OTGFS_GUSBCFG_FDMOD;
  regval |= OTGFS_GUSBCFG_FHMOD;
  stm32_putreg(STM32_OTGFS_GUSBCFG, regval);
  up_mdelay(50);

  /* Initialize host mode and return success */

  stm32_host_initialize(priv);
  return OK;
}

/*******************************************************************************
 * Public Functions
 *******************************************************************************/

/*******************************************************************************
 * Name: usbhost_initialize
 *
 * Description:
 *   Initialize USB host device controller hardware.
 *
 * Input Parameters:
 *   controller -- If the device supports more than USB host controller, then
 *     this identifies which controller is being intialized.  Normally, this
 *     is just zero.
 *
 * Returned Value:
 *   And instance of the USB host interface.  The controlling task should
 *   use this interface to (1) call the wait() method to wait for a device
 *   to be connected, and (2) call the enumerate() method to bind the device
 *   to a class driver.
 *
 * Assumptions:
 * - This function should called in the initialization sequence in order
 *   to initialize the USB device functionality.
 * - Class drivers should be initialized prior to calling this function.
 *   Otherwise, there is a race condition if the device is already connected.
 *
 *******************************************************************************/

FAR struct usbhost_driver_s *usbhost_initialize(int controller)
{
  /* At present, there is only support for a single OTG FS host. Hence it is
   * pre-allocated as g_usbhost.  However, in most code, the private data
   * structure will be referenced using the 'priv' pointer (rather than the
   * global data) in order to simplify any future support for multiple devices.
   */

  FAR struct stm32_usbhost_s *priv = &g_usbhost;
  int ret;

  /* Sanity checks */

  DEBUGASSERT(controller == 0);

  /* Make sure that interrupts from the OTG FS core are disabled */

  stm32_gint_disable();

  /* Reset the state of the host driver */

  stm32_sw_initialize(priv);

  /* Alternate function pin configuration.  Here we assume that:
   *
   * 1. GPIOA, SYSCFG, and OTG FS peripheral clocking have already been\
   *    enabled as part of the boot sequence.
   * 2. Board-specific logic has already enabled other board specific GPIOs
   *    for things like soft pull-up, VBUS sensing, power controls, and over-
   *    current detection.
   */

  /* Configure OTG FS alternate function pins for DM, DP, ID, and SOF.
   *
   * PIN* SIGNAL      DIRECTION
   * ---- ----------- ----------
   * PA8  OTG_FS_SOF  SOF clock output
   * PA9  OTG_FS_VBUS VBUS input for device, Driven by external regulator by
   *                  host (not an alternate function)
   * PA10 OTG_FS_ID   OTG ID pin (only needed in Dual mode)
   * PA11 OTG_FS_DM   D- I/O
   * PA12 OTG_FS_DP   D+ I/O
   *
   * *Pins may vary from device-to-device.
   */

  stm32_configgpio(GPIO_OTGFS_DM);
  stm32_configgpio(GPIO_OTGFS_DP);
  stm32_configgpio(GPIO_OTGFS_ID);    /* Only needed for OTG */

  /* SOF output pin configuration is configurable */

#ifdef CONFIG_STM32_OTGFS_SOFOUTPUT
  stm32_configgpio(GPIO_OTGFS_SOF);
#endif

  /* Initialize the USB OTG FS core */     

  stm32_hw_initialize(priv);

  /* Attach USB host controller interrupt handler */

  if (irq_attach(STM32_IRQ_OTGFS, stm32_gint_isr) != 0)
    {
      udbg("Failed to attach IRQ\n");
      return NULL;
    }

  /* Enable USB OTG FS global interrupts */

  stm32_gint_enable();

  /* Enable interrupts at the interrupt controller */

  up_enable_irq(STM32_IRQ_OTGFS);
  return &priv->drvr;
}