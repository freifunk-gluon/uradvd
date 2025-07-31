# uradvd

A tiny IPv6 router advertisement daemon.

```
Usage: uradvd [-h] -i <interface> -a/-p <prefix> [ -a/-p <prefix> ... ] [ --default-lifetime <seconds> ] [ --rdnss <ip> ... ] [ --valid-lifetime <seconds> ] [ --preferred-lifetime <seconds> ] [ --max-router-adv-interval <seconds> ]
```

* `-i <interface>`: interface to listen on (e.g. `eth0`)
* `-a <prefix>`: IPv6 prefix (e.g. `2001:db8:1::/32`) that can be used for stateless address configuration (SLAAC). The `A` flag of the IPv6 Router Advertisement message is set.
* `-p <prefix>`: Similar to `-a`. But the `A` and `L` flags are set. The `L` flags means that IPv6 addresses within that prefix are on the same L2 subnet. In doubt, use this option over `-a`.
* `--default-lifetime <seconds>`: lifetime of the default route (default is `0` => do not use router as a default route)
* `--rdnss <ip>`: DNS server (e.g. `2001:4860:4860::8888`)
* `--valid-lifetime <seconds>`: The total time the prefix remains available before becoming unusable (default is `86400` seconds => one day)
* `--preferred-lifetime <seconds>`: The span of time during which the address can be freely used as a source and destination for traffic. Should be less or equal valid-lifetime. (default is `14400` seconds => 4h)
* `--max-router-adv-interval <seconds>`: The maximum time allowed between sending unsolicited multicast router advertisements from the interface, in seconds. (default is `600` seconds => 10m)

This program is packaged for [OpenWrt](https://openwrt.org/).
