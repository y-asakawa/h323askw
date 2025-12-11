/*
 * usb_hid_impl.mm
 *
 * USB HID implementation using IOKit (pure Objective-C++, no PTLib)
 * This file is isolated from PTLib to avoid ULONG type conflict
 *
 * Copyright (c) 2024 H323ASKW Project
 */

#include "usb_hid_impl.h"

#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDLib.h>
#import <IOKit/hid/IOHIDKeys.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <cstdio>
#include <cstring>

// HID Usage Pages and Usage IDs for mute buttons
namespace HIDUsage {
    const uint32_t Consumer = 0x0C;      // Consumer Devices
    const uint32_t Telephony = 0x0B;     // Telephony Devices
    const uint32_t LED = 0x08;           // LEDs
    const uint32_t Button = 0x09;        // Button
    const uint32_t Mute = 0xE2;          // Consumer Mute
    const uint32_t PhoneMute = 0x2F;     // Phone Mute (Telephony)
    const uint32_t HookSwitch = 0x20;    // Hook Switch (Telephony)
    const uint32_t MuteLED = 0x09;       // Mute LED
    const uint32_t OffHookLED = 0x17;    // Off-Hook LED
    
    // Jabra vendor-specific usage pages
    const uint32_t JabraVendor1 = 0xFF30;  // Jabra specific (65328)
    const uint32_t JabraVendor2 = 0xFF20;  // Jabra specific (65312)
}

// Implementation structure
struct USBHIDImpl {
    IOHIDManagerRef hidManager;
    CFRunLoopRef runLoop;
    IOHIDDeviceRef currentDevice;     // Primary device (Jabra/Plantronics preferred)
    IOHIDDeviceRef jabraDevice;       // Specifically track Jabra device
    int jabraVid;                     // Jabra device VID for verification
    
    pthread_t thread;
    bool threadRunning;
    bool stopRequested;
    
    USBHIDMuteCallback muteCallback;
    void* muteContext;
    
    USBHIDDeviceCallback deviceCallback;
    void* deviceContext;
    
    char deviceName[256];
    bool hasDevice;
    bool isMuted;
    
    pthread_mutex_t mutex;
};

// Forward declarations
static void* ThreadMain(void* arg);
static void DeviceMatchedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef device);
static void DeviceRemovedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef device);
static void InputValueCallback(void* context, IOReturn result, void* sender, IOHIDValueRef value);
static IOHIDElementRef FindLEDElement(IOHIDDeviceRef device, uint32_t usage);
static bool SetDeviceLED(USBHIDImpl* impl, bool isMuted);
static bool SetOffHookLED(USBHIDImpl* impl, bool offHook);

// ============================================================================
// Public C interface
// ============================================================================

USBHIDImplRef USBHIDImpl_Create(void) {
    USBHIDImpl* impl = new USBHIDImpl();
    memset(impl, 0, sizeof(USBHIDImpl));
    pthread_mutex_init(&impl->mutex, NULL);
    return impl;
}

void USBHIDImpl_Destroy(USBHIDImplRef impl) {
    if (impl == NULL) return;
    
    USBHIDImpl_Stop(impl);
    pthread_mutex_destroy(&impl->mutex);
    delete impl;
}

int USBHIDImpl_Start(USBHIDImplRef impl) {
    if (impl == NULL) return 0;
    
    pthread_mutex_lock(&impl->mutex);
    
    if (impl->threadRunning) {
        pthread_mutex_unlock(&impl->mutex);
        return 1;  // Already running
    }
    
    // Create HID manager
    impl->hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (impl->hidManager == NULL) {
        fprintf(stderr, "HIDController: Failed to create HID manager\n");
        pthread_mutex_unlock(&impl->mutex);
        return 0;
    }
    
    // Create matching dictionaries for Consumer and Telephony devices
    CFMutableArrayRef matchingArray = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    
    // Match Consumer devices (page 0x0C)
    CFMutableDictionaryRef consumerDict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int consumerPage = HIDUsage::Consumer;
    CFNumberRef consumerPageNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &consumerPage);
    CFDictionarySetValue(consumerDict, CFSTR(kIOHIDDeviceUsagePageKey), consumerPageNum);
    CFRelease(consumerPageNum);
    CFArrayAppendValue(matchingArray, consumerDict);
    CFRelease(consumerDict);
    
    // Match Telephony devices (page 0x0B)
    CFMutableDictionaryRef telephonyDict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int telephonyPage = HIDUsage::Telephony;
    CFNumberRef telephonyPageNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &telephonyPage);
    CFDictionarySetValue(telephonyDict, CFSTR(kIOHIDDeviceUsagePageKey), telephonyPageNum);
    CFRelease(telephonyPageNum);
    CFArrayAppendValue(matchingArray, telephonyDict);
    CFRelease(telephonyDict);
    
    // Match Jabra devices by Vendor ID (0x0B0E) - catches all Jabra HID interfaces
    CFMutableDictionaryRef jabraVidDict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int jabraVid = 0x0B0E;  // Jabra/GN Audio Vendor ID
    CFNumberRef jabraVidNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &jabraVid);
    CFDictionarySetValue(jabraVidDict, CFSTR(kIOHIDVendorIDKey), jabraVidNum);
    CFRelease(jabraVidNum);
    CFArrayAppendValue(matchingArray, jabraVidDict);
    CFRelease(jabraVidDict);
    
    // Match Plantronics/Poly devices by Vendor ID (0x047F)
    CFMutableDictionaryRef polyVidDict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int polyVid = 0x047F;  // Plantronics/Poly Vendor ID
    CFNumberRef polyVidNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &polyVid);
    CFDictionarySetValue(polyVidDict, CFSTR(kIOHIDVendorIDKey), polyVidNum);
    CFRelease(polyVidNum);
    CFArrayAppendValue(matchingArray, polyVidDict);
    CFRelease(polyVidDict);
    
    IOHIDManagerSetDeviceMatchingMultiple(impl->hidManager, matchingArray);
    CFRelease(matchingArray);
    
    // Register callbacks
    IOHIDManagerRegisterDeviceMatchingCallback(impl->hidManager, DeviceMatchedCallback, impl);
    IOHIDManagerRegisterDeviceRemovalCallback(impl->hidManager, DeviceRemovedCallback, impl);
    IOHIDManagerRegisterInputValueCallback(impl->hidManager, InputValueCallback, impl);
    
    // Start thread
    impl->stopRequested = false;
    if (pthread_create(&impl->thread, NULL, ThreadMain, impl) != 0) {
        fprintf(stderr, "HIDController: Failed to create thread\n");
        CFRelease(impl->hidManager);
        impl->hidManager = NULL;
        pthread_mutex_unlock(&impl->mutex);
        return 0;
    }
    
    impl->threadRunning = true;
    pthread_mutex_unlock(&impl->mutex);
    
    fprintf(stderr, "HIDController: Started\n");
    return 1;
}

void USBHIDImpl_Stop(USBHIDImplRef impl) {
    if (impl == NULL) return;
    
    pthread_mutex_lock(&impl->mutex);
    
    if (!impl->threadRunning) {
        pthread_mutex_unlock(&impl->mutex);
        return;
    }
    
    impl->stopRequested = true;
    
    if (impl->runLoop != NULL) {
        CFRunLoopStop(impl->runLoop);
    }
    
    pthread_mutex_unlock(&impl->mutex);
    
    // Wait for thread to finish
    pthread_join(impl->thread, NULL);
    
    pthread_mutex_lock(&impl->mutex);
    
    if (impl->hidManager != NULL) {
        CFRelease(impl->hidManager);
        impl->hidManager = NULL;
    }
    
    impl->threadRunning = false;
    impl->currentDevice = NULL;
    impl->hasDevice = false;
    impl->deviceName[0] = '\0';
    
    pthread_mutex_unlock(&impl->mutex);
    
    fprintf(stderr, "HIDController: Stopped\n");
}

void USBHIDImpl_SetMuteCallback(USBHIDImplRef impl, USBHIDMuteCallback callback, void* context) {
    if (impl == NULL) return;
    pthread_mutex_lock(&impl->mutex);
    impl->muteCallback = callback;
    impl->muteContext = context;
    pthread_mutex_unlock(&impl->mutex);
}

void USBHIDImpl_SetDeviceCallback(USBHIDImplRef impl, USBHIDDeviceCallback callback, void* context) {
    if (impl == NULL) return;
    pthread_mutex_lock(&impl->mutex);
    impl->deviceCallback = callback;
    impl->deviceContext = context;
    pthread_mutex_unlock(&impl->mutex);
}

void USBHIDImpl_SetMuteLED(USBHIDImplRef impl, int isMuted) {
    if (impl == NULL) return;
    pthread_mutex_lock(&impl->mutex);
    impl->isMuted = (isMuted != 0);
    SetDeviceLED(impl, impl->isMuted);
    pthread_mutex_unlock(&impl->mutex);
}

int USBHIDImpl_HasDevice(USBHIDImplRef impl) {
    if (impl == NULL) return 0;
    pthread_mutex_lock(&impl->mutex);
    int result = impl->hasDevice ? 1 : 0;
    pthread_mutex_unlock(&impl->mutex);
    return result;
}

const char* USBHIDImpl_GetDeviceName(USBHIDImplRef impl) {
    if (impl == NULL) return "";
    // Note: Not thread-safe for simplicity, but deviceName is only modified in callbacks
    return impl->deviceName;
}

// ============================================================================
// Internal functions
// ============================================================================

static void* ThreadMain(void* arg) {
    USBHIDImpl* impl = (USBHIDImpl*)arg;
    
    fprintf(stderr, "HIDController: Thread started\n");
    
    // Store the run loop for this thread
    impl->runLoop = CFRunLoopGetCurrent();
    
    // Schedule the HID manager with this run loop
    IOHIDManagerScheduleWithRunLoop(impl->hidManager, impl->runLoop, kCFRunLoopDefaultMode);
    
    // Open the HID manager
    IOReturn result = IOHIDManagerOpen(impl->hidManager, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "HIDController: Failed to open HID manager: %d\n", result);
        impl->runLoop = NULL;
        return NULL;
    }
    
    fprintf(stderr, "HIDController: HID manager opened, starting run loop\n");
    
    // Run until stopped
    while (!impl->stopRequested) {
        CFRunLoopRunResult runResult = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, true);
        if (runResult == kCFRunLoopRunFinished) {
            break;
        }
    }
    
    // Cleanup
    IOHIDManagerUnscheduleFromRunLoop(impl->hidManager, impl->runLoop, kCFRunLoopDefaultMode);
    IOHIDManagerClose(impl->hidManager, kIOHIDOptionsTypeNone);
    
    impl->runLoop = NULL;
    
    fprintf(stderr, "HIDController: Thread ended\n");
    return NULL;
}

static void DeviceMatchedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef device) {
    USBHIDImpl* impl = (USBHIDImpl*)context;
    if (impl == NULL) return;
    
    // Get device info
    CFStringRef productName = (CFStringRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    CFNumberRef vendorId = (CFNumberRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
    CFNumberRef productId = (CFNumberRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
    
    int vid = 0, pid = 0;
    if (vendorId) CFNumberGetValue(vendorId, kCFNumberIntType, &vid);
    if (productId) CFNumberGetValue(productId, kCFNumberIntType, &pid);
    
    pthread_mutex_lock(&impl->mutex);
    
    impl->deviceName[0] = '\0';
    if (productName) {
        CFStringGetCString(productName, impl->deviceName, sizeof(impl->deviceName), kCFStringEncodingUTF8);
    } else {
        snprintf(impl->deviceName, sizeof(impl->deviceName), "Unknown Device");
    }
    
    fprintf(stderr, "HIDController: Device matched: %s (VID: 0x%04X, PID: 0x%04X)\n",
            impl->deviceName, vid, pid);
    
    // Register input value callback for this specific device
    IOHIDDeviceRegisterInputValueCallback(device, InputValueCallback, impl);
    fprintf(stderr, "HIDController: Registered InputValueCallback for device: %s\n", impl->deviceName);
    
    // Prioritize Jabra/Plantronics devices over generic keyboards/mice
    bool isHeadset = (vid == 0x0B0E) ||  // Jabra/GN Audio
                     (vid == 0x047F) ||  // Plantronics/Poly
                     (vid == 0x17EF) ||  // Lenovo (ThinkPad docks with headsets)
                     (vid == 0x046D);    // Logitech headsets
    
    // Only update currentDevice if this is a headset or we don't have one yet
    if (isHeadset || impl->currentDevice == NULL) {
        impl->currentDevice = device;
        impl->hasDevice = true;
    }
    
    // Specifically track Jabra device
    if (vid == 0x0B0E) {
        impl->jabraDevice = device;
        impl->jabraVid = vid;
    }
    
    // Debug: List all LED elements available on this device
    CFArrayRef elements = IOHIDDeviceCopyMatchingElements(device, NULL, kIOHIDOptionsTypeNone);
    if (elements) {
        fprintf(stderr, "HIDController: Available LED/Output elements for %s:\n", impl->deviceName);
        CFIndex count = CFArrayGetCount(elements);
        for (CFIndex i = 0; i < count; i++) {
            IOHIDElementRef element = (IOHIDElementRef)CFArrayGetValueAtIndex(elements, i);
            uint32_t usagePage = IOHIDElementGetUsagePage(element);
            uint32_t usage = IOHIDElementGetUsage(element);
            IOHIDElementType type = IOHIDElementGetType(element);
            
            // Log LED (0x08) and Button (0x09) elements
            if (type == kIOHIDElementTypeOutput || usagePage == HIDUsage::LED) {
                fprintf(stderr, "  Element: UsagePage=0x%04X Usage=0x%04X Type=%d\n", 
                        usagePage, usage, (int)type);
            }
        }
        CFRelease(elements);
    }
    
    // IMPORTANT: For Jabra devices, set Off-Hook LED first to enable software control mode
    // This tells the headset that we are managing it, enabling mute button HID events
    if (vid == 0x0B0E) {  // Jabra/GN Audio
        fprintf(stderr, "HIDController: Jabra device detected - enabling software control mode\n");
        // Use jabraDevice directly for LED control
        IOHIDDeviceRef targetDevice = impl->jabraDevice;
        if (targetDevice) {
            // Set Off-Hook LED
            IOHIDElementRef offHookLed = FindLEDElement(targetDevice, HIDUsage::OffHookLED);
            if (offHookLed) {
                uint64_t timestamp = mach_absolute_time();
                IOHIDValueRef value = IOHIDValueCreateWithIntegerValue(
                    kCFAllocatorDefault, offHookLed, timestamp, 1);
                if (value) {
                    IOReturn result = IOHIDDeviceSetValue(targetDevice, offHookLed, value);
                    CFRelease(value);
                    if (result == kIOReturnSuccess) {
                        fprintf(stderr, "HIDController: Off-Hook LED set to: ON (software control mode ENABLED)\n");
                    } else {
                        fprintf(stderr, "HIDController: Failed to set Off-Hook LED: %d\n", result);
                    }
                }
            } else {
                fprintf(stderr, "HIDController: Off-Hook LED not found on Jabra device\n");
            }
            
            // Set Mute LED
            IOHIDElementRef muteLed = FindLEDElement(targetDevice, HIDUsage::MuteLED);
            if (muteLed) {
                uint64_t timestamp = mach_absolute_time();
                IOHIDValueRef value = IOHIDValueCreateWithIntegerValue(
                    kCFAllocatorDefault, muteLed, timestamp, impl->isMuted ? 1 : 0);
                if (value) {
                    IOReturn result = IOHIDDeviceSetValue(targetDevice, muteLed, value);
                    CFRelease(value);
                    if (result == kIOReturnSuccess) {
                        fprintf(stderr, "HIDController: Mute LED set to: %s\n", impl->isMuted ? "ON" : "OFF");
                    }
                }
            } else {
                fprintf(stderr, "HIDController: Mute LED not found on Jabra device\n");
            }
        }
    }
    
    // Copy callback info before releasing lock
    USBHIDDeviceCallback callback = impl->deviceCallback;
    void* callbackContext = impl->deviceContext;
    char nameCopy[256];
    strncpy(nameCopy, impl->deviceName, sizeof(nameCopy) - 1);
    nameCopy[sizeof(nameCopy) - 1] = '\0';
    
    pthread_mutex_unlock(&impl->mutex);
    
    // Call callback outside of lock
    if (callback != NULL) {
        callback(callbackContext, nameCopy, 1);
    }
}

static void DeviceRemovedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef device) {
    USBHIDImpl* impl = (USBHIDImpl*)context;
    if (impl == NULL) return;
    
    pthread_mutex_lock(&impl->mutex);
    
    if (impl->currentDevice == device) {
        fprintf(stderr, "HIDController: Device removed: %s\n", impl->deviceName);
        
        char nameCopy[256];
        strncpy(nameCopy, impl->deviceName, sizeof(nameCopy) - 1);
        nameCopy[sizeof(nameCopy) - 1] = '\0';
        
        impl->currentDevice = NULL;
        impl->hasDevice = false;
        impl->deviceName[0] = '\0';
        
        USBHIDDeviceCallback callback = impl->deviceCallback;
        void* callbackContext = impl->deviceContext;
        
        pthread_mutex_unlock(&impl->mutex);
        
        if (callback != NULL) {
            callback(callbackContext, nameCopy, 0);
        }
    } else {
        pthread_mutex_unlock(&impl->mutex);
    }
}

static void InputValueCallback(void* context, IOReturn result, void* sender, IOHIDValueRef value) {
    USBHIDImpl* impl = (USBHIDImpl*)context;
    if (impl == NULL) return;
    
    IOHIDElementRef element = IOHIDValueGetElement(value);
    if (element == NULL) return;
    
    uint32_t usagePage = IOHIDElementGetUsagePage(element);
    uint32_t usage = IOHIDElementGetUsage(element);
    CFIndex intValue = IOHIDValueGetIntegerValue(value);
    
    // Get device info for logging
    IOHIDDeviceRef device = IOHIDElementGetDevice(element);
    int vid = 0, pid = 0;
    if (device) {
        CFNumberRef vendorId = (CFNumberRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
        CFNumberRef productId = (CFNumberRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
        if (vendorId) CFNumberGetValue(vendorId, kCFNumberIntType, &vid);
        if (productId) CFNumberGetValue(productId, kCFNumberIntType, &pid);
    }
    
    // Debug: Log all HID input events for Jabra devices (VID 0x0B0E) or any non-zero value
    if (vid == 0x0B0E || (intValue != 0 && usagePage != 0x07)) {  // Skip keyboard events
        fprintf(stderr, "HIDController: Input [VID:0x%04X] UsagePage: 0x%04X, Usage: 0x%04X, Value: %ld\n",
                vid, usagePage, usage, (long)intValue);
    }
    
    bool isMuteButton = false;
    
    // Check for Consumer Mute (0x0C / 0xE2)
    if (usagePage == HIDUsage::Consumer && usage == HIDUsage::Mute) {
        isMuteButton = true;
    }
    // Check for Telephony Phone Mute (0x0B / 0x2F)
    else if (usagePage == HIDUsage::Telephony && usage == HIDUsage::PhoneMute) {
        isMuteButton = true;
    }
    // Jabra vendor-specific mute button (0xFF30 / 0x2F or 0xFF20 / 0xE2)
    else if ((usagePage == HIDUsage::JabraVendor1 || usagePage == HIDUsage::JabraVendor2) &&
             (usage == HIDUsage::PhoneMute || usage == HIDUsage::Mute || usage == 0x2F || usage == 0xE2)) {
        isMuteButton = true;
        fprintf(stderr, "HIDController: Jabra vendor mute detected (UsagePage: 0x%04X, Usage: 0x%02X)\n",
                usagePage, usage);
    }
    // Check for Volume Increment/Decrement which Jabra might use
    else if (usagePage == HIDUsage::Consumer && (usage == 0xE9 || usage == 0xEA)) {
        // 0xE9 = Volume Increment, 0xEA = Volume Decrement - not mute, skip
    }
    // Jabra devices may use Consumer:Hook Switch (0x20) or other telephony buttons
    else if (usagePage == HIDUsage::Telephony && usage == HIDUsage::HookSwitch) {
        fprintf(stderr, "HIDController: Hook Switch detected (0x0B/0x20)\n");
    }
    // Some Jabra devices use Consumer page with custom usages
    else if (usagePage == HIDUsage::Consumer && 
             (usage == 0xCF || usage == 0x21 || usage == 0xB5 || usage == 0xB6)) {
        // 0xCF = Voice Command, 0x21 = Microsoft Button, 0xB5/0xB6 = Scan Next/Prev
        fprintf(stderr, "HIDController: Consumer button detected (0x0C/0x%02X)\n", usage);
    }
    
    if (isMuteButton && intValue == 1) {  // Button pressed (value = 1)
        fprintf(stderr, "HIDController: Mute button pressed (UsagePage: 0x%02X, Usage: 0x%02X)\n",
                usagePage, usage);
        
        pthread_mutex_lock(&impl->mutex);
        
        // Toggle mute state
        impl->isMuted = !impl->isMuted;
        bool newState = impl->isMuted;
        
        // Update LED
        SetDeviceLED(impl, newState);
        
        USBHIDMuteCallback callback = impl->muteCallback;
        void* callbackContext = impl->muteContext;
        
        pthread_mutex_unlock(&impl->mutex);
        
        // Call callback outside of lock
        if (callback != NULL) {
            callback(callbackContext, newState ? 1 : 0);
        }
    }
}

static IOHIDElementRef FindLEDElement(IOHIDDeviceRef device, uint32_t targetUsage) {
    if (device == NULL) return NULL;
    
    CFArrayRef elements = IOHIDDeviceCopyMatchingElements(device, NULL, kIOHIDOptionsTypeNone);
    if (elements == NULL) return NULL;
    
    IOHIDElementRef foundElement = NULL;
    CFIndex count = CFArrayGetCount(elements);
    
    for (CFIndex i = 0; i < count; i++) {
        IOHIDElementRef element = (IOHIDElementRef)CFArrayGetValueAtIndex(elements, i);
        
        uint32_t usagePage = IOHIDElementGetUsagePage(element);
        uint32_t usage = IOHIDElementGetUsage(element);
        IOHIDElementType type = IOHIDElementGetType(element);
        
        // Look for LED elements (Usage Page 0x08)
        if (usagePage == HIDUsage::LED && type == kIOHIDElementTypeOutput) {
            if (usage == targetUsage) {
                foundElement = element;
                break;
            }
        }
    }
    
    CFRelease(elements);
    return foundElement;
}

static bool SetDeviceLED(USBHIDImpl* impl, bool isMuted) {
    // Note: This function is called with mutex already locked
    // Prefer Jabra device if available, otherwise fall back to currentDevice
    IOHIDDeviceRef targetDevice = impl->jabraDevice ? impl->jabraDevice : impl->currentDevice;
    if (targetDevice == NULL) {
        return false;
    }
    
    IOHIDElementRef ledElement = FindLEDElement(targetDevice, HIDUsage::MuteLED);
    if (ledElement == NULL) {
        fprintf(stderr, "HIDController: Mute LED element not found on target device\n");
        return false;
    }
    
    uint64_t timestamp = mach_absolute_time();
    IOHIDValueRef value = IOHIDValueCreateWithIntegerValue(
        kCFAllocatorDefault, ledElement, timestamp, isMuted ? 1 : 0);
    
    if (value == NULL) {
        return false;
    }
    
    IOReturn result = IOHIDDeviceSetValue(targetDevice, ledElement, value);
    CFRelease(value);
    
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "HIDController: Failed to set Mute LED: %d\n", result);
        return false;
    }
    
    fprintf(stderr, "HIDController: Mute LED set to: %s\n", isMuted ? "ON" : "OFF");
    return true;
}

static bool SetOffHookLED(USBHIDImpl* impl, bool offHook) {
    // Note: This function is called with mutex already locked
    // Prefer Jabra device if available
    IOHIDDeviceRef targetDevice = impl->jabraDevice ? impl->jabraDevice : impl->currentDevice;
    if (targetDevice == NULL) {
        return false;
    }
    
    IOHIDElementRef ledElement = FindLEDElement(targetDevice, HIDUsage::OffHookLED);
    if (ledElement == NULL) {
        fprintf(stderr, "HIDController: Off-Hook LED element not found, trying alternative LEDs\n");
        
        // Try Ring LED (0x18) or Line LED (0x19) as fallback
        ledElement = FindLEDElement(targetDevice, 0x18);  // Ring
        if (ledElement == NULL) {
            ledElement = FindLEDElement(targetDevice, 0x19);  // Line
        }
        
        if (ledElement == NULL) {
            fprintf(stderr, "HIDController: No telephony LED elements found\n");
            return false;
        }
    }
    
    uint64_t timestamp = mach_absolute_time();
    IOHIDValueRef value = IOHIDValueCreateWithIntegerValue(
        kCFAllocatorDefault, ledElement, timestamp, offHook ? 1 : 0);
    
    if (value == NULL) {
        return false;
    }
    
    IOReturn result = IOHIDDeviceSetValue(targetDevice, ledElement, value);
    CFRelease(value);
    
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "HIDController: Failed to set Off-Hook LED: %d\n", result);
        return false;
    }
    
    fprintf(stderr, "HIDController: Off-Hook LED set to: %s (software control mode %s)\n", 
            offHook ? "ON" : "OFF",
            offHook ? "ENABLED" : "disabled");
    return true;
}
