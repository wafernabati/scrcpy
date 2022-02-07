#include "usb.h"

#include <assert.h>

#include "util/log.h"

static inline void
log_libusb_error(enum libusb_error errcode) {
    LOGW("libusb error: %s", libusb_strerror(errcode));
}

static char *
read_string(libusb_device_handle *handle, uint8_t desc_index) {
    char buffer[128];
    int result =
        libusb_get_string_descriptor_ascii(handle, desc_index,
                                           (unsigned char *) buffer,
                                           sizeof(buffer));
    if (result < 0) {
        return NULL;
    }

    assert((size_t) result <= sizeof(buffer));

    // When non-negative, 'result' contains the number of bytes written
    char *s = malloc(result + 1);
    memcpy(s, buffer, result);
    s[result] = '\0';
    return s;
}

static bool
sc_usb_read_device(libusb_device *device, struct sc_usb_device *out) {
    // Do not log any USB error in this function, it is expected that many USB
    // devices available on the computer have permission restrictions

    struct libusb_device_descriptor desc;
    int result = libusb_get_device_descriptor(device, &desc);
    if (result < 0 || !desc.iSerialNumber) {
        return false;
    }

    libusb_device_handle *handle;
    result = libusb_open(device, &handle);
    if (result < 0) {
        return false;
    }

    char *device_serial = read_string(handle, desc.iSerialNumber);
    if (!device_serial) {
        libusb_close(handle);
        return false;
    }

    out->device = libusb_ref_device(device);
    out->serial = device_serial;
    out->vid = desc.idVendor;
    out->pid = desc.idProduct;
    out->manufacturer = read_string(handle, desc.iManufacturer);
    out->product = read_string(handle, desc.iProduct);
    out->selected = false;

    libusb_close(handle);

    return true;
}

void
sc_usb_device_destroy(struct sc_usb_device *usb_device) {
    if (usb_device->device) {
        libusb_unref_device(usb_device->device);
    }
    free(usb_device->serial);
    free(usb_device->manufacturer);
    free(usb_device->product);
}

void
sc_usb_device_move(struct sc_usb_device *dst, struct sc_usb_device *src) {
    *dst = *src;
    src->device = NULL;
    src->serial = NULL;
    src->manufacturer = NULL;
    src->product = NULL;
}

void
sc_usb_devices_destroy_all(struct sc_usb_device *usb_devices, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        sc_usb_device_destroy(&usb_devices[i]);
    }
}

static ssize_t
sc_usb_list_devices(struct sc_usb *usb, struct sc_usb_device *devices,
                    size_t len) {
    libusb_device **list;
    ssize_t count = libusb_get_device_list(usb->context, &list);
    if (count < 0) {
        log_libusb_error((enum libusb_error) count);
        return -1;
    }

    size_t idx = 0;
    for (size_t i = 0; i < (size_t) count && idx < len; ++i) {
        libusb_device *device = list[i];

        if (sc_usb_read_device(device, &devices[idx])) {
            ++idx;
        }
    }

    libusb_free_device_list(list, 1);
    return idx;
}

static bool
sc_usb_accept_device(const struct sc_usb_device *device, const char *serial) {
    if (!serial) {
        return true;
    }

    return !strcmp(serial, device->serial);
}

static size_t
sc_usb_devices_select(struct sc_usb_device *devices, size_t len,
                      const char *serial, size_t *idx_out) {
    size_t count = 0;
    for (size_t i = 0; i < len; ++i) {
        struct sc_usb_device *device = &devices[i];
        device->selected = sc_usb_accept_device(device, serial);
        if (device->selected) {
            if (idx_out && !count) {
                *idx_out = i;
            }
            ++count;
        }
    }

    return count;
}

static void
sc_usb_devices_log(enum sc_log_level level, struct sc_usb_device *devices,
                   size_t count) {
    for (size_t i = 0; i < count; ++i) {
        struct sc_usb_device *d = &devices[i];
        const char *selection = d->selected ? "-->" : "   ";
        LOG(level, "    %s %-18s (%04" PRIx16 ":%04" PRIx16 ")  %s %s",
            selection, d->serial, d->vid, d->pid, d->manufacturer, d->product);
    }
}

bool
sc_usb_select_device(struct sc_usb *usb, const char *serial,
                     struct sc_usb_device *out_device) {
    struct sc_usb_device usb_devices[16];
    ssize_t count =
        sc_usb_list_devices(usb, usb_devices, ARRAY_LEN(usb_devices));
    if (count == -1) {
        LOGE("Could not list USB devices");
        return false;
    }

    if (count == 0) {
        LOGE("Could not find any USB device");
        return false;
    }

    size_t sel_idx; // index of the single matching device if sel_count == 1
    size_t sel_count =
        sc_usb_devices_select(usb_devices, count, serial, &sel_idx);

    if (sel_count == 0) {
        // if count > 0 && sel_count == 0, then necessarily a serial is provided
        assert(serial);
        LOGE("Could not find USB device %s", serial);
        sc_usb_devices_log(SC_LOG_LEVEL_ERROR, usb_devices, count);
        sc_usb_devices_destroy_all(usb_devices, count);
        return false;
    }

    if (sel_count > 1) {
        if (serial) {
            LOGE("Multiple (%" SC_PRIsizet ") USB devices with serial %s:",
                 sel_count, serial);
        } else {
            LOGE("Multiple (%" SC_PRIsizet ") USB devices:", sel_count);
        }
        sc_usb_devices_log(SC_LOG_LEVEL_ERROR, usb_devices, count);
        if (!serial) {
            LOGE("Specify the device via -s or --serial");
        }
        sc_usb_devices_destroy_all(usb_devices, count);
        return false;
    }

    assert(sel_count == 1); // sel_idx is valid only if sel_count == 1
    struct sc_usb_device *device = &usb_devices[sel_idx];

    LOGD("USB device found:");
    sc_usb_devices_log(SC_LOG_LEVEL_DEBUG, usb_devices, count);

    // Move device into out_device (do not destroy device)
    sc_usb_device_move(out_device, device);
    sc_usb_devices_destroy_all(usb_devices, count);
    return true;
}

static libusb_device_handle *
sc_usb_open_handle(libusb_device *device) {
    libusb_device_handle *handle;
    int result = libusb_open(device, &handle);
    if (result < 0) {
        log_libusb_error((enum libusb_error) result);
        return NULL;
    }
    return handle;
 }

bool
sc_usb_init(struct sc_usb *usb) {
    usb->handle = NULL;
    return libusb_init(&usb->context) == LIBUSB_SUCCESS;
}

void
sc_usb_destroy(struct sc_usb *usb) {
    libusb_exit(usb->context);
}

static int
sc_usb_libusb_callback(libusb_context *ctx, libusb_device *device,
                       libusb_hotplug_event event, void *userdata) {
    (void) ctx;
    (void) device;
    (void) event;

    struct sc_usb *usb = userdata;

    libusb_device *dev = libusb_get_device(usb->handle);
    assert(dev);
    if (dev != device) {
        // Not the connected device
        return 0;
    }

    assert(usb->cbs && usb->cbs->on_disconnected);
    usb->cbs->on_disconnected(usb, usb->cbs_userdata);

    // Do not automatically deregister the callback by returning 1. Instead,
    // manually deregister to interrupt libusb_handle_events() from the libusb
    // event thread: <https://stackoverflow.com/a/60119225/1987178>
    return 0;
}

static int
run_libusb_event_handler(void *data) {
    struct sc_usb *usb = data;
    while (!atomic_load(&usb->stopped)) {
        // Interrupted by events or by libusb_hotplug_deregister_callback()
        libusb_handle_events(usb->context);
    }
    return 0;
}

static bool
sc_usb_register_callback(struct sc_usb *usb) {
    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        LOGW("libusb does not have hotplug capability");
        return false;
    }

    libusb_device *device = libusb_get_device(usb->handle);
    assert(device);

    struct libusb_device_descriptor desc;
    int result = libusb_get_device_descriptor(device, &desc);
    if (result < 0) {
        log_libusb_error((enum libusb_error) result);
        LOGW("Could not read USB device descriptor");
        return false;
    }

    int events = LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT;
    int flags = LIBUSB_HOTPLUG_NO_FLAGS;
    int vendor_id = desc.idVendor;
    int product_id = desc.idProduct;
    int dev_class = LIBUSB_HOTPLUG_MATCH_ANY;
    result = libusb_hotplug_register_callback(usb->context, events, flags,
                                              vendor_id, product_id, dev_class,
                                              sc_usb_libusb_callback, usb,
                                              &usb->callback_handle);
    if (result < 0) {
        log_libusb_error((enum libusb_error) result);
        LOGW("Could not register USB callback");
        return false;
    }

    usb->has_callback_handle = true;
    return true;
}

bool
sc_usb_connect(struct sc_usb *usb, libusb_device *device,
              const struct sc_usb_callbacks *cbs, void *cbs_userdata) {
    usb->handle = sc_usb_open_handle(device);
    if (!usb->handle) {
        return false;
    }

    usb->has_callback_handle = false;
    usb->has_libusb_event_thread = false;

    // If cbs is set, then cbs->on_disconnected must be set
    assert(!cbs || cbs->on_disconnected);
    usb->cbs = cbs;
    usb->cbs_userdata = cbs_userdata;

    if (cbs) {
        atomic_init(&usb->stopped, false);
        if (sc_usb_register_callback(usb)) {
            // Create a thread to process libusb events, so that device
            // disconnection could be detected immediately
            usb->has_libusb_event_thread =
                sc_thread_create(&usb->libusb_event_thread,
                                 run_libusb_event_handler, "scrcpy-usbev", usb);
            if (!usb->has_libusb_event_thread) {
                LOGW("Libusb event thread handler could not be created, USB "
                     "device disconnection might not be detected immediately");
            }
        } else {
            LOGW("Could not register USB device disconnection callback");
        }
    }

    return true;
}

void
sc_usb_disconnect(struct sc_usb *usb) {
    libusb_close(usb->handle);
}

void
sc_usb_stop(struct sc_usb *usb) {
    if (usb->has_callback_handle) {
        atomic_store(&usb->stopped, true);
        libusb_hotplug_deregister_callback(usb->context, usb->callback_handle);
    }
}

void
sc_usb_join(struct sc_usb *usb) {
    if (usb->has_libusb_event_thread) {
        sc_thread_join(&usb->libusb_event_thread, NULL);
    }
}
