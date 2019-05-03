/*
 * Copyright (c) 2018-2019 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */

#include "parsec/data_dist/matrix/two_dim_rectangle_cyclic.h"
#include "parsec/data_dist/matrix/matrix.h"
#include "parsec/runtime.h"
#include "parsec/data.h"
#include <assert.h>

#if defined(PARSEC_HAVE_MPI)
#include <mpi.h>
#endif

/* New structure */
typedef struct two_dim_block_cyclic_band_s {
    two_dim_block_cyclic_t super;
    two_dim_block_cyclic_t band;
    unsigned int band_size;     /** Number of band rows = 2 * band_size - 1 */ 
} two_dim_block_cyclic_band_t;

/* New rank_of, rank_of_key for two dim block cyclic band */
uint32_t twoDBC_band_rank_of(parsec_data_collection_t * desc, ...);
uint32_t twoDBC_band_rank_of_key(parsec_data_collection_t *desc, parsec_data_key_t key);

/* New data_of, data_of_key for two dim block cyclic band */
parsec_data_t* twoDBC_band_data_of(parsec_data_collection_t *desc, ...);
parsec_data_t* twoDBC_band_data_of_key(parsec_data_collection_t *desc, parsec_data_key_t key);

int twoDBC_band_get_rank(two_dim_block_cyclic_t *dc, unsigned int m, unsigned int n);
void twoDBC_band_offset(two_dim_block_cyclic_t *dc, unsigned int *m, unsigned int *n);