#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/udcd.h>

#include "uapi/hidkeyboard_uapi.h"
#include "usb_descriptors.h"
#include "ascii_to_usb_hid.h"
#include "layouts/layouts.h"

#define VITA_USB_KEYBOARD       "VITA_KEYBOARD"
#define VITA_USB_KEYBOARD_PID   0x1338

static char g_inputs[8] __attribute__((aligned(64))) = { 0 };
static struct SceUdcdDeviceRequest g_request;
static struct SceUdcdDeviceRequest g_reportrequest;
static SceUID g_thid = -1;
static int g_run = 1;

static int hidkeyboard_driver_registered = 0;
static int hidkeyboard_driver_activated  = 0;

static int  hasPendingKey = 0;
static char pendingKey    = 0x00;
static char modifier      = 0x00;
int mtxLock = -1;

/* Forward declarations */
static int  start_func(int size, void* args, void* user_data);
static int  stop_func(int size, void* args, void* user_data);
static int  usb_recvctl(int arg1, int arg2, struct SceUdcdEP0DeviceRequest* req, void* user_data);
static int  usb_change(int interfaceNumber, int alternateSetting, int bus);
static int  usb_attach(int usb_version, void* user_data);
static void usb_detach(void* user_data);
static void usb_configure(int usb_version, int desc_count, struct SceUdcdInterfaceSettings* settings, void* user_data);

struct SceUdcdDriver g_driver =
{
    VITA_USB_KEYBOARD, 2, &endpoints[0], &interfaces[0],
    &devdesc_hi, &config_hi, &devdesc_full, &config_full,
    &descriptors[0], NULL, NULL,
    &usb_recvctl, &usb_change, &usb_attach, &usb_detach, &usb_configure,
    &start_func, &stop_func, 0, 0, NULL
};

static void send_inputs(void);

static void complete_request(struct SceUdcdDeviceRequest* req)
{
    req->unused = NULL;
}

static int usb_recvctl(int arg1, int arg2, struct SceUdcdEP0DeviceRequest* req, void* user_data)
{
    /* Host requesting HID report descriptor */
    if (req->bmRequestType == 0x81 && req->bRequest == 0x06 && req->wValue == 0x2200 && arg2 != -1) {
        if (!g_reportrequest.unused) {
            g_reportrequest.data = hid_report;
            g_reportrequest.size = sizeof(hid_report);
            g_reportrequest.endpoint = &endpoints[0];
            if (g_reportrequest.size > req->wLength)
                g_reportrequest.size = req->wLength;
            g_reportrequest.isControlRequest = 0;
            g_reportrequest.onComplete = &complete_request;
            g_reportrequest.transmitted = 0;
            g_reportrequest.returnCode = 0;
            g_reportrequest.unused = &g_reportrequest;
            g_reportrequest.next = NULL;
            g_reportrequest.physicalAddress = NULL;
            ksceUdcdReqSend(&g_reportrequest);
        }
    }
    return 0;
}

static int  usb_change(int interfaceNumber, int alternateSetting, int bus) { return 0; }
static int  usb_attach(int usb_version, void* user_data) { return 0; }
static void usb_detach(void* user_data) {}
static void usb_configure(int usb_version, int desc_count, struct SceUdcdInterfaceSettings* settings, void* user_data) {}
static int  start_func(int size, void* p, void* user_data) { return 0; }
static int  stop_func(int size, void* p, void* user_data)  { return 0; }

static void send_inputs(void)
{
    if (g_request.unused)
        return;

    g_request.endpoint         = &endpoints[1];
    g_request.data             = g_inputs;
    g_request.size             = sizeof(g_inputs);
    g_request.isControlRequest = 0;
    g_request.onComplete       = &complete_request;
    g_request.transmitted      = 0;
    g_request.returnCode       = 0;
    g_request.unused           = &g_request;
    g_request.next             = NULL;
    g_request.physicalAddress  = NULL;

    /* Critical: flush the cache so DMA sees current g_inputs contents.
     * Without this, the USB controller reads stale data and the host
     * never sees key-up events (or sees repeated key-down). */
    ksceKernelDcacheCleanRange(g_inputs, sizeof(g_inputs));
    ksceUdcdReqSend(&g_request);
}

static int update_keyboard(SceSize args, void* argp)
{
    int pressed = 0;
    int changed = 0;

    while (g_run) {
        /* Don't touch state until the host has actually established the connection.
         * If we send keys before this, they're consumed into the void. */
        unsigned int state = ksceUdcdGetDeviceState();
        if (!(state & SCE_UDCD_STATUS_CONNECTION_ESTABLISHED)) {
            ksceKernelDelayThread(10000);
            continue;
        }

        ksceKernelLockMutex(mtxLock, 1, 0);
        if (pressed) {
            /* Release */
            g_inputs[0] = 0x00;
            g_inputs[2] = 0x00;
            pressed = 0;
            changed = 1;
        }
        else if (hasPendingKey) {
            /* Press */
            g_inputs[0] = modifier;
            g_inputs[2] = pendingKey;
            hasPendingKey = 0;
            pressed = 1;
            changed = 1;
        }
        ksceKernelUnlockMutex(mtxLock, 1);

        if (changed) {
            send_inputs();
            changed = 0;
        }

        ksceKernelDelayThread(10000);
    }
    return 0;
}

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp)
{
    int ret;

    g_thid = ksceKernelCreateThread("update_thread", &update_keyboard,
                                    0x3C, 0x1000, 0, 0x10000, 0);
    if (g_thid < 0)
        goto err_return;

    ret = ksceUdcdRegister(&g_driver);
    if (ret < 0)
        goto err_destroy_thread;

    ret = ksceKernelStartThread(g_thid, 0, 0);
    if (ret < 0)
        goto err_unregister;

    hidkeyboard_driver_activated  = 0;
    hidkeyboard_driver_registered = 1;
    return SCE_KERNEL_START_SUCCESS;

err_unregister:
    ksceUdcdUnregister(&g_driver);
err_destroy_thread:
    ksceKernelDeleteThread(g_thid);
err_return:
    return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize args, void *argp)
{
    if (g_thid > 0) {
        SceUInt timeout = 0xFFFFFFFF;
        g_run = 0;
        ksceKernelWaitThreadEnd(g_thid, NULL, &timeout);
        ksceKernelDeleteThread(g_thid);
    }
    ksceUdcdDeactivate();
    ksceUdcdStop(VITA_USB_KEYBOARD, 0, NULL);
    ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
    ksceUdcdUnregister(&g_driver);
    return SCE_KERNEL_STOP_SUCCESS;
}

int hidkeyboard_user_start(void)
{
    int state = 0;
    int ret;

    ENTER_SYSCALL(state);

    if (!hidkeyboard_driver_registered) {
        EXIT_SYSCALL(state);
        return HIDKEYBOARD_ERROR_DRIVER_NOT_REGISTERED;
    }
    if (hidkeyboard_driver_activated) {
        EXIT_SYSCALL(state);
        return HIDKEYBOARD_ERROR_DRIVER_ALREADY_ACTIVATED;
    }

    ret = ksceUdcdDeactivate();
    if (ret < 0 && ret != SCE_UDCD_ERROR_INVALID_ARGUMENT) {
        EXIT_SYSCALL(state);
        return ret;
    }

    ksceUdcdStop("USB_MTP_Driver", 0, NULL);
    ksceUdcdStop("USBPSPCommunicationDriver", 0, NULL);
    ksceUdcdStop("USBSerDriver", 0, NULL);
    ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);

    ret = ksceUdcdStart("USBDeviceControllerDriver", 0, 0);
    if (ret < 0) {
        EXIT_SYSCALL(state);
        return ret;
    }

    ret = ksceUdcdStart(VITA_USB_KEYBOARD, 0, 0);
    if (ret < 0) {
        ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
        EXIT_SYSCALL(state);
        return ret;
    }

    ret = ksceUdcdActivate(VITA_USB_KEYBOARD_PID);
    if (ret < 0) {
        ksceUdcdStop(VITA_USB_KEYBOARD, 0, NULL);
        ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
        EXIT_SYSCALL(state);
        return ret;
    }

    mtxLock = ksceKernelCreateMutex("HidKeyboardMutex", 0, 0, 0);
    hidkeyboard_driver_activated = 1;

    EXIT_SYSCALL(state);
    return 0;
}

int hidkeyboard_user_stop(void)
{
    int state = 0;
    ENTER_SYSCALL(state);

    if (!hidkeyboard_driver_activated) {
        EXIT_SYSCALL(state);
        return HIDKEYBOARD_ERROR_DRIVER_NOT_ACTIVATED;
    }

    ksceUdcdDeactivate();
    ksceUdcdStop(VITA_USB_KEYBOARD, 0, NULL);
    ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
    ksceUdcdStart("USBDeviceControllerDriver", 0, NULL);
    ksceUdcdStart("USB_MTP_Driver", 0, NULL);
    ksceUdcdActivate(0x4E4);

    ksceKernelDeleteMutex(mtxLock);
    mtxLock = -1;
    hidkeyboard_driver_activated = 0;

    EXIT_SYSCALL(state);
    return 0;
}

int HidKeyBoardSendModifierAndKey(char mod, char key)
{
    int state = 0;
    ENTER_SYSCALL(state);

    ksceKernelLockMutex(mtxLock, 1, 0);
    hasPendingKey = 1;
    modifier      = mod;
    pendingKey    = key;
    ksceKernelUnlockMutex(mtxLock, 1);

    EXIT_SYSCALL(state);
    return 0;
}

int HidKeyboardSendChar(unsigned short int c)
{
    int state = 0;
    ENTER_SYSCALL(state);

    utf16_to_hid_mapping map = getLayoutMappingFromUtf16(c, pt_BR_layout,
        sizeof(pt_BR_layout) / sizeof(utf16_to_hid_mapping));

    if (map.utf16_char == VITAKEYBOARD_ERR_MAPPING_NOT_FOUND) {
        EXIT_SYSCALL(state);
        return 0;
    }

    ksceKernelLockMutex(mtxLock, 1, 0);
    hasPendingKey = 1;
    pendingKey    = map.hid_key1;
    modifier      = map.hid_modifiers1;
    ksceKernelUnlockMutex(mtxLock, 1);

    EXIT_SYSCALL(state);
    return 0;
}
