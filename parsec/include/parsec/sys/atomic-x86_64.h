/*
 * Copyright (c) 2009-2016 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */

static inline void parsec_mfence(void)
{
    __asm__ __volatile__ ("mfence\n\t":::"memory");
}

static inline int parsec_atomic_cas_32b(volatile uint32_t* location,
                                       uint32_t old_value,
                                       uint32_t new_value)
{
    unsigned char ret;
    __asm__ __volatile__ (
                          "lock; cmpxchgl %3,%2   \n\t"
                          "      sete     %0      \n\t"
                          : "=qm" (ret), "+a" (old_value), "+m" (*location)
                          : "q"(new_value)
                          : "memory", "cc");

    return (int)ret;
}

static inline int parsec_atomic_bor_32b(volatile uint32_t* location,
                                       uint32_t value)
{
    uint32_t old_value;

    do {
        old_value = *location;
    } while( !parsec_atomic_cas_32b(location, old_value, (old_value|value) ));
    return old_value | value;
}

static inline int parsec_atomic_band_32b(volatile uint32_t* location,
                                        uint32_t value)
{
    uint32_t old_value;

    do {
        old_value = *location;
    } while( !parsec_atomic_cas_32b(location, old_value, (old_value&value) ));
    return old_value & value;
}

static inline int parsec_atomic_cas_64b(volatile uint64_t* location,
                                       uint64_t old_value,
                                       uint64_t new_value)
{
    unsigned char ret;
    __asm__ __volatile__ (
                          "lock; cmpxchgq %3,%2   \n\t"
                          "      sete     %0      \n\t"
                          : "=qm" (ret), "+a" (old_value), "+m" (*((volatile long*)location))
                          : "q"(new_value)
                          : "memory", "cc");

   return (int)ret;
}

#define PARSEC_ATOMIC_HAS_ATOMIC_INC_32B
static inline uint32_t parsec_atomic_inc_32b(volatile uint32_t *location)
{
    __asm__ __volatile__ (
                          "lock; incl %0\n"
                          : "+m" (*(location)));
    return (*location);
}

#define PARSEC_ATOMIC_HAS_ATOMIC_DEC_32B
static inline uint32_t parsec_atomic_dec_32b(volatile uint32_t *location)
{
    __asm__ __volatile__ (
                          "lock; decl %0\n"
                          : "+m" (*(location)));
    return (*location);
}

#define PARSEC_ATOMIC_HAS_ATOMIC_ADD_32B
static inline int32_t parsec_atomic_add_32(volatile int32_t* v, int32_t i)
{
    int ret = i;
   __asm__ __volatile__(
                        "lock; xaddl %1,%0"
                        :"+m" (*v), "+r" (ret)
                        :
                        :"memory", "cc");
   return (ret+i);
}

#define PARSEC_ATOMIC_HAS_ATOMIC_SUB_32B
static inline int32_t parsec_atomic_sub_32(volatile int32_t* v, int32_t i)
{
    int ret = -i;
   __asm__ __volatile__(
                        "lock; xaddl %1,%0"
                        :"+m" (*v), "+r" (ret)
                        :
                        :"memory", "cc");
   return (ret-i);
}

