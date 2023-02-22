/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <assert.h>
#include <string.h>
#include "os/mynewt.h"
#include "bsp/bsp.h"
#include "console/console.h"

/* BLE */
#include "nimble/ble.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

/* Application-specified header. */
#include "blesync.h"

#define BLESYNC_MAIN_TASK_PRIO 0xf0

static int blesync_gap_event(struct ble_gap_event *event, void *arg);

/* Define stack, object and semaphore for central main task. */
#define BLESYNC_MAIN_TASK_STACK_SIZE (128)
static struct os_task blesync_main_task;
static os_stack_t blesync_main_task_stack[BLESYNC_MAIN_TASK_STACK_SIZE];
static void blesync_main_task_fn(void *arg);
static struct os_sem blesync_main_sem;

static int
blesync_sync_create(void)
{
    int rc;
    struct ble_gap_periodic_sync_params params = { 0 };
    ble_addr_t blepadv_addr = {
        BLE_ADDR_PUBLIC,
        {0x03, 0x00, 0x00, 0x1e, 0xbb, 0xba}
    };
    
    params.skip = 0;
    /* in units of 10 ms. Min 0x000A, max 0x4000 */
    params.sync_timeout = 0x4000;
    params.reports_disabled = 0;
    
    rc = ble_gap_periodic_adv_sync_create(&blepadv_addr, 0, &params,
                                          blesync_gap_event, NULL);

    console_printf("Sync to periodic advertising started, status %d\n", rc);

    return rc;
}

int
blesync_ext_scan(void)
{
    struct ble_gap_ext_disc_params params = {0};
    int rc;

    params.itvl = BLE_GAP_SCAN_FAST_INTERVAL_MAX;
    params.passive = 1;
    params.window = BLE_GAP_SCAN_FAST_WINDOW;

    rc = ble_gap_ext_disc(BLE_ADDR_PUBLIC, 0, 0, 0, BLE_HCI_SCAN_FILT_NO_WL,
                          0, &params, NULL, blesync_gap_event, NULL);

    console_printf("Extended scan started, status %d\n", rc);
    
    return rc;
}

static int
blesync_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_EXT_DISC:
        /* Stop scanning. */
        //rc = ble_gap_disc_cancel();
        //console_printf("Extended scan cancelled, status %d\n", rc);
        return 0;

    case BLE_GAP_EVENT_PERIODIC_SYNC:
        if (event->periodic_sync.status) {
            console_printf("Periodic Sync Establishment Failed; status=%u\n",
                           event->periodic_sync.status);
        } else {
            console_printf("Periodic Sync Established; sync_handle=%u sid=%u"
                           " phy=%u adv_interval=%u ca=%u addr_type=%u addr=",
                            event->periodic_sync.sync_handle,
                            event->periodic_sync.sid, event->periodic_sync.adv_phy,
                            event->periodic_sync.per_adv_ival,
                            event->periodic_sync.adv_clk_accuracy,
                            event->periodic_sync.adv_addr.type);
            print_addr(event->periodic_sync.adv_addr.val);
            console_printf("\n");
        }
        return 0;
        
    case BLE_GAP_EVENT_PERIODIC_REPORT:
        console_printf("BLE_GAP_EVENT_PERIODIC_REPORT\n");
        return 0;

    case BLE_GAP_EVENT_PERIODIC_SYNC_LOST:
        console_printf("Periodic Sync Lost, reason %d\n",
                       event->periodic_sync_lost.reason);
        return 0;
    
    default:
        console_printf("Event %d not handled\n", event->type);
        return 0;
    }
}

static void
blesync_on_reset(int reason)
{
    console_printf("Resetting state; reason=%d\n", reason);
}

static void
blesync_on_sync(void)
{
    int rc;

    /* Make sure we have proper identity address set (public preferred) */
    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* Create the BLE Central main task */
    rc = os_task_init(&blesync_main_task, "blesync_main_task",
                      blesync_main_task_fn, NULL, BLESYNC_MAIN_TASK_PRIO,
                      OS_WAIT_FOREVER, blesync_main_task_stack,
                      BLESYNC_MAIN_TASK_STACK_SIZE);

    assert(rc == 0);
}

static void
blesync_main_task_fn(void *arg)
{
    int rc;

    console_printf("BLE Sync Main Task welcomes you on-board\n");
    
    /* Init semaphore with 0 tokens. */
    rc = os_sem_init(&blesync_main_sem, 0);
    assert(rc == 0);

    /* Synchronize to periodic advertising from blepadv */
    rc = blesync_sync_create();
    assert(rc == 0);

    /* Start extended scanning (required to sync to periodic advertising) */
    rc = blesync_ext_scan();
    assert(rc == 0);

    /* Just a test... */
    os_sem_pend(&blesync_main_sem, OS_TICKS_PER_SEC/2);

    console_printf("Entering infinite loop\n");

    /* Task should never return */
    while (1) {
        /* Delay used only to prevent watchdog to reset the device. */
        os_time_delay(os_time_ms_to_ticks32(2000));
    }
}

/**
 * main
 *
 * All application logic and NimBLE host work is performed in default task.
 *
 * @return int NOTE: this function should never return!
 */
static int
main_fn(int argc, char **argv)
{
    /* Initialize OS */
    sysinit();

    console_printf("Hello, BLE periodic advertiser!\n");

    /* Configure the host. */
    ble_hs_cfg.reset_cb = blesync_on_reset;
    ble_hs_cfg.sync_cb = blesync_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* os start should never return. If it does, this should be an error */
    while (1) {
        os_eventq_run(os_eventq_dflt_get());
    }

    return 0;
}

int
main(int argc, char **argv)
{
#if BABBLESIM
    extern void bsim_init(int argc, char** argv, void *main_fn);
    bsim_init(argc, argv, main_fn);
#else
    main_fn(argc, argv);
#endif

    return 0;
}
