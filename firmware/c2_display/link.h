#pragma once

#include <stdint.h>

/**
 * USB CDC link (binary framed protocol, see docs/link-protocol.md).
 *
 * The receiver is a byte-wise state machine, so a corrupted or truncated frame
 * only costs one frame: the parser re-synchronises on the next magic word.
 */
void link_init(void);

/** Read pending bytes from USB CDC and dispatch complete frames (call every loop). */
void link_poll(void);
