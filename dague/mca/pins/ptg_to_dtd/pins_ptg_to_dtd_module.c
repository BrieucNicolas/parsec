/*
 * Copyright (c) 2013-2015 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */

#include "dague_config.h"
#include "dague/mca/pins/pins.h"
#include "pins_ptg_to_dtd.h"
#include "dague/profiling.h"
#include "dague/scheduling.h"
#include "dague/utils/mca_param.h"
#include "dague/devices/device.h"
#include "dague/interfaces/superscalar/insert_function_internal.h"

#include <stdio.h>

/* Global list to push tasks in */
dague_list_t *dtd_global_deque;

/* Prototype of some of the static functions */
static void pins_handle_init_ptg_to_dtd(struct dague_handle_s * handle);
static void pins_handle_fini_ptg_to_dtd(struct dague_handle_s * handle);
static int fake_hook_for_testing(dague_execution_unit_t    *context,
                                 dague_execution_context_t *__this_task);

const dague_pins_module_t dague_pins_ptg_to_dtd_module = {
    &dague_pins_ptg_to_dtd_component,
    {
        NULL,
        NULL,
        pins_handle_init_ptg_to_dtd,
        pins_handle_fini_ptg_to_dtd,
        NULL,
        NULL
    }
};

/**
 * Make a copy of the original chores of the dague_handle_t and replace them with a
 * set of temporary hooks allowing to transition all tasks as DTD tasks. Only make
 * a copy of the officially exposed nb_function hooks, protecting all those that are
 * hidden (such as the initialization functions).
 */
static void
copy_chores(dague_handle_t *handle, dague_dtd_handle_t *dtd_handle)
{
    int i, j;
    for( i = 0; i < (int)handle->nb_functions; i++) {
        for( j = 0; NULL != handle->functions_array[i]->incarnations[j].hook; j++) {
            dague_hook_t **hook_not_const = (dague_hook_t **)&(handle->functions_array[i]->incarnations[j].hook);

            /* saving the CPU hook only */
            if (handle->functions_array[i]->incarnations[j].type == DAGUE_DEV_CPU) {
                dtd_handle->actual_hook[i].hook = handle->functions_array[i]->incarnations[j].hook;
            }
            /* copying the fake hook in all the hooks (CPU, GPU etc) */
            *hook_not_const = &fake_hook_for_testing;
        }
    }
}

static void pins_handle_init_ptg_to_dtd(dague_handle_t *handle)
{
    if(handle->destructor == (dague_destruct_fn_t)dague_dtd_handle_destruct) {
        return;
    }

    dague_dtd_init();
    dtd_global_deque = OBJ_NEW(dague_list_t);
    __dtd_handle = dague_dtd_handle_new(handle->context);
    copy_chores(handle, __dtd_handle);
    (void)dague_handle_enable((dague_handle_t *)__dtd_handle, NULL, NULL, NULL, handle->nb_local_tasks);
}

static void pins_handle_fini_ptg_to_dtd(dague_handle_t *handle)
{
    (void)handle;

    if(handle->destructor == (dague_destruct_fn_t)dague_dtd_handle_destruct) {
        return;
    }
}

/**
 * This function acts as the hook to connect the PaRSEC task with the actual task.
 * The function users passed while inserting task in PaRSEC is called in this procedure.
 * Called internally by the scheduler
 * Arguments:
 *   - the execution unit (dague_execution_unit_t *)
 *   - the PaRSEC task (dague_execution_context_t *)
 */
static int
testing_hook_of_dtd_task(dague_execution_unit_t    *context,
                         dague_execution_context_t *this_task)
{
    dague_dtd_task_t   *dtd_task   = (dague_dtd_task_t*)this_task;
    dague_execution_context_t *orig_task = dtd_task->orig_task;
    int rc = 0;

    DAGUE_TASK_PROF_TRACE(context->eu_profile,
                          this_task->dague_handle->profiling_array[2 * this_task->function->function_id],
                          this_task);

    /**
     * Check to see which interface, if it is the PTG inserting task in DTD then
     * this condition will be true
     */
    rc = dtd_task->fpointer(context, orig_task);
    if(rc == DAGUE_HOOK_RETURN_DONE) {
        /* Completing the orig task */
        __dague_complete_execution( context, orig_task );
    }

    return rc;
}

/* chores and dague_function_t structure initialization
 * This is for the ptg inserting task in dtd mode
 */
static const __dague_chore_t dtd_chore_for_testing[] = {
    {.type      = DAGUE_DEV_CPU,
     .evaluate  = NULL,
     .hook      = testing_hook_of_dtd_task },
    {.type      = DAGUE_DEV_NONE,
     .evaluate  = NULL,
     .hook      = NULL},             /* End marker */
};


/* Function to manage tiles once insert_task() is called, this functions is to
 * generate tasks from PTG and insert it using insert task interface.
 * This function checks if the tile structure(dague_dtd_tile_t) is created for the data
 * already or not.
 * Arguments:   - dague handle (dague_dtd_handle_t *)
                - data descriptor (dague_ddesc_t *)
                - key of this data (dague_data_key_t)
 * Returns:     - tile, creates one if not already created, and returns that
                  tile, (dague_dtd_tile_t *)
 */
dague_dtd_tile_t*
tile_manage_for_testing(dague_dtd_handle_t *dague_dtd_handle,
                        dague_ddesc_t *ddesc, dague_data_key_t key)
{
    dague_dtd_tile_t *tmp = dague_dtd_tile_find ( dague_dtd_handle, key,
                                                  ddesc );
    if( NULL == tmp) {
        dague_dtd_tile_t *temp_tile = (dague_dtd_tile_t *) dague_thread_mempool_allocate
                                                          (dague_dtd_handle->tile_mempool->thread_mempools);
        temp_tile->key                  = key;
        temp_tile->rank                 = 0;
        temp_tile->vp_id                = 0;
        temp_tile->data                 = (dague_data_t*)ddesc;
        temp_tile->data_copy            = temp_tile->data->device_copies[0];
        temp_tile->ddesc                = ddesc;
        temp_tile->last_user.flow_index = -1;
        temp_tile->last_user.op_type    = -1;
        temp_tile->last_user.task       = NULL;

        dague_dtd_tile_insert ( dague_dtd_handle, temp_tile->key,
                                temp_tile, ddesc );

        return temp_tile;
    } else {
        return tmp;
    }
}

/*
 * INSERT Task Function.
 * Each time the user calls it a task is created with the respective parameters the user has passed.
 * For each task class a structure known as "function" is created as well. (e.g. for Cholesky 4 function
 structures are created for each task class).
 * The flow of data from each task to others and all other dependencies are tracked from this function.
 */
void
insert_task_generic_fptr_for_testing(dague_dtd_handle_t *__dague_handle,
                                     dague_dtd_funcptr_t* fpointer, dague_execution_context_t *orig_task,
                                     char* name, dague_dtd_task_param_t *head_paramm)
{
    dague_dtd_task_param_t *current_paramm;
    int next_arg=-1, i, flow_index = 0;
    int tile_op_type;
#if defined(DAGUE_PROF_TRACE)
    int track_function_created_or_not = 0;
#endif
    dague_dtd_task_param_t *head_of_param_list, *current_param, *tmp_param;
    void *tmp, *value_block, *current_val;
    static int vpid = 0;

    /* Creating master function structures */
    /* Hash table lookup to check if the function structure exists or not */
    dague_function_t *function = (dague_function_t *)dague_dtd_function_find
                                                    ( __dague_handle, fpointer );

    if( NULL == function ) {
        /* calculating the size of parameters for each task class*/
        int count_of_params = 0;
        long unsigned int size_of_param = 0;

        current_paramm = head_paramm;

        while(current_paramm != NULL) {
            count_of_params++;
            current_paramm = current_paramm->next;
        }

        if (dump_function_info) {
            printf("Function Created for task Class: %s\n Has %d parameters\n Total Size: %lu\n", name, count_of_params, size_of_param);
        }

        function = create_function(__dague_handle, fpointer, name, count_of_params, size_of_param, count_of_params);
        __dague_chore_t **incarnations = (__dague_chore_t **)&(function->incarnations);
        *incarnations                   = (__dague_chore_t *)dtd_chore_for_testing;
#if defined(DAGUE_PROF_TRACE)
        track_function_created_or_not = 1;
#endif
    }

    dague_mempool_t * context_mempool_in_function = ((dague_dtd_function_t*) function)->context_mempool;

    dague_dtd_tile_t *tile;
    dague_dtd_task_t *temp_task;

    /* Creating Task object */
    temp_task = (dague_dtd_task_t *) dague_thread_mempool_allocate(context_mempool_in_function->thread_mempools);

    for(i=0;i<MAX_DESC;i++) {
        temp_task->desc[i].task           = NULL;
        temp_task->dont_skip_releasing_data[i] = 0;
    }

    temp_task->orig_task = orig_task;
    temp_task->super.dague_handle = (dague_handle_t*)__dague_handle;
    temp_task->belongs_to_function = function->function_id;
    temp_task->super.function = __dague_handle->super.functions_array[(temp_task->belongs_to_function)];
    temp_task->flow_satisfied = 0;
    temp_task->ready_mask = 0;
    temp_task->super.super.key = __dague_handle->task_id;
    temp_task->flow_count = temp_task->super.function->nb_flows+1; /* +1 to make sure the task is completely ready before it gets executed */
    temp_task->fpointer = fpointer;
    temp_task->super.priority = 0;
    temp_task->super.chore_id = 0;
    temp_task->super.status = DAGUE_TASK_STATUS_NONE;
    temp_task->super.unused[0] = 0;

    head_of_param_list = (dague_dtd_task_param_t *) (((char *)temp_task) + sizeof(dague_dtd_task_t)); /* Getting the pointer allocated from mempool */
    current_param = head_of_param_list;
    value_block = ((char *)head_of_param_list) + ((dague_dtd_function_t*)function)->count_of_params * sizeof(dague_dtd_task_param_t);
    current_val = value_block;

    current_paramm = head_paramm;

    while(current_paramm != NULL) {
        tmp = current_paramm->pointer_to_tile;
        tile = (dague_dtd_tile_t *) tmp;
        tile_op_type = current_paramm->operation_type;
        current_param->tile_type_index = REGION_FULL;

        set_task(temp_task, tmp, tile,
                 tile_op_type, current_param,
                 __dague_handle->flow_set_flag, &current_val,
                 __dague_handle, &flow_index, &next_arg);

        tmp_param = current_param;
        current_param = current_param + 1;
        tmp_param->next = current_param;

        current_paramm = current_paramm->next;
    }

    /* Bypassing constness in function structure */
    dague_flow_t **in = (dague_flow_t **)&(__dague_handle->super.functions_array[temp_task->belongs_to_function]->in[flow_index]);
    *in = NULL;
    dague_flow_t **out = (dague_flow_t **)&(__dague_handle->super.functions_array[temp_task->belongs_to_function]->out[flow_index]);
    *out = NULL;
    __dague_handle->flow_set_flag[temp_task->belongs_to_function] = 1;

    /* Assigning values to task objects  */
    temp_task->param_list = head_of_param_list;

    /* Atomically increasing the nb_local_tasks_counter */
    dague_atomic_add_32b((int *)&(__dague_handle->super.nb_local_tasks),1);
    dague_atomic_add_32b((int *)&(temp_task->flow_satisfied),1); /* in attempt to make the task not ready till the whole body is constructed */

    if(!__dague_handle->super.context->active_objects) {
        assert(0);
        __dague_handle->task_id++;
        /* executing the tasks as soon as we find it if no engine is attached */
        __dague_execute(__dague_handle->super.context->virtual_processes[0]->execution_units[0],
                        (dague_execution_context_t *)temp_task);
        return;
    }

    /* Building list of initial ready task */
    if(temp_task->flow_count == temp_task->flow_satisfied) {
        DAGUE_LIST_ITEM_SINGLETON(temp_task);
        if (NULL != __dague_handle->startup_list[vpid]) {
            dague_list_item_ring_merge((dague_list_item_t *)temp_task,
                                       (dague_list_item_t *) (__dague_handle->startup_list[vpid]));
        }
        __dague_handle->startup_list[vpid] = (dague_execution_context_t*)temp_task;
        vpid = (vpid+1)%__dague_handle->super.context->nb_vp;
    }

#if defined(DAGUE_PROF_TRACE)
    if(track_function_created_or_not) {
        add_profiling_info(__dague_handle, function, name, flow_index);
        track_function_created_or_not = 0;
    }
#endif /* defined(DAGUE_PROF_TRACE) */

    /* task_insert_h_t(__dague_handle->task_h_table, task_id, temp_task, __dague_handle->task_h_size); */
    dague_atomic_add_32b((int *)&(__dague_handle->task_id),1);

    if((__dague_handle->task_id % __dague_handle->task_window_size) == 0 ) {
        schedule_tasks (__dague_handle);
        /*if ( __dague_handle->task_window_size <= window_size ) {
            __dague_handle->task_window_size *= 2;
        }*/
    }
}

/* To copy the dague_context_t of the predecessor needed for tracking control flow
 *
 */
static dague_ontask_iterate_t copy_content(dague_execution_unit_t *eu,
                const dague_execution_context_t *newcontext,
                const dague_execution_context_t *oldcontext,
                const dep_t *dep, dague_dep_data_description_t *data,
                int src_rank, int dst_rank, int dst_vpid, void *param)
{
    (void)eu; (void)newcontext; (void)oldcontext; (void)dep; (void)data; (void)src_rank;
    (void)dst_rank; (void)dst_vpid; (void)param;
    /* assinging 1 to "unused" field in dague_context_t of the successor to indicate we found a predecesor */
    uint8_t *val = (uint8_t *) &(oldcontext->unused[0]);
    *val += 1;

    /* Saving the flow index of the parent in the "unused" field of the predecessor */
    uint8_t *val1 = (uint8_t *) &(newcontext->unused[0]);
    dague_flow_t* parent_outflow = (dague_flow_t*)(dep->flow);
    *val1 = parent_outflow->flow_index;

    memcpy(param, newcontext, sizeof(dague_execution_context_t));
    return DAGUE_ITERATE_STOP;
}

static int
fake_hook_for_testing(dague_execution_unit_t    *context,
                      dague_execution_context_t *this_task)
{
    /* We will try to push our tasks in the same Global Deque.
     * Then we will try to take ownership of the Global Deque and try to pull the tasks from there and build a list.
     */
    /* push task in the global dequeue */
    dague_list_push_back( dtd_global_deque, (dague_list_item_t*)this_task );

    /* try to get ownership of global deque*/
    if( !dague_atomic_trylock(&dtd_global_deque->atomic_lock) )
        return DAGUE_HOOK_RETURN_ASYNC;
    /* Extract all the elements in the queue and then release the queue as fast as possible */
    dague_list_item_t* local_list = dague_list_nolock_unchain(dtd_global_deque);
    dague_atomic_unlock(&dtd_global_deque->atomic_lock);

    /* Successful in ataining the lock, now we will pop all the tasks out of the list and put it
     * in our local list.
     */
    dague_execution_context_t T1;
    for( this_task = (dague_execution_context_t*)local_list;
         NULL != this_task;
         this_task = (dague_execution_context_t*)local_list ) {

        int i, tmp_op_type;
        dague_dtd_handle_t *dtd_handle = __dtd_handle;
        dague_dtd_task_param_t *head_param = NULL, *current_param = NULL, *tmp_param = NULL;
        dague_ddesc_t *ddesc;
        dague_data_key_t key;
        data_repo_entry_t *entry;

        local_list = dague_list_item_ring_chop(local_list);

        for (i=0; this_task->function->in[i] != NULL ; i++) {

            dague_data_copy_t* copy;
            tmp_op_type = this_task->function->in[i]->flow_flags;
            int op_type, pred_found = 0;

            if ((tmp_op_type & FLOW_ACCESS_RW) == FLOW_ACCESS_RW) {
                op_type = INOUT | REGION_FULL;
            } else if((tmp_op_type & FLOW_ACCESS_READ) == FLOW_ACCESS_READ) {
                op_type = INPUT | REGION_FULL;
            } else if((tmp_op_type & FLOW_ACCESS_WRITE) == FLOW_ACCESS_WRITE) {
                op_type = OUTPUT | REGION_FULL;
            } else if((tmp_op_type) == FLOW_ACCESS_NONE || tmp_op_type == FLOW_HAS_IN_DEPS) {
                op_type = INOUT | REGION_FULL;

                this_task->unused[0] = 0;
                this_task->function->iterate_predecessors(context, this_task,  1 << i, copy_content, (void*)&T1);
                if (this_task->unused[0] != 0) {
                    pred_found = 1;
                    uint64_t id = T1.function->key(T1.dague_handle, T1.locals);
                    entry = data_repo_lookup_entry(T1.dague_handle->repo_array[T1.function->function_id], id);
                    copy = entry->data[T1.unused[0]];
                    ddesc = (dague_ddesc_t *)copy->original;
                    key   =  copy->original->key;
                } else {
                    continue;  /* next IN flow */
                }

            } else {
                continue;  /* next IN flow */
            }

            if (pred_found == 0) {
                ddesc = (dague_ddesc_t *)this_task->data[i].data_in->original;
                key = this_task->data[i].data_in->original->key;
                OBJ_RETAIN(this_task->data[i].data_in);
            }
            dague_dtd_tile_t *tile = tile_manage_for_testing(dtd_handle, ddesc, key);

            tmp_param = (dague_dtd_task_param_t *) malloc(sizeof(dague_dtd_task_param_t));
            tmp_param->pointer_to_tile = (void *)tile;
            tmp_param->operation_type = op_type;
            tmp_param->tile_type_index = REGION_FULL;
            tmp_param->next = NULL;

            if(head_param == NULL) {
                head_param = tmp_param;
            } else {
                current_param->next = tmp_param;
            }
            current_param = tmp_param;
        }

        for( i = 0; NULL != this_task->function->out[i]; i++) {
            int op_type;
            tmp_op_type = this_task->function->out[i]->flow_flags;
            if((tmp_op_type & FLOW_ACCESS_WRITE) == FLOW_ACCESS_WRITE) {
                op_type = OUTPUT | REGION_FULL;
                ddesc = (dague_ddesc_t *)this_task->data[i].data_out->original;
                key = this_task->data[i].data_out->original->key;
                OBJ_RETAIN(this_task->data[i].data_out);
            } else if((tmp_op_type) == FLOW_ACCESS_NONE || tmp_op_type == FLOW_HAS_IN_DEPS) {
                op_type = INOUT | REGION_FULL;

                dague_data_t *fake_data = OBJ_NEW(dague_data_t);
                fake_data->key = rand();
                dague_data_copy_t *copy = OBJ_NEW(dague_data_copy_t);
                copy->original = fake_data;
                this_task->data[this_task->function->out[i]->flow_index].data_out = copy;

                ddesc = (dague_ddesc_t *)fake_data;
                key   =  fake_data->key;
            } else {
                continue;
            }

            dague_dtd_tile_t *tile = tile_manage_for_testing(dtd_handle, ddesc, key);

            tmp_param = (dague_dtd_task_param_t *) malloc(sizeof(dague_dtd_task_param_t));
            tmp_param->pointer_to_tile = (void *)tile;
            tmp_param->operation_type = op_type;
            tmp_param->tile_type_index = REGION_FULL;
            tmp_param->next = NULL;

            if(head_param == NULL) {
                head_param = tmp_param;
            } else {
                current_param->next = tmp_param;
            }
            current_param = tmp_param;
        }

        insert_task_generic_fptr_for_testing(dtd_handle, __dtd_handle->actual_hook[this_task->function->function_id].hook,
                                             this_task, (char *)this_task->function->name, head_param);
    }

    return DAGUE_HOOK_RETURN_ASYNC;
}
