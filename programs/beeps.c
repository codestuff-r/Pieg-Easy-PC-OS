#include "libpieg.h"

#define MAX_NOTES 256
#define DEFAULT_NOTE_DURATION 200
#define POST_NOTE_DELAY 50

/*
 * Hardware-level audio control via the x86 PC Speaker
 * Uses PIT (Programmable Interval Timer) Ports 0x61, 0x43, and 0x42
 */
static void set_speaker_frequency(uint32_t frequency) {
    if (frequency == 0) {
        // Disable speaker output
        asm volatile ("inb $0x61, %%al; andb $0xfc, %%al; outb %%al, $0x61" ::: "eax");
        return;
    }

    // Calculate PIT divisor (Internal oscillator clock: 1193180 Hz)
    uint32_t divisor = 1193180 / frequency;

    // Configure PIT Channel 2 (Square wave generator mode)
    asm volatile ("movb $0xb6, %%al; outb %%al, $0x43" ::: "eax");
    asm volatile ("movb %%cl, %%al; outb %%al, $0x42" :: "c"(divisor & 0xff) : "eax");
    asm volatile ("movb %%ch, %%al; outb %%al, $0x42" :: "c"((divisor >> 8) & 0xff) : "eax");

    // Enable speaker gating
    asm volatile ("inb $0x61, %%al; orb $0x03, %%al; outb %%al, $0x61" ::: "eax");
}

/*
 * Active-wait delay loop calibrated for execution
 * context timing within the kernel environment
 */
static void precision_delay(uint32_t ms) {
    // Dynamic loop iteration bound adjustment (QEMU environment specific)
    volatile uint32_t iterations = ms * 500000;
    while (iterations > 0) {
        iterations--;
    }
}

static void emit_tone(uint32_t frequency, uint32_t duration_ms) {
    set_speaker_frequency(frequency);
    precision_delay(duration_ms);
    set_speaker_frequency(0);
    precision_delay(POST_NOTE_DELAY);
}

// C-Major diatonic scale frequencies (C4 through C5)
static const uint32_t DIATONIC_SCALE[] = {
    261, // 1: C4
    294, // 2: D4
    329, // 3: E4
    349, // 4: F4
    392, // 5: G4
    440, // 6: A4
    494, // 7: B4
    523  // 8: C5
};

// Application sequence buffer residing in the BSS segment to protect the 64 KiB kernel stack
static uint32_t dynamic_composition[MAX_NOTES];
static uint32_t sequence_length = 0;

int main(int argc, char **argv) {
    p_putln("=========================================");
    p_putln("        PIEG AUDIO COMPOSER V1.0        ");
    p_putln("=========================================");
    p_putln(" Controls:");
    p_putln("   [1 - 8]  : Step-input note entry (C4-C5)");
    p_putln("   [P]      : Playback sequence");
    p_putln("   [C]      : Clear volatile buffer");
    p_putln("   [Q]      : Terminate application");
    p_putln("-----------------------------------------");

    while (1) {
        char input_token = p_getc();

        // Note Entry Handler
        if (input_token >= '1' && input_token <= '8') {
            uint32_t pitch_index = input_token - '1';
            uint32_t selected_pitch = DIATONIC_SCALE[pitch_index];

            p_puts("Input Registered: Note ");
            p_putc(input_token);
            p_putln("");

            emit_tone(selected_pitch, DEFAULT_NOTE_DURATION);

            if (sequence_length < MAX_NOTES) {
                dynamic_composition[sequence_length++] = selected_pitch;
            } else {
                p_putln("Buffer Alert: Maximum sequence length reached.");
            }
        }
        // Playback Handler
        else if (input_token == 'p' || input_token == 'P') {
            if (sequence_length == 0) {
                p_putln("Playback Deferred: Sequence buffer is empty.");
                continue;
            }
            p_putln("Status: Executing playback sequence...");
            for (uint32_t i = 0; i < sequence_length; i++) {
                emit_tone(dynamic_composition[i], DEFAULT_NOTE_DURATION);
            }
            p_putln("Status: Playback completed successfully.");
        }
        // Buffer Clear Handler
        else if (input_token == 'c' || input_token == 'C') {
            sequence_length = 0;
            p_putln("Status: Sequence buffer cleared.");
        }
        // Exit Handler
        else if (input_token == 'q' || input_token == 'Q') {
            p_putln("Status: Shutting down. Exiting workspace context.");
            break;
        }
    }

    return 0;
}
