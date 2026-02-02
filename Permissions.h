#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*perm_done_cb)(void* ctx);

// Request camera and microphone (TCC) permissions.
void askw_request_camera_and_mic(perm_done_cb cb, void* ctx);

// Proactively trigger the local-network permission prompt.
void askw_trigger_local_network_prompt(perm_done_cb cb, void* ctx);

// Trigger the macOS firewall inbound-connection prompt once.
void askw_trigger_firewall_prompt_once(void);

#ifdef __cplusplus
}
#endif
