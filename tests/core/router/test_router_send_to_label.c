#include "router.c"

#include "mock_daemon.h"
#include "mock_log.h"
#include "mock_timer.h"
#include "tests_utils.h"
#include "tests_router_utils.h"

int
main(void)
{
    lgtd_lifx_wire_setup();

    struct lgtd_lifx_gateway *gw_1 = lgtd_tests_insert_mock_gateway(1);
    struct lgtd_lifx_bulb *bulb_1 = lgtd_tests_insert_mock_bulb(gw_1, 1);
    struct lgtd_lifx_gateway *gw_2 = lgtd_tests_insert_mock_gateway(2);
    struct lgtd_lifx_bulb *bulb_2 = lgtd_tests_insert_mock_bulb(gw_2, 2);
    struct lgtd_lifx_bulb *bulb_3 = lgtd_tests_insert_mock_bulb(gw_2, 3);

    const char *label = "feed";
    strcpy(bulb_1->state.label, label);
    strcpy(bulb_3->state.label, label);
    strcpy(bulb_2->state.label, "trololo");

    struct lgtd_lifx_packet_power_state payload = {
        .power = LGTD_LIFX_POWER_ON
    };
    struct lgtd_proto_target_list *targets;
    targets = lgtd_tests_build_target_list(label, NULL);
    lgtd_router_send(targets, LGTD_LIFX_SET_POWER_STATE, &payload);

    if (lgtd_tests_gw_pkt_queue_size != 2) {
        lgtd_errx(1, "2 packet should have been sent");
    }

    for (int i = 0; i != lgtd_tests_gw_pkt_queue_size; i++) {
        struct lgtd_lifx_gateway *recpt_gw = lgtd_tests_gw_pkt_queue[0].gw;
        struct lgtd_lifx_packet_header *hdr_queued = lgtd_tests_gw_pkt_queue[0].hdr;
        const void *pkt_queued = lgtd_tests_gw_pkt_queue[0].pkt;
        int pkt_size = lgtd_tests_gw_pkt_queue[0].pkt_size;


    int expected_flags = LGTD_LIFX_ADDRESSABLE|LGTD_LIFX_RES_REQUIRED;
        if (!lgtd_tests_lifx_header_has_flags(hdr_queued, expected_flags)) {
            lgtd_errx(1, "the packet header doesn't have the right protocol flags");
        }
        if (pkt_queued != &payload) {
            lgtd_errx(1, "invalid payload");
        }
        if (pkt_size != sizeof(payload)) {
            lgtd_errx(
                1, "unexpected pkt size %d (expected %ju)",
                pkt_size, (uintmax_t)sizeof(payload)
            );
        }

        if (recpt_gw == gw_1) {
            if (memcmp(hdr_queued->target.device_addr, bulb_1->addr, sizeof(bulb_1->addr))) {
                lgtd_errx(1, "the packet header doesn't have the right target address");
            }
            if (memcmp(gw_1->site.as_array, hdr_queued->site, sizeof(hdr_queued->site))) {
                lgtd_errx(1, "incorrect site in the headers");
            }
        } else if (recpt_gw == gw_2) {
            if (memcmp(hdr_queued->target.device_addr, bulb_3->addr, sizeof(bulb_3->addr))) {
                lgtd_errx(1, "the packet header doesn't have the right target address");
            }
            if (memcmp(gw_2->site.as_array, hdr_queued->site, sizeof(hdr_queued->site))) {
                lgtd_errx(1, "incorrect site in the headers");
            }
        }
    }

    return 0;
}
