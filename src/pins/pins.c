/* PaRSEC Performance Instrumentation Callback System */
#include <stdlib.h>
#include "pins.h"
#include "debug.h"

static int registration_disabled;

parsec_pins_callback * pins_array[A_COUNT_NOT_A_FLAG] = { 0 };

void empty_callback(dague_execution_unit_t * exec_unit, dague_execution_context_t * task, void * data);

void parsec_pins(PINS_FLAG method_flag, 
                 dague_execution_unit_t * exec_unit,
                 dague_execution_context_t * task, 
                 void * data) {
    (*(pins_array[method_flag]))(exec_unit, task, data);
}

void pins_disable_registration(int disable) {
	if (disable)
		DEBUG3(("PINS registration is disabled.\n"));
	else
		DEBUG3(("PINS registration is enabled.\n"));
	registration_disabled = disable;
}

/**
 The behavior of the PaRSEC PINS system is undefined if 
 pins_register_callback is not called at least once before 
 any call to parsec_pins.
 */
parsec_pins_callback * pins_register_callback(PINS_FLAG method_flag, parsec_pins_callback * cb) {
    if (!pins_array[0]) {
        int i = 0;
        for (; i < A_COUNT_NOT_A_FLAG; i++) {
            if (pins_array[i] == NULL)
                pins_array[i] = &empty_callback;
        }
        DEBUG(("Initialized PaRSEC PINS callbacks to empty_callback()"));
    }
    assert(cb != NULL);
    if (method_flag >= 0 && method_flag < A_COUNT_NOT_A_FLAG) {
	    if (registration_disabled) {
		    DEBUG3(("NOTE: PINS has been disabled by command line argument, causing this registration to fail."));
		    return NULL;
	    }
	    else {
		    parsec_pins_callback * prev = pins_array[method_flag];
		    pins_array[method_flag] = cb;
		    return prev;
	    }
    }
    else {
        DEBUG(("WARNING: Attempted to use the invalid flag %d with PaRSEC Performance Instrumentation!\n", method_flag));
    }
    return NULL;
}

parsec_pins_callback * pins_unregister_callback(PINS_FLAG method_flag) {
    if (method_flag >= PARSEC_SCHEDULED && method_flag < A_COUNT_NOT_A_FLAG) {
	    if (registration_disabled) {
		    DEBUG3(("NOTE: PINS has been disabled by command line argument, causing this UN-registration to fail."));
		    return NULL;
	    }
	    else {
		    parsec_pins_callback * prev = pins_array[method_flag];
		    pins_array[method_flag] = &empty_callback;
		    return prev;
	    }
    }
    else {
        DEBUG(("WARNING: Attempted to use the invalid flag %d with PaRSEC Performance Instrumentation!\n", method_flag));
    }
    return NULL;
}

void empty_callback(dague_execution_unit_t * exec_unit, dague_execution_context_t * task, void * data) {
    // do nothing
    (void) exec_unit;
    (void) task;
    (void) data;
}

