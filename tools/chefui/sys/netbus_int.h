/*
 * netbus_int.h - the netbus connection for other system-bus clients in
 * the app (wifi.c): one sd-bus connection on the chefui loop, shared.
 *
 * A client registers a callback that gets the bus when it (re)opens and
 * NULL when it is lost or closed; on NULL it must unref every slot it
 * holds (slots keep the old bus alive). Calls made outside a bus
 * callback need netbus_kick() afterwards so the loop writes them out.
 * Every call carries NO_AUTO_START (netbus_new_call).
 */
#ifndef CHEF_NETBUS_INT_H
#define CHEF_NETBUS_INT_H

#include <stdint.h>

#if defined(CHEF_BASU)
#include <basu/sd-bus.h>
#else
#include <systemd/sd-bus.h>
#endif

#define NETBUS_CLIENTS_MAX 4

typedef void (*netbus_bus_cb)(sd_bus *bus);

void netbus_add_client(netbus_bus_cb cb);
sd_bus *netbus_bus(void);
int netbus_new_call(sd_bus_message **m, const char *dest, const char *path, const char *iface,
		    const char *member);
/* sd_bus_call_async; usec 0 = the default timeout. A failure marks the
 * bus broken (it is reopened). */
int netbus_send(sd_bus_message *m, sd_bus_slot **slot, sd_bus_message_handler_t cb, void *ud,
		uint64_t usec);
int netbus_match(sd_bus_slot **slot, const char *rule, sd_bus_message_handler_t cb, void *ud);
void netbus_kick(void);

#endif /* CHEF_NETBUS_INT_H */
