/*******************************************************************************
*   Factom Wallet 
*   (c) 2018 The Factoid Authority
*            ledger@factoid.org
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/

#include "os.h"
#include "cx.h"
#include <stdbool.h>

#include "fctUtils.h"
#include "fctParse.h"
#include "ecParse.h"
#include "ccParse.h"
#include "uint256.h"

#include "os_io_seproxyhal.h"
#include "btchip_apdu_constants.h"
#include "string.h"

#include "glyphs.h"

#ifdef HAVE_U2F

#include "u2f_service.h"
#include "u2f_transport.h"

volatile unsigned char u2fMessageBuffer[U2F_MAX_MESSAGE_SIZE];

extern void USB_power_U2F(unsigned char enabled, unsigned char fido);
extern bool fidoActivated;

#endif

unsigned char G_io_seproxyhal_spi_buffer[IO_SEPROXYHAL_BUFFER_SIZE_B];

unsigned int io_seproxyhal_touch_settings(const bagl_element_t *e);
unsigned int io_seproxyhal_touch_exit(const bagl_element_t *e);
unsigned int io_seproxyhal_touch_tx_ok(const bagl_element_t *e);
unsigned int io_seproxyhal_touch_ec_tx_ok(const bagl_element_t *e);
unsigned int io_seproxyhal_touch_tx_cancel(const bagl_element_t *e);
unsigned int io_seproxyhal_touch_address_ok(const bagl_element_t *e);
unsigned int io_seproxyhal_touch_address_cancel(const bagl_element_t *e);

void ui_idle(void);

uint32_t set_result_get_publicKey(void);

//the address maxpath will be 10 when path (m/44'/131'/0'/0') is encoded to an array 
#define MAX_BIP32_PATH 10 

#define CLA 0xE0
#define INS_GET_PUBLIC_KEY 0x02
#define INS_SIGN 0x04
#define INS_GET_APP_CONFIGURATION 0x06
#define INS_SIGN_PERSONAL_MESSAGE 0x08
#define INS_GET_PUBLIC_EC_KEY 0x10
#define INS_COMMIT_SIGN 0x12

#define COIN_TYPE_EC 132
#define COIN_TYPE_FCT 131


#define P1_CONFIRM 0x01
#define P1_NON_CONFIRM 0x00
#define P2_NO_CHAINCODE 0x00
#define P2_CHAINCODE 0x01
#define P1_FIRST 0x00
#define P1_MORE 0x80
#define P1_LAST 0x8F


#define OFFSET_CLA 0
#define OFFSET_INS 1
#define OFFSET_P1 2
#define OFFSET_P2 3
#define OFFSET_LC 4
#define OFFSET_CDATA 5

//max transaction can be the header + 10 inputs + 10 outputs
#define MAX_OUTPUTS 10
#define TX_HEADER_SIZE (1+6+3)
#define TX_MAX_AMT_ADDR_SIZE (8+32)
#define TX_MAX_AMT_ADDR_COUNT MAX_OUTPUTS
#define MAX_RAW_TX TX_HEADER_SIZE + MAX_OUTPUTS * TX_MAX_AMT_ADDR_SIZE + TX_MAX_AMT_ADDR_SIZE //200

typedef struct publicKeyContext_t {
    cx_ecfp_public_key_t publicKey;
    uint8_t address[53];//factom addresses are 52 bytes
    uint8_t chainCode[32];
    bool getChaincode;
} publicKeyContext_t;

typedef struct transactionContext_t {
    cx_curve_t curve;
    uint8_t pathLength;
    uint32_t bip32Path[MAX_BIP32_PATH];
    uint8_t rawTx[MAX_RAW_TX];
    uint32_t rawTxLength;
    uint32_t expectedTxLength;
} transactionContext_t;


typedef struct messageSigningContext_t {
    uint8_t pathLength;
    uint32_t bip32Path[MAX_BIP32_PATH];
    uint8_t hash[32];
    uint32_t remainingLength;
} messageSigningContext_t;

union {
    publicKeyContext_t publicKeyContext;
    transactionContext_t transactionContext;
    messageSigningContext_t messageSigningContext;
} tmpCtx;

txContent_t txContent;

union {
    txContent_t txContent;
    cx_sha256_t sha2;
} tmpContent;

#define FCT_ADDRESS_LENGTH 52

volatile uint8_t dataAllowed;
volatile uint8_t fidoTransport;
volatile char fullAddress[FCT_ADDRESS_LENGTH+1];
volatile char addressSummary[32];
volatile char fullAmount[32];
volatile char maxFee[16];
volatile bool dataPresent;
volatile bool skipWarning;
volatile uint8_t addresses_type[MAX_OUTPUTS];
volatile txContentAddress_t *addresses[MAX_OUTPUTS];

bagl_element_t tmp_element;

#ifdef HAVE_U2F

volatile u2f_service_t u2fService;

#endif

ux_state_t ux;
// display stepped screens
unsigned int ux_step;
unsigned int ux_step_count;

typedef struct internalStorage_t {
    uint8_t dataAllowed;
    uint8_t fidoTransport;
    uint8_t initialized;
} internalStorage_t;

WIDE internalStorage_t N_storage_real;
#define N_storage (*(WIDE internalStorage_t *)PIC(&N_storage_real))


//static const char const SIGN_MAGIC[] = "\x19"
//                                       "Factoid Signed Message:\n";

const unsigned char hex_digits[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

void array_hexstr(char *strbuf, const void *bin, unsigned int len) {
    while (len--) {
        *strbuf++ = hex_digits[((*((char *)bin)) >> 4) & 0xF];
        *strbuf++ = hex_digits[(*((char *)bin)) & 0xF];
        bin = (const void *)((unsigned int)bin + 1);
    }
    *strbuf = 0; // EOS
}

#ifdef HAVE_U2F

void u2f_proxy_response(u2f_service_t *service, unsigned int tx) {
    os_memset(service->messageBuffer, 0, 5);
    os_memmove(service->messageBuffer + 5, G_io_apdu_buffer, tx);
    service->messageBuffer[tx + 5] = 0x90;
    service->messageBuffer[tx + 6] = 0x00;
    u2f_send_fragmented_response(service, U2F_CMD_MSG, service->messageBuffer,
                                 tx + 7, true);
}

#endif

const bagl_element_t *ui_menu_item_out_over(const bagl_element_t *e) {
    // the selection rectangle is after the none|touchable
    e = (const bagl_element_t *)(((unsigned int)e) + sizeof(bagl_element_t));
    return e;
}

#define BAGL_FONT_OPEN_SANS_LIGHT_16_22PX_AVG_WIDTH 10
#define BAGL_FONT_OPEN_SANS_REGULAR_10_13PX_AVG_WIDTH 8
#define MAX_CHAR_PER_LINE 25

#define COLOR_BG_1 0xF9F9F9
#define COLOR_APP 0x0ebdcf
#define COLOR_APP_LIGHT 0x87dee6

#if defined(TARGET_BLUE)
const bagl_element_t ui_idle_blue[] = {
    // type                               userid    x    y   w    h  str rad
    // fill      fg        bg      fid iid  txt   touchparams...       ]
    {{BAGL_RECTANGLE, 0x00, 0, 68, 320, 413, 0, 0, BAGL_FILL, COLOR_BG_1,
      0x000000, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    // erase screen (only under the status bar)
    {{BAGL_RECTANGLE, 0x00, 0, 20, 320, 48, 0, 0, BAGL_FILL, COLOR_APP,
      COLOR_APP, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    /// TOP STATUS BAR
    {{BAGL_LABELINE, 0x00, 0, 45, 320, 30, 0, 0, BAGL_FILL, 0xFFFFFF, COLOR_APP,
      BAGL_FONT_OPEN_SANS_SEMIBOLD_10_13PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "FACTOM",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 0, 19, 56, 44, 0, 0,
      BAGL_FILL, COLOR_APP, COLOR_APP_LIGHT,
      BAGL_FONT_SYMBOLS_0 | BAGL_FONT_ALIGNMENT_CENTER |
          BAGL_FONT_ALIGNMENT_MIDDLE,
      0},
     BAGL_FONT_SYMBOLS_0_SETTINGS,
     0,
     COLOR_APP,
     0xFFFFFF,
     io_seproxyhal_touch_settings,
     NULL,
     NULL},
    {{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 264, 19, 56, 44, 0, 0,
      BAGL_FILL, COLOR_APP, COLOR_APP_LIGHT,
      BAGL_FONT_SYMBOLS_0 | BAGL_FONT_ALIGNMENT_CENTER |
          BAGL_FONT_ALIGNMENT_MIDDLE,
      0},
     BAGL_FONT_SYMBOLS_0_DASHBOARD,
     0,
     COLOR_APP,
     0xFFFFFF,
     io_seproxyhal_touch_exit,
     NULL,
     NULL},

    // BADGE_MFW.GIF
    {{BAGL_ICON, 0x00, 135, 178, 50, 50, 0, 0, BAGL_FILL, 0, COLOR_BG_1, 0, 0},
     &C_badge_mfw,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x00, 0, 270, 320, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_LIGHT_16_22PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "Open MyFactomWallet",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x00, 0, 308, 320, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_10_13PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "Connect your Ledger Blue and open your",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x00, 0, 331, 320, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_10_13PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "preferred wallet to view your accounts.",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x00, 0, 450, 320, 14, 0, 0, 0, 0x999999, COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_8_11PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "Validation requests will show automatically.",
     10,
     0,
     COLOR_BG_1,
     NULL,
     NULL,
     NULL},
};

unsigned int ui_idle_blue_button(unsigned int button_mask,
                                 unsigned int button_mask_counter) {
    return 0;
}
#endif // #if defined(TARGET_BLUE)

#if defined(TARGET_NANOS)

const ux_menu_entry_t menu_main[];
const ux_menu_entry_t menu_settings[];
const ux_menu_entry_t menu_settings_browser[];
const ux_menu_entry_t menu_settings_data[];

#ifdef HAVE_U2F

// change the setting
void menu_settings_data_change(unsigned int enabled) {
    dataAllowed = enabled;
    nvm_write(&N_storage.dataAllowed, (void *)&dataAllowed, sizeof(uint8_t));
    USB_power_U2F(0, 0);
    USB_power_U2F(1, N_storage.fidoTransport);
    // go back to the menu entry
    UX_MENU_DISPLAY(0, menu_settings, NULL);
}

// show the currently activated entry
void menu_settings_data_init(unsigned int ignored) {
    UNUSED(ignored);
    UX_MENU_DISPLAY(N_storage.dataAllowed ? 1 : 0, menu_settings_data, NULL);
}

#ifdef HAVE_U2F
// change the setting
void menu_settings_browser_change(unsigned int enabled) {
    fidoTransport = enabled;
    nvm_write(&N_storage.fidoTransport, (void *)&fidoTransport,
              sizeof(uint8_t));
    USB_power_U2F(0, 0);
    USB_power_U2F(1, N_storage.fidoTransport);
    // go back to the menu entry
    UX_MENU_DISPLAY(1, menu_settings, NULL);
}

// show the currently activated entry
void menu_settings_browser_init(unsigned int ignored) {
    UNUSED(ignored);
    UX_MENU_DISPLAY(N_storage.fidoTransport ? 1 : 0, menu_settings_browser,
                    NULL);
}

const ux_menu_entry_t menu_settings_browser[] = {
    {NULL, menu_settings_browser_change, 0, NULL, "No", NULL, 0, 0},
    {NULL, menu_settings_browser_change, 1, NULL, "Yes", NULL, 0, 0},
    UX_MENU_END};
#endif // HAVE_U2F

const ux_menu_entry_t menu_settings_data[] = {
    {NULL, menu_settings_data_change, 0, NULL, "No", NULL, 0, 0},
    {NULL, menu_settings_data_change, 1, NULL, "Yes", NULL, 0, 0},
    UX_MENU_END};

const ux_menu_entry_t menu_settings[] = {
   // {NULL, menu_settings_data_init, 0, NULL, "Contract data", NULL, 0, 0},
#ifdef HAVE_U2F
    {NULL, menu_settings_browser_init, 0, NULL, "Browser support", NULL, 0, 0},
#endif // HAVE_U2F
    {menu_main, NULL, 1, &C_icon_back, "Back", NULL, 61, 40},
    UX_MENU_END};
#endif // HAVE_U2F

const ux_menu_entry_t menu_about[] = {
    {NULL, NULL, 0, NULL, "MyFactomWallet", ".com", 0, 0},
    {NULL, NULL, 0, NULL, "Version", APPVERSION, 0, 0},
    {NULL, NULL, 0, NULL, "ledger","@factoid.org", 0, 0},
    {menu_main, NULL, 2, &C_icon_back, "Back", NULL, 61, 40},
    UX_MENU_END};

const ux_menu_entry_t menu_main[] = {
    {NULL, NULL, 0, &C_icon_mfw, "Use MFW to", "view accounts", 33, 12},
    {menu_settings, NULL, 0, NULL, "Settings", NULL, 0, 0},
    {menu_about, NULL, 0, NULL, "About", NULL, 0, 0},
    {NULL, os_sched_exit, 0, &C_icon_dashboard, "Quit app", NULL, 50, 29},
    UX_MENU_END};

#endif // #if defined(TARGET_NANOS)

#if defined(TARGET_BLUE)
const bagl_element_t *ui_settings_blue_toggle_data(const bagl_element_t *e) {
    // swap setting and request redraw of settings elements
    uint8_t setting = N_storage.dataAllowed ? 0 : 1;
    nvm_write(&N_storage.dataAllowed, (void *)&setting, sizeof(uint8_t));

    // only refresh settings mutable drawn elements
    UX_REDISPLAY_IDX(12);

    // won't redisplay the bagl_none
    return 0;
}

const bagl_element_t *ui_settings_blue_toggle_browser(const bagl_element_t *e) {
    // swap setting and request redraw of settings elements
    uint8_t setting = N_storage.fidoTransport ? 0 : 1;
    nvm_write(&N_storage.fidoTransport, (void *)&setting, sizeof(uint8_t));

    // only refresh settings mutable drawn elements
    UX_REDISPLAY_IDX(12);

    // won't redisplay the bagl_none
    return 0;
}

// don't perform any draw/color change upon finger event over settings
const bagl_element_t *ui_settings_out_over(const bagl_element_t *e) {
    return NULL;
}

unsigned int ui_settings_back_callback(const bagl_element_t *e) {
    // go back to idle
    ui_idle();
    return 0;
}

const bagl_element_t ui_settings_blue[] = {
    // type                               userid    x    y   w    h  str rad
    // fill      fg        bg      fid iid  txt   touchparams...       ]
    {{BAGL_RECTANGLE, 0x00, 0, 68, 320, 413, 0, 0, BAGL_FILL, COLOR_BG_1,
      0x000000, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    // erase screen (only under the status bar)
    {{BAGL_RECTANGLE, 0x00, 0, 20, 320, 48, 0, 0, BAGL_FILL, COLOR_APP,
      COLOR_APP, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    /// TOP STATUS BAR
    {{BAGL_LABELINE, 0x00, 0, 45, 320, 30, 0, 0, BAGL_FILL, 0xFFFFFF, COLOR_APP,
      BAGL_FONT_OPEN_SANS_SEMIBOLD_10_13PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "SETTINGS",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 0, 19, 50, 44, 0, 0,
      BAGL_FILL, COLOR_APP, COLOR_APP_LIGHT,
      BAGL_FONT_SYMBOLS_0 | BAGL_FONT_ALIGNMENT_CENTER |
          BAGL_FONT_ALIGNMENT_MIDDLE,
      0},
     BAGL_FONT_SYMBOLS_0_LEFT,
     0,
     COLOR_APP,
     0xFFFFFF,
     ui_settings_back_callback,
     NULL,
     NULL},
    {{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 264, 19, 56, 44, 0, 0,
      BAGL_FILL, COLOR_APP, COLOR_APP_LIGHT,
      BAGL_FONT_SYMBOLS_0 | BAGL_FONT_ALIGNMENT_CENTER |
          BAGL_FONT_ALIGNMENT_MIDDLE,
      0},
     BAGL_FONT_SYMBOLS_0_DASHBOARD,
     0,
     COLOR_APP,
     0xFFFFFF,
     io_seproxyhal_touch_exit,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x00, 30, 105, 160, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     "Contract data",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x00, 30, 126, 260, 30, 0, 0, BAGL_FILL, 0x999999,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_8_11PX, 0},
     "Allow contract data in transactions",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_NONE | BAGL_FLAG_TOUCHABLE, 0x00, 0, 78, 320, 68, 0, 0, BAGL_FILL,
      0xFFFFFF, 0x000000, 0, 0},
     NULL,
     0,
     0xEEEEEE,
     0x000000,
     ui_settings_blue_toggle_data,
     ui_settings_out_over,
     ui_settings_out_over},

#ifdef HAVE_U2F
    {{BAGL_RECTANGLE, 0x00, 30, 146, 260, 1, 1, 0, 0, 0xEEEEEE, COLOR_BG_1, 0,
      0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x00, 30, 174, 160, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     "Browser support",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x00, 30, 195, 260, 30, 0, 0, BAGL_FILL, 0x999999,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_8_11PX, 0},
     "Enable integrated browser support",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_NONE | BAGL_FLAG_TOUCHABLE, 0x00, 0, 147, 320, 68, 0, 0, BAGL_FILL,
      0xFFFFFF, 0x000000, 0, 0},
     NULL,
     0,
     0xEEEEEE,
     0x000000,
     ui_settings_blue_toggle_browser,
     ui_settings_out_over,
     ui_settings_out_over},

    // at the end to minimize the number of refreshed items upon setting change
    {{BAGL_ICON, 0x02, 258, 167, 32, 18, 0, 0, BAGL_FILL, 0x000000, COLOR_BG_1,
      0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
#endif // HAVE_U2F
    {{BAGL_ICON, 0x01, 258, 98, 32, 18, 0, 0, BAGL_FILL, 0x000000, COLOR_BG_1,
      0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
};

const bagl_element_t *ui_settings_blue_prepro(const bagl_element_t *e) {
    // none elements are skipped
    if ((e->component.type & (~BAGL_FLAG_TOUCHABLE)) == BAGL_NONE) {
        return 0;
    }
    // swap icon buffer to be displayed depending on if corresponding setting is
    // enabled or not.
    if (e->component.userid) {
        os_memmove(&tmp_element, e, sizeof(bagl_element_t));
        switch (e->component.userid) {
        case 0x01:
            // swap icon content
            if (N_storage.dataAllowed) {
                tmp_element.text = &C_icon_toggle_set;
            } else {
                tmp_element.text = &C_icon_toggle_reset;
            }
            break;
        case 0x02:
            // swap icon content
            if (N_storage.fidoTransport) {
                tmp_element.text = &C_icon_toggle_set;
            } else {
                tmp_element.text = &C_icon_toggle_reset;
            }
            break;
        }
        return &tmp_element;
    }
    return 1;
}

unsigned int ui_settings_blue_button(unsigned int button_mask,
                                     unsigned int button_mask_counter) {
    return 0;
}
#endif // #if defined(TARGET_BLUE)

#if defined(TARGET_BLUE)
// reuse addressSummary for each line content
const char *ui_details_title;
const char *ui_details_content;
typedef void (*callback_t)(void);
callback_t ui_details_back_callback;

const bagl_element_t *
ui_details_blue_back_callback(const bagl_element_t *element) {
    ui_details_back_callback();
    return 0;
}

const bagl_element_t ui_details_blue[] = {
    // erase screen (only under the status bar)
    {{BAGL_RECTANGLE, 0x00, 0, 68, 320, 413, 0, 0, BAGL_FILL, COLOR_BG_1,
      0x000000, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_RECTANGLE, 0x00, 0, 20, 320, 48, 0, 0, BAGL_FILL, COLOR_APP,
      COLOR_APP, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    /// TOP STATUS BAR
    {{BAGL_LABELINE, 0x01, 0, 45, 320, 30, 0, 0, BAGL_FILL, 0xFFFFFF, COLOR_APP,
      BAGL_FONT_OPEN_SANS_SEMIBOLD_10_13PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 0, 19, 50, 44, 0, 0,
      BAGL_FILL, COLOR_APP, COLOR_APP_LIGHT,
      BAGL_FONT_SYMBOLS_0 | BAGL_FONT_ALIGNMENT_CENTER |
          BAGL_FONT_ALIGNMENT_MIDDLE,
      0},
     BAGL_FONT_SYMBOLS_0_LEFT,
     0,
     COLOR_APP,
     0xFFFFFF,
     ui_details_blue_back_callback,
     NULL,
     NULL},
    //{{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 264,  19,  56,  44, 0, 0,
    //BAGL_FILL, COLOR_APP, COLOR_APP_LIGHT,
    //BAGL_FONT_SYMBOLS_0|BAGL_FONT_ALIGNMENT_CENTER|BAGL_FONT_ALIGNMENT_MIDDLE,
    //0 }, " " /*BAGL_FONT_SYMBOLS_0_DASHBOARD*/, 0, COLOR_APP, 0xFFFFFF,
    //io_seproxyhal_touch_exit, NULL, NULL},

    {{BAGL_LABELINE, 0x00, 30, 106, 320, 30, 0, 0, BAGL_FILL, 0x999999,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_SEMIBOLD_8_11PX, 0},
     "VALUE",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x10, 30, 136, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x11, 30, 159, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x12, 30, 182, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x13, 30, 205, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x14, 30, 228, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x15, 30, 251, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x16, 30, 274, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x17, 30, 297, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x18, 30, 320, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    //"..." at the end if too much
    {{BAGL_LABELINE, 0x19, 30, 343, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x00, 0, 450, 320, 14, 0, 0, 0, 0x999999, COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_8_11PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "Review the whole value before continuing.",
     10,
     0,
     COLOR_BG_1,
     NULL,
     NULL,
     NULL},
};

const bagl_element_t *ui_details_blue_prepro(const bagl_element_t *element) {
    if (element->component.userid == 1) {
        os_memmove(&tmp_element, element, sizeof(bagl_element_t));
        tmp_element.text = ui_details_title;
        return &tmp_element;
    } else if (element->component.userid > 0) {
        unsigned int length = strlen(ui_details_content);
        if (length >= (element->component.userid & 0xF) * MAX_CHAR_PER_LINE) {
            os_memset(addressSummary, 0, MAX_CHAR_PER_LINE + 1);
            os_memmove(
                addressSummary,
                ui_details_content +
                    (element->component.userid & 0xF) * MAX_CHAR_PER_LINE,
                MIN(length -
                        (element->component.userid & 0xF) * MAX_CHAR_PER_LINE,
                    MAX_CHAR_PER_LINE));
            return 1;
        }
        // nothing to draw for this line
        return 0;
    }
    return 1;
}

unsigned int ui_details_blue_button(unsigned int button_mask,
                                    unsigned int button_mask_counter) {
    return 0;
}

void ui_details_init(const char *title, const char *content,
                     callback_t back_callback) {
    ui_details_title = title;
    ui_details_content = content;
    ui_details_back_callback = back_callback;
    UX_DISPLAY(ui_details_blue, ui_details_blue_prepro);
}

void ui_approval_blue_init(void);

bagl_element_callback_t ui_approval_blue_ok;
bagl_element_callback_t ui_approval_blue_cancel;

const bagl_element_t *ui_approval_blue_ok_callback(const bagl_element_t *e) {
    return ui_approval_blue_ok(e);
}

const bagl_element_t *
ui_approval_blue_cancel_callback(const bagl_element_t *e) {
    return ui_approval_blue_cancel(e);
}

typedef enum {
    APPROVAL_TRANSACTION,
    APPROVAL_MESSAGE,
} ui_approval_blue_state_t;
ui_approval_blue_state_t G_ui_approval_blue_state;
// pointer to value to be displayed
const char *ui_approval_blue_values[3];
// variable part of the structure
const char *const ui_approval_blue_details_name[][5] = {
    /*APPROVAL_TRANSACTION*/
    {
        "AMOUNT", "ADDRESS", "MAX FEES", "CONFIRM TRANSACTION",
        "Transaction details",
    },

    /*APPROVAL_MESSAGE*/
    {
        "HASH", NULL, NULL, "SIGN MESSAGE", "Message signature",
    },
};

const bagl_element_t *ui_approval_blue_1_details(const bagl_element_t *e) {
    if (strlen(ui_approval_blue_values[0]) *
            BAGL_FONT_OPEN_SANS_LIGHT_16_22PX_AVG_WIDTH >=
        160) {
        // display details screen
        ui_details_init(ui_approval_blue_details_name[G_ui_approval_blue_state]
                                                     [0],
                        ui_approval_blue_values[0], ui_approval_blue_init);
    }
    return 0;
};

const bagl_element_t *ui_approval_blue_2_details(const bagl_element_t *e) {
    if (strlen(ui_approval_blue_values[1]) *
            BAGL_FONT_OPEN_SANS_REGULAR_10_13PX_AVG_WIDTH >=
        160) {
        ui_details_init(ui_approval_blue_details_name[G_ui_approval_blue_state]
                                                     [1],
                        ui_approval_blue_values[1], ui_approval_blue_init);
    }
    return 0;
};

const bagl_element_t *ui_approval_blue_3_details(const bagl_element_t *e) {
    if (strlen(ui_approval_blue_values[2]) *
            BAGL_FONT_OPEN_SANS_REGULAR_10_13PX_AVG_WIDTH >=
        160) {
        ui_details_init(ui_approval_blue_details_name[G_ui_approval_blue_state]
                                                     [2],
                        ui_approval_blue_values[2], ui_approval_blue_init);
    }
    return 0;
};

const bagl_element_t ui_approval_blue[] = {
    {{BAGL_RECTANGLE, 0x00, 0, 68, 320, 413, 0, 0, BAGL_FILL, COLOR_BG_1,
      0x000000, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    // erase screen (only under the status bar)
    {{BAGL_RECTANGLE, 0x00, 0, 20, 320, 48, 0, 0, BAGL_FILL, COLOR_APP,
      COLOR_APP, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    /// TOP STATUS BAR
    {{BAGL_LABELINE, 0x60, 0, 45, 320, 30, 0, 0, BAGL_FILL, 0xFFFFFF, COLOR_APP,
      BAGL_FONT_OPEN_SANS_SEMIBOLD_10_13PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    // BADGE_TRANSACTION.GIF
    {{BAGL_ICON, 0x40, 30, 98, 50, 50, 0, 0, BAGL_FILL, 0, COLOR_BG_1, 0, 0},
     &C_badge_transaction,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x50, 100, 117, 320, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x00, 100, 138, 320, 30, 0, 0, BAGL_FILL, 0x999999,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_8_11PX, 0},
     "Check and confirm values",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x70, 30, 196, 100, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_SEMIBOLD_8_11PX, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL}, // AMOUNT
    // x-18 when ...
    {{BAGL_LABELINE, 0x10, 130, 200, 160, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_LIGHT_16_22PX | BAGL_FONT_ALIGNMENT_RIGHT,
      0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL}, // fullAmount
    {{BAGL_LABELINE, 0x20, 284, 196, 6, 16, 0, 0, BAGL_FILL, 0x999999,
      COLOR_BG_1, BAGL_FONT_SYMBOLS_0 | BAGL_FONT_ALIGNMENT_RIGHT, 0},
     BAGL_FONT_SYMBOLS_0_MINIRIGHT,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_NONE | BAGL_FLAG_TOUCHABLE, 0x00, 0, 168, 320, 48, 0, 9, BAGL_FILL,
      0xFFFFFF, 0x000000, 0, 0},
     NULL,
     0,
     0xEEEEEE,
     0x000000,
     ui_approval_blue_1_details,
     ui_menu_item_out_over,
     ui_menu_item_out_over},
    {{BAGL_RECTANGLE, 0x20, 0, 168, 5, 48, 0, 0, BAGL_FILL, COLOR_BG_1,
      COLOR_BG_1, 0, 0},
     NULL,
     0,
     0x41CCB4,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_RECTANGLE, 0x31, 30, 216, 260, 1, 1, 0, 0, 0xEEEEEE, COLOR_BG_1, 0,
      0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x71, 30, 245, 100, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_SEMIBOLD_8_11PX, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL}, // ADDRESS
    // x-18 when ...
    {{BAGL_LABELINE, 0x11, 130, 245, 160, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_10_13PX | BAGL_FONT_ALIGNMENT_RIGHT, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL}, // fullAddress
    {{BAGL_LABELINE, 0x21, 284, 245, 6, 16, 0, 0, BAGL_FILL, 0x999999,
      COLOR_BG_1, BAGL_FONT_SYMBOLS_0 | BAGL_FONT_ALIGNMENT_RIGHT, 0},
     BAGL_FONT_SYMBOLS_0_MINIRIGHT,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_NONE | BAGL_FLAG_TOUCHABLE, 0x00, 0, 217, 320, 48, 0, 9, BAGL_FILL,
      0xFFFFFF, 0x000000, 0, 0},
     NULL,
     0,
     0xEEEEEE,
     0x000000,
     ui_approval_blue_2_details,
     ui_menu_item_out_over,
     ui_menu_item_out_over},
    {{BAGL_RECTANGLE, 0x21, 0, 217, 5, 48, 0, 0, BAGL_FILL, COLOR_BG_1,
      COLOR_BG_1, 0, 0},
     NULL,
     0,
     0x41CCB4,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_RECTANGLE, 0x32, 30, 265, 260, 1, 1, 0, 0, 0xEEEEEE, COLOR_BG_1, 0,
      0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x72, 30, 294, 100, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_SEMIBOLD_8_11PX, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL}, // MAX FEES
    // x-18 when ...
    {{BAGL_LABELINE, 0x12, 130, 294, 160, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_10_13PX | BAGL_FONT_ALIGNMENT_RIGHT, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL}, // maxFee
    {{BAGL_LABELINE, 0x22, 284, 294, 6, 16, 0, 0, BAGL_FILL, 0x999999,
      COLOR_BG_1, BAGL_FONT_SYMBOLS_0 | BAGL_FONT_ALIGNMENT_RIGHT, 0},
     BAGL_FONT_SYMBOLS_0_MINIRIGHT,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_NONE | BAGL_FLAG_TOUCHABLE, 0x00, 0, 266, 320, 48, 0, 9, BAGL_FILL,
      0xFFFFFF, 0x000000, 0, 0},
     NULL,
     0,
     0xEEEEEE,
     0x000000,
     ui_approval_blue_3_details,
     ui_menu_item_out_over,
     ui_menu_item_out_over},
    {{BAGL_RECTANGLE, 0x22, 0, 266, 5, 48, 0, 0, BAGL_FILL, COLOR_BG_1,
      COLOR_BG_1, 0, 0},
     NULL,
     0,
     0x41CCB4,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_RECTANGLE, 0x06, 30, 314, 260, 1, 1, 0, 0, 0xEEEEEE, COLOR_BG_1, 0,
      0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x06, 30, 343, 120, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_SEMIBOLD_8_11PX, 0},
     "CONTRACT DATA",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    //{{BAGL_LABELINE                       , 0x05, 130, 343, 160,  30, 0, 0,
    //BAGL_FILL, 0x666666, COLOR_BG_1,
    //BAGL_FONT_OPEN_SANS_REGULAR_10_13PX|BAGL_FONT_ALIGNMENT_RIGHT, 0   }, "Not
    //present", 0, 0, 0, NULL, NULL, NULL},
    {{BAGL_LABELINE, 0x06, 133, 343, 140, 30, 0, 0, BAGL_FILL, 0x666666,
      COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_10_13PX | BAGL_FONT_ALIGNMENT_RIGHT, 0},
     "Present",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_ICON, 0x06, 278, 333, 12, 12, 0, 0, BAGL_FILL, 0, COLOR_BG_1, 0, 0},
     &C_icon_warning,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 40, 414, 115, 36, 0, 18,
      BAGL_FILL, 0xCCCCCC, COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_11_14PX | BAGL_FONT_ALIGNMENT_CENTER |
          BAGL_FONT_ALIGNMENT_MIDDLE,
      0},
     "REJECT",
     0,
     0xB7B7B7,
     COLOR_BG_1,
     ui_approval_blue_cancel_callback,
     NULL,
     NULL},
    {{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 165, 414, 115, 36, 0, 18,
      BAGL_FILL, 0x41ccb4, COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_11_14PX | BAGL_FONT_ALIGNMENT_CENTER |
          BAGL_FONT_ALIGNMENT_MIDDLE,
      0},
     "CONFIRM",
     0,
     0x3ab7a2,
     COLOR_BG_1,
     ui_approval_blue_ok_callback,
     NULL,
     NULL},

};

const bagl_element_t *ui_approval_blue_prepro(const bagl_element_t *element) {
    if (element->component.userid == 0) {
        return 1;
    }
    // none elements are skipped
    if ((element->component.type & (~BAGL_FLAG_TOUCHABLE)) == BAGL_NONE) {
        return 0;
    } else {
        switch (element->component.userid & 0xF0) {
        // icon
        case 0x40:
            return 1;
            break;

        // TITLE
        case 0x60:
            os_memmove(&tmp_element, element, sizeof(bagl_element_t));
            tmp_element.text =
                ui_approval_blue_details_name[G_ui_approval_blue_state][3];
            return &tmp_element;
            break;

        // SUBLINE
        case 0x50:
            os_memmove(&tmp_element, element, sizeof(bagl_element_t));
            tmp_element.text =
                ui_approval_blue_details_name[G_ui_approval_blue_state][4];
            return &tmp_element;

        // details label
        case 0x70:
            if (!ui_approval_blue_details_name[G_ui_approval_blue_state]
                                              [element->component.userid &
                                               0xF]) {
                return NULL;
            }
            os_memmove(&tmp_element, element, sizeof(bagl_element_t));
            tmp_element.text =
                ui_approval_blue_details_name[G_ui_approval_blue_state]
                                             [element->component.userid & 0xF];
            return &tmp_element;

        // detail value
        case 0x10:
            // won't display
            if (!ui_approval_blue_details_name[G_ui_approval_blue_state]
                                              [element->component.userid &
                                               0xF]) {
                return NULL;
            }
            // always display the value
            os_memmove(&tmp_element, element, sizeof(bagl_element_t));
            tmp_element.text =
                ui_approval_blue_values[(element->component.userid & 0xF)];

            // x -= 18 when overflow is detected
            if (strlen(ui_approval_blue_values[(element->component.userid &
                                                0xF)]) *
                    BAGL_FONT_OPEN_SANS_LIGHT_16_22PX_AVG_WIDTH >=
                160) {
                tmp_element.component.x -= 18;
            }
            return &tmp_element;
            break;

        // right arrow and left selection rectangle
        case 0x20:
            if (!ui_approval_blue_details_name[G_ui_approval_blue_state]
                                              [element->component.userid &
                                               0xF]) {
                return NULL;
            }
            if (strlen(ui_approval_blue_values[(element->component.userid &
                                                0xF)]) *
                    BAGL_FONT_OPEN_SANS_LIGHT_16_22PX_AVG_WIDTH <
                160) {
                return NULL;
            }

        // horizontal delimiter
        case 0x30:
            return ui_approval_blue_details_name[G_ui_approval_blue_state]
                                                [element->component.userid &
                                                 0xF] != NULL
                       ? element
                       : NULL;

        case 0x05:
            return !dataPresent;
        case 0x06:
            return dataPresent;
        }
    }
    return element;
}
unsigned int ui_approval_blue_button(unsigned int button_mask,
                                     unsigned int button_mask_counter) {
    return 0;
}

#endif // #if defined(TARGET_BLUE)

#if defined(TARGET_BLUE)
const bagl_element_t ui_address_blue[] = {
    {{BAGL_RECTANGLE, 0x00, 0, 68, 320, 413, 0, 0, BAGL_FILL, COLOR_BG_1,
      0x000000, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    // erase screen (only under the status bar)
    {{BAGL_RECTANGLE, 0x00, 0, 20, 320, 48, 0, 0, BAGL_FILL, COLOR_APP,
      COLOR_APP, 0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    /// TOP STATUS BAR
    {{BAGL_LABELINE, 0x00, 0, 45, 320, 30, 0, 0, BAGL_FILL, 0xFFFFFF, COLOR_APP,
      BAGL_FONT_OPEN_SANS_SEMIBOLD_10_13PX | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "CONFIRM ACCOUNT",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    //{{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 264,  19,  56,  44, 0, 0,
    //BAGL_FILL, COLOR_APP, COLOR_APP_LIGHT,
    //BAGL_FONT_SYMBOLS_0|BAGL_FONT_ALIGNMENT_CENTER|BAGL_FONT_ALIGNMENT_MIDDLE,
    //0 }, " " /*BAGL_FONT_SYMBOLS_0_DASHBOARD*/, 0, COLOR_APP, 0xFFFFFF,
    //io_seproxyhal_touch_exit, NULL, NULL},

    {{BAGL_LABELINE, 0x00, 30, 106, 320, 30, 0, 0, BAGL_FILL, 0x999999,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_SEMIBOLD_8_11PX, 0},
     "ACCOUNT",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x10, 30, 136, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x11, 30, 159, 260, 30, 0, 0, BAGL_FILL, 0x000000,
      COLOR_BG_1, BAGL_FONT_OPEN_SANS_REGULAR_10_13PX, 0},
     addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 40, 414, 115, 36, 0, 18,
      BAGL_FILL, 0xCCCCCC, COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_11_14PX | BAGL_FONT_ALIGNMENT_CENTER |
          BAGL_FONT_ALIGNMENT_MIDDLE,
      0},
     "REJECT",
     0,
     0xB7B7B7,
     COLOR_BG_1,
     io_seproxyhal_touch_address_cancel,
     NULL,
     NULL},
    {{BAGL_RECTANGLE | BAGL_FLAG_TOUCHABLE, 0x00, 165, 414, 115, 36, 0, 18,
      BAGL_FILL, 0x41ccb4, COLOR_BG_1,
      BAGL_FONT_OPEN_SANS_REGULAR_11_14PX | BAGL_FONT_ALIGNMENT_CENTER |
          BAGL_FONT_ALIGNMENT_MIDDLE,
      0},
     "CONFIRM",
     0,
     0x3ab7a2,
     COLOR_BG_1,
     io_seproxyhal_touch_address_ok,
     NULL,
     NULL},
};

unsigned int ui_address_blue_prepro(const bagl_element_t *element) {
    if (element->component.userid > 0) {
        unsigned int length = strlen(fullAddress);
        if (length >= (element->component.userid & 0xF) * MAX_CHAR_PER_LINE) {
            os_memset(addressSummary, 0, MAX_CHAR_PER_LINE + 1);
            os_memmove(
                addressSummary,
                fullAddress +
                    (element->component.userid & 0xF) * MAX_CHAR_PER_LINE,
                MIN(length -
                        (element->component.userid & 0xF) * MAX_CHAR_PER_LINE,
                    MAX_CHAR_PER_LINE));
            return 1;
        }
        // nothing to draw for this line
        return 0;
    }
    return 1;
}

unsigned int ui_address_blue_button(unsigned int button_mask,
                                    unsigned int button_mask_counter) {
    return 0;
}
#endif // #if defined(TARGET_BLUE)

#if defined(TARGET_NANOS)

#include "ui_address_nanos.h"

unsigned int ui_address_prepro(const bagl_element_t *element) {
    if (element->component.userid > 0) {
        unsigned int display = (ux_step == element->component.userid - 1);
        if (display) {
            switch (element->component.userid) {
            case 1:
                UX_CALLBACK_SET_INTERVAL(2000);
                break;
            case 2:
                UX_CALLBACK_SET_INTERVAL(MAX(
                    3000, 1000 + bagl_label_roundtrip_duration_ms(element, 7)));
                break;
            }
        }
        return display;
    }
    return 1;
}

unsigned int ui_address_nanos_button(unsigned int button_mask,
                                     unsigned int button_mask_counter);
#endif // #if defined(TARGET_NANOS)

#if defined(TARGET_NANOS)
const char *const ui_approval_details[][2] = {
    {"Output 1 Amount", (const char*)fullAmount}, {"Output 1 Address", (const char*)addressSummary},
    {"Output 2 Amount", (const char*)fullAmount}, {"Output 2 Address", (const char*)addressSummary},
    {"Output 3 Amount", (const char*)fullAmount}, {"Output 3 Address", (const char*)addressSummary},
    {"Output 4 Amount", (const char*)fullAmount}, {"Output 4 Address", (const char*)addressSummary},
    {"Output 5 Amount", (const char*)fullAmount}, {"Output 5 Address", (const char*)addressSummary},
    {"Output 6 Amount", (const char*)fullAmount}, {"Output 6 Address", (const char*)addressSummary},
    {"Output 7 Amount", (const char*)fullAmount}, {"Output 7 Address", (const char*)addressSummary},
    {"Output 8 Amount", (const char*)fullAmount}, {"Output 8 Address", (const char*)addressSummary},
    {"Output 9 Amount", (const char*)fullAmount}, {"Output 9 Address", (const char*)addressSummary},
    {"Output 10 Amount", (const char*)fullAmount}, {"Output 10 Address", (const char*)addressSummary},
    {"Fees", (const char*)maxFee},
};

const bagl_element_t ui_approval_nanos[] = {
    // type                               userid    x    y   w    h  str rad
    // fill      fg        bg      fid iid  txt   touchparams...       ]
    {{BAGL_RECTANGLE, 0x00, 0, 0, 128, 32, 0, 0, BAGL_FILL, 0x000000, 0xFFFFFF,
      0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_ICON, 0x00, 3, 12, 7, 7, 0, 0, 0, 0xFFFFFF, 0x000000, 0,
      BAGL_GLYPH_ICON_CROSS},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_ICON, 0x00, 117, 13, 8, 6, 0, 0, 0, 0xFFFFFF, 0x000000, 0,
      BAGL_GLYPH_ICON_CHECK},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    //{{BAGL_ICON                           , 0x01,  21,   9,  14,  14, 0, 0, 0
    //, 0xFFFFFF, 0x000000, 0, BAGL_GLYPH_ICON_TRANSACTION_BADGE  }, NULL, 0, 0,
    // 0, NULL, NULL, NULL },
    {{BAGL_LABELINE, 0x01, 0, 12, 128, 32, 0, 0, 0, 0xFFFFFF, 0x000000,
      BAGL_FONT_OPEN_SANS_EXTRABOLD_11px | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "Confirm",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x01, 0, 26, 128, 32, 0, 0, 0, 0xFFFFFF, 0x000000,
      BAGL_FONT_OPEN_SANS_EXTRABOLD_11px | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "transaction",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_LABELINE, 0x02, 0, 12, 128, 32, 0, 0, 0, 0xFFFFFF, 0x000000,
      BAGL_FONT_OPEN_SANS_REGULAR_11px | BAGL_FONT_ALIGNMENT_CENTER, 0},
     NULL, //"Amount",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x12, 23, 26, 82, 12, 0x80 | 10, 0, 0, 0xFFFFFF, 0x000000,
      BAGL_FONT_OPEN_SANS_EXTRABOLD_11px | BAGL_FONT_ALIGNMENT_CENTER, 26},
     NULL, //(char *)fullAmount,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

};

unsigned int ui_approval_prepro(const bagl_element_t *element) {
    unsigned int display = 1;
    uint8_t offset = 0;
    if (element->component.userid > 0) {
        // display the meta element when at least bigger
        display = (ux_step == element->component.userid - 1) ||
                  (element->component.userid >= 0x02 && ux_step >= 1);
        if (display) {
            switch (element->component.userid) {
            case 0x01:
                UX_CALLBACK_SET_INTERVAL(1000);
                break;
            case 0x02:
            case 0x12:
                os_memmove(&tmp_element, element, sizeof(bagl_element_t));
                display = ux_step - 1;
                switch (display) {
                case 0: // amount
                    offset = 0;
                    display_amount_offset:
                    if ( addresses[offset] )
                    {
                        fct_print_amount(addresses[offset]->value,(int8_t*)fullAmount,sizeof(fullAmount));

                        goto display_detail;
                    }
                case 1: // address
                    offset = 0;
                    display_address_offset:
                    if ( addresses[offset] )
                    {
                        getFctAddressStringFromRCDHash((uint8_t*)addresses[offset]->rcdhash,(uint8_t*)fullAddress, addresses_type[offset]);

                        os_memmove((void *)addressSummary, (void*)fullAddress, 10);
                        os_memmove((void *)(addressSummary + 10), "..", 2);
                        os_memmove((void *)(addressSummary + 12),
                                   (void*)(fullAddress + FCT_ADDRESS_LENGTH - 4), 4);
                        goto display_detail;
                    }
                display_detail:
                    tmp_element.text =
                        ui_approval_details[display]
                                           [(element->component.userid) >> 4];
                    break;
                case 2:
                    offset = 1;
                    if ( addresses[offset] )
                    {
                        display = 2;
                        goto display_amount_offset;
                    }
                // no break is intentional
                case 3:
                    offset = 1;
                    if ( addresses[offset] )
                    {
                        display = 3;
                        goto display_address_offset;
                    }
                case 4:
                    offset = 2;
                    if ( addresses[offset] )
                    {
                        display = 4;
                        goto display_amount_offset;
                    }
                // no break is intentional
                case 5:
                    offset = 2;
                    if ( addresses[offset] )
                    {
                        display = 5;
                        goto display_address_offset;
                    }
                    // no break is intentional
                case 6:
                    offset = 3;
                    if ( addresses[offset] )
                    {
                        display = 6;
                        goto display_amount_offset;
                    }
                // no break is intentional
                case 7:
                    offset = 3;
                    if ( addresses[offset] )
                    {
                        display = 7;
                        goto display_address_offset;
                    }

                // no break is intentional
                case 8:
                    offset = 4;
                    if ( addresses[offset] )
                    {
                        display = 8;
                        goto display_amount_offset;
                    }
                    // no break is intentional
                case 9:
                    offset = 4;
                    if ( addresses[offset] )
                    {
                        display = 9;
                        goto display_address_offset;
                    }

                // no break is intentional
                case 10:
                    offset = 5;
                    if ( addresses[offset] )
                    {
                        display = 10;
                        goto display_amount_offset;
                    }
                    // no break is intentional
                case 11:
                    offset = 5;
                    if ( addresses[offset] )
                    {
                        display = 11;
                        goto display_address_offset;
                    }
                    // no break is intentional
                case 12:
                    offset = 6;
                    if ( addresses[offset] )
                    {
                        display = 12;
                        goto display_amount_offset;
                    }
                    // no break is intentional
                case 13:
                    offset = 6;
                    if ( addresses[offset] )
                    {
                        display = 13;
                        goto display_address_offset;
                    }

                    // no break is intentional
                case 14:
                    offset = 7;
                    if ( addresses[offset] )
                    {
                        display = 14;
                        goto display_amount_offset;
                    }
                    // no break is intentional
                case 15:
                    offset = 7;
                    if ( addresses[offset] )
                    {
                        display = 15;
                        goto display_address_offset;
                    }
                    // no break is intentional
                case 16:
                    offset = 8;
                    if ( addresses[offset] )
                    {
                        display = 16;
                        goto display_amount_offset;
                    }
                    // no break is intentional
                case 17:
                    offset = 8;
                    if ( addresses[offset] )
                    {
                        display = 17;
                        goto display_address_offset;
                    }
                    // no break is intentional
                case 18:
                    offset = 9;
                    if ( addresses[offset] )
                    {
                        display = 18;
                        goto display_amount_offset;
                    }
                    // no break is intentional
                case 19:
                    offset = 9;
                    if ( addresses[offset] )
                    {
                        display = 19;
                        goto display_address_offset;
                    }
                // no break is intentional
                case 20: // fees
                    display = 20;
                    goto display_detail;
                }

                UX_CALLBACK_SET_INTERVAL(MAX(
                    2000,
                    1000 + bagl_label_roundtrip_duration_ms(&tmp_element, 7)));
                return &tmp_element;
            }
        }
    }
    return display;
}


const bagl_element_t ui_approval_nanos_ec[] = {
    // type                               userid    x    y   w    h  str rad
    // fill      fg        bg      fid iid  txt   touchparams...       ]
    {{BAGL_RECTANGLE, 0x00, 0, 0, 128, 32, 0, 0, BAGL_FILL, 0x000000, 0xFFFFFF,
      0, 0},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    {{BAGL_ICON, 0x00, 3, 12, 7, 7, 0, 0, 0, 0xFFFFFF, 0x000000, 0,
      BAGL_GLYPH_ICON_CROSS},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_ICON, 0x00, 117, 13, 8, 6, 0, 0, 0, 0xFFFFFF, 0x000000, 0,
      BAGL_GLYPH_ICON_CHECK},
     NULL,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},

    //{{BAGL_ICON                           , 0x01,  21,   9,  14,  14, 0, 0, 0
    //, 0xFFFFFF, 0x000000, 0, BAGL_GLYPH_ICON_TRANSACTION_BADGE  }, NULL, 0, 0,
    // 0, NULL, NULL, NULL },
    {{BAGL_LABELINE, 0x01, 0, 12, 128, 32, 0, 0, 0, 0xFFFFFF, 0x000000,
      BAGL_FONT_OPEN_SANS_EXTRABOLD_11px | BAGL_FONT_ALIGNMENT_CENTER, 0},
     "Confirm",
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
    {{BAGL_LABELINE, 0x01, 0, 26, 128, 32, 0, 0, 0, 0xFFFFFF, 0x000000,
      BAGL_FONT_OPEN_SANS_EXTRABOLD_11px | BAGL_FONT_ALIGNMENT_CENTER, 0},
     (char*)addressSummary,
     0,
     0,
     0,
     NULL,
     NULL,
     NULL},
};


unsigned int ui_approval_prepro_ec(const bagl_element_t *element) {
    unsigned int display = 1;
    if (element->component.userid > 0) {
        display = (ux_step == element->component.userid - 1);
        //if we are on the address and amount then repeat for all outputs
        if (display) {
            switch (element->component.userid) {
            case 1:
                UX_CALLBACK_SET_INTERVAL(2000);
                break;
            case 2:
                if (dataPresent) {
                    UX_CALLBACK_SET_INTERVAL(2000);
                } else {
                    display = 0;
                    ux_step++; // display the next step
                }
                break;
            case 3:
                UX_CALLBACK_SET_INTERVAL(MAX(
                    3000, 1000 + bagl_label_roundtrip_duration_ms(element, 7)));
                break;
            case 4:
                UX_CALLBACK_SET_INTERVAL(3000);
                break;
            case 5:
                UX_CALLBACK_SET_INTERVAL(MAX(
                    3000, 1000 + bagl_label_roundtrip_duration_ms(element, 7)));
                break;
            }
        }
    }
    return display;
}


unsigned int ui_approval_nanos_button(unsigned int button_mask,
                                      unsigned int button_mask_counter);



#endif // #if defined(TARGET_NANOS)

void ui_idle(void) {
    skipWarning = false;
#if defined(TARGET_BLUE)
    UX_DISPLAY(ui_idle_blue, NULL);
#elif defined(TARGET_NANOS)
    UX_MENU_DISPLAY(0, menu_main, NULL);
#endif // #if TARGET_ID
}

#if defined(TARGET_BLUE)
unsigned int io_seproxyhal_touch_settings(const bagl_element_t *e) {
    UX_DISPLAY(ui_settings_blue, ui_settings_blue_prepro);
    return 0; // do not redraw button, screen has switched
}
#endif // #if defined(TARGET_BLUE)

unsigned int io_seproxyhal_touch_exit(const bagl_element_t *e) {
    // Go back to the dashboard
    os_sched_exit(0);
    return 0; // do not redraw the widget
}

unsigned int io_seproxyhal_touch_address_ok(const bagl_element_t *e) {
    uint32_t tx = set_result_get_publicKey();
    G_io_apdu_buffer[tx++] = 0x90;
    G_io_apdu_buffer[tx++] = 0x00;
#ifdef HAVE_U2F
    if (fidoActivated) {
        u2f_proxy_response((u2f_service_t *)&u2fService, tx);
    } else {
        // Send back the response, do not restart the event loop
        io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, tx);
    }
#else  // HAVE_U2F
    // Send back the response, do not restart the event loop
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, tx);
#endif // HAVE_U2F
    // Display back the original UX
    ui_idle();
    return 0; // do not redraw the widget
}

unsigned int io_seproxyhal_touch_address_cancel(const bagl_element_t *e) {
    G_io_apdu_buffer[0] = 0x69;
    G_io_apdu_buffer[1] = 0x85;
#ifdef HAVE_U2F
    if (fidoActivated) {
        u2f_proxy_response((u2f_service_t *)&u2fService, 2);
    } else {
        // Send back the response, do not restart the event loop
        io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, 2);
    }
#else  // HAVE_U2F
    // Send back the response, do not restart the event loop
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, 2);
#endif // HAVE_U2F
    // Display back the original UX
    ui_idle();
    return 0; // do not redraw the widget
}

#if defined(TARGET_NANOS)
unsigned int ui_address_nanos_button(unsigned int button_mask,
                                     unsigned int button_mask_counter) {
    switch (button_mask) {
    case BUTTON_EVT_RELEASED | BUTTON_LEFT: // CANCEL
        io_seproxyhal_touch_address_cancel(NULL);
        break;

    case BUTTON_EVT_RELEASED | BUTTON_RIGHT: { // OK
        io_seproxyhal_touch_address_ok(NULL);
        break;
    }
    }
    return 0;
}
#endif // #if defined(TARGET_NANOS)

unsigned int io_seproxyhal_touch_tx_ok(const bagl_element_t *e)
{
    uint8_t privateKeyData[64];
    uint16_t signatureLength = 0;
    cx_ecfp_private_key_t privateKey;

    os_perso_derive_node_bip32(
        CX_CURVE_Ed25519, 
        tmpCtx.transactionContext.bip32Path,
        tmpCtx.transactionContext.pathLength, 
        privateKeyData, NULL);
    cx_ecfp_init_private_key(CX_CURVE_Ed25519, privateKeyData, 32, &privateKey);
    os_memset(privateKeyData, 0, sizeof(privateKeyData));

    //store signature in 35..97
#ifdef LEGACY_SUPPORT
    signatureLength = cx_eddsa_sign(&privateKey, NULL, CX_LAST, CX_SHA512,
                            tmpCtx.transactionContext.rawTx,
                            tmpCtx.transactionContext.rawTxLength,
                            &G_io_apdu_buffer[35]);
#else
    signatureLength = cx_eddsa_sign(&privateKey, CX_LAST, CX_SHA512,
                            tmpCtx.transactionContext.rawTx,
                            tmpCtx.transactionContext.rawTxLength,
			    NULL, 0,
                            &G_io_apdu_buffer[35], NULL);
#endif

    cx_ecfp_generate_pair(CX_CURVE_Ed25519,
                         &tmpCtx.publicKeyContext.publicKey,
                         &privateKey, 1);

    os_memset(&privateKey, 0, sizeof(privateKey));

    //return length of signature in 33..34, should be 64
    G_io_apdu_buffer[33] = (uint8_t)(signatureLength >> 8);
    G_io_apdu_buffer[34] = (uint8_t)(signatureLength);
    signatureLength += 2; 

    //Return RCD in 0..33
    getRCDFromEd25519PublicKey(&tmpCtx.publicKeyContext.publicKey,
                               G_io_apdu_buffer, 33);
    signatureLength+=33;

    //append success 
    G_io_apdu_buffer[signatureLength++] = 0x90;
    G_io_apdu_buffer[signatureLength++] = 0x00;

#ifdef HAVE_U2F
    if (fidoActivated) {
        u2f_proxy_response((u2f_service_t *)&u2fService, signatureLength);
    } else {
        // Send back the response, do not restart the event loop
        io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, signatureLength);
    }
#else  // HAVE_U2F
    // Send back the response, do not restart the event loop
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, signatureLength);
#endif // HAVE_U2F

    // Display back the original UX
    ui_idle();

    return 0; // do not redraw the widget
}

unsigned int io_seproxyhal_touch_ec_tx_ok(const bagl_element_t *e)
{
    uint8_t privateKeyData[64];
    uint16_t signatureLength = 0;
    cx_ecfp_private_key_t privateKey;

    os_perso_derive_node_bip32(
        CX_CURVE_Ed25519,
        tmpCtx.transactionContext.bip32Path,
        tmpCtx.transactionContext.pathLength,
        privateKeyData, NULL);
    cx_ecfp_init_private_key(CX_CURVE_Ed25519, privateKeyData, 32, &privateKey);
    os_memset(privateKeyData, 0, sizeof(privateKeyData));

    //store signature in 34..96
    signatureLength = cx_eddsa_sign(&privateKey, NULL, CX_LAST, CX_SHA512,
                            tmpCtx.transactionContext.rawTx,
                            tmpCtx.transactionContext.rawTxLength - EC_PUBLIC_KEY_LENGTH,
                            &G_io_apdu_buffer[34]);

    cx_ecfp_generate_pair(CX_CURVE_Ed25519,
                         &tmpCtx.publicKeyContext.publicKey,
                         &privateKey, 1);

    os_memset(&privateKey, 0, sizeof(privateKey));

    //return length of signature in 32..32, should be 64
    G_io_apdu_buffer[32] = (uint8_t)(signatureLength >> 8);
    G_io_apdu_buffer[33] = (uint8_t)(signatureLength);
    signatureLength += 2;

    //Return RCD in 0..33
    getECKeyFromEd25519PublicKey(&tmpCtx.publicKeyContext.publicKey,
                               G_io_apdu_buffer, 32);
    signatureLength+=32;

    //append success
    G_io_apdu_buffer[signatureLength++] = 0x90;
    G_io_apdu_buffer[signatureLength++] = 0x00;

#ifdef HAVE_U2F
    if (fidoActivated) {
        u2f_proxy_response((u2f_service_t *)&u2fService, signatureLength);
    } else {
        // Send back the response, do not restart the event loop
        io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, signatureLength);
    }
#else  // HAVE_U2F
    // Send back the response, do not restart the event loop
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, signatureLength);
#endif // HAVE_U2F

    // Display back the original UX
    ui_idle();

    return 0; // do not redraw the widget
}

unsigned int io_seproxyhal_touch_tx_cancel(const bagl_element_t *e)
{
    G_io_apdu_buffer[0] = 0x69;
    G_io_apdu_buffer[1] = 0x85;
#ifdef HAVE_U2F
    if (fidoActivated) {
        u2f_proxy_response((u2f_service_t *)&u2fService, 2);
    } else {
        // Send back the response, do not restart the event loop
        io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, 2);
    }
#else  // HAVE_U2F
    // Send back the response, do not restart the event loop
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, 2);
#endif // HAVE_U2F
    // Display back the original UX
    ui_idle();
    return 0; // do not redraw the widget
}

#if defined(TARGET_BLUE)
void ui_approval_blue_init(void) {
    UX_DISPLAY(ui_approval_blue, ui_approval_blue_prepro);
}

void ui_approval_transaction_blue_init(void) {
    ui_approval_blue_ok = (bagl_element_callback_t)io_seproxyhal_touch_tx_ok;
    ui_approval_blue_cancel =
        (bagl_element_callback_t)io_seproxyhal_touch_tx_cancel;
    G_ui_approval_blue_state = APPROVAL_TRANSACTION;
    ui_approval_blue_values[0] = fullAmount;
    ui_approval_blue_values[1] = fullAddress;
    ui_approval_blue_values[2] = maxFee;
    ui_approval_blue_init();
}


#elif defined(TARGET_NANOS)
unsigned int ui_approval_nanos_button(unsigned int button_mask,
                                      unsigned int button_mask_counter)
{
    switch (button_mask)
    {
        case BUTTON_EVT_RELEASED | BUTTON_LEFT:
            io_seproxyhal_touch_tx_cancel(NULL);
            break;

        case BUTTON_EVT_RELEASED | BUTTON_RIGHT:
        {
            io_seproxyhal_touch_tx_ok(NULL);
            break;
        }
    }

    return 0;
}

unsigned int ui_approval_nanos_ec_button(unsigned int button_mask,
                                       unsigned int button_mask_counter)
{
    switch (button_mask)
    {
        case BUTTON_EVT_RELEASED | BUTTON_LEFT:
            io_seproxyhal_touch_tx_cancel(NULL);
            break;

        case BUTTON_EVT_RELEASED | BUTTON_RIGHT:
        {
            io_seproxyhal_touch_ec_tx_ok(NULL);
            break;
        }
    }

    return 0;
}
#endif // #if defined(TARGET_NANOS)

unsigned short io_exchange_al(unsigned char channel, unsigned short tx_len) {
    switch (channel & ~(IO_FLAGS)) {
    case CHANNEL_KEYBOARD:
        break;

    // multiplexed io exchange over a SPI channel and TLV encapsulated protocol
    case CHANNEL_SPI:
        if (tx_len) {
            io_seproxyhal_spi_send(G_io_apdu_buffer, tx_len);

            if (channel & IO_RESET_AFTER_REPLIED) {
                reset();
            }
            return 0; // nothing received from the master so far (it's a tx
                      // transaction)
        } else {
            return io_seproxyhal_spi_recv(G_io_apdu_buffer,
                                          sizeof(G_io_apdu_buffer), 0);
        }

    default:
        THROW(INVALID_PARAMETER);
    }
    return 0;
}

uint32_t set_result_get_publicKey() {
    uint32_t tx = 0;
    G_io_apdu_buffer[tx++] = tmpCtx.publicKeyContext.publicKey.W_len;
    os_memmove(G_io_apdu_buffer + tx, tmpCtx.publicKeyContext.publicKey.W, tmpCtx.publicKeyContext.publicKey.W_len);
    tx += tmpCtx.publicKeyContext.publicKey.W_len;
    G_io_apdu_buffer[tx++] = 52;
    os_memmove(G_io_apdu_buffer + tx, tmpCtx.publicKeyContext.address, 52);
    tx += 52;
    if (tmpCtx.publicKeyContext.getChaincode) {
        os_memmove(G_io_apdu_buffer + tx, tmpCtx.publicKeyContext.chainCode,
                   32);
        tx += 32;
    }
    return tx;
}

void convertUint256BE(uint8_t *data, uint32_t length, uint256_t *target) {
    uint8_t tmp[32];
    os_memset(tmp, 0, 32);
    os_memmove(tmp + 32 - length, data, length);
    readu256BE(tmp, target);
}

void handleGetPublicKey(uint8_t p1, uint8_t p2, uint8_t *dataBuffer,
                        uint16_t dataLength, 
                        volatile unsigned int *flags,
                        volatile unsigned int *tx) {
    UNUSED(dataLength);
    uint8_t privateKeyData[32];
    cx_ecfp_private_key_t privateKey;
    uint32_t bip32Path[MAX_BIP32_PATH];
    uint8_t bip32PathLength = *(dataBuffer++);

    if ((bip32PathLength < 0x01) || (bip32PathLength > MAX_BIP32_PATH))
    {
        THROW(0x6a80);
    }
    if ((p1 != P1_CONFIRM) && (p1 != P1_NON_CONFIRM))
    {
        THROW(0x6B00);
    }
    if ((p2 != P2_CHAINCODE) && (p2 != P2_NO_CHAINCODE))
    {
        THROW(0x6B00);
    }
    //walk through the path 
    //e.g. FCT -- m/44'/131'/0'/0'/0'
    //e.g. EC  -- m/44'/132'/0'/0'/0'
    uint8_t keytype = PUBLIC_OFFSET_FCT;
    for (int i = 0; i < bip32PathLength;++i) {
        bip32Path[i] = (dataBuffer[0] << 24)  
		     | (dataBuffer[1] << 16)  
		     | (dataBuffer[2] << 8)   
		     | (dataBuffer[3]);       

        dataBuffer += 4;
    }

    if ( (uint8_t)bip32Path[1] == 0x84 )
    {
        keytype = PUBLIC_OFFSET_EC;
    }

    tmpCtx.publicKeyContext.getChaincode = (p2 == P2_CHAINCODE);
    os_perso_derive_node_bip32(CX_CURVE_Ed25519, 
		               bip32Path, bip32PathLength,
                               privateKeyData,
                               (tmpCtx.publicKeyContext.getChaincode
                                    ? tmpCtx.publicKeyContext.chainCode
                                    : NULL));

    cx_ecfp_init_private_key(CX_CURVE_Ed25519, privateKeyData, 32, &privateKey);

    //generate the public / private key pair.  i
    cx_ecfp_generate_pair(CX_CURVE_Ed25519, &tmpCtx.publicKeyContext.publicKey,
                          &privateKey, 1);

    os_memset(&privateKey, 0, sizeof(privateKey));
    os_memset(privateKeyData, 0, sizeof(privateKeyData));

    if ( keytype == PUBLIC_OFFSET_FCT )
    {
        //The public key generated is 64 bytes not 32, so compress it
        //and convert to an rcd
        uint8_t rcd[33];
        getRCDFromEd25519PublicKey(&tmpCtx.publicKeyContext.publicKey,
                                   rcd, sizeof(rcd));

        //make the compressed key the new key.
        os_memset(tmpCtx.publicKeyContext.publicKey.W, 0, 65);
        tmpCtx.publicKeyContext.publicKey.W_len = sizeof(rcd);
        os_memmove(tmpCtx.publicKeyContext.publicKey.W, rcd,sizeof(rcd));
    }
    else
    {
        uint8_t key[32];
        getECKeyFromEd25519PublicKey(&tmpCtx.publicKeyContext.publicKey,
                                   key, sizeof(key));
        //make the compressed key the new key.
        os_memset(tmpCtx.publicKeyContext.publicKey.W, 0, 65);
        tmpCtx.publicKeyContext.publicKey.W_len = sizeof(key);
        os_memmove(tmpCtx.publicKeyContext.publicKey.W, key,sizeof(key));

    }

    //convert the public key to an address
    //publicKey is RCD or EC Key
    getFctAddressStringFromKey(&tmpCtx.publicKeyContext.publicKey,
                                tmpCtx.publicKeyContext.address, 
				keytype);


    //tmpCtx.publicKeyContext.publicKey.W[0] = bip32Path[1];
    //tmpCtx.publicKeyContext.publicKey.W_len = 1;
    if (p1 == P1_NON_CONFIRM) {
        *tx = set_result_get_publicKey();
        THROW(0x9000);
    } else {
        *tx = set_result_get_publicKey();

        // prepare for a UI based reply
        skipWarning = false;
#if defined(TARGET_BLUE)
        snprintf(fullAddress, sizeof(fullAddress), "%.*s", 52,
                 tmpCtx.publicKeyContext.address);
        UX_DISPLAY(ui_address_blue, ui_address_blue_prepro);
#elif defined(TARGET_NANOS)
        snprintf((char*)fullAddress, sizeof(fullAddress), " %.*s ", 52,
                 tmpCtx.publicKeyContext.address);
        ux_step = 0;
        ux_step_count = 2;
        UX_DISPLAY(ui_address_nanos, ui_address_prepro);
#endif // #if TARGET_ID

        *flags |= IO_ASYNCH_REPLY;
    }
}

void handleGetAppConfiguration(uint8_t p1, uint8_t p2, uint8_t *workBuffer,
                               uint16_t dataLength,
                               volatile unsigned int *flags,
                               volatile unsigned int *tx) {
    UNUSED(p1);
    UNUSED(p2);
    UNUSED(workBuffer);
    UNUSED(dataLength);
    UNUSED(flags);
    G_io_apdu_buffer[0] = (N_storage.dataAllowed ? 0x01 : 0x00);
    G_io_apdu_buffer[1] = 0;
    G_io_apdu_buffer[2] = 9;
    G_io_apdu_buffer[3] = 1;
    *tx = 4;
    THROW(0x9000);
}

void handleSign(uint8_t p1, uint8_t p2, uint8_t *workBuffer,
                uint16_t dataLength, volatile unsigned int *flags,
                volatile unsigned int *tx) {
    UNUSED(tx);
    *tx = 0;
    
    uint32_t keytype =  PUBLIC_OFFSET_FCT;

    switch (p1)
    {
    case P1_FIRST: 
        os_memset(&tmpCtx.transactionContext,0,sizeof(tmpCtx.transactionContext));

        tmpCtx.transactionContext.pathLength = workBuffer[0];
        if ((tmpCtx.transactionContext.pathLength < 0x01) ||
            (tmpCtx.transactionContext.pathLength > MAX_BIP32_PATH)) {
            THROW(0x6a80);
        }
        workBuffer++;
        dataLength--;
        
        //now extract the pathing information
        for (int i = 0; i < tmpCtx.transactionContext.pathLength; i++) {
            tmpCtx.transactionContext.bip32Path[i] =
                (workBuffer[0] << 24) |
                (workBuffer[1] << 16) |
                (workBuffer[2] << 8)  |
                (workBuffer[3]);
            workBuffer += 4;
            dataLength -= 4;
        }
        tmpCtx.transactionContext.curve = CX_CURVE_Ed25519;
        tmpCtx.transactionContext.rawTxLength = dataLength;

        if ( (uint8_t)tmpCtx.transactionContext.bip32Path[1] == 0x84 )
        {
            keytype = PUBLIC_OFFSET_EC;
        }

        if ( dataLength > MAX_RAW_TX )
        {
            THROW(BTCHIP_SW_NOT_ENOUGH_MEMORY_SPACE);
        }

        os_memmove(tmpCtx.transactionContext.rawTx, workBuffer, dataLength);
        if ( (uint8_t)tmpCtx.transactionContext.bip32Path[1] != 0x83 )
        {
            THROW(BTCHIP_SW_INCORRECT_DATA);
        }
    break;

    case P1_MORE: 
        if ( tmpCtx.transactionContext.rawTxLength  > sizeof(tmpCtx.transactionContext.rawTx) )
        {
           os_memset(&tmpCtx.transactionContext, 0, sizeof(tmpCtx.transactionContext));
           //Transaction too large for device
           THROW(0x6A80);
        }
        os_memmove(&tmpCtx.transactionContext.rawTx[tmpCtx.transactionContext.rawTxLength],
                   workBuffer, dataLength);
        tmpCtx.transactionContext.rawTxLength += dataLength;
    break;

    default:
        THROW(0x6B00);
    };


    if ( p2 == 0 ) {
        THROW(0x9000);
    }

#if WANT_INVALID_BUFFER_CHECK
    if ( tmpCtx.transactionContext.expectedTxLength < tmpCtx.transactionContext.rawTxLength )
    {
        //too little data
        THROW(0x6B00);
    }
    else if ( tmpCtx.transactionContext.expectedTxLength > 
		    tmpCtx.transactionContext.rawTx )
    {
        //too much data
        THROW(0x6C80);
    }
    else
#endif

    int ret = parseTx(tmpCtx.transactionContext.rawTx,
                       tmpCtx.transactionContext.rawTxLength,
                       &txContent);
    if ( ret != USTREAM_FINISHED)
    {
        THROW(ret);
    }


    if ( txContent.header.inputcount < 1 )
    {
        THROW(USTREAM_FAULT_INPUT_COUNT);
    }


    if ( txContent.header.outputcount + txContent.header.ecpurchasecount > MAX_OUTPUTS )
    {
        THROW(USTREAM_FAULT_TOO_MANY_OUTPUTS);
    }

    // "confirm", amount, address, ..., amount[n], address[n], fee
    os_memset((void*)addressSummary, 0, sizeof(addressSummary));
    os_memset((void*)fullAmount, 0, sizeof(fullAmount));
    for ( int i = 0; i < MAX_OUTPUTS; ++i )
    {
        addresses[i] = NULL;
    }

    ux_step_count = 2;
    ux_step = 0;

    fct_print_amount(txContent.fees, (int8_t*)maxFee, sizeof(maxFee));


    int output_ct = 0;
    for ( int i = 0; i < txContent.header.outputcount;++i )
    {
        addresses_type[output_ct] = PUBLIC_OFFSET_FCT;
        addresses[output_ct++] = &txContent.outputs[i];
        ux_step_count += 2;
    }

    for ( int i = 0; i < txContent.header.ecpurchasecount;++i )
    {

        addresses_type[output_ct] = PUBLIC_OFFSET_EC;
        addresses[output_ct++] = &txContent.ecpurchase[i];
        ux_step_count += 2;
    }

#if defined(TARGET_BLUE)
    ux_step_count = 0;
    ui_approval_transaction_blue_init();
#elif defined(TARGET_NANOS)

    UX_DISPLAY(ui_approval_nanos, ui_approval_prepro);

#endif // #if TARGET

    *flags |= IO_ASYNCH_REPLY;
    *tx = 0;

}


void handleCommitSign(uint8_t p1, uint8_t p2, uint8_t *workBuffer,
                  uint16_t dataLength,
                  volatile unsigned int *flags,
                  volatile unsigned int *tx)
{
    UNUSED(tx);
    *tx = 0;

    switch (p1)
    {
    case P1_FIRST:
        os_memset(&tmpCtx.transactionContext,0,sizeof(tmpCtx.transactionContext));

        tmpCtx.transactionContext.pathLength = workBuffer[0];
        if ((tmpCtx.transactionContext.pathLength < 0x01) ||
            (tmpCtx.transactionContext.pathLength > MAX_BIP32_PATH))
        {
            THROW(0x6a80);
        }
        workBuffer++;
        dataLength--;

        //now extract the pathing information
        for (int i = 0; i < tmpCtx.transactionContext.pathLength; i++)
        {
            tmpCtx.transactionContext.bip32Path[i] =
                (workBuffer[0] << 24) |
                (workBuffer[1] << 16) |
                (workBuffer[2] << 8)  |
                (workBuffer[3]);
            workBuffer += 4;
            dataLength -= 4;
        }
        tmpCtx.transactionContext.curve = CX_CURVE_Ed25519;
        tmpCtx.transactionContext.rawTxLength = dataLength;

        if ( dataLength > MAX_RAW_TX )
        {
            THROW(BTCHIP_SW_NOT_ENOUGH_MEMORY_SPACE);
        }

        os_memmove(tmpCtx.transactionContext.rawTx, workBuffer, dataLength);
        if ( (uint8_t)tmpCtx.transactionContext.bip32Path[1] != 0x84 )
        {
            THROW(BTCHIP_SW_INCORRECT_DATA);
        }
        break;
    case P1_MORE:
        if ( tmpCtx.transactionContext.rawTxLength  > sizeof(tmpCtx.transactionContext.rawTx) )
        {
            os_memset(&tmpCtx.transactionContext, 0, sizeof(tmpCtx.transactionContext));
            //Transaction too large for device
            THROW(0x6A80);
        }
        os_memmove(&tmpCtx.transactionContext.rawTx[tmpCtx.transactionContext.rawTxLength],
                   workBuffer, dataLength);
        tmpCtx.transactionContext.rawTxLength += dataLength;
        break;
    default:
        THROW(0x6B00);
    };

    if ( (p2&0x02) == 0 ) 
    {
        THROW(0x9000);
    }



    cx_ecfp_public_key_t pubkey;
    pubkey.W_len = 32;

    if ( p2&0x01 )
    {
        //this means we have a chain commit
        txCcContent_t txCcContent;
        int ret = parseCcTx(tmpCtx.transactionContext.rawTx,
                          tmpCtx.transactionContext.rawTxLength,
                          &txCcContent);
        if ( ret != USTREAM_FINISHED)
        {
            THROW(ret);
        }

	snprintf(addressSummary,sizeof(addressSummary),"Sign Chain?");
        //os_memmove(pubkey.W,txCcContent.ecpubkey,pubkey.W_len);

    }
    else //otherwise entry commit
    {
        txEcContent_t txEcContent;
        int ret = parseEcTx(tmpCtx.transactionContext.rawTx,
                          tmpCtx.transactionContext.rawTxLength,
                          &txEcContent);
        if ( ret != USTREAM_FINISHED)
        {
            THROW(ret);
        }

        //os_memmove(pubkey.W,txEcContent.ecpubkey,pubkey.W_len);
	snprintf(addressSummary,sizeof(addressSummary),"Sign Entry?");

    }



#if WANT_REDUNDANT_DISPALY_OF_EC_ADDRESS
    char addr[64];
    getFctAddressStringFromKey(&pubkey,addr,PUBLIC_OFFSET_EC);

    os_memset((void *) addressSummary, 0, sizeof(addressSummary));
    os_memmove((void *) addressSummary, addr, 10);
    os_memmove((void *)(addressSummary + 10), "..", 2);
    os_memmove((void *)(addressSummary + 12),
                        addr + addressLength - 4, 4);
    ux_step_count = 2; 
#else
    ux_step_count = 0;
#endif
    ux_step = 0;


#if defined(TARGET_BLUE)
    ux_step_count = 0;
    ui_approval_transaction_blue_init();
#elif defined(TARGET_NANOS)

    UX_DISPLAY(ui_approval_nanos_ec, ui_approval_prepro_ec);

#endif // #if TARGET

    *flags |= IO_ASYNCH_REPLY;
    *tx = 0;

}


void handleApdu(volatile unsigned int *flags, volatile unsigned int *tx) {
    unsigned short sw = 0;

    BEGIN_TRY {
        TRY {
            if (G_io_apdu_buffer[OFFSET_CLA] != CLA) {
                THROW(0x6E00);
            }

            switch (G_io_apdu_buffer[OFFSET_INS]) {
            case INS_GET_PUBLIC_KEY:
                handleGetPublicKey(G_io_apdu_buffer[OFFSET_P1],
                                   G_io_apdu_buffer[OFFSET_P2],
                                   G_io_apdu_buffer + OFFSET_CDATA,
                                   G_io_apdu_buffer[OFFSET_LC], flags, tx);
                break;

            case INS_SIGN:
                handleSign(G_io_apdu_buffer[OFFSET_P1],
                           G_io_apdu_buffer[OFFSET_P2],
                           G_io_apdu_buffer + OFFSET_CDATA,
                           G_io_apdu_buffer[OFFSET_LC], flags, tx);
                break;
            case INS_COMMIT_SIGN:
                handleCommitSign(G_io_apdu_buffer[OFFSET_P1],
                           G_io_apdu_buffer[OFFSET_P2],
                           G_io_apdu_buffer + OFFSET_CDATA,
                           G_io_apdu_buffer[OFFSET_LC], flags, tx);
                break;

            case INS_GET_APP_CONFIGURATION:
                handleGetAppConfiguration(
                    G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2],
                    G_io_apdu_buffer + OFFSET_CDATA,
                    G_io_apdu_buffer[OFFSET_LC], flags, tx);
                break;

            default:
                THROW(0x6D00);
                break;
            }
        }
        CATCH_OTHER(e) {
            switch (e & 0xF000) {
            case 0x6000:
                // Wipe the transaction context and report the exception
                sw = e;
                os_memset(&txContent, 0, sizeof(txContent));
                break;
            case 0x9000:
                // All is well
                sw = e;
                break;
            default:
                // Internal error
                sw = 0x6800 | (e & 0x7FF);
                break;
            }
            // Unexpected exception => report
            G_io_apdu_buffer[*tx] = sw >> 8;
            G_io_apdu_buffer[*tx + 1] = sw;
            *tx += 2;
        }
        FINALLY {
        }
    }
    END_TRY;
}

void sample_main(void) {
    volatile unsigned int rx = 0;
    volatile unsigned int tx = 0;
    volatile unsigned int flags = 0;

    // DESIGN NOTE: the bootloader ignores the way APDU are fetched. The only
    // goal is to retrieve APDU.
    // When APDU are to be fetched from multiple IOs, like NFC+USB+BLE, make
    // sure the io_event is called with a
    // switch event, before the apdu is replied to the bootloader. This avoid
    // APDU injection faults.
    for (;;) {
        volatile unsigned short sw = 0;

        BEGIN_TRY {
            TRY {
                rx = tx;
                tx = 0; // ensure no race in catch_other if io_exchange throws
                        // an error
                rx = io_exchange(CHANNEL_APDU | flags, rx);
                flags = 0;

                // no apdu received, well, reset the session, and reset the
                // bootloader configuration
                if (rx == 0) {
                    THROW(0x6982);
                }

                handleApdu(&flags, &tx);
            }
            CATCH_OTHER(e) {
                switch (e & 0xF000) {
                case 0x6000:
                    // Wipe the transaction context and report the exception
                    sw = e;
                    os_memset(&txContent, 0, sizeof(txContent));
                    break;
                case 0x9000:
                    // All is well
                    sw = e;
                    break;
                default:
                    // Internal error
                    sw = 0x6800 | (e & 0x7FF);
                    break;
                }
                // Unexpected exception => report
                G_io_apdu_buffer[tx] = sw >> 8;
                G_io_apdu_buffer[tx + 1] = sw;
                tx += 2;
            }
            FINALLY {
            }
        }
        END_TRY;
    }

    // return_to_dashboard:
    return;
}

// override point, but nothing more to do
void io_seproxyhal_display(const bagl_element_t *element) {
    io_seproxyhal_display_default((bagl_element_t *)element);
}

unsigned char io_event(unsigned char channel) {
    // nothing done with the event, throw an error on the transport layer if
    // needed

    // can't have more than one tag in the reply, not supported yet.
    switch (G_io_seproxyhal_spi_buffer[0]) {
    case SEPROXYHAL_TAG_FINGER_EVENT:
        UX_FINGER_EVENT(G_io_seproxyhal_spi_buffer);
        break;

    case SEPROXYHAL_TAG_BUTTON_PUSH_EVENT:
        UX_BUTTON_PUSH_EVENT(G_io_seproxyhal_spi_buffer);
        break;

    case SEPROXYHAL_TAG_STATUS_EVENT:
        if (G_io_apdu_media == IO_APDU_MEDIA_USB_HID &&
            !(U4BE(G_io_seproxyhal_spi_buffer, 3) &
              SEPROXYHAL_TAG_STATUS_EVENT_FLAG_USB_POWERED)) {
            THROW(EXCEPTION_IO_RESET);
        }
    // no break is intentional
    default:
        UX_DEFAULT_EVENT();
        break;

    case SEPROXYHAL_TAG_DISPLAY_PROCESSED_EVENT:
        UX_DISPLAYED_EVENT({});
        break;

    case SEPROXYHAL_TAG_TICKER_EVENT:
        UX_TICKER_EVENT(G_io_seproxyhal_spi_buffer, {
            if (UX_ALLOWED) {
                if (skipWarning && (ux_step == 0)) {
                    ux_step++;
                }

                if (ux_step_count) {
                    // prepare next screen
                    ux_step = (ux_step + 1) % ux_step_count;
                    // redisplay screen
                    UX_REDISPLAY();
                }
            }
        });
        break;
    }

    // close the event if not done previously (by a display or whatever)
    if (!io_seproxyhal_spi_is_status_sent()) {
        io_seproxyhal_general_status();
    }

    // command has been processed, DO NOT reset the current APDU transport
    return 1;
}

void app_exit(void) {
    BEGIN_TRY_L(exit) {
        TRY_L(exit) {
            os_sched_exit(-1);
        }
        FINALLY_L(exit) {
        }
    }
    END_TRY_L(exit);
}

__attribute__((section(".boot"))) int main(void) {
    // exit critical section
    __asm volatile("cpsie i");

    // ensure exception will work as planned
    os_boot();

    for (;;) {
        os_memset(&txContent, 0, sizeof(txContent));

        UX_INIT();
        BEGIN_TRY {
            TRY {
                io_seproxyhal_init();

                if (N_storage.initialized != 0x01) {
                    internalStorage_t storage;
                    storage.fidoTransport = 0x00;
                    storage.initialized = 0x01;
                    nvm_write(&N_storage, (void *)&storage,
                              sizeof(internalStorage_t));
                }

#ifdef HAVE_U2F
                os_memset((unsigned char *)&u2fService, 0, sizeof(u2fService));
                u2fService.inputBuffer = G_io_apdu_buffer;
                u2fService.outputBuffer = G_io_apdu_buffer;
                u2fService.messageBuffer = (uint8_t *)u2fMessageBuffer;
                u2fService.messageBufferSize = U2F_MAX_MESSAGE_SIZE;
                u2f_initialize_service((u2f_service_t *)&u2fService);

                USB_power_U2F(1, N_storage.fidoTransport);
#else  // HAVE_U2F
                USB_power_U2F(1, 0);
#endif // HAVE_U2F

                ui_idle();

#if defined(TARGET_BLUE)
                // setup the status bar colors (remembered after wards, even
                // more if another app does not resetup after app switch)
                UX_SET_STATUS_BAR_COLOR(0xFFFFFF, COLOR_APP);
#endif // #if defined(TARGET_BLUE)

                sample_main();
            }
            CATCH(EXCEPTION_IO_RESET) {
                // reset IO and UX before continuing
                continue;
            }
            CATCH_ALL {
                break;
            }
            FINALLY {
            }
        }
        END_TRY;
    }
    app_exit();

    return 0;
}
