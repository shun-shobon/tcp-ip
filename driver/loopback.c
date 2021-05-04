#include <stdint.h>
#include <stdio.h>

#include "net.h"
#include "util.h"

#define LOOPBACK_MTU UINT16_MAX

static int loopback_transmit(struct net_device *dev, uint16_t type,
                             const uint8_t *data, size_t len, const void *dst) {
  debugf("dev=%s, type=0x%04x, len=%zu", dev->name, type, len);
  debugdump(data, len);

  return net_input_handler(type, data, len, dev);
}

static struct net_device_ops loopback_ops = {
    .transmit = loopback_transmit,
};

struct net_device *loopback_init(void) {
  struct net_device *dev;

  dev = net_device_alloc();
  if (!dev) {
    errorf("net_device_alloc() failure");
    return NULL;
  }

  dev->type = NET_DEVICE_TYPE_LOOPBACK;
  dev->mtu = LOOPBACK_MTU;
  dev->hlen = 0;
  dev->alen = 0;
  dev->ops = &loopback_ops;

  if (net_device_register(dev) == -1) {
    errorf("net_device_register() failure");
    return NULL;
  }

  debugf("initialized, dev=%s", dev->name);

  return dev;
}
