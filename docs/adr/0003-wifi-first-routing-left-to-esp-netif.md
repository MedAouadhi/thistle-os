# WiFi-first routing is left to esp_netif, and mobile data stays up beside WiFi

With a cellular modem there are two ways to the internet, and traffic should take WiFi when it is there and LTE when it is not. We let esp_netif choose. It already makes the connected interface with the highest route priority the default route: WiFi station at 100, PPP at 20. With `CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF`, the DNS servers follow that choice. The kernel's network coordinator (`net_coordinator.rs`) decides only what to keep up. While mobile data is on, the cellular link stays connected beside WiFi, so it carries traffic the moment WiFi drops. The coordinator also rejoins the saved WiFi network after a drop, since the WiFi manager joins only when asked. `net_manager` ranks transports the same way esp_netif does, so the transport it names is the one in use.

## Considered Options

**The coordinator pins the default route** with `esp_netif_set_default_netif()`. This was rejected. A pinned default turns off esp_netif's own switching until the pinned interface is destroyed. Every up, down, got-IP and lost-IP event would then be the kernel's to track, and the failover esp_netif already does at the moment WiFi's link goes down would be lost.

**Mobile data only while WiFi is down**: dial when WiFi drops, hang up when it returns. This was rejected. It puts a dial, about 5 s, between WiFi dropping and traffic flowing again, and a cold start of the modem takes 20 s or more. Meanwhile an idle PPP session costs almost nothing: no traffic is routed to it, the LCP keepalive goes only between the SoC and the modem, and the network drops an idle radio to its low-power state on its own. Receiving SMS will need the modem powered and registered in any case.

**Leaving DNS global**, as it was, was rejected. lwIP keeps one set of DNS servers, and whichever link connected last sets them. A cellular link that comes up after WiFi points every lookup made over WiFi at the carrier's resolvers, which are often reachable only from inside the carrier's network.

## Consequences

The ranking in `net_manager.rs` (`transport_rank`) must agree with the route priorities in esp-idf's `esp_netif_defaults.h`. A test pins the order.

While the modem carries data it answers no AT commands: the session runs over plain PPP, not CMUX. Signal and operator are therefore the values read before dialling. Sending an SMS interrupts the session for the length of the exchange. Incoming SMS are polled only when no session is up. With mobile data off the modem is powered down, so in practice no SMS is received at all. Moving to CMUX would lift all of these restrictions.

The modem is powered down when the SIM is missing or locked, and it is not tried again until mobile data is switched off and on. A SIM PIN is not entered: a PIN retried by a background loop would use up the card's attempts and lock it behind its PUK.

WiFi can be switched off in Settings (`thistle_os.wifi.radio_off`). That stops the radio and the coordinator's rejoining, so traffic takes LTE without any route being touched. The key is separate from `wifi.enabled`, which says only whether the saved network is joined at boot and is false in a fresh system.json.

The coordinator does all its work on its own thread, whose stack, in internal RAM, it gets at boot: modem attempts, power-downs and WiFi rejoins, one at a time. A thread started for each attempt once could not be: on a T-Deck Max, mobile data switched on after boot never connected because its 8 KB thread found no internal RAM. A stack in PSRAM is no way round that for a thread that rejoins WiFi, which writes NVS, since PSRAM cannot be read while flash is written. The cost is that WiFi is rejoined only once a modem attempt ends, up to two minutes later; traffic has the cellular link meanwhile. Switching mobile data off during an attempt makes the driver give up through `hal_net_driver_t.cancel` rather than waiting for it.
