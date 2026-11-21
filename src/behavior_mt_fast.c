/*
 * Tap-first mod-tap behavior.
 *
 * Unlike the stock hold-tap flavors, the tap binding is pressed
 * immediately on key down, so the tap key's key-down reaches the host
 * without waiting for a tap/hold decision. If the key is still held
 * when the tapping term expires, the tap binding is released and the
 * hold binding is pressed instead. Releasing the key releases
 * whichever binding is currently active.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_mt_fast

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define MT_FAST_MAX_HELD 8
#define MT_FAST_POSITION_NOT_USED 9999

struct behavior_mt_fast_config {
    int32_t tapping_term_ms;
    const char *hold_behavior_dev;
    const char *tap_behavior_dev;
};

struct behavior_mt_fast_data {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    struct behavior_parameter_metadata_set set;
#endif
};

struct active_mt_fast {
    uint32_t position;
    int64_t timestamp;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    uint32_t param_hold;
    uint32_t param_tap;
    const struct behavior_mt_fast_config *config;
    bool hold_active;
    bool released;
    bool work_is_cancelled;
    struct k_work_delayable work;
};

static struct active_mt_fast active_items[MT_FAST_MAX_HELD] = {};

static struct active_mt_fast *find_mt_fast(uint32_t position) {
    for (int i = 0; i < MT_FAST_MAX_HELD; i++) {
        if (active_items[i].position == position) {
            return &active_items[i];
        }
    }
    return NULL;
}

static void clear_mt_fast(struct active_mt_fast *item) {
    item->position = MT_FAST_POSITION_NOT_USED;
    item->hold_active = false;
    item->released = false;
    item->work_is_cancelled = false;
}

static int invoke(const struct behavior_mt_fast_config *config, const char *behavior_dev,
                  uint32_t param, struct active_mt_fast *item, bool pressed) {
    struct zmk_behavior_binding_event event = {
        .position = item->position,
        .timestamp = item->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = item->source,
#endif
    };
    struct zmk_behavior_binding binding = {.behavior_dev = behavior_dev, .param1 = param};
    return zmk_behavior_invoke_binding(&binding, event, pressed);
}

static int press_tap(struct active_mt_fast *item) {
    return invoke(item->config, item->config->tap_behavior_dev, item->param_tap, item, true);
}

static int release_tap(struct active_mt_fast *item) {
    return invoke(item->config, item->config->tap_behavior_dev, item->param_tap, item, false);
}

static int press_hold(struct active_mt_fast *item) {
    return invoke(item->config, item->config->hold_behavior_dev, item->param_hold, item, true);
}

static int release_hold(struct active_mt_fast *item) {
    return invoke(item->config, item->config->hold_behavior_dev, item->param_hold, item, false);
}

void behavior_mt_fast_timer_work_handler(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    struct active_mt_fast *item = CONTAINER_OF(d_work, struct active_mt_fast, work);

    if (item->work_is_cancelled || item->released) {
        clear_mt_fast(item);
        return;
    }

    if (item->position == MT_FAST_POSITION_NOT_USED || item->hold_active) {
        return;
    }

    // Tapping term expired while still held: swap the tap for the hold.
    release_tap(item);
    press_hold(item);
    item->hold_active = true;
}

static int on_mt_fast_binding_pressed(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_mt_fast_config *cfg = dev->config;

    struct active_mt_fast *item = find_mt_fast(event.position);
    if (item == NULL) {
        for (int i = 0; i < MT_FAST_MAX_HELD; i++) {
            if (active_items[i].position == MT_FAST_POSITION_NOT_USED) {
                item = &active_items[i];
                break;
            }
        }
    }
    if (item == NULL) {
        LOG_ERR("unable to store mt-fast state, did you press more than %d mt-fast keys?",
                MT_FAST_MAX_HELD);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    k_work_cancel_delayable(&item->work);
    item->position = event.position;
    item->timestamp = event.timestamp;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    item->source = event.source;
#endif
    item->param_hold = binding->param1;
    item->param_tap = binding->param2;
    item->config = cfg;
    item->hold_active = false;
    item->released = false;
    item->work_is_cancelled = false;

    // The whole point of this behavior: the tap goes out right away.
    press_tap(item);

    k_work_schedule(&item->work, K_MSEC(cfg->tapping_term_ms));
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_mt_fast_binding_released(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    struct active_mt_fast *item = find_mt_fast(event.position);
    if (item == NULL) {
        LOG_ERR("mt-fast key released without a stored press (position %d)", event.position);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    item->released = true;
    if (k_work_cancel_delayable(&item->work)) {
        // The timer handler is already running; it will clear the entry.
        item->work_is_cancelled = true;
    }

    if (item->hold_active) {
        release_hold(item);
    } else {
        release_tap(item);
    }

    if (!item->work_is_cancelled) {
        clear_mt_fast(item);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static int mt_fast_parameter_metadata(const struct device *dev,
                                      struct behavior_parameter_metadata *param_metadata) {
    const struct behavior_mt_fast_config *cfg = dev->config;
    struct behavior_mt_fast_data *data = dev->data;
    int err;
    struct behavior_parameter_metadata child_meta;

    err = behavior_get_parameter_metadata(zmk_behavior_get_binding(cfg->hold_behavior_dev),
                                          &child_meta);
    if (err < 0) {
        LOG_WRN("Failed to get the hold behavior parameter: %d", err);
        return err;
    }

    if (child_meta.sets_len > 0) {
        data->set.param1_values = child_meta.sets[0].param1_values;
        data->set.param1_values_len = child_meta.sets[0].param1_values_len;
    }

    err = behavior_get_parameter_metadata(zmk_behavior_get_binding(cfg->tap_behavior_dev),
                                          &child_meta);
    if (err < 0) {
        LOG_WRN("Failed to get the tap behavior parameter: %d", err);
        return err;
    }

    if (child_meta.sets_len > 0) {
        data->set.param2_values = child_meta.sets[0].param1_values;
        data->set.param2_values_len = child_meta.sets[0].param1_values_len;
    }

    param_metadata->sets = &data->set;
    param_metadata->sets_len = 1;

    return 0;
}

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_driver_api behavior_mt_fast_driver_api = {
    .binding_pressed = on_mt_fast_binding_pressed,
    .binding_released = on_mt_fast_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = mt_fast_parameter_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

static int behavior_mt_fast_init(const struct device *dev) {
    static bool init_first_run = true;

    if (init_first_run) {
        for (int i = 0; i < MT_FAST_MAX_HELD; i++) {
            k_work_init_delayable(&active_items[i].work, behavior_mt_fast_timer_work_handler);
            active_items[i].position = MT_FAST_POSITION_NOT_USED;
        }
    }
    init_first_run = false;
    return 0;
}

#define MT_FAST_INST(n)                                                                            \
    static const struct behavior_mt_fast_config behavior_mt_fast_config_##n = {                    \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms),                                       \
        .hold_behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 0)),               \
        .tap_behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, 1)),                \
    };                                                                                             \
    static struct behavior_mt_fast_data behavior_mt_fast_data_##n = {};                            \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_mt_fast_init, NULL, &behavior_mt_fast_data_##n,            \
                            &behavior_mt_fast_config_##n, POST_KERNEL,                             \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_mt_fast_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MT_FAST_INST)

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
