#import "Permissions.h"

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>

#import <netinet/in.h>
#import <sys/socket.h>
#import <unistd.h>
#import <string.h>

static void call_done(perm_done_cb cb, void* ctx)
{
  if (cb) {
    cb(ctx);
  }
}

// 1) Camera + Microphone (TCC)
void askw_request_camera_and_mic(perm_done_cb cb, void* ctx)
{
  @autoreleasepool {
    dispatch_group_t g = dispatch_group_create();

    dispatch_group_enter(g);
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(__unused BOOL granted) {
      dispatch_group_leave(g);
    }];

    dispatch_group_enter(g);
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(__unused BOOL granted) {
      dispatch_group_leave(g);
    }];

    dispatch_group_notify(g, dispatch_get_main_queue(), ^{
      call_done(cb, ctx);
    });
  }
}

// 2) Local Network permission trigger (brief Bonjour publish/browse)
//    Requires NSBonjourServices entry in Info.plist for _perm._tcp.
void askw_trigger_local_network_prompt(perm_done_cb cb, void* ctx)
{
  @autoreleasepool {
    NSNetService* svc = [[NSNetService alloc] initWithDomain:@"local."
                                                        type:@"_perm._tcp."
                                                        name:@"perm"
                                                        port:9];
    [svc publish];

    NSNetServiceBrowser* browser = [NSNetServiceBrowser new];
    [browser searchForServicesOfType:@"_perm._tcp." inDomain:@"local."];

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
      [browser stop];
      [svc stop];
      call_done(cb, ctx);
    });
  }
}

// 3) Firewall inbound-permission trigger (open/close a listen socket)
void askw_trigger_firewall_prompt_once(void)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return;
  }

  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
  addr.sin_port = htons(0);                 // any free port

  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
    listen(fd, 1);
  }
  close(fd);
}
