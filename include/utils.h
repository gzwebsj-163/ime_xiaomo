#pragma once
#ifndef UTILS_H
#define UTILS_H
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <functional>
#include <math.h>
#include <cstring>
#ifdef __cplusplus
extern "C"
{
#endif
void normalize_vector(float *vec, int dim);
float vector_cosine_similarity(float* vec1, float* vec2, int dim);
char *str_right(const char *src, size_t n, char *dest, size_t dest_size);
#ifdef __cplusplus    
}
#endif
#endif //UTILS_H