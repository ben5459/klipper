// Code to setup clocks and gpio on stm32g4
//
// Copyright (C) 2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_CLOCK_REF_FREQ
#include "board/armcm_boot.h" // VectorTable
#include "board/armcm_reset.h" // try_request_canboot
#include "board/irq.h" // irq_disable
#include "board/usb_cdc.h" // usb_request_bootloader
#include "board/misc.h" // bootloader_request
#include "command.h" // DECL_CONSTANT_STR
#include "internal.h" // enable_pclock
#include "sched.h" // sched_main

#define FREQ_PERIPH_DIV 1
#define FREQ_PERIPH (CONFIG_CLOCK_FREQ / FREQ_PERIPH_DIV)
#define FREQ_USB 48000000

// Map a peripheral address to its enable bits
struct cline
lookup_clock_line(uint32_t periph_base)
{
    if (periph_base < APB2PERIPH_BASE) {
        uint32_t pos = (periph_base - APB1PERIPH_BASE) / 0x400;
        if (pos < 32) {
            return (struct cline){.en = &RCC->APB1ENR1,
                                  .rst = &RCC->APB1RSTR1,
                                  .bit = 1 << pos};
        } else {
            return (struct cline){.en = &RCC->APB1ENR2,
                                  .rst = &RCC->APB1RSTR2,
                                  .bit = 1 << (pos - 32)};
        }
    } else if (periph_base < AHB1PERIPH_BASE) {
        uint32_t pos = (periph_base - APB2PERIPH_BASE) / 0x400;
        return (struct cline){.en = &RCC->APB2ENR,
                              .rst = &RCC->APB2RSTR,
                              .bit = 1 << pos};

    } else if (periph_base < AHB2PERIPH_BASE) {
        uint32_t pos = (periph_base - AHB1PERIPH_BASE) / 0x400;
        return (struct cline){.en = &RCC->AHB1ENR,
                              .rst = &RCC->AHB1RSTR,
                              .bit = 1 << pos};

    } else {
        uint32_t pos = (periph_base - AHB2PERIPH_BASE) / 0x400;
        return (struct cline){.en = &RCC->AHB2ENR,
                              .rst = &RCC->AHB2RSTR,
                              .bit = 1 << pos};
    }
    if ((periph_base == FDCAN1_BASE) || (periph_base == FDCAN2_BASE))
        return (struct cline){.en=&RCC->APBENR1,.rst=&RCC->APBRSTR1,.bit=1<<12};
    if (periph_base == USB_BASE)
        return (struct cline){.en=&RCC->APBENR1,.rst=&RCC->APBRSTR1,.bit=1<<13};
    if (periph_base == CRS_BASE)
        return (struct cline){.en=&RCC->APBENR1,.rst=&RCC->APBRSTR1,.bit=1<<16};
    if (periph_base == I2C3_BASE)
        return (struct cline){.en=&RCC->APBENR1,.rst=&RCC->APBRSTR1,.bit=1<<23};
    if (periph_base == TIM1_BASE)
        return (struct cline){.en=&RCC->APBENR2,.rst=&RCC->APBRSTR2,.bit=1<<11};
    if (periph_base == SPI1_BASE)
        return (struct cline){.en=&RCC->APBENR2,.rst=&RCC->APBRSTR2,.bit=1<<12};
    if (periph_base == USART1_BASE)
        return (struct cline){.en=&RCC->APBENR2,.rst=&RCC->APBRSTR2,.bit=1<<14};
    if (periph_base == TIM14_BASE)
        return (struct cline){.en=&RCC->APBENR2,.rst=&RCC->APBRSTR2,.bit=1<<15};
    if (periph_base == TIM15_BASE)
        return (struct cline){.en=&RCC->APBENR2,.rst=&RCC->APBRSTR2,.bit=1<<16};
    if (periph_base == TIM16_BASE)
        return (struct cline){.en=&RCC->APBENR2,.rst=&RCC->APBRSTR2,.bit=1<<17};
    if (periph_base == TIM17_BASE)
        return (struct cline){.en=&RCC->APBENR2,.rst=&RCC->APBRSTR2,.bit=1<<18};
    if (periph_base == ADC1_BASE)
        return (struct cline){.en=&RCC->APBENR2,.rst=&RCC->APBRSTR2,.bit=1<<20};
    if (periph_base >= APBPERIPH_BASE && periph_base <= LPTIM1_BASE)
    {
        uint32_t bit = 1 << ((periph_base - APBPERIPH_BASE) / 0x400);
        return (struct cline){.en=&RCC->APBENR1, .rst=&RCC->APBRSTR1, .bit=bit};
    }
    // unknown peripheral. returning .bit=0 makes this a no-op
    return (struct cline){.en=&RCC->APBENR1, .rst=NULL, .bit=0};
}

// Return the frequency of the given peripheral clock
uint32_t
get_pclock_frequency(uint32_t periph_base)
{
    return FREQ_PERIPH;
}

// Enable a GPIO peripheral clock
void
gpio_clock_enable(GPIO_TypeDef *regs)
{
    uint32_t rcc_pos = ((uint32_t)regs - GPIOA_BASE) / 0x400;
    RCC->AHB2ENR |= 1 << rcc_pos;
    RCC->AHB2ENR;
}

#if !CONFIG_STM32_CLOCK_REF_INTERNAL
DECL_CONSTANT_STR("RESERVE_PINS_crystal", "PF0,PF1");
#endif

static void
enable_clock_stm32g4(void)
{
    uint32_t pll_base = 4000000, pll_freq = CONFIG_CLOCK_FREQ * 2, pllcfgr;
    if (!CONFIG_STM32_CLOCK_REF_INTERNAL) {
        // Configure 150Mhz PLL from external crystal (HSE)
        uint32_t div = CONFIG_CLOCK_REF_FREQ / pll_base - 1;
        RCC->CR |= RCC_CR_HSEON;
        while (!(RCC->CR & RCC_CR_HSERDY))
            ;
        pllcfgr = RCC_PLLCFGR_PLLSRC_HSE | (div << RCC_PLLCFGR_PLLM_Pos);
    } else {
        // Configure 150Mhz PLL from internal 16Mhz oscillator (HSI)
        uint32_t div = 16000000 / pll_base - 1;
        pllcfgr = RCC_PLLCFGR_PLLSRC_HSI | (div << RCC_PLLCFGR_PLLM_Pos);
        RCC->CR |= RCC_CR_HSION;
        while (!(RCC->CR & RCC_CR_HSIRDY))
            ;
    }
    pllcfgr |= (pll_freq/pll_base) << RCC_PLLCFGR_PLLN_Pos;
    pllcfgr |= (pll_freq/CONFIG_CLOCK_FREQ - 1) << RCC_PLLCFGR_PLLR_Pos;
    pllcfgr |= (pll_freq/FREQ_USB - 1) << RCC_PLLCFGR_PLLQ_Pos;
    RCC->PLLCFGR = (pllcfgr | ((pll_freq/pll_base) << RCC_PLLCFGR_PLLN_Pos)
                    | (0 << RCC_PLLCFGR_PLLR_Pos));
    RCC->CR |= RCC_CR_PLLON;

    // Enable 48Mhz USB clock using clock recovery
    if (CONFIG_USBSERIAL) {
        RCC->CRRCR |= RCC_CRRCR_HSI48ON;
        while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY))
            ;
        enable_pclock(CRS_BASE);
        CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;
    }
    if (CONFIG_USB) {
        uint32_t ref = (CONFIG_STM32_CLOCK_REF_INTERNAL
                        ? 16000000 : CONFIG_CLOCK_REF_FREQ);
        uint32_t plls_base = 2000000, plls_freq = FREQ_USB * 4;
        RCC->PLLSAICFGR = (
            ((ref/plls_base) << RCC_PLLSAICFGR_PLLSAIM_Pos)
            | ((plls_freq/plls_base) << RCC_PLLSAICFGR_PLLSAIN_Pos)
            | (((plls_freq/FREQ_USB)/2 - 1) << RCC_PLLSAICFGR_PLLSAIP_Pos)
            | ((plls_freq/FREQ_USB) << RCC_PLLSAICFGR_PLLSAIQ_Pos));
        RCC->CR |= RCC_CR_PLLSAION;
        while (!(RCC->CR & RCC_CR_PLLSAIRDY))
            ;

        RCC->DCKCFGR2 = RCC_DCKCFGR2_CK48MSEL;
    }
}

// Main clock setup called at chip startup
static void
clock_setup(void)
{
    enable_clock_stm32g4();

    // Set flash latency
    uint32_t latency = ((CONFIG_CLOCK_FREQ>150000000) ? FLASH_ACR_LATENCY_5WS :
                       ((CONFIG_CLOCK_FREQ>120000000) ? FLASH_ACR_LATENCY_4WS :
                       ((CONFIG_CLOCK_FREQ>90000000) ? FLASH_ACR_LATENCY_3WS :
                       ((CONFIG_CLOCK_FREQ>60000000) ? FLASH_ACR_LATENCY_2WS :
                       ((CONFIG_CLOCK_FREQ>30000000) ? FLASH_ACR_LATENCY_1WS :
                                                    FLASH_ACR_LATENCY_0WS)))));
    FLASH->ACR = (latency | FLASH_ACR_ICEN | FLASH_ACR_DCEN
                  | FLASH_ACR_PRFTEN | FLASH_ACR_DBG_SWEN);

    enable_pclock(PWR_BASE);
    PWR->CR3 |= PWR_CR3_APC; // allow gpio pullup/down

    // Wait for PLL lock
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;

    RCC->PLLCFGR |= RCC_PLLCFGR_PLLREN;

    // Switch system clock to PLL
    RCC->CFGR = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV1 | RCC_CFGR_PPRE2_DIV1
                | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL)
        ;
}



/****************************************************************
 * Bootloader
 ****************************************************************/

#define USB_BOOT_FLAG_ADDR (CONFIG_RAM_START + CONFIG_RAM_SIZE - 4096)
#define USB_BOOT_FLAG 0x55534220424f4f54 // "USB BOOT"

// Handle USB reboot requests
void
bootloader_request(void)
{
    try_request_canboot();
    usb_reboot_for_dfu_bootloader();
}

// Reboot into USB "HID" bootloader
static void
usb_hid_bootloader(void)
{
    irq_disable();
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    RCC->APB1ENR;
    PWR->CR |= PWR_CR_DBP;
    RTC->BKP4R = 0x424C; // HID Bootloader magic key
    PWR->CR &= ~PWR_CR_DBP;
    NVIC_SystemReset();
}

// Flag that bootloader is desired and reboot
static void
usb_reboot_for_dfu_bootloader(void)
{
    irq_disable();
    *(uint64_t*)USB_BOOT_FLAG_ADDR = USB_BOOT_FLAG;
    NVIC_SystemReset();
}

// Check if rebooting into system DFU Bootloader
static void
check_usb_dfu_bootloader(void)
{
    if (!CONFIG_USB || *(uint64_t*)USB_BOOT_FLAG_ADDR != USB_BOOT_FLAG)
        return;
    *(uint64_t*)USB_BOOT_FLAG_ADDR = 0;
    uint32_t *sysbase = (uint32_t*)0x1fff0000;
    asm volatile("mov sp, %0\n bx %1"
                 : : "r"(sysbase[0]), "r"(sysbase[1]));
}


/****************************************************************
 * Startup
 ****************************************************************/

// Main entry point - called from armcm_boot.c:ResetHandler()
void
armcm_main(void)
{
     if (CONFIG_USBSERIAL && *(uint64_t*)USB_BOOT_FLAG_ADDR == USB_BOOT_FLAG) {
        *(uint64_t*)USB_BOOT_FLAG_ADDR = 0;
        uint32_t *sysbase = (uint32_t*)0x1fff0000;
        asm volatile("mov sp, %0\n bx %1"
                     : : "r"(sysbase[0]), "r"(sysbase[1]));
    }
    SCB->VTOR = (uint32_t)VectorTable;

    // Reset clock registers (in case bootloader has changed them)
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY))
        ;
    RCC->CFGR = 0x00000000;
    RCC->CR = RCC_CR_HSION;
    while (RCC->CR & RCC_CR_PLLRDY)
        ;
    RCC->PLLCFGR = 0x00001000;
    RCC->IOPENR = 0x00000000;
    RCC->AHBENR = 0x00000100;
    RCC->APBENR1 = 0x00000000;
    RCC->APBENR2 = 0x00000000;

    check_usb_dfu_bootloader();

    // Set flash latency, cache and prefetch; use reset value as base
    uint32_t acr = 0x00040600;
    acr = (acr & ~FLASH_ACR_LATENCY) | (2<<FLASH_ACR_LATENCY_Pos);
    acr |= FLASH_ACR_ICEN | FLASH_ACR_PRFTEN;
    FLASH->ACR = acr;

     // Run SystemInit() and then restore VTOR
    SystemInit();
    SCB->VTOR = (uint32_t)VectorTable;   
    
    // Configure main clock
    clock_setup();

    sched_main();
}
