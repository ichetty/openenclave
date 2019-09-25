// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <openenclave/host.h>
#include <openenclave/internal/error.h>
#include <openenclave/internal/tests.h>
#include <openenclave/internal/thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __GNUC__
#include <pthread.h>
#elif _MSC_VER
#include <windows.h>
#endif
#include "switchless_threads_u.h"

#define NUM_HOST_THREADS 7
#define STRING_LEN 100
#define STRING_HELLO "Hello World"
#define HOST_PARAM_STRING "host string parameter"
#define HOST_STACK_STRING "host string on stack"

static int thread_create(oe_thread_t* thread, void* (*func)(void*), void* arg)
{
#if __GNUC__
    return pthread_create(thread, NULL, func, arg);
#elif _MSC_VER
    typedef DWORD (*start_routine_t)(void*);
    start_routine_t start_routine = (start_routine_t)func;
    *thread = (oe_thread_t)CreateThread(NULL, 0, start_routine, arg, 0, NULL);
    return *thread == (oe_thread_t)NULL ? 1 : 0;
#endif
}

static int thread_join(oe_thread_t thread)
{
#if __GNUC__
    return pthread_join(thread, NULL);
#elif _MSC_VER
    HANDLE handle = (HANDLE)thread;
    if (WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0)
    {
        CloseHandle(handle);
        return 0;
    }
    return 1;
#endif
}

int host_echo_switchless(char* in, char* out, char* str1, char str2[STRING_LEN])
{
    OE_TEST(strcmp(str1, HOST_PARAM_STRING) == 0);
    OE_TEST(strcmp(str2, HOST_STACK_STRING) == 0);

    strcpy(out, in);

    return 0;
}

int host_echo_regular(char* in, char* out, char* str1, char str2[STRING_LEN])
{
    OE_TEST(strcmp(str1, HOST_PARAM_STRING) == 0);
    OE_TEST(strcmp(str2, HOST_STACK_STRING) == 0);

    strcpy(out, in);

    return 0;
}

void* thread_func(void* arg)
{
    char out[100];
    int return_val;

    oe_enclave_t* enclave = (oe_enclave_t*)arg;
    oe_result_t result =
        enc_echo_single(enclave, &return_val, "Hello World", out);

    if (result != OE_OK)
        oe_put_err("oe_call_enclave() failed: result=%u", result);

    if (return_val != 0)
        oe_put_err("ECALL failed args.result=%d", return_val);

    if (strcmp("Hello World", out) != 0)
        oe_put_err("ecall failed: %s != %s\n", "Hello World", out);

    return NULL;
}

int main(int argc, const char* argv[])
{
    oe_enclave_t* enclave = NULL;
    oe_result_t result;

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s ENCLAVE_PATH\n", argv[0]);
        return 1;
    }

    const uint32_t flags = oe_get_create_flags();

    // Enable switchless and configure host worker number
    oe_enclave_config_context_switchless_t config = {2, 0};
    oe_enclave_config_t configs[] = {{
        .config_type = OE_ENCLAVE_CONFIG_CONTEXT_SWITCHLESS,
        .u.context_switchless_config = &config,
    }};

    if ((result = oe_create_switchless_threads_enclave(
             argv[1],
             OE_ENCLAVE_TYPE_SGX,
             flags,
             configs,
             OE_COUNTOF(configs),
             &enclave)) != OE_OK)
        oe_put_err("oe_create_enclave(): result=%u", result);

    oe_thread_t threads[NUM_HOST_THREADS];

    // Start threads that each invokes 'enc_echo_single', an ECALL that makes
    // only one regular OCALL and one switchless OCALL.
    for (int i = 0; i < NUM_HOST_THREADS; i++)
    {
        int ret = 0;
        if ((ret = thread_create(&threads[i], thread_func, enclave)))
        {
            oe_put_err("thread_create(host): ret=%u", ret);
        }
    }

    // Invoke 'enc_echo_multiple` which makes multiple regular OCALLs and
    // multiple switchless OCALLs.
    char out[STRING_LEN];
    int return_val;
    int repeats = 10;
    OE_TEST(
        enc_echo_multiple(enclave, &return_val, "Hello World", out, repeats) ==
        OE_OK);

    // Wait for the threads to complete.
    for (int i = 0; i < NUM_HOST_THREADS; i++)
    {
        thread_join(threads[i]);
    }

    result = oe_terminate_enclave(enclave);
    OE_TEST(result == OE_OK);

    printf("=== passed all tests (switchless_threads)\n");

    return 0;
}
