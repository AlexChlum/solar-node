#ifndef SRC_OUTPUT_CONTROL_H_
#define SRC_OUTPUT_CONTROL_H_

#include <stdbool.h>

/* Returns 0 on success, negative errno on failure. */
int output_control_init(void);
int output_control_set(bool on);

#endif /* SRC_OUTPUT_CONTROL_H_ */
