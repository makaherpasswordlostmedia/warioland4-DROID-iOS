/* port override: agbcc register-allocation barriers are meaningless to clang. */
#ifndef COMPILER_H
#define COMPILER_H
#define COMPILER_FORGET_VALUE(value) ((void)0)
#define COMPILER_BARRIER(value) ((void)0)
#define COMPILER_BARRIER_MEMORY(value) ((void)0)
#define COMPILER_BARRIER_INPUT(value) ((void)0)
#define COMPILER_BARRIER2(first, second) ((void)0)
#define COMPILER_BARRIER4(first, second, third, fourth) ((void)0)
#define COMPILER_BARRIER5(first, second, third, fourth, fifth) ((void)0)
#endif
