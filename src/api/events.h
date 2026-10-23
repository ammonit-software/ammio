#ifndef AMMIO_EVENTS_H
#define AMMIO_EVENTS_H

// Optional nng PUB socket that publishes one JSON message per change of a variable's value or
// quality, so clients can observe transitions shorter than their polling interval:
//
//   {"var_id":"door_open_cmd","value":1,"quality":"good","timestamp_ns":1761234567890123456}
//
// Messages start with {"var_id":"<id>" so an nng SUB socket can filter on that prefix.

int events_open(const char *endpoint);
void events_close(void);

#endif
