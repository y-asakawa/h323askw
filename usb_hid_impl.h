/*
 * usb_hid_impl.h
 *
 * Pure C interface for USB HID controller implementation
 * This isolates IOKit from PTLib to avoid ULONG type conflict
 *
 * Copyright (c) 2024 H323ASKW Project
 */

#ifndef USB_HID_IMPL_H
#define USB_HID_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the implementation
typedef struct USBHIDImpl* USBHIDImplRef;

// Callback type for mute button press
typedef void (*USBHIDMuteCallback)(void* context, int isMuted);

// Callback type for device connect/disconnect
typedef void (*USBHIDDeviceCallback)(void* context, const char* deviceName, int connected);

// Create a new USB HID controller implementation
USBHIDImplRef USBHIDImpl_Create(void);

// Destroy the USB HID controller implementation
void USBHIDImpl_Destroy(USBHIDImplRef impl);

// Start monitoring HID devices
// Returns 1 on success, 0 on failure
int USBHIDImpl_Start(USBHIDImplRef impl);

// Stop monitoring HID devices
void USBHIDImpl_Stop(USBHIDImplRef impl);

// Set the mute callback
void USBHIDImpl_SetMuteCallback(USBHIDImplRef impl, USBHIDMuteCallback callback, void* context);

// Set the device callback
void USBHIDImpl_SetDeviceCallback(USBHIDImplRef impl, USBHIDDeviceCallback callback, void* context);

// Set the mute LED state on the device
void USBHIDImpl_SetMuteLED(USBHIDImplRef impl, int isMuted);

// Check if a HID device is connected
int USBHIDImpl_HasDevice(USBHIDImplRef impl);

// Get the device name (caller must not free the returned string)
const char* USBHIDImpl_GetDeviceName(USBHIDImplRef impl);

#ifdef __cplusplus
}
#endif

#endif // USB_HID_IMPL_H
