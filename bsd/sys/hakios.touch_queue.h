/* From danstronics/ios-s9plus-exynos core/drivers/input, now xnu/files/bsd/sys (MIT, "HakiOS authors"); see docs/star2lte-absorb.md. */
/* Called with the input queue locked. Preserve transitions; replace only a trailing MOVE. */
#ifndef HAKIOS_TOUCH_QUEUE_H
#define HAKIOS_TOUCH_QUEUE_H
#include <sys/hakios.input.h>

static inline struct hakios_touch *
hakios_touch_queue_slot(struct hakios_touch *queue, uint32_t capacity, uint32_t *head, uint32_t *tail,
    uint32_t type, uint32_t finger, uint32_t coalesce, uint32_t *replaced)
{
    if (coalesce && type == HAKIOS_TOUCH_MOVE && *head != *tail) {
        struct hakios_touch *last = &queue[(*tail - 1) % capacity];
        if (last->type == HAKIOS_TOUCH_MOVE && last->finger == finger) {
            (*replaced)++;
            return last;
        }
    }
    if (*tail - *head == capacity) (*head)++;
    return &queue[(*tail)++ % capacity];
}
#endif
