/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <assert.h>
#include <stdint.h>
#include "os/os_cputime.h"
#include "syscfg/syscfg.h"
#include "flash_map/flash_map.h"
#include "hal/hal_flash.h"
#include "hal/hal_bsp.h"
#include "bsp/cmsis_nvic.h"
#include "nrf51.h"
#include "nrf51_bitfields.h"
#include "mcu/nrf51_hal.h"
#if MYNEWT_VAL(SPI_0_MASTER)
#include "nrf_drv_spi.h"
#endif
#if MYNEWT_VAL(SPI_1_SLAVE)
#include "nrf_drv_spis.h"
#endif
#include "hal/hal_spi.h"
#include "os/os_dev.h"
#include "uart/uart.h"
#include "uart_hal/uart_hal.h"

#define BSP_LOWEST_PRIO     ((1 << __NVIC_PRIO_BITS) - 1)


#if MYNEWT_VAL(UART_0)
static struct uart_dev os_bsp_uart0;
static const struct nrf51_uart_cfg os_bsp_uart0_cfg = {
    .suc_pin_tx = MYNEWT_VAL(UART_0_PIN_TX),
    .suc_pin_rx = MYNEWT_VAL(UART_0_PIN_RX),
    .suc_pin_rts = MYNEWT_VAL(UART_0_PIN_RTS),
    .suc_pin_cts = MYNEWT_VAL(UART_0_PIN_CTS),
};
#endif

#if MYNEWT_VAL(SPI_0_MASTER)
/*
 * NOTE: do not set the ss pin here! This would cause the nordic SDK
 * to start using the SS pin when configured as a master and this is
 * not what our HAL expects. Our HAL expects that the SS pin, if used,
 * is treated as a gpio line and is handled outside the SPI routines.
 */
static const nrf_drv_spi_config_t os_bsp_spi0m_cfg = {
    .sck_pin      = 29,
    .mosi_pin     = 25,
    .miso_pin     = 28,
    .ss_pin       = NRF_DRV_SPI_PIN_NOT_USED,
    .irq_priority = (1 << __NVIC_PRIO_BITS) - 1,
    .orc          = 0xFF,
    .frequency    = NRF_DRV_SPI_FREQ_4M,
    .mode         = NRF_DRV_SPI_MODE_0,
    .bit_order    = NRF_DRV_SPI_BIT_ORDER_MSB_FIRST
};
#endif

#if MYNEWT_VAL(SPI_1_SLAVE)
static const nrf_drv_spis_config_t os_bsp_spi1s_cfg = {
    .sck_pin      = 29,
    .mosi_pin     = 25,
    .miso_pin     = 28,
    .csn_pin      = 24,
    .miso_drive   = NRF_DRV_SPIS_DEFAULT_MISO_DRIVE,
    .csn_pullup   = NRF_GPIO_PIN_PULLUP,
    .orc          = NRF_DRV_SPIS_DEFAULT_ORC,
    .def          = NRF_DRV_SPIS_DEFAULT_DEF,
    .mode         = NRF_DRV_SPIS_MODE_0,
    .bit_order    = NRF_DRV_SPIS_BIT_ORDER_MSB_FIRST,
    .irq_priority = (1 << __NVIC_PRIO_BITS) - 1
};
#endif

void
hal_bsp_init(void)
{
    int rc;

#if MYNEWT_VAL(UART_0)
    rc = os_dev_create((struct os_dev *) &os_bsp_uart0, "uart0",
      OS_DEV_INIT_PRIMARY, 0, uart_hal_init, (void *)&os_bsp_uart0_cfg);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(TIMER_0)
    rc = hal_timer_init(0, NULL);
    assert(rc == 0);
#endif
#if MYNEWT_VAL(TIMER_1)
    rc = hal_timer_init(1, NULL);
    assert(rc == 0);
#endif
#if MYNEWT_VAL(TIMER_2)
    rc = hal_timer_init(2, NULL);
    assert(rc == 0);
#endif

    /* Set cputime to count at 1 usec increments */
    rc = os_cputime_init(MYNEWT_VAL(CLOCK_FREQ));
    assert(rc == 0);

#if MYNEWT_VAL(SPI_0_MASTER)
    rc = hal_spi_init(0, (void *)&os_bsp_spi0m_cfg, HAL_SPI_TYPE_MASTER);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(SPI_1_SLAVE)
    rc = hal_spi_init(1, (void *)&os_bsp_spi1s_cfg, HAL_SPI_TYPE_SLAVE);
    assert(rc == 0);
#endif
}

extern void timer_handler(void);
static void
rtc0_timer_handler(void)
{
    if (NRF_RTC0->EVENTS_TICK) {
        NRF_RTC0->EVENTS_TICK = 0;
        timer_handler();
    }
}

void
os_bsp_systick_init(uint32_t os_ticks_per_sec, int prio)
{
    uint32_t ctx;
    uint32_t mask;
    uint32_t pre_scaler;

    /* Turn on the LFCLK */
    NRF_CLOCK->XTALFREQ = CLOCK_XTALFREQ_XTALFREQ_16MHz;
    NRF_CLOCK->TASKS_LFCLKSTOP = 1;
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
    NRF_CLOCK->LFCLKSRC = CLOCK_LFCLKSRC_SRC_Xtal;
    NRF_CLOCK->TASKS_LFCLKSTART = 1;

    /* Wait here till started! */
    mask = CLOCK_LFCLKSTAT_STATE_Msk | CLOCK_LFCLKSTAT_SRC_Xtal;
    while (1) {
        if (NRF_CLOCK->EVENTS_LFCLKSTARTED) {
            if ((NRF_CLOCK->LFCLKSTAT & mask) == mask) {
                break;
            }
        }
    }

    /* Is this exact frequency obtainable? */
    pre_scaler = (32768 / os_ticks_per_sec) - 1;

    /* disable interrupts */
    __HAL_DISABLE_INTERRUPTS(ctx);

    NRF_RTC0->TASKS_STOP = 1;
    NRF_RTC0->EVENTS_TICK = 0;
    NRF_RTC0->PRESCALER = pre_scaler;
    NRF_RTC0->INTENCLR = 0xffffffff;
    NRF_RTC0->TASKS_CLEAR = 1;

    /* Set isr in vector table and enable interrupt */
    NVIC_SetPriority(RTC0_IRQn, prio);
    NVIC_SetVector(RTC0_IRQn, (uint32_t)rtc0_timer_handler);
    NVIC_EnableIRQ(RTC0_IRQn);

    NRF_RTC0->INTENSET = RTC_INTENSET_TICK_Msk;
    NRF_RTC0->TASKS_START = 1;

    __HAL_ENABLE_INTERRUPTS(ctx);
}
