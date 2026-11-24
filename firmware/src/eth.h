#pragma once
#include <Arduino.h>

// "ETH" (network adapter) mode.
//
// While it is on, the device shows an additional USB CDC-NCM network adapter to its host and
// the two bridges carry IPv4 packets between their hosts - over LoRa, or over the 2.4 GHz link
// while FAST mode is up. Each bridge is the DHCP server for its own host: the host of the
// master gets the "master IP", the host of the slave the "slave IP", both in the same subnet.
// There is no address for the bridges themselves: ARP for the other host is answered locally
// and only IP packets (no Ethernet headers, no broadcasts) go over the air.
//
// The mode switch and the addresses are one shared setting: a change on either end (button,
// AT command) is sent to the peer and stored there too; every heartbeat carries a checksum of
// it, so two ends that disagree (one was off, reset to defaults, ...) find out and the newer
// setting wins.
namespace eth {

void begin();   // after configLoad(), before USB is attached again in setup()
void loop();

bool enabled();
bool synced();  // the peer runs the same setting (so its network adapter is on as well)
const char* setEnabled(bool on);  // nullptr = done, else why not
// network a.b.c.d/prefix with the two host addresses; nullptr = stored, else what is wrong
const char* setAddresses(uint32_t masterIp, uint32_t slaveIp, uint8_t prefix);
uint32_t network();
uint32_t netmask();
uint32_t hostIp();   // address of the host on this end
uint32_t peerIp();   // address of the host on the other end
bool leased();       // the host has taken its address
bool busy();
void status(Print& o);
bool parseIp(const char* s, uint32_t& ip, const char** end = nullptr);
void fmtIp(char* out, size_t n, uint32_t ip);

// ---- hooks for lora_link.cpp
size_t ctlPoll(uint8_t* buf, size_t max);      // a control frame to send now (0 = none)
void onCtl(const uint8_t* pl, size_t n);       // control frame from the peer
uint32_t digest();                             // checksum of the shared setting (goes into every ping)
void onPeerDigest(bool present, uint32_t d);   // ... as received from the peer
void onPacket(const uint8_t* ip, size_t n);    // IP packet from the peer's host

}  // namespace eth
