/* Register before SDK event processing starts; payload pointers are transient. */
#include "rbf_api.h"
#include "rbf_magnetic.h"
#include "rbf_magnetic2.h"
#include <stdio.h>
static int old_input(uint8_t no, rbf_magnetic_input_status_t *s)
{
    printf("MC %u door=%u tamper=%u\n", no, s->alarm, s->tamper);
    return 0;
}
static int new_input(uint8_t no, rbf_magnetic2_input_status_t *s)
{
    printf("MC2 %u door=%u tamper=%u antimasking=%u angle=%u shock=%u external=%u uncalibrated=%u\n",
           no, s->alarm, s->tamper, s->antimasking, s->angle_alarm,
           s->shock_alarm, s->external_alarm, s->uncalibrated);
    return 0;
}
int magnetic2_example_register(void)
{
    rbf_magnetic_callbacks_t old = {0};
    rbf_magnetic2_callbacks_t newer = {0};
    old.input_status_cb = old_input;
    newer.input_status_cb = new_input;
    return rbf_magnetic_register_callbacks(&old) || rbf_magnetic2_register_callbacks(&newer);
}

/* Enrollment type selects the configuration API; ID/route lists contain cat/no only. */
int magnetic2_example_is_new(const RBF_register_response_t *response)
{
    return response && response->type == RBF_DEV_TYPE_MAGNETIC2;
}
