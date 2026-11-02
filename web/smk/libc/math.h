#pragma once
#define sqrt __builtin_sqrt
#define sqrtf __builtin_sqrtf
#define fabs __builtin_fabs
#define fabsf __builtin_fabsf
#define floor __builtin_floor
#define ceil __builtin_ceil
#define floorf __builtin_floorf
double sin(double); double cos(double); double tan(double); double exp(double); double log(double); double log10(double);
double log2(double); double pow(double, double); double atan(double); double atan2(double, double); double fmod(double, double);
float sinf(float); float cosf(float); float expf(float); float logf(float); float powf(float, float); float tanhf(float); double tanh(double);
double round(double); float roundf(float); long lround(double);
#define M_PI 3.14159265358979323846
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
