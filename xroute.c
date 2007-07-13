/*
Copyright (c) 2007 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "babel.h"
#include "kernel.h"
#include "neighbour.h"
#include "route.h"
#include "xroute.h"
#include "util.h"

struct xroute xroutes[MAXXROUTES];
int numxroutes = 0;

struct xroute myxroutes[MAXMYXROUTES];
int nummyxroutes = 0;

int xroute_gc_delay = 180;
int xroute_hold_delay = 45;

static int
xroute_prefix(struct xroute *xroute, const unsigned char *prefix, int plen)
{
    return (xroute->plen == plen &&
            memcmp(xroute->prefix, prefix, 16) == 0);
}

static struct xroute *
find_installed_xroute(unsigned char *prefix, unsigned short plen)
{
    int i;
    for(i = 0; i < numxroutes; i++) {
        if(xroutes[i].installed && xroute_prefix(&xroutes[i], prefix, plen))
            return &xroutes[i];
    }
    return NULL;
}

static struct xroute *
find_installed_myxroute(unsigned char *prefix, unsigned short plen)
{
    int i;
    for(i = 0; i < nummyxroutes; i++) {
        if(myxroutes[i].installed && xroute_prefix(&myxroutes[i], prefix, plen))
            return &myxroutes[i];
    }
    return NULL;
}

static struct xroute *
find_best_xroute(unsigned char *prefix, unsigned short plen)
{
    struct xroute *xroute = NULL;
    struct route *route;
    int i;

    for(i = 0; i < numxroutes; i++) {
        if(!xroute_prefix(&xroutes[i], prefix, plen))
            continue;
        if(xroutes[i].metric >= INFINITY && xroutes[i].cost < INFINITY)
            continue;
        route = find_installed_route(xroutes[i].gateway);
        if(!route || route->nexthop != xroutes[i].nexthop)
            continue;
        if(!xroute || xroutes[i].metric < xroute->metric)
            xroute = &xroutes[i];
    }
    return xroute;
}

void
install_xroute(struct xroute *xroute)
{
    struct route *gwroute;
    struct xroute *installed;
    int rc;

    if(xroute->installed)
        return;

    if(xroute->metric >= INFINITY && xroute->cost < INFINITY)
        return;

    gwroute = find_installed_route(xroute->gateway);
    if(!gwroute || gwroute->nexthop != xroute->nexthop) {
        fprintf(stderr,
                "Attempted to install a blackhole xroute "
                "(this shouldn't happen).\n");
        return;
    }

    installed = find_installed_xroute(xroute->prefix, xroute->plen);
    if(installed)
        uninstall_xroute(installed);

    rc = kernel_route(ROUTE_ADD, xroute->prefix, xroute->plen,
                      gwroute->nexthop->address,
                      gwroute->nexthop->network->ifindex,
                      metric_to_kernel(xroute->metric), 0);
    if(rc < 0) {
        perror("kernel_route(ADD)");
        if(errno != EEXIST)
            return;
    }
    xroute->installed = 1;
}

void
uninstall_xroute(struct xroute *xroute)
{
    struct route *gwroute;
    int rc;

    if(!xroute->installed)
        return;

    gwroute = find_installed_route(xroute->gateway);
    if(!gwroute) {
        fprintf(stderr,
                "Attempted to uninstall a blackhole xroute "
                "(this shouldn't happen).\n");
        return;
    }

    rc = kernel_route(ROUTE_FLUSH, xroute->prefix, xroute->plen,
                      gwroute->nexthop->address,
                      gwroute->nexthop->network->ifindex,
                      metric_to_kernel(xroute->metric), 0);
    if(rc < 0)
        perror("kernel_route(FLUSH)");
    xroute->installed = 0;
}

void
consider_xroute(struct xroute *xroute)
{
    struct xroute *installed;
    struct route *route;

    if(xroute->installed)
        return;

    route = find_installed_route(xroute->gateway);

    if(xroute->nexthop != route->nexthop)
        return;

    update_xroute_metric(xroute, xroute->cost);

    installed = find_installed_myxroute(xroute->prefix, xroute->plen);
    if(!installed) {
        installed = find_installed_xroute(xroute->prefix, xroute->plen);
        if(!installed || installed->metric > xroute->metric + 64)
            install_xroute(xroute);
    }
}

void
consider_all_xroutes(struct route *route)
{
    int i;

    for(i = 0; i < numxroutes; i++) {
        if(xroutes[i].gateway == route->dest &&
           xroutes[i].nexthop == route->nexthop)
            consider_xroute(&xroutes[i]);
    }
}

void
flush_xroute(struct xroute *xroute)
{
    int n;
    int install = 0;
    unsigned char prefix[16];
    unsigned short plen = 0;

    n = xroute - xroutes;
    assert(n >= 0 && n < numxroutes);

    if(xroute->installed) {
        uninstall_xroute(xroute);
        memcpy(prefix, xroute->prefix, 16);
        plen = xroute->plen;
        install = 1;
    }

    if(n != numxroutes - 1)
        memcpy(xroutes + n, xroutes + numxroutes - 1,
               sizeof(struct xroute));
    numxroutes--;
    VALGRIND_MAKE_MEM_UNDEFINED(xroutes + numxroutes, sizeof(struct xroute));

    if(install) {
        struct xroute *xroute;
        xroute = find_best_xroute(prefix, plen);
        if(xroute)
            install_xroute(xroute);
    }
}

void
flush_neighbour_xroutes(struct neighbour *neigh)
{
    int i;

    i = 0;
    while(i < numxroutes) {
        if(xroutes[i].nexthop == neigh) {
            flush_xroute(xroutes + i);
            continue;
        }
        i++;
    }
}

void
retract_xroutes(struct destination *gateway, struct neighbour *nexthop,
                const struct xroute *except, int numexcept)
{
    int i, j;

    for(i = 0; i < numxroutes; i++) {
        if(xroutes[i].cost < INFINITY && xroutes[i].gateway == gateway &&
           xroutes[i].nexthop == nexthop) {
            for(j = 0; j < numexcept; j++) {
                if(xroute_prefix(&xroutes[i], except[j].prefix, except[j].plen))
                    goto skip;
            }
            update_xroute_metric(&xroutes[i], INFINITY);
        }
    skip: ;
    }
}

struct xroute *
update_xroute(const unsigned char *prefix, unsigned short plen,
              struct destination *gateway, struct neighbour *nexthop, int cost)
{
    int i;
    struct xroute *xroute = NULL;
    struct route *gwroute;

    if(martian_prefix(prefix, plen)) {
        fprintf(stderr, "Ignoring martian xroute.\n");
        return NULL;
    }

    if(gateway == NULL) {
        fprintf(stderr, "Ignoring xroute through unknown destination.\n");
        return NULL;
    }

    for(i = 0; i < numxroutes; i++) {
        xroute = &xroutes[i];
        if(xroute->gateway == gateway && xroute->nexthop == nexthop &&
           xroute_prefix(xroute, prefix, plen)) {
            update_xroute_metric(xroute, cost);
            xroute->time = now.tv_sec;
            return xroute;
        }
    }

    if(numxroutes >= MAXXROUTES) {
        fprintf(stderr, "Too many xroutes.\n");
        return NULL;
    }

    gwroute = find_installed_route(gateway);

    xroute = &xroutes[numxroutes];
    memcpy(&xroute->prefix, prefix, 16);
    xroute->plen = plen;
    xroute->gateway = gateway;
    xroute->nexthop = nexthop;
    xroute->cost = cost;
    xroute->metric =
        gwroute ? MIN(gwroute->metric + cost, INFINITY) : INFINITY;
    xroute->time = now.tv_sec;
    xroute->installed = 0;
    numxroutes++;

    if(gwroute)
        consider_xroute(xroute);
    return xroute;
}

void
update_xroute_metric(struct xroute *xroute, int cost)
{
    struct route *gwroute;
    int oldmetric, newmetric;
    int rc;

    gwroute = find_installed_route(xroute->gateway);

    oldmetric = xroute->metric;
    newmetric = gwroute ? MIN(gwroute->metric + cost, INFINITY) : INFINITY;

    if(xroute->cost != cost || oldmetric != newmetric) {
        xroute->cost = cost;
        if(xroute->installed) {
            if(gwroute == NULL) {
                fprintf(stderr, "Found installed blackhole xroute!.\n");
                return;
            }
            rc = kernel_route(ROUTE_MODIFY, xroute->prefix, xroute->plen,
                              gwroute->nexthop->address,
                              gwroute->nexthop->network->ifindex,
                              metric_to_kernel(oldmetric),
                              metric_to_kernel(newmetric));
            if(rc < 0) {
                perror("kernel_route(MODIFY)");
                return;
            }
        }
        xroute->metric = newmetric;
        if(newmetric > oldmetric) {
            struct xroute *best;
            best = find_best_xroute(xroute->prefix, xroute->plen);
            if(best)
                consider_xroute(best);
        }
    }
}
int
check_myxroutes()
{
    int i, j, n, change;
    struct kernel_route routes[120];

    debugf("Checking kernel routes.\n");

    n = -1;
    for(i = 0; i < nummyxroutes; i++)
        if(myxroutes[i].installed < 2)
            n = MAX(n, myxroutes[i].plen);

    if(n < 0)
        return 0;

    n = kernel_routes(n, routes, 120);
    if(n < 0)
        return -1;

    change = 0;
    for(i = 0; i < nummyxroutes; i++) {
        int installed;
        if(myxroutes[i].installed == 2)
            continue;
        installed = 0;
        for(j = 0; j < n; j++) {
            if(xroute_prefix(&myxroutes[i],
                             routes[j].prefix, routes[j].plen)) {
                installed = 1;
                break;
            }
        }
        if(myxroutes[i].installed != installed) {
            myxroutes[i].installed = installed;
            change = 1;
        }
    }
    return change;
}
