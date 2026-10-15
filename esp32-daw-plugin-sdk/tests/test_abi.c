/* SPDX-License-Identifier: MIT
 * The public headers compile as C11 and have the documented, padding-free layout. */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "daw_plugin_api.h"

_Static_assert(sizeof(daw_event_t) == 32, "daw_event_t is 32 bytes");
_Static_assert(offsetof(daw_event_t, u) == 8, "event payload at offset 8");
_Static_assert(sizeof(((daw_event_t *)0)->u) == 24, "event payload is 24 bytes");
_Static_assert(DAW_PLUGIN_API_MAJOR == 1, "ABI major 1");
/* 1.2: the chase field shares the slide field's place; the layout is that of 1.1 */
_Static_assert(offsetof(daw_event_t, u.note.chase_mbeats) == offsetof(daw_event_t, u.note.slide_ms), "chase aliases slide");
_Static_assert(offsetof(daw_event_t, u.note.slide_ms) == 28, "slide_ms at offset 28");

int main(void)
{
    printf("PASS: test_abi (event %zu B, descriptor %zu B, param %zu B, process %zu B)\n", sizeof(daw_event_t),
           sizeof(daw_plugin_descriptor_t), sizeof(daw_param_desc_t), sizeof(daw_process_t));
    return 0;
}
