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

namespace {
    const size_t kMaxTrackedDevices = 16;
    const int kJabraVendorId = 0x0B0E;
    const int kPolyVendorId = 0x047F;
    const int kLenovoVendorId = 0x17EF;
    const int kLogitechVendorId = 0x046D;
}

// Implementation structure
struct USBHIDImpl {
    IOHIDManagerRef hidManager;
    CFRunLoopRef runLoop;
    IOHIDDeviceRef currentDevice;     // Primary device (Jabra/Plantronics preferred)
    IOHIDDeviceRef jabraDevice;       // Specifically track Jabra device
    int jabraVid;                     // Jabra device VID for verification
    IOHIDDeviceRef connectedDevices[kMaxTrackedDevices];
    char connectedNames[kMaxTrackedDevices][256];
    size_t connectedCount;
    
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
static bool SetDeviceLEDOnTarget(IOHIDDeviceRef targetDevice, bool isMuted);
static bool SetOffHookLEDOnTarget(IOHIDDeviceRef targetDevice, bool offHook);
static bool IsPreferredHeadsetVendor(int vid);
static int GetVendorID(IOHIDDeviceRef device);
static void GetProductName(IOHIDDeviceRef device, char* buffer, size_t bufferSize);
static int FindConnectedDeviceIndex(const USBHIDImpl* impl, IOHIDDeviceRef device);
static bool AddConnectedDevice(USBHIDImpl* impl, IOHIDDeviceRef device, const char* deviceName);
static bool RemoveConnectedDevice(USBHIDImpl* impl, IOHIDDeviceRef device, char* removedName, size_t removedNameSize);
static void SelectPrimaryDevice(USBHIDImpl* impl);

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
    
    // NOTE:
    // Broad Consumer/Telephony matching can include internal keyboards and
    // trigger kIOReturnNotPermitted on Finder launch (TCC/Input Monitoring).
    // To keep distribution builds working without extra permission prompts,
    // default to headset-vendor matching only.
    CFMutableArrayRef matchingArray = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    auto appendVendorMatch = [&](int vendorId) {
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFNumberRef vidNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendorId);
        CFDictionarySetValue(dict, CFSTR(kIOHIDVendorIDKey), vidNum);
        CFRelease(vidNum);
        CFArrayAppendValue(matchingArray, dict);
        CFRelease(dict);
    };

    appendVendorMatch(kJabraVendorId);    // Jabra/GN Audio
    appendVendorMatch(kPolyVendorId);     // Poly/Plantronics
    appendVendorMatch(kLenovoVendorId);   // Lenovo headsets/speakerphones
    appendVendorMatch(kLogitechVendorId); // Logitech headsets

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
    impl->hidManager = NULL;
    impl->runLoop = NULL;
    impl->threadRunning = false;
    impl->currentDevice = NULL;
    impl->jabraDevice = NULL;
    impl->jabraVid = 0;
    impl->connectedCount = 0;
    memset(impl->connectedDevices, 0, sizeof(impl->connectedDevices));
    memset(impl->connectedNames, 0, sizeof(impl->connectedNames));
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
    pthread_mutex_lock(&impl->mutex);
    impl->runLoop = CFRunLoopGetCurrent();
    pthread_mutex_unlock(&impl->mutex);
    
    // Schedule the HID manager with this run loop
    IOHIDManagerScheduleWithRunLoop(impl->hidManager, impl->runLoop, kCFRunLoopDefaultMode);
    
    // Open the HID manager
    bool managerOpened = false;
    IOReturn result = IOHIDManagerOpen(impl->hidManager, kIOHIDOptionsTypeNone);
    if (result != kIOReturnSuccess) {
        fprintf(stderr, "HIDController: Failed to open HID manager: %d\n", result);
    } else {
        managerOpened = true;
        fprintf(stderr, "HIDController: HID manager opened, starting run loop\n");
    }
    
    // Run until stopped
    if (managerOpened) {
        while (!impl->stopRequested) {
            CFRunLoopRunResult runResult = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, true);
            if (runResult == kCFRunLoopRunFinished) {
                break;
            }
        }
    }
    
    // Cleanup on the HID thread. Releasing manager from another thread can
    // trigger IOKit assertions inside unschedule/close paths.
    IOHIDManagerRef manager = NULL;
    CFRunLoopRef runLoop = NULL;
    pthread_mutex_lock(&impl->mutex);
    manager = impl->hidManager;
    runLoop = impl->runLoop;
    impl->hidManager = NULL;
    impl->runLoop = NULL;
    pthread_mutex_unlock(&impl->mutex);

    if (manager != NULL) {
        if (runLoop != NULL) {
            IOHIDManagerUnscheduleFromRunLoop(manager, runLoop, kCFRunLoopDefaultMode);
        }
        if (managerOpened) {
            IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        }
        CFRelease(manager);
    }
    
    fprintf(stderr, "HIDController: Thread ended\n");
    return NULL;
}

static void DeviceMatchedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef device) {
    USBHIDImpl* impl = (USBHIDImpl*)context;
    if (impl == NULL) return;
    
    (void)result;
    (void)sender;

    // Register input value callback for this specific device
    IOHIDDeviceRegisterInputValueCallback(device, InputValueCallback, impl);

    int vid = GetVendorID(device);
    int pid = 0;
    CFNumberRef productId = (CFNumberRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
    if (productId) {
        CFNumberGetValue(productId, kCFNumberIntType, &pid);
    }

    char matchedName[256];
    GetProductName(device, matchedName, sizeof(matchedName));
    fprintf(stderr, "HIDController: Device matched: %s (VID: 0x%04X, PID: 0x%04X)\n",
            matchedName, vid, pid);
    fprintf(stderr, "HIDController: Registered InputValueCallback for device: %s\n", matchedName);
    
    // Debug: List all LED elements available on this device
    CFArrayRef elements = IOHIDDeviceCopyMatchingElements(device, NULL, kIOHIDOptionsTypeNone);
    if (elements) {
        fprintf(stderr, "HIDController: Available LED/Output elements for %s:\n", matchedName);
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
    
    pthread_mutex_lock(&impl->mutex);
    bool wasAvailable = impl->hasDevice;
    char previousPrimary[256];
    strncpy(previousPrimary, impl->deviceName, sizeof(previousPrimary) - 1);
    previousPrimary[sizeof(previousPrimary) - 1] = '\0';

    bool added = AddConnectedDevice(impl, device, matchedName);
    SelectPrimaryDevice(impl);

    if (vid == kJabraVendorId) {
        fprintf(stderr, "HIDController: Jabra device detected - enabling software control mode\n");
        SetOffHookLEDOnTarget(device, true);
    }

    // New device joins centralized mute immediately.
    SetDeviceLEDOnTarget(device, impl->isMuted);

    USBHIDDeviceCallback callback = impl->deviceCallback;
    void* callbackContext = impl->deviceContext;
    bool shouldNotifyConnected = false;
    if (!wasAvailable && impl->hasDevice) {
        shouldNotifyConnected = true;
    } else if (strcmp(previousPrimary, impl->deviceName) != 0) {
        shouldNotifyConnected = true;
    } else if (added) {
        shouldNotifyConnected = true;
    }
    char nameCopy[256];
    strncpy(nameCopy, impl->deviceName, sizeof(nameCopy) - 1);
    nameCopy[sizeof(nameCopy) - 1] = '\0';
    bool hasDeviceNow = impl->hasDevice;

    pthread_mutex_unlock(&impl->mutex);
    
    // Call callback outside of lock
    if (callback != NULL && shouldNotifyConnected && hasDeviceNow) {
        callback(callbackContext, nameCopy, 1);
    }
}

static void DeviceRemovedCallback(void* context, IOReturn result, void* sender, IOHIDDeviceRef device) {
    USBHIDImpl* impl = (USBHIDImpl*)context;
    if (impl == NULL) return;

    (void)result;
    (void)sender;

    pthread_mutex_lock(&impl->mutex);
    bool wasAvailable = impl->hasDevice;
    char previousPrimary[256];
    strncpy(previousPrimary, impl->deviceName, sizeof(previousPrimary) - 1);
    previousPrimary[sizeof(previousPrimary) - 1] = '\0';

    char removedName[256];
    bool removed = RemoveConnectedDevice(impl, device, removedName, sizeof(removedName));
    if (!removed) {
        pthread_mutex_unlock(&impl->mutex);
        return;
    }

    fprintf(stderr, "HIDController: Device removed: %s\n", removedName);

    SelectPrimaryDevice(impl);

    USBHIDDeviceCallback callback = impl->deviceCallback;
    void* callbackContext = impl->deviceContext;

    bool notifyDisconnected = (wasAvailable && !impl->hasDevice);
    bool notifyConnected = false;
    if (impl->hasDevice && strcmp(previousPrimary, impl->deviceName) != 0) {
        notifyConnected = true;
    }

    char nameCopy[256];
    if (notifyDisconnected) {
        strncpy(nameCopy, removedName, sizeof(nameCopy) - 1);
    } else {
        strncpy(nameCopy, impl->deviceName, sizeof(nameCopy) - 1);
    }
    nameCopy[sizeof(nameCopy) - 1] = '\0';

    pthread_mutex_unlock(&impl->mutex);

    if (callback != NULL) {
        if (notifyDisconnected) {
            callback(callbackContext, nameCopy, 0);
        } else if (notifyConnected) {
            callback(callbackContext, nameCopy, 1);
        }
    }
}

static void InputValueCallback(void* context, IOReturn result, void* sender, IOHIDValueRef value) {
    USBHIDImpl* impl = (USBHIDImpl*)context;
    if (impl == NULL) return;

    (void)result;
    (void)sender;
    
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

static bool IsPreferredHeadsetVendor(int vid)
{
    return vid == kJabraVendorId
        || vid == kPolyVendorId
        || vid == kLenovoVendorId
        || vid == kLogitechVendorId;
}

static int GetVendorID(IOHIDDeviceRef device)
{
    if (device == NULL) {
        return 0;
    }

    int vid = 0;
    CFNumberRef vendorId = (CFNumberRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
    if (vendorId) {
        CFNumberGetValue(vendorId, kCFNumberIntType, &vid);
    }
    return vid;
}

static void GetProductName(IOHIDDeviceRef device, char* buffer, size_t bufferSize)
{
    if (bufferSize == 0) {
        return;
    }

    buffer[0] = '\0';
    if (device == NULL) {
        snprintf(buffer, bufferSize, "Unknown Device");
        return;
    }

    CFStringRef productName = (CFStringRef)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    if (productName != NULL &&
        CFStringGetCString(productName, buffer, (CFIndex)bufferSize, kCFStringEncodingUTF8)) {
        return;
    }

    snprintf(buffer, bufferSize, "Unknown Device");
}

static int FindConnectedDeviceIndex(const USBHIDImpl* impl, IOHIDDeviceRef device)
{
    if (impl == NULL || device == NULL) {
        return -1;
    }

    for (size_t i = 0; i < impl->connectedCount; ++i) {
        if (impl->connectedDevices[i] == device) {
            return (int)i;
        }
    }

    return -1;
}

static bool AddConnectedDevice(USBHIDImpl* impl, IOHIDDeviceRef device, const char* deviceName)
{
    if (impl == NULL || device == NULL) {
        return false;
    }

    int index = FindConnectedDeviceIndex(impl, device);
    if (index >= 0) {
        strncpy(impl->connectedNames[index], deviceName ? deviceName : "Unknown Device",
                sizeof(impl->connectedNames[index]) - 1);
        impl->connectedNames[index][sizeof(impl->connectedNames[index]) - 1] = '\0';
        return false;
    }

    if (impl->connectedCount >= kMaxTrackedDevices) {
        fprintf(stderr, "HIDController: Device list full (%zu), skipping additional device\n", kMaxTrackedDevices);
        return false;
    }

    size_t newIndex = impl->connectedCount++;
    impl->connectedDevices[newIndex] = device;
    strncpy(impl->connectedNames[newIndex], deviceName ? deviceName : "Unknown Device",
            sizeof(impl->connectedNames[newIndex]) - 1);
    impl->connectedNames[newIndex][sizeof(impl->connectedNames[newIndex]) - 1] = '\0';
    return true;
}

static bool RemoveConnectedDevice(USBHIDImpl* impl, IOHIDDeviceRef device, char* removedName, size_t removedNameSize)
{
    if (removedNameSize > 0) {
        removedName[0] = '\0';
    }

    if (impl == NULL || device == NULL) {
        return false;
    }

    int index = FindConnectedDeviceIndex(impl, device);
    if (index < 0) {
        return false;
    }

    if (removedNameSize > 0) {
        strncpy(removedName, impl->connectedNames[index], removedNameSize - 1);
        removedName[removedNameSize - 1] = '\0';
    }

    for (size_t i = (size_t)index; i + 1 < impl->connectedCount; ++i) {
        impl->connectedDevices[i] = impl->connectedDevices[i + 1];
        strncpy(impl->connectedNames[i], impl->connectedNames[i + 1], sizeof(impl->connectedNames[i]) - 1);
        impl->connectedNames[i][sizeof(impl->connectedNames[i]) - 1] = '\0';
    }

    if (impl->connectedCount > 0) {
        impl->connectedCount--;
        impl->connectedDevices[impl->connectedCount] = NULL;
        impl->connectedNames[impl->connectedCount][0] = '\0';
    }

    return true;
}

static void SelectPrimaryDevice(USBHIDImpl* impl)
{
    if (impl == NULL) {
        return;
    }

    impl->jabraDevice = NULL;
    impl->jabraVid = 0;

    IOHIDDeviceRef preferredDevice = NULL;
    IOHIDDeviceRef fallbackDevice = NULL;
    for (size_t i = 0; i < impl->connectedCount; ++i) {
        IOHIDDeviceRef candidate = impl->connectedDevices[i];
        if (candidate == NULL) {
            continue;
        }

        if (fallbackDevice == NULL) {
            fallbackDevice = candidate;
        }

        int vid = GetVendorID(candidate);
        if (vid == kJabraVendorId && impl->jabraDevice == NULL) {
            impl->jabraDevice = candidate;
            impl->jabraVid = vid;
        }

        if (preferredDevice == NULL && IsPreferredHeadsetVendor(vid)) {
            preferredDevice = candidate;
        }
    }

    if (impl->jabraDevice != NULL) {
        impl->currentDevice = impl->jabraDevice;
    } else if (preferredDevice != NULL) {
        impl->currentDevice = preferredDevice;
    } else {
        impl->currentDevice = fallbackDevice;
    }

    impl->hasDevice = (impl->currentDevice != NULL);
    if (impl->hasDevice) {
        GetProductName(impl->currentDevice, impl->deviceName, sizeof(impl->deviceName));
    } else {
        impl->deviceName[0] = '\0';
    }
}

static bool SetDeviceLEDOnTarget(IOHIDDeviceRef targetDevice, bool isMuted)
{
    if (targetDevice == NULL) {
        return false;
    }

    IOHIDElementRef ledElement = FindLEDElement(targetDevice, HIDUsage::MuteLED);
    if (ledElement == NULL) {
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

    return result == kIOReturnSuccess;
}

static bool SetOffHookLEDOnTarget(IOHIDDeviceRef targetDevice, bool offHook)
{
    if (targetDevice == NULL) {
        return false;
    }

    IOHIDElementRef ledElement = FindLEDElement(targetDevice, HIDUsage::OffHookLED);
    if (ledElement == NULL) {
        // Try Ring LED (0x18) or Line LED (0x19) as fallback
        ledElement = FindLEDElement(targetDevice, 0x18);
        if (ledElement == NULL) {
            ledElement = FindLEDElement(targetDevice, 0x19);
        }
        if (ledElement == NULL) {
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

    return result == kIOReturnSuccess;
}

static bool SetDeviceLED(USBHIDImpl* impl, bool isMuted)
{
    // Note: This function is called with mutex already locked
    if (impl == NULL || impl->connectedCount == 0) {
        return false;
    }

    bool anySuccess = false;
    for (size_t i = 0; i < impl->connectedCount; ++i) {
        IOHIDDeviceRef device = impl->connectedDevices[i];
        if (SetDeviceLEDOnTarget(device, isMuted)) {
            anySuccess = true;
        }
    }

    if (!anySuccess) {
        fprintf(stderr, "HIDController: Mute LED element not found on connected devices\n");
        return false;
    }

    fprintf(stderr, "HIDController: Mute LED set to: %s on %zu device(s)\n",
            isMuted ? "ON" : "OFF", impl->connectedCount);
    return true;
}
