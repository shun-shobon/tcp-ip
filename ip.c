#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ip.h"
#include "net.h"
#include "util.h"

struct ip_hdr {
  uint8_t vhl;
  uint8_t tos;
  uint16_t total;
  uint16_t id;
  uint16_t offset;
  uint8_t ttl;
  uint8_t protocol;
  uint16_t sum;
  ip_addr_t src;
  ip_addr_t dst;
  uint8_t options[0];
};

struct ip_protocol {
  struct ip_protocol *next;
  uint8_t type;
  void (*handler)(const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst,
                  struct ip_iface *iface);
};

const ip_addr_t IP_ADDR_ANY = 0x00000000;       /* 0.0.0.0 */
const ip_addr_t IP_ADDR_BROADCAST = 0xffffffff; /* 255.255.255.255 */

/* NOTE: if you want to add/delete the entries after net_run(), you need to
 * protect these lists with a mutex */
static struct ip_iface *ifaces;
static struct ip_protocol *protocols;

int ip_addr_pton(const char *p, ip_addr_t *n) {
  char *sp, *ep;
  int idx;
  long ret;

  sp = (char *)p;

  for (idx = 0; idx < 4; idx++) {
    ret = strtol(sp, &ep, 10);
    if (ret < 0 || ret > 255) {
      return -1;
    }
    if (ep == sp) {
      return -1;
    }
    if ((idx == 3 && *ep != '\0') || (idx != 3 && *ep != '.')) {
      return -1;
    }
    ((uint8_t *)n)[idx] = ret;
    sp = ep + 1;
  }
  return 0;
}

char *ip_addr_ntop(const ip_addr_t n, char *p, size_t size) {
  uint8_t *u8;

  u8 = (uint8_t *)&n;
  snprintf(p, size, "%d.%d.%d.%d", u8[0], u8[1], u8[2], u8[3]);

  return p;
}

void ip_dump(const uint8_t *data, size_t len) {
  struct ip_hdr *hdr;
  uint8_t v, hl, hlen;
  uint16_t total, offset;
  char addr[IP_ADDR_STR_LEN];

  flockfile(stderr);
  hdr = (struct ip_hdr *)data;
  v = (hdr->vhl & 0xf0) >> 4;
  hl = hdr->vhl & 0x0f;
  hlen = hl << 2;
  fprintf(stderr, "        vhl: 0x%02x [v: %u, hl: %u (%u)]\n", hdr->vhl, v, hl,
          hlen);
  fprintf(stderr, "        tos: 0x%02x\n", hdr->tos);
  total = ntoh16(hdr->total);
  fprintf(stderr, "      total: %u (payload: %u)\n", total, total - hlen);
  fprintf(stderr, "         id: %u\n", ntoh16(hdr->id));
  offset = ntoh16(hdr->offset);
  fprintf(stderr, "     offset: 0x%04x [flags=%x, offset=%u]\n", offset,
          (offset & 0xe000) >> 13, offset & 0x1fff);
  fprintf(stderr, "        ttl: %u\n", hdr->ttl);
  fprintf(stderr, "   protocol: %u\n", hdr->protocol);
  fprintf(stderr, "        sum: 0x%04x\n", ntoh16(hdr->sum));
  fprintf(stderr, "        src: %s\n",
          ip_addr_ntop(hdr->src, addr, sizeof(addr)));
  fprintf(stderr, "        dst: %s\n",
          ip_addr_ntop(hdr->dst, addr, sizeof(addr)));

#ifdef HEXDUMP
  hexdump(stderr, data, len);
#endif

  funlockfile(stderr);
}

struct ip_iface *ip_iface_alloc(const char *unicast, const char *netmask) {
  struct ip_iface *iface;

  iface = calloc(1, sizeof(*iface));
  if (!iface) {
    errorf("calloc() failure");
    return NULL;
  }

  NET_IFACE(iface)->family = NET_IFACE_FAMILY_IP;
  if (ip_addr_pton(unicast, &iface->unicast) == -1) {
    errorf("ip_addr_pton() failure, addr=%s", unicast);
    free(iface);
    return NULL;
  }
  if (ip_addr_pton(netmask, &iface->netmask) == -1) {
    errorf("ip_addr_pton() failure, addr=%s", unicast);
    free(iface);
    return NULL;
  }
  iface->broadcast = (iface->unicast & iface->netmask) | ~iface->netmask;

  return iface;
}
/* NOTE: must not be call after net_run() */
int ip_iface_register(struct net_device *dev, struct ip_iface *iface) {
  char addr1[IP_ADDR_STR_LEN];
  char addr2[IP_ADDR_STR_LEN];
  char addr3[IP_ADDR_STR_LEN];

  if (net_device_add_iface(dev, NET_IFACE(iface)) == -1) {
    errorf("net_device_add_iface() failure");
    return -1;
  }

  iface->next = ifaces;
  ifaces = iface;

  infof("registered: dev=%s, unicast=%s, netmask=%s, broadcast=%s", dev->name,
        ip_addr_ntop(iface->unicast, addr1, sizeof(addr1)),
        ip_addr_ntop(iface->unicast, addr2, sizeof(addr2)),
        ip_addr_ntop(iface->unicast, addr3, sizeof(addr3)));

  return 0;
}
struct ip_iface *ip_iface_select(ip_addr_t addr) {
  struct ip_iface *entry;

  for (entry = ifaces; entry; entry = entry->next) {
    if (entry->unicast == addr) {
      return entry;
    }
  }

  return NULL;
}

static void ip_input(const uint8_t *data, size_t len, struct net_device *dev) {
  struct ip_hdr *hdr;
  uint8_t v, hlen;
  uint16_t offset, total;
  struct ip_iface *iface;
  struct ip_protocol *proto;
  char addr[IP_ADDR_STR_LEN];

  if (len < IP_HDR_SIZE_MIN) {
    errorf("too short");
    return;
  }

  hdr = (struct ip_hdr *)data;
  v = hdr->vhl >> 4;
  hlen = (hdr->vhl & 0x0f) << 2;

  if (v != IP_VERSION_IPV4) {
    errorf("ip version error: v=%u", v);
    return;
  }
  if (len < hlen) {
    errorf("header length error: hlen=%u, len=%u", hlen, len);
    return;
  }
  total = ntoh16(hdr->total);
  if (len < total) {
    errorf("total length error: total=%u, len=%u", total, len);
    return;
  }
  if (cksum16((uint16_t *)hdr, hlen, 0) != 0) {
    errorf("checksum error: sum=0x%0fx, verify=0x%04x", ntoh16(hdr->sum),
           ntoh16(cksum16((uint16_t *)hdr, hlen, -hdr->sum)));
    return;
  }

  offset = ntoh16(hdr->offset);
  if (offset & 0x2000 || offset & 0x1fff) {
    errorf("fragments does not support");
    return;
  }

  iface = (struct ip_iface *)net_device_get_iface(dev, NET_IFACE_FAMILY_IP);
  if (!iface) {
    errorf("iface is not registered to the device");
    return;
  }
  if (hdr->dst != iface->unicast && hdr->dst != iface->broadcast &&
      hdr->dst != IP_ADDR_BROADCAST) {
    return;
  }

  debugf("dev=%s, iface=%s, protocol=%u, total=%u", dev->name,
         ip_addr_ntop(iface->unicast, addr, sizeof(addr)), hdr->protocol,
         total);
  ip_dump(data, total);

  for (proto = protocols; proto; proto = proto->next) {
    if (proto->type == hdr->protocol) {
      proto->handler((uint8_t *)data + hlen, len - hlen, hdr->src, hdr->dst,
                     iface);
      return;
    }
  }
  /* unsupported protocol */
};

static int ip_output_device(struct ip_iface *iface, const uint8_t *data,
                            size_t len, ip_addr_t dst) {
  uint8_t hwaddr[NET_DEVICE_ADDR_LEN] = {};

  if (NET_IFACE(iface)->dev->flags & NET_DEVICE_FLAG_NEED_ARP) {
    if (dst == iface->broadcast || dst == IP_ADDR_BROADCAST) {
      memcpy(hwaddr, NET_IFACE(iface)->dev->broadcast,
             NET_IFACE(iface)->dev->alen);
    } else {
      errorf("arp does not implement");
      return -1;
    }
  }

  return net_device_output(NET_IFACE(iface)->dev, NET_PROTOCOL_TYPE_IP, data,
                           len, hwaddr);
}

static ssize_t ip_output_core(struct ip_iface *iface, uint8_t protocol,
                              const uint8_t *data, size_t len, ip_addr_t src,
                              ip_addr_t dst, uint16_t id, uint16_t offset) {
  uint8_t buf[IP_TOTAL_SIZE_MAX];
  struct ip_hdr *hdr;
  uint16_t hlen, total;
  char addr[IP_ADDR_STR_LEN];

  hdr = (struct ip_hdr *)buf;
  hlen = sizeof(*hdr);
  hdr->vhl = (IP_VERSION_IPV4 << 4) | (hlen >> 2);
  hdr->tos = 0;
  total = hlen + len;
  hdr->total = hton16(total);
  hdr->id = hton16(id);
  hdr->offset = hton16(offset);
  hdr->ttl = 0xff;
  hdr->protocol = protocol;
  hdr->sum = 0;
  hdr->src = src;
  hdr->dst = dst;
  hdr->sum = cksum16((uint16_t *)hdr, hlen, 0); /* don't convert bytoder */
  memcpy(hdr + 1, data, len);

  debugf("dev=%s, iface=%s, protocol=%u, len=%u", NET_IFACE(iface)->dev->name,
         ip_addr_ntop(dst, addr, sizeof(addr)), protocol, total);

  return ip_output_device(iface, buf, total, dst);
}

static uint16_t ip_generate_id(void) {
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  static uint16_t id = 128;
  uint16_t ret;

  pthread_mutex_lock(&mutex);
  ret = id++;
  pthread_mutex_unlock(&mutex);

  return ret;
}

ssize_t ip_output(uint8_t protocol, const uint8_t *data, size_t len,
                  ip_addr_t src, ip_addr_t dst) {
  struct ip_iface *iface;
  char addr[IP_ADDR_STR_LEN];
  uint16_t id;

  if (src == IP_ADDR_ANY) {
    errorf("routing does not implement");
    return -1;
  } else {
    iface = ip_iface_select(src);
    if (!iface) {
      errorf("iface not found, addr=%s", ip_addr_ntop(src, addr, sizeof(addr)));
      return -1;
    }
    if ((dst & iface->netmask) != (iface->unicast & iface->netmask) &&
        dst != IP_ADDR_BROADCAST) {
      errorf("not reached, addr=%s", ip_addr_ntop(src, addr, sizeof(addr)));
      return -1;
    }
  }
  if (NET_IFACE(iface)->dev->mtu < IP_HDR_SIZE_MIN + len) {
    errorf("too long, dev=%s, mtu=%u < %zu", NET_IFACE(iface)->dev->name,
           NET_IFACE(iface)->dev->mtu, IP_HDR_SIZE_MIN + len);
    return -1;
  }
  id = ip_generate_id();
  if (ip_output_core(iface, protocol, data, len, iface->unicast, dst, id, 0) ==
      -1) {
    errorf("ip_output_core() failure");
    return -1;
  }

  return len;
}

/* NOTE: must not be call after net_run() */
int ip_protocol_register(uint8_t type,
                         void (*handler)(const uint8_t *data, size_t len,
                                         ip_addr_t src, ip_addr_t dst,
                                         struct ip_iface *iface)) {
  struct ip_protocol *proto;

  for (proto = protocols; proto; proto = proto->next) {
    if (type == proto->type) {
      errorf("already registered, type=0x%04x", type);
      return -1;
    }
  }

  proto = calloc(1, sizeof(*proto));
  if (!proto) {
    errorf("calloc() failure");
    return -1;
  }

  proto->type = type;
  proto->handler = handler;
  proto->next = protocols;
  protocols = proto;

  infof("registered, type=0x%04x", type);

  return 0;
}

int ip_init(void) {
  if (net_protocol_register(NET_PROTOCOL_TYPE_IP, ip_input) == -1) {
    errorf("net_protocol_register() failure");
    return -1;
  }
  return 0;
}
