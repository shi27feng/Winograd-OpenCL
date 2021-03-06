#include <stdio.h>
#include <stdlib.h>
#include <CL/cl.h>
#include <math.h>
#include <time.h>
#include "timer.h"
#include <clBLAS.h>

#define CHECK_ERROR(err) \
    if (err != CL_SUCCESS) { \
        printf("[%s:%d] OpenCL error %d\n", __FILE__, __LINE__, err); \
        exit(EXIT_FAILURE); \
    }

cl_int err;

cl_program create_and_build_program(cl_context context, cl_device_id device, const char *file_name) {
    FILE *file = fopen(file_name, "rb");
    if (file == NULL) {
        printf("Failed to open %s\n", file_name);
        exit(EXIT_FAILURE);
    }
    fseek(file, 0, SEEK_END);
    size_t source_size = ftell(file);
    rewind(file);
    char *source_code = (char*)malloc(source_size + 1);
    fread(source_code, sizeof(char), source_size, file);
    source_code[source_size] = '\0';
    fclose(file);

    cl_program program = clCreateProgramWithSource(context, 1, (const char**)&source_code, &source_size, &err);
    CHECK_ERROR(err);
    free(source_code);

    err = clBuildProgram(program, 1, &device, "", NULL, NULL);
    {
        size_t log_size;
        CHECK_ERROR(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size));
        char *log = (char*)malloc(log_size + 1);
        CHECK_ERROR(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL));
        log[log_size] = 0;
        printf("Build log :\n%s\n", log);
        free(log);
    }
    CHECK_ERROR(err);

    return program;
}

int _ceil(int x, int y) {
    return (x + y - 1) / y * y;
}

int _ceil_div(int x, int y) {
    return (x + y - 1) / y;
}

void fillData(float *d, int n) {
    for (int i = 0; i < n; ++i) {
        d[i] = rand() % 2 / 1.0;
    }
}

void printData(float *d, int N, int C, int H, int W, const char *name) {
    printf("%s.shape = (%d, %d, %d, %d)\n", name, N, C, H, W);
    for (int n = 0; n < N; ++n) {
        for (int c = 0; c < C; ++c) {
            printf("(%d, %d, :, :) =\n", n, c);
            for (int h = 0; h < H; ++h) {
                for (int w = 0; w < W; ++w) {
                    printf("%f ", d[((n * C + c) * H + h) * W + w]);
                }
                printf("\n");
            }
        }
    }
}

// true if equal, false otherwise
int equalData(float *d0, float *d1, int N, int C, int H, int W) {
    for (int n = 0; n < N; ++n) {
        for (int c = 0; c < C; ++c) {
            for (int h = 0; h < H; ++h) {
                for (int w = 0; w < W; ++w) {
                    int x = ((n * C + c) * H + h) * W + w;
                    if ((d0[x] + d1[x] != 0 && fabs((d0[x] - d1[x]) / (d0[x] + d1[x])) > 1e-4) 
                            || (d0[x] + d1[x] == 0 && d0[x] != 0)) {
                        printf("d0 = %f, d1 = %f\n", d0[x], d1[x]);
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

void convolution_cpu(float *inputs, float *outputs, float *filters, float *dx, float *dw, int N, int C, int H, int W, int K, int P, int Q, int R, int S, int pad) {
    timer_start(0);
    for (int n = 0; n < N; ++n) {
        for (int k = 0; k < K; ++k) {
            for (int p = 0; p < P; ++p) {
                for (int q = 0; q < Q; ++q) {
                    float x = 0;
                    for (int c = 0; c < C; ++c) {
                        for (int r = 0; r < R; ++r) {
                            for (int s = 0; s < S; ++s) {
                                int h = p + r - pad, w = q + s - pad;
                                if (0 <= h && h < H && 0 <= w && w < W) {
                                    x += inputs[((n * C + c) * H + h) * W + w] * filters[((k * C + c) * R + r) * S + s];
                                }
                            }
                        }
                    }
                    outputs[((n * K + k) * P + p) * Q + q] = x;
                }
            }
        }
    }
    timer_end(0, "cpu fwd");

    timer_start(0);
    for (int n = 0; n < N; ++n) {
        for (int c = 0; c < C; ++c) {
            for (int h = 0; h < H; ++h) {
                for (int w = 0; w < W; ++w) {
                    float x = 0;
                    for (int k = 0; k < K; ++k) {
                        for (int r = 0; r < R; ++r) {
                            for (int s = 0; s < S; ++s) {
                                int p = h - r + pad, q = w - s + pad;
                                if (0 <= p && p < P && 0 <= q && q < Q) {
                                    x += outputs[((n * K + k) * P + p) * Q + q] * filters[((k * C + c) * R + r) * S + s];
                                }
                            }
                        }
                    }
                    dx[((n * C + c) * H + h) * W + w] = x;
                }
            }
        }
    }
    timer_end(0, "cpu bwd data");

    timer_start(0);
    for (int k = 0; k < K; ++k) {
        for (int c = 0; c < C; ++c) {
            for (int r = 0; r < R; ++r) {
                for (int s = 0; s < S; ++s) {
                    float x = 0;
                    for (int n = 0; n < N; ++n) {
                        for (int p = 0; p < P; ++p) {
                            for (int q = 0; q < Q; ++q) {
                                int h = p + r - pad, w = q + s - pad;
                                if (0 <= h && h < H && 0 <= w && w < W) {
                                    x += outputs[((n * K + k) * P + p) * Q + q] * inputs[((n * C + c) * H + h) * W + w];
                                }
                            }
                        }
                    }
                    dw[((k * C + c) * R + r) * S + s] = x;
                }
            }
        }
    }
    timer_end(0, "cpu bwd filter");
}

#define CONV_BK 16
#define CONV_BKH 8
#define CONV_BIJ 64
#define CONV_LXY 16
#define CONV_RXY 4

void convolution_mc(float *inputs, float *outputs, float *filters, float *bias, int N, int C, int H, int W, int K, int P, int Q, int R, int S, int pad, cl_context context, cl_command_queue queue, cl_program program) {
    cl_kernel kernel0 = clCreateKernel(program, "conv_preA", &err);
    CHECK_ERROR(err);
    cl_kernel kernel1 = clCreateKernel(program, "conv_postC", &err);
    CHECK_ERROR(err);
    cl_kernel kernel2 = clCreateKernel(program, "conv", &err);
    CHECK_ERROR(err);

    int IU = K, KU = C * 9, JU = H * W;
    int IA = _ceil(IU, CONV_BIJ), KA = _ceil(KU, CONV_BK), JA = _ceil(JU, CONV_BIJ);
    int KK = KA / CONV_BK;

    cl_mem filters_pre_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_dev = clCreateBuffer(context, 0, sizeof(float) * (KA * IA), NULL, &err);
    CHECK_ERROR(err);
    cl_mem inputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * C * H * W), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_dev = clCreateBuffer(context, 0, sizeof(float) * (IA * JA), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_post_dev = clCreateBuffer(context, 0, sizeof(float) * (N * K * P * Q), NULL, &err);
    CHECK_ERROR(err);
    cl_mem bias_dev = clCreateBuffer(context, 0, sizeof(float) * (K), NULL, &err);
    CHECK_ERROR(err);

    CHECK_ERROR(clEnqueueWriteBuffer(queue, inputs_dev, CL_TRUE, 0, sizeof(float) * (N * C * H * W), inputs, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, filters_pre_dev, CL_TRUE, 0, sizeof(float) * (K * C * R * S), filters, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, bias_dev, CL_TRUE, 0, sizeof(float) * (K), bias, 0, NULL, NULL));

    {
        CHECK_ERROR(clSetKernelArg(kernel0, 0, sizeof(cl_mem), &filters_pre_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 1, sizeof(cl_mem), &filters_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 2, sizeof(int), &KU));
        CHECK_ERROR(clSetKernelArg(kernel0, 3, sizeof(int), &IA));
        CHECK_ERROR(clSetKernelArg(kernel0, 4, sizeof(int), &KA));
        size_t gws[] = {IA * KA};
        size_t lws[] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel0, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel2, 0, sizeof(cl_mem), &filters_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 1, sizeof(cl_mem), &inputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 2, sizeof(cl_mem), &outputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 3, sizeof(cl_mem), &bias_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 4, sizeof(int), &KK));
        CHECK_ERROR(clSetKernelArg(kernel2, 5, sizeof(int), &IA));
        CHECK_ERROR(clSetKernelArg(kernel2, 6, sizeof(int), &JA));
        CHECK_ERROR(clSetKernelArg(kernel2, 7, sizeof(int), &KA));
        CHECK_ERROR(clSetKernelArg(kernel2, 8, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel2, 9, sizeof(int), &H)); // assume H == W
        size_t gws[] = {JA / CONV_RXY, IA / CONV_RXY, N};
        size_t lws[] = {CONV_LXY, CONV_LXY, 1};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel2, 3, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "mc");
    }

    {
        CHECK_ERROR(clSetKernelArg(kernel1, 0, sizeof(cl_mem), &outputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 1, sizeof(cl_mem), &outputs_post_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 2, sizeof(int), &IA));
        CHECK_ERROR(clSetKernelArg(kernel1, 3, sizeof(int), &JA));
        CHECK_ERROR(clSetKernelArg(kernel1, 4, sizeof(int), &IU));
        CHECK_ERROR(clSetKernelArg(kernel1, 5, sizeof(int), &JU));
        size_t gws[] = {JA, IA, N};
        size_t lws[] = {CONV_BIJ, 256 / CONV_BIJ, 1};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel1, 3, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
    }

    CHECK_ERROR(clEnqueueReadBuffer(queue, outputs_post_dev, CL_TRUE, 0, sizeof(float) * (N * K * P * Q), outputs, 0, NULL, NULL));

    clReleaseMemObject(inputs_dev);
    clReleaseMemObject(outputs_dev);
    clReleaseMemObject(outputs_post_dev);
    clReleaseMemObject(filters_dev);
    clReleaseMemObject(filters_pre_dev);
    clReleaseMemObject(bias_dev);

    clReleaseKernel(kernel0);
    clReleaseKernel(kernel1);
    clReleaseKernel(kernel2);
}

void convolution_wino16(float *inputs, float *outputs, float *filters, float *bias, int N, int C, int H, int W, int K, int P, int Q, int R, int S, int pad, cl_context context, cl_command_queue queue, cl_program program) {
    cl_kernel kernel0 = clCreateKernel(program, "NCHW2CHWN", &err);
    CHECK_ERROR(err);
    cl_kernel kernel1 = clCreateKernel(program, "winograd_2x2_3x3_16x16", &err);
    CHECK_ERROR(err);
    cl_kernel kernel2 = clCreateKernel(program, "CHWN2NCHW", &err);
    CHECK_ERROR(err);

    cl_mem inputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * C * H * W), NULL, &err);
    CHECK_ERROR(err);
    cl_mem inputs_CHWN_dev = clCreateBuffer(context, 0, sizeof(float) * (N * C * H * W), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * K * P * Q), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_CHWN_dev = clCreateBuffer(context, 0, sizeof(float) * (N * K * P * Q), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_CHWN_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem bias_dev = clCreateBuffer(context, 0, sizeof(float) * (K), NULL, &err);
    CHECK_ERROR(err);

    timer_start(0);
    CHECK_ERROR(clEnqueueWriteBuffer(queue, inputs_dev, CL_TRUE, 0, sizeof(float) * (N * C * H * W), inputs, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, filters_dev, CL_TRUE, 0, sizeof(float) * (K * C * R * S), filters, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, bias_dev, CL_TRUE, 0, sizeof(float) * (K), bias, 0, NULL, NULL));
    //timer_end(0, "wino WriteBuffer");

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel0, 0, sizeof(cl_mem), &inputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 1, sizeof(cl_mem), &inputs_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel0, 3, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel0, 4, sizeof(int), &H));
        CHECK_ERROR(clSetKernelArg(kernel0, 5, sizeof(int), &W));
        size_t gws[1] = {_ceil(N * C * H * W, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel0, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        //timer_end(0, "wino input transform");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel0, 0, sizeof(cl_mem), &filters_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 1, sizeof(cl_mem), &filters_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 2, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel0, 3, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel0, 4, sizeof(int), &R));
        CHECK_ERROR(clSetKernelArg(kernel0, 5, sizeof(int), &S));
        size_t gws[1] = {_ceil(K * C * R * S, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel0, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        //timer_end(0, "wino filter transform");
    }

    {
        timer_start(0);
        int BN = 1, BK = 1;
        int TPmask, TPwidth, TPshift, TQmask, TQwidth, TQshift, Nmask, Nwidth;
        if (N < 2)
            TPmask = 0x0c, TPwidth = 2, TPshift = 2, TQmask = 0x03, TQwidth = 2, TQshift = 0, Nmask = 0x00, Nwidth = 0;
        else if (N < 4)
            TPmask = 0x08, TPwidth = 1, TPshift = 3, TQmask = 0x06, TQwidth = 2, TQshift = 1, Nmask = 0x01, Nwidth = 1;
        else if (N < 8)
            TPmask = 0x08, TPwidth = 1, TPshift = 3, TQmask = 0x04, TQwidth = 1, TQshift = 2, Nmask = 0x03, Nwidth = 2;
        else if (N < 16)
            TPmask = 0x00, TPwidth = 0, TPshift = 0, TQmask = 0x08, TQwidth = 1, TQshift = 3, Nmask = 0x07, Nwidth = 3;
        else
            TPmask = 0x00, TPwidth = 0, TPshift = 0, TQmask = 0x00, TQwidth = 0, TQshift = 0, Nmask = 0x0f, Nwidth = 4;
        int TP = _ceil_div(P, 2 << TPwidth), TQ = _ceil_div(Q, 2 << TQwidth);
        int TK = _ceil_div(K, 16), TN = _ceil_div(N, 1 << Nwidth);

        CHECK_ERROR(clSetKernelArg(kernel1, 0, sizeof(cl_mem), &inputs_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 1, sizeof(cl_mem), &outputs_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 2, sizeof(cl_mem), &filters_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 3, sizeof(cl_mem), &bias_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 4, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel1, 5, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel1, 6, sizeof(int), &H));
        CHECK_ERROR(clSetKernelArg(kernel1, 7, sizeof(int), &W));
        CHECK_ERROR(clSetKernelArg(kernel1, 8, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel1, 9, sizeof(int), &P));
        CHECK_ERROR(clSetKernelArg(kernel1, 10, sizeof(int), &Q));
        CHECK_ERROR(clSetKernelArg(kernel1, 11, sizeof(int), &pad));
        CHECK_ERROR(clSetKernelArg(kernel1, 12, sizeof(int), &TP));
        CHECK_ERROR(clSetKernelArg(kernel1, 13, sizeof(int), &TQ));
        CHECK_ERROR(clSetKernelArg(kernel1, 14, sizeof(int), &BN));
        CHECK_ERROR(clSetKernelArg(kernel1, 15, sizeof(int), &BK));
        CHECK_ERROR(clSetKernelArg(kernel1, 16, sizeof(int), &TPmask));
        CHECK_ERROR(clSetKernelArg(kernel1, 17, sizeof(int), &TPwidth));
        CHECK_ERROR(clSetKernelArg(kernel1, 18, sizeof(int), &TPshift));
        CHECK_ERROR(clSetKernelArg(kernel1, 19, sizeof(int), &TQmask));
        CHECK_ERROR(clSetKernelArg(kernel1, 20, sizeof(int), &TQwidth));
        CHECK_ERROR(clSetKernelArg(kernel1, 21, sizeof(int), &TQshift));
        CHECK_ERROR(clSetKernelArg(kernel1, 22, sizeof(int), &Nmask));
        CHECK_ERROR(clSetKernelArg(kernel1, 23, sizeof(int), &Nwidth));
        size_t gws[3] = {TP * TQ * BN * BK * 256, TK / BK, TN / BN};
        size_t lws[3] = {256, 1, 1};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel1, 3, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino16");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel2, 0, sizeof(cl_mem), &outputs_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 1, sizeof(cl_mem), &outputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel2, 3, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel2, 4, sizeof(int), &P));
        CHECK_ERROR(clSetKernelArg(kernel2, 5, sizeof(int), &Q));
        size_t gws[1] = {_ceil(N * K * P * Q, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel2, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        //timer_end(0, "wino output transform");
    }

    timer_start(0);
    CHECK_ERROR(clEnqueueReadBuffer(queue, outputs_dev, CL_TRUE, 0, sizeof(float) * (N * K * P * Q), outputs, 0, NULL, NULL));
    //timer_end(0, "wino ReadBuffer");

    clReleaseMemObject(inputs_dev);
    clReleaseMemObject(inputs_CHWN_dev);
    clReleaseMemObject(outputs_dev);
    clReleaseMemObject(outputs_CHWN_dev);
    clReleaseMemObject(filters_dev);
    clReleaseMemObject(filters_CHWN_dev);
    clReleaseMemObject(bias_dev);

    clReleaseKernel(kernel0);
    clReleaseKernel(kernel1);
    clReleaseKernel(kernel2);
}

void convolution_wino32(float *inputs, float *outputs, float *filters, float *bias, int N, int C, int H, int W, int K, int P, int Q, int R, int S, int pad, cl_context context, cl_command_queue queue, cl_program program) {
    cl_kernel kernel0 = clCreateKernel(program, "NCHW2CHWN", &err);
    CHECK_ERROR(err);
    cl_kernel kernel1 = clCreateKernel(program, "winograd_2x2_3x3_32x32", &err);
    CHECK_ERROR(err);
    cl_kernel kernel2 = clCreateKernel(program, "CHWN2NCHW", &err);
    CHECK_ERROR(err);

    cl_mem inputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * C * H * W), NULL, &err);
    CHECK_ERROR(err);
    cl_mem inputs_CHWN_dev = clCreateBuffer(context, 0, sizeof(float) * (N * C * H * W), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * K * P * Q), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_CHWN_dev = clCreateBuffer(context, 0, sizeof(float) * (N * K * P * Q), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_CHWN_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem bias_dev = clCreateBuffer(context, 0, sizeof(float) * (K), NULL, &err);
    CHECK_ERROR(err);

    timer_start(0);
    CHECK_ERROR(clEnqueueWriteBuffer(queue, inputs_dev, CL_TRUE, 0, sizeof(float) * (N * C * H * W), inputs, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, filters_dev, CL_TRUE, 0, sizeof(float) * (K * C * R * S), filters, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, bias_dev, CL_TRUE, 0, sizeof(float) * (K), bias, 0, NULL, NULL));
    //timer_end(0, "wino WriteBuffer");

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel0, 0, sizeof(cl_mem), &inputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 1, sizeof(cl_mem), &inputs_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel0, 3, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel0, 4, sizeof(int), &H));
        CHECK_ERROR(clSetKernelArg(kernel0, 5, sizeof(int), &W));
        size_t gws[1] = {_ceil(N * C * H * W, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel0, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        //timer_end(0, "wino input transform");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel0, 0, sizeof(cl_mem), &filters_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 1, sizeof(cl_mem), &filters_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 2, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel0, 3, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel0, 4, sizeof(int), &R));
        CHECK_ERROR(clSetKernelArg(kernel0, 5, sizeof(int), &S));
        size_t gws[1] = {_ceil(K * C * R * S, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel0, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        //timer_end(0, "wino filter transform");
    }

    {
        timer_start(0);
        int BN = 1, BK = 1;
        int TPmask, TPwidth, TPshift, TQmask, TQwidth, TQshift, Nmask, Nwidth;
        if (N < 2)
            TPmask = 0x18, TPwidth = 2, TPshift = 3, TQmask = 0x07, TQwidth = 3, TQshift = 0, Nmask = 0x00, Nwidth = 0;
        else if (N < 4)
            TPmask = 0x18, TPwidth = 2, TPshift = 3, TQmask = 0x06, TQwidth = 2, TQshift = 1, Nmask = 0x01, Nwidth = 1;
        else if (N < 8)
            TPmask = 0x10, TPwidth = 1, TPshift = 4, TQmask = 0x0c, TQwidth = 2, TQshift = 2, Nmask = 0x03, Nwidth = 2;
        else if (N < 16)
            TPmask = 0x10, TPwidth = 1, TPshift = 4, TQmask = 0x08, TQwidth = 1, TQshift = 3, Nmask = 0x07, Nwidth = 3;
        else if (N < 32)
            TPmask = 0x00, TPwidth = 0, TPshift = 0, TQmask = 0x10, TQwidth = 1, TQshift = 4, Nmask = 0x0f, Nwidth = 4;
        else
            TPmask = 0x00, TPwidth = 0, TPshift = 0, TQmask = 0x00, TQwidth = 0, TQshift = 0, Nmask = 0x1f, Nwidth = 5;
        int TP = _ceil_div(P, 2 << TPwidth), TQ = _ceil_div(Q, 2 << TQwidth);
        int TK = _ceil_div(K, 32), TN = _ceil_div(N, 1 << Nwidth);

        CHECK_ERROR(clSetKernelArg(kernel1, 0, sizeof(cl_mem), &inputs_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 1, sizeof(cl_mem), &outputs_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 2, sizeof(cl_mem), &filters_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 3, sizeof(cl_mem), &bias_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 4, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel1, 5, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel1, 6, sizeof(int), &H));
        CHECK_ERROR(clSetKernelArg(kernel1, 7, sizeof(int), &W));
        CHECK_ERROR(clSetKernelArg(kernel1, 8, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel1, 9, sizeof(int), &P));
        CHECK_ERROR(clSetKernelArg(kernel1, 10, sizeof(int), &Q));
        CHECK_ERROR(clSetKernelArg(kernel1, 11, sizeof(int), &pad));
        CHECK_ERROR(clSetKernelArg(kernel1, 12, sizeof(int), &TP));
        CHECK_ERROR(clSetKernelArg(kernel1, 13, sizeof(int), &TQ));
        CHECK_ERROR(clSetKernelArg(kernel1, 14, sizeof(int), &BN));
        CHECK_ERROR(clSetKernelArg(kernel1, 15, sizeof(int), &BK));
        CHECK_ERROR(clSetKernelArg(kernel1, 16, sizeof(int), &TPmask));
        CHECK_ERROR(clSetKernelArg(kernel1, 17, sizeof(int), &TPwidth));
        CHECK_ERROR(clSetKernelArg(kernel1, 18, sizeof(int), &TPshift));
        CHECK_ERROR(clSetKernelArg(kernel1, 19, sizeof(int), &TQmask));
        CHECK_ERROR(clSetKernelArg(kernel1, 20, sizeof(int), &TQwidth));
        CHECK_ERROR(clSetKernelArg(kernel1, 21, sizeof(int), &TQshift));
        CHECK_ERROR(clSetKernelArg(kernel1, 22, sizeof(int), &Nmask));
        CHECK_ERROR(clSetKernelArg(kernel1, 23, sizeof(int), &Nwidth));
        size_t gws[3] = {TP * TQ * BN * BK * 256, TK / BK, TN / BN};
        size_t lws[3] = {256, 1, 1};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel1, 3, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel2, 0, sizeof(cl_mem), &outputs_CHWN_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 1, sizeof(cl_mem), &outputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel2, 3, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel2, 4, sizeof(int), &P));
        CHECK_ERROR(clSetKernelArg(kernel2, 5, sizeof(int), &Q));
        size_t gws[1] = {_ceil(N * K * P * Q, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel2, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        //timer_end(0, "wino output transform");
    }

    timer_start(0);
    CHECK_ERROR(clEnqueueReadBuffer(queue, outputs_dev, CL_TRUE, 0, sizeof(float) * (N * K * P * Q), outputs, 0, NULL, NULL));
    //timer_end(0, "wino ReadBuffer");

    clReleaseMemObject(inputs_dev);
    clReleaseMemObject(inputs_CHWN_dev);
    clReleaseMemObject(outputs_dev);
    clReleaseMemObject(outputs_CHWN_dev);
    clReleaseMemObject(filters_dev);
    clReleaseMemObject(filters_CHWN_dev);
    clReleaseMemObject(bias_dev);

    clReleaseKernel(kernel0);
    clReleaseKernel(kernel1);
    clReleaseKernel(kernel2);
}

void convolution_current(float *inputs, float *outputs, float *filters, float *bias, int N, int C, int H, int W, int K, int P, int Q, int R, int S, int pad, cl_context context, cl_command_queue queue, cl_program program) {
    cl_kernel kernel = clCreateKernel(program, "CCF", &err);
    CHECK_ERROR(err);

    cl_mem inputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * C * H * W), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * K * P * Q), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem bias_dev = clCreateBuffer(context, 0, sizeof(float) * (K), NULL, &err);
    CHECK_ERROR(err);

    timer_start(0);
    CHECK_ERROR(clEnqueueWriteBuffer(queue, inputs_dev, CL_TRUE, 0, sizeof(float) * (N * C * H * W), inputs, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, filters_dev, CL_TRUE, 0, sizeof(float) * (K * C * R * S), filters, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, bias_dev, CL_TRUE, 0, sizeof(float) * (K), bias, 0, NULL, NULL));
    //timer_end(0, "current WriteBuffer");

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel, 0, sizeof(cl_mem), &inputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel, 1, sizeof(cl_mem), &outputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel, 2, sizeof(cl_mem), &filters_dev));
        CHECK_ERROR(clSetKernelArg(kernel, 3, sizeof(cl_mem), &bias_dev));
        CHECK_ERROR(clSetKernelArg(kernel, 4, sizeof(int), &Q));
        CHECK_ERROR(clSetKernelArg(kernel, 5, sizeof(int), &P));
        CHECK_ERROR(clSetKernelArg(kernel, 6, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel, 7, sizeof(int), &W));
        CHECK_ERROR(clSetKernelArg(kernel, 8, sizeof(int), &H));
        CHECK_ERROR(clSetKernelArg(kernel, 9, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel, 10, sizeof(int), &S));
        CHECK_ERROR(clSetKernelArg(kernel, 11, sizeof(int), &R));
        CHECK_ERROR(clSetKernelArg(kernel, 12, sizeof(int), &pad));
        int stride = 1;
        CHECK_ERROR(clSetKernelArg(kernel, 13, sizeof(int), &stride));
        size_t gws[3] = {P * Q, K, N};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel, 3, NULL, gws, NULL, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "current");
    }

    timer_start(0);
    CHECK_ERROR(clEnqueueReadBuffer(queue, outputs_dev, CL_TRUE, 0, sizeof(float) * (N * K * P * Q), outputs, 0, NULL, NULL));
    //timer_end(0, "current ReadBuffer");

    clReleaseMemObject(inputs_dev);
    clReleaseMemObject(outputs_dev);
    clReleaseMemObject(filters_dev);
    clReleaseMemObject(bias_dev);

    clReleaseKernel(kernel);
}

void convolution_wino_nonfused_fwd(float *inputs, float *outputs, float *filters, int N, int C, int H, int W, int K, int P, int Q, int R, int S, int pad, cl_context context, cl_command_queue queue, cl_program program) {
    cl_kernel kernel0 = clCreateKernel(program, "winograd_2x2_3x3_data_transform", &err);
    CHECK_ERROR(err);
    cl_kernel kernel1 = clCreateKernel(program, "winograd_2x2_3x3_filter_transform", &err);
    CHECK_ERROR(err);
    cl_kernel kernel2 = clCreateKernel(program, "winograd_2x2_3x3_inverse_transform", &err);
    CHECK_ERROR(err);

    int TP = _ceil_div(P, 2), TQ = _ceil_div(Q, 2);

    cl_mem inputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * C * H * W), NULL, &err);
    CHECK_ERROR(err);
    cl_mem inputs_T_dev = clCreateBuffer(context, 0, sizeof(float) * (16 * C * N * TP * TQ), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_T_dev = clCreateBuffer(context, 0, sizeof(float) * (16 * K * C), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * K * P * Q), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_T_dev = clCreateBuffer(context, 0, sizeof(float) * (16 * K * N * TP * TQ), NULL, &err);
    CHECK_ERROR(err);

    CHECK_ERROR(clEnqueueWriteBuffer(queue, inputs_dev, CL_TRUE, 0, sizeof(float) * (N * C * H * W), inputs, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, filters_dev, CL_TRUE, 0, sizeof(float) * (K * C * R * S), filters, 0, NULL, NULL));

    timer_start(1);

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel0, 0, sizeof(cl_mem), &inputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 1, sizeof(cl_mem), &inputs_T_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel0, 3, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel0, 4, sizeof(int), &H));
        CHECK_ERROR(clSetKernelArg(kernel0, 5, sizeof(int), &W));
        CHECK_ERROR(clSetKernelArg(kernel0, 6, sizeof(int), &pad));
        CHECK_ERROR(clSetKernelArg(kernel0, 7, sizeof(int), &TP));
        CHECK_ERROR(clSetKernelArg(kernel0, 8, sizeof(int), &TQ));
        size_t gws[1] = {_ceil(N * C * TP * TQ, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel0, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused data_transform");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel1, 0, sizeof(cl_mem), &filters_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 1, sizeof(cl_mem), &filters_T_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 2, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel1, 3, sizeof(int), &C));
        size_t gws[1] = {_ceil(K * C, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel1, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused filter_transform");
    }

    {
        timer_start(0);
        for (int i = 0; i < 16; ++i) {
            cl_event event;
            err = clblasSgemm(clblasRowMajor, clblasNoTrans, clblasNoTrans,
                K, N * TP * TQ, C, 1,
                filters_T_dev, i * K * C, C, inputs_T_dev, i * C * N * TP * TQ, N * TP * TQ,
                0, outputs_T_dev, i * K * N * TP * TQ, N * TP * TQ,
                1, &queue, 0, NULL, &event);
        }
        clFinish(queue);
        timer_end(0, "wino_nonfused GEMM");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel2, 0, sizeof(cl_mem), &outputs_T_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 1, sizeof(cl_mem), &outputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel2, 3, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel2, 4, sizeof(int), &P));
        CHECK_ERROR(clSetKernelArg(kernel2, 5, sizeof(int), &Q));
        CHECK_ERROR(clSetKernelArg(kernel2, 6, sizeof(int), &TP));
        CHECK_ERROR(clSetKernelArg(kernel2, 7, sizeof(int), &TQ));
        size_t gws[1] = {_ceil(K * N * TP * TQ, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel2, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused inverse_transform");
    }

    clFinish(queue);
    timer_end(1, "wino_nonfused");

    CHECK_ERROR(clEnqueueReadBuffer(queue, outputs_dev, CL_TRUE, 0, sizeof(float) * (N * K * P * Q), outputs, 0, NULL, NULL));

    clReleaseMemObject(inputs_dev);
    clReleaseMemObject(inputs_T_dev);
    clReleaseMemObject(outputs_dev);
    clReleaseMemObject(outputs_T_dev);
    clReleaseMemObject(filters_dev);
    clReleaseMemObject(filters_T_dev);

    clReleaseKernel(kernel0);
    clReleaseKernel(kernel1);
    clReleaseKernel(kernel2);
}

void convolution_wino_nonfused_bwd_data(float *inputs, float *outputs, float *filters, int N, int C, int H, int W, int K, int P, int Q, int R, int S, int pad, cl_context context, cl_command_queue queue, cl_program program) {
    cl_kernel kernel0 = clCreateKernel(program, "winograd_2x2_3x3_data_transform", &err);
    CHECK_ERROR(err);
    cl_kernel kernel1 = clCreateKernel(program, "winograd_2x2_3x3_filter_transform", &err);
    CHECK_ERROR(err);
    cl_kernel kernel2 = clCreateKernel(program, "winograd_2x2_3x3_inverse_transform", &err);
    CHECK_ERROR(err);
    cl_kernel kernel3 = clCreateKernel(program, "flip_filter", &err);
    CHECK_ERROR(err);

    int TP = _ceil_div(H, 2), TQ = _ceil_div(W, 2);
    pad = R - 1 - pad;

    cl_mem inputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * C * H * W), NULL, &err);
    CHECK_ERROR(err);
    cl_mem inputs_T_dev = clCreateBuffer(context, 0, sizeof(float) * (16 * C * N * TP * TQ), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_F_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_T_dev = clCreateBuffer(context, 0, sizeof(float) * (16 * K * C), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * K * P * Q), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_T_dev = clCreateBuffer(context, 0, sizeof(float) * (16 * K * N * TP * TQ), NULL, &err);
    CHECK_ERROR(err);

    CHECK_ERROR(clEnqueueWriteBuffer(queue, outputs_dev, CL_TRUE, 0, sizeof(float) * (N * K * P * Q), outputs, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, filters_dev, CL_TRUE, 0, sizeof(float) * (K * C * R * S), filters, 0, NULL, NULL));

    timer_start(1);

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel0, 0, sizeof(cl_mem), &outputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 1, sizeof(cl_mem), &outputs_T_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel0, 3, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel0, 4, sizeof(int), &P));
        CHECK_ERROR(clSetKernelArg(kernel0, 5, sizeof(int), &Q));
        CHECK_ERROR(clSetKernelArg(kernel0, 6, sizeof(int), &pad));
        CHECK_ERROR(clSetKernelArg(kernel0, 7, sizeof(int), &TP));
        CHECK_ERROR(clSetKernelArg(kernel0, 8, sizeof(int), &TQ));
        size_t gws[1] = {_ceil(N * K * TP * TQ, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel0, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused data_transform");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel3, 0, sizeof(cl_mem), &filters_dev));
        CHECK_ERROR(clSetKernelArg(kernel3, 1, sizeof(cl_mem), &filters_F_dev));
        CHECK_ERROR(clSetKernelArg(kernel3, 2, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel3, 3, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel3, 4, sizeof(int), &R));
        CHECK_ERROR(clSetKernelArg(kernel3, 5, sizeof(int), &S));
        size_t gws[1] = {_ceil(K * C * R * S, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel3, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused flip_filter");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel1, 0, sizeof(cl_mem), &filters_F_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 1, sizeof(cl_mem), &filters_T_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 2, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel1, 3, sizeof(int), &K));
        size_t gws[1] = {_ceil(C * K, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel1, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused filter_transform");
    }

    {
        timer_start(0);
        for (int i = 0; i < 16; ++i) {
            cl_event event;
            err = clblasSgemm(clblasRowMajor, clblasNoTrans, clblasNoTrans,
                C, N * TP * TQ, K, 1,
                filters_T_dev, i * C * K, K, outputs_T_dev, i * K * N * TP * TQ, N * TP * TQ,
                0, inputs_T_dev, i * C * N * TP * TQ, N * TP * TQ,
                1, &queue, 0, NULL, &event);
        }
        clFinish(queue);
        timer_end(0, "wino_nonfused GEMM");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel2, 0, sizeof(cl_mem), &inputs_T_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 1, sizeof(cl_mem), &inputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel2, 3, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel2, 4, sizeof(int), &H));
        CHECK_ERROR(clSetKernelArg(kernel2, 5, sizeof(int), &W));
        CHECK_ERROR(clSetKernelArg(kernel2, 6, sizeof(int), &TP));
        CHECK_ERROR(clSetKernelArg(kernel2, 7, sizeof(int), &TQ));
        size_t gws[1] = {_ceil(C * N * TP * TQ, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel2, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused inverse_transform");
    }

    clFinish(queue);
    timer_end(1, "wino_nonfused");

    CHECK_ERROR(clEnqueueReadBuffer(queue, inputs_dev, CL_TRUE, 0, sizeof(float) * (N * C * H * W), inputs, 0, NULL, NULL));

    clReleaseMemObject(inputs_dev);
    clReleaseMemObject(inputs_T_dev);
    clReleaseMemObject(outputs_dev);
    clReleaseMemObject(outputs_T_dev);
    clReleaseMemObject(filters_dev);
    clReleaseMemObject(filters_F_dev);
    clReleaseMemObject(filters_T_dev);

    clReleaseKernel(kernel0);
    clReleaseKernel(kernel1);
    clReleaseKernel(kernel2);
    clReleaseKernel(kernel3);
}

void convolution_wino_nonfused_bwd_filter(float *inputs, float *outputs, float *filters, int N, int C, int H, int W, int K, int P, int Q, int R, int S, int pad, cl_context context, cl_command_queue queue, cl_program program) {
    cl_kernel kernel0 = clCreateKernel(program, "winograd_3x3_2x2_data_transform", &err);
    CHECK_ERROR(err);
    cl_kernel kernel1 = clCreateKernel(program, "winograd_3x3_2x2_filter_transform", &err);
    CHECK_ERROR(err);
    cl_kernel kernel2 = clCreateKernel(program, "winograd_3x3_2x2_inverse_transform", &err);
    CHECK_ERROR(err);

    int TP = _ceil_div(P, 2), TQ = _ceil_div(Q, 2);

    cl_mem inputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * C * H * W), NULL, &err);
    CHECK_ERROR(err);
    cl_mem inputs_T_dev = clCreateBuffer(context, 0, sizeof(float) * (16 * C * N * TP * TQ), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_dev = clCreateBuffer(context, 0, sizeof(float) * (K * C * R * S), NULL, &err);
    CHECK_ERROR(err);
    cl_mem filters_T_dev = clCreateBuffer(context, 0, sizeof(float) * (16 * K * C), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_dev = clCreateBuffer(context, 0, sizeof(float) * (N * K * P * Q), NULL, &err);
    CHECK_ERROR(err);
    cl_mem outputs_T_dev = clCreateBuffer(context, 0, sizeof(float) * (16 * K * N * TP * TQ), NULL, &err);
    CHECK_ERROR(err);

    CHECK_ERROR(clEnqueueWriteBuffer(queue, inputs_dev, CL_TRUE, 0, sizeof(float) * (N * C * H * W), inputs, 0, NULL, NULL));
    CHECK_ERROR(clEnqueueWriteBuffer(queue, outputs_dev, CL_TRUE, 0, sizeof(float) * (N * K * P * Q), outputs, 0, NULL, NULL));

    timer_start(1);

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel0, 0, sizeof(cl_mem), &inputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 1, sizeof(cl_mem), &inputs_T_dev));
        CHECK_ERROR(clSetKernelArg(kernel0, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel0, 3, sizeof(int), &C));
        CHECK_ERROR(clSetKernelArg(kernel0, 4, sizeof(int), &H));
        CHECK_ERROR(clSetKernelArg(kernel0, 5, sizeof(int), &W));
        CHECK_ERROR(clSetKernelArg(kernel0, 6, sizeof(int), &pad));
        CHECK_ERROR(clSetKernelArg(kernel0, 7, sizeof(int), &TP));
        CHECK_ERROR(clSetKernelArg(kernel0, 8, sizeof(int), &TQ));
        size_t gws[1] = {_ceil(N * C * TP * TQ, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel0, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused data_transform");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel1, 0, sizeof(cl_mem), &outputs_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 1, sizeof(cl_mem), &outputs_T_dev));
        CHECK_ERROR(clSetKernelArg(kernel1, 2, sizeof(int), &N));
        CHECK_ERROR(clSetKernelArg(kernel1, 3, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel1, 4, sizeof(int), &P));
        CHECK_ERROR(clSetKernelArg(kernel1, 5, sizeof(int), &Q));
        CHECK_ERROR(clSetKernelArg(kernel1, 6, sizeof(int), &TP));
        CHECK_ERROR(clSetKernelArg(kernel1, 7, sizeof(int), &TQ));
        size_t gws[1] = {_ceil(N * K * TP * TQ, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel1, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused filter_transform");
    }

    {
        timer_start(0);
        for (int i = 0; i < 16; ++i) {
            cl_event event;
            CHECK_ERROR(clblasSgemm(clblasRowMajor, clblasNoTrans, clblasTrans,
                K, C, N * TP * TQ, 1,
                outputs_T_dev, i * K * N * TP * TQ, N * TP * TQ, inputs_T_dev, i * C * N * TP * TQ, N * TP * TQ,
                0, filters_T_dev, i * K * C, C,
                1, &queue, 0, NULL, &event));
        }
        clFinish(queue);
        timer_end(0, "wino_nonfused GEMM");
    }

    {
        timer_start(0);
        CHECK_ERROR(clSetKernelArg(kernel2, 0, sizeof(cl_mem), &filters_T_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 1, sizeof(cl_mem), &filters_dev));
        CHECK_ERROR(clSetKernelArg(kernel2, 2, sizeof(int), &K));
        CHECK_ERROR(clSetKernelArg(kernel2, 3, sizeof(int), &C));
        size_t gws[1] = {_ceil(K * C, 256)};
        size_t lws[1] = {256};
        CHECK_ERROR(clEnqueueNDRangeKernel(queue, kernel2, 1, NULL, gws, lws, 0, NULL, NULL));
        clFinish(queue);
        timer_end(0, "wino_nonfused inverse_transform");
    }

    clFinish(queue);
    timer_end(1, "wino_nonfused");

    CHECK_ERROR(clEnqueueReadBuffer(queue, filters_dev, CL_TRUE, 0, sizeof(float) * (K * C * R * S), filters, 0, NULL, NULL));

    clReleaseMemObject(inputs_dev);
    clReleaseMemObject(inputs_T_dev);
    clReleaseMemObject(outputs_dev);
    clReleaseMemObject(outputs_T_dev);
    clReleaseMemObject(filters_dev);
    clReleaseMemObject(filters_T_dev);

    clReleaseKernel(kernel0);
    clReleaseKernel(kernel1);
    clReleaseKernel(kernel2);
}

void validate(int N, int C, int H, int W, int K, int P, int Q, int R, int S, int pad, cl_context context, cl_command_queue queue, cl_program program) {
    float *inputs = (float*)malloc(sizeof(float) * (N * C * H * W));
    float *filters = (float*)malloc(sizeof(float) * (K * C * R * S));
    float *bias = (float*)malloc(sizeof(float) * (K));
    fillData(inputs, N * C * H * W);
    fillData(filters, K * C * R * S);
    fillData(bias, K);
    //printData(inputs, N, C, H, W, "inputs");
    //printData(filters, K, C, R, S, "filters");
    //printData(bias, 1, 1, 1, K, "bias");

    float *outputs_cpu = (float*)malloc(sizeof(float) * (N * K * P * Q));
    float *outputs_wino32 = (float*)malloc(sizeof(float) * (N * K * P * Q));
    float *outputs_wino16 = (float*)malloc(sizeof(float) * (N * K * P * Q));
    float *outputs_current = (float*)malloc(sizeof(float) * (N * K * P * Q));
    float *outputs_mc = (float*)malloc(sizeof(float) * (N * K * P * Q));
    float *outputs_wino_nonfused = (float*)malloc(sizeof(float) * (N * K * P * Q));

    float *dx_cpu = (float*)malloc(sizeof(float) * (N * C * H * W));
    float *dx_wino_nonfused = (float*)malloc(sizeof(float) * (N * C * H * W));

    float *dw_cpu = (float*)malloc(sizeof(float) * (K * C * R * S));
    float *dw_wino_nonfused = (float*)malloc(sizeof(float) * (K * C * R * S));

    //convolution_cpu(inputs, outputs_cpu, filters, dx_cpu, dw_cpu, N, C, H, W, K, P, Q, R, S, pad);
    for (int i = 0; i < 4; ++i) {
        //convolution_current(inputs, outputs_current, filters, bias, N, C, H, W, K, P, Q, R, S, pad, context, queue, program);
        //convolution_wino32(inputs, outputs_wino32, filters, bias, N, C, H, W, K, P, Q, R, S, pad, context, queue, program);
        //convolution_wino16(inputs, outputs_wino16, filters, bias, N, C, H, W, K, P, Q, R, S, pad, context, queue, program);
        //convolution_mc(inputs, outputs_mc, filters, bias, N, C, H, W, K, P, Q, R, S, pad, context, queue, program);
        convolution_wino_nonfused_fwd(inputs, outputs_wino_nonfused, filters, N, C, H, W, K, P, Q, R, S, pad, context, queue, program);
        convolution_wino_nonfused_bwd_data(dx_wino_nonfused, outputs_wino_nonfused, filters, N, C, H, W, K, P, Q, R, S, pad, context, queue, program);
        convolution_wino_nonfused_bwd_filter(inputs, outputs_wino_nonfused, dw_wino_nonfused, N, C, H, W, K, P, Q, R, S, pad, context, queue, program);
    }
    //printData(outputs_current, N, K, P, Q, "outputs_current");
    //printf("!!!!! WINO VALIDATION %s !!!!!\n", equalData(outputs_cpu, outputs_wino, N, K, P, Q) ? "SUCCESS" : "FAIL");
    //printf("!!!!! CURRENT VALIDATION %s !!!!!\n", equalData(outputs_cpu, outputs_current, N, K, P, Q) ? "SUCCESS" : "FAIL");
    //printf("!!!!! WINO32 == CURRENT VALIDATION %s !!!!!\n", equalData(outputs_wino32, outputs_current, N, K, P, Q) ? "SUCCESS" : "FAIL");
    //printf("!!!!! WINO16 == CURRENT VALIDATION %s !!!!!\n", equalData(outputs_wino16, outputs_current, N, K, P, Q) ? "SUCCESS" : "FAIL");
    //printf("!!!!! MC == CURRENT VALIDATION %s !!!!!\n", equalData(outputs_mc, outputs_current, N, K, P, Q) ? "SUCCESS" : "FAIL");
    //printf("!!!!! WINO_NONFUSED == CURRENT VALIDATION %s !!!!!\n", equalData(outputs_wino_nonfused, outputs_current, N, K, P, Q) ? "SUCCESS" : "FAIL");
    printf("!!!!! cpu == wino_nonfused VALIDATION FWD %s !!!!!\n", equalData(outputs_cpu, outputs_wino_nonfused, N, K, P, Q) ? "SUCCESS" : "FAIL");
    printf("!!!!! cpu == wino_nonfused VALIDATION BWD DATA %s !!!!!\n", equalData(dx_cpu, dx_wino_nonfused, N, C, H, W) ? "SUCCESS" : "FAIL");
    printf("!!!!! cpu == wino_nonfused VALIDATION BWD FILTER %s !!!!!\n", equalData(dw_cpu, dw_wino_nonfused, K, C, R, S) ? "SUCCESS" : "FAIL");

    free(inputs);
    free(filters);
    free(bias);
    free(outputs_cpu);
    free(outputs_wino32);
    free(outputs_wino16);
    free(outputs_current);
    free(outputs_mc);
    free(outputs_wino_nonfused);
    free(dx_cpu);
    free(dx_wino_nonfused);
    free(dw_cpu);
    free(dw_wino_nonfused);
}

int main() {
    //srand(time(NULL));

    cl_platform_id platform;
    err = clGetPlatformIDs(1, &platform, NULL);
    CHECK_ERROR(err);

    cl_device_id device;
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL);
    CHECK_ERROR(err);

    cl_context context;
    context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    CHECK_ERROR(err);

    cl_command_queue queue;
    queue = clCreateCommandQueue(context, device, 0, &err);
    CHECK_ERROR(err);

    CHECK_ERROR(clblasSetup());

    cl_program program = create_and_build_program(context, device, "kernel.cl");

    //validate(1, 1, 4, 4, 1, 2, 2, 3, 3, 0, context, queue, program);
    //validate(1, 1, 8, 8, 1, 6, 6, 3, 3, 0, context, queue, program);
    //validate(1, 1, 3, 3, 1, 1, 1, 3, 3, 0, context, queue, program);
    //validate(1, 1, 15, 15, 1, 13, 13, 3, 3, 0, context, queue, program);
    //validate(33, 63, 17, 17, 63, 17, 17, 3, 3, 1, context, queue, program);
    //validate(1, 3, 224, 224, 64, 224, 224, 3, 3, 1, context, queue, program);
    //validate(32, 256, 56, 56, 256, 56, 56, 3, 3, 1, context, queue, program);
    //validate(32, 512, 28, 28, 512, 28, 28, 3, 3, 1, context, queue, program);
    //validate(22, 22, 22, 22, 22, 22, 22, 3, 3, 1, context, queue, program); // crazy gemm time

    clblasTeardown();

    clReleaseProgram(program);
    clReleaseCommandQueue(queue);
    clReleaseContext(context);
    return 0;
}
