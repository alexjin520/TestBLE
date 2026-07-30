#ifndef EV_CAN_H_
#define EV_CAN_H_

void ev_can_try_attach(void);
void ev_can_hub_service(void);
void ev_can_sample_arm(void);
void ev_can_sample_arm_deferred(void);
void ev_can_sample_disarm(void);
void ev_can_ota_arm(void);
void ev_can_ota_arm_deferred(void);
void ev_can_ota_disarm(void);
void ev_can_factory_arm(void);
void ev_can_factory_arm_deferred(void);
void ev_can_factory_disarm(void);

#endif
