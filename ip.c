#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>

#include "platform.h"

#include "util.h"
#include "net.h"
#include "ip.h"

struct ip_hdr
{
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
    uint8_t options[];
};

struct ip_protocol
{
    struct ip_protocol *next;
    uint8_t type;
    void (*handler)(const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, struct ip_iface *iface);
};

const ip_addr_t IP_ADDR_ANY = 0x00000000;
const ip_addr_t IP_ADDR_BROADCAST = 0xffffffff;

static struct ip_iface *ifaces;
static struct ip_protocol *protocols;

int ip_addr_pton(const char *p, ip_addr_t *n)
{
    char *sp, *ep;
    int idx;
    long ret;

    sp = (char *)p;
    for (idx = 0; idx < 4; idx++)
    {
        ret = strtol(sp, &ep, 10);
        if (ret < 0 || ret > 255)
        {
            return -1;
        }

        if (ep == sp)
        {
            return -1;
        }

        if ((idx == 3 && *ep != '\0') || (idx != 3 && *ep != '.'))
        {
            return -1;
        }

        ((uint8_t *)n)[idx] = ret;
        sp = ep + 1;
    }
    return 0;
}

char *
ip_addr_ntop(ip_addr_t n, char *p, size_t size)
{
    u_int8_t *u8;

    u8 = (u_int8_t *)&n;
    snprintf(p, size, "%d.%d.%d.%d", u8[0], u8[1], u8[2], u8[3]);
    return p;
}

void ip_dump(const uint8_t *data, size_t len)
{
    struct ip_hdr *hdr;
    uint8_t v, hl, hlen;
    uint16_t total, offset;
    char addr[IP_ADDR_STR_LEN];
    flockfile(stderr);
    hdr = (struct ip_hdr *)data;
    v = (hdr->vhl & 0xf0) >> 4;
    hl = hdr->vhl & 0x0f;
    hlen = hl << 2;
    fprintf(stderr, " vhl: 0x%02x [v: %u, hl: %u (%u)]\n", hdr->vhl, v, hl, hlen);
    fprintf(stderr, " tos: 0x%02x\n", hdr->tos);
    total = ntoh16(hdr->total);
    fprintf(stderr, " total: %u (payload: %u)\n", total, total - hlen);
    fprintf(stderr, " id: %u\n", ntoh16(hdr->id));
    offset = ntoh16(hdr->offset);
    fprintf(stderr, " offset: 0x%04x [flags=%x, offset=%u]\n", offset, (offset & 0xe000) >> 13, offset & 0x1fff);
    fprintf(stderr, " ttl: %u\n", hdr->ttl);
    fprintf(stderr, " protocol: %u\n", hdr->protocol);
    fprintf(stderr, " sum: 0x%04x\n", ntoh16(hdr->sum));
    fprintf(stderr, " src: %s\n", ip_addr_ntop(hdr->src, addr, sizeof(addr)));
    fprintf(stderr, " dst: %s\n", ip_addr_ntop(hdr->dst, addr, sizeof(addr)));
#ifdef HEXDUMP
    hexdump(stderr, data, len);
#endif
    funlockfile(stderr);
}

struct ip_iface *
ip_iface_alloc(const char *addr, const char *netmask)
{
    struct ip_iface *iface;
    // ip_addr_t *unicast, *netmas;

    iface = memory_alloc(sizeof(*iface));
    if (!iface)
    {
        errorf("memory_alloc() failure");
        return NULL;
    }

    NET_IFACE(iface)->family = NET_IFACE_FAMILY_IP;

    if (ip_addr_pton(addr, &iface->unicast) == -1)
    {
        errorf("ip_addr_pton() failure. unicast: %s", addr);
        memory_free(iface);
        return NULL;
    }

    if (ip_addr_pton(netmask, &iface->netmask) == -1)
    {
        errorf("ip_addr_pton() failure. netmask: %s", netmask);
        memory_free(iface);
        return NULL;
    }

    iface->broadcast = (iface->unicast & iface->netmask);
    iface->broadcast |= ~(iface->netmask);
    return iface;
}

int ip_iface_register(struct net_device *dev, struct ip_iface *iface)
{
    char addr1[IP_ADDR_STR_LEN];
    char addr2[IP_ADDR_STR_LEN];
    char addr3[IP_ADDR_STR_LEN];

    if (net_device_add_iface(dev, (struct net_iface *)iface) == -1)
    {
        errorf("net_device_add_iface failure. dev=%s", dev->name);
        return -1;
    }

    iface->next = ifaces;
    ifaces = iface;

    infof("registered: dev=%s, unicast=%s, netmask=%s, broadcast=%s", dev->name,
          ip_addr_ntop(iface->unicast, addr1, sizeof(addr1)),
          ip_addr_ntop(iface->netmask, addr2, sizeof(addr2)),
          ip_addr_ntop(iface->broadcast, addr3, sizeof(addr3)));
    return 0;
}

struct ip_iface *
ip_iface_select(ip_addr_t addr)
{
    struct ip_iface *iface;
    for (iface = ifaces; iface; iface = iface->next)
    {
        if (iface->unicast == addr)
        {
            return iface;
        }
    }

    return NULL;
}

int ip_protocol_register(uint8_t type, void (*handler)(const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, struct ip_iface *iface))
{
    struct ip_protocol *entry, *proto;

    for (proto = protocols; proto; proto = proto->next)
    {
        if (proto->type == type)
        {
            errorf("already registered, type=0x%04x", type);
            return -1;
        }
    }

    entry = memory_alloc(sizeof(*entry));
    if (!entry)
    {
        errorf("memory_alloc() failure");
        return -1;
    }

    entry->handler = handler;
    entry->type = type;
    entry->next = protocols;

    protocols = entry;

    infof("registered, type=%u", entry->type);
    return 0;
}

static void ip_input(const uint8_t *data, size_t len, struct net_device *dev)
{
    struct ip_hdr *hdr;
    uint8_t v, hl;
    uint16_t hlen, total, offset, sum;
    struct ip_iface *iface;
    char addr[IP_ADDR_STR_LEN];

    if (len < IP_HDR_SIZE_MIN)
    {
        errorf("too short");
        return;
    }

    hdr = (struct ip_hdr *)data;
    v = (hdr->vhl & 0xf0) >> 4;
    if (!(v & IP_VERSION_IPV4))
    {
        errorf("version %u not supported", v);
        return;
    }

    hl = hdr->vhl & 0x0f;
    hlen = hl << 2;
    if (hlen > len)
    {
        errorf("header length invalid. len: %zu, hlen: %u", len, hlen);
        return;
    }

    total = ntoh16(hdr->total);
    if (total > len)
    {
        errorf("total length invalid. len: %zu, total: %u", len, total);
        return;
    }

    sum = cksum16((uint16_t *)data, hlen, 0);
    if (sum & 0x1111)
    {
        errorf("checksum failed");
        return;
    }

    offset = ntoh16(hdr->offset);
    if (offset & 0x2000 || offset & 0x1fff)
    {
        errorf("fragments does not support");
        return;
    }

    iface = (struct ip_iface *)dev->ifaces;
    if (!(iface))
    {
        errorf("ip_iface not found");
        return;
    }

    if (!(hdr->dst & iface->unicast || (uint32_t)hdr->dst & 0xffffffff || hdr->dst & iface->broadcast))
    {
        return;
    }

    debugf("dev=%s, iface=%s, protocol=%u, total=%u", dev->name, ip_addr_ntop(iface->unicast, addr, sizeof(addr)), hdr->protocol, total);
    ip_dump(data, total);

    struct ip_protocol *proto;
    for (proto = protocols; proto; proto = proto->next)
    {
        if (proto->type == hdr->protocol)
        {
            proto->handler((uint8_t *)hdr + hlen, total - hlen, hdr->src, hdr->dst, iface);
            return;
        }
    }
}

static int
ip_output_device(struct ip_iface *iface, const uint8_t *data, size_t len, ip_addr_t dst)
{
    uint8_t hwaddr[NET_DEVICE_ADDR_LEN] = {};

    if (NET_IFACE(iface)->dev->flags & NET_DEVICE_FLAG_NEED_ARP)
    {
        if (dst == iface->broadcast || dst == IP_ADDR_BROADCAST)
        {
            memcpy(hwaddr, NET_IFACE(iface)->dev->broadcast, NET_IFACE(iface)->dev->alen);
        }
        else
        {
            errorf("arp does not implement");
            return -1;
        }
    }

    return net_device_output(NET_IFACE(iface)->dev, NET_PROTOCOL_TYPE_IP, data, len, hwaddr);
}

static ssize_t
ip_output_core(struct ip_iface *iface, uint8_t protocol, const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst, uint16_t id, uint16_t offset)
{
    uint8_t buf[IP_TOTAL_SIZE_MAX];
    struct ip_hdr *hdr;
    uint16_t hlen, total;
    char addr[IP_ADDR_STR_LEN];

    hdr = (struct ip_hdr *)buf;
    hlen = IP_HDR_SIZE_MIN; // sizeof(*hdr);
    debugf("len: %d, hlen: %u", len, hlen);
    total = len + hlen;
    hdr->vhl = IP_VERSION_IPV4 << 4 | hlen >> 2;
    hdr->tos = 0;
    hdr->total = hton16(total);
    hdr->id = hton16(id);
    hdr->offset = hton16(offset);
    hdr->ttl = 0xff;
    hdr->protocol = protocol;
    hdr->sum = 0;
    hdr->dst = dst;
    hdr->src = src;
    hdr->sum = cksum16((uint16_t *)hdr, hlen, 0);
    memcpy(hdr + 1, data, len);

    debugf("dev=%s, dst=%s, protocol=%u, len=%u",
           NET_IFACE(iface)->dev->name, ip_addr_ntop(dst, addr, sizeof(addr)), protocol, total);
    ip_dump(buf, total);
    return ip_output_device(iface, buf, total, dst);
}

static uint16_t
ip_generate_id(void)
{
    static mutex_t mutex = MUTEX_INITIALIZER;
    static uint16_t id = 128;
    uint16_t ret;

    mutex_lock(&mutex);
    ret = id++;
    mutex_unlock(&mutex);
    return ret;
}

ssize_t
ip_output(uint8_t protocol, const uint8_t *data, size_t len, ip_addr_t src, ip_addr_t dst)
{
    struct ip_iface *iface;
    char addr[IP_ADDR_STR_LEN];
    uint16_t id;

    if (src == IP_ADDR_ANY)
    {
        errorf("ip routing does not implement");
        return -1;
    }
    else
    {
        for (iface = ifaces; iface; iface = iface->next)
        {
            if (iface->unicast == src)
            {
                break;
            }
        }
        if (iface == NULL)
        {
            errorf("iface not found. src: %s", ip_addr_ntop(src, addr, sizeof(addr)));
            return -1;
        }

        if (!(dst & IP_ADDR_BROADCAST) && !((dst & iface->netmask) == (iface->unicast & iface->netmask)))
        {
            errorf("invalid dst. dst: %s", ip_addr_ntop(src, addr, sizeof(addr)));
            return -1;
        }
    }

    if (NET_IFACE(iface)->dev->mtu < IP_HDR_SIZE_MIN + len)
    {
        errorf("too long, dev=%s, mtu=%u < %zu", NET_IFACE(iface)->dev->name, NET_IFACE(iface)->dev->mtu, IP_HDR_SIZE_MIN + len);
        return -1;
    }
    id = ip_generate_id();
    if (ip_output_core(iface, protocol, data, len, iface->unicast, dst, id, 0) == -1)
    {
        errorf("ip_output_core() failure");
        return -1;
    }

    return len;
}

int ip_init(void)
{
    if (net_protocol_register(NET_PROTOCOL_TYPE_IP, ip_input) == -1)
    {
        errorf("net_protocol_register() failure");
        return -1;
    }
    return 0;
}