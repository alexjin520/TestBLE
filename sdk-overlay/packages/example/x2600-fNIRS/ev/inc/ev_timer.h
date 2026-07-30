#ifndef EV_TIMER_H_
#define EV_TIMER_H_

void ev_timer_arm(void);
void ev_timer_note(const char *msg);
void ev_timer_note_unsafe(const char *msg);

#endif
