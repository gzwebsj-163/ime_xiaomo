#include "./include/utils.h"
char *str_right(const char *src, size_t n, char *dest, size_t dest_size) {
    if (src == NULL || dest == NULL || dest_size == 0) {
        fprintf(stderr, "str_right 参数错误\n");
        return NULL;
    }
    size_t src_len = strlen(src);
    size_t take_len = (n >= src_len) ? src_len : n;
    const char *start = src + (src_len - take_len);
    size_t copy_len = (take_len < dest_size - 1) ? take_len : (dest_size - 1);
    strncpy(dest, start, copy_len);
    dest[copy_len] = '\0';
    return dest;
}
void normalize_vector(float *vec, int dim)
{
    if (vec == nullptr || dim <= 0)
        return;
    float sum = 0.0f;
    float max_val = 0.0f;
    for (int i = 0; i < dim; i++)
    {
        sum += fabs(vec[i]);
        if (fabs(vec[i]) > max_val)
            max_val = fabs(vec[i]);
    }
    if (sum < 1e-6 && max_val < 1e-6)
        return;
    if (sum > 1e-6)
    {
        for (int i = 0; i < dim; i++)
            vec[i] /= sum;
    }
    else
    {
        for (int i = 0; i < dim; i++)
            vec[i] /= max_val;
    }
}

float vector_cosine_similarity(float* vec1, float* vec2, int dim)
{
    if(vec1 == nullptr || vec2 == nullptr || dim <= 0) return 0.0f;
    float dot_product = 0.0f;
    float norm1 = 0.0f;
    float norm2 = 0.0f;
    for(int i=0; i<dim; i++)
    {
        dot_product += vec1[i] * vec2[i];
        norm1 += vec1[i] * vec1[i];
        norm2 += vec2[i] * vec2[i];
    }
    if(norm1 < 1e-6 || norm2 < 1e-6) return 0.0f;
    return dot_product / (sqrt(norm1) * sqrt(norm2));
}