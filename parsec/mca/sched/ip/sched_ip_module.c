/**
 * Copyright (c) 2013-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "parsec/parsec_config.h"
#include "parsec/parsec_internal.h"
#include "parsec/utils/debug.h"
#include "parsec/mca/sched/sched.h"
#include "parsec/mca/sched/ip/sched_ip.h"
#include "parsec/class/dequeue.h"
#include "parsec/mca/pins/pins.h"

/**
 * Module functions
 */
static int sched_ip_install(parsec_context_t* master);
static int sched_ip_schedule(parsec_execution_stream_t* es,
                             parsec_task_t* new_context,
                             int32_t distance);
static parsec_task_t* sched_ip_select(parsec_execution_stream_t *es,
                                                   int32_t* distance);
static int flow_ip_init(parsec_execution_stream_t* es, struct parsec_barrier_t* barrier);
static void sched_ip_remove(parsec_context_t* master);
static void sched_ip_register_sde(parsec_execution_stream_t *es);

const parsec_sched_module_t parsec_sched_ip_module = {
    &parsec_sched_ip_component,
    {
        sched_ip_install,
        flow_ip_init,
        sched_ip_schedule,
        sched_ip_select,
        NULL,
        sched_ip_register_sde,
        sched_ip_remove
    }
};

typedef struct {
    parsec_list_t *list;
    int            local_counter;
} shared_list_with_local_counter_t;
#define LOCAL_SCHED_OBJECT(eu_context) ((shared_list_with_local_counter_t*)(eu_context)->scheduler_object)

static long long int parsec_shared_list_length( parsec_vp_t *vp )
{
    int thid;
    long long int sum = 0;

    for(thid = 0; thid < vp->nb_cores; thid++) {
        sum += LOCAL_SCHED_OBJECT(vp->execution_streams[thid])->local_counter;
    }
    return sum;
}

static int sched_ip_install( parsec_context_t *master )
{
    (void)master;
    return 0;
}

static void sched_ip_register_sde( parsec_execution_stream_t *es )
{
    char event_name[256];
    if( NULL != es && 0 == es->th_id ) {
        snprintf(event_name, 256, "PARSEC::SCHEDULER::PENDING_TASKS::QUEUE=%d::SCHED=IP", es->virtual_process->vp_id);
        papi_sde_register_fp_counter(parsec_papi_sde_handle, event_name, PAPI_SDE_RO|PAPI_SDE_INSTANT,
                                     PAPI_SDE_int, (papi_sde_fptr_t)parsec_shared_list_length, es->virtual_process);
        papi_sde_add_counter_to_group(parsec_papi_sde_handle, event_name,
                                      "PARSEC::SCHEDULER::PENDING_TASKS", PAPI_SDE_SUM);
        papi_sde_add_counter_to_group(parsec_papi_sde_handle, event_name,
                                      "PARSEC::SCHEDULER::PENDING_TASKS::SCHED=IP", PAPI_SDE_SUM);
    }
    if( NULL == es || 0 == es->th_id ) {
        papi_sde_describe_counter(parsec_papi_sde_handle, "PARSEC::SCHEDULER::PENDING_TASKS::SCHED=IP",
                                  "the number of pending tasks for the IP scheduler");
        papi_sde_describe_counter(parsec_papi_sde_handle, "PARSEC::SCHEDULER::PENDING_TASKS::QUEUE=<VPID>::SCHED=IP",
                                  "the number of pending tasks for the IP scheduler on virtual process <VPID>");
    }
}

static int flow_ip_init(parsec_execution_stream_t* es, struct parsec_barrier_t* barrier)
{
    parsec_vp_t *vp = es->virtual_process;

    shared_list_with_local_counter_t *sl = (shared_list_with_local_counter_t*)calloc(sizeof(shared_list_with_local_counter_t), 1);
    es->scheduler_object = sl;
    if (es == vp->execution_streams[0])
        sl->list = OBJ_NEW(parsec_list_t);
    
    parsec_barrier_wait(barrier);

    sl->list = LOCAL_SCHED_OBJECT(vp->execution_streams[0])->list;
    sched_ip_register_sde( es );
    
    return 0;
}

static parsec_task_t*
sched_ip_select(parsec_execution_stream_t *es,
                int32_t* distance)
{
    shared_list_with_local_counter_t *sl = LOCAL_SCHED_OBJECT(es);
    parsec_task_t * context =
        (parsec_task_t*)parsec_list_pop_back(sl->list);
    if(NULL != context)
        sl->local_counter--;
    *distance = 0;
    return context;
}

static int sched_ip_schedule(parsec_execution_stream_t* es,
                             parsec_task_t* new_context,
                             int32_t distance)
{
    int len = 0;
    shared_list_with_local_counter_t *sl = LOCAL_SCHED_OBJECT(es);
#if defined(PARSEC_DEBUG_NOISIER)
    parsec_list_item_t *it = (parsec_list_item_t*)new_context;
    char tmp[MAX_TASK_STRLEN];
    do {
        PARSEC_DEBUG_VERBOSE(20, parsec_debug_output, "IP:\t Pushing task %s",
                parsec_task_snprintf(tmp, MAX_TASK_STRLEN, (parsec_task_t*)it));
        it = (parsec_list_item_t*)((parsec_list_item_t*)it)->list_next;
    } while( it != (parsec_list_item_t*)new_context );
#endif
    _LIST_ITEM_ITERATOR(new_context, &new_context->super, item, {len++; });
    if( 0 == distance ) {
        parsec_list_chain_sorted(sl->list,
                                 (parsec_list_item_t*)new_context,
                                 parsec_execution_context_priority_comparator);
    } else {
        parsec_list_chain_back(sl->list,
                               (parsec_list_item_t*)new_context);
    }
    sl->local_counter += len;
    return 0;
}

static void sched_ip_remove( parsec_context_t *master )
{
    int p, t;
    parsec_vp_t *vp;
    parsec_execution_stream_t *es;
    shared_list_with_local_counter_t *sl;
    char event_name[256];

    for(p = 0; p < master->nb_vp; p++) {
        vp = master->virtual_processes[p];
        for(t = 0; t < vp->nb_cores; t++) {
            es = vp->execution_streams[t];
            sl = LOCAL_SCHED_OBJECT(es);
            if( es->th_id == 0 ) {
                OBJ_DESTRUCT( sl->list );
                free(sl->list);
            }
            free(sl);
            es->scheduler_object = NULL;
        }
        snprintf(event_name, 256, "PARSEC::SCHEDULER::PENDING_TASKS::QUEUE=%d::SCHED=IP", p);
        papi_sde_unregister_counter(parsec_papi_sde_handle, event_name);
    }
    papi_sde_unregister_counter(parsec_papi_sde_handle, "PARSEC::SCHEDULER::PENDING_TASKS::SCHED=IP");
}
