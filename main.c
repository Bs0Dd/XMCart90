#include "tusb.h"
#include "fatfs/ff.h"
#include "stdlib.h"
#include "ctype.h"
#include "hardware/pll.h"
#include "hardware/clocks.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/structs/syscfg.h"
#include "hardware/adc.h"
#include "hardware/rosc.h"
#include "hardware/xosc.h"
#include "hardware/flash.h"
#include "main.h"
#include "mk90.pio.h"
#include "emu_codes.h"
#include "files11.h"
#include "cartcrash/cartcrash.h"

FATFS FATFS_Obj;
FIL appFile;
DWORD diskSpaces[2] = {0};
char fileList[256][12+1];
char filename[12+1] = {0};
uint8_t readBuffer;
PIO pio;
uint32_t sm_tx;
uint32_t sm_rx;

DWORD flashPtr = 0;

uint8_t rwCart[64 * 1024] = {0xFF}; // RAM zone for RW SMP carts
uint32_t rwCartSize = 0;
uint16_t rwCartPtr = 0;
bool rwCartReqFlush = 0;
bool rwCartMode = 0;

bool contLock = 0;
uint8_t cLAttm = 0;

bool errMode = 0;

enum crStatus crtStat = CRS_NONE;

const char* autorunf = "AUTORUN.BIN";

static void pio_irq(void);

static void error_handler(void) {
    // enter permanent USB mode (e.g. in case flash is not formatted)
    errMode = 1;
    if (crtStat >= 0xE0 && crtStat <= 0xEB) {
        strcpy(crash_inform+crsi_msg_ptr, errMsgs[crtStat & 0xF]);
    }
    else {
        strcpy(crash_inform+crsi_msg_ptr, genErrMsg);
    }
    gpio_put(PIN_LED, 1);
    tud_connect();
    pio_sm_clear_fifos(pio, sm_rx);
    irq_clear(PIO0_IRQ_0);
    while (1){
        __wfi();
        tud_task(); // tinyusb device task
    }
}

static void clocks_initialize(void) {  
    // Enable the xosc
    xosc_init();

    // Before we touch PLLs, switch sys and ref cleanly away from their aux sources.
    hw_clear_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
    while (clocks_hw->clk[clk_sys].selected != 0x1) tight_loop_contents();

    pll_init(pll_usb, 1, 768 * MHZ, 4, 4);
    pll_deinit(pll_sys);

    clock_configure(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ,
                    48 * MHZ);

    clock_configure(clk_usb,
                    0, // No GLMUX
                    CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ,
                    48 * MHZ);

    clock_configure(clk_adc,
                    0, // No GLMUX
                    CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    48 * MHZ,
                    48 * MHZ);

    clock_stop(clk_ref);
    clock_stop(clk_peri);
    clock_stop(clk_rtc);
    clock_stop(clk_gpout0);
    clock_stop(clk_gpout1);
    clock_stop(clk_gpout2);
    clock_stop(clk_gpout3);

    clocks_hw->wake_en0 = ~(
        //CLOCKS_WAKE_EN0_CLK_SYS_VREG_AND_CHIP_RESET_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_RESETS_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_SPI0_BITS |
        CLOCKS_WAKE_EN0_CLK_PERI_SPI0_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_SPI1_BITS |
        CLOCKS_WAKE_EN0_CLK_PERI_SPI1_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_ROSC_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_PLL_SYS_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_PWM_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_PIO1_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_JTAG_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_I2C0_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_I2C1_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_RTC_BITS |
        CLOCKS_WAKE_EN0_CLK_RTC_RTC_BITS |
        CLOCKS_WAKE_EN0_CLK_SYS_DMA_BITS
    );

    clocks_hw->wake_en1 = ~(
        CLOCKS_WAKE_EN1_CLK_SYS_WATCHDOG_BITS |
        CLOCKS_WAKE_EN1_CLK_SYS_UART0_BITS |
        CLOCKS_WAKE_EN1_CLK_PERI_UART0_BITS |
        CLOCKS_WAKE_EN1_CLK_SYS_UART1_BITS |
        CLOCKS_WAKE_EN1_CLK_PERI_UART1_BITS |
        CLOCKS_WAKE_EN1_CLK_SYS_TBMAN_BITS |
        CLOCKS_WAKE_EN1_CLK_SYS_TIMER_BITS |
        CLOCKS_WAKE_EN1_CLK_SYS_SYSINFO_BITS
    );
    
}

static inline void gpio_initialize(void) {
    gpio_pull_up(PIN_CLK);
    gpio_pull_up(PIN_SELECT);
    gpio_pull_up(PIN_DATA);
    gpio_pull_up(PIN_ENAUMT);
    gpio_pull_up(PIN_ENUSBM);
    gpio_pull_up(PIN_SAVRAM);

    gpio_init(PIN_SELECT);
    gpio_init(PIN_VBUS);
    gpio_init(PIN_LED);
    gpio_init(PIN_ENAUMT);
    gpio_init(PIN_ENUSBM);
    gpio_init(PIN_SAVRAM);

    gpio_set_dir(PIN_VBUS, GPIO_IN);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_set_dir(PIN_SELECT, GPIO_IN);
    gpio_set_dir(PIN_ENAUMT, GPIO_IN);
    gpio_set_dir(PIN_ENUSBM, GPIO_IN);
    gpio_set_dir(PIN_SAVRAM, GPIO_IN);
}

static inline void pio_initialize(void) {
    pio = pio0;
    sm_tx = pio_claim_unused_sm(pio, true);
    sm_rx = pio_claim_unused_sm(pio, true);
    mk90bus_program_init(pio, sm_tx, sm_rx);
    
    irq_set_exclusive_handler(PIO0_IRQ_0, pio_irq);
    irq_set_enabled(PIO0_IRQ_0, true);
    pio_set_irq0_source_enabled(pio, pis_sm1_rx_fifo_not_empty, true);
    irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
}

static int strComp(const void *a, const void *b) { // strings comparsion
    return strcmp(a, b);
}

static FRESULT updateList() {
    DIR dj;
    FATFS *fs;
    FILINFO fno;
    FRESULT fr;
    DWORD fre_clust, fre_bytes;
    
    fr = f_opendir(&dj, "");
    if (fr == FR_OK) {
        uint32_t i = 0;
        do {
            fr = f_readdir(&dj, &fno);
            if (fr == FR_OK && (strcmp(fno.fname, autorunf) != 0) && !(fno.fattrib & AM_DIR)) {
                strcpy(fileList[i++], fno.fname);
            }
        } while (fr == FR_OK && i < 255 && fno.fname[0] != 0);
        f_closedir(&dj);
        
        if (fileList[i][0] == 0) i--;
        qsort(fileList, i, sizeof(fileList[0]), strComp);
    }
    else {
        crtStat = CRS_FDRE; // Error 0 : FAT driver error
        errMode = 1;
        return fr;
    }
    
    fr = f_getfree("0", &fre_clust, &fs);
    if (fr == FR_OK) {
        diskSpaces[0] = (fs->n_fatent - 2) * fs->csize * FLASH_SECTOR_SIZE;
        diskSpaces[1] = fre_clust * fs->csize * FLASH_SECTOR_SIZE;
    }
    else {
        crtStat = CRS_DSIE; // Error 2 : FS space calc error
        errMode = 1;
    }
    
    return fr;
}

static FRESULT openFile(const char *filename, BYTE mode) {
    FRESULT fr;

    f_close(&appFile);

    if (filename != NULL) {
        fr = f_open(&appFile, filename, mode);
    }

    if (fr != FR_OK && (mode == FA_READ) && (strcmp(fileList[0], "") != 0)) {
        fr = f_open(&appFile, fileList[0], FA_READ);
    }
 
    if (fr == FR_OK && !(mode & FA_CREATE_NEW)) {
        uint32_t lktbl[256];
        lktbl[0] = 256;
        appFile.cltbl = lktbl;
        fr = f_lseek(&appFile, CREATE_LINKMAP);
    }

    return fr;
}

static void flushRwCart(void) {
    //Flush RAM cart to the file, if required
    if (rwCartMode && rwCartReqFlush){
        rwCartReqFlush = 0;
        gpio_put(PIN_LED, 1);
        if (openFile(filename, FA_WRITE) != FR_OK) {
            crtStat = CRS_FDRE; // Error 0 : FAT driver error
            errMode = 1;
            return;
        }
        UINT bw = 0;
        if ((f_write(&appFile, rwCart, rwCartSize, &bw) != FR_OK) || (bw != rwCartSize)) {
            crtStat = CRS_WERR; // Error 4 : write error
            errMode = 1;
        }        
        f_close(&appFile);
        gpio_put(PIN_LED, 0);
    }
}

static void remount(void) {
    flushRwCart();
    if (errMode) {
        return;
    }
    rwCartMode = 0;
    flashPtr = 0;
    gpio_put(PIN_LED, 1);
    if (f_mount(&FATFS_Obj, "0", 1) == FR_OK) {
        FRESULT fr = updateList();
        if (fr != FR_OK) {
            return;
        }
        fr = openFile(autorunf, FA_READ);
        if (fr != FR_OK) {
            if (fr == FR_NO_FILE) {
                crtStat = CRS_NAUT; // Error 5 : No files to mount
            }
            else {
                crtStat = CRS_FDRE; // Error 0 : FAT driver error
            }
            errMode = 1;
        }
    }
    else {
        crtStat = CRS_BFAT; // Error 1 : bad filesystem
        errMode = 1;
    }
    gpio_put(PIN_LED, 0);
}

static void dormant_goto_and_comeback(void) {
    uint32_t intstatus = save_and_disable_interrupts();
    gpio_set_dormant_irq_enabled(PIN_SELECT, IO_BANK0_DORMANT_WAKE_INTE0_GPIO0_EDGE_LOW_BITS, true);

    // Turn off flash
    xip_ctrl_hw->ctrl |= XIP_CTRL_POWER_DOWN_BITS;
    // Turn off SRAMs
    syscfg_hw->mempowerdown =
        0x00000020 | //SRAM5
        0x00000010 | //SRAM4
        0x00000080 | //USB
        0x00000040; //ROM

    // Before we touch PLLs, switch sys and ref cleanly away from their aux sources.
    hw_clear_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
    while (clocks_hw->clk[clk_sys].selected != 0x1) tight_loop_contents();
    pll_deinit(pll_sys);
    pll_deinit(pll_usb);

    // Sleep
    xosc_dormant();

    // Turn on SRAMs
    syscfg_hw->mempowerdown = 0;
    // Turn on flash
    xip_ctrl_hw->ctrl &= ~XIP_CTRL_POWER_DOWN_BITS;
    // Clear the irq so we can go back to dormant mode again if we want
    gpio_acknowledge_irq(PIN_SELECT, IO_BANK0_DORMANT_WAKE_INTE0_GPIO0_EDGE_LOW_BITS);
    // Restore clocks
    clocks_initialize();
    restore_interrupts(intstatus);
}

static void adc_irq(void) {
    uint16_t value = adc_fifo_get();
    if ((value > 0) && (value < ADC_4V0_THRESH)) {
        //flushRwCart(); // we want, but can't, not enough power from MK90's standby PS :(
        dormant_goto_and_comeback();
        if (!gpio_get(PIN_ENAUMT)) { // Auto-Unmount on MK sleep enabled.
            remount();
        }
    }
}

static inline void adc_initialize(void) {
    adc_init();
    adc_gpio_init(29);
    adc_select_input(3); // start with measurement for GPIO29 - IP Used in ADC mode (ADC3) to measure VSYS/3
    adc_fifo_setup(true, false, 1, true, false);
    irq_set_exclusive_handler(ADC_IRQ_FIFO, adc_irq);
    adc_irq_set_enabled(true);
    irq_set_enabled(ADC_IRQ_FIFO, true);
    irq_set_priority(ADC_IRQ_FIFO, PICO_LOWEST_IRQ_PRIORITY);
    adc_set_clkdiv(65535); // slowest possible 65535: 832 measurements / sec
    adc_run(true);
}

static inline void parseMinimal(uint32_t command) {
    switch(command) {
        case 0x18:
        case 0x10: // read postdecrement
        case 0xD8:
        case 0xD0: // read postincrement
            do {
                uint8_t rdVal = (crsi_ptr > sizeof(crash_inform)) ? 0xFF : crash_inform[crsi_ptr];
                pio_sm_put(pio, sm_tx, rdVal << 24);
                if ((command & 0xC0) == 0) crsi_ptr--; // postdecrement
                else crsi_ptr++; // postincrement
                while (!pio_interrupt_get(pio, SENDED_IRQ)) tight_loop_contents();
            } while(!gpio_get(PIN_SELECT));
            break;

        case 0x00: // query status
            pio_sm_put(pio, sm_tx, 0x00 << 24);
            while (!pio_interrupt_get(pio, SENDED_IRQ) || !gpio_get(PIN_SELECT)) tight_loop_contents();
            break;

        case 0xA8: // set 24b address
            crsi_ptr = mk90bus_read24(pio, sm_rx) & 0xFFFF; // for compatibility
            break;

        case 0xA0: // set 16b address
            crsi_ptr = mk90bus_read16(pio, sm_rx);
            break;
            
        case 0xB0: // read 16b address
        case 0xB8: // read 24b address
            if (command & 0x08) {
                pio_sm_put(pio, sm_tx, (crsi_ptr >> 16) << 24);
                while (!pio_interrupt_get(pio, SENDED_IRQ)) tight_loop_contents();
            }
            pio_sm_put(pio, sm_tx, (crsi_ptr >> 8) << 24);
            while (!pio_interrupt_get(pio, SENDED_IRQ)) tight_loop_contents();
            pio_sm_put(pio, sm_tx, crsi_ptr << 24);
            while (!pio_interrupt_get(pio, SENDED_IRQ) || !gpio_get(PIN_SELECT)) tight_loop_contents();
            break;

        case 0xFE: // XMCart90: status code register (clears after reading)
            pio_sm_put(pio, sm_tx, crtStat << 24);
            crtStat = CRS_NONE;
            while (!pio_interrupt_get(pio, SENDED_IRQ) || !gpio_get(PIN_SELECT)) tight_loop_contents();
            break;

        default: // unknown command - just ignore
            while (!gpio_get(PIN_SELECT)) tight_loop_contents(); // wait until it ends
            break;
    }
}

static inline void parseCommand(uint32_t command) {
    FILINFO fno;
    FRESULT fr;
    FATFS *fs;
    DWORD fre_clust, fre_bytes;
    uint32_t i = 0;
    switch(command) {
        case 0x18:
        case 0x10: // read postdecrement
        case 0xD8:
        case 0xD0: // read postincrement
            if (rwCartMode) { // RAM mode
                if (contLock) {
                    break;
                }
                do {
                    pio_sm_put(pio, sm_tx, rwCart[rwCartPtr] << 24);
                    if ((command & 0xC0) == 0) rwCartPtr--; // postdecrement
                    else rwCartPtr++; // postincrement
                    while (!pio_interrupt_get(pio, SENDED_IRQ)) tight_loop_contents();
                } while(!gpio_get(PIN_SELECT));
            }
            else { // FLASH mode
                do {
                    pio_sm_put(pio, sm_tx, readBuffer << 24);
                    if ((command & 0xC0) == 0) { // postdecrement
                        if (flashPtr == 0) {  // 24-bit address overflow
                            flashPtr == 0x1000000;
                            f_lseek(&appFile, 0x1000000-1);
                        }
                        else if (flashPtr == appFile.obj.objsize) {
                            f_lseek(&appFile, appFile.fptr-1);
                        }
                        else if (flashPtr < appFile.obj.objsize) {
                            f_lseek(&appFile, appFile.fptr-2);
                        }
                        flashPtr--;
                    }
                    else { // postincrement
                        if (++flashPtr == 0x1000000) { // 24-bit address overflow
                            flashPtr = 0;
                            f_rewind(&appFile);
                        }
                    }
                    f_read_byte(&appFile, &readBuffer);
                    while (!pio_interrupt_get(pio, SENDED_IRQ)) tight_loop_contents();
                } while(!gpio_get(PIN_SELECT));
                if (flashPtr == 0) {  // 24-bit address overflow
                    flashPtr == 0x1000000;
                    f_lseek(&appFile, 0x1000000-1);
                }
                else if (flashPtr == appFile.obj.objsize) {
                    f_lseek(&appFile, appFile.fptr-1);
                }
                else if (flashPtr < appFile.obj.objsize) {
                    f_lseek(&appFile, appFile.fptr-2);
                }
                flashPtr--;
                f_read_byte(&appFile, &readBuffer);
            }
            break;
            
        case 0xE0: // write postdecrement
        case 0xC0: // write postincrement
            if (rwCartMode) { // RAM mode only
                if (contLock) {
                    break;
                }
                do {
                    readBuffer = mk90bus_read8(pio, sm_rx);
                    if (rwCartPtr < rwCartSize) {
                        rwCart[rwCartPtr] = readBuffer; // write if in virtual RAM "chip" range
                        rwCartReqFlush = 1;
                    }
                    if ((command & 0x20) != 0) rwCartPtr--; // postdecrement
                    else rwCartPtr++; // postincrement
                    while (pio_sm_is_rx_fifo_empty(pio, sm_rx) && !gpio_get(PIN_SELECT)) tight_loop_contents();
                } while(!gpio_get(PIN_SELECT));
            }
            break;
            
        case 0x20: // erase postdecrement
            if (rwCartMode && (rwCartPtr == 0xFFFF)) { // RAM mode only, addres must be at the end of the 64KB space
                uint16_t ercount = 0;
                do {
                    readBuffer = mk90bus_read8(pio, sm_rx);
                    if (rwCartPtr < rwCartSize) {
                        rwCart[rwCartPtr] = readBuffer; // write if in virtual RAM "chip" range
                        rwCartReqFlush = 1;
                    }
                    rwCartPtr--; // postdecrement only
                    if (contLock) {
                        if (++ercount == 0){ // Cart unlock if entire 64KB space erased
                            contLock = 0;
                            cLAttm = 0;
                        }
                    }
                    while (pio_sm_is_rx_fifo_empty(pio, sm_rx) && !gpio_get(PIN_SELECT)) tight_loop_contents();
                } while(!gpio_get(PIN_SELECT));
            }
            break;

        case 0x00: // query status
            pio_sm_put(pio, sm_tx, ((contLock << 7) & (cLAttm << 5)) << 24);
            while (!pio_interrupt_get(pio, SENDED_IRQ) || !gpio_get(PIN_SELECT)) tight_loop_contents();
            break;

        case 0xA8: // set 24b address
            if (rwCartMode) { // RAM mode
                rwCartPtr = mk90bus_read24(pio, sm_rx) & 0xFFFF; // for compatibility
            }
            else { // FLASH mode
                flashPtr = mk90bus_read24(pio, sm_rx);
                f_lseek(&appFile, flashPtr);
                f_read_byte(&appFile, &readBuffer);
            }
            break;

        case 0xA0: // set 16b address
            if (rwCartMode) { // RAM mode
                rwCartPtr = mk90bus_read16(pio, sm_rx);
            }
            else { // FLASH mode
                flashPtr = mk90bus_read16(pio, sm_rx);
                f_lseek(&appFile, flashPtr);
                f_read_byte(&appFile, &readBuffer);
            }
            break;
            
        case 0xB0: // read 16b address
        case 0xB8: // read 24b address
            uint32_t addr = rwCartMode ? rwCartPtr : flashPtr;
            if (command & 0x08) {
                pio_sm_put(pio, sm_tx, (addr >> 16) << 24);
            }
            pio_sm_put(pio, sm_tx, (addr >> 8) << 24);
            pio_sm_put(pio, sm_tx, addr << 24);
            while (!pio_interrupt_get(pio, SENDED_IRQ) || !gpio_get(PIN_SELECT)) tight_loop_contents();
            break;
            
        case 0x80: // lock content (just why not)
            if (rwCartMode && !contLock) { // RAM mode
                contLock = 1;
            }
            break;
            
        case 0x90: // unlock content (same)
            if (rwCartMode) { // RAM mode
                if (!contLock || (cLAttm == 3) || (rwCartPtr != 0)) {
                    break;
                }
                
                i = 0;
                uint8_t passw[8] = {0};
                do {
                passw[i] = tolower(mk90bus_read8(pio, sm_rx));
                } while (++i < 9);
                
                bool correct = 1;
                for (int i = 0; i < 8; i++) {
                    if (passw[i] != rwCart[rwCartPtr++]){
                        correct = 0;
                        break;
                    }
                }
                
                if (correct) {
                    contLock = 0;
                    cLAttm = 0;
                }
                else {
                    cLAttm++;
                }
            }
            break;

        case 0xF0: { // Genjitsu: get files list
            i = 0;
            while (fileList[i][0]) {
                uint32_t j = 0;
                do {
                    pio_sm_put(pio, sm_tx, fileList[i][j] << 24);
                    while (!pio_interrupt_get(pio, SENDED_IRQ)) tight_loop_contents();
                } while (fileList[i][j++]);
                i++;
            }
            pio_sm_put(pio, sm_tx, 0xFF << 24);
            while (!pio_interrupt_get(pio, SENDED_IRQ) || !gpio_get(PIN_SELECT)) tight_loop_contents();
            }
            break;

        case 0xF1: // Genjitsu: mount selected file
            char mountfile[12+1] = {0};
            i = 0;
            do {
                mountfile[i] = tolower(mk90bus_read8(pio, sm_rx));
            } while (mountfile[i] && ++i < 13);
            flushRwCart();
            if (errMode) {
                break;
            }
            
            fr = openFile(mountfile, FA_READ);
            if (fr != FR_OK) {
                if (fr == FR_INVALID_NAME) {
                    crtStat = CRS_IFIN; // Error 6 : invalid file name
                }
                else if (fr == FR_NO_PATH) {
                    crtStat = CRS_IPAT; // Error 7 : invalid path
                }
                else if (fr == FR_NO_FILE) {
                    crtStat = CRS_FINF; // Error A : file not found
                }
                else {
                    crtStat = CRS_FDRE; // Error 0 : FAT driver error
                    errMode = 1;
                    break;
                }
                remount();
                break;
            }
            
            gpio_put(PIN_LED, 1);
            
            // is this is a RW image?
            strcpy(filename, mountfile);
            fr = f_stat(filename, &fno);
            
            if (fr != FR_OK) {
                crtStat = CRS_FDRE; // Error 0 : FAT driver error
                errMode = 1;
                break;
            }
            
            char *dot = strrchr(filename, '.');
            if ((dot && (strcmp(dot, ".smp") == 0)) && (fno.fsize <= 64*1024)
                 && (fno.fsize >= 1024) && (fno.fsize % 1024 == 0) && !(fno.fattrib & AM_RDO)){
                rwCartMode = 1;
                rwCartSize = fno.fsize;
                rwCartReqFlush = 0;
                rwCartPtr = 0;
                memset(rwCart, 0xFF, 64*1024);
                UINT br = 0;
                if ((f_read(&appFile, rwCart, rwCartSize, &br) != FR_OK) || (br != rwCartSize)) {
                    crtStat = CRS_RERR; // Error 3 : read error
                    errMode = 1;
                    break;
                }
                f_close(&appFile);
            }
            else {
                rwCartMode = 0;
                flashPtr = 0;
            }
            gpio_put(PIN_LED, 0);
            break;
            
        case 0xF2: // Genjitsu: unmount current file and go to the autorun
            remount();
            break;
            
        case 0xFA: // XMCart90: create and mount new file
            DWORD nfsize = mk90bus_read8(pio, sm_rx); // first byte - configuration
            bool format = (nfsize & 0x80); // b7 = BASIC format required
            bool forver = (nfsize & 0x40); // b6 = BASIC version (0 - 1.0, 1 - 2.0)
            nfsize = ((nfsize & 0x3F) + 1) * 1024; // b5-b0 = file size in n+1 KB (0 = 1KB, ..., 63 = 64KB)
            
            char newfile[12+1] = {0};
            uint32_t i = 0;
            do {
                newfile[i] = tolower(mk90bus_read8(pio, sm_rx));
            } while (newfile[i] && ++i < 9); // name without extension (always be ".SMP")
            strcpy(newfile+i, ".smp");
            
            flushRwCart();
            if (errMode) {
                break;
            }

            fr = f_stat(newfile, &fno);
            
            if (fr != FR_NO_FILE) {
                if (fr == FR_INVALID_NAME) {
                    crtStat = CRS_IFIN; // Error 6 : invalid file name
                }
                else if (fr == FR_NO_PATH) {
                    crtStat = CRS_IPAT; // Error 7 : invalid path
                }
                else if (fr == FR_OK) {
                    crtStat = CRS_FAEX; // Error 8 : file already exists
                }
                else {
                    crtStat = CRS_FDRE; // Error 0 : FAT driver error
                    errMode = 1;
                }
                break;
            }
            
            if (f_getfree("0", &fre_clust, &fs) != FR_OK) {
                crtStat = CRS_DSIE; // Error 2 : FS space calc error
                errMode = 1;
                break;
            }
            fre_bytes = fre_clust * fs->csize * FLASH_SECTOR_SIZE;
            
            if (fre_bytes < nfsize) {
                crtStat = CRS_NESP; // Error 9 : not enough space
                remount();
                break;
            }
            
            if (openFile(newfile, FA_CREATE_NEW | FA_WRITE) != FR_OK) {
                crtStat = CRS_FDRE; // Error 0 : FAT driver error
                errMode = 1;
                break;
            }
            
            rwCartMode = 1;
            rwCartSize = nfsize;
            rwCartReqFlush = 0;
            rwCartPtr = 0;
            gpio_put(PIN_LED, 1);
                        
            if (format) {
                basic_format(rwCart, rwCartSize, forver);
            }
            else {
                memset(rwCart, 0xFF, 64*1024);
            }
            
            UINT bw = 0;
            fr = f_write(&appFile, rwCart, rwCartSize, &bw);
            if ((fr != FR_OK) || (bw != rwCartSize)) {
                crtStat = CRS_WERR; // Error 4 : write error
                f_close(&appFile);
                errMode = 1;
                break;
            }   
            f_close(&appFile);
            strcpy(filename, newfile);
            crtStat = CRS_FCOK;
            updateList();
            gpio_put(PIN_LED, 0);
            break;
            
        case 0xFB: // XMCart90: rename file
            char oldname[12+1] = {0};
            char newname[12+1] = {0};
            bool rncfile = 0;
            i = 0;
            do {
               oldname[i] = tolower(mk90bus_read8(pio, sm_rx));
            } while (oldname[i] && ++i < 13);
            i = 0;
            do {
               newname[i] = tolower(mk90bus_read8(pio, sm_rx));
            } while (newname[i] && ++i < 13);
            flushRwCart();
            if (errMode) {
                break;
            }
            
            if (strcmp(newname, filename) == 0) {
                crtStat = CRS_FAEX; // Error 8 : file already exists
                break;
            }
            else if (strcmp(oldname, filename) == 0) { // renaming current file
                f_close(&appFile);
                rncfile = 1;
            }
            
            fr = f_stat(oldname, &fno);
            
            if (fr != FR_OK) {
                if (fr == FR_INVALID_NAME) {
                    crtStat = CRS_IFIN; // Error 6 : invalid file name
                }
                else if (fr == FR_NO_PATH) {
                    crtStat = CRS_IPAT; // Error 7 : invalid path
                }
                else if (fr == FR_NO_FILE) {
                    crtStat = CRS_FINF; // Error A : file not found
                }
                else {
                    crtStat = CRS_FDRE; // Error 0 : FAT driver error
                    errMode = 1;
                }
                break;
            }
            
            fr = f_stat(newname, &fno);
            
            if (fr != FR_NO_FILE) {
                if (fr == FR_INVALID_NAME) {
                    crtStat = CRS_IFIN; // Error 6 : invalid file name
                }
                else if (fr == FR_NO_PATH) {
                    crtStat = CRS_IPAT; // Error 7 : invalid path
                }
                else if (fr == FR_OK) {
                    crtStat = CRS_FAEX; // Error 8 : file already exists
                }
                else {
                    crtStat = CRS_FDRE; // Error 0 : FAT driver error
                    errMode = 1;
                }
                break;
            }
            
            fr = f_rename(oldname, newname);
            
            if (fr != FR_OK) {
                crtStat = CRS_FDRE; // Error 0 : FAT driver error
                errMode = 1;
                break;
            }
            
            crtStat = CRS_RNOK;
            
            if (rncfile) {
                remount();
            }
            else {
                updateList();
            }
            break;
            
        case 0xFC: // XMCart90: delete file
            char delfile[12+1] = {0};
            bool dlcfile = 0;
            i = 0;
            do {
               delfile[i] = tolower(mk90bus_read8(pio, sm_rx));
            } while (delfile[i] && ++i < 13);
            flushRwCart();
            if (errMode) {
                break;
            }
            
            if (strcmp(delfile, filename) == 0) { // deleting current file
                f_close(&appFile);
                dlcfile = 1;
            }
            
            fr = f_stat(delfile, &fno);
            
            if (fr != FR_OK) {
                if (fr == FR_INVALID_NAME) {
                    crtStat = CRS_IFIN; // Error 6 : invalid file name
                }
                else if (fr == FR_NO_PATH) {
                    crtStat = CRS_IPAT; // Error 7 : invalid path
                }
                else if (fr == FR_NO_FILE) {
                    crtStat = CRS_FINF; // Error A : file not found
                }
                else {
                    crtStat = CRS_FDRE; // Error 0 : FAT driver error
                    errMode = 1;
                }
                break;
            }
            
            fr = f_unlink(delfile);
            
            if (fr != FR_OK) {
                if (fr == FR_DENIED || fr == FR_WRITE_PROTECTED) {
                    crtStat = CRS_ACDN; // Error B : Access denied
                }
                else {
                    crtStat = CRS_FDRE; // Error 0 : FAT driver error
                    errMode = 1;
                }
                break;
            }
            
            crtStat = CRS_DLOK;
            
            if (dlcfile) {
                remount();
            }
            else {
                updateList();
            }
            break;
        
        case 0xFD: // XMCart90: get total and free space
            for (int i = 0; i < 2; i++) { // 4 bytes - total, 4 bytes - free.
                pio_sm_put(pio, sm_tx, (diskSpaces[i] >> 24) << 24);
                pio_sm_put(pio, sm_tx, (diskSpaces[i] >> 16) << 24);
                pio_sm_put(pio, sm_tx, (diskSpaces[i] >> 8) << 24);
                pio_sm_put(pio, sm_tx, diskSpaces[i] << 24);
                while (!pio_interrupt_get(pio, SENDED_IRQ)) tight_loop_contents();
            }
            while (!gpio_get(PIN_SELECT)) tight_loop_contents();
            if (crtStat == CRS_DSIE) {
                break;
            }
            crtStat = CRS_DIOK;
            break;
            
        case 0xFE: // XMCart90: status code register (clears after reading)
            pio_sm_put(pio, sm_tx, crtStat << 24);
            crtStat = CRS_NONE;
            while (!pio_interrupt_get(pio, SENDED_IRQ) || !gpio_get(PIN_SELECT)) tight_loop_contents();
            break;

        default: // unknown command - just ignore
            while (!gpio_get(PIN_SELECT)) tight_loop_contents(); // wait until it ends
            break;
    }
}

static void pio_irq(void) {
    if (!errMode){
        parseCommand(mk90bus_read8(pio, sm_rx));
    }
    else {
        parseMinimal(mk90bus_read8(pio, sm_rx));
    }
    pio_sm_clear_fifos(pio, sm_rx);
    irq_clear(PIO0_IRQ_0);
}

int main(void) {
    // Set system clock to 200,000 kHz (200 MHz)
    //set_sys_clock_khz(200000, true);
    
    gpio_initialize();
    rosc_disable();

    //Disable XIP Cache
    xip_ctrl_hw->ctrl &= ~XIP_CTRL_EN_BITS;

    //Sleep if USB VBUS isn't present
    if (!gpio_get(PIN_VBUS)) {
        dormant_goto_and_comeback();
    } else {
        clocks_initialize();
    }
    adc_initialize();
    pio_initialize();
    tusb_init();
    remount();

    while (1) {
        __wfi();
        if (errMode) {
            error_handler();
        }
        else if (rwCartReqFlush && !gpio_get(PIN_SAVRAM)) { // Save RAM image to the file on FLASH
            flushRwCart();
        }
        else if (!gpio_get(PIN_ENUSBM)) { // USB operation enabled
            flushRwCart();
            if (errMode){
                continue;
            }
            gpio_put(PIN_LED, 1);
            adc_run(false); // disable auto-sleep function
            adc_irq_set_enabled(false);
            pio_sm_set_enabled(pio, sm_rx, false); // disable SMP emulation
            pio_sm_set_enabled(pio, sm_tx, false);
            tud_connect();
            while (!gpio_get(PIN_ENUSBM)){
                tud_task(); // tinyusb device task
            }
            tud_disconnect();
            pio_sm_set_enabled(pio, sm_tx, true); // enable SMP emulation
            pio_sm_set_enabled(pio, sm_rx, true);
            adc_irq_set_enabled(true); // enable auto-sleep function
            adc_run(true);
            remount(); // reset mounted file
        }
    }
}
