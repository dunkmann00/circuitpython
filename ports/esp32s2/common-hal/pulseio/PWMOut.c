/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Lucian Copeland for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <math.h>

#include "common-hal/pulseio/PWMOut.h"
#include "shared-bindings/pulseio/PWMOut.h"
#include "py/runtime.h"
#include "driver/ledc.h"

#define INDEX_EMPTY 0xFF

STATIC bool not_first_reset = false;
STATIC uint32_t reserved_timer_freq[LEDC_TIMER_MAX];
STATIC uint8_t reserved_channels[LEDC_CHANNEL_MAX];
STATIC bool never_reset_tim[LEDC_TIMER_MAX];
STATIC bool never_reset_chan[LEDC_CHANNEL_MAX];

void pwmout_reset(void) {
    for (size_t i = 0; i < LEDC_CHANNEL_MAX; i++ ) {
        if (reserved_channels[i] != INDEX_EMPTY && not_first_reset) {
            ledc_stop(LEDC_LOW_SPEED_MODE, i, 0);
        }
        if (!never_reset_chan[i]) {
            reserved_channels[i] = INDEX_EMPTY;
        }
    }
    for (size_t i = 0; i < LEDC_TIMER_MAX; i++ ) {
        if (reserved_timer_freq[i]) {
            ledc_timer_rst(LEDC_LOW_SPEED_MODE, i);
        }
        if (!never_reset_tim[i]) {
            reserved_timer_freq[i] = 0;
        }
    }
    not_first_reset = true;
}

pwmout_result_t common_hal_pulseio_pwmout_construct(pulseio_pwmout_obj_t* self,
                                                    const mcu_pin_obj_t* pin,
                                                    uint16_t duty,
                                                    uint32_t frequency,
                                                    bool variable_frequency) {
    // Calculate duty cycle
    uint32_t duty_bits = 0;
    uint32_t interval = LEDC_APB_CLK_HZ/frequency;
    for (size_t i = 0; i < 32; i++) {
        if(!(interval >> i)) {
            duty_bits = i - 1;
            break;
        }
    }
    if (duty_bits < 1) {
        mp_raise_ValueError(translate("Invalid frequency"));
    } else if (duty_bits >= LEDC_TIMER_14_BIT) {
        duty_bits = LEDC_TIMER_13_BIT;
    }

    // Find a viable timer
    size_t timer_index = INDEX_EMPTY;
    size_t channel_index = INDEX_EMPTY;
    for (size_t i = 0; i < LEDC_TIMER_MAX; i++) {
        if ((reserved_timer_freq[i] == frequency) && !variable_frequency) {
            //prioritize matched frequencies so we don't needlessly take slots
            timer_index = i;
            break;
        } else if (reserved_timer_freq[i] == 0) {
            timer_index = i;
            break;
        }
    }
    if (timer_index == INDEX_EMPTY) {
        // Running out of timers isn't pin related on ESP32S2 so we can't re-use error messages
        mp_raise_ValueError(translate("No more timers available"));
    }

    // Find a viable channel
    for (size_t i = 0; i < LEDC_CHANNEL_MAX; i++) {
        if (reserved_channels[i] == INDEX_EMPTY) {
            channel_index = i;
            break;
        }
    }
    if (channel_index == INDEX_EMPTY) {
        mp_raise_ValueError(translate("No more channels available"));
    }

    // Run configuration
    self->tim_handle.timer_num = timer_index;
    self->tim_handle.duty_resolution = duty_bits;
    self->tim_handle.freq_hz = frequency;
    self->tim_handle.speed_mode = LEDC_LOW_SPEED_MODE;
    self->tim_handle.clk_cfg = LEDC_AUTO_CLK;

    if (ledc_timer_config(&(self->tim_handle)) != ESP_OK) {
        mp_raise_ValueError(translate("Could not initialize timer"));
    }

    self->chan_handle.channel = channel_index;
    self->chan_handle.duty = duty >> (16 - duty_bits);
    self->chan_handle.gpio_num = pin->number;
    self->chan_handle.speed_mode = LEDC_LOW_SPEED_MODE; // Only LS is allowed on ESP32-S2
    self->chan_handle.hpoint = 0;
    self->chan_handle.timer_sel = timer_index;

    if (ledc_channel_config(&(self->chan_handle))) {
        mp_raise_ValueError(translate("Could not initialize channel"));
    }

    // Make reservations
    reserved_timer_freq[timer_index] = frequency;
    reserved_channels[channel_index] = timer_index;

    self->variable_frequency = variable_frequency;
    self->pin_number = pin->number;
    self->deinited = false;
    self->duty_resolution = duty_bits;
    claim_pin(pin);

    // Set initial duty
    common_hal_pulseio_pwmout_set_duty_cycle(self, duty);

    return PWMOUT_OK;
}

void common_hal_pulseio_pwmout_never_reset(pulseio_pwmout_obj_t *self) {
    never_reset_tim[self->tim_handle.timer_num] = true;
    never_reset_chan[self->chan_handle.channel] = true;
}

void common_hal_pulseio_pwmout_reset_ok(pulseio_pwmout_obj_t *self) {
    never_reset_tim[self->tim_handle.timer_num] = false;
    never_reset_chan[self->chan_handle.channel] = false;
}

bool common_hal_pulseio_pwmout_deinited(pulseio_pwmout_obj_t* self) {
    return self->deinited == true;
}

void common_hal_pulseio_pwmout_deinit(pulseio_pwmout_obj_t* self) {
    if (common_hal_pulseio_pwmout_deinited(self)) {
        return;
    }

    if (reserved_channels[self->chan_handle.channel] != INDEX_EMPTY) {
        ledc_stop(LEDC_LOW_SPEED_MODE, self->chan_handle.channel, 0);
    }
    // Search if any other channel is using the timer
    bool taken = false;
    for (size_t i =0; i < LEDC_CHANNEL_MAX; i++) {
        if (reserved_channels[i] == self->tim_handle.timer_num) {
            taken = true;
        }
    }
    // Variable frequency means there's only one channel on the timer
    if (!taken || self->variable_frequency) {
        if (reserved_timer_freq[self->tim_handle.timer_num] != 0) {
            ledc_timer_rst(LEDC_LOW_SPEED_MODE, self->tim_handle.timer_num);
        }
        reserved_timer_freq[self->tim_handle.timer_num] = 0;
    }
    reset_pin_number(self->pin_number);
    reserved_channels[self->chan_handle.channel] = INDEX_EMPTY;
    self->deinited = true;
}

void common_hal_pulseio_pwmout_set_duty_cycle(pulseio_pwmout_obj_t* self, uint16_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, self->chan_handle.channel, duty >> (16 - self->duty_resolution));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, self->chan_handle.channel);
}

uint16_t common_hal_pulseio_pwmout_get_duty_cycle(pulseio_pwmout_obj_t* self) {
    return ledc_get_duty(LEDC_LOW_SPEED_MODE, self->chan_handle.channel) << (16 - self->duty_resolution);
}

void common_hal_pulseio_pwmout_set_frequency(pulseio_pwmout_obj_t* self, uint32_t frequency) {
    ledc_set_freq(LEDC_LOW_SPEED_MODE, self->tim_handle.timer_num, frequency);
}

uint32_t common_hal_pulseio_pwmout_get_frequency(pulseio_pwmout_obj_t* self) {
    return ledc_get_freq(LEDC_LOW_SPEED_MODE, self->tim_handle.timer_num);
}

bool common_hal_pulseio_pwmout_get_variable_frequency(pulseio_pwmout_obj_t* self) {
    return self->variable_frequency;
}
