/*
 * usb_hid_controller.h
 *
 * USB HID Controller for mute button integration
 * Supports Jabra, Plantronics, Logitech, and other HID-compliant devices
 *
 * Copyright (c) 2024 H323ASKW Project
 */

#ifndef USB_HID_CONTROLLER_H
#define USB_HID_CONTROLLER_H

#include <ptlib.h>
#include <functional>

// Forward declaration of C implementation
struct USBHIDImpl;

/**
 * USB HID Controller class for handling mute button events
 * 
 * This class monitors USB HID devices for mute button presses and
 * synchronizes the mute state with the application.
 * 
 * Uses a C interface wrapper to avoid conflicts between PTLib and IOKit types.
 */
class USBHIDController : public PObject
{
    PCLASSINFO(USBHIDController, PObject);

public:
    /**
     * Callback type for mute state changes
     * @param isMuted true if the new state is muted
     */
    typedef std::function<void(bool isMuted)> MuteStateCallback;

    /**
     * Constructor
     */
    USBHIDController();

    /**
     * Destructor
     */
    ~USBHIDController();

    /**
     * Start monitoring HID devices for mute button events
     * @return true if successfully started
     */
    bool Start();

    /**
     * Stop monitoring HID devices
     */
    void Stop();

    /**
     * Check if the controller is running
     * @return true if monitoring is active
     */
    bool IsRunning() const { return m_isRunning; }

    /**
     * Set the callback for mute state changes
     * @param callback Function to call when mute state changes
     */
    void SetMuteStateCallback(MuteStateCallback callback);

    /**
     * Set the mute state (called by application to sync with device LED)
     * @param isMuted true to set muted state
     */
    void SetMuteState(bool isMuted);

    /**
     * Get the current mute state
     * @return true if currently muted
     */
    bool GetMuteState() const { return m_isMuted; }

    /**
     * Toggle the mute state
     */
    void ToggleMute();

    /**
     * Get the name of the connected HID device (if any)
     * @return Device name or empty string
     */
    PString GetDeviceName() const { return m_deviceName; }

    /**
     * Check if a compatible HID device is connected
     * @return true if a device is connected
     */
    bool HasDevice() const { return m_hasDevice; }

private:
    // C implementation handle
    USBHIDImpl* m_impl;
    
    // State
    PMutex m_mutex;
    bool m_isRunning;
    bool m_isMuted;
    bool m_hasDevice;
    PString m_deviceName;
    MuteStateCallback m_muteCallback;
    
#ifdef P_MACOSX
    // Static callback wrappers for C interface
    static void MuteCallbackStatic(void* context, int isMuted);
    static void DeviceCallbackStatic(void* context, const char* deviceName, int connected);
    
    // Instance callback handlers
    void OnMuteButtonPressed(bool isMuted);
    void OnDeviceChanged(const char* deviceName, bool connected);
#endif
};

#endif // USB_HID_CONTROLLER_H
