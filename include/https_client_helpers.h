#pragma once

// 0.2.37 BUG-083: shared BearSSL client configuration.
//
// Three call sites (central_client::postWithFallback,
// central_client::getWithFallback, web_server_manager::proxyToHub,
// bootstrap_main::announceOnceForBootstrap) all repeated:
//
//     client->setInsecure();
//     client->setBufferSizes(512, 512);
//
// with identical 0.2.5 pool-revert comments. A change to the TLS
// tuning (e.g. a future cert-pinning ship) had to land in all
// three places independently. Now there's one source of truth:
// every HTTPS call site that talks to the hub goes through this
// helper.
//
// Kept as an inline function in a header (not a .cpp) so the
// compiler can keep it on the call site's stack frame and no
// extra object file links in.

#include <WiFiClientSecureBearSSL.h>

inline void configureBearSSLClient(BearSSL::WiFiClientSecure& client) {
  // setInsecure() skips certificate verification. The hub's TLS cert
  // can change at any time and the device cannot run a CA-bundle big
  // enough to track WebPKI; the trust model is "the network is the
  // perimeter" — devices live on the operator's LAN talking to the
  // operator's hub, and a MITM there is a physical-access threat
  // outside this device's protection scope.
  client.setInsecure();
  // setBufferSizes(512, 512) caps BearSSL's per-connection RX + TX
  // buffer allocations at 512 bytes each — the smallest that still
  // handles the operator's hub responses (heartbeat JSON, command
  // poll, firmware metadata). Default is 16K which would not fit
  // the ESP8266 heap budget alongside the rest of the firmware. The
  // 0.2.5 pool-revert demonstrated that per-call BearSSL with these
  // small buffers is the only configuration that doesn't fragment
  // the heap into a death spiral.
  client.setBufferSizes(512, 512);
}
