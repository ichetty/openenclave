# Switchless Calls Sample

This sample demonstrates how to make switchless calls to host from inside an enclave.
It has the following properties:

- Explain the concept of switchless calls
- Identify cases where switchless calls are appropriate
- Demonstrate how to mark a function as `transition_using_threads` in EDL, and use [`oeedger8r`](https://github.com/openenclave/openenclave/tree/master/docs/GettingStartedDocs/Edger8rGettingStarted.md) tool to compile it
- Demonstrate how to configure an enclave to enable switchless calls originated within it
- Recommend the number of host worker threads required for switchless calls in practice

Prerequisite: you may want to read [Common Sample Information](../README.md#common-sample-information) before going further.

## Switchless Calls

In an enclave application, the host makes **ECALL**s into functions exposed by the enclaves it created. Likewise,
the enclaves may make **OCALL**s into functions exposed by the host that created them. In either case, the
execution has to be transitioned from an untrusted environment to a trusted environment, or vice versa. Since the
transition is costly due to heavy security checks, it might be more performance advantageous to make the calls
**context-switchless**: the caller delegates the function call to a worker thread in the other environment, which does the real job of calling the function and post the result to the caller. Both the calling thread and the
worker thread never leave their respective execution contexts during the perceived function call.

The calling thread and the worker thread need to exchange information twice during the call. When the switchless
call is initiated, the caller needs to pass the `job` (encapsulating information regarding the function call in a
 single object, for details see the next section) to the worker thread. And when the call finishes, the worker
thread needs to pass the result back to the caller. Both exchanges need to be synchronized.

While switchless calls save transition time, they require at least one additional thread to service the calls.
More threads typically means more competition of the CPU cores and more context switches, hurting the performance.
Whether to make a particular function switchless has to weigh the associated costs and savings. In general, the
good candidates for switchless calls are functions that are: 1) short, thus the transition takes relatively high
percentage of the overall execution time of the call; and 2) called frequently, so the savings in transition time
add up.

## How does OpenEnclave support switchless OCALLs

OpenEnclave only supports synchronous switchless OCALLs currently. When the caller within an enclave makes a
switchless OCALL, the trusted OpenEnclave runtime creates a `job` out of the function call. The `job` object
includes information such as the function ID, the parameters marshaled into a buffer, and a buffer for holding the
return value(s). The job is posted to a shared memory region which both the enclave and the host can access.

A host worker thread checks and retrieves `job` from the shared memory region. It uses the untrusted OpenEnclave
runtime to process the `job` by unmarshaling the parameters, then dispatching to the callee function, and finally
relaying the result back to the trusted OpenEnclave runtime, which is further forwarded back to the caller.

To support simultaneous switchless OCALLs made from enclaves, the host workers are multi-threaded. OpenEnclave
allows users to configure how many host worker threads are to be created for servicing switchless OCALLs. The
following example illustrates how to do that. A word of caution is that too many host worker threads might increase
competition of cores between threads and degrade the performance. Therefore, if a enclave has switchless calls
enabled, OpenEnclave caps the number of host worker threads for it to the number of enclave threads specified.

With the current implementation, we recommend users to avoid using more host worker threads than the minimum of:

1. the number of simultaneously active enclave threads, and
2. the number of cores that are potentially available to host worker threads.

For example, on a 4-core machine, if the number of the simultaneously active enclave threads is 2, and there is no
host threads other than the two threads making ECALLs and the switchless worker threads, both 1) and 2) would be 2. So we recommend setting the number of host worker threads to 2.

The exception to the above rule happens when 1) or 2) is zero or negative. For example, if the host starts two more
additional threads that are expected to be active along with the two enclave threads, the number of cores available
to the worker threads is actually 0, and the minimum of 1) and 2) would be 0. In this case, we recommend setting
the number of host worker threads to 1 nevertheless, to ensure switchless calls are serviced by at least one thread.

The above recommendation may change when we modify the behavior of worker threads in the future.

## About the EDL

In this sample, we pretend the enclave doesn't know addition. It relies on a host function `host_increment` to
increment a number by 1, and repeat calling it `N` times to add `N` to a given number. Since `host_increment` is
short and called frequently, it is appropriate to make it a switchless function.

First we need to define the functions we want to call between the host and the enclave. To do this we create a `switchless.edl` file:

```edl
enclave {
    trusted {
        public void enclave_add_N([in, out] int* m, int n);
    };

    untrusted {
        void host_increment([in, out] int* m) transition_using_threads;
    };
};
```

Function `host_increment`'s declaration ends with keyword `transition_using_threads`, indicating it should be called switchlessly at run time. However, this a best-effort directive. OpenEnclave runtime may still choose to fall back to a tradition OCALL if switchless call resources are unavailable, e.g., the enclave is not configured as switchless-capable, or the host worker threads are busy servicing other switchless OCALLs.

To generate the functions with the marshaling code, the `oeedger8r` tool is called in both the host and enclave directories from their Makefiles. For example:

```bash
cd host
oeedger8r ../switchless.edl --untrusted --experimental
```

`oeedger8r` needs the command line flag `--experimental` to be able to recognize the keyword `transition_using_threads`.

## About the host

The host first defines a structure specifically for configuring switchless calls. In this case, we specify the
first field `1` as the number of host worker threads for switchless OCALLs. In this example, 1) There is at most
1 enclave thread all the time, and 2) The number of cores available to the host worker threads is  1 (assuming a
2-core machine) or 3 (assuming a 4-core machine). In any case, The minimum of 1) and 2) is 1, which we choose to
be the number of host worker threads. The 2nd field specifies the number of enclave threads for switchless ECALLs.
Since switchless ECALL is not yet implemented, we require the 2nd field to be `0`.

```c
oe_enclave_config_context_switchless_t config = {1, 0};
```

The host then puts the structure address and the configuration type in an array of configurations for the enclave to be created. Even though we only have one configuration (for switchless) for the enclave, we'd like the flexibility of adding more than one configuration (with different types) for an enclave in the future.

```c
oe_enclave_config_t configs[] = {{
        .config_type = OE_ENCLAVE_CONFIG_CONTEXT_SWITCHLESS,
        .u.context_switchless_config = &config,
    }};
```

To make the configurations created above effective, we need to pass the array `configs` into `oe_create_enclave`
in the following way:

```c
oe_create_switchless_enclave(
             argv[1],
             OE_ENCLAVE_TYPE_SGX,
             flags,
             configs,
             OE_COUNTOF(configs),
             &enclave);
```

The host then makes an ECALL of `enclave_add_N` to transition into the enclave. After the ECALL returns, the
host prints the returned value and terminates the enclave.

As shown in the EDL file, the host exposes a host function `host_increment` which takes an integer `n`, and returns the result of `n+1`.

## About the enclave

The enclave exposes only one function `enclave_add_N`. The function takes two parameter `m` and `n`. It calls
the host function `host_increment` switchlessly in a loop of `n` iterations.

## Build and run

Note that there are two different build systems supported, one using GNU Make and
`pkg-config`, the other using CMake.

If the build and run succeed, the following output is expected:

```bash
host/switchlesshost ./enclave/switchlessenc.signed
enclave_add_N(): 10000 + 10000 = 20000
```

### CMake

This uses the CMake package provided by the Open Enclave SDK.

```bash
cd switchless
mkdir build && cd build
cmake ..
make run
```

### GNU Make

```bash
cd switchless
make build
make run
```
#### Note

switchless sample can run under OpenEnclave simulation mode.

To run the switchless sample in simulation mode from the command like, use the following:

```bash
# if built with cmake
./host/switchless_host ./enclave/switchless_enc.signed --simulate
# or, if built with GNU Make and pkg-config
make simulate
```
