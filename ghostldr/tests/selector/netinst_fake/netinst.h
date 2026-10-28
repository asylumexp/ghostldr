/*
 * A stand-in for the network installer (native/netinst, rel-netinstall),
 * linked into a test build of the selector module (test_selector.py builds
 * it with make SEL_NET_DIR=../tests/selector/netinst_fake): the interface
 * the selector calls (selector/src/info.c), and a fake behind it.
 */
#pragma once

#define NETINST_IDLE 0
#define NETINST_WAITING 1
#define NETINST_BUSY 2
#define NETINST_STOPPED 3
#define NETINST_FAILED 4
struct netinst_status {
    unsigned size;
    unsigned state;
    char url[40];
    char token[12];
    char title[16];
    char name[64];
    unsigned long long done, total;
    unsigned rate_kib;
    unsigned installed;
    char message[128];
    unsigned changes;
};

struct ghost_api;
int netinst_start(const struct ghost_api *api, void **handle);
int netinst_status(void *handle, struct netinst_status *st);
void netinst_stop(void **handle);
