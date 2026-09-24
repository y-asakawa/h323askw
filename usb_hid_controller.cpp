/*
 * usb_hid_controller.cpp
 *
 * USB HID Controller wrapper using PTLib
 * This file wraps the pure C implementation (usb_hid_impl.mm)
 *
 * Copyright (c) 2024 H323ASKW Project
 * SPDX-License-Identifier: MPL-1.0
 */

#include "usb_hid_controller.h"
#include "usb_hid_impl.h"

// ============================================================================
// USBHIDController implementation
// ============================================================================

USBHIDController::USBHIDController()
    : m_impl(NULL)
    , m_isRunning(false)
    , m_isMuted(false)
    , m_hasDevice(false)
{
#ifdef P_MACOSX
    m_impl = USBHIDImpl_Create();
    if (m_impl) {
        // Set up callbacks
        USBHIDImpl_SetMuteCallback(m_impl, &USBHIDController::MuteCallbackStatic, this);
        USBHIDImpl_SetDeviceCallback(m_impl, &USBHIDController::DeviceCallbackStatic, this);
    }
#endif
    PTRACE(4, "HIDController\tCreated");
}

USBHIDController::~USBHIDController()
{
    Stop();
#ifdef P_MACOSX
    if (m_impl) {
        USBHIDImpl_Destroy(m_impl);
        m_impl = NULL;
    }
#endif
    PTRACE(4, "HIDController\tDestroyed");
}

bool USBHIDController::Start()
{
    PWaitAndSignal lock(m_mutex);
    
    if (m_isRunning) {
        PTRACE(4, "HIDController\tAlready running");
        return true;
    }
    
#ifdef P_MACOSX
    if (m_impl && USBHIDImpl_Start(m_impl)) {
        m_isRunning = true;
        PTRACE(3, "HIDController\tStarted");
        return true;
    }
    PTRACE(1, "HIDController\tFailed to start");
    return false;
#else
    PTRACE(2, "HIDController\tNot supported on this platform");
    return false;
#endif
}

void USBHIDController::Stop()
{
    PWaitAndSignal lock(m_mutex);
    
    if (!m_isRunning) {
        return;
    }
    
#ifdef P_MACOSX
    if (m_impl) {
        USBHIDImpl_Stop(m_impl);
    }
#endif
    
    m_isRunning = false;
    m_hasDevice = false;
    m_deviceName = PString::Empty();
    
    PTRACE(3, "HIDController\tStopped");
}

void USBHIDController::SetMuteStateCallback(MuteStateCallback callback)
{
    PWaitAndSignal lock(m_mutex);
    m_muteCallback = callback;
}

void USBHIDController::SetMuteState(bool isMuted)
{
    PWaitAndSignal lock(m_mutex);
    
    if (m_isMuted == isMuted) {
        return;
    }
    
    m_isMuted = isMuted;
    PTRACE(4, "HIDController\tMute state set to: " << (isMuted ? "MUTED" : "UNMUTED"));
    
#ifdef P_MACOSX
    if (m_impl) {
        USBHIDImpl_SetMuteLED(m_impl, isMuted ? 1 : 0);
    }
#endif
}

void USBHIDController::ToggleMute()
{
    bool newState;
    MuteStateCallback callback;
    
    {
        PWaitAndSignal lock(m_mutex);
        m_isMuted = !m_isMuted;
        newState = m_isMuted;
        callback = m_muteCallback;
        
        PTRACE(3, "HIDController\tMute toggled to: " << (newState ? "MUTED" : "UNMUTED"));
        
#ifdef P_MACOSX
        if (m_impl) {
            USBHIDImpl_SetMuteLED(m_impl, newState ? 1 : 0);
        }
#endif
    }
    
    // Call callback outside of lock
    if (callback) {
        callback(newState);
    }
}

#ifdef P_MACOSX
// Static callback wrappers
void USBHIDController::MuteCallbackStatic(void* context, int isMuted)
{
    USBHIDController* self = static_cast<USBHIDController*>(context);
    if (self) {
        self->OnMuteButtonPressed(isMuted != 0);
    }
}

void USBHIDController::DeviceCallbackStatic(void* context, const char* deviceName, int connected)
{
    USBHIDController* self = static_cast<USBHIDController*>(context);
    if (self) {
        self->OnDeviceChanged(deviceName, connected != 0);
    }
}

void USBHIDController::OnMuteButtonPressed(bool isMuted)
{
    MuteStateCallback callback;
    
    {
        PWaitAndSignal lock(m_mutex);
        m_isMuted = isMuted;
        callback = m_muteCallback;
        PTRACE(3, "HIDController\tMute button pressed, new state: " 
               << (isMuted ? "MUTED" : "UNMUTED"));
    }
    
    // Call callback outside of lock
    if (callback) {
        callback(isMuted);
    }
}

void USBHIDController::OnDeviceChanged(const char* deviceName, bool connected)
{
    PWaitAndSignal lock(m_mutex);
    
    if (connected) {
        m_hasDevice = true;
        m_deviceName = deviceName;
        PTRACE(3, "HIDController\tDevice connected: " << m_deviceName);
    } else {
        m_hasDevice = false;
        m_deviceName = PString::Empty();
        PTRACE(3, "HIDController\tDevice disconnected");
    }
}
#endif
